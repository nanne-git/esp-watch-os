/*
 * Settings app: Wi-Fi, Bluetooth, Web & remote, Display, Sound, Notifications,
 * Date & time, Battery, Storage, Weather, Activity, Updates, About.
 */
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "apps.h"
#include "audio.h"
#include "bsp/esp-bsp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ble_svc.h"
#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_idf_version.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "file_manager.h"
#include "hw.h"
#include "ota.h"
#include "settings.h"
#include "timesvc.h"
#include "ui.h"
#include "weather.h"
#include "wifi_svc.h"

/* ======================================================================== generic helpers */

#define OFF(f) ((void *)offsetof(settings_t, f))

static void bool_cb(lv_event_t *e)
{
    size_t off = (size_t)lv_event_get_user_data(e);
    bool on = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    settings_t *s = settings_edit();
    *(bool *)((uint8_t *)s + off) = on;
    settings_commit();
}

static lv_obj_t *toggle(lv_obj_t *c, const char *icon, uint32_t col, const char *text, void *off)
{
    bool on = *(const bool *)((const uint8_t *)settings() + (size_t)off);
    return ui_row_switch(c, icon, col, text, on, bool_cb, off);
}

/* text setting edited with the keyboard: user data = field offset, size via table */
typedef struct {
    size_t      off;
    size_t      size;
    const char *title;
    bool        password;
} text_field_t;

static void text_done(const char *text, void *user)
{
    const text_field_t *f = user;
    settings_t *s = settings_edit();
    strlcpy((char *)s + f->off, text, f->size);
    settings_commit();
    ui_page_rebuild();
}

static void text_click(lv_event_t *e)
{
    const text_field_t *f = lv_event_get_user_data(e);
    ui_keyboard(f->title, f->password ? "" : (const char *)settings() + f->off, f->password, text_done,
                (void *)f);
}

#define TEXT_FIELD(name, field, title, pw) \
    static const text_field_t name = { offsetof(settings_t, field), sizeof(((settings_t *)0)->field), title, pw }

static void fmt_bytes(uint64_t b, char *buf, size_t len)
{
    if (b >= 1024ULL * 1024 * 1024) snprintf(buf, len, "%.2f GB", b / 1073741824.0);
    else if (b >= 1024 * 1024) snprintf(buf, len, "%.1f MB", b / 1048576.0);
    else snprintf(buf, len, "%.0f KB", b / 1024.0);
}

/* ======================================================================== Wi-Fi */

static char s_join_ssid[33];

static void wifi_pass_done(const char *pass, void *user)
{
    wifi_svc_connect(s_join_ssid, pass);
    ui_toast("Connecting...");
}

static void wifi_net_click(lv_event_t *e)
{
    lv_obj_t *row = lv_event_get_current_target(e);
    const char *ssid = lv_obj_get_user_data(row);
    bool secure = lv_obj_has_flag(row, LV_OBJ_FLAG_USER_1);
    bool saved = lv_obj_has_flag(row, LV_OBJ_FLAG_USER_2);
    strlcpy(s_join_ssid, ssid, sizeof(s_join_ssid));
    if (secure && !saved) {
        ui_keyboard(s_join_ssid, "", true, wifi_pass_done, NULL);
    } else {
        wifi_svc_connect(s_join_ssid, saved ? NULL : "");   /* NULL = use the saved password */
        ui_toast("Connecting...");
    }
}

static void open_wifi_details(lv_event_t *e);

static wifi_ap_info_t s_aps[20];
static int            s_ap_n;

static void wifi_scan_build(lv_obj_t *c, void *ctx)
{
    if (s_ap_n == 0) {
        ui_label(c, "No networks found", &lv_font_montserrat_18, UI_MUTED);
    }
    for (int i = 0; i < s_ap_n; i++) {
        char val[16];
        snprintf(val, sizeof(val), "%s%d", s_aps[i].secure ? LV_SYMBOL_EYE_CLOSE " " : "", s_aps[i].rssi);
        lv_obj_t *r = ui_row(c, LV_SYMBOL_WIFI, s_aps[i].saved ? UI_GREEN : UI_BLUE, s_aps[i].ssid, val,
                             wifi_net_click, NULL);
        lv_obj_set_user_data(r, s_aps[i].ssid);
        if (s_aps[i].secure) lv_obj_add_flag(r, LV_OBJ_FLAG_USER_1);
        if (s_aps[i].saved) lv_obj_add_flag(r, LV_OBJ_FLAG_USER_2);
    }
}

static void wifi_scan_click(lv_event_t *e)
{
    ui_toast("Scanning...");
    /* scanning blocks ~3 s; it's acceptable here and keeps the code simple */
    s_ap_n = wifi_svc_scan(s_aps, 20);
    ui_page_push("Networks", wifi_scan_build, NULL, NULL);
}

static char s_forget[33];
static void do_forget(void *u)
{
    wifi_svc_forget(s_forget);
    ui_page_rebuild();
}
static void saved_click(lv_event_t *e)
{
    strlcpy(s_forget, lv_obj_get_user_data(lv_event_get_current_target(e)), sizeof(s_forget));
    char msg[80];
    snprintf(msg, sizeof(msg), "Forget \"%s\"? You'll need the password to join again.", s_forget);
    ui_confirm("Forget network", msg, "Forget", UI_RED, do_forget, NULL);
}

