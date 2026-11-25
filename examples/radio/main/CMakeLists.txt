idf_component_register(
    SRCS "radio_main.c" "radio_ao.c"
    INCLUDE_DIRS "."
    REQUIRES kode fck_sys
)
