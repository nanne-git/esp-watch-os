/*
 * File manager (on screen) + text viewer.
 * Each folder is its own page (ctx = malloc'd absolute path). A shared clipboard holds
 * one copied/cut path; "Paste here" in any folder copies or moves it in.
 */
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "apps.h"
#include "file_manager.h"
#include "ui.h"

#define MAX_ENTRIES 300

typedef struct {
    char    *name;
    bool     dir;
    uint32_t size;
} entry_t;

static char s_clip[FM_MAX_PATH];
static bool s_clip_cut;
static char s_sel[FM_MAX_PATH];          /* file the action menu is about */

static void folder_build(lv_obj_t *c, void *ctx);

static void fmt_size(uint64_t b, char *buf, size_t len)
{
    if (b < 1024) snprintf(buf, len, "%u B", (unsigned)b);
    else if (b < 1024 * 1024) snprintf(buf, len, "%.1f KB", b / 1024.0);
    else if (b < 1024ULL * 1024 * 1024) snprintf(buf, len, "%.1f MB", b / 1048576.0);
    else snprintf(buf, len, "%.2f GB", b / 1073741824.0);
}

static int entry_cmp(const void *a, const void *b)
{
    const entry_t *x = a, *y = b;
    if (x->dir != y->dir) return x->dir ? -1 : 1;
    return strcasecmp(x->name, y->name);
}

static const char *file_icon(const char *name, uint32_t *color)
{
    const char *dot = strrchr(name, '.');
    *color = UI_CARD2;
    if (!dot) return LV_SYMBOL_FILE;
    if (!strcasecmp(dot, ".jpg") || !strcasecmp(dot, ".jpeg") || !strcasecmp(dot, ".png") ||
        !strcasecmp(dot, ".bmp") || !strcasecmp(dot, ".webp")) { *color = UI_ORANGE; return LV_SYMBOL_IMAGE; }
    if (!strcasecmp(dot, ".mp3") || !strcasecmp(dot, ".wav")) { *color = UI_PINK; return LV_SYMBOL_AUDIO; }
    if (!strcasecmp(dot, ".mp4") || !strcasecmp(dot, ".avi") || !strcasecmp(dot, ".mjpeg") ||
        !strcasecmp(dot, ".mjpg")) { *color = UI_PURPLE; return LV_SYMBOL_VIDEO; }
    if (!strcasecmp(dot, ".txt") || !strcasecmp(dot, ".md") || !strcasecmp(dot, ".log") ||
        !strcasecmp(dot, ".json") || !strcasecmp(dot, ".csv")) { *color = UI_TEAL; return LV_SYMBOL_EDIT; }
    if (!strcasecmp(dot, ".bin")) { *color = UI_BLUE; return LV_SYMBOL_DOWNLOAD; }
    return LV_SYMBOL_FILE;
}

/* ------------------------------------------------------------------ navigation */

static void open_folder(const char *abs)
{
    char *path = strdup(abs);
    if (!path) return;
    const char *title = strcmp(abs, "/sdcard") == 0 ? "SD card" : strrchr(abs, '/') + 1;
    ui_page_push(title, folder_build, (page_fn_t)free, path);
}

void app_files_open(void)
{
    if (!fm_is_mounted() && !fm_mount()) {
        ui_toast("No SD card");
        return;
    }
    open_folder("/sdcard");
}

void app_files_open_path(const char *card_path)
{
    char abs[FM_MAX_PATH];
    if (fm_card_path(card_path, abs, sizeof(abs)) && fm_is_dir(abs)) {
        open_folder(abs);
    }
}

/* ------------------------------------------------------------------ actions */

static void after_change(void)
{
    ui_page_rebuild();
}

static void rename_done(const char *name, void *user)
{
    if (!name[0] || strchr(name, '/')) {
        ui_toast("Invalid name");
        return;
    }
    char dst[FM_MAX_PATH];
    const char *slash = strrchr(s_sel, '/');
    snprintf(dst, sizeof(dst), "%.*s/%s", (int)(slash - s_sel), s_sel, name);
    if (access(dst, F_OK) == 0) {
        ui_toast("That name already exists");
    } else if (rename(s_sel, dst) != 0) {
        ui_toast(strerror(errno));
    }
    after_change();
}

static void do_delete(void *u)
{
    if (fm_remove_recursive(s_sel) != 0) {
        ui_toast(strerror(errno));
    } else {
        ui_toast("Deleted");
    }
    after_change();
}

