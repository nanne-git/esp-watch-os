/*
 * ESP-Watch OS - custom firmware for the Waveshare ESP32-S3-Touch-AMOLED-2.06.
 *
 * Start order: NVS/settings -> display (BSP + LVGL) -> hardware -> time -> SD card ->
 * audio -> UI -> Bluetooth -> Wi-Fi -> web server -> background services.
 */
#include "alarm.h"
#include "audio.h"
#include "ble_svc.h"
#include "bsp/esp-bsp.h"
#include "display.h"
#include "bsp/display.h"
#include "bsp/touch.h"
#include "esp_heap_caps.h"
#include "esp_lvgl_port.h"
#include "esp_lcd_touch.h"
#include "devtools.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_pm.h"
#include "esp_system.h"
#include "file_manager.h"
#include "hw.h"
#include "nvs_flash.h"
#include "ota.h"
#include "settings.h"
#include "timesvc.h"
#include "ui.h"
#include "weather.h"
#include "history.h"
#include "net_console.h"
#include "web_server.h"
#include "wifi_svc.h"

static const char *TAG = "main";

static void on_web_transfer(void)
{
    ble_svc_pause_adv_for_transfer();
    wifi_svc_set_high_throughput(true);     /* radio awake while files move, 15 s timeout */
}

void app_main(void)
{
    const esp_app_desc_t *app = esp_app_get_description();
    ESP_LOGI(TAG, "ESP-Watch OS %s (built %s %s)", app->version, app->date, app->time);

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    devtools_init();       /* log capture + boot counter (needs NVS) */
    settings_init();

    /* dynamic frequency scaling: 80 MHz when idle, up to 160 MHz while someone holds a lock
     * (the UI holds one while the screen is on; Wi-Fi/SPI drivers take their own) */
    esp_pm_config_t pm = { .max_freq_mhz = 240, .min_freq_mhz = 80, .light_sleep_enable = false };   /* 240 only while drawing (ui.c) */
    if (esp_pm_configure(&pm) != ESP_OK) {
        ESP_LOGW(TAG, "power management not available");
    }
    /* network stack first: SNTP, mDNS and Wi-Fi all need it */
    esp_netif_init();
    esp_event_loop_create_default();

    lv_display_t *d = display_start();
    if (!d) {
        ESP_LOGE(TAG, "display start failed - restarting");
        vTaskDelay(pdMS_TO_TICKS(2000));
        esp_restart();               /* never run the UI without a display (would crash-loop) */
    }
    display_brightness_set(settings()->brightness);

    hw_init();
    timesvc_init();
    file_manager_start(ble_svc_send);      /* mounts the SD card */
    audio_init();
    alarm_init();
    ui_start();

    ble_svc_start();
    wifi_svc_start();
    web_server_start(on_web_transfer);
    weather_start();
    history_start();
    net_console_start();
    ota_start();

    /* we got this far: keep this firmware (rollback protection for OTA updates) */
    esp_ota_mark_app_valid_cancel_rollback();
    ESP_LOGI(TAG, "ready");
}
