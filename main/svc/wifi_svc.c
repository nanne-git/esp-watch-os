#include "wifi_svc.h"
#include "dns_server.h"

#include <stdio.h>
#include <string.h>

#include "esp_coexist.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mdns.h"
#include "nvs.h"
#include "settings.h"
#include "ui.h"

static const char *TAG = "wifi";

#define EV_GOT_IP     BIT0
#define EV_DISCONN    BIT1
#define EV_KICK       BIT2

typedef struct {
    char ssid[33];
    char pass[65];
} saved_net_t;

static saved_net_t        s_nets[WIFI_SAVED_MAX];
static EventGroupHandle_t s_ev;
static SemaphoreHandle_t  s_scan_lock;
static esp_netif_t       *s_sta, *s_ap;
static volatile wifi_state_t s_state = WIFI_ST_OFF;
static char               s_cur_ssid[33];
static char               s_url[96];
static bool               s_started;     /* esp_wifi_start() called */
static bool               s_mdns_up;
static char               s_pending_ssid[33], s_pending_pass[65];
static volatile bool      s_pending;

/* ------------------------------------------------------------------ saved networks (NVS) */

static void nets_load(void)
{
    nvs_handle_t h;
    size_t len = sizeof(s_nets);
    memset(s_nets, 0, sizeof(s_nets));
    if (nvs_open("watch", NVS_READONLY, &h) == ESP_OK) {
        nvs_get_blob(h, "wifinets", s_nets, &len);
        nvs_close(h);
    }
    bool any = false;
    for (int i = 0; i < WIFI_SAVED_MAX; i++) {
        any |= s_nets[i].ssid[0] != 0;
    }
    if (!any && CONFIG_WATCH_WIFI_SSID[0]) {        /* first boot: seed from menuconfig */
        strlcpy(s_nets[0].ssid, CONFIG_WATCH_WIFI_SSID, sizeof(s_nets[0].ssid));
        strlcpy(s_nets[0].pass, CONFIG_WATCH_WIFI_PASSWORD, sizeof(s_nets[0].pass));
    }
}

static void nets_save(void)
{
    nvs_handle_t h;
    if (nvs_open("watch", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_blob(h, "wifinets", s_nets, sizeof(s_nets));
        nvs_commit(h);
        nvs_close(h);
    }
}

static volatile int s_ap_clients;     /* devices connected to our hotspot */

static int nets_count(void)
{
    int n = 0;
    for (int i = 0; i < WIFI_SAVED_MAX; i++) n += s_nets[i].ssid[0] != 0;
    return n;
}

static saved_net_t *nets_find(const char *ssid)
{
    for (int i = 0; i < WIFI_SAVED_MAX; i++) {
        if (s_nets[i].ssid[0] && strcmp(s_nets[i].ssid, ssid) == 0) {
            return &s_nets[i];
        }
    }
    return NULL;
}

static void nets_put(const char *ssid, const char *pass)
{
    saved_net_t n = { 0 };
    strlcpy(n.ssid, ssid, sizeof(n.ssid));
    strlcpy(n.pass, pass ? pass : "", sizeof(n.pass));
    /* move to the front (most recently used), drop duplicates / the oldest */
    saved_net_t out[WIFI_SAVED_MAX] = { 0 };
    out[0] = n;
    int j = 1;
    for (int i = 0; i < WIFI_SAVED_MAX && j < WIFI_SAVED_MAX; i++) {
        if (s_nets[i].ssid[0] && strcmp(s_nets[i].ssid, ssid) != 0) {
            out[j++] = s_nets[i];
        }
    }
    memcpy(s_nets, out, sizeof(s_nets));
    nets_save();
}

int wifi_svc_saved(char ssids[][33], int max)
{
    int n = 0;
    for (int i = 0; i < WIFI_SAVED_MAX && n < max; i++) {
        if (s_nets[i].ssid[0]) {
            strlcpy(ssids[n++], s_nets[i].ssid, 33);
        }
    }
    return n;
}

void wifi_svc_forget(const char *ssid)
{
    saved_net_t *n = nets_find(ssid);
    if (n) {
        memset(n, 0, sizeof(*n));
        nets_save();
    }
    if (strcmp(s_cur_ssid, ssid) == 0) {
        esp_wifi_disconnect();
    }
}

/* ------------------------------------------------------------------ events */

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_state == WIFI_ST_CONNECTED) {
            ESP_LOGW(TAG, "disconnected from %s", s_cur_ssid);
        }
        if (s_state != WIFI_ST_OFF) {
            s_state = WIFI_ST_IDLE;
        }
        s_url[0] = '\0';
        xEventGroupSetBits(s_ev, EV_DISCONN);
        ui_on_data_changed();
    } else if (base == IP_EVENT && id == IP_EVENT_ASSIGNED_IP_TO_CLIENT) {
        ip_event_assigned_ip_to_client_t *e = data;
        ESP_LOGI(TAG, "hotspot gave " IPSTR " to a device", IP2STR(&e->ip));
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = data;
        s_state = WIFI_ST_CONNECTED;
        snprintf(s_url, sizeof(s_url), "http://" IPSTR "/", IP2STR(&e->ip_info.ip));
        ESP_LOGI(TAG, "connected to %s: %s", s_cur_ssid, s_url);
        xEventGroupSetBits(s_ev, EV_GOT_IP);
        ui_on_data_changed();
    } else if (base == WIFI_EVENT &&
               (id == WIFI_EVENT_AP_STACONNECTED || id == WIFI_EVENT_AP_STADISCONNECTED)) {
        s_ap_clients += id == WIFI_EVENT_AP_STACONNECTED ? 1 : -1;
        if (s_ap_clients < 0) s_ap_clients = 0;
        ESP_LOGI(TAG, "hotspot: %d device(s) connected", s_ap_clients);
        ui_on_data_changed();
    }
}

