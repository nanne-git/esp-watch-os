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
#include "esp_timer.h"
#include "lwip/sockets.h"
#include "file_manager.h"
#include "ui.h"

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

/* ------------------------------------------------------------------ start */

void web_server_start(web_transfer_cb_t on_transfer)
{
    s_on_transfer = on_transfer;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 40;
    config.stack_size = 16384;
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
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        httpd_register_uri_handler(server, &routes[i]);
    }
    ESP_LOGI(TAG, "Web server running on port %d", config.server_port);
}
