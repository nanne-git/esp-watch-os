/*
 * HTTP API (all paths are card paths, "/" = card root, URL-encoded):
 *
 *   GET  /                              the file manager page (www/index.html)
 *   GET  /api/info                      card name, total and free space
 *   GET  /api/list?path=/dir            folder contents as JSON
 *   GET  /api/file?path=/a.jpg          download / view a file
 *   POST /api/upload?path=/dir/a.jpg    request body = file data (&overwrite=1 to replace)
 *   POST /api/mkdir?path=/new
 *   POST /api/delete?path=/x            file or folder (with contents)
 *   POST /api/move?from=/a&to=/b        move or rename
 *   POST /api/copy?from=/a&to=/b        copy file or folder
 *
 * Replies: {"ok":true,...} or {"ok":false,"error":"..."} with a matching HTTP status.
 */
#include "web_server.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "lwip/sockets.h"
#include "file_manager.h"
#include "ui.h"
#include "alarm.h"
#include "devtools.h"
#include "lua_apps.h"
#include "esp_flash.h"
#include "ble_svc.h"
#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "hw.h"
#include "mbedtls/base64.h"
#include "ota.h"
#include "phone.h"
#include "settings.h"
#include "wifi_svc.h"

static const char *TAG = "WEB";

#define IO_BUF_SIZE (16 * 1024)

extern const char index_html_start[] asm("_binary_index_html_start");
extern const char index_html_end[]   asm("_binary_index_html_end");

/* ------------------------------------------------------------------ helpers */

static esp_err_t send_error(httpd_req_t *req, const char *status, const char *msg)
{
    char body[160];
    snprintf(body, sizeof(body), "{\"ok\":false,\"error\":\"%s\"}", msg);
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, body);
}

static esp_err_t send_errno(httpd_req_t *req)
{
    int e = errno;
    const char *status = e == ENOENT ? "404 Not Found"
                       : e == EEXIST ? "409 Conflict"
                       : e == ENOSPC ? "507 Insufficient Storage"
                       : "500 Internal Server Error";
    return send_error(req, status, strerror(e));
}

static esp_err_t send_ok(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

/* "%20" -> " ", "%C3%A9" -> UTF-8 bytes, in place. */
static void url_decode(char *s)
{
    char *out = s;
    while (*s) {
        if (s[0] == '%' && isxdigit((unsigned char)s[1]) && isxdigit((unsigned char)s[2])) {
            char hex[3] = { s[1], s[2], 0 };
            *out++ = (char)strtol(hex, NULL, 16);
            s += 3;
        } else if (*s == '+') {
            *out++ = ' ';
            s++;
        } else {
            *out++ = *s++;
        }
    }
    *out = '\0';
}

static bool get_query_value(httpd_req_t *req, const char *key, char *out, size_t out_size)
{
    size_t qlen = httpd_req_get_url_query_len(req);
    if (qlen == 0) {
        return false;
    }
    char *query = malloc(qlen + 1);
    if (!query) {
        return false;
    }
    bool ok = httpd_req_get_url_query_str(req, query, qlen + 1) == ESP_OK &&
              httpd_query_key_value(query, key, out, out_size) == ESP_OK;
    free(query);
    if (ok) {
        url_decode(out);
    }
    return ok;
}

/* Read ?key=/card/path from the URL and turn it into "/sdcard/card/path". */
static bool get_path(httpd_req_t *req, const char *key, char *abs, size_t abs_size)
{
    char raw[FM_MAX_PATH * 3];
    return get_query_value(req, key, raw, sizeof(raw)) && fm_card_path(raw, abs, abs_size);
}

static void send_json_string(httpd_req_t *req, const char *s)
{
    char buf[128];
    size_t n = 0;
    buf[n++] = '"';
    for (; *s; s++) {
        char esc[8];
        size_t len;
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') {
            esc[0] = '\\';
            esc[1] = (char)c;
            len = 2;
        } else if (c < 0x20) {
            len = snprintf(esc, sizeof(esc), "\\u%04x", c);
        } else {
            esc[0] = (char)c;
            len = 1;
        }
        if (n + len >= sizeof(buf)) {
            httpd_resp_send_chunk(req, buf, n);
            n = 0;
        }
        memcpy(buf + n, esc, len);
        n += len;
    }
    buf[n++] = '"';
    httpd_resp_send_chunk(req, buf, n);
}

