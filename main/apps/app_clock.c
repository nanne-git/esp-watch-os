/*
 * Alarms, countdown timer and stopwatch.
 */
#include <stdio.h>
#include <string.h>

#include "alarm.h"
#include "apps.h"
#include "ui.h"

/* ======================================================================== alarms */

static alarm_t s_edit;
static int     s_edit_idx;        /* -1 = new */

static void alarm_list_build(lv_obj_t *c, void *ctx);
static void alarm_edit_build(lv_obj_t *c, void *ctx);

static void alarm_toggle(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    alarm_t a;
    if (alarm_get(i, &a)) {
        a.enabled = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
        alarm_set(i, &a);
    }
}

static void alarm_open(lv_event_t *e)
{
    s_edit_idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (s_edit_idx >= 0) {
        alarm_get(s_edit_idx, &s_edit);
    } else {
        memset(&s_edit, 0, sizeof(s_edit));
        s_edit.enabled = true;
        s_edit.hour = 7;
        s_edit.days = 0x3E;      /* weekdays */
    }
    ui_page_push(s_edit_idx >= 0 ? "Edit alarm" : "New alarm", alarm_edit_build, NULL, NULL);
}

static void alarm_list_build(lv_obj_t *c, void *ctx)
{
    int n = alarm_count();
    for (int i = 0; i < n; i++) {
        alarm_t a;
        alarm_get(i, &a);
        lv_obj_t *card = ui_card(c);
        lv_obj_set_height(card, 96);
        lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_color(card, lv_color_hex(UI_CARD2), LV_STATE_PRESSED);
        lv_obj_add_event_cb(card, alarm_open, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        char t[8], days[40];
        snprintf(t, sizeof(t), "%02d:%02d", a.hour, a.min);
        lv_obj_t *tl = ui_label(card, t, &lv_font_montserrat_36, a.enabled ? UI_TEXT : UI_MUTED);
        lv_obj_align(tl, LV_ALIGN_TOP_LEFT, 0, -4);
        alarm_days_str(a.days, days, sizeof(days));
        char sub[64];
        snprintf(sub, sizeof(sub), "%s%s%s", a.label, a.label[0] ? "  -  " : "", days);
        lv_obj_t *dl = ui_label(card, sub, &lv_font_montserrat_16, UI_MUTED);
        lv_obj_align(dl, LV_ALIGN_BOTTOM_LEFT, 0, 4);
        lv_obj_t *sw = lv_switch_create(card);
        lv_obj_set_size(sw, 58, 32);
        lv_obj_align(sw, LV_ALIGN_RIGHT_MID, 0, 0);
        lv_obj_set_style_bg_color(sw, lv_color_hex(UI_ORANGE), LV_PART_INDICATOR | LV_STATE_CHECKED);
        if (a.enabled) lv_obj_add_state(sw, LV_STATE_CHECKED);
        lv_obj_add_event_cb(sw, alarm_toggle, LV_EVENT_VALUE_CHANGED, (void *)(intptr_t)i);
    }
    if (n == 0) {
        lv_obj_t *l = ui_label(c, "No alarms yet", &lv_font_montserrat_20, UI_MUTED);
        lv_obj_set_style_pad_top(l, 40, 0);
        lv_obj_set_width(l, lv_pct(100));
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    }
    if (n < ALARM_MAX) {
        lv_obj_t *b = ui_button(c, LV_SYMBOL_PLUS "  Add alarm", UI_ORANGE, alarm_open, (void *)-1);
        lv_obj_set_width(b, lv_pct(100));
    }
}

void app_alarms_open(void)
{
    ui_page_push("Alarms", alarm_list_build, NULL, NULL);
}

/* ---- edit ---- */

static void edit_min(int v, void *u) { s_edit.min = v; ui_page_rebuild(); }
static void edit_hour(int v, void *u) { s_edit.hour = v; ui_number_picker("Minute", 0, 59, s_edit.min, edit_min, NULL); }
static void time_click(lv_event_t *e) { ui_number_picker("Hour", 0, 23, s_edit.hour, edit_hour, NULL); }

static void day_click(lv_event_t *e)
{
    int d = (int)(intptr_t)lv_event_get_user_data(e);
    s_edit.days ^= (1 << d);
    lv_obj_t *b = lv_event_get_current_target(e);
    lv_obj_set_style_bg_color(b, lv_color_hex(s_edit.days & (1 << d) ? UI_ORANGE : UI_CARD2), 0);
}

static void label_done(const char *t, void *u)
{
    strlcpy(s_edit.label, t, sizeof(s_edit.label));
    ui_page_rebuild();
}
static void label_click(lv_event_t *e) { ui_keyboard("Alarm label", s_edit.label, false, label_done, NULL); }

static void save_click(lv_event_t *e)
{
    s_edit.enabled = true;
    if (s_edit_idx >= 0) {
        alarm_set(s_edit_idx, &s_edit);
    } else {
        alarm_add(&s_edit);
    }
    ui_page_pop();
    ui_page_rebuild();
    char nx[24];
    if (alarm_next(nx, sizeof(nx))) {
        char m[48];
        snprintf(m, sizeof(m), "Next alarm: %s", nx);
        ui_toast(m);
    }
}

static void do_delete(void *u)
{
    alarm_delete(s_edit_idx);
    ui_page_pop();
    ui_page_rebuild();
}
static void delete_click(lv_event_t *e) { ui_confirm("Delete alarm", "Delete this alarm?", "Delete", UI_RED, do_delete, NULL); }

static void alarm_edit_build(lv_obj_t *c, void *ctx)
{
    char t[8];
    snprintf(t, sizeof(t), "%02d:%02d", s_edit.hour, s_edit.min);
    lv_obj_t *tb = lv_button_create(c);
    lv_obj_set_size(tb, lv_pct(100), 110);
    lv_obj_set_style_bg_color(tb, lv_color_hex(UI_CARD), 0);
    lv_obj_set_style_radius(tb, 20, 0);
    lv_obj_set_style_shadow_width(tb, 0, 0);
    lv_obj_add_event_cb(tb, time_click, LV_EVENT_CLICKED, NULL);
    lv_obj_t *tl = ui_label(tb, t, &lv_font_montserrat_48, UI_TEXT);
    lv_obj_set_style_transform_scale(tl, 340, 0);
    lv_obj_set_style_transform_pivot_x(tl, lv_pct(50), 0);
    lv_obj_set_style_transform_pivot_y(tl, lv_pct(50), 0);
    lv_obj_center(tl);

    ui_section(c, "REPEAT");
    lv_obj_t *row = lv_obj_create(c);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, lv_pct(100), 52);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    static const char *dn[] = { "S", "M", "T", "W", "T", "F", "S" };
    for (int d = 0; d < 7; d++) {
        lv_obj_t *b = lv_button_create(row);
        lv_obj_set_size(b, 48, 48);
        lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_shadow_width(b, 0, 0);
        lv_obj_set_style_bg_color(b, lv_color_hex(s_edit.days & (1 << d) ? UI_ORANGE : UI_CARD2), 0);
        lv_obj_add_event_cb(b, day_click, LV_EVENT_CLICKED, (void *)(intptr_t)d);
        lv_obj_t *l = ui_label(b, dn[d], &lv_font_montserrat_18, UI_TEXT);
        lv_obj_center(l);
    }
    ui_label(c, "No days selected = rings once", &lv_font_montserrat_14, UI_MUTED);
    ui_row(c, LV_SYMBOL_EDIT, UI_CARD2, "Label", s_edit.label[0] ? s_edit.label : "Alarm", label_click, NULL);
    lv_obj_t *s = ui_button(c, "Save", UI_ORANGE, save_click, NULL);
    lv_obj_set_width(s, lv_pct(100));
    if (s_edit_idx >= 0) {
        lv_obj_t *d = ui_button(c, "Delete", UI_CARD2, delete_click, NULL);
        lv_obj_set_width(d, lv_pct(100));
    }
}

