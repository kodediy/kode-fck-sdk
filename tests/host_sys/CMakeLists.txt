# The extended layer's suite: fck_sys.c (components/fck_sys/src), what an Active Object calls
# (fck_sys.h), on kode-sdk's own QP/C and its own event pool, over a core that does what a case
# scripts (fakes/fake_core.c) and an owner that keeps a per-task error, the sinks and the cancel marks
# (fakes/fake_aos.c). The Active Objects are stand-ins: a queue where the port reads it and a priority,
# drained by hand, as tests/host_ao does the owner's.
cmake_minimum_required(VERSION 3.16)

# kode-sdk at the release the apps pin, for its headers and its QP/C: the clone tests/host_ao keeps
# (its CMakeLists.txt fetches it if it is not there; .gitignore leaves it out).
set(KODE_SDK_VERSION "v0.1.5")
set(KODE_SDK_DIR "${CMAKE_CURRENT_LIST_DIR}/../host_ao/kode-sdk-${KODE_SDK_VERSION}")
if(NOT EXISTS "${KODE_SDK_DIR}/components/kode_sys/include/kode_sys.h")
    file(REMOVE_RECURSE "${KODE_SDK_DIR}")
    execute_process(
        COMMAND git clone --quiet --depth 1 --branch "${KODE_SDK_VERSION}"
                "https://github.com/kodediy/kode-sdk.git" "${KODE_SDK_DIR}"
        RESULT_VARIABLE fetched)
    if(NOT fetched EQUAL 0)
        message(FATAL_ERROR "kode-sdk ${KODE_SDK_VERSION} could not be fetched")
    endif()
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# QP/C as kode-sdk's own tests/host takes it: its component, as it ships.
set(EXTRA_COMPONENT_DIRS "${KODE_SDK_DIR}/components/qpc")

# Before project(), as in tests/host: only main and what it requires.
set(COMPONENTS main)

project(fck_sys_tests)
