/* Display + touch + LVGL port bring-up, and panel brightness. */
#pragma once
#include "esp_err.h"
#include "lvgl.h"

lv_display_t *display_start(void);          /* NULL on failure */
esp_err_t     display_brightness_set(int percent);   /* 0..100 */
