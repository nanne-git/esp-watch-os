#include "devtools.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_core_dump.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "ui.h"

#define LOG_RING_SIZE (64 * 1024)

/* ------------------------------------------------------------------ log capture */

static char          *s_ring;
static volatile uint32_t s_total;          /* bytes ever written (absolute position) */
static portMUX_TYPE   s_mux = portMUX_INITIALIZER_UNLOCKED;
static vprintf_like_t s_orig_vprintf;

static void ring_put(const char *s, size_t n)
{
    if (!s_ring || !n) return;
    if (n > LOG_RING_SIZE) {
        s += n - LOG_RING_SIZE;
        n = LOG_RING_SIZE;
    }
    portENTER_CRITICAL(&s_mux);
    uint32_t w = s_total % LOG_RING_SIZE;
    size_t first = n < LOG_RING_SIZE - w ? n : LOG_RING_SIZE - w;
    memcpy(s_ring + w, s, first);
    memcpy(s_ring, s + first, n - first);
    s_total += n;
    portEXIT_CRITICAL(&s_mux);
}

/* every ESP_LOGx line goes through here: keep a copy, then print as usual */
static int log_vprintf(const char *fmt, va_list ap)
{
    char line[256];
    va_list copy;
    va_copy(copy, ap);
    int n = vsnprintf(line, sizeof(line), fmt, copy);
    va_end(copy);
    if (n > 0) {
        size_t len = n < (int)sizeof(line) ? (size_t)n : sizeof(line) - 1;
        /* strip the colour escape codes the console uses */
        char clean[256];
        size_t c = 0;
        for (size_t i = 0; i < len; i++) {
            if (line[i] == 0x1B) {
                while (i < len && line[i] != 'm') i++;
                continue;
            }
            clean[c++] = line[i];
        }
        ring_put(clean, c);
    }
    return s_orig_vprintf ? s_orig_vprintf(fmt, ap) : vprintf(fmt, ap);
}

uint32_t devlog_end(void) { return s_total; }

size_t devlog_read(uint32_t *pos, char *out, size_t max)
{
    if (!s_ring) return 0;
    portENTER_CRITICAL(&s_mux);
    uint32_t total = s_total;
    uint32_t oldest = total > LOG_RING_SIZE ? total - LOG_RING_SIZE : 0;
    uint32_t p = *pos < oldest ? oldest : *pos;
    if (p > total) p = total;
    size_t n = total - p;
    if (n > max) n = max;
    for (size_t i = 0; i < n; i++) {
        out[i] = s_ring[(p + i) % LOG_RING_SIZE];
    }
    portEXIT_CRITICAL(&s_mux);
    *pos = p + n;
    return n;
}

/* ------------------------------------------------------------------ reset reason / boots */

static uint32_t s_boots;

const char *dev_reset_reason(void)
{
    switch (esp_reset_reason()) {
    case ESP_RST_POWERON:   return "Power on";
    case ESP_RST_SW:        return "Restart (software)";
    case ESP_RST_PANIC:     return "Crash (panic)";
    case ESP_RST_INT_WDT:   return "Watchdog (interrupt)";
    case ESP_RST_TASK_WDT:  return "Watchdog (task)";
    case ESP_RST_WDT:       return "Watchdog";
    case ESP_RST_BROWNOUT:  return "Brown-out (low voltage)";
    case ESP_RST_DEEPSLEEP: return "Wake from deep sleep";
    case ESP_RST_USB:       return "USB reset";
    case ESP_RST_JTAG:      return "JTAG reset";
    case ESP_RST_EXT:       return "External reset";
    default:                return "Unknown";
    }
}

uint32_t dev_boot_count(void) { return s_boots; }

/* ------------------------------------------------------------------ crash (core dump) */

bool dev_crash_present(void)
{
    return esp_core_dump_image_check() == ESP_OK;
}

bool dev_crash_image(size_t *addr, size_t *size)
{
    return esp_core_dump_image_get(addr, size) == ESP_OK;
}

void dev_crash_erase(void)
{
    esp_core_dump_image_erase();
}

