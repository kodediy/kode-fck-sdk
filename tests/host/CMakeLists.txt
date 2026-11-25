# The host test suite: every test that needs no board, built for ESP-IDF's
# `linux` target.
#
# `set(COMPONENTS main)` must come BEFORE project(): without it the configure
# walks the whole component registry looking for host implementations that
# do not exist (kode-sdk's tests/host says the same, and why).
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

set(COMPONENTS main)

project(fck_host_tests)
