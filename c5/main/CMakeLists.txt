idf_component_register(SRCS "main.c" "fck_c5_ce.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES esp_driver_uart esp_wifi esp_event esp_timer nvs_flash esp_app_format)

# The Kode's SDK knows the official firmware by its name and reads up to 23
# characters of it (fck_c5_parse_info()). A name it would take for the official firmware,
# or misread, stops the build in main.c rather than here, so that
# idf.py menuconfig still opens to change it.
string(LENGTH "${CONFIG_FCK_C5_FW_NAME}" fw_name_len)
if(CONFIG_FCK_C5_FW_NAME STREQUAL "fck-c5" OR CONFIG_FCK_C5_FW_NAME STREQUAL "c5_fck_agent"
   OR fw_name_len GREATER 23
   OR NOT CONFIG_FCK_C5_FW_NAME MATCHES "^[A-Za-z0-9_.-]+$")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE FCK_C5_FW_NAME_BAD=1)
endif()
