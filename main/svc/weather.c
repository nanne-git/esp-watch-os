#include "weather.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "settings.h"
#include "ui.h"
#include "wifi_svc.h"

static const char *TAG = "weather";

static weather_t    s_w;
static TaskHandle_t s_task;
static bool         s_from_net;      /* current data came from open-meteo */

void weather_get(weather_t *out) { *out = s_w; }

const char *weather_desc(int c)
{
    if (c == 0) return "Clear";
    if (c <= 2) return "Partly cloudy";
    if (c == 3) return "Cloudy";
    if (c == 45 || c == 48) return "Fog";
    if (c >= 51 && c <= 57) return "Drizzle";
    if (c >= 61 && c <= 67) return "Rain";
    if (c >= 71 && c <= 77) return "Snow";
    if (c >= 80 && c <= 82) return "Showers";
    if (c >= 95) return "Thunderstorm";
    return "--";
}

/* OpenWeatherMap condition code (Gadgetbridge) -> WMO code (open-meteo) */
static int owm_to_wmo(int c)
{
    if (c >= 200 && c < 300) return 95;
    if (c >= 300 && c < 400) return 51;
    if (c >= 500 && c < 600) return c >= 520 ? 80 : 61;
    if (c >= 600 && c < 700) return 71;
    if (c >= 700 && c < 800) return 45;
    if (c == 800) return 0;
    if (c == 801 || c == 802) return 2;
    return 3;
}

void weather_set_from_phone(float temp_c, int owm_code, int humidity, float wind_kmh, float hi_c, float lo_c)
{
    /* fresh internet data (< 1 h) wins; otherwise take the phone's */
    if (s_from_net && time(NULL) - s_w.updated < 3600) {
        return;
    }
    weather_t w = s_w;
    w.valid = true;
    w.temp_c = temp_c;
    w.feels_c = temp_c;
    w.code = owm_to_wmo(owm_code);
    w.humidity = humidity;
    w.wind_kmh = wind_kmh;
    w.tmax = hi_c;
    w.tmin = lo_c;
    w.daily_code[0] = w.code;
    w.daily_max[0] = hi_c;
    w.daily_min[0] = lo_c;
    w.updated = time(NULL);
    s_w = w;
    s_from_net = false;
    ui_on_data_changed();
}

static bool fetch(void)
{
    char url[320];
    snprintf(url, sizeof(url),
             "https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f"
             "&current=temperature_2m,apparent_temperature,relative_humidity_2m,weather_code,wind_speed_10m"
             "&daily=weather_code,temperature_2m_max,temperature_2m_min&forecast_days=5&timezone=auto",
             settings()->latitude, settings()->longitude);
    esp_http_client_config_t cfg = { .url = url, .crt_bundle_attach = esp_crt_bundle_attach, .timeout_ms = 10000 };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) {
        return false;
    }
    bool ok = false;
    char *body = NULL;
    if (esp_http_client_open(c, 0) == ESP_OK) {
        esp_http_client_fetch_headers(c);
        int cap = 8192, len = 0, r;
        body = malloc(cap);
        while (body && (r = esp_http_client_read(c, body + len, cap - len - 1)) > 0) {
            len += r;
            if (len >= cap - 1) break;
        }
        if (body) {
            body[len] = '\0';
        }
    }
    esp_http_client_cleanup(c);
    if (!body) {
        return false;
    }
    cJSON *j = cJSON_Parse(body);
    free(body);
    if (!j) {
        return false;
    }
    cJSON *cur = cJSON_GetObjectItem(j, "current");
    cJSON *daily = cJSON_GetObjectItem(j, "daily");
    if (cur && daily) {
        weather_t w = { 0 };
        w.temp_c = cJSON_GetObjectItem(cur, "temperature_2m")->valuedouble;
        w.feels_c = cJSON_GetObjectItem(cur, "apparent_temperature")->valuedouble;
        w.humidity = cJSON_GetObjectItem(cur, "relative_humidity_2m")->valueint;
        w.code = cJSON_GetObjectItem(cur, "weather_code")->valueint;
        w.wind_kmh = cJSON_GetObjectItem(cur, "wind_speed_10m")->valuedouble;
        cJSON *codes = cJSON_GetObjectItem(daily, "weather_code");
        cJSON *mx = cJSON_GetObjectItem(daily, "temperature_2m_max");
        cJSON *mn = cJSON_GetObjectItem(daily, "temperature_2m_min");
        for (int i = 0; i < 5 && i < cJSON_GetArraySize(codes); i++) {
            w.daily_code[i] = cJSON_GetArrayItem(codes, i)->valueint;
            w.daily_max[i] = cJSON_GetArrayItem(mx, i)->valuedouble;
            w.daily_min[i] = cJSON_GetArrayItem(mn, i)->valuedouble;
        }
        w.tmax = w.daily_max[0];
        w.tmin = w.daily_min[0];
        w.updated = time(NULL);
        w.valid = true;
        s_w = w;
        s_from_net = true;
        ok = true;
        ESP_LOGI(TAG, "%.1f C, %s", w.temp_c, weather_desc(w.code));
    }
    cJSON_Delete(j);
    return ok;
}

static void weather_task(void *arg)
{
    while (true) {
        wifi_status_t ws;
        wifi_svc_status(&ws);
        uint32_t wait_ms = 60 * 1000;
        if (ws.state == WIFI_ST_CONNECTED) {
            if (fetch()) {
                ui_on_data_changed();
                wait_ms = 30 * 60 * 1000;          /* every 30 minutes */
            }
        }
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(wait_ms));
    }
}

void weather_refresh(void)
{
    if (s_task) {
        xTaskNotifyGive(s_task);
    }
}

void weather_start(void)
{
    /* internal stack: a PSRAM stack gets corrupted when an interrupt hits while another task
     * writes flash (PSRAM cache off) - it crashed the scheduler */
    xTaskCreate(weather_task, "weather", 6144, NULL, 2, &s_task);
}
