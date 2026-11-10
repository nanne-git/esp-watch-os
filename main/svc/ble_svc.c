#include "ble_svc.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "file_manager.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "phone.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "settings.h"
#include "ui.h"

static const char *TAG = "ble";

#define PREFERRED_MTU       247
#define ADV_RESUME_DELAY_US (15 * 1000 * 1000)
#define RX_LINE_MAX         4096      /* GB notifications with long bodies can be big */
#define RX_WRITE_MAX        512       /* largest single GATT write we accept */
#define TX_QUEUE_LEN        24
#define TX_LINE_MAX         2048
#define TX_RETRY_MS         5000      /* keep retrying a busy link this long before giving up */

/* Nordic UART Service 6E400001-B5A3-F393-E0A9-E50E24DCCA9E (little-endian bytes) */
static const ble_uuid128_t nus_svc_uuid = BLE_UUID128_INIT(
    0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0, 0x93, 0xf3, 0xa3, 0xb5, 0x01, 0x00, 0x40, 0x6e);
static const ble_uuid128_t nus_rx_uuid = BLE_UUID128_INIT(
    0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0, 0x93, 0xf3, 0xa3, 0xb5, 0x02, 0x00, 0x40, 0x6e);
static const ble_uuid128_t nus_tx_uuid = BLE_UUID128_INIT(
    0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0, 0x93, 0xf3, 0xa3, 0xb5, 0x03, 0x00, 0x40, 0x6e);

static uint8_t            s_addr_type;
static uint8_t            s_addr[6];
static volatile uint16_t  s_conn = BLE_HS_CONN_HANDLE_NONE;
static uint16_t           s_tx_handle;
static volatile bool      s_subscribed;
static volatile bool      s_adv_paused;
static volatile bool      s_advertising;
static volatile bool      s_synced;
static esp_timer_handle_t s_resume_timer;

static char   s_line[RX_LINE_MAX];
static size_t s_line_len;
static bool   s_line_overflow;

typedef struct {
    char  *buf;
    size_t len;
} tx_msg_t;
static QueueHandle_t s_tx_queue;
static volatile bool s_tx_partial;      /* last line was cut off mid-way: terminate it first */

static void advertise(void);

/* ------------------------------------------------------------------ send */

/* Gadgetbridge splits incoming data at "\n" and throws away the character in front of it,
 * because Espruino ends lines with "\r\n". So every line we send must end in "\r\n",
 * otherwise a JSON line loses its closing '}' -> "Malformed JSON from Bangle.js". */
