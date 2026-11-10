/*
 * SD card file manager for the Waveshare ESP32-S3-Touch-AMOLED-2.06.
 *
 * The card is wired to the ESP32-S3 SDMMC controller in 1-bit mode
 * (pins taken from Waveshare's board support package):
 *     CLK = GPIO2, CMD = GPIO1, D0 = GPIO3
 *
 * It is mounted at /sdcard, but the user sees the card root as "/".
 * Commands arrive as text lines from BLE or the USB serial port and are executed
 * one at a time by a worker task, so slow file operations never block Bluetooth.
 */
#include "file_manager.h"

#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "driver/sdmmc_host.h"
#include "driver/usb_serial_jtag.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdmmc_cmd.h"
#include "wifi_svc.h"
#include "esp_heap_caps.h"
#include "hw.h"
#include "mbedtls/base64.h"
#include "lua_apps.h"
#include "phone.h"
#include "settings.h"
#include "timesvc.h"
#include "ui.h"

static const char *TAG = "FILES";

#define MOUNT_POINT   "/sdcard"
#define PIN_SD_CLK    GPIO_NUM_2
#define PIN_SD_CMD    GPIO_NUM_1
#define PIN_SD_D0     GPIO_NUM_3

#define MAX_CMD_LEN   244          /* one BLE write at MTU 247 */
#define MAX_PATH_LEN  FM_MAX_PATH
#define MAX_CAT_BYTES 2048         /* don't flood the phone with huge files */
#define COPY_BUF_SIZE (16 * 1024)

typedef struct {
    bool from_usb;
    char text[MAX_CMD_LEN + 1];
} command_t;

static fm_send_fn_t  s_ble_send;
static QueueHandle_t s_queue;
static sdmmc_card_t *s_card;
static bool          s_reply_to_usb;              /* where the current command's replies go */
static char         *s_capture;                   /* web console: collect replies here */
static size_t        s_capture_len, s_capture_max;
static SemaphoreHandle_t s_cmd_lock;              /* one command at a time */
static char          s_cwd[MAX_PATH_LEN] = MOUNT_POINT;
static char          s_clip[MAX_PATH_LEN];        /* clipboard: path of copied/cut file */
static bool          s_clip_is_cut;

/* ------------------------------------------------------------------ replies */

static void reply(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < 0) {
        return;
    }
    if (n >= (int)sizeof(buf)) {
        n = sizeof(buf) - 1;
    }
    if (s_capture) {
        int room = (int)(s_capture_max - s_capture_len) - 2;
        if (room > 0) {
            int k = n < room ? n : room;
            memcpy(s_capture + s_capture_len, buf, k);
            s_capture_len += k;
            s_capture[s_capture_len++] = '\n';
            s_capture[s_capture_len] = '\0';
        }
    } else if (s_reply_to_usb) {
        printf("%s\n", buf);
    } else {
        s_ble_send(buf, n);
    }
}

static void reply_errno(const char *what, const char *path)
{
    reply("%s %s: %s", what, path, strerror(errno));
}

static const char *human_size(uint64_t bytes, char *out, size_t out_size)
{
    if (bytes < 1024) {
        snprintf(out, out_size, "%llu B", (unsigned long long)bytes);
    } else if (bytes < 1024 * 1024) {
        snprintf(out, out_size, "%.1f KB", bytes / 1024.0);
    } else if (bytes < 1024ULL * 1024 * 1024) {
        snprintf(out, out_size, "%.1f MB", bytes / (1024.0 * 1024));
    } else {
        snprintf(out, out_size, "%.2f GB", bytes / (1024.0 * 1024 * 1024));
    }
    return out;
}

/* ------------------------------------------------------------------ paths */

/* What the user sees: "/sdcard/photos" -> "/photos", "/sdcard" -> "/". */
static const char *display_path(const char *abs)
{
    const char *p = abs + strlen(MOUNT_POINT);
    return *p ? p : "/";
}

/*
 * Turn what the user typed into a clean absolute path on the card.
 *   "/notes/a.txt" -> card root based      "a.txt" / "../a.txt" -> relative to cwd
 * "." and ".." are resolved, and ".." can never climb above the card root.
 */