/* ------------------------------------------------------------------ mode / power */

/*
 * Radio power: normally the station naps between router beacons (modem sleep) - this is
 * the biggest power/heat saver. Power-saving mode naps longer. During web transfers /
 * OTA downloads the radio stays awake (boost) and drops back 15 s after the last activity.
 */
static esp_timer_handle_t s_boost_timer;
static volatile bool      s_boosted;

static void apply_ps(void)
{
    if (!s_started) return;
    if (s_boosted) {
        esp_wifi_set_ps(WIFI_PS_NONE);
    } else {
        esp_wifi_set_ps(settings()->power_save ? WIFI_PS_MAX_MODEM : WIFI_PS_MIN_MODEM);
    }
}

static void boost_end(void *arg)
{
    s_boosted = false;
    apply_ps();
}

void wifi_svc_set_high_throughput(bool on)
{
    if (!s_boost_timer) {
        const esp_timer_create_args_t ta = { .callback = boost_end, .name = "wifi_boost" };
        esp_timer_create(&ta, &s_boost_timer);
    }
    esp_timer_stop(s_boost_timer);
    if (on) {
        if (!s_boosted) {
            s_boosted = true;
            apply_ps();
        }
        esp_timer_start_once(s_boost_timer, 15 * 1000000);   /* auto-off if nobody calls again */
    } else {
        boost_end(NULL);
    }
}

static void start_mdns(void)
{
    if (!s_mdns_up && mdns_init() == ESP_OK) {
        s_mdns_up = true;
        mdns_instance_name_set("ESP-Watch");
        mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    }
    if (s_mdns_up) {
        mdns_hostname_set(settings()->hostname);
    }
}

static void apply_mode(void)
{
    const settings_t *s = settings();
    if (!s->wifi_on && !s->hotspot_on) {
        if (s_started) {
            s_state = WIFI_ST_OFF;
            esp_wifi_stop();
            s_started = false;
            s_url[0] = '\0';
        }
        return;
    }
    wifi_mode_t mode = s->wifi_on ? (s->hotspot_on ? WIFI_MODE_APSTA : WIFI_MODE_STA) : WIFI_MODE_AP;
    esp_wifi_set_mode(mode);
    if (s->hotspot_on) {
        wifi_config_t ap = { 0 };
        strlcpy((char *)ap.ap.ssid, s->ap_ssid, sizeof(ap.ap.ssid));
        strlcpy((char *)ap.ap.password, s->ap_pass, sizeof(ap.ap.password));
        ap.ap.ssid_len = strlen(s->ap_ssid);
        ap.ap.channel = 6;
        ap.ap.max_connection = 4;
        ap.ap.authmode = strlen(s->ap_pass) >= 8 ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
        esp_wifi_set_config(WIFI_IF_AP, &ap);
    }
    if (!s_started) {
        esp_wifi_start();
        s_started = true;
        apply_ps();
        esp_coex_preference_set(ESP_COEX_PREFER_BALANCE);
    }
    s_state = s->wifi_on ? (s_state == WIFI_ST_CONNECTED ? WIFI_ST_CONNECTED : WIFI_ST_IDLE) : WIFI_ST_OFF;
    start_mdns();
    xEventGroupSetBits(s_ev, EV_KICK);
}