static void info_build(lv_obj_t *c, void *ctx)
{
    struct stat st;
    char b[32];
    ui_row(c, NULL, 0, "Name", strrchr(s_sel, '/') + 1, NULL, NULL);
    ui_row(c, NULL, 0, "Folder", fm_display_path(s_sel), NULL, NULL);
    if (stat(s_sel, &st) == 0) {
        if (S_ISDIR(st.st_mode)) {
            ui_row(c, NULL, 0, "Type", "Folder", NULL, NULL);
        } else {
            fmt_size(st.st_size, b, sizeof(b));
            ui_row(c, NULL, 0, "Size", b, NULL, NULL);
        }
        struct tm lt;
        localtime_r(&st.st_mtime, &lt);
        snprintf(b, sizeof(b), "%04d-%02d-%02d %02d:%02d", lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday,
                 lt.tm_hour, lt.tm_min);
        ui_row(c, NULL, 0, "Modified", b, NULL, NULL);
    }
}

static const char *ACTIONS_FILE[] = { "Open", "Copy", "Cut", "Rename", "Delete", "Info" };
static const char *ACTIONS_DIR[] = { "Open", "Copy", "Cut", "Rename", "Delete (with contents)", "Info" };

static void action_chosen(int i, void *u)
{
    switch (i) {
    case 0:
        if (fm_is_dir(s_sel)) open_folder(s_sel); else app_open_file(s_sel);
        break;
    case 1:
    case 2:
        strlcpy(s_clip, s_sel, sizeof(s_clip));
        s_clip_cut = i == 2;
        ui_toast(i == 2 ? "Cut - open a folder and tap Paste" : "Copied - open a folder and tap Paste");
        after_change();
        break;
    case 3:
        ui_keyboard("Rename", strrchr(s_sel, '/') + 1, false, rename_done, NULL);
        break;
    case 4: {
        char msg[160];
        snprintf(msg, sizeof(msg), "Delete \"%s\"%s? This cannot be undone.", strrchr(s_sel, '/') + 1,
                 fm_is_dir(s_sel) ? " and everything inside it" : "");
        ui_confirm("Delete", msg, "Delete", UI_RED, do_delete, NULL);
        break;
    }
    case 5:
        ui_page_push("Info", info_build, NULL, NULL);
        break;
    }
}

static void entry_long(lv_event_t *e)
{
    const char *full = lv_obj_get_user_data(lv_event_get_current_target(e));
    strlcpy(s_sel, full, sizeof(s_sel));
    lv_indev_wait_release(lv_indev_active());
    ui_choose(strrchr(s_sel, '/') + 1, fm_is_dir(s_sel) ? ACTIONS_DIR : ACTIONS_FILE, 6, -1, action_chosen, NULL);
}

static void entry_click(lv_event_t *e)
{
    const char *full = lv_obj_get_user_data(lv_event_get_current_target(e));
    if (fm_is_dir(full)) {
        open_folder(full);
    } else {
        app_open_file(full);
    }
}

static void entry_more(lv_event_t *e)
{
    entry_long(e);
}

/* ------------------------------------------------------------------ folder toolbar */

static void mkdir_done(const char *name, void *user)
{
    const char *dir = user;
    char p[FM_MAX_PATH];
    if (!name[0] || strchr(name, '/')) {
        ui_toast("Invalid name");
        return;
    }
    snprintf(p, sizeof(p), "%s/%s", dir, name);
    if (mkdir(p, 0775) != 0) {
        ui_toast(strerror(errno));
    }
    after_change();
}

static void mkdir_click(lv_event_t *e)
{
    ui_keyboard("New folder", "", false, mkdir_done, lv_event_get_user_data(e));
}

static void paste_click(lv_event_t *e)
{
    const char *dir = lv_event_get_user_data(e);
    if (!s_clip[0]) {
        ui_toast("Clipboard is empty");
        return;
    }
    const char *name = strrchr(s_clip, '/') + 1;
    char dst[FM_MAX_PATH];
    snprintf(dst, sizeof(dst), "%s/%s", dir, name);
    if (strcmp(dst, s_clip) == 0 && s_clip_cut) {
        ui_toast("Already here");
        return;
    }
    /* copying into the same folder: make "name copy.ext" */
    if (access(dst, F_OK) == 0) {
        if (s_clip_cut) {
            ui_toast("A file with that name exists here");
            return;
        }
        const char *dot = strrchr(name, '.');
        int base = dot && dot != name ? (int)(dot - name) : (int)strlen(name);
        for (int i = 1; i < 50 && access(dst, F_OK) == 0; i++) {
            snprintf(dst, sizeof(dst), "%s/%.*s copy%s%.0d%s", dir, base, name, i > 1 ? " " : "", i > 1 ? i : 0,
                     dot && dot != name ? dot : "");
        }
    }
    size_t cl = strlen(s_clip);
    if (strncmp(dst, s_clip, cl) == 0 && dst[cl] == '/') {
        ui_toast("Can't put a folder inside itself");
        return;
    }
    ui_toast(s_clip_cut ? "Moving..." : "Copying...");
    bool ok = s_clip_cut ? rename(s_clip, dst) == 0 : fm_copy_recursive(s_clip, dst);
    if (!ok) {
        ui_toast(strerror(errno));
    } else {
        ui_toast(s_clip_cut ? "Moved" : "Copied");
        if (s_clip_cut) s_clip[0] = '\0';
    }
    after_change();
}