static bool resolve_path(const char *in, char *out, size_t out_size)
{
    char tmp[MAX_PATH_LEN * 2];
    if (in == NULL || *in == '\0') {
        in = ".";
    }
    if (in[0] == '/') {
        snprintf(tmp, sizeof(tmp), "%s%s", MOUNT_POINT, in);
    } else {
        snprintf(tmp, sizeof(tmp), "%s/%s", s_cwd, in);
    }

    char *parts[32];
    int count = 0;
    char *save = NULL;
    for (char *tok = strtok_r(tmp, "/", &save); tok; tok = strtok_r(NULL, "/", &save)) {
        if (strcmp(tok, ".") == 0) {
            continue;
        }
        if (strcmp(tok, "..") == 0) {
            if (count > 1) {          /* parts[0] is "sdcard": never pop it */
                count--;
            }
            continue;
        }
        if (count == 32) {
            return false;
        }
        parts[count++] = tok;
    }

    size_t len = 0;
    for (int i = 0; i < count; i++) {
        int w = snprintf(out + len, out_size - len, "/%s", parts[i]);
        if (w < 0 || (size_t)w >= out_size - len) {
            return false;
        }
        len += w;
    }
    return count > 0 && strcmp(parts[0], MOUNT_POINT + 1) == 0;
}

static bool is_dir(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static const char *base_name(const char *path)
{
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

/* Destination for cp/mv/paste: if `dst` is a folder, put the file inside it with its own name. */
static bool make_dest_path(const char *src, const char *dst_dir_or_file, char *out, size_t out_size)
{
    if (is_dir(dst_dir_or_file)) {
        int w = snprintf(out, out_size, "%s/%s", dst_dir_or_file, base_name(src));
        return w > 0 && (size_t)w < out_size;
    }
    snprintf(out, out_size, "%s", dst_dir_or_file);
    return true;
}

/* ------------------------------------------------------------------ SD card */

static bool sd_mount(void)
{
    if (s_card) {
        return true;
    }
    esp_vfs_fat_mount_config_t mount_cfg = {
        .format_if_mount_failed = false,      /* never wipe the user's card */
        .max_files = 5,
        .allocation_unit_size = 16 * 1024,
    };
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 1;
    slot.clk = PIN_SD_CLK;
    slot.cmd = PIN_SD_CMD;
    slot.d0  = PIN_SD_D0;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    esp_err_t err = esp_vfs_fat_sdmmc_mount(MOUNT_POINT, &host, &slot, &mount_cfg, &s_card);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SD card mount failed: %s (is a card inserted? FAT32/exFAT formatted?)",
                 esp_err_to_name(err));
        s_card = NULL;
        return false;
    }
    sdmmc_card_print_info(stdout, s_card);
    ESP_LOGI(TAG, "SD card mounted at %s", MOUNT_POINT);
    return true;
}

static void sd_unmount(void)
{
    if (s_card) {
        esp_vfs_fat_sdcard_unmount(MOUNT_POINT, s_card);
        s_card = NULL;
        snprintf(s_cwd, sizeof(s_cwd), "%s", MOUNT_POINT);
    }
}

