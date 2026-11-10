/*
 * Music (SD card MP3/WAV), Phone music remote (Gadgetbridge), Voice recorder.
 */
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "apps.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "audio.h"
#include "file_manager.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "phone.h"
#include "settings.h"
#include "ui.h"

/* ======================================================================== music library */

#define MAX_TRACKS 200

static char  *s_tracks[MAX_TRACKS];
static int    s_track_n;
static int    s_track_cur = -1;
static bool   s_shuffle;
static TaskHandle_t s_music_task;
static int64_t s_track_start_us;     /* when the current track was started */
static int    s_quick_fails;         /* tracks in a row that ended right away (undecodable) */

static bool is_audio(const char *n)
{
    const char *d = strrchr(n, '.');
    return d && (!strcasecmp(d, ".mp3") || !strcasecmp(d, ".wav"));
}

static int track_cmp(const void *a, const void *b)
{
    return strcasecmp(strrchr(*(char *const *)a, '/'), strrchr(*(char *const *)b, '/'));
}

static void scan_dir(const char *dir, int depth)
{
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    char p[FM_MAX_PATH];
    while ((e = readdir(d)) != NULL && s_track_n < MAX_TRACKS) {
        if (e->d_name[0] == '.') continue;
        snprintf(p, sizeof(p), "%s/%s", dir, e->d_name);
        if (e->d_type == DT_DIR) {
            if (depth < 2 && strcasecmp(e->d_name, "Recordings") != 0) scan_dir(p, depth + 1);
        } else if (is_audio(e->d_name)) {
            s_tracks[s_track_n++] = strdup(p);
        }
    }
    closedir(d);
}

static void library_scan(void)
{
    for (int i = 0; i < s_track_n; i++) free(s_tracks[i]);
    s_track_n = 0;
    if (fm_is_mounted()) scan_dir("/sdcard", 0);
    qsort(s_tracks, s_track_n, sizeof(char *), track_cmp);
}

static void play_index(int i)
{
    if (i < 0 || i >= s_track_n) return;
    s_track_cur = i;
    s_track_start_us = esp_timer_get_time();
    if (!player_play(s_tracks[i])) {
        char msg[96];
        snprintf(msg, sizeof(msg), "Can't play %s", strrchr(s_tracks[i], '/') + 1);
        ui_toast(msg);
    }
    ui_on_data_changed();
}

static void next_track(void)
{
    if (s_track_n == 0) return;
    int n = s_shuffle ? (int)(esp_random() % s_track_n) : (s_track_cur + 1) % s_track_n;
    play_index(n);
}

/* the player calls this from its own task: hand over to ours */
static void on_track_done(void)
{
    if (s_music_task) xTaskNotifyGive(s_music_task);
}

static void music_task(void *arg)
{
    while (true) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        vTaskDelay(pdMS_TO_TICKS(200));
        /* a track that "finishes" within 2 s most likely couldn't be decoded: don't spin
           through a library of broken files forever */
        if (esp_timer_get_time() - s_track_start_us < 2000000) s_quick_fails++;
        else s_quick_fails = 0;
        if (s_quick_fails >= 3 || s_quick_fails >= s_track_n) {
            s_quick_fails = 0;
            ui_toast("Can't play this file");
            ui_on_data_changed();
            continue;
        }
        next_track();
    }
}

static void music_init(void)
{
    if (!s_music_task) {
        xTaskCreate(music_task, "music", 4096, NULL, 4, &s_music_task);
        player_set_done_cb(on_track_done);
    }
}

/* ---- now playing ---- */

static lv_obj_t *s_np_title, *s_np_time, *s_np_play, *s_np_shuffle;

