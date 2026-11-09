/*
 * Wi-Fi manager: saved networks (auto-join strongest), scan, hotspot (AP+STA), status.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define WIFI_SAVED_MAX 5

typedef struct {
    char   ssid[33];
    int8_t rssi;
    bool   secure;
    bool   saved;
} wifi_ap_info_t;

typedef enum { WIFI_ST_OFF, WIFI_ST_IDLE, WIFI_ST_CONNECTING, WIFI_ST_CONNECTED } wifi_state_t;

typedef struct {
    wifi_state_t state;
    char   ssid[33];
    char   ip[16], gw[16], mask[16], dns[16];
    int8_t rssi;
    char   mac[18];
    bool   ap_on;
    char   ap_ssid[33];
    char   ap_ip[16];
    int    ap_clients;
} wifi_status_t;

void wifi_svc_start(void);
void wifi_svc_status(wifi_status_t *out);
int  wifi_svc_scan(wifi_ap_info_t *out, int max);            /* blocking, ~2-4 s */
void wifi_svc_connect(const char *ssid, const char *pass);    /* saves + connects */
void wifi_svc_forget(const char *ssid);
int  wifi_svc_saved(char ssids[][33], int max);
void wifi_svc_set_high_throughput(bool on);                   /* radio awake during transfers */

/* "http://10.0.0.5/" style address of the web manager, or "" if not reachable */
const char *wifi_get_url(void);