static void on_settings(void)
{
    static bool last_wifi, last_ap, last_ps;
    static char last_ap_ssid[33], last_ap_pass[65], last_host[33];
    const settings_t *s = settings();
    if (s->wifi_on != last_wifi || s->hotspot_on != last_ap || strcmp(s->ap_ssid, last_ap_ssid) ||
        strcmp(s->ap_pass, last_ap_pass) || strcmp(s->hostname, last_host) || s->power_save != last_ps) {
        last_wifi = s->wifi_on;
        last_ap = s->hotspot_on;
        last_ps = s->power_save;
        strlcpy(last_ap_ssid, s->ap_ssid, sizeof(last_ap_ssid));
        strlcpy(last_ap_pass, s->ap_pass, sizeof(last_ap_pass));
        strlcpy(last_host, s->hostname, sizeof(last_host));
        apply_mode();
        if (s_started) {
            apply_ps();
        }
    }
}

/* ------------------------------------------------------------------ scan */

int wifi_svc_scan(wifi_ap_info_t *out, int max)
{
    if (!s_started || !settings()->wifi_on) {
        return 0;
    }
    xSemaphoreTake(s_scan_lock, portMAX_DELAY);
    wifi_scan_config_t cfg = { .show_hidden = false };
    esp_err_t err = ESP_FAIL;
    for (int i = 0; i < 4 && err != ESP_OK; i++) {
        err = esp_wifi_scan_start(&cfg, true);
        if (err != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(700));      /* busy connecting: try again */
        }
    }
    int n = 0;
    if (err == ESP_OK) {
        uint16_t count = 0;
        esp_wifi_scan_get_ap_num(&count);
        wifi_ap_record_t *recs = calloc(count ? count : 1, sizeof(wifi_ap_record_t));
        if (recs && esp_wifi_scan_get_ap_records(&count, recs) == ESP_OK) {
            for (int i = 0; i < count && n < max; i++) {
                if (!recs[i].ssid[0]) {
                    continue;
                }
                bool dup = false;          /* same network on several access points */
                for (int j = 0; j < n; j++) {
                    dup |= strcmp(out[j].ssid, (char *)recs[i].ssid) == 0;
                }
                if (dup) {
                    continue;
                }
                strlcpy(out[n].ssid, (char *)recs[i].ssid, sizeof(out[n].ssid));
                out[n].rssi = recs[i].rssi;
                out[n].secure = recs[i].authmode != WIFI_AUTH_OPEN;
                out[n].saved = nets_find(out[n].ssid) != NULL;
                n++;
            }
        }
        free(recs);
    }
    xSemaphoreGive(s_scan_lock);
    return n;
}

/* ------------------------------------------------------------------ connection task */

static bool try_connect(const char *ssid, const char *pass)
{
    wifi_config_t sta = { 0 };
    strlcpy((char *)sta.sta.ssid, ssid, sizeof(sta.sta.ssid));
    strlcpy((char *)sta.sta.password, pass, sizeof(sta.sta.password));
    sta.sta.threshold.authmode = pass[0] ? WIFI_AUTH_WPA_PSK : WIFI_AUTH_OPEN;
    sta.sta.pmf_cfg.capable = true;
    esp_wifi_disconnect();
    esp_wifi_set_config(WIFI_IF_STA, &sta);
    strlcpy(s_cur_ssid, ssid, sizeof(s_cur_ssid));
    s_state = WIFI_ST_CONNECTING;
    ui_on_data_changed();
    xEventGroupClearBits(s_ev, EV_GOT_IP | EV_DISCONN);
    ESP_LOGI(TAG, "joining %s", ssid);
    esp_wifi_connect();
    EventBits_t b = xEventGroupWaitBits(s_ev, EV_GOT_IP, pdFALSE, pdFALSE, pdMS_TO_TICKS(15000));
    if (!(b & EV_GOT_IP)) {
        esp_wifi_disconnect();
        s_state = WIFI_ST_IDLE;
        return false;
    }
    return true;
}

