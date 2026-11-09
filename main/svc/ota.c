/*
 * Firmware updates.
 *
 * GitHub Releases: GET /repos/{owner}/{repo}/releases/latest, pick the .bin asset, download it
 * with esp_https_ota into the other app slot, reboot. Private repos need a token; the asset
 * download answers with a redirect to a pre-signed URL which must be fetched WITHOUT the
 * Authorization header, so the redirect is followed by hand.
 *
 * Browser upload: begin/write/end into the other slot, then reboot.
 * Rollback: the bootloader returns to the old firmware if the new one doesn't call
 * esp_ota_mark_app_valid_cancel_rollback() (done in app_main once everything started).
 */
#include "ota.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "settings.h"
#include "ui.h"
#include "wifi_svc.h"

static const char *TAG = "ota";

static ota_status_t s_st;
static char         s_asset_url[256];
static volatile bool s_busy;

void ota_get(ota_status_t *o) { *o = s_st; }

static void set_state(ota_state_t st, const char *msg)
{
    s_st.state = st;
    if (msg) {
        strlcpy(s_st.message, msg, sizeof(s_st.message));
        ESP_LOGI(TAG, "%s", msg);
    }
    ui_on_data_changed();
}

/* "v1.2.3" newer than "v1.2.0"? Non-numeric versions (dev builds): any different tag counts. */
static bool is_newer(const char *latest, const char *current)
{
    int a[3] = { 0 }, b[3] = { 0 };
    const char *l = latest[0] == 'v' ? latest + 1 : latest;
    const char *c = current[0] == 'v' ? current + 1 : current;
    int nl = sscanf(l, "%d.%d.%d", &a[0], &a[1], &a[2]);
    int nc = sscanf(c, "%d.%d.%d", &b[0], &b[1], &b[2]);
    if (nl < 1 || nc < 1) {
        return strcmp(latest, current) != 0;
    }
    for (int i = 0; i < 3; i++) {
        if (a[i] != b[i]) return a[i] > b[i];
    }
    /* same numbers: a dev build ("v1.2.3-4-gabc") is older than the clean release */
    return strchr(c, '-') != NULL && strchr(l, '-') == NULL;
}

static void add_auth(esp_http_client_handle_t c)
{
    esp_http_client_set_header(c, "User-Agent", "esp-watch-os");
    const char *tok = settings()->ota_token;
    if (tok[0]) {
        char h[128];
        snprintf(h, sizeof(h), "Bearer %s", tok);
        esp_http_client_set_header(c, "Authorization", h);
    }
}

static bool http_get_json(const char *url, cJSON **out, int *status)
{
    esp_http_client_config_t cfg = { .url = url, .crt_bundle_attach = esp_crt_bundle_attach, .timeout_ms = 15000,
                                     .buffer_size = 2048, .buffer_size_tx = 1024 };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return false;
    add_auth(c);
    esp_http_client_set_header(c, "Accept", "application/vnd.github+json");
    bool ok = false;
    *out = NULL;
    if (esp_http_client_open(c, 0) == ESP_OK) {
        esp_http_client_fetch_headers(c);
        *status = esp_http_client_get_status_code(c);
        int cap = 64 * 1024, len = 0, r;
        char *body = malloc(cap);
        while (body && (r = esp_http_client_read(c, body + len, cap - 1 - len)) > 0) {
            len += r;
            if (len >= cap - 1) break;
        }
        if (body) {
            body[len] = '\0';
            *out = cJSON_Parse(body);
            free(body);
            ok = *out != NULL;
        }
    }
    esp_http_client_cleanup(c);
    return ok;
}

