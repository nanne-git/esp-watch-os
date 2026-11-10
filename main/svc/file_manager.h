/*
 * SD card file manager.
 *  - text commands from BLE or the USB serial port (ls, cp, mv, rm, ...)
 *  - shared file helpers used by the web server (web_server.c)
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define FM_MAX_PATH 512

/* Function the file manager uses to send replies over Bluetooth. */
typedef bool (*fm_send_fn_t)(const void *data, size_t len);

/* Mount the SD card and start the command worker task (and the USB command reader). */
void file_manager_start(fm_send_fn_t ble_send);

/* Queue one command line received over Bluetooth. Safe to call from the BLE host task. */
void file_manager_submit_ble(const uint8_t *data, size_t len);

/* ---- helpers shared with the web server ---- */

bool fm_is_mounted(void);

/* "/photos/a.jpg" (card root = "/") -> "/sdcard/photos/a.jpg". Rejects paths that leave the card. */
bool fm_card_path(const char *card_path, char *abs_out, size_t out_size);

/* "/sdcard/photos" -> "/photos" */
const char *fm_display_path(const char *abs_path);

bool fm_is_dir(const char *abs_path);

/* Copy a file or a whole folder. Fails if the destination already exists. */
bool fm_copy_recursive(const char *src_abs, const char *dst_abs);

/* Delete a file or a whole folder with everything inside it. Returns 0 on success. */
int fm_remove_recursive(const char *abs_path);

/* web console: run one text command, replies collected into out (NUL-terminated) */
size_t fm_run_capture(const char *line, char *out, size_t max);

bool fm_mount(void);
void fm_unmount(void);
bool fm_format(void);          /* erases the whole card (FAT) */

bool fm_card_info(char *name, size_t name_size, uint64_t *total_bytes, uint64_t *free_bytes);
