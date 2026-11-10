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

#define DRAW_LINES 16

static const char *TAG = "main";

static void on_web_transfer(void)
{
    ble_svc_pause_adv_for_transfer();
    wifi_svc_set_high_throughput(true);     /* radio awake while files move, 15 s timeout */
}

/* ------------------------------------------------------------------ display
 * We set the display up ourselves instead of bsp_display_start_with_config(), because that
 * BSP function ignores the buffer settings it is given: it always used a 100-line buffer
 * (82 KB -> lands in PSRAM, which the SPI DMA can't read, so every flush needed a temporary
 * internal copy that often failed = stale/garbage areas on the panel) and registered the
 * QSPI panel as an RGB display, which reports "flush done" before the transfer finished. */

/* the AMOLED controller needs areas that start on an even pixel and have even size */
static void display_rounder(lv_event_t *e)
{
    lv_area_t *a = lv_event_get_param(e);
    a->x1 &= ~1;
    a->y1 &= ~1;
    a->x2 |= 1;
    a->y2 |= 1;
}

static lv_display_t *display_start(void)
{
    const lvgl_port_cfg_t port_cfg = {
        .task_priority = 4,
        .task_stack = 12288,
        .task_affinity = 1,
        .task_max_sleep_ms = 500,
        .timer_period_ms = 10,
    };
    if (lvgl_port_init(&port_cfg) != ESP_OK) return NULL;

    esp_lcd_panel_handle_t panel = NULL;
    esp_lcd_panel_io_handle_t io = NULL;
    const bsp_display_config_t bsp_cfg = {
        .max_transfer_sz = BSP_LCD_H_RES * DRAW_LINES * 2,     /* one buffer = one DMA transfer */
    };
    if (bsp_display_new(&bsp_cfg, &panel, &io) != ESP_OK) return NULL;

    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = io,
        .panel_handle = panel,
        .buffer_size = BSP_LCD_H_RES * DRAW_LINES,
        .double_buffer = true,                 /* draw the next strip while the last one is sent */
        .hres = BSP_LCD_H_RES,
        .vres = BSP_LCD_V_RES,
        .monochrome = false,
        .color_format = LV_COLOR_FORMAT_RGB565,
        .flags = {
            .buff_dma = true,                  /* internal DMA RAM: the SPI reads it directly */
            .buff_spiram = false,
            .swap_bytes = true,
        },
    };
    lv_display_t *disp = lvgl_port_add_disp(&disp_cfg);   /* generic SPI path: waits for DMA done */
    if (!disp) return NULL;
    lv_display_add_event_cb(disp, display_rounder, LV_EVENT_INVALIDATE_AREA, NULL);

    esp_lcd_touch_handle_t tp = NULL;
    if (bsp_touch_new(NULL, &tp) == ESP_OK && tp) {
        const lvgl_port_touch_cfg_t touch_cfg = { .disp = disp, .handle = tp };
        lvgl_port_add_touch(&touch_cfg);
    }
    bsp_display_brightness_init();
    ESP_LOGI(TAG, "display: 2 x %d-line DMA buffers, internal free %u", DRAW_LINES,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    return disp;
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
    esp_pm_config_t pm = { .max_freq_mhz = 160, .min_freq_mhz = 80, .light_sleep_enable = false };
    if (esp_pm_configure(&pm) != ESP_OK) {
        ESP_LOGW(TAG, "power management not available");
    }
    /* network stack first: SNTP, mDNS and Wi-Fi all need it */
    esp_netif_init();
    esp_event_loop_create_default();

    lv_display_t *d = display_start();
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
    history_start();
    net_console_start();
    ota_start();

    /* we got this far: keep this firmware (rollback protection for OTA updates) */
    esp_ota_mark_app_valid_cancel_rollback();
    ESP_LOGI(TAG, "ready");
}
