/*
 * Recent apps: the last apps you opened, with what is still running in the background
 * (music, recording, timer, stopwatch, Lua app). Tap a card to go back to the app,
 * the X closes it (stops whatever it was doing), "Close all" stops everything.
 * Open with: long-press BOOT on the watch face, or "Recent" in the app list.
 */
#include <stdio.h>
#include <string.h>

#include "alarm.h"
#include "apps.h"
#include "audio.h"
#include "lua_apps.h"
#include "ui.h"

#define MAX_RECENTS 8

typedef struct {
    char        name[32];
    char        icon[64];
    bool        icon_img;
    uint32_t    color;
    void      (*open)(void);   /* built-in app */
    char        lua_id[32];    /* or a Lua app */
} recent_t;

static recent_t s_rec[MAX_RECENTS];
static int      s_n;
static lv_obj_t *s_list;
static lv_obj_t *s_status[MAX_RECENTS];   /* status labels, updated once a second */

void ui_recents_note(const char *name, const char *icon, bool icon_img, uint32_t color,
                     void (*open)(void), const char *lua_id)
{
    if (!name) return;
    int found = -1;
    for (int i = 0; i < s_n; i++) {
        if (strcmp(s_rec[i].name, name) == 0) found = i;
    }
    recent_t r = { 0 };
    strlcpy(r.name, name, sizeof(r.name));
    strlcpy(r.icon, icon ? icon : "", sizeof(r.icon));
    r.icon_img = icon_img;
    r.color = color;
    r.open = open;
    if (lua_id) strlcpy(r.lua_id, lua_id, sizeof(r.lua_id));
    int last = found >= 0 ? found : (s_n < MAX_RECENTS ? s_n++ : MAX_RECENTS - 1);
    memmove(&s_rec[1], &s_rec[0], last * sizeof(recent_t));     /* newest first */
    s_rec[0] = r;
}

/* what the app is doing in the background right now ("" = nothing) */
static const char *status_of(const recent_t *r, char *buf, size_t size)
{
    buf[0] = '\0';
    if (r->lua_id[0]) {
        char id[32];
        if (lua_apps_running(id, sizeof(id)) && strcmp(id, r->lua_id) == 0) strlcpy(buf, "Open", size);
    } else if (strcmp(r->name, "Music") == 0) {
        player_state_t st = player_state();
        if (st == PLAYER_PLAYING) strlcpy(buf, LV_SYMBOL_PLAY " Playing", size);
        else if (st == PLAYER_PAUSED) strlcpy(buf, LV_SYMBOL_PAUSE " Paused", size);
    } else if (strcmp(r->name, "Recorder") == 0) {
        if (rec_active()) snprintf(buf, size, "Recording %d:%02d", rec_seconds() / 60, rec_seconds() % 60);
    } else if (strcmp(r->name, "Timer") == 0) {
        if (timer_running() || timer_paused()) {
            int s = timer_remaining();
            snprintf(buf, size, "%s %d:%02d", timer_paused() ? "Paused" : "Running", s / 60, s % 60);
        }
    } else if (strcmp(r->name, "Stopwatch") == 0) {
        if (sw_running() || sw_elapsed_ms() > 0) {
            int s = (int)(sw_elapsed_ms() / 1000);
            snprintf(buf, size, "%s %d:%02d", sw_running() ? "Running" : "Stopped", s / 60, s % 60);
        }
    }
    return buf;
}

static void stop_app(const recent_t *r)
{
    if (r->lua_id[0]) {
        char id[32];
        if (lua_apps_running(id, sizeof(id)) && strcmp(id, r->lua_id) == 0) lua_apps_stop();
    } else if (strcmp(r->name, "Music") == 0) {
        player_stop();
    } else if (strcmp(r->name, "Recorder") == 0) {
        if (rec_active()) rec_stop();
    } else if (strcmp(r->name, "Timer") == 0) {
        timer_cancel();
    } else if (strcmp(r->name, "Stopwatch") == 0) {
        sw_stop();
        sw_reset();
    }
}

static void remove_at(int i)
{
    stop_app(&s_rec[i]);
    memmove(&s_rec[i], &s_rec[i + 1], (s_n - i - 1) * sizeof(recent_t));
    s_n--;
}

