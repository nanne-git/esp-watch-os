/*
 * Home screen (watch faces) + launcher, quick settings, notification centre,
 * widgets page, face picker and power menu.
 */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "alarm.h"
#include "bsp/esp-bsp.h"
#include "esp_timer.h"
#include "apps.h"
#include "lua_apps.h"
#include "esp_heap_caps.h"
#include "ble_svc.h"
#include "hw.h"
#include "phone.h"
#include "settings.h"
#include "timesvc.h"
#include "ui.h"
#include "display.h"
#include "weather.h"
#include "wifi_svc.h"

static const char *WEEKDAYS[] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
static const char *MONTHS[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };

/* ======================================================================== status cache */

static power_status_t s_pwr;
static int64_t        s_pwr_at;

static const power_status_t *pwr(void)
{
    int64_t now = esp_timer_get_time();
    if (!s_pwr_at || now - s_pwr_at > 5000000) {
        power_get(&s_pwr);
        s_pwr_at = now;
    }
    return &s_pwr;
}

static const char *battery_symbol(int pct)
{
    if (pct < 0) return LV_SYMBOL_BATTERY_EMPTY;
    if (pct > 85) return LV_SYMBOL_BATTERY_FULL;
    if (pct > 60) return LV_SYMBOL_BATTERY_3;
    if (pct > 35) return LV_SYMBOL_BATTERY_2;
    if (pct > 12) return LV_SYMBOL_BATTERY_1;
    return LV_SYMBOL_BATTERY_EMPTY;
}

static void status_text(char *buf, size_t len)
{
    const power_status_t *p = pwr();
    wifi_status_t w;
    wifi_svc_status(&w);
    char bat[24] = "";
    if (p->percent >= 0) {
        snprintf(bat, sizeof(bat), "%s%s %d%%", p->charging ? LV_SYMBOL_CHARGE " " : "",
                 battery_symbol(p->percent), p->percent);
    } else if (p->usb) {
        snprintf(bat, sizeof(bat), LV_SYMBOL_USB);
    }
    snprintf(buf, len, "%s%s%s%s   %s", notif_dnd_active() ? LV_SYMBOL_MUTE "  " : "",
             ble_svc_connected() ? LV_SYMBOL_BLUETOOTH "  " : "",
             w.state == WIFI_ST_CONNECTED ? LV_SYMBOL_WIFI "  " : "",
             notif_count() ? LV_SYMBOL_BELL : "", bat);
}

/* only touch a label when its text really changes: every set_text redraws that area */
static void set_txt(lv_obj_t *l, const char *t)
{
    if (l && strcmp(lv_label_get_text(l), t) != 0) {
        lv_label_set_text(l, t);
    }
}

/* ======================================================================== watch faces */

typedef struct {
    lv_obj_t *root;
    lv_draw_buf_t *dial;     /* analog dial picture (PSRAM) */
    lv_obj_t *status, *date, *time, *ampm, *hint;
    lv_obj_t *c_steps_arc, *c_steps_lbl, *c_notif_lbl, *c_bat_arc, *c_bat_lbl, *c_extra_lbl;
    lv_obj_t *scale;
    lv_obj_t *needle_h, *needle_m, *needle_s;
    lv_obj_t *hour_big, *min_big;
    int       face;
    bool      aod;
    int       needle_key, needle_sec;   /* last drawn hour*60+min / second */
} face_t;

static face_t s_face = { .face = -1 };