static void wifi_details_build(lv_obj_t *c, void *ctx)
{
    wifi_status_t w;
    wifi_svc_status(&w);
    char buf[24];
    ui_row(c, NULL, 0, "Network", w.ssid[0] ? w.ssid : "-", NULL, NULL);
    ui_row(c, NULL, 0, "IP address", w.ip[0] ? w.ip : "-", NULL, NULL);
    ui_row(c, NULL, 0, "Gateway", w.gw[0] ? w.gw : "-", NULL, NULL);
    ui_row(c, NULL, 0, "Subnet mask", w.mask[0] ? w.mask : "-", NULL, NULL);
    ui_row(c, NULL, 0, "DNS", w.dns[0] ? w.dns : "-", NULL, NULL);
    snprintf(buf, sizeof(buf), "%d dBm", w.rssi);
    ui_row(c, NULL, 0, "Signal", w.rssi ? buf : "-", NULL, NULL);
    ui_row(c, NULL, 0, "MAC", w.mac, NULL, NULL);
    ui_row(c, NULL, 0, "Hostname", settings()->hostname, NULL, NULL);
}

TEXT_FIELD(tf_ap_ssid, ap_ssid, "Hotspot name", false);
TEXT_FIELD(tf_ap_pass, ap_pass, "Hotspot password", true);
TEXT_FIELD(tf_hostname, hostname, "Hostname", false);

static lv_obj_t *s_wifi_status_row;

static void wifi_refresh(void *ctx)
{
    if (!s_wifi_status_row) return;
    wifi_status_t w;
    wifi_svc_status(&w);
    const char *v = w.state == WIFI_ST_CONNECTED ? w.ssid :
                    w.state == WIFI_ST_CONNECTING ? "Connecting..." :
                    w.state == WIFI_ST_OFF ? "Off" : "Not connected";
    ui_row_set_value(s_wifi_status_row, v);
}

static void wifi_build(lv_obj_t *c, void *ctx)
{
    wifi_status_t w;
    wifi_svc_status(&w);
    toggle(c, LV_SYMBOL_WIFI, UI_BLUE, "Wi-Fi", OFF(wifi_on));
    s_wifi_status_row = ui_row(c, LV_SYMBOL_OK, UI_GREEN, "Status", "", NULL, NULL);
    ui_row(c, LV_SYMBOL_LIST, UI_CARD2, "Connection details", w.ip[0] ? w.ip : NULL, open_wifi_details, NULL);
    ui_row(c, LV_SYMBOL_REFRESH, UI_BLUE, "Find networks", NULL, wifi_scan_click, NULL);

    char nets[WIFI_SAVED_MAX][33];
    int n = wifi_svc_saved(nets, WIFI_SAVED_MAX);
    ui_section(c, "SAVED NETWORKS (tap to forget)");
    static char saved_names[WIFI_SAVED_MAX][33];
    for (int i = 0; i < n; i++) {
        strlcpy(saved_names[i], nets[i], 33);
        lv_obj_t *r = ui_row(c, LV_SYMBOL_WIFI, UI_CARD2, saved_names[i], NULL, saved_click, NULL);
        lv_obj_set_user_data(r, saved_names[i]);
    }
    if (!n) ui_label(c, "  none yet", &lv_font_montserrat_16, UI_MUTED);

    ui_section(c, "HOTSPOT (watch creates its own Wi-Fi)");
    toggle(c, LV_SYMBOL_UPLOAD, UI_ORANGE, "Hotspot", OFF(hotspot_on));
    ui_row(c, NULL, 0, "Name", settings()->ap_ssid, text_click, (void *)&tf_ap_ssid);
    ui_row(c, NULL, 0, "Password", settings()->ap_pass[0] ? "********" : "none (open)", text_click,
           (void *)&tf_ap_pass);
    if (w.ap_on) {
        char cl[32];
        snprintf(cl, sizeof(cl), "%d connected", w.ap_clients);
        ui_row(c, NULL, 0, "Address", "192.168.4.1", NULL, NULL);
        ui_row(c, NULL, 0, "Clients", cl, NULL, NULL);
    }
    ui_section(c, "NETWORK NAME");
    ui_row(c, NULL, 0, "Hostname", settings()->hostname, text_click, (void *)&tf_hostname);
    wifi_refresh(NULL);
    ui_page_set_refresh(wifi_refresh);
}

static void wifi_close(void *ctx) { s_wifi_status_row = NULL; }

static void open_wifi(lv_event_t *e) { ui_page_push("Wi-Fi", wifi_build, wifi_close, NULL); }
static void open_wifi_details(lv_event_t *e) { ui_page_push("Connection", wifi_details_build, NULL, NULL); }

/* ======================================================================== Bluetooth */

TEXT_FIELD(tf_bt_name, bt_name, "Bluetooth name", false);

