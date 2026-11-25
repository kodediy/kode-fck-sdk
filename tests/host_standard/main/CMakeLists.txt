# The standard layer's suite: fck_standard.c and fck_events.c, what a task calls, over a core that
# only writes down what it is asked (fakes/fake_core.c, generated from fck_core.h) and an owner that
# keeps a per-task error and two sinks. A project of its own because the real core defines the
# fck_core_*() calls these fake: tests/host_owner runs the whole stack over a model of the module,
# tests/host_facade the core over a fake owner.
set(SDK_DIR "${CMAKE_CURRENT_LIST_DIR}/../../..")

set(SOURCES_UNDER_TEST
    "${SDK_DIR}/components/fck/src/fck_standard.c"
    "${SDK_DIR}/components/fck/src/fck_events.c"
)

file(GLOB CASE_SOURCES "${CMAKE_CURRENT_LIST_DIR}/../cases/test_*.c")
list(SORT CASE_SOURCES)

# kode_status_t: kdefs' one header, from the kode-sdk clone of ../../kode_sdk.cmake
# (from v0.1.0 kdefs declares esp32p4 only, and the component manager refuses it
# for `linux`).
include(${CMAKE_CURRENT_LIST_DIR}/../../kode_sdk.cmake)

idf_component_register(
    SRCS "runner.c"
         "fakes/fake_core.c" "fakes/fake_aos.c"
         ${CASE_SOURCES}
         ${SOURCES_UNDER_TEST}
    INCLUDE_DIRS "${KODE_SDK_DIR}/components/kdefs/include"
                 "." "fakes"
                 "${SDK_DIR}/components/fck/include"
                 "${SDK_DIR}/components/fck/core/include"
                 "${SDK_DIR}/components/fck/src"
                 "${SDK_DIR}/components/fck_aos/core/include"
                 "${SDK_DIR}/components/fck_drv/c5/include"
                 "${SDK_DIR}/components/fck_drv/cc1101/include"
                 "${SDK_DIR}/components/fck_drv/link/include"
                 "${SDK_DIR}/components/fck_drv/nmea/include"
                 "${SDK_DIR}/components/fck_drv/ook/include"
                 "${SDK_DIR}/components/fck_drv/pins/include"
                 "${SDK_DIR}/components/fck_drv/subghz_duty/include"
                 "${SDK_DIR}/components/fck_drv/subghz_modem/include"
                 "${SDK_DIR}/components/fck_drv/subghz_scan/include"
                 "${SDK_DIR}/components/fck_drv/tca6408a/include"
                 "${SDK_DIR}/components/fck_bsp/board/include"
    REQUIRES unity freertos
    WHOLE_ARCHIVE
)

# fck_core.h is the SDK's own: the suite is one of the two layers' components.
target_compile_definitions(${COMPONENT_LIB} PRIVATE FCK_CORE_ALLOWED)

# Sanitizers, only when FCK_SANITIZE is set; see tests/sanitize.cmake.
include(${CMAKE_CURRENT_LIST_DIR}/../../sanitize.cmake)
