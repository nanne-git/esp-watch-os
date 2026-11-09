idf_component_register(SRC_DIRS "." "svc" "hw" "ui" "apps"
                       INCLUDE_DIRS "." "svc" "hw" "ui" "apps"
                       EMBED_FILES "www/index.html")
# snprintf into fixed label buffers truncates on purpose
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-format-truncation)
