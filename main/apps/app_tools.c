/*
 * Activity (steps), Weather, Flashlight, Find phone, Calculator.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "apps.h"
#include "bsp/esp-bsp.h"
#include "hw.h"
#include "phone.h"
#include "settings.h"
#include "ui.h"
#include "weather.h"

/* ======================================================================== activity */

static lv_obj_t *s_act_arc, *s_act_steps, *s_act_detail;

static void act_refresh(void *ctx)
{
    if (!s_act_arc) return;
    uint32_t st = imu_steps_today();
    uint16_t goal = settings()->step_goal ? settings()->step_goal : 8000;
    lv_arc_set_value(s_act_arc, st >= goal ? 1000 : st * 1000 / goal);
    char b[96];
    snprintf(b, sizeof(b), "%lu", (unsigned long)st);
    lv_label_set_text(s_act_steps, b);
    float km = st * 0.75f / 1000.0f;            /* ~75 cm per step */
    int kcal = (int)(st * 0.04f);
    snprintf(b, sizeof(b), "Goal %u   -   %.2f km   -   %d kcal", goal, km, kcal);
    lv_label_set_text(s_act_detail, b);
}

static void act_build(lv_obj_t *c, void *ctx)
{
    lv_obj_t *arc = lv_arc_create(c);
    s_act_arc = arc;
    lv_obj_set_size(arc, 280, 280);
    lv_obj_align(arc, LV_ALIGN_TOP_MID, 0, 10);
    lv_arc_set_bg_angles(arc, 135, 45);
    lv_arc_set_range(arc, 0, 1000);
    lv_obj_remove_style(arc, NULL, LV_PART_KNOB);
    lv_obj_remove_flag(arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(arc, 22, LV_PART_MAIN);
    lv_obj_set_style_arc_width(arc, 22, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(arc, lv_color_hex(0x1E3A26), LV_PART_MAIN);
    lv_obj_set_style_arc_color(arc, lv_color_hex(UI_GREEN), LV_PART_INDICATOR);
    s_act_steps = ui_label(arc, "0", &lv_font_montserrat_48, UI_TEXT);
    lv_obj_align(s_act_steps, LV_ALIGN_CENTER, 0, -10);
    lv_obj_t *l = ui_label(arc, "steps today", &lv_font_montserrat_18, UI_MUTED);
    lv_obj_align(l, LV_ALIGN_CENTER, 0, 36);
    s_act_detail = ui_label(c, "", &lv_font_montserrat_16, UI_MUTED);
    lv_obj_align(s_act_detail, LV_ALIGN_TOP_MID, 0, 310);
    if (!imu_ok()) {
        lv_obj_t *w = ui_label(c, "Motion sensor not found", &lv_font_montserrat_16, UI_RED);
        lv_obj_align(w, LV_ALIGN_TOP_MID, 0, 340);
    }
    act_refresh(NULL);
    ui_page_set_refresh(act_refresh);
}

static void act_close(void *ctx) { s_act_arc = NULL; }
void app_steps_open(void) { ui_page_push_raw("Activity", act_build, act_close, NULL); }

/* ======================================================================== weather */

static const char *weather_symbol(int code)
{
    if (code == 0) return LV_SYMBOL_EYE_OPEN;          /* clear */
    if (code >= 51 && code <= 82) return LV_SYMBOL_TINT;
    if (code >= 95) return LV_SYMBOL_CHARGE;
    return LV_SYMBOL_IMAGE;
}

static void wx_refresh_click(lv_event_t *e) { weather_refresh(); ui_toast("Updating..."); }

static void wx_build(lv_obj_t *c, void *ctx)
{
    weather_t w;
    weather_get(&w);
    lv_obj_t *card = ui_card(c);
    lv_obj_set_height(card, 170);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x0F2A3A), 0);
    char b[64];
    ui_label(card, settings()->city, &lv_font_montserrat_20, UI_TEXT);
    if (w.valid) {
        snprintf(b, sizeof(b), "%.0f°", w.temp_c);
        lv_obj_t *t = ui_label(card, b, &lv_font_montserrat_48, UI_TEXT);
        lv_obj_align(t, LV_ALIGN_LEFT_MID, 0, 10);
        lv_obj_t *ic = ui_label(card, weather_symbol(w.code), &lv_font_montserrat_48, UI_TEAL);
        lv_obj_align(ic, LV_ALIGN_RIGHT_MID, -10, 0);
        snprintf(b, sizeof(b), "%s   H %.0f°  L %.0f°", weather_desc(w.code), w.tmax, w.tmin);
        lv_obj_t *d = ui_label(card, b, &lv_font_montserrat_16, 0xC7E8F5);
        lv_obj_align(d, LV_ALIGN_BOTTOM_LEFT, 0, 0);
        snprintf(b, sizeof(b), "%.0f°C", w.feels_c);
        ui_row(c, NULL, 0, "Feels like", b, NULL, NULL);
        snprintf(b, sizeof(b), "%d %%", w.humidity);
        ui_row(c, NULL, 0, "Humidity", b, NULL, NULL);
        snprintf(b, sizeof(b), "%.0f km/h", w.wind_kmh);
        ui_row(c, NULL, 0, "Wind", b, NULL, NULL);
        ui_section(c, "NEXT DAYS");
        static const char *wd[] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
        time_t now = time(NULL);
        struct tm lt;
        localtime_r(&now, &lt);
        for (int i = 1; i < 5; i++) {
            snprintf(b, sizeof(b), "%.0f° / %.0f°", w.daily_max[i], w.daily_min[i]);
            char name[32];
            snprintf(name, sizeof(name), "%s  %s", wd[(lt.tm_wday + i) % 7], weather_desc(w.daily_code[i]));
            ui_row(c, weather_symbol(w.daily_code[i]), UI_TEAL, name, b, NULL, NULL);
        }
    } else {
        lv_obj_t *t = ui_label(card, "No weather yet.\nConnect to Wi-Fi - it updates every 30 min.",
                               &lv_font_montserrat_16, 0xC7E8F5);
        lv_obj_align(t, LV_ALIGN_LEFT_MID, 0, 10);
    }
    ui_row(c, LV_SYMBOL_REFRESH, UI_BLUE, "Update now", NULL, wx_refresh_click, NULL);
    ui_label(c, "Set your city in Settings > Weather location", &lv_font_montserrat_14, UI_MUTED);
}

