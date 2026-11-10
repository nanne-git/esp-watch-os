/*
 * Network console on TCP port 23 - the same commands as the USB console and the web console.
 *
 *   nc esp-watch.local 23        (or: telnet esp-watch.local)
 *
 * Asks for the web password if one is set. One client at a time. Extra commands:
 *   log     follow the live log (press Enter to stop)
 *   exit    disconnect
 * Only runs while "Web & remote" is on. Plain text like the web UI: home network only.
 */
#include "net_console.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

#include "devtools.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "file_manager.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "settings.h"

static const char *TAG = "netcon";

#define PORT     23
#define OUT_SIZE 8192

static void send_str(int s, const char *txt)
{
    size_t n = strlen(txt);
    while (n) {
        int w = send(s, txt, n, 0);
        if (w <= 0) return;
        txt += w;
        n -= w;
    }
}

/* read one line, dropping telnet negotiation (IAC ...). Returns length or -1 when closed. */
static int read_line(int s, char *buf, int max)
{
    int n = 0, iac = 0;
    while (true) {
        unsigned char c;
        int r = recv(s, &c, 1, 0);
        if (r <= 0) return -1;
        if (iac) {                 /* IAC <cmd> <opt>: skip two bytes */
            iac--;
            continue;
        }
        if (c == 0xFF) { iac = 2; continue; }
        if (c == '\n' || c == '\r') {
            if (n == 0 && c == '\n') continue;   /* "\r\n": ignore the \n after \r */
            buf[n] = '\0';
            return n;
        }
        if (c == 0x08 || c == 0x7F) { if (n) n--; continue; }
        if (c >= 0x20 && n < max - 1) buf[n++] = (char)c;
    }
}

/* stream new log text until the user presses Enter */
static void follow_log(int s)
{
    send_str(s, "-- live log, press Enter to stop --\r\n");
    uint32_t pos = devlog_end() > 4096 ? devlog_end() - 4096 : 0;
    char *buf = malloc(1024);
    char *crlf = malloc(2100);
    if (!buf || !crlf) goto out;
    struct timeval tv = { .tv_sec = 0, .tv_usec = 300000 };
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    while (true) {
        size_t n;
        while ((n = devlog_read(&pos, buf, 1023)) > 0) {
            size_t k = 0;
            for (size_t i = 0; i < n; i++) {
                if (buf[i] == '\n') crlf[k++] = '\r';
                crlf[k++] = buf[i];
            }
            crlf[k] = '\0';
            send_str(s, crlf);
        }
        char c;
        int r = recv(s, &c, 1, 0);
        if (r == 0 || (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) break;
        if (r == 1 && (c == '\r' || c == '\n')) break;
    }
    tv.tv_sec = 120;                 /* back to the session timeout (0 would mean "forever") */
    tv.tv_usec = 0;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
out:
    free(buf);
    free(crlf);
}

static void session(int s)
{
    char line[244];
    send_str(s, "\r\nESP-Watch console. Type 'help', 'log' (live log) or 'exit'.\r\n");
    const char *pass = settings()->web_pass;
    if (pass[0]) {
        for (int tries = 0;; tries++) {
            send_str(s, "Password: ");
            if (read_line(s, line, sizeof(line)) < 0) return;
            if (strcmp(line, pass) == 0) break;
            if (tries >= 2) {
                send_str(s, "Wrong password.\r\n");
                vTaskDelay(pdMS_TO_TICKS(2000));
                return;
            }
        }
    }
    char *out = heap_caps_malloc(OUT_SIZE, MALLOC_CAP_SPIRAM);
    char *crlf = heap_caps_malloc(OUT_SIZE * 2, MALLOC_CAP_SPIRAM);
    if (!out || !crlf) goto done;
    while (settings()->web_on) {
        send_str(s, "watch> ");
        if (read_line(s, line, sizeof(line)) < 0) break;
        if (!line[0]) continue;
        if (strcmp(line, "exit") == 0 || strcmp(line, "quit") == 0) break;
        if (strcmp(line, "log") == 0) {
            follow_log(s);
            continue;
        }
        ESP_LOGI(TAG, "> %s", line);
        fm_run_capture(line, out, OUT_SIZE);
        size_t k = 0;
        for (size_t i = 0; out[i] && k < OUT_SIZE * 2 - 3; i++) {
            if (out[i] == '\n') crlf[k++] = '\r';
            crlf[k++] = out[i];
        }
        crlf[k] = '\0';
        send_str(s, crlf[0] ? crlf : "(no output)\r\n");
    }
done:
    free(out);
    free(crlf);
    send_str(s, "bye\r\n");
}

static void console_task(void *arg)
{
    while (true) {
        if (!settings()->web_on) {
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }
        int ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (ls < 0) {
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }
        int yes = 1;
        setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
        struct sockaddr_in addr = { .sin_family = AF_INET, .sin_port = htons(PORT), .sin_addr.s_addr = htonl(INADDR_ANY) };
        if (bind(ls, (struct sockaddr *)&addr, sizeof(addr)) != 0 || listen(ls, 1) != 0) {
            close(ls);
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }
        ESP_LOGI(TAG, "listening on port %d", PORT);
        while (settings()->web_on) {
            struct sockaddr_in peer;
            socklen_t plen = sizeof(peer);
            int s = accept(ls, (struct sockaddr *)&peer, &plen);
            if (s < 0) break;
            setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
            /* a silent or vanished client must not block the (single) console forever */
            struct timeval tv = { .tv_sec = 120 };
            setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            setsockopt(s, SOL_SOCKET, SO_KEEPALIVE, &yes, sizeof(yes));
            int idle = 30, intvl = 10, cnt = 3;
            setsockopt(s, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
            setsockopt(s, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof(intvl));
            setsockopt(s, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof(cnt));
            ESP_LOGI(TAG, "client connected");
            session(s);
            shutdown(s, SHUT_RDWR);
            close(s);
            ESP_LOGI(TAG, "client left");
        }
        close(ls);
    }
}

void net_console_start(void)
{
    xTaskCreate(console_task, "netcon", 4096, NULL, 3, NULL);
}
