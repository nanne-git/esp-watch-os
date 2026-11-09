/*
 * Gallery (JPEG via esp_jpeg with down-scaling, PNG/BMP via LVGL) and
 * Video player (MJPEG: raw .mjpeg or MJPEG-in-AVI; no audio).
 */
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "apps.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "file_manager.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "jpeg_decoder.h"
#include "ui.h"

#define MAX_FILES 300

static bool ext_in(const char *n, const char *const *exts)
{
    const char *d = strrchr(n, '.');
    if (!d) return false;
    for (int i = 0; exts[i]; i++) {
        if (!strcasecmp(d, exts[i])) return true;
    }
    return false;
}

static int path_cmp(const void *a, const void *b)
{
    return strcasecmp(*(char *const *)a, *(char *const *)b);
}

static void scan(const char *dir, int depth, const char *const *exts, char **out, int *n)
{
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    char p[FM_MAX_PATH];
    while ((e = readdir(d)) != NULL && *n < MAX_FILES) {
        if (e->d_name[0] == '.') continue;
        snprintf(p, sizeof(p), "%s/%s", dir, e->d_name);
        if (e->d_type == DT_DIR) {
            if (depth < 2) scan(p, depth + 1, exts, out, n);
        } else if (ext_in(e->d_name, exts)) {
            out[(*n)++] = strdup(p);
        }
    }
    closedir(d);
}

/* Decode a JPEG file to RGB565 in PSRAM, picking a scale so it fits around max_w x max_h. */
static uint8_t *decode_jpeg_file(const char *path, int max_w, int max_h, int *w, int *h)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0 || size > 8 * 1024 * 1024) {
        fclose(f);
        return NULL;
    }
    uint8_t *in = heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
    if (!in) {
        fclose(f);
        return NULL;
    }
    size_t rd = fread(in, 1, size, f);
    fclose(f);
    esp_jpeg_image_cfg_t cfg = { .indata = in, .indata_size = rd, .out_format = JPEG_IMAGE_FORMAT_RGB565 };
    esp_jpeg_image_output_t info;
    if (esp_jpeg_get_image_info(&cfg, &info) != ESP_OK) {
        free(in);
        return NULL;
    }
    int scale = 0;
    while (scale < 3 && (info.width >> scale) > max_w * 2 && (info.height >> scale) > max_h * 2) scale++;
    while (scale < 3 && ((info.width >> scale) > 1600 || (info.height >> scale) > 1600)) scale++;
    int ow = info.width >> scale, oh = info.height >> scale;
    size_t out_size = (size_t)(ow + 16) * (oh + 16) * 2;
    uint8_t *out = heap_caps_malloc(out_size, MALLOC_CAP_SPIRAM);
    if (!out) {
        free(in);
        return NULL;
    }
    cfg.indata = in;
    cfg.indata_size = rd;
    cfg.outbuf = out;
    cfg.outbuf_size = out_size;
    cfg.out_scale = (esp_jpeg_image_scale_t)scale;
    cfg.flags.swap_color_bytes = 0;
    esp_jpeg_image_output_t img;
    esp_err_t err = esp_jpeg_decode(&cfg, &img);
    free(in);
    if (err != ESP_OK) {
        free(out);
        return NULL;
    }
    *w = img.width;
    *h = img.height;
    return out;
}

/* ======================================================================== gallery */

static char *s_imgs[MAX_FILES];
static int   s_img_n, s_img_cur;
static lv_obj_t *s_view_img, *s_view_info;
static lv_image_dsc_t s_view_dsc;
static uint8_t *s_view_buf;
static char s_view_src[FM_MAX_PATH + 4];

static void gallery_scan(void)
{
    static const char *exts[] = { ".jpg", ".jpeg", ".png", ".bmp", NULL };
    for (int i = 0; i < s_img_n; i++) free(s_imgs[i]);
    s_img_n = 0;
    if (fm_is_mounted()) scan("/sdcard", 0, exts, s_imgs, &s_img_n);
    qsort(s_imgs, s_img_n, sizeof(char *), path_cmp);
}