static void check_task(void *arg)
{
    bool install_after = (bool)(intptr_t)arg;
    char url[160];
    snprintf(url, sizeof(url), "https://api.github.com/repos/%s/releases/latest", settings()->ota_repo);
    set_state(OTA_CHECKING, "Checking GitHub...");
    cJSON *j = NULL;
    int status = 0;
    if (!http_get_json(url, &j, &status) || status != 200) {
        set_state(OTA_ERROR, status == 404 ? "No release found (or private repo needs a token)"
                                           : status == 401 ? "Token rejected" : "Can't reach GitHub");
        cJSON_Delete(j);
        s_busy = false;
        vTaskDelete(NULL);
    }
    const cJSON *tag = cJSON_GetObjectItem(j, "tag_name");
    const cJSON *assets = cJSON_GetObjectItem(j, "assets");
    s_asset_url[0] = '\0';
    const cJSON *a;
    cJSON_ArrayForEach(a, assets) {
        const cJSON *name = cJSON_GetObjectItem(a, "name");
        const cJSON *aurl = cJSON_GetObjectItem(a, "url");
        if (cJSON_IsString(name) && cJSON_IsString(aurl) && strstr(name->valuestring, ".bin")) {
            strlcpy(s_asset_url, aurl->valuestring, sizeof(s_asset_url));
            if (strcmp(name->valuestring, "esp-watch-os.bin") == 0) break;
        }
    }
    strlcpy(s_st.latest, cJSON_IsString(tag) ? tag->valuestring : "", sizeof(s_st.latest));
    cJSON_Delete(j);
    s_st.update_available = s_st.latest[0] && s_asset_url[0] && is_newer(s_st.latest, s_st.current);
    char msg[96];
    snprintf(msg, sizeof(msg), s_st.update_available ? "Update %s available" : "Up to date (%s)", s_st.latest);
    set_state(OTA_IDLE, msg);
    if (s_st.update_available && !install_after) {
        ui_toast(msg);
    }
    s_busy = false;
    if (install_after && s_st.update_available) {
        ota_install_async();
    }
    vTaskDelete(NULL);
}

void ota_check_async(void)
{
    if (s_busy) return;
    s_busy = true;
    xTaskCreate(check_task, "ota_check", 8192, (void *)0, 3, NULL);
}

/* ------------------------------------------------------------------ install */

static char s_location[1024];

static esp_err_t loc_event(esp_http_client_event_t *e)
{
    if (e->event_id == HTTP_EVENT_ON_HEADER && strcasecmp(e->header_key, "Location") == 0) {
        strlcpy(s_location, e->header_value, sizeof(s_location));
    }
    return ESP_OK;
}

/* asset API URL -> pre-signed download URL (no auth needed) */
static bool resolve_download_url(void)
{
    s_location[0] = '\0';
    esp_http_client_config_t cfg = { .url = s_asset_url, .crt_bundle_attach = esp_crt_bundle_attach,
                                     .timeout_ms = 15000, .disable_auto_redirect = true, .event_handler = loc_event,
                                     .buffer_size = 4096, .buffer_size_tx = 1024 };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return false;
    add_auth(c);
    esp_http_client_set_header(c, "Accept", "application/octet-stream");
    esp_err_t err = esp_http_client_open(c, 0);
    if (err == ESP_OK) {
        esp_http_client_fetch_headers(c);
    }
    int status = esp_http_client_get_status_code(c);
    esp_http_client_cleanup(c);
    ESP_LOGI(TAG, "asset status %d, redirect %s", status, s_location[0] ? "yes" : "no");
    return (status == 301 || status == 302 || status == 307) && s_location[0];
}

static void install_task(void *arg)
{
    set_state(OTA_DOWNLOADING, "Preparing download...");
    s_st.progress = 0;
    if (!resolve_download_url()) {
        set_state(OTA_ERROR, "Download link failed");
        s_busy = false;
        vTaskDelete(NULL);
    }
    esp_http_client_config_t http = { .url = s_location, .crt_bundle_attach = esp_crt_bundle_attach,
                                      .timeout_ms = 30000, .buffer_size = 8192, .buffer_size_tx = 2048,
                                      .keep_alive_enable = true };
    esp_https_ota_config_t cfg = { .http_config = &http };
    esp_https_ota_handle_t h = NULL;
    wifi_svc_set_high_throughput(true);
    esp_err_t err = esp_https_ota_begin(&cfg, &h);
    if (err == ESP_OK) {
        int total = esp_https_ota_get_image_size(h);
        while ((err = esp_https_ota_perform(h)) == ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
            int done = esp_https_ota_get_image_len_read(h);
            if (total > 0) {
                int p = done * 100 / total;
                if (p != s_st.progress) {
                    s_st.progress = p;
                    ui_on_data_changed();
                }
            }
        }
        if (err == ESP_OK && esp_https_ota_is_complete_data_received(h)) {
            err = esp_https_ota_finish(h);
        } else {
            esp_https_ota_abort(h);
            if (err == ESP_OK) err = ESP_FAIL;
        }
    }
    wifi_svc_set_high_throughput(false);
    if (err == ESP_OK) {
        s_st.progress = 100;
        set_state(OTA_DONE, "Update installed - restarting");
        ui_toast("Update installed - restarting");
        vTaskDelay(pdMS_TO_TICKS(2000));
        esp_restart();
    }
    char msg[64];
    snprintf(msg, sizeof(msg), "Install failed: %s", esp_err_to_name(err));
    set_state(OTA_ERROR, msg);
    s_busy = false;
    vTaskDelete(NULL);
}

