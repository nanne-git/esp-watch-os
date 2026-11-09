#include "ble_svc.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "file_manager.h"
#include "freertos/FreeRTOS.h"
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
#define RX_LINE_MAX            1536

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

static void advertise(void);

/* ------------------------------------------------------------------ send */

bool ble_svc_send(const void *data, size_t len)
{
    const uint8_t *p = data;
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
        for (int i = 0; i < 100 && rc == BLE_HS_ENOMEM; i++) {
            struct os_mbuf *om = ble_hs_mbuf_from_flat(p, chunk);
            rc = om ? ble_gatts_notify_custom(s_conn, s_tx_handle, om) : BLE_HS_ENOMEM;
            if (rc == BLE_HS_ENOMEM) {
                vTaskDelay(pdMS_TO_TICKS(10));
            }
        }
        if (rc != 0) {
            return false;
        }
        p += chunk;
        len -= chunk;
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

static void dispatch_line(char *line)
{
    /* Gadgetbridge prefixes lines with 0x10 (and may send 0x03); skip control chars */
    while (*line && (unsigned char)*line < 0x20) {
        line++;
    }
    if (!*line) {
        return;
    }
    if (strstr(line, "GB(") || strstr(line, "setTime(")) {
        gb_handle_line(line);
    } else {
        file_manager_submit_ble((const uint8_t *)line, strlen(line));
    }
}

static void on_rx(const uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        char c = (char)data[i];
        if (c == '\n' || c == '\r') {
            s_line[s_line_len] = '\0';
            if (s_line_len) {
                dispatch_line(s_line);
            }
            s_line_len = 0;
        } else if (s_line_len < RX_LINE_MAX - 1) {
            s_line[s_line_len++] = c;
        }
    }
}

static int gatt_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        uint8_t buf[PREFERRED_MTU];
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