/* ======================================================================== timer */

static lv_obj_t *s_timer_lbl, *s_timer_arc, *s_timer_btn_lbl;
static int s_timer_total = 300;
static int s_set_h, s_set_m;

static void timer_refresh(void *ctx)
{
    if (!s_timer_lbl) return;
    int left = timer_running() ? timer_remaining() : s_timer_total;
    char b[16];
    if (left >= 3600) snprintf(b, sizeof(b), "%d:%02d:%02d", left / 3600, (left / 60) % 60, left % 60);
    else snprintf(b, sizeof(b), "%02d:%02d", left / 60, left % 60);
    lv_label_set_text(s_timer_lbl, b);
    lv_arc_set_value(s_timer_arc, s_timer_total ? left * 1000 / s_timer_total : 0);
    lv_label_set_text(s_timer_btn_lbl, !timer_running() ? LV_SYMBOL_PLAY : timer_paused() ? LV_SYMBOL_PLAY : LV_SYMBOL_PAUSE);
}

static void timer_go(lv_event_t *e)
{
    if (!timer_running()) {
        if (s_timer_total > 0) timer_start(s_timer_total);
    } else if (timer_paused()) {
        timer_resume();
    } else {
        timer_pause();
    }
    timer_refresh(NULL);
}

static void timer_reset(lv_event_t *e) { timer_cancel(); timer_refresh(NULL); }

