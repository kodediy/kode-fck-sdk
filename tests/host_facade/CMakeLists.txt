# The facade's suite: fck.c, fck_wifi.c, fck_radio.c and fck_gnss.c, what an
# app calls, over an owner that only writes down what it is asked. A project
# of its own because a suite that linked the real owner would define fck_aos_*
# twice, and C allows one definition per binary (tests/host_owner runs the
# facade over the real owner, against a model of the module).
#
# `set(COMPONENTS main)` must come BEFORE project(): see tests/host.
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

set(COMPONENTS main)

project(fck_facade_tests)
