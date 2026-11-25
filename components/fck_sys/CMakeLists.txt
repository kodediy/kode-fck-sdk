# fck_sys - the extended layer: fck_sys.h, for an app's Active Objects. Every request is carried
# out on a task of this component through the core of the fck component (fck_core.h, which no app
# includes) and answered to the asking Active Object with a signal. It names no driver
# type and no QP/C beyond what kode_sys.h hands an app.
idf_component_register(
    SRCS "src/fck_sys.c"
    INCLUDE_DIRS "include"
    REQUIRES fck kode kode_sys
    PRIV_REQUIRES fck_aos fck_bsp freertos
)
# fck_core.h is for the fck component and this one alone; fck_aos.h for these two and the owner.
target_compile_definitions(${COMPONENT_LIB} PRIVATE FCK_CORE_ALLOWED FCK_AOS_ALLOWED)