static void bt_build(lv_obj_t *c, void *ctx)
{
    toggle(c, LV_SYMBOL_BLUETOOTH, UI_BLUE, "Bluetooth", OFF(bt_on));
    ui_row(c, NULL, 0, "Name", settings()->bt_name, text_click, (void *)&tf_bt_name);
    ui_row(c, NULL, 0, "Status", ble_svc_connected() ? "Phone connected" :
           (ble_svc_advertising() ? "Visible, waiting" : "Off"), NULL, NULL);
    char addr[20];
    ble_svc_get_addr(addr, sizeof(addr));
    ui_row(c, NULL, 0, "Address", addr, NULL, NULL);
    ui_section(c, "CONNECT YOUR ANDROID PHONE");
    lv_obj_t *card = ui_card(c);
    lv_obj_t *t = ui_label(card,
                           "1. Install Gadgetbridge (F-Droid or Play Store)\n"
                           "2. Tap + and pick \"Bangle.js ESP-Watch\"\n"
                           "3. Allow notification access\n\n"
                           "Keep the name starting with \"Bangle.js\" so Gadgetbridge "
                           "recognises the watch.",
                           &lv_font_montserrat_16, 0xD1D1D6);
    lv_label_set_long_mode(t, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(t, lv_pct(100));
}

static void open_bt(lv_event_t *e) { ui_page_push("Bluetooth", bt_build, NULL, NULL); }

/* ======================================================================== Web & remote */

TEXT_FIELD(tf_web_pass, web_pass, "Web password", true);

static void web_build(lv_obj_t *c, void *ctx)
{
    toggle(c, LV_SYMBOL_DRIVE, UI_BLUE, "Web manager", OFF(web_on));
    const char *url = wifi_get_url();
    ui_row(c, NULL, 0, "Address", url[0] ? url : "not on Wi-Fi", NULL, NULL);
    char host[48];
    snprintf(host, sizeof(host), "%s.local", settings()->hostname);
    ui_row(c, NULL, 0, "Or", host, NULL, NULL);
    ui_row(c, LV_SYMBOL_EYE_CLOSE, UI_ORANGE, "Password", settings()->web_pass[0] ? "set" : "none",
           text_click, (void *)&tf_web_pass);
    lv_obj_t *card = ui_card(c);
    lv_obj_t *t = ui_label(card,
                           "Open the address in a browser on the same Wi-Fi (or the watch "
                           "hotspot) to manage files, settings, the screen and updates.",
                           &lv_font_montserrat_16, 0xD1D1D6);
    lv_label_set_long_mode(t, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(t, lv_pct(100));
}

static void open_web(lv_event_t *e) { ui_page_push("Web & remote", web_build, NULL, NULL); }

/* ======================================================================== Display */

static const char *TIMEOUT_NAMES[] = { "5 seconds", "10 seconds", "15 seconds", "30 seconds",
                                       "1 minute", "2 minutes", "5 minutes", "Never" };
static const uint16_t TIMEOUT_VALS[] = { 5, 10, 15, 30, 60, 120, 300, 0 };

static void timeout_chosen(int i, void *u)
{
    settings_t *s = settings_edit();
    s->screen_timeout_s = TIMEOUT_VALS[i];
    settings_commit();
    ui_page_rebuild();
}

static void timeout_click(lv_event_t *e)
{
    int cur = 2;
    for (int i = 0; i < 8; i++) {
        if (TIMEOUT_VALS[i] == settings()->screen_timeout_s) cur = i;
    }
    ui_choose("Screen timeout", TIMEOUT_NAMES, 8, cur, timeout_chosen, NULL);
}

static const char *ACCENT_NAMES[] = { "Blue", "Green", "Orange", "Pink", "Purple", "Teal", "Yellow", "Red" };
static const uint32_t ACCENT_VALS[] = { 0x4C9AFF, UI_GREEN, UI_ORANGE, UI_PINK, UI_PURPLE, UI_TEAL, UI_YELLOW, UI_RED };

static void accent_chosen(int i, void *u)
{
    settings_t *s = settings_edit();
    s->accent_color = ACCENT_VALS[i];
    settings_commit();
    ui_page_rebuild();
}

static void accent_click(lv_event_t *e)
{
    int cur = 0;
    for (int i = 0; i < 8; i++) {
        if (ACCENT_VALS[i] == settings()->accent_color) cur = i;
    }
    ui_choose("Accent colour", ACCENT_NAMES, 8, cur, accent_chosen, NULL);
}

static void bright_change(lv_event_t *e)
{
    bsp_display_brightness_set(lv_slider_get_value(lv_event_get_target(e)));
}

static void bright_release(lv_event_t *e)
{
    settings_t *s = settings_edit();
    s->brightness = lv_slider_get_value(lv_event_get_target(e));
    settings_commit();
}

static void display_build(lv_obj_t *c, void *ctx)
{
    lv_obj_t *sl = ui_row_slider(c, LV_SYMBOL_EYE_OPEN, "Brightness", 5, 100, settings()->brightness,
                                 bright_change, NULL);
    lv_obj_add_event_cb(sl, bright_release, LV_EVENT_RELEASED, NULL);
    const char *tv = "15 seconds";
    for (int i = 0; i < 8; i++) {
        if (TIMEOUT_VALS[i] == settings()->screen_timeout_s) tv = TIMEOUT_NAMES[i];
    }
    ui_row(c, LV_SYMBOL_EYE_CLOSE, UI_PURPLE, "Screen timeout", tv, timeout_click, NULL);
    toggle(c, LV_SYMBOL_UP, UI_GREEN, "Raise to wake", OFF(raise_to_wake));
    toggle(c, LV_SYMBOL_EYE_OPEN, UI_ORANGE, "Always-on display", OFF(always_on));
    static const char *faces[] = { "Digital", "Analog", "Minimal" };
    ui_row(c, LV_SYMBOL_IMAGE, UI_BLUE, "Watch face", faces[settings()->watchface % 3],
           (lv_event_cb_t)ui_open_face_picker, NULL);
    const char *an = "Custom";
    for (int i = 0; i < 8; i++) {
        if (ACCENT_VALS[i] == settings()->accent_color) an = ACCENT_NAMES[i];
    }
    ui_row(c, LV_SYMBOL_TINT, settings()->accent_color, "Accent colour", an, accent_click, NULL);
}

static void open_display(lv_event_t *e) { ui_page_push("Display", display_build, NULL, NULL); }

/* ======================================================================== Sound */

static void vol_release(lv_event_t *e)
{
    settings_t *s = settings_edit();
    s->volume = lv_slider_get_value(lv_event_get_target(e));
    settings_commit();
    audio_beep(880, 150);
}

static void test_beep(lv_event_t *e) { audio_beep(1000, 300); }
static void test_vib(lv_event_t *e)
{
    static const uint16_t p[] = { 150, 100, 150 };
    haptic_pattern(p, 3);
}

static void sound_build(lv_obj_t *c, void *ctx)
{
    lv_obj_t *sl = ui_row_slider(c, LV_SYMBOL_VOLUME_MAX, "Volume", 0, 100, settings()->volume, NULL, NULL);
    lv_obj_add_event_cb(sl, vol_release, LV_EVENT_RELEASED, NULL);
    toggle(c, LV_SYMBOL_MUTE, UI_RED, "Mute", OFF(muted));
    toggle(c, LV_SYMBOL_BELL, UI_ORANGE, "Vibration", OFF(vibration));
    ui_row(c, LV_SYMBOL_AUDIO, UI_PINK, "Test speaker", NULL, test_beep, NULL);
    ui_row(c, LV_SYMBOL_BELL, UI_PURPLE, "Test vibration", NULL, test_vib, NULL);
}

static void open_sound(lv_event_t *e) { ui_page_push("Sound & vibration", sound_build, NULL, NULL); }

/* ======================================================================== Notifications */

static int s_dnd_edit;  /* 0 start, 1 end */
static int s_dnd_hour;

static void dnd_min_chosen(int m, void *u)
{
    settings_t *s = settings_edit();
    uint16_t v = s_dnd_hour * 60 + m;
    if (s_dnd_edit == 0) s->dnd_start_min = v; else s->dnd_end_min = v;
    settings_commit();
    ui_page_rebuild();
}

static void dnd_hour_chosen(int h, void *u)
{
    s_dnd_hour = h;
    ui_number_picker("Minute", 0, 59, 0, dnd_min_chosen, NULL);
}

static void dnd_time_click(lv_event_t *e)
{
    s_dnd_edit = (int)(intptr_t)lv_event_get_user_data(e);
    uint16_t cur = s_dnd_edit == 0 ? settings()->dnd_start_min : settings()->dnd_end_min;
    ui_number_picker("Hour", 0, 23, cur / 60, dnd_hour_chosen, NULL);
}

static void notif_settings_build(lv_obj_t *c, void *ctx)
{
    toggle(c, LV_SYMBOL_BELL, UI_RED, "Phone notifications", OFF(notif_on));
    toggle(c, LV_SYMBOL_EYE_OPEN, UI_BLUE, "Wake screen", OFF(notif_wake));
    ui_section(c, "DO NOT DISTURB");
    toggle(c, LV_SYMBOL_MUTE, UI_PURPLE, "Do not disturb", OFF(dnd));
    toggle(c, LV_SYMBOL_LOOP, UI_PURPLE, "Scheduled", OFF(dnd_scheduled));
    char b[16];
    snprintf(b, sizeof(b), "%02d:%02d", settings()->dnd_start_min / 60, settings()->dnd_start_min % 60);
    ui_row(c, NULL, 0, "From", b, dnd_time_click, (void *)0);
    snprintf(b, sizeof(b), "%02d:%02d", settings()->dnd_end_min / 60, settings()->dnd_end_min % 60);
    ui_row(c, NULL, 0, "Until", b, dnd_time_click, (void *)1);
}

static void open_notif(lv_event_t *e) { ui_page_push("Notifications", notif_settings_build, NULL, NULL); }

/* ======================================================================== Date & time */

static const char *TZ_NAMES[] = {
    "India (UTC+5:30)", "UTC", "London", "Berlin / Paris", "Dubai (UTC+4)", "Pakistan (UTC+5)",
    "Nepal (UTC+5:45)", "Bangladesh (UTC+6)", "Singapore (UTC+8)", "China (UTC+8)", "Tokyo (UTC+9)",
    "Sydney", "New York", "Chicago", "Denver", "Los Angeles",
};
static const char *TZ_VALS[] = {
    "IST-5:30", "UTC0", "GMT0BST,M3.5.0/1,M10.5.0", "CET-1CEST,M3.5.0,M10.5.0/3", "<+04>-4", "PKT-5",
    "<+0545>-5:45", "<+06>-6", "<+08>-8", "CST-8", "JST-9", "AEST-10AEDT,M10.1.0,M4.1.0/3",
    "EST5EDT,M3.2.0,M11.1.0", "CST6CDT,M3.2.0,M11.1.0", "MST7MDT,M3.2.0,M11.1.0", "PST8PDT,M3.2.0,M11.1.0",
};
#define TZ_N (sizeof(TZ_VALS) / sizeof(TZ_VALS[0]))

static void tz_chosen(int i, void *u)
{
    settings_t *s = settings_edit();
    strlcpy(s->timezone, TZ_VALS[i], sizeof(s->timezone));
    settings_commit();
    ui_page_rebuild();
}

static void tz_click(lv_event_t *e)
{
    int cur = -1;
    for (size_t i = 0; i < TZ_N; i++) {
        if (strcmp(TZ_VALS[i], settings()->timezone) == 0) cur = i;
    }
    ui_choose("Time zone", TZ_NAMES, TZ_N, cur, tz_chosen, NULL);
}

static struct tm s_edit_tm;

static void apply_edit_tm(void)
{
    s_edit_tm.tm_isdst = -1;
    time_t t = mktime(&s_edit_tm);      /* local time -> UTC */
    timesvc_set_utc(t);
    ui_page_rebuild();
}

static void set_min(int v, void *u) { s_edit_tm.tm_min = v; s_edit_tm.tm_sec = 0; apply_edit_tm(); }
static void set_hour(int v, void *u) { s_edit_tm.tm_hour = v; ui_number_picker("Minute", 0, 59, s_edit_tm.tm_min, set_min, NULL); }
static void set_day(int v, void *u) { s_edit_tm.tm_mday = v; apply_edit_tm(); }
static void set_mon(int v, void *u) { s_edit_tm.tm_mon = v - 1; ui_number_picker("Day", 1, 31, s_edit_tm.tm_mday, set_day, NULL); }
static void set_year(int v, void *u) { s_edit_tm.tm_year = v - 1900; ui_number_picker("Month", 1, 12, s_edit_tm.tm_mon + 1, set_mon, NULL); }

static void set_time_click(lv_event_t *e)
{
    time_t now = time(NULL);
    localtime_r(&now, &s_edit_tm);
    ui_number_picker("Hour", 0, 23, s_edit_tm.tm_hour, set_hour, NULL);
}

static void set_date_click(lv_event_t *e)
{
    time_t now = time(NULL);
    localtime_r(&now, &s_edit_tm);
    int y = s_edit_tm.tm_year + 1900;
    ui_number_picker("Year", 2024, 2040, y < 2024 ? 2026 : y, set_year, NULL);
}

static void datetime_build(lv_obj_t *c, void *ctx)
{
    char b[48];
    timesvc_format_time(b, sizeof(b), true);
    time_t now = time(NULL);
    struct tm lt;
    localtime_r(&now, &lt);
    char d[24];
    snprintf(d, sizeof(d), "%04d-%02d-%02d", lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday);
    toggle(c, LV_SYMBOL_REFRESH, UI_BLUE, "Automatic time", OFF(auto_time));
    ui_row(c, LV_SYMBOL_BELL, UI_ORANGE, "Time", b, set_time_click, NULL);
    ui_row(c, LV_SYMBOL_LIST, UI_ORANGE, "Date", d, set_date_click, NULL);
    const char *tzn = settings()->timezone;
    for (size_t i = 0; i < TZ_N; i++) {
        if (strcmp(TZ_VALS[i], tzn) == 0) tzn = TZ_NAMES[i];
    }
    ui_row(c, LV_SYMBOL_GPS, UI_GREEN, "Time zone", tzn, tz_click, NULL);
    toggle(c, LV_SYMBOL_LOOP, UI_PURPLE, "24-hour clock", OFF(time_24h));
    lv_obj_t *card = ui_card(c);
    lv_obj_t *t = ui_label(card, "With automatic time on, the watch syncs from the internet over Wi-Fi "
                                 "and from your phone (Gadgetbridge), which also sets the time zone.",
                           &lv_font_montserrat_14, UI_MUTED);
    lv_label_set_long_mode(t, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(t, lv_pct(100));
}

static void open_datetime(lv_event_t *e) { ui_page_push("Date & time", datetime_build, NULL, NULL); }

/* ======================================================================== Battery & power */

static lv_obj_t *s_bat_rows[5];

static void battery_refresh(void *ctx)
{
    if (!s_bat_rows[0]) return;
    power_status_t p;
    power_get(&p);
    char b[32];
    snprintf(b, sizeof(b), p.percent >= 0 ? "%d %%" : "-", p.percent);
    ui_row_set_value(s_bat_rows[0], b);
    ui_row_set_value(s_bat_rows[1], p.charging ? "Charging" : p.full ? "Full" : p.usb ? "On USB" : "On battery");
    snprintf(b, sizeof(b), "%.2f V", p.voltage_mv / 1000.0);
    ui_row_set_value(s_bat_rows[2], b);
    snprintf(b, sizeof(b), "%.0f °C", p.temp_c);
    ui_row_set_value(s_bat_rows[3], b);
}

static void do_restart(void *u) { power_restart(); }
static void do_off(void *u) { power_off(); }
static void restart_click(lv_event_t *e) { ui_confirm("Restart", "Restart the watch now?", "Restart", UI_ORANGE, do_restart, NULL); }
static void off_click(lv_event_t *e) { ui_confirm("Power off", "Turn the watch off? Press PWR to turn it on.", "Power off", UI_RED, do_off, NULL); }

static void battery_build(lv_obj_t *c, void *ctx)
{
    s_bat_rows[0] = ui_row(c, LV_SYMBOL_BATTERY_FULL, UI_GREEN, "Battery", "", NULL, NULL);
    s_bat_rows[1] = ui_row(c, LV_SYMBOL_CHARGE, UI_YELLOW, "State", "", NULL, NULL);
    s_bat_rows[2] = ui_row(c, NULL, 0, "Voltage", "", NULL, NULL);
    s_bat_rows[3] = ui_row(c, NULL, 0, "Power chip temp.", "", NULL, NULL);
    toggle(c, LV_SYMBOL_BATTERY_1, UI_ORANGE, "Power saving", OFF(power_save));
    lv_obj_t *card = ui_card(c);
    lv_obj_t *t = ui_label(card, "Power saving: brightness max 50 %, screen off after 10 s, "
                                 "Wi-Fi radio naps, slower Bluetooth advertising.",
                           &lv_font_montserrat_14, UI_MUTED);
    lv_label_set_long_mode(t, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(t, lv_pct(100));
    ui_row(c, LV_SYMBOL_REFRESH, UI_ORANGE, "Restart", NULL, restart_click, NULL);
    ui_row(c, LV_SYMBOL_POWER, UI_RED, "Power off", NULL, off_click, NULL);
    battery_refresh(NULL);
    ui_page_set_refresh(battery_refresh);
}

static void battery_close(void *ctx) { s_bat_rows[0] = NULL; }
static void open_battery(lv_event_t *e) { ui_page_push("Battery & power", battery_build, battery_close, NULL); }

/* ======================================================================== Storage */

static void do_format(void *u)
{
    ui_toast(fm_format() ? "SD card formatted" : "Format failed");
    ui_page_rebuild();
}
static void format_click(lv_event_t *e)
{
    ui_confirm("Format SD card", "This ERASES EVERYTHING on the memory card. Continue?", "Erase", UI_RED,
               do_format, NULL);
}
static void mount_click(lv_event_t *e)
{
    if (fm_is_mounted()) {
        fm_unmount();
        ui_toast("Safe to remove the card");
    } else {
        ui_toast(fm_mount() ? "SD card mounted" : "No card found");
    }
    ui_page_rebuild();
}

static void storage_build(lv_obj_t *c, void *ctx)
{
    char name[16], b1[24], b2[24], b3[24];
    uint64_t total = 0, freeb = 0;
    if (fm_is_mounted() && fm_card_info(name, sizeof(name), &total, &freeb)) {
        lv_obj_t *card = ui_card(c);
        lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(card, 10, 0);
        fmt_bytes(total - freeb, b1, sizeof(b1));
        fmt_bytes(total, b2, sizeof(b2));
        char line[64];
        snprintf(line, sizeof(line), "%s used of %s", b1, b2);
        ui_label(card, line, &lv_font_montserrat_20, UI_TEXT);
        lv_obj_t *bar = lv_bar_create(card);
        lv_obj_set_size(bar, lv_pct(100), 14);
        lv_bar_set_range(bar, 0, 1000);
        lv_bar_set_value(bar, total ? (int)((total - freeb) * 1000 / total) : 0, LV_ANIM_OFF);
        lv_obj_set_style_bg_color(bar, ui_accent(), LV_PART_INDICATOR);
        fmt_bytes(freeb, b3, sizeof(b3));
        ui_row(c, LV_SYMBOL_SD_CARD, UI_BLUE, "Card", name, NULL, NULL);
        ui_row(c, NULL, 0, "Free", b3, NULL, NULL);
        ui_row(c, LV_SYMBOL_DIRECTORY, UI_BLUE, "Browse files", NULL, (lv_event_cb_t)app_files_open, NULL);
        ui_row(c, LV_SYMBOL_EJECT, UI_ORANGE, "Unmount (safe remove)", NULL, mount_click, NULL);
        ui_row(c, LV_SYMBOL_TRASH, UI_RED, "Format card", NULL, format_click, NULL);
    } else {
        ui_row(c, LV_SYMBOL_SD_CARD, UI_CARD2, "SD card", "not mounted", NULL, NULL);
        ui_row(c, LV_SYMBOL_REFRESH, UI_BLUE, "Mount card", NULL, mount_click, NULL);
    }
    uint32_t flash = 0;
    esp_flash_get_size(NULL, &flash);
    fmt_bytes(flash, b1, sizeof(b1));
    ui_section(c, "INTERNAL");
    ui_row(c, NULL, 0, "Flash", b1, NULL, NULL);
    fmt_bytes(heap_caps_get_free_size(MALLOC_CAP_SPIRAM), b2, sizeof(b2));
    ui_row(c, NULL, 0, "Free PSRAM", b2, NULL, NULL);
}

static void open_storage(lv_event_t *e) { ui_page_push("Storage", storage_build, NULL, NULL); }

/* ======================================================================== Weather & location */

TEXT_FIELD(tf_city, city, "City name", false);

static int s_coord_field;   /* 0 lat, 1 lon */
static void coord_done(const char *text, void *u)
{
    float v = strtof(text, NULL);
    settings_t *s = settings_edit();
    if (s_coord_field == 0 && v >= -90 && v <= 90) s->latitude = v;
    if (s_coord_field == 1 && v >= -180 && v <= 180) s->longitude = v;
    settings_commit();
    weather_refresh();
    ui_page_rebuild();
}
static void coord_click(lv_event_t *e)
{
    s_coord_field = (int)(intptr_t)lv_event_get_user_data(e);
    char b[24];
    snprintf(b, sizeof(b), "%.4f", s_coord_field == 0 ? settings()->latitude : settings()->longitude);
    ui_keyboard(s_coord_field == 0 ? "Latitude" : "Longitude", b, false, coord_done, NULL);
}
static void weather_now(lv_event_t *e) { weather_refresh(); ui_toast("Updating weather..."); }

static void location_build(lv_obj_t *c, void *ctx)
{
    char b[24];
    ui_row(c, LV_SYMBOL_HOME, UI_TEAL, "City", settings()->city, text_click, (void *)&tf_city);
    snprintf(b, sizeof(b), "%.4f", settings()->latitude);
    ui_row(c, NULL, 0, "Latitude", b, coord_click, (void *)0);
    snprintf(b, sizeof(b), "%.4f", settings()->longitude);
    ui_row(c, NULL, 0, "Longitude", b, coord_click, (void *)1);
    ui_row(c, LV_SYMBOL_REFRESH, UI_BLUE, "Update weather now", NULL, weather_now, NULL);
}

static void open_location(lv_event_t *e) { ui_page_push("Weather location", location_build, NULL, NULL); }

/* ======================================================================== Activity */

static void goal_chosen(int v, void *u)
{
    settings_t *s = settings_edit();
    s->step_goal = v * 500;
    settings_commit();
    ui_page_rebuild();
}
static void goal_click(lv_event_t *e) { ui_number_picker("Goal x500 steps", 2, 60, settings()->step_goal / 500, goal_chosen, NULL); }
static void reset_steps(void *u) { imu_reset_steps(); ui_toast("Steps reset"); }
static void reset_steps_click(lv_event_t *e) { ui_confirm("Reset steps", "Set today's step count to 0?", "Reset", UI_ORANGE, reset_steps, NULL); }

static void activity_build(lv_obj_t *c, void *ctx)
{
    char b[24];
    snprintf(b, sizeof(b), "%u steps", settings()->step_goal);
    ui_row(c, LV_SYMBOL_SHUFFLE, UI_GREEN, "Daily goal", b, goal_click, NULL);
    ui_row(c, LV_SYMBOL_TRASH, UI_ORANGE, "Reset today's steps", NULL, reset_steps_click, NULL);
}

static void open_activity(lv_event_t *e) { ui_page_push("Activity", activity_build, NULL, NULL); }

/* ======================================================================== Updates */

TEXT_FIELD(tf_repo, ota_repo, "GitHub repo (owner/name)", false);
TEXT_FIELD(tf_token, ota_token, "GitHub token", true);

static lv_obj_t *s_ota_status;

static void updates_refresh(void *ctx)
{
    if (!s_ota_status) return;
    ota_status_t o;
    ota_get(&o);
    char b[128];
    switch (o.state) {
    case OTA_CHECKING: snprintf(b, sizeof(b), "Checking..."); break;
    case OTA_DOWNLOADING: snprintf(b, sizeof(b), "Downloading %d %%", o.progress); break;
    case OTA_DONE: snprintf(b, sizeof(b), "Installed - restarting"); break;
    case OTA_ERROR: snprintf(b, sizeof(b), "%s", o.message); break;
    default:
        if (o.latest[0]) {
            snprintf(b, sizeof(b), o.update_available ? "Update %s available" : "Up to date (%s)", o.latest);
        } else {
            snprintf(b, sizeof(b), "Not checked yet");
        }
    }
    lv_label_set_text(s_ota_status, b);
}

static void check_click(lv_event_t *e) { ota_check_async(); }
static void do_install(void *u) { ota_install_async(); }
static void install_click(lv_event_t *e)
{
    ota_status_t o;
    ota_get(&o);
    if (!o.update_available) {
        ui_toast("No update found yet - tap Check first");
        return;
    }
    ui_confirm("Install update", "Download and install the new firmware? The watch restarts when done.",
               "Install", UI_BLUE, do_install, NULL);
}

static void updates_build(lv_obj_t *c, void *ctx)
{
    ota_status_t o;
    ota_get(&o);
    ui_row(c, LV_SYMBOL_DOWNLOAD, UI_BLUE, "Installed version", o.current, NULL, NULL);
    lv_obj_t *card = ui_card(c);
    s_ota_status = ui_label(card, "", &lv_font_montserrat_18, UI_TEXT);
    lv_label_set_long_mode(s_ota_status, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_ota_status, lv_pct(100));
    ui_row(c, LV_SYMBOL_REFRESH, UI_BLUE, "Check for updates", NULL, check_click, NULL);
    ui_row(c, LV_SYMBOL_DOWNLOAD, UI_GREEN, "Install update", NULL, install_click, NULL);
    toggle(c, LV_SYMBOL_LOOP, UI_PURPLE, "Check automatically", OFF(ota_auto_check));
    ui_section(c, "SOURCE (GITHUB RELEASES)");
    ui_row(c, NULL, 0, "Repository", settings()->ota_repo, text_click, (void *)&tf_repo);
    ui_row(c, NULL, 0, "Access token", settings()->ota_token[0] ? "set" : "not set", text_click,
           (void *)&tf_token);
    lv_obj_t *c2 = ui_card(c);
    lv_obj_t *t = ui_label(c2, "Updates come from your GitHub releases over any internet connection. "
                               "A private repo needs a read-only token. You can also upload a "
                               "firmware file in the web manager.",
                           &lv_font_montserrat_14, UI_MUTED);
    lv_label_set_long_mode(t, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(t, lv_pct(100));
    updates_refresh(NULL);
    ui_page_set_refresh(updates_refresh);
}

static void updates_close(void *ctx) { s_ota_status = NULL; }
static void open_updates(lv_event_t *e) { ui_page_push("Updates", updates_build, updates_close, NULL); }

/* ======================================================================== About */

static void do_factory(void *u)
{
    settings_factory_reset();
    ui_toast("Settings reset - restarting");
    vTaskDelay(pdMS_TO_TICKS(1500));
    power_restart();
}
static void factory_click(lv_event_t *e)
{
    ui_confirm("Factory reset", "Reset all settings, saved Wi-Fi networks and alarms? Files on the SD card "
               "are kept.", "Reset", UI_RED, do_factory, NULL);
}

static void about_build(lv_obj_t *c, void *ctx)
{
    const esp_app_desc_t *app = esp_app_get_description();
    char b[48];
    ui_row(c, NULL, 0, "Device", "ESP32-S3 AMOLED 2.06\"", NULL, NULL);
    ui_row(c, NULL, 0, "Firmware", app->version, NULL, NULL);
    snprintf(b, sizeof(b), "%s", app->date);
    ui_row(c, NULL, 0, "Built", b, NULL, NULL);
    ui_row(c, NULL, 0, "ESP-IDF", esp_get_idf_version(), NULL, NULL);
    esp_chip_info_t ci;
    esp_chip_info(&ci);
    snprintf(b, sizeof(b), "ESP32-S3 rev %d.%d, %d cores", ci.revision / 100, ci.revision % 100, ci.cores);
    ui_row(c, NULL, 0, "Chip", b, NULL, NULL);
    wifi_status_t w;
    wifi_svc_status(&w);
    ui_row(c, NULL, 0, "IP address", w.ip[0] ? w.ip : "-", NULL, NULL);
    ui_row(c, NULL, 0, "Wi-Fi MAC", w.mac, NULL, NULL);
    char addr[20];
    ble_svc_get_addr(addr, sizeof(addr));
    ui_row(c, NULL, 0, "Bluetooth MAC", addr, NULL, NULL);
    int64_t up = esp_timer_get_time() / 1000000;
    snprintf(b, sizeof(b), "%lldd %02lld:%02lld", up / 86400, (up / 3600) % 24, (up / 60) % 60);
    ui_row(c, NULL, 0, "Uptime", b, NULL, NULL);
    fmt_bytes(heap_caps_get_free_size(MALLOC_CAP_INTERNAL), b, sizeof(b));
    ui_row(c, NULL, 0, "Free RAM", b, NULL, NULL);
    fmt_bytes(heap_caps_get_free_size(MALLOC_CAP_SPIRAM), b, sizeof(b));
    ui_row(c, NULL, 0, "Free PSRAM", b, NULL, NULL);
    ui_row(c, LV_SYMBOL_WARNING, UI_RED, "Factory reset", NULL, factory_click, NULL);
}

void app_about_open(void) { ui_page_push("About", about_build, NULL, NULL); }
static void open_about(lv_event_t *e) { app_about_open(); }

/* ======================================================================== main list */

static void settings_build(lv_obj_t *c, void *ctx)
{
    wifi_status_t w;
    wifi_svc_status(&w);
    ui_row(c, LV_SYMBOL_WIFI, UI_BLUE, "Wi-Fi", w.state == WIFI_ST_CONNECTED ? w.ssid :
           (settings()->wifi_on ? "Not connected" : "Off"), open_wifi, NULL);
    ui_row(c, LV_SYMBOL_BLUETOOTH, UI_BLUE, "Bluetooth", ble_svc_connected() ? "Connected" :
           (settings()->bt_on ? "On" : "Off"), open_bt, NULL);
    ui_row(c, LV_SYMBOL_DRIVE, UI_TEAL, "Web & remote", settings()->web_on ? "On" : "Off", open_web, NULL);
    ui_row(c, LV_SYMBOL_EYE_OPEN, UI_PURPLE, "Display", NULL, open_display, NULL);
    ui_row(c, LV_SYMBOL_VOLUME_MAX, UI_PINK, "Sound & vibration", NULL, open_sound, NULL);
    ui_row(c, LV_SYMBOL_BELL, UI_RED, "Notifications", settings()->dnd ? "DND" : NULL, open_notif, NULL);
    ui_row(c, LV_SYMBOL_LOOP, UI_ORANGE, "Date & time", NULL, open_datetime, NULL);
    ui_row(c, LV_SYMBOL_BATTERY_FULL, UI_GREEN, "Battery & power", NULL, open_battery, NULL);
    ui_row(c, LV_SYMBOL_SD_CARD, UI_BLUE, "Storage", NULL, open_storage, NULL);
    ui_row(c, LV_SYMBOL_GPS, UI_TEAL, "Weather location", settings()->city, open_location, NULL);
    ui_row(c, LV_SYMBOL_SHUFFLE, UI_GREEN, "Activity", NULL, open_activity, NULL);
    ui_row(c, LV_SYMBOL_DOWNLOAD, UI_BLUE, "Updates", NULL, open_updates, NULL);
    ui_row(c, LV_SYMBOL_HOME, UI_CARD2, "About", NULL, open_about, NULL);
}

void app_settings_open(void)
{
    ui_page_push("Settings", settings_build, NULL, NULL);
}