cJSON *dev_crash_json(void)
{
    if (!dev_crash_present()) return NULL;
    esp_core_dump_summary_t *sum = calloc(1, sizeof(*sum));
    if (!sum) return NULL;
    cJSON *o = NULL;
    if (esp_core_dump_get_summary(sum) == ESP_OK) {
        o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "task", sum->exc_task);
        char b[24];
        snprintf(b, sizeof(b), "0x%08lx", (unsigned long)sum->exc_pc);
        cJSON_AddStringToObject(o, "pc", b);
        cJSON *bt = cJSON_AddArrayToObject(o, "backtrace");
        for (uint32_t i = 0; i < sum->exc_bt_info.depth && i < 16; i++) {
            snprintf(b, sizeof(b), "0x%08lx", (unsigned long)sum->exc_bt_info.bt[i]);
            cJSON_AddItemToArray(bt, cJSON_CreateString(b));
        }
        cJSON_AddBoolToObject(o, "backtrace_corrupted", sum->exc_bt_info.corrupted);
        snprintf(b, sizeof(b), "%lu", (unsigned long)sum->ex_info.exc_cause);
        cJSON_AddStringToObject(o, "cause", b);
        snprintf(b, sizeof(b), "0x%08lx", (unsigned long)sum->ex_info.exc_vaddr);
        cJSON_AddStringToObject(o, "address", b);
        cJSON_AddStringToObject(o, "elf_sha256", (const char *)sum->app_elf_sha256);
    }
    free(sum);
    return o;
}

/* ------------------------------------------------------------------ tasks */

typedef struct {
    TaskHandle_t h;
    uint32_t     runtime;
} prev_t;

static prev_t   s_prev[48];
static int      s_prev_n;
static uint32_t s_prev_total;

cJSON *dev_tasks_json(void)
{
    UBaseType_t n = uxTaskGetNumberOfTasks();
    TaskStatus_t *ts = calloc(n + 4, sizeof(TaskStatus_t));
    if (!ts) return NULL;
    uint32_t total = 0;
    n = uxTaskGetSystemState(ts, n + 4, &total);
    uint32_t dt = total - s_prev_total;
    cJSON *a = cJSON_CreateArray();
    for (UBaseType_t i = 0; i < n; i++) {
        uint32_t prev = 0;
        for (int j = 0; j < s_prev_n; j++) {
            if (s_prev[j].h == ts[i].xHandle) prev = s_prev[j].runtime;
        }
        /* two cores: 100 % per core, so a task can use up to 100 % of one core */
        float cpu = (s_prev_total && dt) ? (ts[i].ulRunTimeCounter - prev) * 100.0f / dt : 0;
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "name", ts[i].pcTaskName);
        cJSON_AddNumberToObject(o, "cpu", (int)(cpu * 10) / 10.0);
        cJSON_AddNumberToObject(o, "stack_free", ts[i].usStackHighWaterMark);
        cJSON_AddNumberToObject(o, "prio", ts[i].uxCurrentPriority);
        cJSON_AddNumberToObject(o, "core", ts[i].xCoreID > 1 ? -1 : ts[i].xCoreID);
        cJSON_AddItemToArray(a, o);
    }
    s_prev_n = 0;
    for (UBaseType_t i = 0; i < n && s_prev_n < 48; i++) {
        s_prev[s_prev_n].h = ts[i].xHandle;
        s_prev[s_prev_n++].runtime = ts[i].ulRunTimeCounter;
    }
    s_prev_total = total;
    free(ts);
    return a;
}

/* ------------------------------------------------------------------ perf overlay */

static bool s_perf_on;

void dev_perf_overlay(bool on)
{
    s_perf_on = on;
    if (ui_lock()) {
        if (on) {
            lv_sysmon_show_performance(NULL);
        } else {
            lv_sysmon_hide_performance(NULL);
        }
        ui_unlock();
    }
}

bool dev_perf_overlay_on(void) { return s_perf_on; }

/* ------------------------------------------------------------------ init */

void devtools_init(void)
{
    s_ring = heap_caps_calloc(1, LOG_RING_SIZE, MALLOC_CAP_SPIRAM);
    s_orig_vprintf = esp_log_set_vprintf(log_vprintf);

    nvs_handle_t h;
    if (nvs_open("watch", NVS_READWRITE, &h) == ESP_OK) {
        nvs_get_u32(h, "boots", &s_boots);
        s_boots++;
        nvs_set_u32(h, "boots", s_boots);
        nvs_commit(h);
        nvs_close(h);
    }
    ESP_LOGI("dev", "boot #%lu, reset reason: %s%s", (unsigned long)s_boots, dev_reset_reason(),
             dev_crash_present() ? " - crash report saved" : "");
}