/* The hotspot keeps the radio fully awake (an access point can't nap like a station does),
 * which is a big heat/battery cost. Turn it off after 10 minutes with nobody connected. */
#define HOTSPOT_IDLE_MIN 10
static volatile bool s_hotspot_idle_off;

static void hotspot_idle_check(void *arg)
{
    static int idle_min;
    if (settings()->hotspot_on && s_ap_clients == 0) {
        if (++idle_min >= HOTSPOT_IDLE_MIN) {
            idle_min = 0;
            s_hotspot_idle_off = true;
            xEventGroupSetBits(s_ev, EV_KICK);       /* wifi_task does the actual change */
        }
    } else {
        idle_min = 0;
    }
}

static void wifi_task(void *arg)
{
    int backoff_s = 5;
    {
        const esp_timer_create_args_t ta = { .callback = hotspot_idle_check, .name = "ap_idle" };
        esp_timer_handle_t t;
        if (esp_timer_create(&ta, &t) == ESP_OK) esp_timer_start_periodic(t, 60ULL * 1000000);
    }
    while (true) {
        if (s_hotspot_idle_off) {
            s_hotspot_idle_off = false;
            ESP_LOGI(TAG, "hotspot unused for %d min - turning it off", HOTSPOT_IDLE_MIN);
            settings_edit()->hotspot_on = false;
            settings_commit();
            ui_toast("Hotspot turned off (not used)");
        }
        if (!s_started || !settings()->wifi_on) {
            xEventGroupWaitBits(s_ev, EV_KICK, pdTRUE, pdFALSE, pdMS_TO_TICKS(5000));
            continue;
        }
        if (s_pending) {
            s_pending = false;
            if (try_connect(s_pending_ssid, s_pending_pass)) {
                nets_put(s_pending_ssid, s_pending_pass);
                ui_toast("Wi-Fi connected");
            } else {
                ui_toast("Wi-Fi: could not connect");
            }
            continue;
        }
        if (s_state == WIFI_ST_CONNECTED) {
            /* wait for a disconnect or a request */
            xEventGroupWaitBits(s_ev, EV_DISCONN | EV_KICK, pdTRUE, pdFALSE, portMAX_DELAY);
            continue;
        }
        /* A scan hops over all channels and takes the radio off the hotspot's channel:
         * the hotspot disappears and its devices drop. So with the hotspot in use (or no
         * saved network to look for) don't scan in the background. */
        if (settings()->hotspot_on && (s_ap_clients > 0 || nets_count() == 0)) {
            xEventGroupWaitBits(s_ev, EV_KICK, pdTRUE, pdFALSE, pdMS_TO_TICKS(30000));
            continue;
        }
        /* not connected: find the strongest saved network */
        wifi_ap_info_t aps[16];
        int n = wifi_svc_scan(aps, 16);
        bool joined = false;
        for (int i = 0; i < n && !joined && !s_pending; i++) {      /* scan is sorted by RSSI */
            saved_net_t *sn = nets_find(aps[i].ssid);
            if (sn) {
                joined = try_connect(sn->ssid, sn->pass);
            }
        }
        if (joined) {
            backoff_s = 5;
            nets_put(s_cur_ssid, nets_find(s_cur_ssid) ? nets_find(s_cur_ssid)->pass : "");
        } else {
            xEventGroupWaitBits(s_ev, EV_KICK, pdTRUE, pdFALSE, pdMS_TO_TICKS(backoff_s * 1000));
            if (backoff_s < 300) {          /* no known network around: scan rarely */
                backoff_s *= 2;
            }
        }
    }
}

