/*
 * LVGL allocator: put every LVGL object, string and draw layer in PSRAM (8 MB).
 * With the C-library malloc, small LVGL allocations land in internal RAM, which
 * Wi-Fi and Bluetooth need - big pages could exhaust it.
 */
#include <string.h>

#include "esp_heap_caps.h"
#include "lvgl.h"

#define CAPS (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)

void lv_mem_init(void) {}
void lv_mem_deinit(void) {}
lv_mem_pool_t lv_mem_add_pool(void *mem, size_t bytes) { return NULL; }
void lv_mem_remove_pool(lv_mem_pool_t pool) {}

void *lv_malloc_core(size_t size)
{
    void *p = heap_caps_malloc(size, CAPS);
    return p ? p : heap_caps_malloc(size, MALLOC_CAP_8BIT);     /* fall back to internal */
}

void *lv_realloc_core(void *p, size_t new_size)
{
    void *n = heap_caps_realloc(p, new_size, CAPS);
    return n ? n : heap_caps_realloc(p, new_size, MALLOC_CAP_8BIT);
}

void lv_free_core(void *p)
{
    heap_caps_free(p);
}

void lv_mem_monitor_core(lv_mem_monitor_t *mon)
{
    memset(mon, 0, sizeof(*mon));
    mon->total_size = heap_caps_get_total_size(CAPS);
    mon->free_size = heap_caps_get_free_size(CAPS);
    mon->free_biggest_size = heap_caps_get_largest_free_block(CAPS);
    mon->used_pct = mon->total_size ? 100 - (mon->free_size * 100 / mon->total_size) : 0;
}

lv_result_t lv_mem_test_core(void)
{
    return LV_RESULT_OK;
}
