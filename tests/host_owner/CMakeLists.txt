# The owner's suite: the owner and the facade, the real sources, running on
# ESP-IDF's `linux` target against a model of the module. No module and no
# Kode needed.
#
# What is faked is what touches hardware - the lines, the I2C expander, the
# UARTs, the CC1101 (fake_cc1101.c: its sends are recorded), the C5's ROM loader and the nRF's DFU - kode-sdk's
# header and log, and the Active Object, which main/fakes/fake_fck_ao.c plays on two
# FreeRTOS tasks behind fck_ao.h. What is not: fck_aos.c, fck.c, fck_flash.c, fck_wifi.c,
# fck_gnss.c, fck_subghz.c and the plain-C drivers (link, c5, nrf, nmea, ook,
# subghz_*), which are the files an app links.
#
# `set(COMPONENTS main)` must come BEFORE project(): see tests/host.
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

set(COMPONENTS main)

project(fck_owner_tests)
