# fck-c5: the firmware of the F*CK module's ESP32-C5 (U501): line protocol on UART0 towards the Kode
# Dot's P4 (the SDK's fck_drv_c5), Wi-Fi scan on both bands, Wi-Fi channel activity survey
# (promiscuous), passive BLE scan (NimBLE), GNSS UART relay and 1PPS counter.
# An app on the Kode embeds this project's images and writes them with fck_flash().
cmake_minimum_required(VERSION 3.16)

# The version, here and nowhere else: ESP-IDF puts it in the app descriptor of the image, and the firmware
# reads it from there for the ver= of its #C5INFO (as major.minor).
set(PROJECT_VER "0.2.0")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(fck-c5)