static void view_show(int i)
{
    if (i < 0 || i >= s_img_n || !s_view_img) return;
    s_img_cur = i;
    const char *p = s_imgs[i];
    static const char *jpg[] = { ".jpg", ".jpeg", NULL };
    lv_image_set_src(s_view_img, NULL);
    if (s_view_buf) {
        free(s_view_buf);
        s_view_buf = NULL;
    }
    char info[96];
    if (ext_in(p, jpg)) {
        int w = 0, h = 0;
        s_view_buf = decode_jpeg_file(p, UI_W, UI_H - 64, &w, &h);
        if (s_view_buf) {
            memset(&s_view_dsc, 0, sizeof(s_view_dsc));
            s_view_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
            s_view_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
            s_view_dsc.header.w = w;
            s_view_dsc.header.h = h;
            s_view_dsc.header.stride = w * 2;
            s_view_dsc.data = s_view_buf;
            s_view_dsc.data_size = w * h * 2;
            lv_image_set_src(s_view_img, &s_view_dsc);
            /* fit inside the screen */
            int sx = UI_W * 256 / w, sy = (UI_H - 64) * 256 / h;
            int sc = sx < sy ? sx : sy;
            lv_image_set_scale(s_view_img, sc > 256 ? 256 : sc);
            snprintf(info, sizeof(info), "%d / %d   %s", i + 1, s_img_n, strrchr(p, '/') + 1);
        } else {
            snprintf(info, sizeof(info), "Can't open %s", strrchr(p, '/') + 1);
        }
    } else {
        snprintf(s_view_src, sizeof(s_view_src), "S:%s", p);
        lv_image_set_src(s_view_img, s_view_src);
        lv_image_set_scale(s_view_img, 256);
        lv_image_header_t hd;
        if (lv_image_decoder_get_info(s_view_src, &hd) == LV_RESULT_OK && hd.w && hd.h) {
            int sx = UI_W * 256 / hd.w, sy = (UI_H - 64) * 256 / hd.h;
            int sc = sx < sy ? sx : sy;
            lv_image_set_scale(s_view_img, sc > 256 ? 256 : sc);
        }
        snprintf(info, sizeof(info), "%d / %d   %s", i + 1, s_img_n, strrchr(p, '/') + 1);
    }
    lv_label_set_text(s_view_info, info);
}

static void view_nav(lv_event_t *e)
{
    int d = (int)(intptr_t)lv_event_get_user_data(e);
    if (s_img_n) view_show((s_img_cur + d + s_img_n) % s_img_n);
}

static void view_gesture(lv_event_t *e)
{
    lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_active());
    if (dir == LV_DIR_LEFT && s_img_n) {
        lv_indev_wait_release(lv_indev_active());
        view_show((s_img_cur + 1) % s_img_n);
    }
}

static void view_build(lv_obj_t *c, void *ctx)
{
    lv_obj_set_style_bg_color(c, lv_color_black(), 0);
    s_view_img = lv_image_create(c);
    lv_obj_center(s_view_img);
    lv_image_set_inner_align(s_view_img, LV_IMAGE_ALIGN_CENTER);
    s_view_info = ui_label(c, "", &lv_font_montserrat_14, 0xC7C7CC);
    lv_label_set_long_mode(s_view_info, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_view_info, UI_W - 150);
    lv_obj_align(s_view_info, LV_ALIGN_BOTTOM_MID, 0, -14);
    lv_obj_t *p = ui_round_button(c, LV_SYMBOL_LEFT, 50, UI_CARD2, view_nav, (void *)-1);
    lv_obj_set_style_bg_opa(p, LV_OPA_70, 0);
    lv_obj_align(p, LV_ALIGN_BOTTOM_LEFT, 12, -6);
    lv_obj_t *n = ui_round_button(c, LV_SYMBOL_RIGHT, 50, UI_CARD2, view_nav, (void *)1);
    lv_obj_set_style_bg_opa(n, LV_OPA_70, 0);
    lv_obj_align(n, LV_ALIGN_BOTTOM_RIGHT, -12, -6);
    lv_obj_add_event_cb(lv_obj_get_screen(c), view_gesture, LV_EVENT_GESTURE, NULL);
    view_show(s_img_cur);
}

static void view_close(void *ctx)
{
    s_view_img = NULL;
    if (s_view_buf) {
        free(s_view_buf);
        s_view_buf = NULL;
    }
}

static void open_viewer(int i)
{
    s_img_cur = i;
    ui_page_push_raw("Photo", view_build, view_close, NULL);
}

static void thumb_click(lv_event_t *e) { open_viewer((int)(intptr_t)lv_event_get_user_data(e)); }

static void gallery_build(lv_obj_t *c, void *ctx)
{
    if (!s_img_n) {
        lv_obj_t *l = ui_label(c, LV_SYMBOL_IMAGE "\n\nNo photos found.\nCopy JPG or PNG files to the SD card\nwith the web manager.",
                               &lv_font_montserrat_18, UI_MUTED);
        lv_obj_set_width(l, lv_pct(100));
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_pad_top(l, 40, 0);
    }
    for (int i = 0; i < s_img_n; i++) {
        ui_row(c, LV_SYMBOL_IMAGE, UI_ORANGE, strrchr(s_imgs[i], '/') + 1, NULL, thumb_click, (void *)(intptr_t)i);
    }
}

void app_gallery_open(void)
{
    gallery_scan();
    ui_page_push("Gallery", gallery_build, NULL, NULL);
}

