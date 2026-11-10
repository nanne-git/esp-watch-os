/*
 * Display bring-up (SH8601 / CO5300 AMOLED over QSPI) + LVGL port.
 *
 * Done here instead of the BSP because:
 *  - bsp_display_start_with_config() ignores buffer settings and registers the QSPI panel
 *    as an RGB display ("flush done" before the transfer finished) -> stale/garbage pixels;
 *  - bsp_display_new() can't enable psram_dma_direct, which lets the SPI DMA read draw
 *    buffers straight from PSRAM. That allows TALL draw strips (fewer passes over every
 *    object per frame = faster) without using the scarce internal RAM.
 */
#include "display.h"

#include "bsp/display.h"
#include "bsp/esp-bsp.h"
#include "bsp/touch.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_sh8601.h"
#include "esp_lcd_touch.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "driver/spi_master.h"

static const char *TAG = "display";

/* 1 = draw buffers in PSRAM (tall strips, DMA reads PSRAM directly)
 * 0 = draw buffers in internal DMA RAM
 * Measured on the watch: PSRAM-direct DMA can't keep up with the 40 MHz QSPI panel - the
 * transfer never completes and the UI hangs. One 32-line internal buffer was 12-15 %
 * faster than 2 x 16 lines (fewer strips = fewer passes over every object). */
#define DRAW_IN_PSRAM 0
#if DRAW_IN_PSRAM
#define DRAW_LINES 128
#define DRAW_DOUBLE true
#else
#define DRAW_LINES 32
#define DRAW_DOUBLE false
#endif

volatile uint32_t g_frames, g_flush_px;      /* debug counters for `top` */

static esp_lcd_panel_io_handle_t s_io;
static esp_lcd_panel_handle_t    s_panel;

/* panel init sequence (from the Waveshare BSP) */
static const sh8601_lcd_init_cmd_t s_init_cmds[] = {
    {0x11, (uint8_t[]){0x00}, 0, 120},
    {0xC4, (uint8_t[]){0x80}, 1, 0},
    {0x44, (uint8_t[]){0x01, 0xD1}, 2, 0},
    {0x35, (uint8_t[]){0x00}, 1, 0},
    {0x53, (uint8_t[]){0x20}, 1, 10},
    {0x63, (uint8_t[]){0xFF}, 1, 10},
    {0x51, (uint8_t[]){0x00}, 1, 10},
    {0x2A, (uint8_t[]){0x00, 0x16, 0x01, 0xAF}, 4, 0},
    {0x2B, (uint8_t[]){0x00, 0x00, 0x01, 0xF5}, 4, 0},
    {0x29, (uint8_t[]){0x00}, 0, 10},
    {0x51, (uint8_t[]){0xFF}, 1, 0},
};

esp_err_t display_brightness_set(int percent)
{
    if (!s_io) return ESP_ERR_INVALID_STATE;
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    uint8_t v = (uint8_t)(percent * 255 / 100);
    return esp_lcd_panel_io_tx_param(s_io, (0x02 << 24) | (0x51 << 8), &v, 1);   /* QSPI "write brightness" */
}

static bool panel_new(size_t max_transfer)
{
    const spi_bus_config_t bus = SH8601_PANEL_BUS_QSPI_CONFIG(BSP_LCD_PCLK, BSP_LCD_DATA0, BSP_LCD_DATA1,
                                                              BSP_LCD_DATA2, BSP_LCD_DATA3, max_transfer);
    if (spi_bus_initialize(BSP_LCD_SPI_NUM, &bus, SPI_DMA_CH_AUTO) != ESP_OK) return false;

    esp_lcd_panel_io_spi_config_t io_cfg = SH8601_PANEL_IO_QSPI_CONFIG(BSP_LCD_CS, NULL, NULL);
    io_cfg.flags.psram_dma_direct = DRAW_IN_PSRAM;
    if (esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)BSP_LCD_SPI_NUM, &io_cfg, &s_io) != ESP_OK) {
        return false;
    }
    sh8601_vendor_config_t vendor = {
        .init_cmds = s_init_cmds,
        .init_cmds_size = sizeof(s_init_cmds) / sizeof(s_init_cmds[0]),
        .flags = { .use_qspi_interface = 1 },
    };
    const esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = BSP_LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = BSP_LCD_BITS_PER_PIXEL,
        .vendor_config = &vendor,
    };
    if (esp_lcd_new_panel_sh8601(s_io, &panel_cfg, &s_panel) != ESP_OK) return false;
    esp_lcd_panel_reset(s_panel);
    esp_lcd_panel_init(s_panel);
    esp_lcd_panel_set_gap(s_panel, 0x16, 0);
    esp_lcd_panel_disp_on_off(s_panel, true);
    return true;
}

/* the AMOLED controller needs areas that start on an even pixel and have even size */
static void rounder_cb(lv_event_t *e)
{
    lv_area_t *a = lv_event_get_param(e);
    a->x1 &= ~1;
    a->y1 &= ~1;
    a->x2 |= 1;
    a->y2 |= 1;
}

static void frame_cb(lv_event_t *e) { g_frames++; }
static void inval_cb(lv_event_t *e) { g_flush_px += lv_area_get_size((const lv_area_t *)lv_event_get_param(e)); }

lv_display_t *display_start(void)
{
    const lvgl_port_cfg_t port_cfg = {
        .task_priority = 4,
        .task_stack = 16384,          /* -O2 + deep layouts: 12 KB left only 2.5 KB spare */
        .task_affinity = 1,
        .task_max_sleep_ms = 500,
        .timer_period_ms = 10,
    };
    if (lvgl_port_init(&port_cfg) != ESP_OK) return NULL;
    if (!panel_new(BSP_LCD_H_RES * DRAW_LINES * 2)) return NULL;

    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = s_io,
        .panel_handle = s_panel,
        .buffer_size = BSP_LCD_H_RES * DRAW_LINES,
        .double_buffer = DRAW_DOUBLE,
        .hres = BSP_LCD_H_RES,
        .vres = BSP_LCD_V_RES,
        .monochrome = false,
        .color_format = LV_COLOR_FORMAT_RGB565,
        .flags = {
            .buff_dma = !DRAW_IN_PSRAM,
            .buff_spiram = DRAW_IN_PSRAM,
            .swap_bytes = true,
        },
    };
    lv_display_t *disp = lvgl_port_add_disp(&disp_cfg);   /* generic SPI path: waits for DMA done */
    if (!disp) return NULL;
    lv_display_add_event_cb(disp, rounder_cb, LV_EVENT_INVALIDATE_AREA, NULL);
    lv_display_add_event_cb(disp, frame_cb, LV_EVENT_RENDER_READY, NULL);
    lv_display_add_event_cb(disp, inval_cb, LV_EVENT_INVALIDATE_AREA, NULL);

    esp_lcd_touch_handle_t tp = NULL;
    if (bsp_touch_new(NULL, &tp) == ESP_OK && tp) {
        const lvgl_port_touch_cfg_t touch_cfg = { .disp = disp, .handle = tp };
        lvgl_port_add_touch(&touch_cfg);
    }
    display_brightness_set(100);
    ESP_LOGI(TAG, "%s %d-line draw buffer(s) in %s, internal free %u", DRAW_DOUBLE ? "2 x" : "1 x", DRAW_LINES,
             DRAW_IN_PSRAM ? "PSRAM" : "internal RAM", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    return disp;
}