static const char *mime_type(const char *path)
{
    static const struct { const char *ext, *type; } types[] = {
        { ".html", "text/html" },        { ".htm", "text/html" },
        { ".txt", "text/plain; charset=utf-8" }, { ".log", "text/plain; charset=utf-8" },
        { ".md", "text/plain; charset=utf-8" },  { ".csv", "text/plain; charset=utf-8" },
        { ".json", "application/json" }, { ".pdf", "application/pdf" },
        { ".jpg", "image/jpeg" },        { ".jpeg", "image/jpeg" },
        { ".png", "image/png" },         { ".gif", "image/gif" },
        { ".webp", "image/webp" },       { ".bmp", "image/bmp" },
        { ".svg", "image/svg+xml" },     { ".mp3", "audio/mpeg" },
        { ".wav", "audio/wav" },         { ".mp4", "video/mp4" },
    };
    const char *ext = strrchr(path, '.');
    if (ext) {
        for (size_t i = 0; i < sizeof(types) / sizeof(types[0]); i++) {
            if (strcasecmp(ext, types[i].ext) == 0) {
                return types[i].type;
            }
        }
    }
    return "application/octet-stream";
}

static web_transfer_cb_t s_on_transfer;

static void notify_transfer(void)
{
    if (s_on_transfer) {
        s_on_transfer();
    }
}

#define REQUIRE_CARD(req) \
    do { if (!fm_is_mounted()) return send_error(req, "503 Service Unavailable", "No SD card"); } while (0)

/* ------------------------------------------------------------------ handlers */

static esp_err_t page_index(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, index_html_start, index_html_end - index_html_start);
}

static esp_err_t api_info(httpd_req_t *req)
{
    char name[16] = "", body[192];
    uint64_t total = 0, free_bytes = 0;
    bool ok = fm_card_info(name, sizeof(name), &total, &free_bytes);
    snprintf(body, sizeof(body), "{\"ok\":true,\"mounted\":%s,\"name\":\"%s\",\"total\":%llu,\"free\":%llu}",
             ok ? "true" : "false", name, (unsigned long long)total, (unsigned long long)free_bytes);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, body);
}

