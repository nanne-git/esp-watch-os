/*
 * Firmware updates: from GitHub Releases (over the internet) or uploaded from the browser.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

typedef enum { OTA_IDLE, OTA_CHECKING, OTA_DOWNLOADING, OTA_DONE, OTA_ERROR } ota_state_t;

typedef struct {
    ota_state_t state;
    int         progress;            /* 0..100 while downloading */
    char        current[32];         /* running version */
    char        latest[32];          /* newest release found ("" = unknown) */
    bool        update_available;
    char        message[96];
} ota_status_t;

void ota_start(void);                /* periodic check if enabled in settings */
void ota_get(ota_status_t *out);
void ota_check_async(void);          /* look for a newer GitHub release */
void ota_install_async(void);        /* download + install the latest release, then reboot */

/* browser upload: begin / write chunks / finish (reboots on success) */
bool ota_upload_begin(size_t total);
bool ota_upload_write(const void *data, size_t len);
bool ota_upload_end(bool ok);