static void row_free_path(lv_event_t *e)
{
    lv_free(lv_obj_get_user_data(lv_event_get_current_target(e)));
}

static void folder_build(lv_obj_t *c, void *ctx)
{
    const char *dir = ctx;
    lv_obj_t *bar = lv_obj_create(c);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(bar, 8, 0);
    lv_obj_t *b = ui_button(bar, LV_SYMBOL_PLUS " Folder", UI_CARD2, mkdir_click, ctx);
    lv_obj_set_flex_grow(b, 1);
    if (s_clip[0]) {
        char pl[48];
        snprintf(pl, sizeof(pl), LV_SYMBOL_PASTE " Paste");
        b = ui_button(bar, pl, s_clip_cut ? UI_ORANGE : UI_BLUE, paste_click, ctx);
        lv_obj_set_flex_grow(b, 1);
    }

    DIR *d = opendir(dir);
    if (!d) {
        ui_label(c, "Can't open this folder", &lv_font_montserrat_18, UI_MUTED);
        return;
    }
    entry_t *list = calloc(MAX_ENTRIES, sizeof(entry_t));
    int n = 0;
    struct dirent *e;
    char full[FM_MAX_PATH];
    while (list && n < MAX_ENTRIES && (e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;             /* hide ._ Mac files and hidden folders */
        list[n].name = strdup(e->d_name);
        list[n].dir = e->d_type == DT_DIR;
        if (!list[n].dir) {
            struct stat st;
            snprintf(full, sizeof(full), "%s/%s", dir, e->d_name);
            list[n].size = stat(full, &st) == 0 ? st.st_size : 0;
        }
        n++;
    }
    closedir(d);
    if (!list) return;
    qsort(list, n, sizeof(entry_t), entry_cmp);
    if (n == 0) {
        lv_obj_t *l = ui_label(c, "Empty folder", &lv_font_montserrat_18, UI_MUTED);
        lv_obj_set_style_pad_top(l, 40, 0);
    }
    for (int i = 0; i < n; i++) {
        char val[24] = "";
        uint32_t col = UI_BLUE;
        const char *icon = LV_SYMBOL_DIRECTORY;
        if (!list[i].dir) {
            icon = file_icon(list[i].name, &col);
            fmt_size(list[i].size, val, sizeof(val));
        }
        lv_obj_t *r = ui_row(c, icon, col, list[i].name, val, entry_click, NULL);
        snprintf(full, sizeof(full), "%s/%s", dir, list[i].name);
        char *p = lv_malloc(strlen(full) + 1);
        strcpy(p, full);
        lv_obj_set_user_data(r, p);
        lv_obj_add_event_cb(r, entry_long, LV_EVENT_LONG_PRESSED, NULL);
        lv_obj_add_event_cb(r, row_free_path, LV_EVENT_DELETE, NULL);
        free(list[i].name);
    }
    free(list);
    lv_obj_t *hint = ui_label(c, "Long-press a file for copy, cut, rename, delete", &lv_font_montserrat_14, 0x636366);
    lv_obj_set_style_pad_top(hint, 6, 0);
    (void)entry_more;
}

/* ------------------------------------------------------------------ text viewer */

static void text_build(lv_obj_t *c, void *ctx)
{
    const char *path = ctx;
    FILE *f = fopen(path, "r");
    if (!f) {
        ui_label(c, "Can't open file", &lv_font_montserrat_18, UI_MUTED);
        return;
    }
    char *buf = malloc(8193);
    size_t n = buf ? fread(buf, 1, 8192, f) : 0;
    bool more = fgetc(f) != EOF;
    fclose(f);
    if (!buf) return;
    buf[n] = '\0';
    lv_obj_t *l = ui_label(c, buf, &lv_font_montserrat_16, 0xE5E5EA);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(l, lv_pct(100));
    free(buf);
    if (more) {
        ui_label(c, "(showing the first 8 KB)", &lv_font_montserrat_14, UI_MUTED);
    }
}

void textview_open_file(const char *abs_path)
{
    ui_page_push(strrchr(abs_path, '/') + 1, text_build, (page_fn_t)free, strdup(abs_path));
}
