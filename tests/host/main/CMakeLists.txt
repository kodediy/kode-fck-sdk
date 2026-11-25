# The suite is one component: the runner, the cases, and the real SDK
# sources under test.
#
# The cases are globbed and the sources under test are listed, on purpose:
# adding a case is adding a file, but pulling a production source into the
# host build is a decision about what it drags in, and a decision should cost
# a line somebody reads.
set(SDK_DIR "${CMAKE_CURRENT_LIST_DIR}/../../..")

set(SOURCES_UNDER_TEST
    "${SDK_DIR}/components/fck_aos/fck/src/fck_plan.c"
    "${SDK_DIR}/components/fck_drv/link/src/fck_drv_link.c"
    "${SDK_DIR}/components/fck_drv/nrf/src/fck_drv_nrf.c"
    "${SDK_DIR}/components/fck_drv/c5/src/fck_drv_c5.c"
    "${SDK_DIR}/components/fck_drv/nmea/src/fck_drv_nmea.c"
    "${SDK_DIR}/components/fck_drv/smp/src/fck_drv_smp.c"
    "${SDK_DIR}/components/fck_drv/ook/src/fck_drv_ook.c"
    "${SDK_DIR}/components/fck_drv/subghz_scan/src/fck_drv_subghz_scan.c"
    "${SDK_DIR}/components/fck_drv/subghz_file/src/fck_drv_subghz_file.c"
    "${SDK_DIR}/components/fck_drv/subghz_modem/src/fck_drv_subghz_modem.c"
    "${SDK_DIR}/components/fck_drv/subghz_duty/src/fck_drv_subghz_duty.c"
    # An example's own rule, and plain C like the rest: the rows of a WiGLE file.
    "${SDK_DIR}/examples/wardriving/main/wardriving_wigle.c"
)

# Sorted, so a local run and CI run the cases in the same order: a case
# registers itself from a constructor, in the order the linker meets it.
file(GLOB CASE_SOURCES "${CMAKE_CURRENT_LIST_DIR}/../cases/test_*.c")
list(SORT CASE_SOURCES)

# WHOLE_ARCHIVE: a case file that nothing references would otherwise be
# dropped from the link, and Unity would report OK over what survived.
# kode_status_t: kdefs' one header, from the kode-sdk clone of ../../kode_sdk.cmake
# (from v0.1.0 kdefs declares esp32p4 only, and the component manager refuses it
# for `linux`).
include(${CMAKE_CURRENT_LIST_DIR}/../../kode_sdk.cmake)

idf_component_register(
    SRCS "runner.c"
         ${CASE_SOURCES}
         ${SOURCES_UNDER_TEST}
    INCLUDE_DIRS "${KODE_SDK_DIR}/components/kdefs/include"
                 "."
                 # fck_plan.h, the owner's rules in plain C.
                 "${SDK_DIR}/components/fck_aos/fck/src"
                 "${SDK_DIR}/components/fck_drv/c5/include"
                 "${SDK_DIR}/components/fck_drv/link/include"
                 "${SDK_DIR}/components/fck_drv/nmea/include"
                 "${SDK_DIR}/components/fck_drv/nrf/include"
                 "${SDK_DIR}/components/fck_drv/ook/include"
                 "${SDK_DIR}/components/fck_drv/smp/include"
                 "${SDK_DIR}/components/fck_drv/subghz_duty/include"
                 "${SDK_DIR}/components/fck_drv/subghz_file/include"
                 "${SDK_DIR}/components/fck_drv/subghz_modem/include"
                 "${SDK_DIR}/components/fck_drv/subghz_scan/include"
                 "${SDK_DIR}/components/fck_bsp/board/include"
                 # The C5 firmware's greeting format, read back by test_c5.c.
                 "${SDK_DIR}/c5/main"
                 # fck.h's types, which examples/wardriving's rows are made from.
                 "${SDK_DIR}/components/fck/include"
                 "${SDK_DIR}/examples/wardriving/main"
    REQUIRES unity
    WHOLE_ARCHIVE
)

# The C maths library: lround() and the rest live in libm, which GCC on Linux
# (the CI) does not link by itself. The Mac's compiler does, so a missing line
# here only shows in the CI.
target_link_libraries(${COMPONENT_LIB} PRIVATE m)

# Sanitizers, only when FCK_SANITIZE is set; see tests/sanitize.cmake.
include(${CMAKE_CURRENT_LIST_DIR}/../../sanitize.cmake)
