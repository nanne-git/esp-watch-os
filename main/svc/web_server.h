/*
 * Browser file manager: serves www/index.html and a small JSON API for the SD card.
 */
#pragma once

/* Called when an upload/download starts (and every ~1 MB while it runs). */
typedef void (*web_transfer_cb_t)(void);

void web_server_start(web_transfer_cb_t on_transfer);
