idf_component_register(SRC_DIRS "." "svc" "hw" "ui" "apps"
                       INCLUDE_DIRS "." "svc" "hw" "ui" "apps"
                       EMBED_FILES "www/index.html")
# snprintf into fixed label buffers truncates on purpose
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-format-truncation)
# LVGL's custom allocator lives in main (ui/lv_mem_psram.c): make the linker keep it
target_link_libraries(${COMPONENT_LIB} INTERFACE "-u lv_malloc_core")
