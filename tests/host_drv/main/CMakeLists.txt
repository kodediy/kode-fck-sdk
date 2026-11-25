# The runner, the cases, the drivers under test and the ESP-IDF they are
# given: the module's pins (fck_drv_pins.c), its expander on the I2C pair
# (fck_drv_tca6408a.c), the chips' UARTs (fck_drv_uart.c), the restart hook
# (fck_drv_system.c), the CC1101 over SPI (fck_drv_cc1101.c) and the write of
# an image into the nRF over SMP (fck_drv_nrf_dfu.c, with fck_drv_smp.c). The same
# shape as tests/host/main: see there for why the cases are globbed, sorted
# and linked whole. ESP-IDF's driver/gpio.h, driver/i2c_master.h,
# driver/uart.h, driver/spi_master.h, driver/rmt_*.h, esp_attr.h,
# esp_heap_caps.h, esp_rom_sys.h, esp_system.h and esp_timer.h are the headers
# in fakes, which come first on the include path; their functions are in
# fakes/fake_idf.c; the chip on the SPI is fakes/fake_cc1101.c, and the nRF
# behind its UART, MCUboot's recovery and what it boots, fakes/fake_mcuboot.c.
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
         "${SDK_DIR}/components/fck_drv/pins/src/fck_drv_pins.c"
         "${SDK_DIR}/components/fck_drv/tca6408a/src/fck_drv_tca6408a.c"
         "${SDK_DIR}/components/fck_drv/uart/src/fck_drv_uart.c"
         "${SDK_DIR}/components/fck_drv/system/src/fck_drv_system.c"
         "${SDK_DIR}/components/fck_drv/cc1101/src/fck_drv_cc1101.c"
         "${SDK_DIR}/components/fck_drv/ook/src/fck_drv_ook.c"
         "${SDK_DIR}/components/fck_drv/subghz_scan/src/fck_drv_subghz_scan.c"
         "${SDK_DIR}/components/fck_drv/subghz_modem/src/fck_drv_subghz_modem.c"
         "${SDK_DIR}/components/fck_drv/smp/src/fck_drv_smp.c"
         "${SDK_DIR}/components/fck_drv/nrf_dfu/src/fck_drv_nrf_dfu.c"
         "${CMAKE_CURRENT_LIST_DIR}/fakes/fake_idf.c"
         "${CMAKE_CURRENT_LIST_DIR}/fakes/fake_cc1101.c"
         "${CMAKE_CURRENT_LIST_DIR}/fakes/fake_mcuboot.c"
    INCLUDE_DIRS "${KODE_SDK_DIR}/components/kdefs/include"
                 "${CMAKE_CURRENT_LIST_DIR}/fakes"
                 "."
                 "${SDK_DIR}/components/fck_drv/cc1101/include"
                 "${SDK_DIR}/components/fck_drv/nrf_dfu/include"
                 "${SDK_DIR}/components/fck_drv/ook/include"
                 "${SDK_DIR}/components/fck_drv/pins/include"
                 "${SDK_DIR}/components/fck_drv/smp/include"
                 "${SDK_DIR}/components/fck_drv/subghz_modem/include"
                 "${SDK_DIR}/components/fck_drv/subghz_scan/include"
                 "${SDK_DIR}/components/fck_drv/system/include"
                 "${SDK_DIR}/components/fck_drv/tca6408a/include"
                 "${SDK_DIR}/components/fck_drv/uart/include"
                 "${SDK_DIR}/components/fck_bsp/board/include"
    REQUIRES unity freertos esp_common
    WHOLE_ARCHIVE
)

# The host port ticks in milliseconds, as the Dot does (tests/host_owner).
# Sanitizers, only when FCK_SANITIZE is set; see tests/sanitize.cmake.
include(${CMAKE_CURRENT_LIST_DIR}/../../sanitize.cmake)