static bool tx_raw(const uint8_t *p, size_t len)
{
    while (len > 0) {
        if (s_conn == BLE_HS_CONN_HANDLE_NONE || !s_subscribed) {
            return false;
        }
        uint16_t mtu = ble_att_mtu(s_conn);
        size_t chunk = len;
        if (mtu > 3 && chunk > (size_t)(mtu - 3)) {
            chunk = mtu - 3;
        }
        int rc = BLE_HS_ENOMEM;
        for (int waited = 0; waited < TX_RETRY_MS; waited += 10) {
            struct os_mbuf *om = ble_hs_mbuf_from_flat(p, chunk);
            rc = om ? ble_gatts_notify_custom(s_conn, s_tx_handle, om) : BLE_HS_ENOMEM;
            if (rc != BLE_HS_ENOMEM && rc != BLE_HS_EBUSY) {
                break;
            }
            if (s_conn == BLE_HS_CONN_HANDLE_NONE) {
                return false;
            }
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        if (rc != 0) {
            return false;
        }
        p += chunk;
        len -= chunk;
    }
    return true;
}

static void tx_task(void *arg)
{
    tx_msg_t m;
    while (true) {
        if (xQueueReceive(s_tx_queue, &m, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (s_conn != BLE_HS_CONN_HANDLE_NONE && s_subscribed) {
            if (s_tx_partial) {
                /* a previous line was cut off: end it so it can't swallow this one */
                if (tx_raw((const uint8_t *)"\r\n", 2)) {
                    s_tx_partial = false;
                }
            }
            if (!tx_raw((const uint8_t *)m.buf, m.len)) {
                ESP_LOGW(TAG, "TX failed, line dropped");
                s_tx_partial = true;
            }
        }
        free(m.buf);
    }
}

bool ble_svc_send(const void *data, size_t len)
{
    if (!s_tx_queue || s_conn == BLE_HS_CONN_HANDLE_NONE || !s_subscribed) {
        return false;
    }
    const char *in = data;
    /* strip whatever line ending the caller used; we add "\r\n" ourselves */
    while (len > 0 && (in[len - 1] == '\n' || in[len - 1] == '\r')) {
        len--;
    }
    if (len > TX_LINE_MAX) {
        len = TX_LINE_MAX;
    }
    /* worst case every byte is a '\n' that becomes "\r\n", plus the final "\r\n" */
    char *out = malloc(len * 2 + 2);
    if (!out) {
        return false;
    }
    size_t o = 0;
    for (size_t i = 0; i < len; i++) {
        char c = in[i];
        if (c == '\r') {
            continue;                       /* re-added before each '\n' below */
        }
        if (c == '\n') {
            out[o++] = '\r';
        }
        out[o++] = c;
    }
    out[o++] = '\r';
    out[o++] = '\n';
    ESP_LOGD(TAG, "tx> %.*s", (int)(o > 2 ? o - 2 : 0), out);     /* debug: what the phone gets */
    tx_msg_t m = { .buf = out, .len = o };
    if (xQueueSend(s_tx_queue, &m, pdMS_TO_TICKS(50)) != pdTRUE) {
        free(out);
        ESP_LOGW(TAG, "TX queue full, line dropped");
        return false;
    }
    return true;
}

bool ble_svc_connected(void) { return s_conn != BLE_HS_CONN_HANDLE_NONE; }
bool ble_svc_advertising(void) { return s_advertising; }

void ble_svc_get_addr(char *buf, size_t len)
{
    snprintf(buf, len, "%02X:%02X:%02X:%02X:%02X:%02X", s_addr[5], s_addr[4], s_addr[3], s_addr[2],
             s_addr[1], s_addr[0]);
}

/* ------------------------------------------------------------------ receive: assemble lines */

/* Does this look like JavaScript rather than a file-manager shell command?
 * e.g. reset();  E.showMessage("x")  require("Storage").list()  load()  Bangle.buzz() */
static bool looks_like_js(const char *line)
{
    size_t first = strcspn(line, " ");            /* first word */
    for (size_t i = 0; i < first; i++) {
        if (line[i] == '(' || line[i] == '=' || line[i] == ';' || line[i] == '{') {
            return true;
        }
    }
    size_t n = strlen(line);
    return n > 0 && line[n - 1] == ';';
}

static void dispatch_line(char *line)
{
    /* Gadgetbridge prefixes every line with 0x10 (DLE = "don't echo") and may send
     * 0x03 (Ctrl-C) or spaces before it. Anything marked like that comes from Gadgetbridge
     * and is JavaScript meant for an Espruino: never hand it to the shell. */
    bool from_gb = false;
    while (*line && ((unsigned char)*line < 0x20 || *line == ' ')) {
        if (*line == 0x10) {
            from_gb = true;
        }
        line++;
    }
    if (!*line) {
        return;
    }
    ESP_LOGD(TAG, "rx< %.100s", line);                                /* debug */
    /* Gadgetbridge's connection probe: a real Bangle.js answers with the JSON string of
     * its console device. Without an answer Gadgetbridge keeps asking / reconnecting. */
    if (strstr(line, "process.env.CONSOLE")) {
        ble_svc_send("\"Bluetooth\"", 11);
        return;
    }
    if (strstr(line, "process.env.VERSION")) {
        ble_svc_send("\"2v21\"", 6);
        return;
    }
    if (strstr(line, "GB(") || strstr(line, "setTime(")) {
        gb_handle_line(line);
        return;
    }
    if (from_gb || looks_like_js(line)) {
        /* other Espruino JS (g.dump(), reset(), E.showMessage(...), Storage...): ignore
         * silently - any text reply would only confuse Gadgetbridge */
        ESP_LOGD(TAG, "ignored JS line: %.60s", line);
        return;
    }
    for (const char *p = line; *p; p++) {
        if ((unsigned char)*p < 0x20) {
            return;                                /* binary junk is not a shell command */
        }
    }
    file_manager_submit_ble((const uint8_t *)line, strlen(line));
}

static void on_rx(const uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        char c = (char)data[i];
        if (c == '\n' || c == '\r') {
            s_line[s_line_len] = '\0';
            if (s_line_overflow) {
                /* a truncated GB(...) line is broken JSON anyway: drop it entirely */
                ESP_LOGW(TAG, "RX line longer than %d bytes dropped", RX_LINE_MAX - 1);
            } else if (s_line_len) {
                dispatch_line(s_line);
            }
            s_line_len = 0;
            s_line_overflow = false;
        } else if (s_line_len < RX_LINE_MAX - 1) {
            s_line[s_line_len++] = c;
        } else {
            s_line_overflow = true;
        }
    }
}

static int gatt_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        static uint8_t buf[RX_WRITE_MAX];     /* host task only: no need for stack space */
        uint16_t len = 0;
        if (ble_hs_mbuf_to_flat(ctxt->om, buf, sizeof(buf), &len) != 0) {
            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        }
        on_rx(buf, len);
        return 0;
    }
    return BLE_ATT_ERR_UNLIKELY;
}

static const struct ble_gatt_svc_def gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &nus_svc_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            { .uuid = &nus_rx_uuid.u, .access_cb = gatt_access,
              .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP },
            { .uuid = &nus_tx_uuid.u, .access_cb = gatt_access, .val_handle = &s_tx_handle,
              .flags = BLE_GATT_CHR_F_NOTIFY },
            { 0 },
        },
    },
    { 0 },
};

/* ------------------------------------------------------------------ GAP */