void ota_install_async(void)
{
    if (s_busy) return;
    s_busy = true;
    if (!s_st.update_available || !s_asset_url[0]) {
        /* check first, then install */
        xTaskCreate(check_task, "ota_check", 8192, (void *)1, 3, NULL);
        return;
    }
    xTaskCreate(install_task, "ota_inst", 8192, NULL, 3, NULL);
}

/* ------------------------------------------------------------------ browser upload */

static esp_ota_handle_t       s_up;
static const esp_partition_t *s_up_part;
static size_t                 s_up_total, s_up_done;
static bool                   s_up_header_ok;

bool ota_upload_begin(size_t total)
{
    if (s_busy) return false;
    s_up_part = esp_ota_get_next_update_partition(NULL);
    if (!s_up_part || total > s_up_part->size) {
        return false;
    }
    if (esp_ota_begin(s_up_part, OTA_WITH_SEQUENTIAL_WRITES, &s_up) != ESP_OK) {
        return false;
    }
    s_busy = true;
    s_up_total = total;
    s_up_done = 0;
    s_up_header_ok = false;
    s_st.progress = 0;
    set_state(OTA_DOWNLOADING, "Receiving firmware...");
    return true;
}

bool ota_upload_write(const void *data, size_t len)
{
    if (!s_up_done && len && ((const uint8_t *)data)[0] != 0xE9) {
        return false;                       /* not an ESP32 app image */
    }
    s_up_header_ok = true;
    if (esp_ota_write(s_up, data, len) != ESP_OK) {
        return false;
    }
    s_up_done += len;
    int p = s_up_total ? (int)(s_up_done * 100 / s_up_total) : 0;
    if (p != s_st.progress) {
        s_st.progress = p;
        ui_on_data_changed();
    }
    return true;
}

static void reboot_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();
}

bool ota_upload_end(bool ok)
{
    bool result = false;
    if (ok && s_up_header_ok && esp_ota_end(s_up) == ESP_OK && esp_ota_set_boot_partition(s_up_part) == ESP_OK) {
        set_state(OTA_DONE, "Firmware uploaded - restarting");
        ui_toast("Update installed - restarting");
        xTaskCreate(reboot_task, "reboot", 2048, NULL, 3, NULL);
        result = true;
    } else {
        if (!ok || !s_up_header_ok) esp_ota_abort(s_up);
        set_state(OTA_ERROR, "Upload failed (not a valid firmware?)");
    }
    s_busy = false;
    return result;
}

/* ------------------------------------------------------------------ periodic check */

static void auto_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(60 * 1000));           /* let Wi-Fi settle after boot */
    while (true) {
        wifi_status_t w;
        wifi_svc_status(&w);
        if (settings()->ota_auto_check && w.state == WIFI_ST_CONNECTED && !s_busy) {
            ota_check_async();
        }
        vTaskDelay(pdMS_TO_TICKS(6 * 3600 * 1000));  /* every 6 hours */
    }
}

void ota_start(void)
{
    strlcpy(s_st.current, esp_app_get_description()->version, sizeof(s_st.current));
    xTaskCreate(auto_task, "ota_auto", 3072, NULL, 2, NULL);
}