void wifi_svc_connect(const char *ssid, const char *pass)
{
    strlcpy(s_pending_ssid, ssid, sizeof(s_pending_ssid));
    if (!pass) {                       /* NULL: reuse the saved password */
        saved_net_t *n = nets_find(ssid);
        pass = n ? n->pass : "";
    }
    strlcpy(s_pending_pass, pass, sizeof(s_pending_pass));
    s_pending = true;
    if (s_state == WIFI_ST_CONNECTED) {
        esp_wifi_disconnect();
    }
    xEventGroupSetBits(s_ev, EV_KICK);
}

/* ------------------------------------------------------------------ status */

void wifi_svc_status(wifi_status_t *o)
{
    memset(o, 0, sizeof(*o));
    o->state = s_state;
    strlcpy(o->ssid, s_cur_ssid, sizeof(o->ssid));
    uint8_t mac[6];
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) == ESP_OK) {
        snprintf(o->mac, sizeof(o->mac), MACSTR, MAC2STR(mac));
    }
    esp_netif_ip_info_t ip;
    if (s_state == WIFI_ST_CONNECTED && esp_netif_get_ip_info(s_sta, &ip) == ESP_OK) {
        snprintf(o->ip, sizeof(o->ip), IPSTR, IP2STR(&ip.ip));
        snprintf(o->gw, sizeof(o->gw), IPSTR, IP2STR(&ip.gw));
        snprintf(o->mask, sizeof(o->mask), IPSTR, IP2STR(&ip.netmask));
        esp_netif_dns_info_t dns;
        if (esp_netif_get_dns_info(s_sta, ESP_NETIF_DNS_MAIN, &dns) == ESP_OK) {
            snprintf(o->dns, sizeof(o->dns), IPSTR, IP2STR(&dns.ip.u_addr.ip4));
        }
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            o->rssi = ap.rssi;
        }
    }
    o->ap_on = s_started && settings()->hotspot_on;
    if (o->ap_on) {
        strlcpy(o->ap_ssid, settings()->ap_ssid, sizeof(o->ap_ssid));
        if (esp_netif_get_ip_info(s_ap, &ip) == ESP_OK) {
            snprintf(o->ap_ip, sizeof(o->ap_ip), IPSTR, IP2STR(&ip.ip));
        }
        wifi_sta_list_t list;
        if (esp_wifi_ap_get_sta_list(&list) == ESP_OK) {
            o->ap_clients = list.num;
        }
    }
}

const char *wifi_get_url(void)
{
    if (s_url[0]) {
        return s_url;
    }
    if (s_started && settings()->hotspot_on) {
        return "http://192.168.4.1/";
    }
    return "";
}

void wifi_svc_start(void)
{
    s_ev = xEventGroupCreate();
    s_scan_lock = xSemaphoreCreateMutex();
    nets_load();
    esp_netif_init();
    esp_event_loop_create_default();
    s_sta = esp_netif_create_default_wifi_sta();
    s_ap = esp_netif_create_default_wifi_ap();
    /* hotspot hands out the watch itself as DNS server (see dns_server.c) */
    {
        esp_netif_dns_info_t dns = { 0 };
        dns.ip.type = ESP_IPADDR_TYPE_V4;
        dns.ip.u_addr.ip4.addr = ESP_IP4TOADDR(192, 168, 4, 1);
        uint8_t offer = 1;           /* OFFER_DNS */
        esp_netif_dhcps_stop(s_ap);
        esp_netif_set_dns_info(s_ap, ESP_NETIF_DNS_MAIN, &dns);
        esp_netif_dhcps_option(s_ap, ESP_NETIF_OP_SET, ESP_NETIF_DOMAIN_NAME_SERVER, &offer, sizeof(offer));
        esp_netif_dhcps_start(s_ap);
    }
    dns_server_start();
    esp_netif_set_hostname(s_sta, settings()->hostname);
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&init);
    esp_wifi_set_storage(WIFI_STORAGE_RAM);       /* we keep our own saved-network list */
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_ASSIGNED_IP_TO_CLIENT, on_event, NULL);
    settings_on_change(on_settings);
    on_settings();
    xTaskCreate(wifi_task, "wifi_mgr", 4096, NULL, 4, NULL);
}