static void build_list(void);
static void rebuild_async(void *arg);

static void open_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i >= s_n) return;
    recent_t r = s_rec[i];
    ui_go_home();
    if (r.lua_id[0]) lua_app_open(r.lua_id);
    else if (r.open) r.open();
    ui_recents_note(r.name, r.icon, r.icon_img, r.color, r.open, r.lua_id[0] ? r.lua_id : NULL);
}

static void close_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < s_n) remove_at(i);
    lv_async_call(rebuild_async, NULL);   /* don't delete the button inside its own click */
}

static void close_all_cb(lv_event_t *e)
{
    while (s_n) remove_at(0);
    ui_toast("All apps closed");
    lv_async_call(rebuild_async, NULL);
}

static void build_list(void)
{
    if (!s_list) return;
    lv_obj_clean(s_list);
    memset(s_status, 0, sizeof(s_status));
    if (s_n == 0) {
        lv_obj_t *l = ui_label(s_list, "No recent apps", &lv_font_montserrat_20, UI_MUTED);
        lv_obj_set_style_pad_top(l, 120, 0);
        lv_obj_set_width(l, lv_pct(100));
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
        return;
    }
    char st[40];
    for (int i = 0; i < s_n; i++) {
        recent_t *r = &s_rec[i];
        lv_obj_t *card = ui_card(s_list);
        lv_obj_set_size(card, lv_pct(100), 84);
        lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_opa(card, LV_OPA_70, LV_STATE_PRESSED);
        lv_obj_add_event_cb(card, open_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        lv_obj_t *ic = lv_obj_create(card);
        lv_obj_remove_style_all(ic);
        lv_obj_set_size(ic, 56, 56);
        lv_obj_align(ic, LV_ALIGN_LEFT_MID, -4, 0);
        lv_obj_set_style_radius(ic, 16, 0);
        lv_obj_set_style_clip_corner(ic, true, 0);
        lv_obj_set_style_bg_opa(ic, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(ic, lv_color_hex(r->color), 0);
        lv_obj_remove_flag(ic, LV_OBJ_FLAG_CLICKABLE);
        if (r->icon_img) {
            lv_obj_t *img = lv_image_create(ic);
            lv_image_set_src(img, r->icon);
            lv_image_set_scale(img, 180);
            lv_obj_center(img);
        } else {
            lv_obj_center(ui_label(ic, r->icon, &lv_font_montserrat_24, UI_TEXT));
        }

        lv_obj_t *name = ui_label(card, r->name, &lv_font_montserrat_20, UI_TEXT);
        status_of(r, st, sizeof(st));
        lv_obj_align(name, LV_ALIGN_LEFT_MID, 66, st[0] ? -13 : 0);   /* centred if nothing runs */
        s_status[i] = ui_label(card, st, &lv_font_montserrat_16, UI_GREEN);
        lv_obj_align(s_status[i], LV_ALIGN_LEFT_MID, 66, 15);
        lv_obj_t *x = ui_round_button(card, LV_SYMBOL_CLOSE, 48, UI_CARD2, close_cb, (void *)(intptr_t)i);
        lv_obj_align(x, LV_ALIGN_RIGHT_MID, 6, 0);
    }
    lv_obj_t *b = ui_button(s_list, "Close all", UI_RED, close_all_cb, NULL);
    lv_obj_set_width(b, lv_pct(100));
}

static void refresh(void *ctx)
{
    char st[40];
    for (int i = 0; i < s_n; i++) {      /* only the status text changes: no rebuild */
        if (s_status[i]) {
            status_of(&s_rec[i], st, sizeof(st));
            if (strcmp(lv_label_get_text(s_status[i]), st) != 0) lv_label_set_text(s_status[i], st);
        }
    }
}

static void rebuild_async(void *arg) { build_list(); }

static void recents_build(lv_obj_t *c, void *ctx)
{
    s_list = c;
    build_list();
    ui_page_set_refresh(refresh);
}

static void recents_close(void *ctx) { s_list = NULL; }

void ui_open_recents(void)
{
    ui_page_push("Recent", recents_build, recents_close, NULL);
}
