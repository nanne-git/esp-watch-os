#include "settings.h"

#include <stddef.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "settings";

#define NVS_NS        "watch"
#define NVS_KEY       "settings"
#define SETTINGS_VER  1
#define MAX_LISTENERS 12

typedef struct {
    uint32_t   version;
    settings_t s;
} stored_t;

static settings_t        s_cur;
static settings_t        s_edit;
static SemaphoreHandle_t s_lock;
static settings_cb_t     s_listeners[MAX_LISTENERS];
static int               s_listener_count;

static void set_defaults(settings_t *s)
{
    memset(s, 0, sizeof(*s));
    s->brightness = 70;
    s->screen_timeout_s = 15;
    s->raise_to_wake = true;
    s->always_on = false;
    s->watchface = 0;
    s->accent_color = 0x4C9AFF;
    s->volume = 60;
    s->vibration = true;
    s->haptic_touch = false;
    strcpy(s->timezone, "IST-5:30");
    s->time_24h = false;
    s->auto_time = true;
    s->wifi_on = true;
    s->hotspot_on = false;
    strcpy(s->ap_ssid, "ESP-Watch");
    strcpy(s->ap_pass, "watchfiles");
    strcpy(s->hostname, "esp-watch");
    s->bt_on = true;
    strcpy(s->bt_name, "Bangle.js ESP-Watch");
    s->web_on = true;
    s->notif_on = true;
    s->notif_wake = true;
    s->dnd_start_min = 23 * 60;
    s->dnd_end_min = 7 * 60;
    strcpy(s->ota_repo, "nanne-git/esp-watch-os");
    s->ota_auto_check = true;
    s->latitude = 17.385f;      /* Hyderabad - change in Settings > Weather */
    s->longitude = 78.4867f;
    strcpy(s->city, "Hyderabad");
    s->step_goal = 8000;
}

static void save(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed");
        return;
    }
    stored_t st = { .version = SETTINGS_VER, .s = s_cur };
    esp_err_t err = nvs_set_blob(h, NVS_KEY, &st, sizeof(st));
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "save failed: %s", esp_err_to_name(err));
    }
}

void settings_init(void)
{
    s_lock = xSemaphoreCreateRecursiveMutex();
    set_defaults(&s_cur);

    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        stored_t st;
        size_t len = sizeof(st);
        if (nvs_get_blob(h, NVS_KEY, &st, &len) == ESP_OK && len == sizeof(st) &&
            st.version == SETTINGS_VER) {
            s_cur = st.s;
            ESP_LOGI(TAG, "loaded");
        } else {
            ESP_LOGW(TAG, "no saved settings (or new layout) - using defaults");
        }
        nvs_close(h);
    }
    s_edit = s_cur;
}

const settings_t *settings(void)
{
    return &s_cur;
}

settings_t *settings_edit(void)
{
    xSemaphoreTakeRecursive(s_lock, portMAX_DELAY);
    s_edit = s_cur;
    return &s_edit;          /* lock is released by settings_commit() */
}

void settings_commit(void)
{
    s_cur = s_edit;
    save();
    xSemaphoreGiveRecursive(s_lock);
    for (int i = 0; i < s_listener_count; i++) {
        s_listeners[i]();
    }
}

void settings_on_change(settings_cb_t cb)
{
    if (s_listener_count < MAX_LISTENERS) {
        s_listeners[s_listener_count++] = cb;
    }
}

void settings_factory_reset(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h);
        nvs_commit(h);
        nvs_close(h);
    }
    settings_edit();
    set_defaults(&s_edit);
    settings_commit();
}

/* ------------------------------------------------------------------ JSON field table */

typedef enum { F_BOOL, F_U8, F_U16, F_U32, F_FLOAT, F_STR } ftype_t;

typedef struct {
    const char *name;
    ftype_t     type;
    size_t      offset;
    size_t      size;     /* string buffer size */
    bool        secret;
} field_t;