void gallery_open_file(const char *abs_path)
{
    gallery_scan();
    for (int i = 0; i < s_img_n; i++) {
        if (strcmp(s_imgs[i], abs_path) == 0) {
            open_viewer(i);
            return;
        }
    }
    if (s_img_n < MAX_FILES) {
        s_imgs[s_img_n] = strdup(abs_path);
        open_viewer(s_img_n++);
    }
}

/* ======================================================================== video (MJPEG) */

typedef struct {
    char          path[FM_MAX_PATH];
    volatile bool run, paused;
    TaskHandle_t  task;
    lv_obj_t     *img, *info, *play;
    lv_image_dsc_t dsc[2];
    uint8_t      *frame[2];
    int           frames, fps10;
} video_t;

static video_t *s_vid;

static int avi_frame_us(FILE *f)
{
    uint8_t h[256];
    fseek(f, 0, SEEK_SET);
    size_t n = fread(h, 1, sizeof(h), f);
    for (size_t i = 0; i + 8 < n; i++) {
        if (!memcmp(h + i, "avih", 4)) {
            return *(uint32_t *)(h + i + 8);         /* dwMicroSecPerFrame */
        }
    }
    return 0;
}

static void video_task(void *arg)
{
    video_t *v = arg;
    FILE *f = fopen(v->path, "rb");
    int us = f ? avi_frame_us(f) : 0;
    if (us < 20000 || us > 500000) us = 66666;          /* default 15 fps */
    if (f) fseek(f, 0, SEEK_SET);
    const size_t cap = 512 * 1024;
    uint8_t *buf = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM);
    size_t len = 0;
    int which = 0;
    int64_t next = esp_timer_get_time();
    while (v->run && f && buf) {
        if (v->paused) {
            vTaskDelay(pdMS_TO_TICKS(50));
            next = esp_timer_get_time();
            continue;
        }
        size_t r = fread(buf + len, 1, cap - len, f);
        len += r;
        if (len == 0) break;
        /* find SOI .. EOI */
        size_t s = 0;
        while (s + 1 < len && !(buf[s] == 0xFF && buf[s + 1] == 0xD8)) s++;
        size_t e = s + 2;
        while (e + 1 < len && !(buf[e] == 0xFF && buf[e + 1] == 0xD9)) e++;
        if (e + 1 >= len) {
            if (r == 0 || s >= len) break;                /* end of file */
            if (s > 0) { memmove(buf, buf + s, len - s); len -= s; continue; }
            break;                                         /* frame larger than buffer */
        }
        e += 2;
        esp_jpeg_image_cfg_t cfg = { .indata = buf + s, .indata_size = e - s, .out_format = JPEG_IMAGE_FORMAT_RGB565 };
        esp_jpeg_image_output_t info;
        if (esp_jpeg_get_image_info(&cfg, &info) == ESP_OK) {
            int scale = 0;
            while (scale < 3 && ((info.width >> scale) > UI_W || (info.height >> scale) > UI_H - 64)) scale++;
            int w = info.width >> scale, hh = info.height >> scale;
            size_t need = (size_t)(w + 16) * (hh + 16) * 2;
            if (!v->frame[which] || v->dsc[which].data_size < need) {
                free(v->frame[which]);
                v->frame[which] = heap_caps_malloc(need, MALLOC_CAP_SPIRAM);
                v->dsc[which].data_size = need;
            }
            if (v->frame[which]) {
                cfg.indata = buf + s;
                cfg.indata_size = e - s;
                cfg.outbuf = v->frame[which];
                cfg.outbuf_size = need;
                cfg.out_scale = (esp_jpeg_image_scale_t)scale;
                esp_jpeg_image_output_t img;
                if (esp_jpeg_decode(&cfg, &img) == ESP_OK && ui_lock()) {
                    lv_image_dsc_t *d = &v->dsc[which];
                    d->header.magic = LV_IMAGE_HEADER_MAGIC;
                    d->header.cf = LV_COLOR_FORMAT_RGB565;
                    d->header.w = img.width;
                    d->header.h = img.height;
                    d->header.stride = img.width * 2;
                    d->data = v->frame[which];
                    if (v->img) {
                        lv_image_set_src(v->img, d);
                        lv_obj_invalidate(v->img);
                    }
                    v->frames++;
                    ui_unlock();
                    which ^= 1;
                }
            }
        }
        memmove(buf, buf + e, len - e);
        len -= e;
        next += us;
        int64_t wait = next - esp_timer_get_time();
        if (wait > 0) vTaskDelay(pdMS_TO_TICKS(wait / 1000));
        else next = esp_timer_get_time();          /* decoding slower than the video: no catch-up */
    }
    if (f) fclose(f);
    free(buf);
    v->run = false;
    v->task = NULL;
    if (ui_lock()) {
        if (v->info) lv_label_set_text(v->info, "Finished");
        ui_unlock();
    }
    vTaskDelete(NULL);
}