void app_weather_open(void) { ui_page_push("Weather", wx_build, NULL, NULL); }

/* ======================================================================== flashlight */

static lv_obj_t *s_fl_scr;

static void fl_close_click(lv_event_t *e)
{
    ui_keep_awake(false);
    bsp_display_brightness_set(settings()->brightness);
    lv_obj_delete(s_fl_scr);
    s_fl_scr = NULL;
}

void app_flashlight_open(void)
{
    if (s_fl_scr) return;
    s_fl_scr = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_fl_scr);
    lv_obj_set_size(s_fl_scr, UI_W, UI_H);
    lv_obj_set_style_bg_color(s_fl_scr, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(s_fl_scr, LV_OPA_COVER, 0);
    lv_obj_add_flag(s_fl_scr, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_fl_scr, fl_close_click, LV_EVENT_CLICKED, NULL);
    lv_obj_t *l = ui_label(s_fl_scr, "Tap to turn off", &lv_font_montserrat_16, 0x8E8E93);
    lv_obj_align(l, LV_ALIGN_BOTTOM_MID, 0, -20);
    bsp_display_brightness_set(100);
    ui_keep_awake(true);
}

/* ======================================================================== find phone */

static bool s_ringing;

static void find_click(lv_event_t *e)
{
    if (!phone_connected()) {
        ui_toast("Phone not connected (Gadgetbridge)");
        return;
    }
    s_ringing = !s_ringing;
    phone_find(s_ringing);
    lv_obj_t *b = lv_event_get_current_target(e);
    lv_label_set_text(lv_obj_get_child(b, 0), s_ringing ? "Stop" : "Ring phone");
    lv_obj_set_style_bg_color(b, lv_color_hex(s_ringing ? UI_RED : UI_PURPLE), 0);
}

static void find_build(lv_obj_t *c, void *ctx)
{
    lv_obj_t *ic = ui_label(c, LV_SYMBOL_CALL, &lv_font_montserrat_48, UI_PURPLE);
    lv_obj_align(ic, LV_ALIGN_TOP_MID, 0, 50);
    lv_obj_t *t = ui_label(c, phone_connected() ? "Make your phone ring, even on silent"
                                                : "Connect your phone with Gadgetbridge first",
                           &lv_font_montserrat_18, UI_MUTED);
    lv_label_set_long_mode(t, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(t, UI_W - 60);
    lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 140);
    lv_obj_t *b = ui_button(c, s_ringing ? "Stop" : "Ring phone", s_ringing ? UI_RED : UI_PURPLE, find_click, NULL);
    lv_obj_set_width(b, 260);
    lv_obj_align(b, LV_ALIGN_BOTTOM_MID, 0, -60);
}

static void find_close(void *ctx)
{
    if (s_ringing) {
        phone_find(false);
        s_ringing = false;
    }
}

void app_findphone_open(void) { ui_page_push_raw("Find phone", find_build, find_close, NULL); }

/* ======================================================================== calculator */

static lv_obj_t *s_calc_disp, *s_calc_hist;
static double s_acc, s_cur;
static char s_op;
static bool s_new_entry = true, s_has_dot;
static char s_entry[24] = "0";

