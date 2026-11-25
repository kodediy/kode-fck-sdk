# The suite is one component: the runner, the cases, and the source under
# test - fck_ao.c, #included by its case file, so a case reaches its statics.
# What is faked is below it: klog's kode_log() (fakes/). The worker is the
# owner's own FreeRTOS task, and runs as it ships, on the host port.
set(SDK_DIR "${CMAKE_CURRENT_LIST_DIR}/../../..")
set(KC "${KODE_SDK_DIR}/components")   # ../CMakeLists.txt fetches it

# kode_sys.h is the public header of the Active Object layer (kqp.h and the signals
# among its includes). None of them includes an ESP-IDF one, so they all compile for `linux`.
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

# WHOLE_ARCHIVE: a case file that nothing references would otherwise be
# dropped from the link, and Unity would report OK over what survived.
idf_component_register(
    SRCS "runner.c"
         ${CASE_SOURCES}
         "${CMAKE_CURRENT_LIST_DIR}/fakes/fake_kode.c"
         "${KC}/kqp/src/kqp_self.c"
    INCLUDE_DIRS "."
                 "${CMAKE_CURRENT_LIST_DIR}/fakes"
                 "${SDK_DIR}/components/fck_aos/fck/src"
                 "${SDK_DIR}/components/fck_aos/core/include"
                 ${KODE_INCLUDES}
    REQUIRES unity qpc freertos
    WHOLE_ARCHIVE
)

# The rest and wake signals exist only with CONFIG_KODE_LOWPOWER; the owner subscribes to them
# under it (fck_ao.c), so the suite builds with it on.
target_compile_definitions(${COMPONENT_LIB} PRIVATE CONFIG_KODE_LOWPOWER=1)

# Sanitizers, only when FCK_SANITIZE is set; see tests/sanitize.cmake.
include(${CMAKE_CURRENT_LIST_DIR}/../../sanitize.cmake)
