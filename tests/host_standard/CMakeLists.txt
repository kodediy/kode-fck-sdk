# The standard layer's suite: fck_standard.c and fck_events.c, what a task calls (fck.h), over a core
# that only writes down what it is asked and an owner that keeps a per-task error and two sinks. A
# project of its own because the real core defines the fck_core_*() calls these fake (tests/host_owner
# runs the whole stack over a model of the module, tests/host_facade the core over a fake owner).
#
# `set(COMPONENTS main)` must come BEFORE project(): see tests/host.
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

set(COMPONENTS main)

project(fck_standard_tests)