static void vid_play(lv_event_t *e)
{
    video_t *v = s_vid;
    if (!v) return;
    if (!v->run) {
        v->run = true;
        v->paused = false;
        lv_label_set_text(v->info, strrchr(v->path, '/') + 1);
        xTaskCreatePinnedToCore(video_task, "video", 8192, v, 5, &v->task, 0);
    } else {
        v->paused = !v->paused;
    }
    lv_label_set_text(lv_obj_get_child(v->play, 0), v->run && !v->paused ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
}

static void vid_build(lv_obj_t *c, void *ctx)
{
    video_t *v = s_vid;
    lv_obj_set_style_bg_color(c, lv_color_black(), 0);
    v->img = lv_image_create(c);
    lv_obj_align(v->img, LV_ALIGN_CENTER, 0, -20);
    v->info = ui_label(c, strrchr(v->path, '/') + 1, &lv_font_montserrat_14, UI_MUTED);
    lv_label_set_long_mode(v->info, LV_LABEL_LONG_DOT);
    lv_obj_set_width(v->info, UI_W - 120);
    lv_obj_align(v->info, LV_ALIGN_BOTTOM_LEFT, 16, -26);
    v->play = ui_round_button(c, LV_SYMBOL_PLAY, 60, UI_PURPLE, vid_play, NULL);
    lv_obj_align(v->play, LV_ALIGN_BOTTOM_RIGHT, -16, -8);
    ui_keep_awake(true);
    vid_play(NULL);
}

static void vid_close(void *ctx)
{
    video_t *v = s_vid;
    ui_keep_awake(false);
    if (!v) return;
    v->img = NULL;
    v->info = NULL;
    v->run = false;
    /* the task frees itself; wait for it before releasing buffers (we hold the UI lock) */
    for (int i = 0; i < 100 && v->task; i++) {
        ui_unlock();
        vTaskDelay(pdMS_TO_TICKS(20));
        ui_lock();
    }
    free(v->frame[0]);
    free(v->frame[1]);
    free(v);
    s_vid = NULL;
}

void video_open_file(const char *abs_path)
{
    static const char *mp4[] = { ".mp4", ".mov", ".mkv", NULL };
    if (ext_in(abs_path, mp4)) {
        ui_confirm("Convert first", "This chip has no video decoder for MP4. Convert on a computer:\n"
                   "ffmpeg -i in.mp4 -vf scale=410:-2,fps=15 -q:v 7 -an out.mjpeg",
                   "OK", UI_PURPLE, NULL, NULL);
        return;
    }
    if (s_vid) return;
    s_vid = calloc(1, sizeof(video_t));
    if (!s_vid) return;
    strlcpy(s_vid->path, abs_path, sizeof(s_vid->path));
    ui_page_push_raw("Video", vid_build, vid_close, NULL);
}

static char *s_vids[MAX_FILES];
static int   s_vid_n;

static void vid_item(lv_event_t *e) { video_open_file(s_vids[(int)(intptr_t)lv_event_get_user_data(e)]); }

static void videos_build(lv_obj_t *c, void *ctx)
{
    for (int i = 0; i < s_vid_n; i++) {
        static const char *mp4[] = { ".mp4", ".mov", ".mkv", NULL };
        ui_row(c, LV_SYMBOL_VIDEO, ext_in(s_vids[i], mp4) ? UI_CARD2 : UI_PURPLE, strrchr(s_vids[i], '/') + 1,
               ext_in(s_vids[i], mp4) ? "convert" : NULL, vid_item, (void *)(intptr_t)i);
    }
    lv_obj_t *card = ui_card(c);
    lv_obj_t *t = ui_label(card, "Plays MJPEG videos (.mjpeg / .avi), no sound. Convert phone videos with:\n"
                                 "ffmpeg -i in.mp4 -vf scale=410:-2,fps=15 -q:v 7 -an out.mjpeg",
                           &lv_font_montserrat_14, UI_MUTED);
    lv_label_set_long_mode(t, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(t, lv_pct(100));
}

void app_video_open(void)
{
    static const char *exts[] = { ".mjpeg", ".mjpg", ".avi", ".mp4", ".mov", ".mkv", NULL };
    for (int i = 0; i < s_vid_n; i++) free(s_vids[i]);
    s_vid_n = 0;
    if (fm_is_mounted()) scan("/sdcard", 0, exts, s_vids, &s_vid_n);
    qsort(s_vids, s_vid_n, sizeof(char *), path_cmp);
    ui_page_push("Videos", videos_build, NULL, NULL);
}