static int gap_event(struct ble_gap_event *e, void *arg)
{
    switch (e->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (e->connect.status == 0) {
            s_conn = e->connect.conn_handle;
            s_advertising = false;
            s_line_len = 0;
            s_line_overflow = false;
            s_tx_partial = false;
            ESP_LOGI(TAG, "connected");
            ui_on_data_changed();
        } else {
            advertise();
        }
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "disconnected (0x%x)", e->disconnect.reason);
        s_conn = BLE_HS_CONN_HANDLE_NONE;
        s_subscribed = false;
        ui_on_data_changed();
        advertise();
        break;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        s_advertising = false;
        advertise();
        break;
    case BLE_GAP_EVENT_SUBSCRIBE:
        if (e->subscribe.attr_handle == s_tx_handle) {
            s_subscribed = e->subscribe.cur_notify;
            if (s_subscribed) {
                /* queued, sent by tx_task - never blocks the host task */
                phone_send_version();
                phone_send_battery();
            }
        }
        break;
    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "MTU %d", e->mtu.value);
        break;
    default:
        break;
    }
    return 0;
}

static void advertise(void)
{
    if (!s_synced || s_adv_paused || !settings()->bt_on || s_conn != BLE_HS_CONN_HANDLE_NONE) {
        return;
    }
    struct ble_hs_adv_fields f = { 0 };
    const char *name = ble_svc_gap_device_name();
    f.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    f.name = (const uint8_t *)name;
    f.name_len = strlen(name);
    f.name_is_complete = 1;
    if (ble_gap_adv_set_fields(&f) != 0) {
        /* name too long for the packet: send a shortened one */
        f.name_len = 26;
        f.name_is_complete = 0;
        ble_gap_adv_set_fields(&f);
    }
    struct ble_hs_adv_fields rsp = { 0 };
    rsp.uuids128 = &nus_svc_uuid;
    rsp.num_uuids128 = 1;
    rsp.uuids128_is_complete = 1;
    ble_gap_adv_rsp_set_fields(&rsp);

    struct ble_gap_adv_params p = { 0 };
    p.conn_mode = BLE_GAP_CONN_MODE_UND;
    p.disc_mode = BLE_GAP_DISC_MODE_GEN;
    p.itvl_min = settings()->power_save ? 1600 : 800;    /* 1 s / 500 ms */
    p.itvl_max = p.itvl_min + 80;
    if (ble_gap_adv_start(s_addr_type, NULL, BLE_HS_FOREVER, &p, gap_event, NULL) == 0) {
        s_advertising = true;
        ESP_LOGI(TAG, "advertising as \"%s\"", name);
    }
}

static void stop_all(void)
{
    if (s_advertising) {
        ble_gap_adv_stop();
        s_advertising = false;
    }
    if (s_conn != BLE_HS_CONN_HANDLE_NONE) {
        ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
    }
}

void ble_svc_apply_settings(void)
{
    if (!s_synced) {
        return;
    }
    const settings_t *s = settings();
    if (strcmp(ble_svc_gap_device_name(), s->bt_name) != 0) {
        ble_svc_gap_device_name_set(s->bt_name);
        if (s_advertising) {
            ble_gap_adv_stop();
            s_advertising = false;
        }
    }
    if (!s->bt_on) {
        stop_all();
    } else if (!s_advertising) {
        advertise();
    }
}

void ble_svc_pause_adv_for_transfer(void)
{
    if (s_conn == BLE_HS_CONN_HANDLE_NONE && !s_adv_paused) {
        s_adv_paused = true;
        if (s_advertising) {
            ble_gap_adv_stop();
            s_advertising = false;
        }
        ESP_LOGI(TAG, "web transfer - advertising paused");
    }
    esp_timer_stop(s_resume_timer);
    esp_timer_start_once(s_resume_timer, ADV_RESUME_DELAY_US);
}

static void resume_adv(void *arg)
{
    if (s_adv_paused) {
        s_adv_paused = false;
        advertise();
    }
}

static void on_sync(void)
{
    ble_hs_util_ensure_addr(0);
    ble_hs_id_infer_auto(0, &s_addr_type);
    ble_hs_id_copy_addr(s_addr_type, s_addr, NULL);
    s_synced = true;
    advertise();
}

static void on_reset(int reason)
{
    ESP_LOGE(TAG, "host reset %d", reason);
}

static void host_task(void *param)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static void on_settings(void)
{
    ble_svc_apply_settings();
}

void ble_svc_start(void)
{
    s_tx_queue = xQueueCreate(TX_QUEUE_LEN, sizeof(tx_msg_t));
    xTaskCreate(tx_task, "ble_tx", 3072, NULL, 5, NULL);

    const esp_timer_create_args_t ta = { .callback = resume_adv, .name = "adv_resume" };
    esp_timer_create(&ta, &s_resume_timer);

    if (nimble_port_init() != ESP_OK) {
        ESP_LOGE(TAG, "NimBLE init failed");
        return;
    }
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;
    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_gatts_count_cfg(gatt_svcs);
    ble_gatts_add_svcs(gatt_svcs);
    ble_svc_gap_device_name_set(settings()->bt_name);
    ble_svc_gap_device_appearance_set(0x00C2);     /* smartwatch */
    ble_att_set_preferred_mtu(PREFERRED_MTU);
    nimble_port_freertos_init(host_task);
    settings_on_change(on_settings);
}