static void preset(lv_event_t *e)
{
    if (timer_running()) return;
    s_timer_total = (int)(intptr_t)lv_event_get_user_data(e);
    timer_refresh(NULL);
}

static void set_m(int v, void *u) { s_timer_total = s_set_h * 3600 + v * 60; timer_refresh(NULL); }
static void set_h(int v, void *u) { s_set_h = v; ui_number_picker("Minutes", 0, 59, s_set_m, set_m, NULL); }
static void custom_click(lv_event_t *e) { if (!timer_running()) ui_number_picker("Hours", 0, 23, 0, set_h, NULL); }

static void timer_build(lv_obj_t *c, void *ctx)
{
    lv_obj_t *arc = lv_arc_create(c);
    s_timer_arc = arc;
    lv_obj_set_size(arc, 250, 250);
    lv_obj_align(arc, LV_ALIGN_TOP_MID, 0, 6);
    lv_arc_set_bg_angles(arc, 0, 360);
    lv_arc_set_rotation(arc, 270);
    lv_arc_set_range(arc, 0, 1000);
    lv_obj_remove_style(arc, NULL, LV_PART_KNOB);
    lv_obj_remove_flag(arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(arc, 12, LV_PART_MAIN);
    lv_obj_set_style_arc_width(arc, 12, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(arc, lv_color_hex(0x262629), LV_PART_MAIN);
    lv_obj_set_style_arc_color(arc, lv_color_hex(UI_TEAL), LV_PART_INDICATOR);
    s_timer_lbl = ui_label(arc, "", &lv_font_montserrat_48, UI_TEXT);
    lv_obj_center(s_timer_lbl);
    lv_obj_add_flag(arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(arc, custom_click, LV_EVENT_CLICKED, NULL);

    lv_obj_t *row = lv_obj_create(c);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, UI_W - 30, 48);
    lv_obj_align(row, LV_ALIGN_TOP_MID, 0, 266);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    static const struct { const char *n; int s; } pr[] = { { "1m", 60 }, { "3m", 180 }, { "5m", 300 }, { "10m", 600 }, { "25m", 1500 } };
    for (int i = 0; i < 5; i++) {
        lv_obj_t *b = ui_button(row, pr[i].n, UI_CARD2, preset, (void *)(intptr_t)pr[i].s);
        lv_obj_set_size(b, 66, 44);
        lv_obj_set_style_pad_hor(b, 0, 0);
    }
    lv_obj_t *rst = ui_round_button(c, LV_SYMBOL_STOP, 76, UI_CARD2, timer_reset, NULL);
    lv_obj_align(rst, LV_ALIGN_BOTTOM_LEFT, 70, -20);
    lv_obj_t *go = ui_round_button(c, LV_SYMBOL_PLAY, 76, UI_TEAL, timer_go, NULL);
    s_timer_btn_lbl = lv_obj_get_child(go, 0);
    lv_obj_align(go, LV_ALIGN_BOTTOM_RIGHT, -70, -20);
    timer_refresh(NULL);
    ui_page_set_refresh(timer_refresh);
}

static void timer_close(void *ctx) { s_timer_lbl = NULL; }
void app_timer_open(void) { ui_page_push_raw("Timer", timer_build, timer_close, NULL); }

/* ======================================================================== stopwatch */

static lv_obj_t *s_sw_lbl, *s_sw_laps, *s_sw_go_lbl, *s_sw_lap_lbl;
static lv_timer_t *s_sw_timer;

static void fmt_ms(int64_t ms, char *b, size_t n)
{
    snprintf(b, n, "%02d:%02d.%02d", (int)(ms / 60000), (int)(ms / 1000) % 60, (int)(ms / 10) % 100);
}

static void sw_update(lv_timer_t *t)
{
    if (!s_sw_lbl) return;
    char b[24];
    fmt_ms(sw_elapsed_ms(), b, sizeof(b));
    lv_label_set_text(s_sw_lbl, b);
}

static void sw_laps_update(void)
{
    int64_t laps[20];
    int n = sw_laps(laps, 20);
    char buf[512] = "";
    for (int i = n - 1; i >= 0 && i >= n - 8; i--) {
        char l[48], t[24], d[24];
        fmt_ms(laps[i], t, sizeof(t));
        fmt_ms(laps[i] - (i ? laps[i - 1] : 0), d, sizeof(d));
        snprintf(l, sizeof(l), "Lap %d    %s    +%s\n", i + 1, t, d);
        strlcat(buf, l, sizeof(buf));
    }
    lv_label_set_text(s_sw_laps, buf);
    lv_label_set_text(s_sw_go_lbl, sw_running() ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
    lv_label_set_text(s_sw_lap_lbl, sw_running() ? "Lap" : "Reset");
}

static void sw_go(lv_event_t *e)
{
    if (sw_running()) sw_stop(); else sw_start();
    sw_laps_update();
}

static void sw_lap_reset(lv_event_t *e)
{
    if (sw_running()) sw_lap(); else sw_reset();
    sw_laps_update();
    sw_update(NULL);
}

static void sw_build(lv_obj_t *c, void *ctx)
{
    s_sw_lbl = ui_label(c, "00:00.00", &lv_font_montserrat_48, UI_TEXT);
    lv_obj_set_style_transform_scale(s_sw_lbl, 330, 0);
    lv_obj_set_style_transform_pivot_x(s_sw_lbl, lv_pct(50), 0);
    lv_obj_set_style_transform_pivot_y(s_sw_lbl, lv_pct(50), 0);
    lv_obj_align(s_sw_lbl, LV_ALIGN_TOP_MID, 0, 40);
    s_sw_laps = ui_label(c, "", &lv_font_montserrat_16, UI_MUTED);
    lv_obj_align(s_sw_laps, LV_ALIGN_TOP_MID, 0, 140);
    lv_obj_t *lap = ui_round_button(c, "", 80, UI_CARD2, sw_lap_reset, NULL);
    s_sw_lap_lbl = lv_obj_get_child(lap, 0);
    lv_obj_set_style_text_font(s_sw_lap_lbl, &lv_font_montserrat_18, 0);
    lv_obj_align(lap, LV_ALIGN_BOTTOM_LEFT, 70, -20);
    lv_obj_t *go = ui_round_button(c, LV_SYMBOL_PLAY, 80, UI_GREEN, sw_go, NULL);
    s_sw_go_lbl = lv_obj_get_child(go, 0);
    lv_obj_align(go, LV_ALIGN_BOTTOM_RIGHT, -70, -20);
    s_sw_timer = lv_timer_create(sw_update, 50, NULL);
    sw_update(NULL);
    sw_laps_update();
    ui_keep_awake(true);
}

static void sw_close(void *ctx)
{
    s_sw_lbl = NULL;
    if (s_sw_timer) {
        lv_timer_delete(s_sw_timer);
        s_sw_timer = NULL;
    }
    ui_keep_awake(false);
}

void app_stopwatch_open(void) { ui_page_push_raw("Stopwatch", sw_build, sw_close, NULL); }
