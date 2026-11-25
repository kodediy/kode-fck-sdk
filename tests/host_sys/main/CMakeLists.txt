# The suite is one component: the runner, the cases, and the source under test - fck_sys.c, #included
# by its case file, so a case reaches its statics (the doors, the subscribers). What is faked is
# below it: the core (fck_core.h), the owner (fck_aos.h) and klog's kode_log().
set(SDK_DIR "${CMAKE_CURRENT_LIST_DIR}/../../..")
set(KC "${KODE_SDK_DIR}/components")   # ../CMakeLists.txt fetches it

# kode_sys.h is the public header of the Active Object layer (kqp.h and the signals among its
# includes). None of them includes an ESP-IDF one, so they all compile for `linux`.
file(GLOB KODE_INCLUDES LIST_DIRECTORIES true
     "${KC}/kdefs/include"
     "${KC}/kode/include"
     "${KC}/kode_sys/include"
     "${KC}/kqp/include"
     "${KC}/klog/include"
     "${KC}/kaos/*/include"
     "${KC}/kdrv/*/include"
     "${KC}/khal/*/include"
     "${KC}/kbsp/*/include")

file(GLOB CASE_SOURCES "${CMAKE_CURRENT_LIST_DIR}/../cases/test_*.c")
list(SORT CASE_SOURCES)

# WHOLE_ARCHIVE: a case file that nothing references would otherwise be dropped from the link, and
# Unity would report OK over what survived.
idf_component_register(
    SRCS "runner.c"
         ${CASE_SOURCES}
         "fakes/fake_core.c" "fakes/fake_aos.c"
         "${CMAKE_CURRENT_LIST_DIR}/../../host_ao/main/fakes/fake_kode.c"
    INCLUDE_DIRS "."
                 "fakes"
                 "${CMAKE_CURRENT_LIST_DIR}/../../host_ao/main/fakes"
                 "${SDK_DIR}/components/fck/include"
                 "${SDK_DIR}/components/fck/core/include"
                 "${SDK_DIR}/components/fck_sys/include"
                 "${SDK_DIR}/components/fck_sys/src"
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
                 ${KODE_INCLUDES}
    REQUIRES unity qpc freertos
    WHOLE_ARCHIVE
)

# fck_core.h and fck_aos.h are the SDK's own: the suite is one of the two layers' components.
target_compile_definitions(${COMPONENT_LIB} PRIVATE FCK_CORE_ALLOWED FCK_AOS_ALLOWED)

# Sanitizers, only when FCK_SANITIZE is set; see tests/sanitize.cmake.
include(${CMAKE_CURRENT_LIST_DIR}/../../sanitize.cmake)
