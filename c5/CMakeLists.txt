# A firmware of your own for the F*CK's ESP32-C5 (U501), to start from: where
# the C5's pins go, the factory's flash map, the greeting the Kode's SDK reads,
# and a radio kept inside the module's limits.
# See README.md.
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(c5_firmware)
