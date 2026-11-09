#include "ota.h"
#include <string.h>
#include "esp_app_desc.h"
static ota_status_t s_st;
void ota_start(void) { strlcpy(s_st.current, esp_app_get_description()->version, sizeof(s_st.current)); }
void ota_get(ota_status_t *o) { *o = s_st; }
void ota_check_async(void) {}
void ota_install_async(void) {}
bool ota_upload_begin(size_t t) { return false; }
bool ota_upload_write(const void *d, size_t l) { return false; }
bool ota_upload_end(bool ok) { return false; }