static esp_err_t api_list(httpd_req_t *req)
{
    REQUIRE_CARD(req);
    char path[FM_MAX_PATH];
    if (!get_path(req, "path", path, sizeof(path))) {
        return send_error(req, "400 Bad Request", "bad path");
    }
    DIR *dir = opendir(path);
    if (!dir) {
        return send_errno(req);
    }
    char *full = malloc(FM_MAX_PATH * 2);
    if (!full) {
        closedir(dir);
        return send_error(req, "500 Internal Server Error", "out of memory");
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr_chunk(req, "{\"ok\":true,\"path\":");
    send_json_string(req, fm_display_path(path));
    httpd_resp_sendstr_chunk(req, ",\"items\":[");

    bool first = true;
    struct dirent *e;
    while ((e = readdir(dir)) != NULL) {
        bool is_dir = e->d_type == DT_DIR;
        long long size = 0;
        struct stat st;
        snprintf(full, FM_MAX_PATH * 2, "%s/%s", path, e->d_name);
        if (!is_dir && stat(full, &st) == 0) {
            size = st.st_size;
        }
        httpd_resp_sendstr_chunk(req, first ? "{\"name\":" : ",{\"name\":");
        first = false;
        send_json_string(req, e->d_name);
        char tail[64];
        snprintf(tail, sizeof(tail), ",\"dir\":%s,\"size\":%lld}", is_dir ? "true" : "false", size);
        httpd_resp_sendstr_chunk(req, tail);
    }
    free(full);
    closedir(dir);
    httpd_resp_sendstr_chunk(req, "]}");
    return httpd_resp_send_chunk(req, NULL, 0);
}

static esp_err_t api_file(httpd_req_t *req)
{
    REQUIRE_CARD(req);
    char path[FM_MAX_PATH];
    if (!get_path(req, "path", path, sizeof(path)) || fm_is_dir(path)) {
        return send_error(req, "400 Bad Request", "not a file");
    }
    FILE *f = fopen(path, "rb");
    if (!f) {
        return send_errno(req);
    }
    char *buf = malloc(IO_BUF_SIZE);
    if (!buf) {
        fclose(f);
        return send_error(req, "500 Internal Server Error", "out of memory");
    }
    httpd_resp_set_type(req, mime_type(path));

    /* Chunked replies mix tiny writes (chunk headers) with big ones. Without TCP_NODELAY,
     * Nagle's algorithm waits for the browser's (delayed) ACK after every chunk. */
    int one = 1;
    setsockopt(httpd_req_to_sockfd(req), IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    notify_transfer();

    esp_err_t err = ESP_OK;
    size_t n, total = 0;
    int64_t t0 = esp_timer_get_time();
    while ((n = fread(buf, 1, IO_BUF_SIZE, f)) > 0) {
        if ((err = httpd_resp_send_chunk(req, buf, n)) != ESP_OK) {
            ESP_LOGW(TAG, "Download of %s aborted by browser", fm_display_path(path));
            break;
        }
        total += n;
        if ((total & ((1 << 20) - 1)) < IO_BUF_SIZE) {
            notify_transfer();                /* long download: keep BLE advertising paused */
        }
    }
    double secs = (esp_timer_get_time() - t0) / 1e6;
    ESP_LOGI(TAG, "Sent %s: %u KB in %.1f s (%.0f KB/s)", fm_display_path(path),
             (unsigned)(total / 1024), secs, secs > 0 ? total / 1024.0 / secs : 0);
    free(buf);
    fclose(f);
    if (err == ESP_OK) {
        err = httpd_resp_send_chunk(req, NULL, 0);
    }
    return err;
}

static esp_err_t api_upload(httpd_req_t *req)
{
    REQUIRE_CARD(req);
    char path[FM_MAX_PATH], overwrite[4] = "";
    if (!get_path(req, "path", path, sizeof(path)) || strcmp(path, "/sdcard") == 0) {
        return send_error(req, "400 Bad Request", "bad path");
    }
    get_query_value(req, "overwrite", overwrite, sizeof(overwrite));
    if (fm_is_dir(path) || (access(path, F_OK) == 0 && strcmp(overwrite, "1") != 0)) {
        return send_error(req, "409 Conflict", "already exists");
    }

    FILE *f = fopen(path, "wb");
    if (!f) {
        return send_errno(req);
    }
    char *buf = malloc(IO_BUF_SIZE);
    size_t remaining = req->content_len;
    int timeouts = 0;
    bool ok = buf != NULL;
    const char *why = ok ? NULL : "out of memory";

    ESP_LOGI(TAG, "Upload %s (%u bytes)", fm_display_path(path), (unsigned)remaining);
    notify_transfer();
    size_t since_notify = 0;
    while (ok && remaining > 0) {
        if (since_notify >= (1 << 20)) {      /* long upload: keep BLE advertising paused */
            notify_transfer();
            since_notify = 0;
        }
        int r = httpd_req_recv(req, buf, remaining < IO_BUF_SIZE ? remaining : IO_BUF_SIZE);
        if (r == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts < 5) {
            continue;                        /* slow network: wait a bit longer */
        }
        if (r <= 0) {
            ok = false;
            why = "upload interrupted";
            break;
        }
        timeouts = 0;
        if (fwrite(buf, 1, r, f) != (size_t)r) {
            ok = false;
            why = "write failed - card full?";
            break;
        }
        remaining -= r;
        since_notify += r;
    }
    free(buf);
    if (fclose(f) != 0 && ok) {
        ok = false;
        why = "write failed - card full?";
    }
    if (!ok) {
        unlink(path);                        /* don't keep half-uploaded files */
        return send_error(req, "500 Internal Server Error", why);
    }
    return send_ok(req);
}

static esp_err_t api_mkdir(httpd_req_t *req)
{
    REQUIRE_CARD(req);
    char path[FM_MAX_PATH];
    if (!get_path(req, "path", path, sizeof(path))) {
        return send_error(req, "400 Bad Request", "bad path");
    }
    return mkdir(path, 0775) == 0 ? send_ok(req) : send_errno(req);
}

static esp_err_t api_delete(httpd_req_t *req)
{
    REQUIRE_CARD(req);
    char path[FM_MAX_PATH];
    if (!get_path(req, "path", path, sizeof(path)) || strcmp(path, "/sdcard") == 0) {
        return send_error(req, "400 Bad Request", "bad path");
    }
    ESP_LOGI(TAG, "Delete %s", fm_display_path(path));
    return fm_remove_recursive(path) == 0 ? send_ok(req) : send_errno(req);
}

static esp_err_t move_or_copy(httpd_req_t *req, bool move)
{
    REQUIRE_CARD(req);
    char from[FM_MAX_PATH], to[FM_MAX_PATH];
    if (!get_path(req, "from", from, sizeof(from)) || !get_path(req, "to", to, sizeof(to)) ||
        strcmp(from, "/sdcard") == 0 || strcmp(to, "/sdcard") == 0) {
        return send_error(req, "400 Bad Request", "bad path");
    }
    if (access(to, F_OK) == 0) {
        return send_error(req, "409 Conflict", "already exists");
    }
    size_t fl = strlen(from);
    if (strncmp(to, from, fl) == 0 && to[fl] == '/') {
        return send_error(req, "400 Bad Request", "can't put a folder inside itself");
    }
    ESP_LOGI(TAG, "%s %s -> %s", move ? "Move" : "Copy", fm_display_path(from), fm_display_path(to));
    bool ok = move ? rename(from, to) == 0 : fm_copy_recursive(from, to);
    return ok ? send_ok(req) : send_errno(req);
}

static esp_err_t api_move(httpd_req_t *req) { return move_or_copy(req, true); }
static esp_err_t api_copy(httpd_req_t *req) { return move_or_copy(req, false); }

/* ------------------------------------------------------------------ remote screen + input */

static esp_err_t api_screenshot(httpd_req_t *req)
{
    size_t len = 0;
    uint8_t *bmp = ui_screenshot_bmp(&len);
    if (!bmp) {
        return send_error(req, "500 Internal Server Error", "screenshot failed");
    }
    httpd_resp_set_type(req, "image/bmp");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_send(req, (const char *)bmp, len);
    free(bmp);
    return err;
}

static int qint(httpd_req_t *req, const char *key, int def)
{
    char v[16];
    return get_query_value(req, key, v, sizeof(v)) ? atoi(v) : def;
}

/* POST /api/input?type=tap&x=..&y=..  | type=long&x&y | type=swipe&x1&y1&x2&y2 | type=key&k=back|home|power|wake */
static esp_err_t api_input(httpd_req_t *req)
{
    char type[12] = "", key[12] = "";
    get_query_value(req, "type", type, sizeof(type));
    if (strcmp(type, "tap") == 0) {
        ui_inject_tap(qint(req, "x", 0), qint(req, "y", 0));
    } else if (strcmp(type, "long") == 0) {
        ui_inject_long_press(qint(req, "x", 0), qint(req, "y", 0));
    } else if (strcmp(type, "swipe") == 0) {
        ui_inject_swipe(qint(req, "x1", 0), qint(req, "y1", 0), qint(req, "x2", 0), qint(req, "y2", 0));
    } else if (strcmp(type, "key") == 0 && get_query_value(req, "k", key, sizeof(key))) {
        ui_inject_key(key);
    } else {
        return send_error(req, "400 Bad Request", "bad input");
    }
    return send_ok(req);
}


/* ------------------------------------------------------------------ JSON helpers */

static char *read_body(httpd_req_t *req, size_t max)
{
    if (req->content_len == 0 || req->content_len > max) {
        return NULL;
    }
    char *b = malloc(req->content_len + 1);
    size_t got = 0;
    while (b && got < req->content_len) {
        int r = httpd_req_recv(req, b + got, req->content_len - got);
        if (r <= 0) {
            if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
            free(b);
            return NULL;
        }
        got += r;
    }
    if (b) b[got] = '\0';
    return b;
}

static esp_err_t send_cjson(httpd_req_t *req, cJSON *o)
{
    char *txt = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_sendstr(req, txt ? txt : "{}");
    cJSON_free(txt);
    return err;
}

/* ------------------------------------------------------------------ device status */

static esp_err_t api_status(httpd_req_t *req)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddTrueToObject(o, "ok");
    const esp_app_desc_t *app = esp_app_get_description();
    cJSON_AddStringToObject(o, "version", app->version);
    cJSON_AddNumberToObject(o, "uptime", esp_timer_get_time() / 1000000);
    power_status_t p;
    power_get(&p);
    cJSON *b = cJSON_AddObjectToObject(o, "battery");
    cJSON_AddNumberToObject(b, "percent", p.percent);
    cJSON_AddNumberToObject(b, "voltage", p.voltage_mv / 1000.0);
    cJSON_AddBoolToObject(b, "charging", p.charging);
    cJSON_AddBoolToObject(b, "usb", p.usb);
    wifi_status_t w;
    wifi_svc_status(&w);
    cJSON *wf = cJSON_AddObjectToObject(o, "wifi");
    cJSON_AddStringToObject(wf, "ssid", w.ssid);
    cJSON_AddStringToObject(wf, "ip", w.ip);
    cJSON_AddNumberToObject(wf, "rssi", w.rssi);
    cJSON_AddStringToObject(wf, "mac", w.mac);
    cJSON_AddBoolToObject(wf, "hotspot", w.ap_on);
    cJSON_AddBoolToObject(o, "phone_connected", ble_svc_connected());
    cJSON_AddNumberToObject(o, "steps", imu_steps_today());
    cJSON_AddNumberToObject(o, "notifications", notif_count());
    cJSON_AddNumberToObject(o, "free_ram", heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(o, "free_psram", heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    cJSON_AddBoolToObject(o, "screen_on", ui_is_awake());
    char name[16] = "";
    uint64_t total = 0, freeb = 0;
    if (fm_card_info(name, sizeof(name), &total, &freeb)) {
        cJSON *sd = cJSON_AddObjectToObject(o, "sd");
        cJSON_AddStringToObject(sd, "name", name);
        cJSON_AddNumberToObject(sd, "total", (double)total);
        cJSON_AddNumberToObject(sd, "free", (double)freeb);
    }
    return send_cjson(req, o);
}

/* ------------------------------------------------------------------ settings */

static esp_err_t api_settings_get(httpd_req_t *req)
{
    return send_cjson(req, settings_to_json());
}

static esp_err_t api_settings_set(httpd_req_t *req)
{
    char *body = read_body(req, 4096);
    cJSON *o = body ? cJSON_Parse(body) : NULL;
    free(body);
    if (!o) {
        return send_error(req, "400 Bad Request", "invalid JSON");
    }
    int n = settings_from_json(o);
    cJSON_Delete(o);
    cJSON *r = cJSON_CreateObject();
    cJSON_AddTrueToObject(r, "ok");
    cJSON_AddNumberToObject(r, "changed", n);
    return send_cjson(req, r);
}

/* ------------------------------------------------------------------ notifications */

static esp_err_t api_notify(httpd_req_t *req)
{
    char *body = read_body(req, 2048);
    cJSON *o = body ? cJSON_Parse(body) : NULL;
    free(body);
    if (!o) {
        return send_error(req, "400 Bad Request", "invalid JSON");
    }
    notif_t n = { 0 };
    n.id = esp_random();
    const cJSON *v;
    strlcpy(n.app, (v = cJSON_GetObjectItem(o, "app")) && cJSON_IsString(v) ? v->valuestring : "Web", sizeof(n.app));
    strlcpy(n.title, (v = cJSON_GetObjectItem(o, "title")) && cJSON_IsString(v) ? v->valuestring : "", sizeof(n.title));
    strlcpy(n.body, (v = cJSON_GetObjectItem(o, "body")) && cJSON_IsString(v) ? v->valuestring : "", sizeof(n.body));
    cJSON_Delete(o);
    notif_add(&n);
    return send_ok(req);
}

static esp_err_t api_notifications(httpd_req_t *req)
{
    cJSON *a = cJSON_CreateArray();
    for (int i = 0; i < notif_count(); i++) {
        notif_t n;
        if (!notif_get(i, &n)) continue;
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "id", n.id);
        cJSON_AddNumberToObject(o, "when", (double)n.when);
        cJSON_AddStringToObject(o, "app", n.app);
        cJSON_AddStringToObject(o, "title", n.title);
        cJSON_AddStringToObject(o, "body", n.body);
        cJSON_AddItemToArray(a, o);
    }
    return send_cjson(req, a);
}

static esp_err_t api_notif_clear(httpd_req_t *req)
{
    notif_clear();
    return send_ok(req);
}

/* ------------------------------------------------------------------ alarms */

static esp_err_t api_alarms(httpd_req_t *req)
{
    cJSON *a = cJSON_CreateArray();
    for (int i = 0; i < alarm_count(); i++) {
        alarm_t al;
        alarm_get(i, &al);
        cJSON *o = cJSON_CreateObject();
        cJSON_AddBoolToObject(o, "enabled", al.enabled);
        cJSON_AddNumberToObject(o, "hour", al.hour);
        cJSON_AddNumberToObject(o, "min", al.min);
        cJSON_AddNumberToObject(o, "days", al.days);
        cJSON_AddStringToObject(o, "label", al.label);
        cJSON_AddItemToArray(a, o);
    }
    return send_cjson(req, a);
}

/* POST /api/alarms  body {"hour":7,"min":30,"days":62,"label":"Work","enabled":true}  (?i=N edits N) */
static esp_err_t api_alarm_set(httpd_req_t *req)
{
    char *body = read_body(req, 1024);
    cJSON *o = body ? cJSON_Parse(body) : NULL;
    free(body);
    if (!o) {
        return send_error(req, "400 Bad Request", "invalid JSON");
    }
    alarm_t al = { .enabled = true };
    const cJSON *v;
    if ((v = cJSON_GetObjectItem(o, "hour")) && cJSON_IsNumber(v)) al.hour = v->valueint % 24;
    if ((v = cJSON_GetObjectItem(o, "min")) && cJSON_IsNumber(v)) al.min = v->valueint % 60;
    if ((v = cJSON_GetObjectItem(o, "days")) && cJSON_IsNumber(v)) al.days = v->valueint & 0x7F;
    if ((v = cJSON_GetObjectItem(o, "enabled")) && cJSON_IsBool(v)) al.enabled = cJSON_IsTrue(v);
    if ((v = cJSON_GetObjectItem(o, "label")) && cJSON_IsString(v)) strlcpy(al.label, v->valuestring, sizeof(al.label));
    cJSON_Delete(o);
    char idx[8];
    if (get_query_value(req, "i", idx, sizeof(idx))) {
        alarm_set(atoi(idx), &al);
    } else if (alarm_add(&al) < 0) {
        return send_error(req, "409 Conflict", "too many alarms");
    }
    ui_on_data_changed();
    return send_ok(req);
}

static esp_err_t api_alarm_delete(httpd_req_t *req)
{
    char idx[8];
    if (!get_query_value(req, "i", idx, sizeof(idx))) {
        return send_error(req, "400 Bad Request", "missing i");
    }
    alarm_delete(atoi(idx));
    ui_on_data_changed();
    return send_ok(req);
}

/* ------------------------------------------------------------------ updates */

static esp_err_t api_ota_status(httpd_req_t *req)
{
    ota_status_t s;
    ota_get(&s);
    cJSON *o = cJSON_CreateObject();
    static const char *states[] = { "idle", "checking", "downloading", "done", "error" };
    cJSON_AddStringToObject(o, "state", states[s.state]);
    cJSON_AddNumberToObject(o, "progress", s.progress);
    cJSON_AddStringToObject(o, "current", s.current);
    cJSON_AddStringToObject(o, "latest", s.latest);
    cJSON_AddBoolToObject(o, "update_available", s.update_available);
    cJSON_AddStringToObject(o, "message", s.message);
    return send_cjson(req, o);
}

static esp_err_t api_ota_check(httpd_req_t *req) { ota_check_async(); return send_ok(req); }
static esp_err_t api_ota_install(httpd_req_t *req) { ota_install_async(); return send_ok(req); }

/* POST /api/ota/upload  body = firmware .bin */
static esp_err_t api_ota_upload(httpd_req_t *req)
{
    if (!ota_upload_begin(req->content_len)) {
        return send_error(req, "409 Conflict", "update busy or file too big");
    }
    notify_transfer();
    char *buf = malloc(IO_BUF_SIZE);
    size_t left = req->content_len;
    bool ok = buf != NULL;
    int timeouts = 0;
    while (ok && left) {
        int r = httpd_req_recv(req, buf, left < IO_BUF_SIZE ? left : IO_BUF_SIZE);
        if (r == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts < 5) continue;
        if (r <= 0) { ok = false; break; }
        timeouts = 0;
        ok = ota_upload_write(buf, r);
        left -= r;
    }
    free(buf);
    if (!ota_upload_end(ok)) {
        return send_error(req, "400 Bad Request", "not a valid firmware image");
    }
    return send_ok(req);
}

static esp_err_t api_reboot(httpd_req_t *req)
{
    send_ok(req);
    vTaskDelay(pdMS_TO_TICKS(500));
    power_restart();
    return ESP_OK;
}


/* ------------------------------------------------------------------ developer */

/* GET /api/logs?from=N  -> log text since position N; header X-Log-Pos = next position */
static esp_err_t api_logs(httpd_req_t *req)
{
    char v[16];
    uint32_t pos = get_query_value(req, "from", v, sizeof(v)) ? strtoul(v, NULL, 10) : 0;
    char *buf = malloc(16384);
    if (!buf) return send_error(req, "500 Internal Server Error", "out of memory");
    size_t n = devlog_read(&pos, buf, 16384);
    char hdr[16];
    snprintf(hdr, sizeof(hdr), "%lu", (unsigned long)pos);
    httpd_resp_set_hdr(req, "X-Log-Pos", hdr);
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    esp_err_t err = httpd_resp_send(req, buf, n);
    free(buf);
    return err;
}

/* ---- Lua apps ---- */
static esp_err_t api_apps(httpd_req_t *req)
{
    return send_cjson(req, lua_apps_json());
}

static esp_err_t api_apps_run(httpd_req_t *req)
{
    char id[40];
    if (!get_query_value(req, "id", id, sizeof(id))) return send_error(req, "400 Bad Request", "id missing");
    return lua_apps_run(id) ? send_ok(req) : send_error(req, "404 Not Found", "no such app (needs /apps/<id>/main.lua)");
}

static esp_err_t api_apps_stop(httpd_req_t *req)
{
    lua_apps_stop();
    return send_ok(req);
}

static esp_err_t api_apps_delete(httpd_req_t *req)
{
    char id[40];
    if (!get_query_value(req, "id", id, sizeof(id))) return send_error(req, "400 Bad Request", "id missing");
    return lua_apps_remove(id) ? send_ok(req) : send_error(req, "500 Internal Server Error", "could not remove");
}

static esp_err_t api_tasks(httpd_req_t *req)
{
    cJSON *a = dev_tasks_json();
    return a ? send_cjson(req, a) : send_error(req, "500 Internal Server Error", "out of memory");
}

static esp_err_t api_dev(httpd_req_t *req)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "reset_reason", dev_reset_reason());
    cJSON_AddNumberToObject(o, "boots", dev_boot_count());
    cJSON_AddBoolToObject(o, "perf_overlay", dev_perf_overlay_on());
    cJSON_AddNumberToObject(o, "min_free_ram", heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(o, "largest_ram_block", heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    cJSON *crash = dev_crash_json();
    if (crash) cJSON_AddItemToObject(o, "crash", crash);
    return send_cjson(req, o);
}

static esp_err_t api_dev_perf(httpd_req_t *req)
{
    char v[4] = "0";
    get_query_value(req, "on", v, sizeof(v));
    dev_perf_overlay(v[0] == '1');
    return send_ok(req);
}

/* download the raw core dump: decode on the laptop with
 *   idf.py coredump-info -c coredump.bin   (same firmware build) */
static esp_err_t api_crash_download(httpd_req_t *req)
{
    size_t addr = 0, size = 0;
    if (!dev_crash_image(&addr, &size)) {
        return send_error(req, "404 Not Found", "no crash report");
    }
    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=\"coredump.bin\"");
    char *buf = malloc(4096);
    esp_err_t err = buf ? ESP_OK : ESP_ERR_NO_MEM;
    for (size_t off = 0; err == ESP_OK && off < size; off += 4096) {
        size_t n = size - off < 4096 ? size - off : 4096;
        err = esp_flash_read(NULL, buf, addr + off, n);
        if (err == ESP_OK) err = httpd_resp_send_chunk(req, buf, n);
    }
    free(buf);
    if (err == ESP_OK) err = httpd_resp_send_chunk(req, NULL, 0);
    return err;
}

static esp_err_t api_crash_erase(httpd_req_t *req)
{
    dev_crash_erase();
    return send_ok(req);
}

/* POST /api/console  body = one command line (same commands as USB / BLE) */
static esp_err_t api_console(httpd_req_t *req)
{
    char *body = read_body(req, 240);
    if (!body) return send_error(req, "400 Bad Request", "empty command");
    char *out = malloc(8192);
    if (!out) {
        free(body);
        return send_error(req, "500 Internal Server Error", "out of memory");
    }
    fm_run_capture(body, out, 8192);
    free(body);
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    esp_err_t err = httpd_resp_sendstr(req, out[0] ? out : "(no output)");
    free(out);
    return err;
}

/* ------------------------------------------------------------------ auth wrapper */

static bool auth_ok(httpd_req_t *req)
{
    const char *pass = settings()->web_pass;
    if (!pass[0]) {
        return true;
    }
    char hdr[160];
    if (httpd_req_get_hdr_value_str(req, "Authorization", hdr, sizeof(hdr)) != ESP_OK ||
        strncmp(hdr, "Basic ", 6) != 0) {
        return false;
    }
    unsigned char dec[128];
    size_t n = 0;
    if (mbedtls_base64_decode(dec, sizeof(dec) - 1, &n, (const unsigned char *)hdr + 6, strlen(hdr + 6)) != 0) {
        return false;
    }
    dec[n] = '\0';
    const char *colon = strchr((char *)dec, ':');       /* "user:password", any user name */
    return colon && strcmp(colon + 1, pass) == 0;
}

static esp_err_t guarded(httpd_req_t *req)
{
    if (!settings()->web_on) {
        return send_error(req, "403 Forbidden", "web manager is turned off on the watch");
    }
    if (!auth_ok(req)) {
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"ESP-Watch\"");
        return httpd_resp_sendstr(req, "password required");
    }
    esp_err_t (*real)(httpd_req_t *) = req->user_ctx;
    return real(req);
}

