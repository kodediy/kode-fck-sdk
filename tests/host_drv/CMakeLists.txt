# The drivers' suite: the drivers that talk to ESP-IDF (pins, the expander, the
# UARTs, the restart hook and the CC1101), run as they are, against a fake
# ESP-IDF with a bus and a chip behind it. tests/host runs the plain-C
# ones.
#
# `set(COMPONENTS main)` must come BEFORE project(): see tests/host.
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

set(COMPONENTS main)

project(fck_drv_tests)
