idf_component_register(
    SRCS "gnss_main.c" "gnss_ao.c"
    INCLUDE_DIRS "."
    REQUIRES kode fck_sys
)
