#include "ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bsp/esp-bsp.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_pm.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "hw.h"
#include "settings.h"

static const char *TAG = "ui";

#define PAGE_MAX 10

typedef struct {
    lv_obj_t   *scr;
    lv_obj_t   *content;
    page_build_t build;
    page_fn_t   on_close;
    page_fn_t   refresh;
    void       *ctx;
    bool        raw;
    char        title[32];
} page_t;

static lv_obj_t *s_home;
static page_t    s_pages[PAGE_MAX];
static int       s_depth;
static volatile bool s_awake = true;
static volatile bool s_dirty;
static int       s_keep_awake;
static bool      s_dimmed_aod;
static int64_t   s_wake_time;
static lv_obj_t *s_call_ov, *s_alarm_ov, *s_notif_banner;
static lv_timer_t *s_banner_timer;
static esp_pm_lock_handle_t s_cpu_lock;   /* held while the screen is on */
static bool s_cpu_locked;

static void cpu_fast(bool on)
{
    if (!s_cpu_lock || on == s_cpu_locked) return;
    s_cpu_locked = on;
    if (on) esp_pm_lock_acquire(s_cpu_lock); else esp_pm_lock_release(s_cpu_lock);
}

lv_color_t ui_accent(void)
{
    return lv_color_hex(settings()->accent_color);
}

bool ui_lock(void) { return lvgl_port_lock(0); }
void ui_unlock(void) { lvgl_port_unlock(); }

/* ======================================================================== pages */

static void page_gesture_cb(lv_event_t *e)
{
    lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_active());
    if (dir == LV_DIR_RIGHT) {
        lv_indev_wait_release(lv_indev_active());
        ui_page_pop();
    }
}

static void back_btn_cb(lv_event_t *e)
{
    ui_page_pop();
}