static void np_refresh(void *ctx)
{
    if (!s_np_title) return;
    const char *cur = player_current();
    const char *name = cur[0] ? strrchr(cur, '/') + 1 : "Nothing playing";
    lv_label_set_text(s_np_title, name);
    int t = player_elapsed_s();
    char b[16];
    snprintf(b, sizeof(b), "%d:%02d", t / 60, t % 60);
    lv_label_set_text(s_np_time, player_state() == PLAYER_IDLE ? "" : b);
    lv_label_set_text(lv_obj_get_child(s_np_play, 0), player_state() == PLAYER_PLAYING ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
    lv_obj_set_style_bg_color(s_np_shuffle, lv_color_hex(s_shuffle ? UI_PINK : UI_CARD2), 0);
}

static void np_play(lv_event_t *e)
{
    player_state_t st = player_state();
    if (st == PLAYER_PLAYING) player_pause();
    else if (st == PLAYER_PAUSED) player_resume();
    else if (s_track_cur >= 0) play_index(s_track_cur);
    else if (s_track_n) play_index(0);
    np_refresh(NULL);
}
static void np_next(lv_event_t *e) { next_track(); np_refresh(NULL); }
static void np_prev(lv_event_t *e)
{
    if (s_track_n == 0) return;
    if (player_elapsed_s() > 3) play_index(s_track_cur);
    else play_index((s_track_cur - 1 + s_track_n) % s_track_n);
    np_refresh(NULL);
}
static void np_shuffle(lv_event_t *e) { s_shuffle = !s_shuffle; np_refresh(NULL); }
static void np_stop(lv_event_t *e) { player_stop(); np_refresh(NULL); }
static void np_vol(lv_event_t *e)
{
    settings_t *s = settings_edit();
    s->volume = lv_slider_get_value(lv_event_get_target(e));
    settings_commit();
}

static void np_build(lv_obj_t *c, void *ctx)
{
    lv_obj_t *art = lv_obj_create(c);
    lv_obj_remove_style_all(art);
    lv_obj_set_size(art, 150, 150);
    lv_obj_align(art, LV_ALIGN_TOP_MID, 0, 6);
    lv_obj_set_style_radius(art, 30, 0);
    lv_obj_set_style_bg_opa(art, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(art, lv_color_hex(UI_PINK), 0);
    lv_obj_set_style_bg_grad_color(art, lv_color_hex(UI_PURPLE), 0);
    lv_obj_set_style_bg_grad_dir(art, LV_GRAD_DIR_VER, 0);
    lv_obj_t *n = ui_label(art, LV_SYMBOL_AUDIO, &lv_font_montserrat_48, UI_TEXT);
    lv_obj_center(n);
    s_np_title = ui_label(c, "", &lv_font_montserrat_20, UI_TEXT);
    lv_label_set_long_mode(s_np_title, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_width(s_np_title, UI_W - 40);
    lv_obj_set_style_text_align(s_np_title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_np_title, LV_ALIGN_TOP_MID, 0, 170);
    s_np_time = ui_label(c, "", &lv_font_montserrat_16, UI_MUTED);
    lv_obj_align(s_np_time, LV_ALIGN_TOP_MID, 0, 200);

    lv_obj_t *prev = ui_round_button(c, LV_SYMBOL_PREV, 64, UI_CARD2, np_prev, NULL);
    lv_obj_align(prev, LV_ALIGN_TOP_MID, -110, 236);
    s_np_play = ui_round_button(c, LV_SYMBOL_PLAY, 84, UI_PINK, np_play, NULL);
    lv_obj_align(s_np_play, LV_ALIGN_TOP_MID, 0, 226);
    lv_obj_t *next = ui_round_button(c, LV_SYMBOL_NEXT, 64, UI_CARD2, np_next, NULL);
    lv_obj_align(next, LV_ALIGN_TOP_MID, 110, 236);
    s_np_shuffle = ui_round_button(c, LV_SYMBOL_SHUFFLE, 44, UI_CARD2, np_shuffle, NULL);
    lv_obj_align(s_np_shuffle, LV_ALIGN_TOP_MID, -150, 330);
    lv_obj_t *stop = ui_round_button(c, LV_SYMBOL_STOP, 44, UI_CARD2, np_stop, NULL);
    lv_obj_align(stop, LV_ALIGN_TOP_MID, 150, 330);

    lv_obj_t *sl = lv_slider_create(c);
    lv_obj_set_size(sl, 200, 10);
    lv_obj_align(sl, LV_ALIGN_TOP_MID, 0, 352);
    lv_slider_set_range(sl, 0, 100);
    lv_slider_set_value(sl, settings()->volume, LV_ANIM_OFF);
    lv_obj_set_ext_click_area(sl, 16);
    lv_obj_add_event_cb(sl, np_vol, LV_EVENT_RELEASED, NULL);
    lv_obj_t *vl = ui_label(c, LV_SYMBOL_VOLUME_MID, &lv_font_montserrat_16, UI_MUTED);
    lv_obj_align_to(vl, sl, LV_ALIGN_OUT_TOP_MID, 0, -6);
    np_refresh(NULL);
    ui_page_set_refresh(np_refresh);
}

static void np_close(void *ctx) { s_np_title = NULL; }
static void open_now_playing(void) { ui_page_push_raw("Now playing", np_build, np_close, NULL); }

static void track_click(lv_event_t *e)
{
    play_index((int)(intptr_t)lv_event_get_user_data(e));
    open_now_playing();
}

static void np_open_click(lv_event_t *e) { open_now_playing(); }
static void scan_work(void *ctx) { library_scan(); }
static void rescan_done(void *ctx) { ui_page_rebuild(); }
static void rescan_click(lv_event_t *e) { ui_run_bg("Finding songs...", scan_work, rescan_done, NULL); }

static void music_build(lv_obj_t *c, void *ctx)
{
    if (player_state() != PLAYER_IDLE) {
        ui_row(c, LV_SYMBOL_PLAY, UI_PINK, "Now playing", strrchr(player_current(), '/') + 1, np_open_click, NULL);
    }
    if (s_track_n == 0) {
        lv_obj_t *l = ui_label(c, LV_SYMBOL_AUDIO "\n\nNo music found.\nCopy MP3 or WAV files to the SD card\n"
                                  "(e.g. a \"Music\" folder) with the web manager.",
                               &lv_font_montserrat_18, UI_MUTED);
        lv_obj_set_width(l, lv_pct(100));
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_pad_top(l, 40, 0);
    }
    for (int i = 0; i < s_track_n; i++) {
        const char *name = strrchr(s_tracks[i], '/') + 1;
        ui_row(c, i == s_track_cur ? LV_SYMBOL_PLAY : LV_SYMBOL_AUDIO, i == s_track_cur ? UI_PINK : UI_CARD2,
               name, NULL, track_click, (void *)(intptr_t)i);
    }
    ui_row(c, LV_SYMBOL_REFRESH, UI_CARD2, "Rescan SD card", NULL, rescan_click, NULL);
}

static void music_open_done(void *ctx) { ui_page_push("Music", music_build, NULL, NULL); }

void app_music_open(void)
{
    music_init();
    ui_run_bg("Finding songs...", scan_work, music_open_done, NULL);   /* SD scan off the UI task */
}

static char s_open_path[FM_MAX_PATH];

static void open_file_done(void *ctx)
{
    const char *abs_path = s_open_path;
    for (int i = 0; i < s_track_n; i++) {
        if (strcmp(s_tracks[i], abs_path) == 0) {
            play_index(i);
            open_now_playing();
            return;
        }
    }
    if (!player_play(abs_path)) {          /* outside the library (e.g. a recording) */
        ui_toast("Can't play this file");
        return;
    }
    open_now_playing();
}

void music_open_file(const char *abs_path)
{
    music_init();
    strlcpy(s_open_path, abs_path, sizeof(s_open_path));
    ui_run_bg("Opening...", scan_work, open_file_done, NULL);
}

/* ======================================================================== phone music */

static lv_obj_t *s_pm_track, *s_pm_artist, *s_pm_play, *s_pm_state;

static void pm_refresh(void *ctx)
{
    if (!s_pm_track) return;
    phone_music_t m;
    phone_music_get(&m);
    lv_label_set_text(s_pm_track, m.valid && m.track[0] ? m.track : "No music info");
    lv_label_set_text(s_pm_artist, m.artist);
    lv_label_set_text(lv_obj_get_child(s_pm_play, 0), m.playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
    lv_label_set_text(s_pm_state, phone_connected() ? "" : "Phone not connected (Gadgetbridge)");
}

static void pm_cmd(lv_event_t *e)
{
    const char *cmd = lv_event_get_user_data(e);
    if (strcmp(cmd, "playpause") == 0) {
        phone_music_t m;
        phone_music_get(&m);
        phone_music_cmd(m.playing ? "pause" : "play");
    } else {
        phone_music_cmd(cmd);
    }
}

static void pm_build(lv_obj_t *c, void *ctx)
{
    lv_obj_t *ic = ui_label(c, LV_SYMBOL_AUDIO, &lv_font_montserrat_48, UI_PINK);
    lv_obj_align(ic, LV_ALIGN_TOP_MID, 0, 20);
    s_pm_track = ui_label(c, "", &lv_font_montserrat_24, UI_TEXT);
    lv_label_set_long_mode(s_pm_track, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_width(s_pm_track, UI_W - 40);
    lv_obj_set_style_text_align(s_pm_track, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_pm_track, LV_ALIGN_TOP_MID, 0, 100);
    s_pm_artist = ui_label(c, "", &lv_font_montserrat_18, UI_MUTED);
    lv_obj_align(s_pm_artist, LV_ALIGN_TOP_MID, 0, 140);
    lv_obj_t *p = ui_round_button(c, LV_SYMBOL_PREV, 64, UI_CARD2, pm_cmd, "previous");
    lv_obj_align(p, LV_ALIGN_TOP_MID, -110, 196);
    s_pm_play = ui_round_button(c, LV_SYMBOL_PLAY, 84, UI_PINK, pm_cmd, "playpause");
    lv_obj_align(s_pm_play, LV_ALIGN_TOP_MID, 0, 186);
    lv_obj_t *n = ui_round_button(c, LV_SYMBOL_NEXT, 64, UI_CARD2, pm_cmd, "next");
    lv_obj_align(n, LV_ALIGN_TOP_MID, 110, 196);
    lv_obj_t *vd = ui_round_button(c, LV_SYMBOL_VOLUME_MID, 56, UI_CARD2, pm_cmd, "volumedown");
    lv_obj_align(vd, LV_ALIGN_TOP_MID, -60, 300);
    lv_obj_t *vu = ui_round_button(c, LV_SYMBOL_VOLUME_MAX, 56, UI_CARD2, pm_cmd, "volumeup");
    lv_obj_align(vu, LV_ALIGN_TOP_MID, 60, 300);
    s_pm_state = ui_label(c, "", &lv_font_montserrat_14, UI_MUTED);
    lv_obj_align(s_pm_state, LV_ALIGN_BOTTOM_MID, 0, -16);
    pm_refresh(NULL);
    ui_page_set_refresh(pm_refresh);
}

static void pm_close(void *ctx) { s_pm_track = NULL; }
void app_phone_music_open(void) { ui_page_push_raw("Phone music", pm_build, pm_close, NULL); }

/* ======================================================================== recorder */

#define REC_DIR "/sdcard/Recordings"

static lv_obj_t *s_rec_time, *s_rec_bar, *s_rec_btn;

static void rec_refresh_ui(lv_timer_t *t)
{
    if (!s_rec_time) return;
    int s = rec_seconds();
    char b[16];
    snprintf(b, sizeof(b), "%02d:%02d", s / 60, s % 60);
    lv_label_set_text(s_rec_time, b);
    lv_bar_set_value(s_rec_bar, rec_level(), LV_ANIM_ON);
    lv_label_set_text(lv_obj_get_child(s_rec_btn, 0), rec_active() ? LV_SYMBOL_STOP : "");
    lv_obj_set_style_radius(s_rec_btn, rec_active() ? 20 : LV_RADIUS_CIRCLE, 0);
}

static lv_timer_t *s_rec_timer;

static void rec_toggle(lv_event_t *e)
{
    if (rec_active()) {
        rec_stop();
        ui_toast("Saved to Recordings");
        ui_keep_awake(false);
        ui_page_rebuild();
        return;
    }
    if (!fm_is_mounted()) {
        ui_toast("No SD card");
        return;
    }
    mkdir(REC_DIR, 0775);
    char path[96];
    time_t now = time(NULL);
    struct tm lt;
    localtime_r(&now, &lt);
    snprintf(path, sizeof(path), REC_DIR "/rec_%04d%02d%02d_%02d%02d%02d.wav", lt.tm_year + 1900, lt.tm_mon + 1,
             lt.tm_mday, lt.tm_hour, lt.tm_min, lt.tm_sec);
    if (rec_start(path)) {
        ui_keep_awake(true);
    } else {
        ui_toast("Could not start recording");
    }
}

static char s_rec_sel[FM_MAX_PATH];
static void rec_del(void *u) { unlink(s_rec_sel); ui_page_rebuild(); }

static void rec_item_action(int i, void *u)
{
    if (i == 0) {
        music_open_file(s_rec_sel);
    } else if (i == 1) {
        ui_confirm("Delete", "Delete this recording?", "Delete", UI_RED, rec_del, NULL);
    }
}

static void rec_item_click(lv_event_t *e)
{
    snprintf(s_rec_sel, sizeof(s_rec_sel), REC_DIR "/%s", (const char *)lv_obj_get_user_data(lv_event_get_current_target(e)));
    static const char *acts[] = { "Play", "Delete" };
    ui_choose(strrchr(s_rec_sel, '/') + 1, acts, 2, -1, rec_item_action, NULL);
}

static void rec_free_name(lv_event_t *e) { lv_free(lv_obj_get_user_data(lv_event_get_current_target(e))); }

static void rec_build(lv_obj_t *c, void *ctx)
{
    lv_obj_t *top = lv_obj_create(c);
    lv_obj_remove_style_all(top);
    lv_obj_set_size(top, lv_pct(100), 230);
    lv_obj_remove_flag(top, LV_OBJ_FLAG_SCROLLABLE);
    s_rec_time = ui_label(top, "00:00", &lv_font_montserrat_48, UI_TEXT);
    lv_obj_align(s_rec_time, LV_ALIGN_TOP_MID, 0, 0);
    s_rec_bar = lv_bar_create(top);
    lv_obj_set_size(s_rec_bar, 260, 10);
    lv_obj_align(s_rec_bar, LV_ALIGN_TOP_MID, 0, 70);
    lv_obj_set_style_bg_color(s_rec_bar, lv_color_hex(UI_RED), LV_PART_INDICATOR);
    s_rec_btn = ui_round_button(top, "", 110, UI_RED, rec_toggle, NULL);
    lv_obj_set_style_border_width(s_rec_btn, 6, 0);
    lv_obj_set_style_border_color(s_rec_btn, lv_color_white(), 0);
    lv_obj_align(s_rec_btn, LV_ALIGN_TOP_MID, 0, 100);

    ui_section(c, "RECORDINGS");
    DIR *d = opendir(REC_DIR);
    int n = 0;
    if (d) {
        struct dirent *e;
        char *names[100];
        while ((e = readdir(d)) != NULL && n < 100) {
            if (e->d_name[0] != '.' && is_audio(e->d_name)) names[n++] = strdup(e->d_name);
        }
        closedir(d);
        /* newest first (names contain the date) */
        for (int i = 0; i < n; i++) for (int j = i + 1; j < n; j++) {
            if (strcmp(names[j], names[i]) > 0) { char *t = names[i]; names[i] = names[j]; names[j] = t; }
        }
        char path[FM_MAX_PATH], sz[16];
        for (int i = 0; i < n; i++) {
            struct stat st;
            snprintf(path, sizeof(path), REC_DIR "/%s", names[i]);
            int secs = stat(path, &st) == 0 ? (int)((st.st_size - 44) / 32000) : 0;
            snprintf(sz, sizeof(sz), "%d:%02d", secs / 60, secs % 60);
            lv_obj_t *r = ui_row(c, LV_SYMBOL_AUDIO, UI_RED, names[i], sz, rec_item_click, NULL);
            char *nm = lv_malloc(strlen(names[i]) + 1);
            strcpy(nm, names[i]);
            lv_obj_set_user_data(r, nm);
            lv_obj_add_event_cb(r, rec_free_name, LV_EVENT_DELETE, NULL);
            free(names[i]);
        }
    }
    if (!n) ui_label(c, "  No recordings yet - tap the red button", &lv_font_montserrat_16, UI_MUTED);
    if (!s_rec_timer) s_rec_timer = lv_timer_create(rec_refresh_ui, 150, NULL);
    rec_refresh_ui(NULL);
}

static void rec_close(void *ctx)
{
    s_rec_time = NULL;
    if (s_rec_timer) {
        lv_timer_delete(s_rec_timer);
        s_rec_timer = NULL;
    }
    if (rec_active()) {
        rec_stop();
        ui_keep_awake(false);
    }
}

void app_recorder_open(void) { ui_page_push("Recorder", rec_build, rec_close, NULL); }
