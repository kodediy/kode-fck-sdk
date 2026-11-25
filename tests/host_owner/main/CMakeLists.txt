# One component: the runner, the model, the cases, and the real sources under
# test: the owner (fck_aos.c) and the core of the facade (fck.c ... fck_subghz.c), which both layers
# stand on. The standard layer (fck_standard.c, fck_events.c) is tests/host_standard's, and the
# extended layer's (fck_sys.c) tests/host_sys's: they run over a fake owner. Cases are globbed and sources are listed, as in tests/host.
set(SDK_DIR "${CMAKE_CURRENT_LIST_DIR}/../../..")

set(SOURCES_UNDER_TEST
    "${SDK_DIR}/components/fck_aos/fck/src/fck_aos.c"
    "${SDK_DIR}/components/fck_aos/fck/src/fck_plan.c"
    "${SDK_DIR}/components/fck/src/fck.c"
    "${SDK_DIR}/components/fck/src/fck_flash.c"
    "${SDK_DIR}/components/fck/src/fck_rescue.c"
    "${SDK_DIR}/components/fck/src/fck_wifi.c"
    "${SDK_DIR}/components/fck/src/fck_radio.c"
    "${SDK_DIR}/components/fck/src/fck_gnss.c"
    "${SDK_DIR}/components/fck/src/fck_subghz.c"
    "${SDK_DIR}/components/fck_drv/link/src/fck_drv_link.c"
    "${SDK_DIR}/components/fck_drv/c5/src/fck_drv_c5.c"
    "${SDK_DIR}/components/fck_drv/nrf/src/fck_drv_nrf.c"
    "${SDK_DIR}/components/fck_drv/nmea/src/fck_drv_nmea.c"
    "${SDK_DIR}/components/fck_drv/ook/src/fck_drv_ook.c"
    "${SDK_DIR}/components/fck_drv/subghz_scan/src/fck_drv_subghz_scan.c"
    "${SDK_DIR}/components/fck_drv/subghz_file/src/fck_drv_subghz_file.c"
    "${SDK_DIR}/components/fck_drv/subghz_modem/src/fck_drv_subghz_modem.c"
    "${SDK_DIR}/components/fck_drv/subghz_duty/src/fck_drv_subghz_duty.c"
)

file(GLOB CASE_SOURCES "${CMAKE_CURRENT_LIST_DIR}/../cases/test_*.c")
list(SORT CASE_SOURCES)

# kode_status_t: kdefs' one header, from the kode-sdk clone of ../../kode_sdk.cmake
# (from v0.1.0 kdefs declares esp32p4 only, and the component manager refuses it
# for `linux`).
include(${CMAKE_CURRENT_LIST_DIR}/../../kode_sdk.cmake)

idf_component_register(
    SRCS "runner.c"
         "fakes/fake_world.c" "fakes/fake_writers.c" "fakes/fake_cc1101.c" "fakes/fake_fck_ao.c"
         ${CASE_SOURCES}
         ${SOURCES_UNDER_TEST}
    INCLUDE_DIRS "${KODE_SDK_DIR}/components/kdefs/include"
                 "." "fakes"
                 "${SDK_DIR}/components/fck/include"
                 # fck_core.h: the calls these cases make, in kode_status_t.
                 "${SDK_DIR}/components/fck/core/include"
                 # fck_private.h, for the cases that reach the facade's helpers.
                 "${SDK_DIR}/components/fck/src"
                 "${SDK_DIR}/components/fck_aos/core/include"
                 # fck_ao.h, the seam fake_fck_ao.c plays the Active Object behind.
                 "${SDK_DIR}/components/fck_aos/fck/src"
                 "${SDK_DIR}/components/fck_drv/c5/include"
                 "${SDK_DIR}/components/fck_drv/c5_rom/include"
                 "${SDK_DIR}/components/fck_drv/cc1101/include"
                 "${SDK_DIR}/components/fck_drv/link/include"
                 "${SDK_DIR}/components/fck_drv/nmea/include"
                 "${SDK_DIR}/components/fck_drv/nrf/include"
                 "${SDK_DIR}/components/fck_drv/nrf_dfu/include"
                 "${SDK_DIR}/components/fck_drv/ook/include"
                 "${SDK_DIR}/components/fck_drv/pins/include"
                 "${SDK_DIR}/components/fck_drv/smp/include"
                 "${SDK_DIR}/components/fck_drv/subghz_duty/include"
                 "${SDK_DIR}/components/fck_drv/subghz_file/include"
                 "${SDK_DIR}/components/fck_drv/subghz_modem/include"
                 "${SDK_DIR}/components/fck_drv/subghz_scan/include"
                 "${SDK_DIR}/components/fck_drv/system/include"
                 "${SDK_DIR}/components/fck_drv/tca6408a/include"
                 "${SDK_DIR}/components/fck_drv/uart/include"
                 "${SDK_DIR}/components/fck_bsp/board/include"
    REQUIRES unity freertos
    WHOLE_ARCHIVE
)

# The bounds of a caller's wait, short enough for a case to reach them: the owner's
# longest job (a send of 800 ms among them) and the grace for its step.
target_compile_definitions(${COMPONENT_LIB} PRIVATE FCK_AOS_JOB_MS_MAX=1500U FCK_AOS_ANSWER_GRACE_MS=200U
                           FCK_CORE_ALLOWED FCK_AOS_ALLOWED)

# The host port has one core, and the owner asks for core 1.
target_compile_options(${COMPONENT_LIB} PRIVATE -include fake_compat.h)

# Sanitizers, only when FCK_SANITIZE is set; see tests/sanitize.cmake.
include(${CMAKE_CURRENT_LIST_DIR}/../../sanitize.cmake)

# The C maths library: lround() and the rest live in libm, which GCC on Linux
# (the CI) does not link by itself. The Mac's compiler does, so a missing line
# here only shows in the CI.
target_link_libraries(${COMPONENT_LIB} PRIVATE m)