static lv_obj_t *new_screen(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_hex(UI_BG), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(scr, lv_color_hex(UI_TEXT), 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    return scr;
}

static void build_page(page_t *p)
{
    lv_obj_clean(p->scr);
    lv_obj_t *hdr = lv_obj_create(p->scr);
    lv_obj_remove_style_all(hdr);
    lv_obj_set_size(hdr, UI_W, 64);
    lv_obj_align(hdr, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_remove_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *back = lv_button_create(hdr);
    lv_obj_set_size(back, 56, 48);
    lv_obj_align(back, LV_ALIGN_LEFT_MID, 8, 4);
    lv_obj_set_style_bg_opa(back, LV_OPA_TRANSP, 0);
    lv_obj_set_style_shadow_width(back, 0, 0);
    lv_obj_add_event_cb(back, back_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bl = lv_label_create(back);
    lv_label_set_text(bl, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_font(bl, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(bl, ui_accent(), 0);
    lv_obj_center(bl);

    lv_obj_t *t = lv_label_create(hdr);
    lv_label_set_text(t, p->title);
    lv_obj_set_style_text_font(t, &lv_font_montserrat_24, 0);
    lv_label_set_long_mode(t, LV_LABEL_LONG_DOT);
    lv_obj_set_width(t, UI_W - 140);
    lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(t, LV_ALIGN_CENTER, 0, 4);

    lv_obj_t *c = lv_obj_create(p->scr);
    lv_obj_remove_style_all(c);
    lv_obj_set_size(c, UI_W, UI_H - 64);
    lv_obj_align(c, LV_ALIGN_TOP_MID, 0, 64);
    if (!p->raw) {
        lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_hor(c, 16, 0);
        lv_obj_set_style_pad_top(c, 4, 0);
        lv_obj_set_style_pad_bottom(c, 40, 0);
        lv_obj_set_style_pad_row(c, 8, 0);
        lv_obj_set_scroll_dir(c, LV_DIR_VER);
        lv_obj_set_scrollbar_mode(c, LV_SCROLLBAR_MODE_ACTIVE);
    } else {
        lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    }
    p->content = c;
    p->refresh = NULL;
    if (p->build) {
        p->build(c, p->ctx);
    }
}

static lv_obj_t *push(const char *title, page_build_t build, page_fn_t on_close, void *ctx, bool raw)
{
    if (s_depth >= PAGE_MAX) {
        ESP_LOGW(TAG, "page stack full");
        return NULL;
    }
    page_t *p = &s_pages[s_depth++];
    memset(p, 0, sizeof(*p));
    p->scr = new_screen();
    p->build = build;
    p->on_close = on_close;
    p->ctx = ctx;
    p->raw = raw;
    strlcpy(p->title, title ? title : "", sizeof(p->title));
    lv_obj_add_event_cb(p->scr, page_gesture_cb, LV_EVENT_GESTURE, NULL);
    build_page(p);
    lv_screen_load_anim(p->scr, LV_SCREEN_LOAD_ANIM_MOVE_LEFT, 180, 0, false);
    return p->content;
}

lv_obj_t *ui_page_push(const char *t, page_build_t b, page_fn_t c, void *x) { return push(t, b, c, x, false); }
lv_obj_t *ui_page_push_raw(const char *t, page_build_t b, page_fn_t c, void *x) { return push(t, b, c, x, true); }

void ui_page_set_refresh(page_fn_t refresh)
{
    if (s_depth > 0) {
        s_pages[s_depth - 1].refresh = refresh;
    }
}

void ui_page_rebuild(void)
{
    if (s_depth > 0) {
        build_page(&s_pages[s_depth - 1]);
    }
}

int ui_page_depth(void) { return s_depth; }

void ui_page_pop(void)
{
    if (s_depth == 0) {
        return;
    }
    page_t *p = &s_pages[--s_depth];
    if (p->on_close) {
        p->on_close(p->ctx);
    }
    lv_obj_t *prev = s_depth > 0 ? s_pages[s_depth - 1].scr : s_home;
    lv_screen_load_anim(prev, LV_SCREEN_LOAD_ANIM_MOVE_RIGHT, 180, 0, true);   /* deletes p->scr */
    p->scr = NULL;
    if (s_depth > 0 && s_pages[s_depth - 1].refresh) {
        s_pages[s_depth - 1].refresh(s_pages[s_depth - 1].ctx);
    } else if (s_depth == 0) {
        ui_home_refresh();
    }
}

/*
 * Close every page and show the watch face.
 * The page on screen must be the LAST thing removed: deleting the active screen first
 * (the old code did that when 2+ pages were open) leaves LVGL without a valid screen,
 * so it only repaints the clock digits and the old page's pixels stay on the panel.
 */
static void go_home(bool animate)
{
    if (s_depth == 0) {
        return;
    }
    lv_obj_t *top = s_pages[s_depth - 1].scr;
    while (s_depth > 0) {
        page_t *p = &s_pages[--s_depth];
        if (p->on_close) {
            p->on_close(p->ctx);
        }
        if (p->scr != top) {
            lv_obj_delete(p->scr);   /* hidden pages underneath: delete directly */
        }
        p->scr = NULL;
    }
    if (animate) {
        lv_screen_load_anim(s_home, LV_SCREEN_LOAD_ANIM_MOVE_RIGHT, 180, 0, true);  /* deletes top */
    } else {
        lv_screen_load(s_home);
        lv_obj_delete(top);
    }
    lv_obj_invalidate(s_home);       /* repaint the whole panel, not just what changed */
    ui_home_refresh();
}

void ui_go_home(void) { go_home(true); }

/* ======================================================================== sleep / wake */

static void apply_brightness(void)
{
    const settings_t *s = settings();
    int b = s->brightness;
    if (s->power_save && b > 50) {
        b = 50;
    }
    bsp_display_brightness_set(b);
}

bool ui_is_awake(void) { return s_awake; }

/* While the screen is off, a full-screen "shield" on the system layer swallows every touch:
 * the first touch only wakes the watch and can't press whatever is under it (e.g. a "Power
 * off" confirm left open, or a swipe opening apps in a pocket). */
static lv_obj_t *s_shield;
static void do_wake(void);

static void shield_cb(lv_event_t *e)
{
    do_wake();
}

static void shield_set(bool on)
{
    if (on && !s_shield) {
        s_shield = lv_obj_create(lv_layer_sys());
        lv_obj_remove_style_all(s_shield);
        lv_obj_set_size(s_shield, UI_W, UI_H);
        lv_obj_add_flag(s_shield, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(s_shield, shield_cb, LV_EVENT_PRESSED, NULL);
    } else if (!on && s_shield) {
        lv_obj_delete(s_shield);       /* safe even from inside its own PRESSED event */
        s_shield = NULL;
    }
}

static void do_wake(void)
{
    shield_set(false);
    if (!s_awake || s_dimmed_aod) {
        cpu_fast(true);
        s_awake = true;
        s_dimmed_aod = false;
        s_wake_time = esp_timer_get_time();
        lv_display_trigger_activity(NULL);
        ui_home_refresh();
        apply_brightness();
    }
}

static void do_sleep(void)
{
    if (!s_awake) {
        return;
    }
    if (s_call_ov || s_alarm_ov) {
        return;
    }
    s_awake = false;
    go_home(false);            /* instant switch: no slide animation into a dark screen */
    /* close leftover confirm dialogs (marked USER_4 by ui_confirm) */
    for (int i = (int)lv_obj_get_child_count(lv_layer_top()) - 1; i >= 0; i--) {
        lv_obj_t *o = lv_obj_get_child(lv_layer_top(), i);
        if (lv_obj_has_flag(o, LV_OBJ_FLAG_USER_4)) lv_obj_delete(o);
    }
    shield_set(true);
    cpu_fast(false);           /* screen off: let the CPU drop to 80 MHz */
    if (settings()->always_on) {
        s_dimmed_aod = true;
        ui_home_refresh();
        lv_obj_invalidate(s_home);
        bsp_display_brightness_set(6);
    } else {
        bsp_display_brightness_set(0);
    }
}

void ui_wake(void)
{
    if (ui_lock()) {
        do_wake();
        ui_unlock();
    }
}

void ui_sleep(void)
{
    if (ui_lock()) {
        do_sleep();
        ui_unlock();
    }
}

void ui_keep_awake(bool on)
{
    s_keep_awake += on ? 1 : -1;
    if (s_keep_awake < 0) {
        s_keep_awake = 0;
    }
}

/* ======================================================================== background jobs */

typedef struct {
    ui_bg_fn_t work, done;
    void      *ctx;
    int        depth;          /* page stack when the job started */
    lv_obj_t  *top;
} bg_job_t;

static bg_job_t s_bg_done_job;

static QueueHandle_t s_bg_queue;
static lv_obj_t     *s_busy_ov;
static volatile bool s_bg_busy;

static void busy_hide(void)
{
    if (s_busy_ov) {
        lv_obj_delete(s_busy_ov);
        s_busy_ov = NULL;
    }
}

static void bg_done_async(void *arg)
{
    bg_job_t j = s_bg_done_job;
    busy_hide();
    s_bg_busy = false;
    ui_keep_awake(false);
    lv_obj_t *top = s_depth ? s_pages[s_depth - 1].scr : NULL;
    if (j.depth != s_depth || j.top != top) {
        ui_toast("Done");                 /* the user left that screen: don't touch another one */
        return;
    }
    if (j.done) j.done(j.ctx);
}

static void bg_task(void *arg)
{
    bg_job_t j;
    while (true) {
        if (xQueueReceive(s_bg_queue, &j, portMAX_DELAY) != pdTRUE) continue;
        if (j.work) j.work(j.ctx);              /* no UI lock held: the screen stays live */
        if (ui_lock()) {
            /* run done() on the LVGL task (12 KB stack) - it usually builds a whole page */
            s_bg_done_job = j;
            lv_async_call(bg_done_async, NULL);
            ui_unlock();
        } else {
            s_bg_busy = false;
        }
    }
}

bool ui_run_bg(const char *busy_msg, ui_bg_fn_t work, ui_bg_fn_t done, void *ctx)
{
    if (s_bg_busy) {
        ui_toast("Busy - please wait");
        return false;
    }
    if (!s_bg_queue) {
        s_bg_queue = xQueueCreate(2, sizeof(bg_job_t));
        if (!s_bg_queue || xTaskCreate(bg_task, "ui_bg", 6144, NULL, 3, NULL) != pdPASS) {
            ui_toast("Out of memory");
            return false;
        }
    }
    /* spinner overlay; it also swallows touches until the job is done */
    if (ui_lock()) {
        busy_hide();
        s_busy_ov = lv_obj_create(lv_layer_top());
        lv_obj_remove_style_all(s_busy_ov);
        lv_obj_set_size(s_busy_ov, UI_W, UI_H);
        lv_obj_set_style_bg_color(s_busy_ov, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(s_busy_ov, LV_OPA_60, 0);
        lv_obj_add_flag(s_busy_ov, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_t *sp = lv_spinner_create(s_busy_ov);
        lv_obj_set_size(sp, 70, 70);
        lv_obj_align(sp, LV_ALIGN_CENTER, 0, -24);
        lv_obj_set_style_arc_color(sp, ui_accent(), LV_PART_INDICATOR);
        lv_obj_t *l = ui_label(s_busy_ov, busy_msg ? busy_msg : "Working...", &lv_font_montserrat_20, UI_TEXT);
        lv_obj_align(l, LV_ALIGN_CENTER, 0, 40);
        ui_unlock();
    }
    s_bg_busy = true;
    ui_keep_awake(true);                  /* don't let the screen sleep (and go home) mid-job */
    bg_job_t j = { work, done, ctx, s_depth, s_depth ? s_pages[s_depth - 1].scr : NULL };
    xQueueSend(s_bg_queue, &j, portMAX_DELAY);
    return true;
}

/* runs in LVGL context every 250 ms */
static void tick_cb(lv_timer_t *t)
{
    static int64_t last_refresh, last_tick;
    uint32_t inactive = lv_display_get_inactive_time(NULL);
    int64_t now = esp_timer_get_time();
    /* freeze detector: this timer should run every 250 ms; a big gap means something
     * blocked the UI task - log it so it can be found and moved to ui_run_bg() */
    if (last_tick && now - last_tick > 600000) {
        ESP_LOGW(TAG, "UI was blocked for %lld ms (page depth %d)", (now - last_tick) / 1000 - 250, s_depth);
    }
    last_tick = now;

    if (!s_awake) {
        /* a touch while the screen is off wakes it */
        if (inactive < 400 && now - s_wake_time > 800000) {
            do_wake();
        }
    } else {
        uint16_t to = settings()->screen_timeout_s;
        if (settings()->power_save && (to == 0 || to > 10)) {
            to = 10;
        }
        if (to && !s_keep_awake && inactive > (uint32_t)to * 1000 && !s_call_ov && !s_alarm_ov) {
            do_sleep();
        }
    }

    if (s_dirty || now - last_refresh > 1000000) {
        s_dirty = false;
        last_refresh = now;
        if (s_depth == 0) {
            if (s_awake || s_dimmed_aod) {
                ui_home_refresh();
            }
        } else if (s_pages[s_depth - 1].refresh && s_awake) {
            s_pages[s_depth - 1].refresh(s_pages[s_depth - 1].ctx);
        }
    }
}

void ui_on_data_changed(void)
{
    s_dirty = true;
}

/* ======================================================================== toasts & overlays */

static void delete_obj_timer(lv_timer_t *t)
{
    lv_obj_t *o = lv_timer_get_user_data(t);
    lv_obj_delete(o);
    lv_timer_delete(t);
}

static void show_toast(const char *msg)
{
    lv_obj_t *box = lv_obj_create(lv_layer_top());
    lv_obj_set_width(box, LV_SIZE_CONTENT);
    lv_obj_set_height(box, LV_SIZE_CONTENT);
    lv_obj_set_style_max_width(box, UI_W - 40, 0);
    lv_obj_set_style_bg_color(box, lv_color_hex(0x3A3A3C), 0);
    lv_obj_set_style_border_width(box, 0, 0);
    lv_obj_set_style_radius(box, 22, 0);
    lv_obj_set_style_pad_hor(box, 18, 0);
    lv_obj_set_style_pad_ver(box, 10, 0);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *l = lv_label_create(box);
    lv_label_set_text(l, msg);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_max_width(l, UI_W - 76, 0);
    lv_obj_set_width(l, LV_SIZE_CONTENT);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(l, lv_color_white(), 0);
    lv_obj_align(box, LV_ALIGN_BOTTOM_MID, 0, -36);
    lv_timer_t *t = lv_timer_create(delete_obj_timer, 2500, box);
    lv_timer_set_repeat_count(t, 1);
}

void ui_toast(const char *msg)
{
    if (ui_lock()) {
        show_toast(msg);
        ui_unlock();
    }
}

static void banner_hide(lv_timer_t *t)
{
    if (s_notif_banner) {
        lv_obj_delete(s_notif_banner);
        s_notif_banner = NULL;
    }
    s_banner_timer = NULL;
    lv_timer_delete(t);
}

static void banner_click(lv_event_t *e)
{
    if (s_banner_timer) {
        banner_hide(s_banner_timer);
    }
    ui_open_notifications();
}

void ui_on_notification(const notif_t *n, bool quiet)
{
    if (!ui_lock()) {
        return;
    }
    if (!quiet && settings()->notif_wake) {
        do_wake();
    }
    if (s_awake) {
        if (s_notif_banner) {
            lv_obj_delete(s_notif_banner);
            if (s_banner_timer) {
                lv_timer_delete(s_banner_timer);
            }
        }
        lv_obj_t *b = lv_obj_create(lv_layer_top());
        s_notif_banner = b;
        lv_obj_set_size(b, UI_W - 24, LV_SIZE_CONTENT);
        lv_obj_align(b, LV_ALIGN_TOP_MID, 0, 10);
        lv_obj_set_style_bg_color(b, lv_color_hex(0x2C2C2E), 0);
        lv_obj_set_style_border_width(b, 0, 0);
        lv_obj_set_style_radius(b, 20, 0);
        lv_obj_set_style_pad_all(b, 14, 0);
        lv_obj_set_flex_flow(b, LV_FLEX_FLOW_COLUMN);
        lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(b, banner_click, LV_EVENT_CLICKED, NULL);
        ui_label(b, n->app, &lv_font_montserrat_14, UI_MUTED);
        lv_obj_t *t = ui_label(b, n->title, &lv_font_montserrat_20, UI_TEXT);
        lv_label_set_long_mode(t, LV_LABEL_LONG_DOT);
        lv_obj_set_width(t, lv_pct(100));
        lv_obj_t *body = ui_label(b, n->body, &lv_font_montserrat_16, 0xD1D1D6);
        lv_label_set_long_mode(body, LV_LABEL_LONG_DOT);
        lv_obj_set_size(body, lv_pct(100), LV_SIZE_CONTENT);
        lv_obj_set_style_max_height(body, 60, 0);
        s_banner_timer = lv_timer_create(banner_hide, 5000, NULL);
    }
    s_dirty = true;
    ui_unlock();
}

/* ---- incoming call ---- */

static void call_btn_cb(lv_event_t *e)
{
    const char *action = lv_event_get_user_data(e);
    call_action(action);
}

void ui_on_call(bool incoming)
{
    if (!ui_lock()) {
        return;
    }
    if (s_call_ov) {
        lv_obj_delete(s_call_ov);
        s_call_ov = NULL;
    }
    if (incoming) {
        do_wake();
        call_t c;
        call_get(&c);
        lv_obj_t *o = lv_obj_create(lv_layer_top());
        s_call_ov = o;
        lv_obj_remove_style_all(o);
        lv_obj_set_size(o, UI_W, UI_H);
        lv_obj_set_style_bg_color(o, lv_color_hex(0x0B1A10), 0);
        lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
        lv_obj_add_flag(o, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_t *l = ui_label(o, "Incoming call", &lv_font_montserrat_20, UI_MUTED);
        lv_obj_align(l, LV_ALIGN_TOP_MID, 0, 70);
        l = ui_label(o, c.name, &lv_font_montserrat_36, UI_TEXT);
        lv_label_set_long_mode(l, LV_LABEL_LONG_SCROLL_CIRCULAR);
        lv_obj_set_width(l, UI_W - 40);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(l, LV_ALIGN_TOP_MID, 0, 120);
        l = ui_label(o, c.number, &lv_font_montserrat_20, UI_MUTED);
        lv_obj_align(l, LV_ALIGN_TOP_MID, 0, 175);
        lv_obj_t *rej = ui_round_button(o, LV_SYMBOL_CLOSE, 96, UI_RED, call_btn_cb, "REJECT");
        lv_obj_align(rej, LV_ALIGN_BOTTOM_LEFT, 50, -70);
        lv_obj_t *acc = ui_round_button(o, LV_SYMBOL_CALL, 96, UI_GREEN, call_btn_cb, "ACCEPT");
        lv_obj_align(acc, LV_ALIGN_BOTTOM_RIGHT, -50, -70);
        lv_obj_t *mute = ui_button(o, "Silence", UI_CARD2, call_btn_cb, "IGNORE");
        lv_obj_align(mute, LV_ALIGN_BOTTOM_MID, 0, -20);
    }
    ui_unlock();
}

/* ---- alarm ---- */

extern void alarm_snooze(void);
extern void alarm_dismiss(void);

static void alarm_btn_cb(lv_event_t *e)
{
    bool snooze = lv_event_get_user_data(e) != NULL;
    if (s_alarm_ov) {
        lv_obj_delete(s_alarm_ov);
        s_alarm_ov = NULL;
    }
    if (snooze) {
        alarm_snooze();
    } else {
        alarm_dismiss();
    }
}

void ui_on_alarm(const char *label)
{
    if (!ui_lock()) {
        return;
    }
    do_wake();
    if (s_alarm_ov) {
        lv_obj_delete(s_alarm_ov);
    }
    lv_obj_t *o = lv_obj_create(lv_layer_top());
    s_alarm_ov = o;
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, UI_W, UI_H);
    lv_obj_set_style_bg_color(o, lv_color_hex(0x1A1200), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_add_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *l = ui_label(o, LV_SYMBOL_BELL, &lv_font_montserrat_48, UI_ORANGE);
    lv_obj_align(l, LV_ALIGN_TOP_MID, 0, 60);
    char tbuf[16];
    time_t now = time(NULL);
    struct tm lt;
    localtime_r(&now, &lt);
    snprintf(tbuf, sizeof(tbuf), "%02d:%02d", lt.tm_hour, lt.tm_min);
    l = ui_label(o, tbuf, &lv_font_montserrat_48, UI_TEXT);
    lv_obj_set_style_transform_scale(l, 400, 0);
    lv_obj_align(l, LV_ALIGN_CENTER, 0, -50);
    l = ui_label(o, label && *label ? label : "Alarm", &lv_font_montserrat_24, UI_MUTED);
    lv_obj_align(l, LV_ALIGN_CENTER, 0, 30);
    lv_obj_t *sn = ui_button(o, "Snooze 5 min", UI_CARD2, alarm_btn_cb, (void *)1);
    lv_obj_set_width(sn, 300);
    lv_obj_align(sn, LV_ALIGN_BOTTOM_MID, 0, -110);
    lv_obj_t *d = ui_button(o, "Dismiss", UI_ORANGE, alarm_btn_cb, NULL);
    lv_obj_set_width(d, 300);
    lv_obj_align(d, LV_ALIGN_BOTTOM_MID, 0, -40);
    ui_unlock();
}

void ui_on_timer_done(void)
{
    if (ui_lock()) {
        do_wake();
        show_toast("Timer finished");
        ui_unlock();
    }
}

/* ======================================================================== remote input */

typedef struct {
    int16_t x, y;
    bool    pressed;
    uint8_t hold;        /* number of read cycles to repeat this point */
} inj_point_t;

static QueueHandle_t s_inj_q;

static void inj_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    static inj_point_t cur = { 0 };
    static int remaining;
    if (remaining <= 0) {
        inj_point_t p;
        if (xQueueReceive(s_inj_q, &p, 0) == pdTRUE) {
            cur = p;
            remaining = p.hold ? p.hold : 1;
        } else {
            cur.pressed = false;
        }
    }
    if (remaining > 0) {
        remaining--;
    }
    data->point.x = cur.x;
    data->point.y = cur.y;
    data->state = cur.pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    data->continue_reading = false;
    /* nothing queued and released: stop polling (saves 30 wake-ups/s, even with screen off) */
    if (remaining <= 0 && !cur.pressed && uxQueueMessagesWaiting(s_inj_q) == 0) {
        lv_timer_pause(lv_indev_get_read_timer(indev));
    }
}

static lv_indev_t *s_inj;

static void inj_push(int x, int y, bool pressed, int hold)
{
    inj_point_t p = { .x = x, .y = y, .pressed = pressed, .hold = hold };
    xQueueSend(s_inj_q, &p, pdMS_TO_TICKS(100));
    if (s_inj && ui_lock()) {
        lv_timer_resume(lv_indev_get_read_timer(s_inj));
        ui_unlock();
    }
}

void ui_inject_tap(int x, int y)
{
    ui_wake();
    inj_push(x, y, true, 3);
    inj_push(x, y, false, 2);
}

void ui_inject_long_press(int x, int y)
{
    ui_wake();
    inj_push(x, y, true, 40);
    inj_push(x, y, false, 2);
}

void ui_inject_swipe(int x1, int y1, int x2, int y2)
{
    ui_wake();
    const int steps = 8;
    for (int i = 0; i <= steps; i++) {
        inj_push(x1 + (x2 - x1) * i / steps, y1 + (y2 - y1) * i / steps, true, 1);
    }
    inj_push(x2, y2, false, 2);
}

void ui_inject_key(const char *key)
{
    if (strcmp(key, "power") == 0) {
        if (s_awake) {
            ui_sleep();
        } else {
            ui_wake();
        }
        return;
    }
    if (strcmp(key, "wake") == 0) {
        ui_wake();
        return;
    }
    if (!ui_lock()) {
        return;
    }
    do_wake();
    if (strcmp(key, "back") == 0) {
        ui_page_pop();
    } else if (strcmp(key, "home") == 0) {
        ui_go_home();
    } else if (strcmp(key, "recents") == 0) {
        ui_go_home();
        ui_open_recents();
    }
    ui_unlock();
}

/* ======================================================================== screenshot */

uint8_t *ui_screenshot_bmp(size_t *out_len)
{
    *out_len = 0;
    if (!ui_lock()) {
        return NULL;
    }
    /* include overlays (calls, alarms, banners) by snapshotting the top layer onto the screen */
    lv_draw_buf_t *snap = lv_snapshot_take(lv_screen_active(), LV_COLOR_FORMAT_RGB565);
    lv_draw_buf_t *top = NULL;
    if (lv_obj_get_child_count(lv_layer_top()) > 0) {
        top = lv_snapshot_take(lv_layer_top(), LV_COLOR_FORMAT_ARGB8888);
    }
    ui_unlock();
    if (!snap) {
        return NULL;
    }
    int w = snap->header.w, h = snap->header.h;
    size_t row = w * 2;
    size_t row_pad = (row + 3) & ~3u;
    size_t hdr = 14 + 40 + 12;
    size_t total = hdr + row_pad * h;
    uint8_t *bmp = malloc(total);
    if (!bmp) {
        lv_draw_buf_destroy(snap);
        if (top) lv_draw_buf_destroy(top);
        return NULL;
    }
    memset(bmp, 0, hdr);
    bmp[0] = 'B'; bmp[1] = 'M';
    *(uint32_t *)(bmp + 2) = total;
    *(uint32_t *)(bmp + 10) = hdr;
    *(uint32_t *)(bmp + 14) = 40;
    *(int32_t *)(bmp + 18) = w;
    *(int32_t *)(bmp + 22) = -h;               /* top-down */
    *(uint16_t *)(bmp + 26) = 1;
    *(uint16_t *)(bmp + 28) = 16;
    *(uint32_t *)(bmp + 30) = 3;               /* BI_BITFIELDS */
    *(uint32_t *)(bmp + 34) = row_pad * h;
    *(uint32_t *)(bmp + 54) = 0xF800;
    *(uint32_t *)(bmp + 58) = 0x07E0;
    *(uint32_t *)(bmp + 62) = 0x001F;
    for (int y = 0; y < h; y++) {
        uint16_t *dst = (uint16_t *)(bmp + hdr + y * row_pad);
        const uint16_t *src = (const uint16_t *)(snap->data + y * snap->header.stride);
        memcpy(dst, src, row);
        if (top && y < (int)top->header.h) {
            const uint8_t *tp = top->data + y * top->header.stride;
            for (int x = 0; x < w && x < (int)top->header.w; x++) {
                uint8_t b = tp[x * 4], g = tp[x * 4 + 1], r = tp[x * 4 + 2], a = tp[x * 4 + 3];
                if (!a) continue;
                uint16_t d = dst[x];
                int dr = (d >> 8) & 0xF8, dg = (d >> 3) & 0xFC, db = (d << 3) & 0xF8;
                dr = (r * a + dr * (255 - a)) / 255;
                dg = (g * a + dg * (255 - a)) / 255;
                db = (b * a + db * (255 - a)) / 255;
                dst[x] = ((dr & 0xF8) << 8) | ((dg & 0xFC) << 3) | (db >> 3);
            }
        }
    }
    lv_draw_buf_destroy(snap);
    if (top) lv_draw_buf_destroy(top);
    *out_len = total;
    return bmp;
}

/* ======================================================================== buttons */

static void on_button(button_id_t id, button_event_t ev)
{
    if (id == BTN_PWR && ev == BTN_SHORT) {
        if (s_awake) {
            ui_sleep();
        } else {
            ui_wake();
        }
        return;
    }
    if (!ui_lock()) {
        return;
    }
    bool was_asleep = !s_awake;
    do_wake();
    if (!was_asleep) {
        if (id == BTN_PWR && ev == BTN_LONG) {
            ui_open_power_menu();
        } else if (id == BTN_BOOT && s_bg_busy) {
            /* a background job is running: keep the screen it will report back to */
        } else if (id == BTN_BOOT && ev == BTN_SHORT) {
            if (s_depth > 0) {
                ui_page_pop();
            } else {
                ui_open_launcher();
            }
        } else if (id == BTN_BOOT && ev == BTN_LONG) {
            if (s_depth > 0) {
                ui_go_home();
            } else {
                ui_open_recents();          /* on the watch face: recent apps */
            }
        }
    }
    ui_unlock();
}

static void on_raise(void)
{
    if (settings()->raise_to_wake && !s_awake) {
        ui_wake();
    }
}

static void on_settings(void)
{
    if (s_awake && ui_lock()) {
        apply_brightness();
        s_dirty = true;
        ui_unlock();
    }
}

/* ======================================================================== touch haptics */

static void touch_haptic_cb(lv_event_t *e)
{
    if (settings()->haptic_touch && settings()->vibration && s_awake) {
        haptic_pulse(12);
    }
}

/* ======================================================================== start */

static void home_gesture_cb(lv_event_t *e)
{
    lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_active());
    lv_indev_wait_release(lv_indev_active());
    switch (dir) {
    case LV_DIR_TOP:    ui_open_launcher(); break;
    case LV_DIR_BOTTOM: ui_open_quick_settings(); break;
    case LV_DIR_LEFT:   ui_open_notifications(); break;
    case LV_DIR_RIGHT:  ui_open_widgets(); break;
    default: break;
    }
}

static void home_long_press_cb(lv_event_t *e)
{
    ui_open_face_picker();
}

void ui_start(void)
{
    s_inj_q = xQueueCreate(32, sizeof(inj_point_t));
    if (esp_pm_lock_create(ESP_PM_CPU_FREQ_MAX, 0, "ui", &s_cpu_lock) == ESP_OK) {
        cpu_fast(true);
    }
    if (!ui_lock()) {
        return;
    }
    lv_theme_t *th = lv_theme_default_init(NULL, ui_accent(), lv_color_hex(UI_RED), true,
                                           &lv_font_montserrat_16);
    lv_display_set_theme(NULL, th);
    lv_sysmon_hide_performance(NULL);     /* FPS/CPU overlay: Settings > Developer */

    s_home = new_screen();
    lv_obj_add_flag(s_home, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_home, home_gesture_cb, LV_EVENT_GESTURE, NULL);
    lv_obj_add_event_cb(s_home, home_long_press_cb, LV_EVENT_LONG_PRESSED, NULL);
    ui_home_build(s_home);
    lv_screen_load(s_home);

    /* short buzz on every touch (Settings > Sound > Touch feedback) */
    for (lv_indev_t *in = lv_indev_get_next(NULL); in; in = lv_indev_get_next(in)) {
        if (lv_indev_get_type(in) == LV_INDEV_TYPE_POINTER) {
            lv_indev_add_event_cb(in, touch_haptic_cb, LV_EVENT_PRESSED, NULL);
        }
    }

    lv_indev_t *inj = lv_indev_create();
    lv_indev_set_type(inj, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(inj, inj_read_cb);
    s_inj = inj;
    lv_timer_pause(lv_indev_get_read_timer(inj));

    lv_timer_create(tick_cb, 250, NULL);
    apply_brightness();
    s_wake_time = esp_timer_get_time();
    ui_unlock();

    buttons_set_cb(on_button);
    imu_set_raise_cb(on_raise);
    settings_on_change(on_settings);
}