static lv_obj_t *complication(lv_obj_t *parent, uint32_t color, const char *icon, lv_obj_t **arc_out,
                              lv_obj_t **lbl_out)
{
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_remove_style_all(c);
    lv_obj_set_size(c, 104, 104);
    lv_obj_remove_flag(c, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *arc = lv_arc_create(c);
    lv_obj_set_size(arc, 104, 104);
    lv_arc_set_bg_angles(arc, 0, 360);
    lv_arc_set_rotation(arc, 270);
    lv_arc_set_range(arc, 0, 100);
    lv_arc_set_value(arc, 0);
    lv_obj_remove_style(arc, NULL, LV_PART_KNOB);
    lv_obj_remove_flag(arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(arc, 8, LV_PART_MAIN);
    lv_obj_set_style_arc_width(arc, 8, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(arc, lv_color_hex(0x262629), LV_PART_MAIN);
    lv_obj_set_style_arc_color(arc, lv_color_hex(color), LV_PART_INDICATOR);
    lv_obj_center(arc);
    lv_obj_t *ic = ui_label(c, icon, &lv_font_montserrat_20, color);
    lv_obj_align(ic, LV_ALIGN_CENTER, 0, -16);
    lv_obj_t *l = ui_label(c, "--", &lv_font_montserrat_18, UI_TEXT);
    lv_obj_align(l, LV_ALIGN_CENTER, 0, 14);
    if (arc_out) *arc_out = arc;
    if (lbl_out) *lbl_out = l;
    return c;
}

LV_FONT_DECLARE(font_clock_112);
LV_FONT_DECLARE(font_clock_144);

static void build_digital(lv_obj_t *r, bool aod)
{
    s_face.status = ui_label(r, "", &lv_font_montserrat_18, UI_MUTED);
    lv_obj_align(s_face.status, LV_ALIGN_TOP_MID, 0, 22);
    s_face.date = ui_label(r, "", &lv_font_montserrat_24, aod ? 0x707070 : UI_MUTED);
    lv_obj_align(s_face.date, LV_ALIGN_TOP_MID, 0, aod ? 120 : 76);
    /* a real 112 px font: scaling a 48 px label up (transform) made every redraw of the
     * time render into an extra layer and resample it - the most expensive thing on screen */
    s_face.time = ui_label(r, "", &font_clock_112, aod ? 0x9A9A9A : UI_TEXT);
    lv_obj_align(s_face.time, LV_ALIGN_TOP_MID, 0, aod ? 160 : 112);
    s_face.ampm = ui_label(r, "", &lv_font_montserrat_20, aod ? 0x606060 : 0);
    lv_obj_set_style_text_color(s_face.ampm, aod ? lv_color_hex(0x606060) : ui_accent(), 0);
    lv_obj_align(s_face.ampm, LV_ALIGN_TOP_MID, 0, aod ? 290 : 240);
    if (aod) {
        return;
    }
    lv_obj_t *row = lv_obj_create(r);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, UI_W - 30, 110);
    lv_obj_align(row, LV_ALIGN_BOTTOM_MID, 0, -54);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    complication(row, UI_GREEN, LV_SYMBOL_SHUFFLE, &s_face.c_steps_arc, &s_face.c_steps_lbl);
    lv_obj_t *n = complication(row, UI_BLUE, LV_SYMBOL_BELL, NULL, &s_face.c_notif_lbl);
    lv_arc_set_value(lv_obj_get_child(n, 0), 100);
    complication(row, UI_ORANGE, LV_SYMBOL_CHARGE, &s_face.c_bat_arc, &s_face.c_bat_lbl);
    s_face.c_extra_lbl = ui_label(r, "", &lv_font_montserrat_16, UI_MUTED);
    lv_obj_align(s_face.c_extra_lbl, LV_ALIGN_BOTTOM_MID, 0, -22);
}

static void build_analog(lv_obj_t *r, bool aod)
{
    lv_obj_t *sc = lv_scale_create(r);
    s_face.scale = sc;
    lv_obj_set_size(sc, 380, 380);
    lv_obj_align(sc, LV_ALIGN_CENTER, 0, aod ? 0 : -20);
    lv_scale_set_mode(sc, LV_SCALE_MODE_ROUND_INNER);
    lv_scale_set_range(sc, 0, 60);
    lv_scale_set_angle_range(sc, 360);
    lv_scale_set_rotation(sc, 270);
    lv_scale_set_total_tick_count(sc, 61);
    lv_scale_set_major_tick_every(sc, 5);
    lv_scale_set_label_show(sc, !aod);
    static const char *nums[] = { "12", "1", "2", "3", "4", "5", "6", "7", "8", "9", "10", "11", "12", NULL };
    lv_scale_set_text_src(sc, nums);
    lv_obj_set_style_bg_opa(sc, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(sc, lv_color_hex(aod ? 0x000000 : 0x111114), 0);
    lv_obj_set_style_radius(sc, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_text_font(sc, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(sc, lv_color_hex(UI_TEXT), 0);
    lv_obj_set_style_length(sc, 6, LV_PART_ITEMS);
    lv_obj_set_style_length(sc, 16, LV_PART_INDICATOR);
    lv_obj_set_style_line_width(sc, 2, LV_PART_ITEMS);
    lv_obj_set_style_line_width(sc, 4, LV_PART_INDICATOR);
    lv_obj_set_style_line_color(sc, lv_color_hex(0x48484A), LV_PART_ITEMS);
    lv_obj_set_style_line_color(sc, lv_color_hex(aod ? 0x707070 : UI_TEXT), LV_PART_INDICATOR);
    lv_obj_set_style_pad_all(sc, 8, 0);

    /* The dial (61 ticks + 12 numbers) never changes but was re-drawn for every frame that
     * touched it (each second-hand tick). Draw it ONCE into a picture and show that behind
     * the hands; the scale object stays only as the hands' container and draws nothing. */
    lv_obj_update_layout(sc);
    lv_draw_buf_t *dial = lv_snapshot_take(sc, LV_COLOR_FORMAT_RGB565);
    if (dial) {
        s_face.dial = dial;
        lv_obj_t *img = lv_image_create(r);
        lv_image_set_src(img, dial);
        lv_obj_align(img, LV_ALIGN_CENTER, 0, aod ? 0 : -20);
        lv_obj_move_background(img);
        lv_obj_remove_flag(img, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(img, LV_OBJ_FLAG_EVENT_BUBBLE | LV_OBJ_FLAG_GESTURE_BUBBLE);
        lv_scale_set_total_tick_count(sc, 0);
        lv_scale_set_label_show(sc, false);
        lv_obj_set_style_bg_opa(sc, LV_OPA_TRANSP, 0);
    }

    s_face.needle_h = lv_line_create(sc);
    lv_obj_set_style_line_width(s_face.needle_h, 9, 0);
    lv_obj_set_style_line_rounded(s_face.needle_h, true, 0);
    lv_obj_set_style_line_color(s_face.needle_h, lv_color_hex(aod ? 0x8A8A8A : UI_TEXT), 0);
    s_face.needle_m = lv_line_create(sc);
    lv_obj_set_style_line_width(s_face.needle_m, 6, 0);
    lv_obj_set_style_line_rounded(s_face.needle_m, true, 0);
    lv_obj_set_style_line_color(s_face.needle_m, lv_color_hex(aod ? 0x8A8A8A : 0xE5E5EA), 0);
    if (!aod) {
        s_face.needle_s = lv_line_create(sc);
        lv_obj_set_style_line_width(s_face.needle_s, 2, 0);
        lv_obj_set_style_line_color(s_face.needle_s, ui_accent(), 0);
    }
    lv_obj_t *dot = lv_obj_create(sc);
    lv_obj_remove_style_all(dot);
    lv_obj_set_size(dot, 16, 16);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(dot, aod ? lv_color_hex(0x8A8A8A) : ui_accent(), 0);
    lv_obj_center(dot);

    s_face.date = ui_label(sc, "", &lv_font_montserrat_18, UI_MUTED);
    lv_obj_align(s_face.date, LV_ALIGN_CENTER, 0, 60);
    if (!aod) {
        s_face.status = ui_label(r, "", &lv_font_montserrat_18, UI_MUTED);
        lv_obj_align(s_face.status, LV_ALIGN_BOTTOM_MID, 0, -26);
    }
}

static void build_minimal(lv_obj_t *r, bool aod)
{
    s_face.hour_big = ui_label(r, "", &font_clock_144, 0);
    lv_obj_set_style_text_color(s_face.hour_big, aod ? lv_color_hex(0x8A8A8A) : ui_accent(), 0);
    lv_obj_align(s_face.hour_big, LV_ALIGN_CENTER, 0, -90);
    s_face.min_big = ui_label(r, "", &font_clock_144, aod ? 0x6A6A6A : UI_TEXT);
    lv_obj_align(s_face.min_big, LV_ALIGN_CENTER, 0, 70);
    s_face.date = ui_label(r, "", &lv_font_montserrat_20, UI_MUTED);
    lv_obj_align(s_face.date, LV_ALIGN_BOTTOM_MID, 0, -40);
    if (!aod) {
        s_face.status = ui_label(r, "", &lv_font_montserrat_16, UI_MUTED);
        lv_obj_align(s_face.status, LV_ALIGN_TOP_MID, 0, 20);
    }
}

static void face_rebuild(lv_obj_t *scr, int face, bool aod)
{
    if (s_face.root) {
        lv_obj_delete(s_face.root);
    }
    if (s_face.dial) {
        lv_draw_buf_destroy(s_face.dial);   /* the image using it was deleted with root */
    }
    memset(&s_face, 0, sizeof(s_face));
    s_face.face = face;
    s_face.aod = aod;
    s_face.needle_key = s_face.needle_sec = -1;
    lv_obj_t *r = lv_obj_create(scr);
    s_face.root = r;
    lv_obj_remove_style_all(r);
    lv_obj_set_size(r, UI_W, UI_H);
    lv_obj_remove_flag(r, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(r, LV_OBJ_FLAG_EVENT_BUBBLE | LV_OBJ_FLAG_GESTURE_BUBBLE);
    switch (face) {
    case 1:  build_analog(r, aod); break;
    case 2:  build_minimal(r, aod); break;
    default: build_digital(r, aod); break;
    }
    /* make every child pass gestures/clicks to the screen */
    for (uint32_t i = 0; i < lv_obj_get_child_count(r); i++) {
        lv_obj_add_flag(lv_obj_get_child(r, i), LV_OBJ_FLAG_EVENT_BUBBLE | LV_OBJ_FLAG_GESTURE_BUBBLE);
        lv_obj_remove_flag(lv_obj_get_child(r, i), LV_OBJ_FLAG_CLICKABLE);
    }
    lv_obj_invalidate(scr);     /* normal <-> always-on: repaint every pixel */
}

static void set_needle(lv_obj_t *line, lv_point_precise_t *pts, float angle_deg, int len, int tail)
{
    float a = (angle_deg - 90) * (float)M_PI / 180.0f;
    int cx = 182, cy = 182;     /* scale is 380 with 8 px padding -> inner centre ~182 */
    pts[0].x = cx - cosf(a) * tail;
    pts[0].y = cy - sinf(a) * tail;
    pts[1].x = cx + cosf(a) * len;
    pts[1].y = cy + sinf(a) * len;
    /* An lv_line is sized from (0,0) to its largest point, so a hand drawn in scale
     * coordinates made a ~340x340 object and every tick redrew half the screen. Shift the
     * points to the hand's own bounding box and move the object there instead. */
    lv_value_precise_t x0 = pts[0].x < pts[1].x ? pts[0].x : pts[1].x;
    lv_value_precise_t y0 = pts[0].y < pts[1].y ? pts[0].y : pts[1].y;
    pts[0].x -= x0; pts[1].x -= x0;
    pts[0].y -= y0; pts[1].y -= y0;
    lv_obj_set_pos(line, (int32_t)x0, (int32_t)y0);
    lv_line_set_points(line, pts, 2);
}

static lv_obj_t *s_home_scr;

void ui_home_build(lv_obj_t *scr)
{
    s_home_scr = scr;
    face_rebuild(scr, settings()->watchface, false);
    ui_home_refresh();
}

void ui_home_refresh(void)
{
    if (!s_home_scr) {
        return;
    }
    bool aod = !ui_is_awake() && settings()->always_on;   /* screen fully off: keep the normal face */
    if (s_face.face != settings()->watchface || s_face.aod != aod) {
        face_rebuild(s_home_scr, settings()->watchface, aod);
    }
    time_t now = time(NULL);
    struct tm lt;
    localtime_r(&now, &lt);
    char buf[96];
    bool valid = timesvc_valid();

    if (s_face.date) {
        if (valid) {
            snprintf(buf, sizeof(buf), "%s, %d %s", WEEKDAYS[lt.tm_wday], lt.tm_mday, MONTHS[lt.tm_mon]);
        } else {
            snprintf(buf, sizeof(buf), "Time not set");
        }
        set_txt(s_face.date, buf);
    }
    if (s_face.status) {
        static int64_t last_status;
        int64_t tnow = esp_timer_get_time();
        if (tnow - last_status > 3000000 || strcmp(lv_label_get_text(s_face.status), "") == 0) {
            last_status = tnow;
            status_text(buf, sizeof(buf));
            set_txt(s_face.status, buf);
        }
    }
    int h12 = lt.tm_hour % 12 ? lt.tm_hour % 12 : 12;
    bool h24 = settings()->time_24h;
    if (s_face.time) {
        snprintf(buf, sizeof(buf), "%d:%02d", h24 ? lt.tm_hour : h12, lt.tm_min);
        if (h24) {
            snprintf(buf, sizeof(buf), "%02d:%02d", lt.tm_hour, lt.tm_min);
        }
        set_txt(s_face.time, buf);
        set_txt(s_face.ampm, h24 ? "" : (lt.tm_hour < 12 ? "AM" : "PM"));
    }
    if (s_face.hour_big) {
        snprintf(buf, sizeof(buf), "%02d", h24 ? lt.tm_hour : h12);
        set_txt(s_face.hour_big, buf);
        snprintf(buf, sizeof(buf), "%02d", lt.tm_min);
        set_txt(s_face.min_big, buf);
    }
    if (s_face.needle_h) {
        static lv_point_precise_t ph[2], pm[2], ps[2];
        int key = lt.tm_hour * 60 + lt.tm_min;
        if (key != s_face.needle_key) {
            s_face.needle_key = key;
            set_needle(s_face.needle_h, ph, (lt.tm_hour % 12) * 30 + lt.tm_min * 0.5f, 95, 14);
            set_needle(s_face.needle_m, pm, lt.tm_min * 6, 145, 16);
        }
        if (s_face.needle_s && lt.tm_sec != s_face.needle_sec) {
            s_face.needle_sec = lt.tm_sec;
            set_needle(s_face.needle_s, ps, lt.tm_sec * 6, 160, 26);
        }
    }
    if (s_face.c_steps_lbl) {
        uint32_t steps = imu_steps_today();
        uint16_t goal = settings()->step_goal ? settings()->step_goal : 8000;
        snprintf(buf, sizeof(buf), "%lu", (unsigned long)steps);
        set_txt(s_face.c_steps_lbl, buf);
        int sv = steps * 100 / goal > 100 ? 100 : steps * 100 / goal;
        if (lv_arc_get_value(s_face.c_steps_arc) != sv) lv_arc_set_value(s_face.c_steps_arc, sv);
        snprintf(buf, sizeof(buf), "%d", notif_count());
        set_txt(s_face.c_notif_lbl, buf);
        const power_status_t *p = pwr();
        if (p->percent >= 0) {
            snprintf(buf, sizeof(buf), "%d%%", p->percent);
            if (lv_arc_get_value(s_face.c_bat_arc) != p->percent) lv_arc_set_value(s_face.c_bat_arc, p->percent);
        } else {
            snprintf(buf, sizeof(buf), p->usb ? "USB" : "--");
        }
        set_txt(s_face.c_bat_lbl, buf);
        char extra[48] = "";
        weather_t w;
        weather_get(&w);
        char next[24];
        if (w.valid && alarm_next(next, sizeof(next))) {
            snprintf(extra, sizeof(extra), "%.0f°C  " LV_SYMBOL_BELL " %s", w.temp_c, next);
        } else if (w.valid) {
            snprintf(extra, sizeof(extra), "%s  %.0f°C", weather_desc(w.code), w.temp_c);
        } else if (alarm_next(next, sizeof(next))) {
            snprintf(extra, sizeof(extra), LV_SYMBOL_BELL " %s", next);
        }
        set_txt(s_face.c_extra_lbl, extra);
    }
}

/* ======================================================================== launcher */

static void app_tile_cb(lv_event_t *e)
{
    const app_def_t *a = lv_event_get_user_data(e);
    ui_recents_note(a->name, a->icon, false, a->color, a->open, NULL);
    a->open();
}

/* Lua apps found on the SD card when the launcher opened */
static lua_app_info_t *s_lua_apps;
static int             s_lua_count;

static void lua_tile_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i >= s_lua_count) return;
    lua_app_info_t *a = &s_lua_apps[i];
    ui_recents_note(a->name, a->icon, a->icon_is_image, a->color, NULL, a->id);
    lua_app_open(a->id);
}

static void lua_uninstall_yes(void *user)
{
    int i = (int)(intptr_t)user;
    if (i < s_lua_count && lua_apps_remove(s_lua_apps[i].id)) {
        ui_toast("App removed");
        ui_page_rebuild();
    }
}

static void lua_tile_long_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i >= s_lua_count) return;
    lv_indev_wait_release(lv_indev_active());   /* no click after the long press */
    static char msg[96];
    snprintf(msg, sizeof(msg), "Remove \"%s\" and its data from the SD card?", s_lua_apps[i].name);
    ui_confirm("Uninstall app", msg, "Uninstall", UI_RED, lua_uninstall_yes, (void *)(intptr_t)i);
}

static lv_obj_t *app_tile(lv_obj_t *c, const char *name, const char *icon, bool icon_img, uint32_t color,
                          lv_event_cb_t cb, void *user)
{
    lv_obj_t *t = lv_obj_create(c);
    lv_obj_remove_style_all(t);
    lv_obj_set_size(t, 118, 122);
    lv_obj_add_flag(t, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(t, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(t, cb, LV_EVENT_CLICKED, user);
    lv_obj_set_style_opa(t, LV_OPA_70, LV_STATE_PRESSED);
    lv_obj_t *ic = lv_obj_create(t);
    lv_obj_remove_style_all(ic);
    lv_obj_set_size(ic, 80, 80);
    lv_obj_align(ic, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_radius(ic, 24, 0);
    if (icon_img) lv_obj_set_style_clip_corner(ic, true, 0);   /* only pictures need clipping */
    lv_obj_set_style_bg_opa(ic, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(ic, lv_color_hex(color), 0);
    /* flat colour: a gradient costs extra work per pixel on every redraw of the list */
    lv_obj_remove_flag(ic, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(ic, LV_OBJ_FLAG_EVENT_BUBBLE);
    if (icon_img) {
        lv_obj_t *img = lv_image_create(ic);
        lv_image_set_src(img, icon);
        lv_obj_center(img);
    } else {
        lv_obj_t *il = ui_label(ic, icon, &lv_font_montserrat_32, UI_TEXT);
        lv_obj_center(il);
    }
    lv_obj_t *nl = ui_label(t, name, &lv_font_montserrat_16, 0xE5E5EA);
    lv_label_set_long_mode(nl, LV_LABEL_LONG_DOT);
    lv_obj_set_width(nl, 118);
    lv_obj_set_style_text_align(nl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(nl, LV_ALIGN_BOTTOM_MID, 0, 0);
    return t;
}

static void recents_btn_cb(lv_event_t *e) { ui_open_recents(); }

static void launcher_build(lv_obj_t *c, void *ctx)
{
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(c, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(c, 14, 0);
    lv_obj_set_style_pad_hor(c, 6, 0);
    /* "Recent" shortcut on top of the grid */
    lv_obj_t *rb = ui_button(c, LV_SYMBOL_LIST "  Recent apps", UI_CARD2, recents_btn_cb, NULL);
    lv_obj_set_width(rb, lv_pct(94));
    for (int i = 0; i < g_app_count; i++) {
        const app_def_t *a = &g_apps[i];
        app_tile(c, a->name, a->icon, false, a->color, app_tile_cb, (void *)a);
    }
    /* installed Lua apps (SD card /apps) */
    if (!s_lua_apps) s_lua_apps = heap_caps_calloc(32, sizeof(lua_app_info_t), MALLOC_CAP_SPIRAM);
    s_lua_count = s_lua_apps ? lua_apps_scan(s_lua_apps, 32) : 0;
    for (int i = 0; i < s_lua_count; i++) {
        lua_app_info_t *a = &s_lua_apps[i];
        lv_obj_t *t = app_tile(c, a->name, a->icon, a->icon_is_image, a->color, lua_tile_cb, (void *)(intptr_t)i);
        lv_obj_add_event_cb(t, lua_tile_long_cb, LV_EVENT_LONG_PRESSED, (void *)(intptr_t)i);
    }
}

void ui_open_launcher(void)
{
    ui_page_push("Apps", launcher_build, NULL, NULL);
}

/* ======================================================================== quick settings */

typedef enum { QS_WIFI, QS_BT, QS_DND, QS_SAVE, QS_HOTSPOT, QS_WEB, QS_AOD, QS_TORCH } qs_t;

static bool qs_state(qs_t q)
{
    const settings_t *s = settings();
    switch (q) {
    case QS_WIFI: return s->wifi_on;
    case QS_BT: return s->bt_on;
    case QS_DND: return s->dnd;
    case QS_SAVE: return s->power_save;
    case QS_HOTSPOT: return s->hotspot_on;
    case QS_WEB: return s->web_on;
    case QS_AOD: return s->always_on;
    default: return false;
    }
}

static void qs_style(lv_obj_t *tile, bool on)
{
    lv_obj_set_style_bg_color(tile, on ? ui_accent() : lv_color_hex(UI_CARD2), 0);
}

static void qs_click(lv_event_t *e)
{
    qs_t q = (qs_t)(intptr_t)lv_event_get_user_data(e);
    if (q == QS_TORCH) {
        app_flashlight_open();
        return;
    }
    settings_t *s = settings_edit();
    switch (q) {
    case QS_WIFI: s->wifi_on = !s->wifi_on; break;
    case QS_BT: s->bt_on = !s->bt_on; break;
    case QS_DND: s->dnd = !s->dnd; break;
    case QS_SAVE: s->power_save = !s->power_save; break;
    case QS_HOTSPOT: s->hotspot_on = !s->hotspot_on; break;
    case QS_WEB: s->web_on = !s->web_on; break;
    case QS_AOD: s->always_on = !s->always_on; break;
    default: break;
    }
    settings_commit();
    qs_style(lv_event_get_current_target(e), qs_state(q));
}

static void qs_bright_cb(lv_event_t *e)
{
    int v = lv_slider_get_value(lv_event_get_target(e));
    if (lv_event_get_code(e) == LV_EVENT_VALUE_CHANGED) {
        display_brightness_set(v);
    }
}

static void qs_bright_release(lv_event_t *e)
{
    settings_t *s = settings_edit();
    s->brightness = lv_slider_get_value(lv_event_get_target(e));
    settings_commit();
}

static void qs_vol_release(lv_event_t *e)
{
    settings_t *s = settings_edit();
    s->volume = lv_slider_get_value(lv_event_get_target(e));
    settings_commit();
}

static lv_obj_t *s_qs_info;

static void qs_refresh(void *ctx)
{
    if (!s_qs_info) {
        return;
    }
    char buf[128], st[64];
    wifi_status_t w;
    wifi_svc_status(&w);
    status_text(st, sizeof(st));
    snprintf(buf, sizeof(buf), "%s\n%s", st,
             w.state == WIFI_ST_CONNECTED ? w.ip : (w.ap_on ? "Hotspot 192.168.4.1" : "Wi-Fi not connected"));
    set_txt(s_qs_info, buf);
}

static void qs_build(lv_obj_t *c, void *ctx)
{
    s_qs_info = ui_label(c, "", &lv_font_montserrat_16, UI_MUTED);
    lv_obj_set_style_text_align(s_qs_info, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(s_qs_info, lv_pct(100));

    lv_obj_t *grid = lv_obj_create(c);
    lv_obj_remove_style_all(grid);
    lv_obj_set_size(grid, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(grid, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(grid, 10, 0);
    static const struct { qs_t q; const char *icon; const char *name; } tiles[] = {
        { QS_WIFI, LV_SYMBOL_WIFI, "Wi-Fi" },       { QS_BT, LV_SYMBOL_BLUETOOTH, "Bluetooth" },
        { QS_DND, LV_SYMBOL_MUTE, "Do not disturb" }, { QS_TORCH, LV_SYMBOL_IMAGE, "Flashlight" },
        { QS_HOTSPOT, LV_SYMBOL_UPLOAD, "Hotspot" }, { QS_WEB, LV_SYMBOL_DRIVE, "Web server" },
        { QS_SAVE, LV_SYMBOL_BATTERY_1, "Power saving" }, { QS_AOD, LV_SYMBOL_EYE_OPEN, "Always on" },
    };
    for (size_t i = 0; i < sizeof(tiles) / sizeof(tiles[0]); i++) {
        lv_obj_t *t = lv_obj_create(grid);
        lv_obj_set_size(t, 180, 76);
        lv_obj_set_style_radius(t, 20, 0);
        lv_obj_set_style_border_width(t, 0, 0);
        lv_obj_set_style_pad_all(t, 12, 0);
        lv_obj_remove_flag(t, LV_OBJ_FLAG_SCROLLABLE);
        qs_style(t, qs_state(tiles[i].q));
        lv_obj_add_event_cb(t, qs_click, LV_EVENT_CLICKED, (void *)(intptr_t)tiles[i].q);
        lv_obj_t *ic = ui_label(t, tiles[i].icon, &lv_font_montserrat_20, UI_TEXT);
        lv_obj_align(ic, LV_ALIGN_TOP_LEFT, 0, 0);
        lv_obj_t *nl = ui_label(t, tiles[i].name, &lv_font_montserrat_16, UI_TEXT);
        lv_obj_align(nl, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    }
    lv_obj_t *bs = ui_row_slider(c, LV_SYMBOL_EYE_OPEN, "Brightness", 5, 100, settings()->brightness,
                                 qs_bright_cb, NULL);
    lv_obj_add_event_cb(bs, qs_bright_release, LV_EVENT_RELEASED, NULL);
    lv_obj_t *vs = ui_row_slider(c, LV_SYMBOL_VOLUME_MAX, "Volume", 0, 100, settings()->volume, NULL, NULL);
    lv_obj_add_event_cb(vs, qs_vol_release, LV_EVENT_RELEASED, NULL);
    ui_row(c, LV_SYMBOL_SETTINGS, UI_CARD2, "All settings", NULL, (lv_event_cb_t)app_settings_open, NULL);
    qs_refresh(NULL);
    ui_page_set_refresh(qs_refresh);
}

static void qs_close(void *ctx) { s_qs_info = NULL; }

void ui_open_quick_settings(void)
{
    ui_page_push("Quick settings", qs_build, qs_close, NULL);
}

/* ======================================================================== notifications */

static void notif_detail_build(lv_obj_t *c, void *ctx);

static void notif_dismiss_cb(lv_event_t *e)
{
    uint32_t id = (uint32_t)(uintptr_t)lv_event_get_user_data(e);
    notif_remove_id(id);
    ui_page_pop();
}

static void notif_open_cb(lv_event_t *e)
{
    uint32_t id = (uint32_t)(uintptr_t)lv_event_get_user_data(e);
    ui_page_push("Notification", notif_detail_build, NULL, (void *)(uintptr_t)id);
}

static void notif_detail_build(lv_obj_t *c, void *ctx)
{
    uint32_t id = (uint32_t)(uintptr_t)ctx;
    notif_t n;
    for (int i = 0; i < notif_count(); i++) {
        if (notif_get(i, &n) && n.id == id) {
            lv_obj_t *card = ui_card(c);
            lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
            lv_obj_set_style_pad_row(card, 8, 0);
            char when[32];
            struct tm lt;
            localtime_r(&n.when, &lt);
            snprintf(when, sizeof(when), "%s  -  %02d:%02d", n.app, lt.tm_hour, lt.tm_min);
            ui_label(card, when, &lv_font_montserrat_16, UI_MUTED);
            lv_obj_t *t = ui_label(card, n.title, &lv_font_montserrat_24, UI_TEXT);
            lv_label_set_long_mode(t, LV_LABEL_LONG_WRAP);
            lv_obj_set_width(t, lv_pct(100));
            lv_obj_t *b = ui_label(card, n.body, &lv_font_montserrat_20, 0xD1D1D6);
            lv_label_set_long_mode(b, LV_LABEL_LONG_WRAP);
            lv_obj_set_width(b, lv_pct(100));
            lv_obj_t *d = ui_button(c, "Dismiss", UI_CARD2, notif_dismiss_cb, (void *)(uintptr_t)id);
            lv_obj_set_width(d, lv_pct(100));
            return;
        }
    }
    ui_label(c, "Notification no longer available", &lv_font_montserrat_18, UI_MUTED);
}

static void notif_clear_cb(lv_event_t *e)
{
    notif_clear();
    ui_page_rebuild();
}

static int s_notif_shown = -1;
static uint32_t s_notif_gen_shown;

static void notif_list_build(lv_obj_t *c, void *ctx);

static void notif_refresh(void *ctx)
{
    if (notif_generation() != s_notif_gen_shown) {     /* count alone misses replacements */
        ui_page_rebuild();
    }
}

static void notif_list_build(lv_obj_t *c, void *ctx)
{
    int n = notif_count();
    s_notif_shown = n;
    s_notif_gen_shown = notif_generation();
    if (n == 0) {
        lv_obj_t *l = ui_label(c, LV_SYMBOL_BELL "\n\nNo notifications", &lv_font_montserrat_20, UI_MUTED);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(l, lv_pct(100));
        lv_obj_set_style_pad_top(l, 100, 0);
        lv_obj_t *hint = ui_label(c, phone_connected() ? "Phone connected" :
                                  "Connect Gadgetbridge on your phone\n(Settings > Bluetooth)",
                                  &lv_font_montserrat_14, 0x636366);
        lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(hint, lv_pct(100));
    }
    for (int i = 0; i < n; i++) {
        notif_t nt;
        if (!notif_get(i, &nt)) {
            continue;
        }
        lv_obj_t *card = ui_card(c);
        lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_color(card, lv_color_hex(UI_CARD2), LV_STATE_PRESSED);
        lv_obj_add_event_cb(card, notif_open_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)nt.id);
        lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(card, 4, 0);
        char hdr[48];
        struct tm lt;
        localtime_r(&nt.when, &lt);
        snprintf(hdr, sizeof(hdr), "%s  %02d:%02d", nt.app, lt.tm_hour, lt.tm_min);
        ui_label(card, hdr, &lv_font_montserrat_14, UI_MUTED);
        lv_obj_t *t = ui_label(card, nt.title, &lv_font_montserrat_20, UI_TEXT);
        lv_label_set_long_mode(t, LV_LABEL_LONG_DOT);
        lv_obj_set_width(t, lv_pct(100));
        lv_obj_t *b = ui_label(card, nt.body, &lv_font_montserrat_16, 0xC7C7CC);
        lv_label_set_long_mode(b, LV_LABEL_LONG_DOT);
        lv_obj_set_size(b, lv_pct(100), 40);
    }
    if (n) {
        lv_obj_t *b = ui_button(c, "Clear all", UI_CARD2, notif_clear_cb, NULL);
        lv_obj_set_width(b, lv_pct(100));
    }
    ui_page_set_refresh(notif_refresh);
}

void ui_open_notifications(void)
{
    ui_page_push("Notifications", notif_list_build, NULL, NULL);
}

void app_notifications_open(void)
{
    ui_open_notifications();
}

/* ======================================================================== widgets page */

static lv_obj_t *s_w_steps, *s_w_weather, *s_w_music, *s_w_alarm, *s_w_bat;

static void widgets_refresh(void *ctx)
{
    if (!s_w_steps) {
        return;
    }
    char buf[128];
    snprintf(buf, sizeof(buf), "%lu / %u steps", (unsigned long)imu_steps_today(), settings()->step_goal);
    set_txt(s_w_steps, buf);
    weather_t w;
    weather_get(&w);
    if (w.valid) {
        snprintf(buf, sizeof(buf), "%s  %.0f°C  (%.0f° / %.0f°)\n%s", settings()->city, w.temp_c, w.tmax,
                 w.tmin, weather_desc(w.code));
    } else {
        snprintf(buf, sizeof(buf), "%s: weather needs Wi-Fi", settings()->city);
    }
    set_txt(s_w_weather, buf);
    phone_music_t m;
    phone_music_get(&m);
    snprintf(buf, sizeof(buf), "%s", m.valid ? (m.track[0] ? m.track : "Phone music") : "No phone music");
    set_txt(s_w_music, buf);
    char next[24];
    snprintf(buf, sizeof(buf), "%s", alarm_next(next, sizeof(next)) ? next : "No alarms");
    set_txt(s_w_alarm, buf);
    const power_status_t *p = pwr();
    snprintf(buf, sizeof(buf), "%d%%  %.2f V  %s", p->percent, p->voltage_mv / 1000.0,
             p->charging ? "charging" : (p->usb ? "USB" : "on battery"));
    set_txt(s_w_bat, buf);
}

static lv_obj_t *widget(lv_obj_t *c, const char *icon, uint32_t color, const char *title, void (*open)(void))
{
    lv_obj_t *card = ui_card(c);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, 4, 0);
    if (open) {
        lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(card, (lv_event_cb_t)open, LV_EVENT_CLICKED, NULL);
    }
    char buf[48];
    snprintf(buf, sizeof(buf), "%s  %s", icon, title);
    lv_obj_t *t = ui_label(card, buf, &lv_font_montserrat_16, color);
    (void)t;
    lv_obj_t *v = ui_label(card, "", &lv_font_montserrat_20, UI_TEXT);
    lv_label_set_long_mode(v, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(v, lv_pct(100));
    return v;
}

static void widgets_build(lv_obj_t *c, void *ctx)
{
    s_w_steps = widget(c, LV_SYMBOL_SHUFFLE, UI_GREEN, "Activity", app_steps_open);
    s_w_weather = widget(c, LV_SYMBOL_IMAGE, UI_TEAL, "Weather", app_weather_open);
    s_w_music = widget(c, LV_SYMBOL_AUDIO, UI_PINK, "Phone music", app_phone_music_open);
    s_w_alarm = widget(c, LV_SYMBOL_BELL, UI_ORANGE, "Next alarm", app_alarms_open);
    s_w_bat = widget(c, LV_SYMBOL_CHARGE, UI_YELLOW, "Battery", NULL);
    widgets_refresh(NULL);
    ui_page_set_refresh(widgets_refresh);
}

static void widgets_close(void *ctx) { s_w_steps = NULL; }

void ui_open_widgets(void)
{
    ui_page_push("Today", widgets_build, widgets_close, NULL);
}

/* ======================================================================== face picker / power */

static void face_chosen(int idx, void *u)
{
    settings_t *s = settings_edit();
    s->watchface = idx;
    settings_commit();
    ui_go_home();
}

void ui_open_face_picker(void)
{
    static const char *faces[] = { "Digital", "Analog", "Minimal" };
    ui_choose("Watch face", faces, 3, settings()->watchface, face_chosen, NULL);
}

static void do_restart(void *u) { power_restart(); }
static void do_poweroff(void *u) { power_off(); }

static void power_btn(lv_event_t *e)
{
    int which = (int)(intptr_t)lv_event_get_user_data(e);
    ui_page_pop();
    if (which == 1) {
        ui_confirm("Restart", "Restart the watch now?", "Restart", UI_ORANGE, do_restart, NULL);
    } else if (which == 2) {
        ui_confirm("Power off", "Turn the watch off? Press the PWR button to turn it on again.",
                   "Power off", UI_RED, do_poweroff, NULL);
    } else if (which == 3) {
        ui_sleep();
    }
}

static void power_build(lv_obj_t *c, void *ctx)
{
    ui_row(c, LV_SYMBOL_REFRESH, UI_ORANGE, "Restart", NULL, power_btn, (void *)1);
    ui_row(c, LV_SYMBOL_POWER, UI_RED, "Power off", NULL, power_btn, (void *)2);
    ui_row(c, LV_SYMBOL_EYE_CLOSE, UI_CARD2, "Screen off", NULL, power_btn, (void *)3);
}

static void power_close(void *ctx)
{
}

void ui_open_power_menu(void)
{
    ui_page_push("Power", power_build, power_close, NULL);
}