/* ------------------------------------------------------------------ start */

void web_server_start(web_transfer_cb_t on_transfer)
{
    s_on_transfer = on_transfer;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 48;
    config.stack_size = 10240;
    config.lru_purge_enable = true;
    config.recv_wait_timeout = 10;
    config.send_wait_timeout = 10;

    httpd_handle_t server = NULL;
    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Web server failed to start");
        return;
    }
    const httpd_uri_t routes[] = {
        { .uri = "/",           .method = HTTP_GET,  .handler = page_index },
        { .uri = "/api/info",   .method = HTTP_GET,  .handler = api_info },
        { .uri = "/api/list",   .method = HTTP_GET,  .handler = api_list },
        { .uri = "/api/file",   .method = HTTP_GET,  .handler = api_file },
        { .uri = "/api/upload", .method = HTTP_POST, .handler = api_upload },
        { .uri = "/api/mkdir",  .method = HTTP_POST, .handler = api_mkdir },
        { .uri = "/api/delete", .method = HTTP_POST, .handler = api_delete },
        { .uri = "/api/move",   .method = HTTP_POST, .handler = api_move },
        { .uri = "/api/copy",   .method = HTTP_POST, .handler = api_copy },
        { .uri = "/api/screenshot.bmp", .method = HTTP_GET, .handler = api_screenshot },
        { .uri = "/api/input",  .method = HTTP_POST, .handler = api_input },
        { .uri = "/api/status", .method = HTTP_GET,  .handler = api_status },
        { .uri = "/api/settings", .method = HTTP_GET, .handler = api_settings_get },
        { .uri = "/api/settings", .method = HTTP_POST, .handler = api_settings_set },
        { .uri = "/api/notify", .method = HTTP_POST, .handler = api_notify },
        { .uri = "/api/notifications", .method = HTTP_GET, .handler = api_notifications },
        { .uri = "/api/notifications/clear", .method = HTTP_POST, .handler = api_notif_clear },
        { .uri = "/api/alarms", .method = HTTP_GET, .handler = api_alarms },
        { .uri = "/api/alarms", .method = HTTP_POST, .handler = api_alarm_set },
        { .uri = "/api/alarms/delete", .method = HTTP_POST, .handler = api_alarm_delete },
        { .uri = "/api/ota", .method = HTTP_GET, .handler = api_ota_status },
        { .uri = "/api/ota/check", .method = HTTP_POST, .handler = api_ota_check },
        { .uri = "/api/ota/install", .method = HTTP_POST, .handler = api_ota_install },
        { .uri = "/api/ota/upload", .method = HTTP_POST, .handler = api_ota_upload },
        { .uri = "/api/reboot", .method = HTTP_POST, .handler = api_reboot },
        { .uri = "/api/logs", .method = HTTP_GET, .handler = api_logs },
        { .uri = "/api/tasks", .method = HTTP_GET, .handler = api_tasks },
        { .uri = "/api/dev", .method = HTTP_GET, .handler = api_dev },
        { .uri = "/api/dev/perf", .method = HTTP_POST, .handler = api_dev_perf },
        { .uri = "/api/crash/download", .method = HTTP_GET, .handler = api_crash_download },
        { .uri = "/api/crash/erase", .method = HTTP_POST, .handler = api_crash_erase },
        { .uri = "/api/console", .method = HTTP_POST, .handler = api_console },
        { .uri = "/api/apps", .method = HTTP_GET, .handler = api_apps },
        { .uri = "/api/apps/run", .method = HTTP_POST, .handler = api_apps_run },
        { .uri = "/api/apps/stop", .method = HTTP_POST, .handler = api_apps_stop },
        { .uri = "/api/apps/delete", .method = HTTP_POST, .handler = api_apps_delete },
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        httpd_uri_t r = routes[i];
        r.user_ctx = (void *)r.handler;      /* every route checks password / on-off first */
        r.handler = guarded;
        httpd_register_uri_handler(server, &r);
    }
    ESP_LOGI(TAG, "Web server running on port %d", config.server_port);
}
