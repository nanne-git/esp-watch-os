/*
 * Developer tools: in-memory log, crash (core dump) summary, task monitor, reset reason.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"

void devtools_init(void);              /* call first in app_main: starts capturing logs */

/* log ring buffer: copies log text from absolute position *pos (0 = oldest kept),
 * returns bytes copied and advances *pos */
size_t devlog_read(uint32_t *pos, char *out, size_t max);
uint32_t devlog_end(void);

/* "Power on", "Crash (panic)", "Watchdog", ... and how many boots so far */
const char *dev_reset_reason(void);
uint32_t    dev_boot_count(void);

/* last crash saved in flash (core dump) */
bool  dev_crash_present(void);
cJSON *dev_crash_json(void);           /* task, PC, backtrace, cause; NULL if none */
bool  dev_crash_image(size_t *addr, size_t *size);
void  dev_crash_erase(void);

/* tasks: name, CPU % since the previous call, free stack, priority, core */
cJSON *dev_tasks_json(void);

/* LVGL FPS/CPU overlay */
void dev_perf_overlay(bool on);
bool dev_perf_overlay_on(void);
