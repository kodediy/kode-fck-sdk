# The runner, the cases, the fakes they drive, and kodeOS's console registry
# with its rules. examples/console_actions/main/console_actions_ao.c is the
# source under test, #included by its case file so a case reaches its state
# handler and its rows: it is not listed here. examples/subghz's
# subghz_arm.c, plain C, is #included by test_arm.c in the same way. fck_sys.h is the real one;
# what answers it is fakes/fake_fck.c. The same shape as tests/host/main: see
# there for why the cases are globbed, sorted and linked whole.
set(SDK_DIR "${CMAKE_CURRENT_LIST_DIR}/../../..")

file(GLOB CASE_SOURCES "${CMAKE_CURRENT_LIST_DIR}/../cases/test_*.c")
list(SORT CASE_SOURCES)

# kode_status_t: kdefs' one header, from the kode-sdk clone of ../../kode_sdk.cmake
# (from v0.1.0 kdefs declares esp32p4 only, and the component manager refuses it
# for `linux`).
include(${CMAKE_CURRENT_LIST_DIR}/../../kode_sdk.cmake)

idf_component_register(
    SRCS "runner.c"
         ${CASE_SOURCES}
         "${CMAKE_CURRENT_LIST_DIR}/fakes/fake_console.c"
         "${CMAKE_CURRENT_LIST_DIR}/fakes/fake_kode.c"
         "${CMAKE_CURRENT_LIST_DIR}/fakes/fake_fck.c"
    INCLUDE_DIRS "${KODE_SDK_DIR}/components/kdefs/include"
                 "."
                 "${CMAKE_CURRENT_LIST_DIR}/fakes"
                 "${SDK_DIR}/components/fck/include"
                 "${SDK_DIR}/components/fck_sys/include"
                 "${SDK_DIR}/components/fck_bsp/board/include"
                 "${SDK_DIR}/examples/console_actions/main"
                 "${SDK_DIR}/examples/subghz/main"
    REQUIRES unity
    WHOLE_ARCHIVE
)

# Sanitizers, only when FCK_SANITIZE is set; see tests/sanitize.cmake.
include(${CMAKE_CURRENT_LIST_DIR}/../../sanitize.cmake)
