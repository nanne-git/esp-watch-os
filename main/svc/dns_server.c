/*
 * Tiny DNS server for the watch's hotspot: answers EVERY name with the hotspot address
 * (192.168.4.1). Together with the connectivity-check URLs in web_server.c this makes phones
 * believe the hotspot "has internet", so they keep using it (Android otherwise sends traffic
 * over mobile data or drops the Wi-Fi: "ESP-Watch has no Internet access") and
 * http://192.168.4.1 or any name like http://watch/ opens the dashboard.
 */
#include "dns_server.h"

#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>

#include "esp_log.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "dns";

static void dns_task(void *arg)
{
    int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    struct sockaddr_in addr = { .sin_family = AF_INET, .sin_port = htons(53), .sin_addr.s_addr = htonl(INADDR_ANY) };
    if (s < 0 || bind(s, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        ESP_LOGE(TAG, "can't open port 53");
        if (s >= 0) close(s);
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "hotspot DNS running");
    uint8_t buf[300];
    while (true) {
        struct sockaddr_in from;
        socklen_t flen = sizeof(from);
        int n = recvfrom(s, buf, sizeof(buf) - 16, 0, (struct sockaddr *)&from, &flen);
        if (n < 12) continue;
        if (buf[2] & 0x80) continue;                 /* a response, not a query */
        /* answer only from the hotspot subnet (192.168.4.x) - never act as DNS on the home LAN */
        uint32_t ip = ntohl(from.sin_addr.s_addr);
        if ((ip & 0xFFFFFF00) != 0xC0A80400) continue;

        /* find the end of the question (name labels + type + class) */
        int p = 12;
        while (p < n && buf[p]) p += buf[p] + 1;
        p += 5;
        if (p > n) continue;
        /* header: response, recursion available, 1 answer */
        buf[2] = 0x81;
        buf[3] = 0x80;
        buf[6] = 0;
        buf[7] = 1;
        buf[8] = buf[9] = buf[10] = buf[11] = 0;
        uint16_t qtype = (buf[p - 4] << 8) | buf[p - 3];
        int len = p;
        if (qtype == 1) {                            /* A record -> 192.168.4.1 */
            const uint8_t ans[] = { 0xC0, 0x0C, 0, 1, 0, 1, 0, 0, 0, 60, 0, 4, 192, 168, 4, 1 };
            memcpy(buf + p, ans, sizeof(ans));
            len += sizeof(ans);
        } else {                                     /* AAAA etc: no answer */
            buf[7] = 0;
        }
        sendto(s, buf, len, 0, (struct sockaddr *)&from, flen);
    }
}

void dns_server_start(void)
{
    static bool started;
    if (started) return;
    started = true;
    xTaskCreate(dns_task, "dns", 3072, NULL, 3, NULL);
}
