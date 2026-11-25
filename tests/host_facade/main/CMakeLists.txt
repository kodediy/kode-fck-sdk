# The runner, the cases, the facade under test, and the owner they are given.
# The same shape as tests/host/main: see there for why the cases are globbed,
# sorted and linked whole. The facade and the plain-C drivers it parses with
# are the only production sources here; the owner is the fake, so that the one
# command each call makes can be read back.
set(SDK_DIR "${CMAKE_CURRENT_LIST_DIR}/../../..")

set(SOURCES_UNDER_TEST
    "${SDK_DIR}/components/fck/src/fck.c"
    "${SDK_DIR}/components/fck/src/fck_wifi.c"
    "${SDK_DIR}/components/fck/src/fck_radio.c"
    "${SDK_DIR}/components/fck/src/fck_gnss.c"
    "${SDK_DIR}/components/fck_drv/link/src/fck_drv_link.c"
    "${SDK_DIR}/components/fck_drv/c5/src/fck_drv_c5.c"
    "${SDK_DIR}/components/fck_drv/nrf/src/fck_drv_nrf.c"
    "${SDK_DIR}/components/fck_drv/nmea/src/fck_drv_nmea.c"
)

file(GLOB CASE_SOURCES "${CMAKE_CURRENT_LIST_DIR}/../cases/test_*.c")
list(SORT CASE_SOURCES)

# kode_status_t: kdefs' one header, from the kode-sdk clone of ../../kode_sdk.cmake
# (from v0.1.0 kdefs declares esp32p4 only, and the component manager refuses it
# for `linux`).
include(${CMAKE_CURRENT_LIST_DIR}/../../kode_sdk.cmake)

idf_component_register(
    SRCS "runner.c"
         ${CASE_SOURCES}
         ${SOURCES_UNDER_TEST}
         "${CMAKE_CURRENT_LIST_DIR}/fakes/fake_fck_aos.c"
    INCLUDE_DIRS "${KODE_SDK_DIR}/components/kdefs/include"
                 "."
                 "${CMAKE_CURRENT_LIST_DIR}/fakes"
                 "${SDK_DIR}/components/fck/include"
                 "${SDK_DIR}/components/fck/src"
                 "${SDK_DIR}/components/fck/core/include"
                 "${SDK_DIR}/components/fck_aos/core/include"
                 "${SDK_DIR}/components/fck_drv/c5/include"
                 "${SDK_DIR}/components/fck_drv/cc1101/include"
                 "${SDK_DIR}/components/fck_drv/ook/include"
                 "${SDK_DIR}/components/fck_drv/subghz_duty/include"
                 "${SDK_DIR}/components/fck_drv/subghz_modem/include"
                 "${SDK_DIR}/components/fck_drv/subghz_scan/include"
                 "${SDK_DIR}/components/fck_drv/link/include"
                 "${SDK_DIR}/components/fck_drv/nmea/include"
                 "${SDK_DIR}/components/fck_drv/nrf/include"
                 "${SDK_DIR}/components/fck_drv/pins/include"
                 "${SDK_DIR}/components/fck_drv/tca6408a/include"
                 "${SDK_DIR}/components/fck_bsp/board/include"
    REQUIRES unity
    WHOLE_ARCHIVE
)

# fck_core.h is the SDK's own: the suite is one of the two layers' components.
target_compile_definitions(${COMPONENT_LIB} PRIVATE FCK_CORE_ALLOWED)

# Sanitizers, only when FCK_SANITIZE is set; see tests/sanitize.cmake.
include(${CMAKE_CURRENT_LIST_DIR}/../../sanitize.cmake)
