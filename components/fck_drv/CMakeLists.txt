# fck_drv - the module's drivers. Each one knows a part or a protocol and
# nothing about who calls it or when: that is fck_aos's.
#
# fck_drv_link.c is plain C on purpose - no ESP-IDF, no FreeRTOS - because it is
# the one piece both firmwares' clients share and tests/host runs it as it is.
# fck_drv_nrf.c and fck_drv_c5.c, the two firmwares' commands and answers, fck_drv_nmea.c,
# the GNSS receiver's sentences, fck_drv_smp.c, mcumgr's SMP the nRF is flashed
# with, fck_drv_ook.c and fck_drv_subghz_scan.c, what the CC1101 hears read back, and
# fck_drv_subghz_file.c, that as a Kode .ksub file, and fck_drv_subghz_modem.c, the
# packet receiver's registers in numbers, and fck_drv_subghz_duty.c, the air time
# of the last hour, are plain C for the second reason alone. fck_drv_nrf_dfu.c is fck_drv_smp on the nRF's
# UART, with its waits. The rest is ESP-IDF's GPIO, I2C master, UART and SPI
# master, and esp-serial-flasher for the C5's ROM loader (fck_drv_c5_rom.c), which
# idf_component.yml brings in as a private requirement.
#
# Laid out as kdrv is: one folder per driver, its header in include/ and its
# source in src/. Every include/ is listed below, so an #include names the
# header alone and the folders are on disk only.
idf_component_register(
    SRCS "link/src/fck_drv_link.c"
         "nrf/src/fck_drv_nrf.c"
         "c5/src/fck_drv_c5.c"
         "nmea/src/fck_drv_nmea.c"
         "smp/src/fck_drv_smp.c"
         "nrf_dfu/src/fck_drv_nrf_dfu.c"
         "ook/src/fck_drv_ook.c"
         "subghz_scan/src/fck_drv_subghz_scan.c"
         "subghz_file/src/fck_drv_subghz_file.c"
         "subghz_modem/src/fck_drv_subghz_modem.c"
         "subghz_duty/src/fck_drv_subghz_duty.c"
         "pins/src/fck_drv_pins.c"
         "tca6408a/src/fck_drv_tca6408a.c"
         "uart/src/fck_drv_uart.c"
         "cc1101/src/fck_drv_cc1101.c"
         "system/src/fck_drv_system.c"
         "c5_rom/src/fck_drv_c5_rom.c"
    INCLUDE_DIRS "c5/include"
                 "c5_rom/include"
                 "cc1101/include"
                 "link/include"
                 "nmea/include"
                 "nrf/include"
                 "nrf_dfu/include"
                 "ook/include"
                 "pins/include"
                 "smp/include"
                 "subghz_duty/include"
                 "subghz_file/include"
                 "subghz_modem/include"
                 "subghz_scan/include"
                 "system/include"
                 "tca6408a/include"
                 "uart/include"
    REQUIRES kdefs fck_bsp
    PRIV_REQUIRES esp_driver_gpio esp_driver_i2c esp_driver_uart esp_driver_spi esp_driver_rmt
                  esp_timer esp_rom esp_system freertos
)
