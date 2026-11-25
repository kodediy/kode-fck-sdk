idf_component_register(
    SRCS "wifi_main.c" "wifi_ao.c"
    INCLUDE_DIRS "."
    REQUIRES kode fck_sys
)
