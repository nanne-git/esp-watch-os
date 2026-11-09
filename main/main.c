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
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "file_manager.h"
#include "hw.h"
#include "nvs_flash.h"
#include "ota.h"
#include "settings.h"
#include "timesvc.h"
#include "ui.h"
#include "weather.h"
#include "web_server.h"
#include "wifi_svc.h"

static const char *TAG = "main";

static void on_web_transfer(void)
{
    ble_svc_pause_adv_for_transfer();
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
    settings_init();
    /* network stack first: SNTP, mDNS and Wi-Fi all need it */
    esp_netif_init();
    esp_event_loop_create_default();

    bsp_display_cfg_t disp = {
        .lvgl_port_cfg = {
            .task_priority = 4,
            .task_stack = 12288,
            .task_affinity = 1,
            .task_max_sleep_ms = 500,
            .timer_period_ms = 5,
        },
        /* LVGL draws into a large PSRAM buffer; the port copies 20-line chunks into a small
         * internal DMA buffer for the SPI transfer, so Wi-Fi/BLE fragmenting internal RAM
         * can't starve the display */
        .buffer_size = BSP_LCD_H_RES * 120,
        .trans_size = BSP_LCD_H_RES * 20,
        .double_buffer = false,
        .flags = { .buff_dma = false, .buff_spiram = true },
    };
    lv_display_t *d = bsp_display_start_with_config(&disp);
    if (!d) {
        ESP_LOGE(TAG, "display start failed");
    }
    bsp_display_brightness_set(settings()->brightness);

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
    ota_start();

    /* we got this far: keep this firmware (rollback protection for OTA updates) */
    esp_ota_mark_app_valid_cancel_rollback();
    ESP_LOGI(TAG, "ready");
}
