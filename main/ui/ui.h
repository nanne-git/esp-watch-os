/*
 * UI framework (LVGL 9).
 *
 * Navigation: a persistent HOME screen (watch face) plus a stack of PAGES.
 *   Home gestures: swipe up = apps, down = quick settings, left = notifications, right = widgets.
 *   Pages: swipe right or BOOT button = back; BOOT long press = home.
 *   PWR button: screen on/off; long press = power menu.
 *
 * Thread rules: LVGL may only be touched while holding the UI lock. Functions marked
 * "any task" take the lock themselves; page/widget helpers must run in LVGL context
 * (event callbacks, lv_timers, or between ui_lock()/ui_unlock()).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "lvgl.h"
#include "phone.h"

#define UI_W 410
#define UI_H 502

/* colours */
#define UI_BG       0x000000
#define UI_CARD     0x1C1C1E
#define UI_CARD2    0x2C2C2E
#define UI_TEXT     0xFFFFFF
#define UI_MUTED    0x8E8E93
#define UI_GREEN    0x30D158
#define UI_RED      0xFF453A
#define UI_ORANGE   0xFF9F0A
#define UI_BLUE     0x0A84FF
#define UI_PURPLE   0xBF5AF2
#define UI_TEAL     0x40C8E0
#define UI_YELLOW   0xFFD60A
#define UI_PINK     0xFF375F

lv_color_t ui_accent(void);

/* ---- lifecycle ---- */
void ui_start(void);                  /* after bsp_display_start(); builds the home screen */
bool ui_lock(void);
void ui_unlock(void);

/* ---- service hooks (any task) ---- */
void ui_on_notification(const notif_t *n, bool quiet);
void ui_on_call(bool incoming);
void ui_on_alarm(const char *label);           /* shows the alarm screen until dismissed */
void ui_on_timer_done(void);
void ui_on_data_changed(void);                 /* refresh status soon */
void ui_toast(const char *msg);
void ui_wake(void);
void ui_sleep(void);
bool ui_is_awake(void);
void ui_keep_awake(bool on);                   /* e.g. video / flashlight / stopwatch view */

/* ---- pages (LVGL context) ---- */
typedef void (*page_build_t)(lv_obj_t *content, void *ctx);
typedef void (*page_fn_t)(void *ctx);

/* Push a page: header with back arrow + title, scrollable column content. */
lv_obj_t *ui_page_push(const char *title, page_build_t build, page_fn_t on_close, void *ctx);
/* Same, but the content area is a plain container (no padding/flex) for custom layouts. */
lv_obj_t *ui_page_push_raw(const char *title, page_build_t build, page_fn_t on_close, void *ctx);
void ui_page_set_refresh(page_fn_t refresh);   /* called ~1/s while the page is on top */
void ui_page_pop(void);
void ui_page_rebuild(void);                    /* clear + build the top page again */
void ui_go_home(void);
int  ui_page_depth(void);

/* ---- remote control / testing (any task) ---- */
void ui_inject_tap(int x, int y);
void ui_inject_long_press(int x, int y);
void ui_inject_swipe(int x1, int y1, int x2, int y2);
void ui_inject_key(const char *key);           /* "back", "home", "power", "wake" */
uint8_t *ui_screenshot_bmp(size_t *out_len);   /* malloc'd BMP, caller frees */

/* ---- widget helpers (LVGL context) ---- */
lv_obj_t *ui_label(lv_obj_t *parent, const char *text, const lv_font_t *font, uint32_t color);
lv_obj_t *ui_section(lv_obj_t *parent, const char *title);
lv_obj_t *ui_card(lv_obj_t *parent);
/* list row: icon (LV_SYMBOL_* or NULL), text, optional value on the right, click callback */
lv_obj_t *ui_row(lv_obj_t *parent, const char *icon, uint32_t icon_color, const char *text,
                 const char *value, lv_event_cb_t cb, void *user);
lv_obj_t *ui_row_switch(lv_obj_t *parent, const char *icon, uint32_t icon_color, const char *text,
                        bool on, lv_event_cb_t cb, void *user);           /* returns the switch */
lv_obj_t *ui_row_slider(lv_obj_t *parent, const char *icon, const char *text, int min, int max,
                        int val, lv_event_cb_t cb, void *user);           /* returns the slider */
lv_obj_t *ui_button(lv_obj_t *parent, const char *text, uint32_t color, lv_event_cb_t cb, void *user);
lv_obj_t *ui_round_button(lv_obj_t *parent, const char *symbol, int size, uint32_t color,
                          lv_event_cb_t cb, void *user);
void ui_row_set_value(lv_obj_t *row, const char *value);

typedef void (*ui_text_cb_t)(const char *text, void *user);
void ui_keyboard(const char *title, const char *initial, bool password, ui_text_cb_t done, void *user);
typedef void (*ui_confirm_cb_t)(void *user);
void ui_confirm(const char *title, const char *msg, const char *yes_text, uint32_t yes_color,
                ui_confirm_cb_t yes, void *user);
/* pick one of several options; selected index passed to cb */
typedef void (*ui_choice_cb_t)(int index, void *user);
void ui_choose(const char *title, const char *const *options, int count, int current,
               ui_choice_cb_t cb, void *user);
/* number picker (e.g. hours/minutes) */
void ui_number_picker(const char *title, int min, int max, int val, ui_choice_cb_t cb, void *user);

/* home-screen pieces (ui_home.c) */
void ui_home_build(lv_obj_t *scr);
void ui_home_refresh(void);
void ui_open_launcher(void);
void ui_open_quick_settings(void);
void ui_open_notifications(void);
void ui_open_widgets(void);
void ui_open_face_picker(void);
void ui_open_power_menu(void);

/* Run slow work (SD card, Wi-Fi scan, file copy) OFF the UI task so the screen never freezes.
 * Shows a spinner with busy_msg; work(ctx) runs in a background task; done(ctx) then runs in
 * LVGL context. One job at a time - returns false if one is already running. */
typedef void (*ui_bg_fn_t)(void *ctx);
bool ui_run_bg(const char *busy_msg, ui_bg_fn_t work, ui_bg_fn_t done, void *ctx);

/* recent apps (ui_recents.c) */
void ui_open_recents(void);
void ui_recents_note(const char *name, const char *icon, bool icon_img, uint32_t color,
                     void (*open)(void), const char *lua_id);