#define FB(n)    { #n, F_BOOL,  offsetof(settings_t, n), 0, false }
#define FU8(n)   { #n, F_U8,    offsetof(settings_t, n), 0, false }
#define FU16(n)  { #n, F_U16,   offsetof(settings_t, n), 0, false }
#define FU32(n)  { #n, F_U32,   offsetof(settings_t, n), 0, false }
#define FF(n)    { #n, F_FLOAT, offsetof(settings_t, n), 0, false }
#define FS(n)    { #n, F_STR,   offsetof(settings_t, n), sizeof(((settings_t *)0)->n), false }
#define FSS(n)   { #n, F_STR,   offsetof(settings_t, n), sizeof(((settings_t *)0)->n), true }

static const field_t FIELDS[] = {
    FU8(brightness), FU16(screen_timeout_s), FB(raise_to_wake), FB(always_on), FU8(watchface),
    FU32(accent_color), FU8(volume), FB(muted), FB(vibration), FB(haptic_touch),
    FS(timezone), FB(time_24h), FB(auto_time),
    FB(wifi_on), FB(hotspot_on), FS(ap_ssid), FSS(ap_pass), FS(hostname),
    FB(bt_on), FS(bt_name), FB(web_on), FSS(web_pass),
    FB(notif_on), FB(notif_wake), FB(dnd), FB(dnd_scheduled), FU16(dnd_start_min), FU16(dnd_end_min),
    FB(power_save), FS(ota_repo), FSS(ota_token), FB(ota_auto_check),
    FF(latitude), FF(longitude), FS(city), FU16(step_goal),
};

cJSON *settings_to_json(void)
{
    cJSON *o = cJSON_CreateObject();
    const uint8_t *base = (const uint8_t *)&s_cur;
    for (size_t i = 0; i < sizeof(FIELDS) / sizeof(FIELDS[0]); i++) {
        const field_t *f = &FIELDS[i];
        const void *p = base + f->offset;
        switch (f->type) {
        case F_BOOL:  cJSON_AddBoolToObject(o, f->name, *(const bool *)p); break;
        case F_U8:    cJSON_AddNumberToObject(o, f->name, *(const uint8_t *)p); break;
        case F_U16:   cJSON_AddNumberToObject(o, f->name, *(const uint16_t *)p); break;
        case F_U32:   cJSON_AddNumberToObject(o, f->name, *(const uint32_t *)p); break;
        case F_FLOAT: cJSON_AddNumberToObject(o, f->name, *(const float *)p); break;
        case F_STR:
            cJSON_AddStringToObject(o, f->name,
                                    (f->secret && ((const char *)p)[0]) ? "***" : (const char *)p);
            break;
        }
    }
    return o;
}

int settings_from_json(const cJSON *obj)
{
    int changed = 0;
    settings_t *s = settings_edit();
    uint8_t *base = (uint8_t *)s;
    for (size_t i = 0; i < sizeof(FIELDS) / sizeof(FIELDS[0]); i++) {
        const field_t *f = &FIELDS[i];
        const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, f->name);
        if (!v) {
            continue;
        }
        void *p = base + f->offset;
        switch (f->type) {
        case F_BOOL:
            if (cJSON_IsBool(v)) { *(bool *)p = cJSON_IsTrue(v); changed++; }
            break;
        case F_U8:
            if (cJSON_IsNumber(v)) { *(uint8_t *)p = (uint8_t)v->valuedouble; changed++; }
            break;
        case F_U16:
            if (cJSON_IsNumber(v)) { *(uint16_t *)p = (uint16_t)v->valuedouble; changed++; }
            break;
        case F_U32:
            if (cJSON_IsNumber(v)) { *(uint32_t *)p = (uint32_t)v->valuedouble; changed++; }
            break;
        case F_FLOAT:
            if (cJSON_IsNumber(v)) { *(float *)p = (float)v->valuedouble; changed++; }
            break;
        case F_STR:
            /* "***" means "unchanged secret" (what we sent out) */
            if (cJSON_IsString(v) && !(f->secret && strcmp(v->valuestring, "***") == 0)) {
                strlcpy((char *)p, v->valuestring, f->size);
                changed++;
            }
            break;
        }
    }
    if (s->brightness < 5) s->brightness = 5;
    if (s->brightness > 100) s->brightness = 100;
    if (s->volume > 100) s->volume = 100;
    settings_commit();
    return changed;
}