static bool copy_file(const char *src, const char *dst, uint64_t *copied)
{
    *copied = 0;
    FILE *in = fopen(src, "rb");
    if (!in) {
        return false;
    }
    FILE *out = fopen(dst, "wb");
    if (!out) {
        int e = errno;
        fclose(in);
        errno = e;
        return false;
    }
    char *buf = malloc(COPY_BUF_SIZE);
    bool ok = buf != NULL;
    int err = ok ? 0 : ENOMEM;

    size_t n;
    while (ok && (n = fread(buf, 1, COPY_BUF_SIZE, in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) {
            ok = false;
            err = errno;
            break;
        }
        *copied += n;
    }
    if (ok && ferror(in)) {
        ok = false;
        err = EIO;
    }
    free(buf);
    fclose(in);
    if (fclose(out) != 0 && ok) {
        ok = false;
        err = errno;
    }
    if (!ok) {
        unlink(dst);                 /* don't leave half-copied files behind */
        errno = err;
    }
    return ok;
}

/* ------------------------------------------------------------------ commands */

static void cmd_help(void)
{
    reply("Files: ls [dir] | cd <dir> | pwd | cat <file>");
    reply("write <file> <text> | mkdir <dir> | rm <file|empty dir>");
    reply("copy <file> | cut <file> | paste [dir]  (clipboard)");
    reply("cp <src> <dst> | mv <src> <dst>  (mv also renames)");
    reply("info | df | mount | unmount | ping | uptime");
    reply("web  -> address of the browser file manager");
    reply("apps | app run <id> | app stop | app rm <id>   (Lua apps in /apps)");
}

static void cmd_ls(const char *arg)
{
    char path[MAX_PATH_LEN];
    if (!resolve_path(arg, path, sizeof(path))) {
        reply("ls: bad path");
        return;
    }
    DIR *dir = opendir(path);
    if (!dir) {
        reply_errno("ls", display_path(path));
        return;
    }
    int count = 0;
    struct dirent *e;
    while ((e = readdir(dir)) != NULL) {
        char full[MAX_PATH_LEN * 2];
        snprintf(full, sizeof(full), "%s/%s", path, e->d_name);
        struct stat st;
        if (e->d_type == DT_DIR) {
            reply("[D] %s/", e->d_name);
        } else if (stat(full, &st) == 0) {
            char sz[16];
            reply("[F] %s  %s", e->d_name, human_size(st.st_size, sz, sizeof(sz)));
        } else {
            reply("[?] %s", e->d_name);
        }
        count++;
    }
    closedir(dir);
    reply("%d item(s) in %s", count, display_path(path));
}

static void cmd_cd(const char *arg)
{
    char path[MAX_PATH_LEN];
    if (!resolve_path(arg ? arg : "/", path, sizeof(path))) {
        reply("cd: bad path");
    } else if (!is_dir(path)) {
        reply("cd: %s is not a folder", display_path(path));
    } else {
        snprintf(s_cwd, sizeof(s_cwd), "%s", path);
        reply("now in %s", display_path(s_cwd));
    }
}

static void cmd_cat(const char *arg)
{
    char path[MAX_PATH_LEN];
    if (!arg || !resolve_path(arg, path, sizeof(path))) {
        reply("usage: cat <file>");
        return;
    }
    FILE *f = fopen(path, "r");
    if (!f) {
        reply_errno("cat", display_path(path));
        return;
    }
    char chunk[200];
    size_t n, total = 0;
    while (total < MAX_CAT_BYTES && (n = fread(chunk, 1, sizeof(chunk) - 1, f)) > 0) {
        chunk[n] = '\0';
        reply("%s", chunk);
        total += n;
    }
    bool more = fgetc(f) != EOF;
    fclose(f);
    reply(more ? "-- (stopped after %u bytes) --" : "-- end (%u bytes) --", (unsigned)total);
}

static void cmd_write(const char *name, const char *text)
{
    char path[MAX_PATH_LEN];
    if (!name || !resolve_path(name, path, sizeof(path))) {
        reply("usage: write <file> <text>");
        return;
    }
    FILE *f = fopen(path, "w");
    if (!f) {
        reply_errno("write", display_path(path));
        return;
    }
    size_t len = text ? strlen(text) : 0;
    bool ok = fwrite(text ? text : "", 1, len, f) == len;
    ok = (fclose(f) == 0) && ok;
    if (ok) {
        reply("wrote %u bytes to %s", (unsigned)len, display_path(path));
    } else {
        reply_errno("write", display_path(path));
    }
}

static void cmd_mkdir(const char *arg)
{
    char path[MAX_PATH_LEN];
    if (!arg || !resolve_path(arg, path, sizeof(path))) {
        reply("usage: mkdir <dir>");
    } else if (mkdir(path, 0775) != 0) {
        reply_errno("mkdir", display_path(path));
    } else {
        reply("created %s/", display_path(path));
    }
}

static void cmd_rm(const char *arg)
{
    char path[MAX_PATH_LEN];
    if (!arg || !resolve_path(arg, path, sizeof(path)) || strcmp(path, MOUNT_POINT) == 0) {
        reply("usage: rm <file | empty folder>");
        return;
    }
    bool dir = is_dir(path);
    int rc = dir ? rmdir(path) : unlink(path);
    if (rc != 0) {
        /* FAT reports a non-empty folder as "access denied" - say what really happened. */
        DIR *d = dir ? opendir(path) : NULL;
        bool has_items = d && readdir(d) != NULL;
        if (d) {
            closedir(d);
        }
        if (has_items) {
            reply("rm: %s/ is not empty - delete what's inside first", display_path(path));
        } else {
            reply_errno("rm", display_path(path));
        }
    } else {
        reply("deleted %s", display_path(path));
    }
}

/* Shared by cp, mv and paste. Returns true if the file was copied/moved. */
static bool do_transfer(const char *src, const char *dst_arg_abs, bool move)
{
    const char *op = move ? "move" : "copy";
    if (is_dir(src)) {
        if (!move) {
            reply("%s: only files can be copied, not folders", op);
            return false;
        }
    } else if (access(src, F_OK) != 0) {
        reply_errno(op, display_path(src));
        return false;
    }
    char dst[MAX_PATH_LEN];
    if (!make_dest_path(src, dst_arg_abs, dst, sizeof(dst))) {
        reply("%s: destination path too long", op);
        return false;
    }
    if (strcmp(src, dst) == 0) {
        reply("%s: source and destination are the same", op);
        return false;
    }
    if (access(dst, F_OK) == 0) {
        reply("%s: %s already exists (delete or rename it first)", op, display_path(dst));
        return false;
    }

    if (move) {
        if (rename(src, dst) != 0) {
            reply_errno("move", display_path(src));
            return false;
        }
        reply("moved %s -> %s", display_path(src), display_path(dst));
    } else {
        int64_t t0 = esp_timer_get_time();
        uint64_t bytes;
        if (!copy_file(src, dst, &bytes)) {
            reply_errno("copy", display_path(src));
            return false;
        }
        double secs = (esp_timer_get_time() - t0) / 1e6;
        char sz[16];
        reply("copied %s -> %s (%s in %.1f s)", display_path(src), display_path(dst),
              human_size(bytes, sz, sizeof(sz)), secs);
    }
    return true;
}

static void cmd_cp_mv(const char *a, const char *b, bool move)
{
    char src[MAX_PATH_LEN], dst[MAX_PATH_LEN];
    if (!a || !b || !resolve_path(a, src, sizeof(src)) || !resolve_path(b, dst, sizeof(dst))) {
        reply(move ? "usage: mv <src> <dst>" : "usage: cp <src> <dst>");
        return;
    }
    do_transfer(src, dst, move);
}

static void cmd_clip(const char *arg, bool cut)
{
    char path[MAX_PATH_LEN];
    if (!arg || !resolve_path(arg, path, sizeof(path))) {
        reply(cut ? "usage: cut <file>" : "usage: copy <file>");
        return;
    }
    if (access(path, F_OK) != 0) {
        reply_errno(cut ? "cut" : "copy", display_path(path));
        return;
    }
    snprintf(s_clip, sizeof(s_clip), "%s", path);
    s_clip_is_cut = cut;
    reply("%s %s to clipboard - cd to a folder and type: paste",
          cut ? "cut" : "copied", display_path(path));
}

static void cmd_paste(const char *arg)
{
    if (s_clip[0] == '\0') {
        reply("paste: clipboard is empty (use copy or cut first)");
        return;
    }
    char dir[MAX_PATH_LEN];
    if (!resolve_path(arg ? arg : ".", dir, sizeof(dir)) || !is_dir(dir)) {
        reply("paste: target must be a folder");
        return;
    }
    if (do_transfer(s_clip, dir, s_clip_is_cut) && s_clip_is_cut) {
        s_clip[0] = '\0';              /* a cut file is gone from its old place: paste only once */
    }
}

static void cmd_info(void)
{
    if (!s_card) {
        reply("no SD card mounted (insert one, then type: mount)");
        return;
    }
    char sz[16];
    uint64_t cap = (uint64_t)s_card->csd.capacity * s_card->csd.sector_size;
    reply("card: %s, %s, %s, %d MHz", s_card->cid.name,
          cap > 32ULL * 1024 * 1024 * 1024 ? "SDXC" : (cap > 2ULL * 1024 * 1024 * 1024 ? "SDHC" : "SDSC"),
          human_size(cap, sz, sizeof(sz)), s_card->real_freq_khz / 1000);
}

static void cmd_df(void)
{
    uint64_t total = 0, free_bytes = 0;
    if (esp_vfs_fat_info(MOUNT_POINT, &total, &free_bytes) != ESP_OK) {
        reply("df: can't read card usage");
        return;
    }
    char t[16], f[16], u[16];
    reply("used %s, free %s, total %s",
          human_size(total - free_bytes, u, sizeof(u)),
          human_size(free_bytes, f, sizeof(f)),
          human_size(total, t, sizeof(t)));
}


/* ------------------------------------------------------------------ device / test commands */

static void usb_write_all(const void *data, size_t len)
{
    const uint8_t *p = data;
    while (len) {
        int n = usb_serial_jtag_write_bytes(p, len > 2048 ? 2048 : len, pdMS_TO_TICKS(1000));
        if (n <= 0) {
            return;
        }
        p += n;
        len -= n;
    }
}

/*
 * Screenshot over USB: the BMP's pixel data is run-length encoded (count u8, pixel u16)
 * because watch screens are mostly flat colour, then base64. Header: "SHOT_RLE <bmp_len> <hdr_len>".
 */
static void cmd_shot(void)
{
    size_t len = 0;
    uint8_t *bmp = ui_screenshot_bmp(&len);
    if (!bmp) {
        reply("shot: failed");
        return;
    }
    if (!s_reply_to_usb) {
        reply("shot: %u bytes (use USB or the web API to fetch it)", (unsigned)len);
        free(bmp);
        return;
    }
    const size_t hdr = 66;
    const uint16_t *px = (const uint16_t *)(bmp + hdr);
    size_t npx = (len - hdr) / 2;
    uint8_t *rle = malloc(hdr + npx * 3 + 16);
    size_t r = 0;
    if (!rle) {
        free(bmp);
        reply("shot: no memory");
        return;
    }
    memcpy(rle, bmp, hdr);
    r = hdr;
    for (size_t i = 0; i < npx;) {
        uint16_t v = px[i];
        size_t run = 1;
        while (i + run < npx && px[i + run] == v && run < 255) {
            run++;
        }
        rle[r++] = (uint8_t)run;
        rle[r++] = v & 0xFF;
        rle[r++] = v >> 8;
        i += run;
    }
    free(bmp);
    char head[64];
    int hl = snprintf(head, sizeof(head), "\nSHOT_RLE %u %u\n", (unsigned)len, (unsigned)hdr);
    usb_write_all(head, hl);
    static unsigned char out[4100];
    for (size_t off = 0; off < r; off += 3072) {
        size_t n = r - off < 3072 ? r - off : 3072, olen = 0;
        mbedtls_base64_encode(out, sizeof(out) - 2, &olen, rle + off, n);
        out[olen++] = '\n';
        usb_write_all(out, olen);
    }
    usb_write_all("SHOT_END\n", 9);
    free(rle);
}

static void cmd_status(void)
{
    power_status_t p;
    power_get(&p);
    wifi_status_t w;
    wifi_svc_status(&w);
    reply("battery %d%% %dmV usb=%d chg=%d temp=%.1fC", p.percent, p.voltage_mv, p.usb, p.charging, p.temp_c);
    reply("wifi state=%d ssid=%s ip=%s rssi=%d ap=%d", w.state, w.ssid, w.ip, w.rssi, w.ap_on);
    reply("heap internal=%u psram=%u min_internal=%u steps=%lu awake=%d",
          (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
          (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
          (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
          (unsigned long)imu_steps_today(), ui_is_awake());
}

static void cmd_scan(void)
{
    wifi_ap_info_t aps[20];
    int n = wifi_svc_scan(aps, 20);
    for (int i = 0; i < n; i++) {
        reply("%4d %s %s%s", aps[i].rssi, aps[i].secure ? "*" : " ", aps[i].ssid, aps[i].saved ? " (saved)" : "");
    }
    reply("%d networks", n);
}

/* set <key> <value>  -> settings field (number, true/false or text) */
static void cmd_set(const char *key, const char *value)
{
    if (!key || !value) {
        reply("usage: set <key> <value>");
        return;
    }
    cJSON *o = cJSON_CreateObject();
    char *end = NULL;
    double d = strtod(value, &end);
    if (strcmp(value, "true") == 0 || strcmp(value, "false") == 0) {
        cJSON_AddBoolToObject(o, key, strcmp(value, "true") == 0);
    } else if (end && *end == '\0') {
        cJSON_AddNumberToObject(o, key, d);
    } else {
        cJSON_AddStringToObject(o, key, value);
    }
    int n = settings_from_json(o);
    cJSON_Delete(o);
    reply(n ? "ok" : "unknown setting");
}

/* putb <w|a> <card-path> <base64>: write (w) or append (a) a chunk of a binary file.
 * Creates missing folders. Used by `watchctl.py app push` to install apps over USB. */
static void mkdirs_for(const char *abs_file)
{
    char tmp[FM_MAX_PATH];
    strlcpy(tmp, abs_file, sizeof(tmp));
    for (char *p = tmp + strlen(MOUNT_POINT) + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0775);
            *p = '/';
        }
    }
}

static void cmd_putb(const char *mode, const char *text)
{
    if (!s_card) { reply("ERR no SD card"); return; }
    const char *sp = text ? strchr(text, ' ') : NULL;
    if (!mode || !sp || (mode[0] != 'w' && mode[0] != 'a')) { reply("ERR usage: putb <w|a> <path> <base64>"); return; }
    char card[FM_MAX_PATH], abs[FM_MAX_PATH];
    size_t plen = (size_t)(sp - text);
    if (plen >= sizeof(card)) { reply("ERR path too long"); return; }
    memcpy(card, text, plen);
    card[plen] = '\0';
    if (!fm_card_path(card, abs, sizeof(abs))) { reply("ERR bad path"); return; }
    unsigned char bin[192];
    size_t n = 0;
    if (mbedtls_base64_decode(bin, sizeof(bin), &n, (const unsigned char *)sp + 1, strlen(sp + 1)) != 0) {
        reply("ERR bad base64");
        return;
    }
    mkdirs_for(abs);
    FILE *f = fopen(abs, mode[0] == 'w' ? "wb" : "ab");
    if (!f) { reply("ERR cannot open %s", card); return; }
    bool ok = fwrite(bin, 1, n, f) == n;
    fclose(f);
    reply(ok ? "OK %u" : "ERR write failed", (unsigned)n);
}

static void cmd_apps(void)
{
    lua_app_info_t *list = calloc(32, sizeof(*list));
    int n = list ? lua_apps_scan(list, 32) : 0;
    char running[32] = "";
    lua_apps_running(running, sizeof(running));
    if (!n) reply("no apps installed (put them in /apps/<name>/main.lua on the SD card)");
    for (int i = 0; i < n; i++) {
        reply("%s%-16s %-20s v%s", strcmp(running, list[i].id) == 0 ? "* " : "  ", list[i].id, list[i].name, list[i].version);
    }
    free(list);
}

static bool device_command(const char *cmd, char *a1, char *a2, char *text, char *line_rest)
{
    if (strcmp(cmd, "shot") == 0) { cmd_shot(); return true; }
    if (strcmp(cmd, "status") == 0) { cmd_status(); return true; }
    if (strcmp(cmd, "scan") == 0) { cmd_scan(); return true; }
    if (strcmp(cmd, "tap") == 0 && a1 && a2) { ui_inject_tap(atoi(a1), atoi(a2)); reply("ok"); return true; }
    if (strcmp(cmd, "long") == 0 && a1 && a2) { ui_inject_long_press(atoi(a1), atoi(a2)); reply("ok"); return true; }
    if (strcmp(cmd, "swipe") == 0 && text) {
        int x1, y1, x2, y2;
        char buf[64];
        snprintf(buf, sizeof(buf), "%s %s", a1, text);
        if (sscanf(buf, "%d %d %d %d", &x1, &y1, &x2, &y2) == 4) {
            ui_inject_swipe(x1, y1, x2, y2);
            reply("ok");
        }
        return true;
    }
    if (strcmp(cmd, "key") == 0 && a1) { ui_inject_key(a1); reply("ok"); return true; }
    if (strcmp(cmd, "join") == 0 && a1) {
        wifi_svc_connect(a1, a2 ? a2 : "");
        reply("joining %s", a1);
        return true;
    }
    if (strcmp(cmd, "set") == 0) { cmd_set(a1, text); return true; }
    if (strcmp(cmd, "gb") == 0 && line_rest) { gb_handle_line(line_rest); reply("ok"); return true; }
    if (strcmp(cmd, "time") == 0 && a1) {       /* time <unix-epoch-seconds> */
        timesvc_set_utc((time_t)strtoll(a1, NULL, 10));
        reply("time set");
        return true;
    }
    if (strcmp(cmd, "restart") == 0) { power_restart(); return true; }
    if (strcmp(cmd, "putb") == 0) { cmd_putb(a1, text); return true; }
    if (strcmp(cmd, "apps") == 0) { cmd_apps(); return true; }
    if (strcmp(cmd, "app") == 0 && a1) {
        if (strcmp(a1, "run") == 0 && a2) reply(lua_apps_run(a2) ? "running %s" : "no app '%s' (needs /apps/%s/main.lua)", a2, a2);
        else if (strcmp(a1, "stop") == 0) { lua_apps_stop(); reply("ok"); }
        else if (strcmp(a1, "rm") == 0 && a2) reply(lua_apps_remove(a2) ? "removed %s" : "could not remove %s", a2);
        else reply("usage: app run <id> | app stop | app rm <id>");
        return true;
    }
    return false;
}

static void run_command(char *line)
{
    /* Split "cmd arg1 arg2...". strtok_r writes '\0' into the line as it splits,
     * so copy "everything after arg1" (used by `write`) before splitting further. */
    char raw_rest[MAX_CMD_LEN + 1];
    const char *sp = strchr(line, ' ');
    snprintf(raw_rest, sizeof(raw_rest), "%s", sp ? sp + 1 : "");
    char *save = NULL;
    char *cmd = strtok_r(line, " ", &save);
    if (!cmd) {
        return;
    }
    char *a1 = strtok_r(NULL, " ", &save);
    char text_buf[MAX_CMD_LEN + 1];
    snprintf(text_buf, sizeof(text_buf), "%s", (save && *save) ? save : "");
    char *text = text_buf[0] ? text_buf : NULL;
    char *a2 = strtok_r(NULL, " ", &save);

    if (strcmp(cmd, "help") == 0)        { cmd_help(); return; }
    if (strcmp(cmd, "ping") == 0)        { reply("pong"); return; }
    if (strcmp(cmd, "uptime") == 0)      { reply("uptime: %llu s", (unsigned long long)(esp_timer_get_time() / 1000000)); return; }
    if (device_command(cmd, a1, a2, text, raw_rest[0] ? raw_rest : NULL)) {
        return;
    }
    if (strcmp(cmd, "web") == 0)         { reply("open in a browser: %s", wifi_get_url()); return; }
    if (strcmp(cmd, "mount") == 0)       { reply(sd_mount() ? "SD card mounted" : "mount failed - card inserted?"); return; }
    if (strcmp(cmd, "unmount") == 0)     { sd_unmount(); reply("SD card unmounted - safe to remove"); return; }

    /* everything below needs the card */
    if (!s_card) {
        reply("no SD card mounted (insert one, then type: mount)");
        return;
    }
    if (strcmp(cmd, "info") == 0)        cmd_info();
    else if (strcmp(cmd, "df") == 0)    cmd_df();
    else if (strcmp(cmd, "pwd") == 0)   reply("%s", display_path(s_cwd));
    else if (strcmp(cmd, "ls") == 0)    cmd_ls(a1);
    else if (strcmp(cmd, "cd") == 0)    cmd_cd(a1);
    else if (strcmp(cmd, "cat") == 0)   cmd_cat(a1);
    else if (strcmp(cmd, "write") == 0) cmd_write(a1, text);
    else if (strcmp(cmd, "mkdir") == 0) cmd_mkdir(a1);
    else if (strcmp(cmd, "rm") == 0)    cmd_rm(a1);
    else if (strcmp(cmd, "cp") == 0)    cmd_cp_mv(a1, a2, false);
    else if (strcmp(cmd, "mv") == 0)    cmd_cp_mv(a1, a2, true);
    else if (strcmp(cmd, "copy") == 0)  cmd_clip(a1, false);
    else if (strcmp(cmd, "cut") == 0)   cmd_clip(a1, true);
    else if (strcmp(cmd, "paste") == 0) cmd_paste(a1);
    else reply("unknown command '%s' - type help", cmd);
}

/* ------------------------------------------------------------------ tasks */

static void submit(const uint8_t *data, size_t len, bool from_usb)
{
    while (len > 0 && (data[len - 1] == '\r' || data[len - 1] == '\n' || data[len - 1] == ' ')) {
        len--;
    }
    if (len == 0) {
        return;
    }
    command_t c = { .from_usb = from_usb };
    if (len > MAX_CMD_LEN) {
        len = MAX_CMD_LEN;
    }
    memcpy(c.text, data, len);
    c.text[len] = '\0';
    if (xQueueSend(s_queue, &c, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Busy - command dropped: %s", c.text);
    }
}

void file_manager_submit_ble(const uint8_t *data, size_t len)
{
    submit(data, len, false);
}

static void worker_task(void *arg)
{
    command_t c;
    while (true) {
        if (xQueueReceive(s_queue, &c, portMAX_DELAY) == pdTRUE) {
            ESP_LOGI(TAG, "%s> %s", c.from_usb ? "usb" : "ble", c.text);
            xSemaphoreTake(s_cmd_lock, portMAX_DELAY);
            s_reply_to_usb = c.from_usb;
            run_command(c.text);
            xSemaphoreGive(s_cmd_lock);
        }
    }
}

/* Lets you type the same commands in `idf.py monitor` over the USB cable. */
static void usb_reader_task(void *arg)
{
    char line[MAX_CMD_LEN + 1];
    size_t len = 0;
    uint8_t ch;
    while (true) {
        if (usb_serial_jtag_read_bytes(&ch, 1, portMAX_DELAY) <= 0) {
            continue;
        }
        if (ch == '\r' || ch == '\n') {
            submit((const uint8_t *)line, len, true);
            len = 0;
        } else if (len < MAX_CMD_LEN) {
            line[len++] = (char)ch;
        }
    }
}

/* ------------------------------------------------------------------ shared helpers */

bool fm_is_mounted(void)
{
    return s_card != NULL;
}

bool fm_card_path(const char *card_path, char *abs_out, size_t out_size)
{
    /* Only absolute card paths: never depends on the BLE "current folder". */
    return card_path && card_path[0] == '/' && resolve_path(card_path, abs_out, out_size);
}

const char *fm_display_path(const char *abs_path)
{
    return display_path(abs_path);
}

bool fm_is_dir(const char *abs_path)
{
    return is_dir(abs_path);
}

int fm_remove_recursive(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0) {
        return -1;
    }
    if (!S_ISDIR(st.st_mode)) {
        return unlink(path);
    }
    DIR *dir = opendir(path);
    if (!dir) {
        return -1;
    }
    char *child = malloc(MAX_PATH_LEN);      /* heap, not stack: this function is recursive */
    if (!child) {
        closedir(dir);
        errno = ENOMEM;
        return -1;
    }
    int rc = 0;
    struct dirent *e;
    while (rc == 0 && (e = readdir(dir)) != NULL) {
        if (snprintf(child, MAX_PATH_LEN, "%s/%s", path, e->d_name) >= MAX_PATH_LEN) {
            errno = ENAMETOOLONG;
            rc = -1;
            break;
        }
        rc = fm_remove_recursive(child);
    }
    closedir(dir);
    free(child);
    return rc == 0 ? rmdir(path) : rc;
}

bool fm_copy_recursive(const char *src, const char *dst)
{
    if (access(dst, F_OK) == 0) {
        errno = EEXIST;
        return false;
    }
    if (!is_dir(src)) {
        uint64_t bytes;
        return copy_file(src, dst, &bytes);
    }
    /* Copying a folder into itself would never end. */
    size_t sl = strlen(src);
    if (strncmp(dst, src, sl) == 0 && dst[sl] == '/') {
        errno = EINVAL;
        return false;
    }
    if (mkdir(dst, 0775) != 0) {
        return false;
    }
    DIR *dir = opendir(src);
    if (!dir) {
        return false;
    }
    char *s = malloc(MAX_PATH_LEN);
    char *d = malloc(MAX_PATH_LEN);
    bool ok = s && d;
    if (!ok) {
        errno = ENOMEM;
    }
    struct dirent *e;
    while (ok && (e = readdir(dir)) != NULL) {
        if (snprintf(s, MAX_PATH_LEN, "%s/%s", src, e->d_name) >= MAX_PATH_LEN ||
            snprintf(d, MAX_PATH_LEN, "%s/%s", dst, e->d_name) >= MAX_PATH_LEN) {
            errno = ENAMETOOLONG;
            ok = false;
            break;
        }
        ok = fm_copy_recursive(s, d);
    }
    closedir(dir);
    free(s);
    free(d);
    return ok;
}

bool fm_card_info(char *name, size_t name_size, uint64_t *total_bytes, uint64_t *free_bytes)
{
    if (!s_card) {
        return false;
    }
    snprintf(name, name_size, "%s", s_card->cid.name);
    return esp_vfs_fat_info(MOUNT_POINT, total_bytes, free_bytes) == ESP_OK;
}

size_t fm_run_capture(const char *line, char *out, size_t max)
{
    if (!max) return 0;
    out[0] = '\0';
    char cmd[MAX_CMD_LEN + 1];
    strlcpy(cmd, line, sizeof(cmd));
    xSemaphoreTake(s_cmd_lock, portMAX_DELAY);
    s_capture = out;
    s_capture_len = 0;
    s_capture_max = max;
    if (strcmp(cmd, "shot") == 0) {
        reply("use the Screen tab for screenshots");
    } else {
        run_command(cmd);
    }
    s_capture = NULL;
    size_t n = s_capture_len;
    xSemaphoreGive(s_cmd_lock);
    return n;
}

bool fm_mount(void) { return sd_mount(); }
void fm_unmount(void) { sd_unmount(); }

bool fm_format(void)
{
    if (!s_card) {
        return false;
    }
    snprintf(s_cwd, sizeof(s_cwd), "%s", MOUNT_POINT);
    return esp_vfs_fat_sdcard_format(MOUNT_POINT, s_card) == ESP_OK;
}

void file_manager_start(fm_send_fn_t ble_send)
{
    s_ble_send = ble_send;
    s_cmd_lock = xSemaphoreCreateMutex();
    s_queue = xQueueCreate(4, sizeof(command_t));
    sd_mount();

    usb_serial_jtag_driver_config_t usb_cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    usb_cfg.tx_buffer_size = 8192;     /* screenshots stream through here */
    if (usb_serial_jtag_driver_install(&usb_cfg) == ESP_OK) {
        xTaskCreate(usb_reader_task, "fm_usb", 3072, NULL, 4, NULL);
    }
    xTaskCreate(worker_task, "fm_worker", 16384, NULL, 4, NULL);
}