static void calc_show(void)
{
    lv_label_set_text(s_calc_disp, s_entry);
}

static void set_entry_num(double v)
{
    if (fabs(v) > 1e12 || (fabs(v) < 1e-9 && v != 0)) {
        snprintf(s_entry, sizeof(s_entry), "%.6g", v);
    } else {
        snprintf(s_entry, sizeof(s_entry), "%.10g", v);
    }
}

static double apply_op(double a, double b, char op)
{
    switch (op) {
    case '+': return a + b;
    case '-': return a - b;
    case 'x': return a * b;
    case '/': return b == 0 ? NAN : a / b;
    default: return b;
    }
}

static void calc_key(lv_event_t *e)
{
    lv_obj_t *bm = lv_event_get_target(e);
    const char *k = lv_buttonmatrix_get_button_text(bm, lv_buttonmatrix_get_selected_button(bm));
    if (!k) return;
    if (k[0] >= '0' && k[0] <= '9') {
        if (s_new_entry) { strcpy(s_entry, "0"); s_new_entry = false; s_has_dot = false; }
        if (strcmp(s_entry, "0") == 0) s_entry[0] = '\0';
        if (strlen(s_entry) < 14) strncat(s_entry, k, 1);
    } else if (k[0] == '.') {
        if (s_new_entry) { strcpy(s_entry, "0"); s_new_entry = false; }
        if (!s_has_dot) { strcat(s_entry, "."); s_has_dot = true; }
    } else if (k[0] == 'C') {
        strcpy(s_entry, "0");
        s_acc = 0; s_op = 0; s_new_entry = true; s_has_dot = false;
        lv_label_set_text(s_calc_hist, "");
    } else if (strcmp(k, "+/-") == 0) {
        set_entry_num(-atof(s_entry));
    } else if (k[0] == '%') {
        set_entry_num(atof(s_entry) / 100.0);
    } else if (k[0] == '=') {
        if (s_op) {
            s_cur = atof(s_entry);
            double r = apply_op(s_acc, s_cur, s_op);
            char h[64];
            snprintf(h, sizeof(h), "%.8g %c %.8g =", s_acc, s_op, s_cur);
            lv_label_set_text(s_calc_hist, h);
            if (isnan(r)) strcpy(s_entry, "Error"); else set_entry_num(r);
            s_op = 0;
            s_new_entry = true;
        }
    } else {                               /* + - x / */
        double v = atof(s_entry);
        if (s_op && !s_new_entry) v = apply_op(s_acc, v, s_op);
        s_acc = v;
        s_op = k[0];
        set_entry_num(v);
        char h[48];
        snprintf(h, sizeof(h), "%.10g %c", v, s_op);
        lv_label_set_text(s_calc_hist, h);
        s_new_entry = true;
    }
    calc_show();
}

static void calc_build(lv_obj_t *c, void *ctx)
{
    s_calc_hist = ui_label(c, "", &lv_font_montserrat_16, UI_MUTED);
    lv_obj_align(s_calc_hist, LV_ALIGN_TOP_RIGHT, -20, 0);
    s_calc_disp = ui_label(c, "0", &lv_font_montserrat_48, UI_TEXT);
    lv_obj_align(s_calc_disp, LV_ALIGN_TOP_RIGHT, -20, 22);
    static const char *map[] = { "C", "+/-", "%", "/", "\n",
                                 "7", "8", "9", "x", "\n",
                                 "4", "5", "6", "-", "\n",
                                 "1", "2", "3", "+", "\n",
                                 "0", ".", "=", "" };
    lv_obj_t *bm = lv_buttonmatrix_create(c);
    lv_buttonmatrix_set_map(bm, map);
    lv_buttonmatrix_set_button_width(bm, 20, 2);
    lv_obj_set_size(bm, UI_W - 10, 340);
    lv_obj_align(bm, LV_ALIGN_BOTTOM_MID, 0, -2);
    lv_obj_set_style_bg_opa(bm, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(bm, 0, 0);
    lv_obj_set_style_pad_all(bm, 4, 0);
    lv_obj_set_style_pad_gap(bm, 8, 0);
    lv_obj_set_style_radius(bm, 30, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(bm, lv_color_hex(0x333336), LV_PART_ITEMS);
    lv_obj_set_style_text_font(bm, &lv_font_montserrat_28, LV_PART_ITEMS);
    lv_obj_set_style_text_color(bm, lv_color_white(), LV_PART_ITEMS);
    lv_obj_set_style_shadow_width(bm, 0, LV_PART_ITEMS);
    lv_obj_add_event_cb(bm, calc_key, LV_EVENT_VALUE_CHANGED, NULL);
    calc_show();
}

void app_calculator_open(void) { ui_page_push_raw("Calculator", calc_build, NULL, NULL); }
