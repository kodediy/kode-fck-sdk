idf_component_register(
    SRCS "subghz_main.c" "subghz_arm.c"
    INCLUDE_DIRS "."
    REQUIRES kode kode_sys fck
)
