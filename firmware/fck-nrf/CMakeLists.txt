cmake_minimum_required(VERSION 3.20.0)
# fck-nrf: the firmware of the nRF54L15 (U701, Minew ME54BS61) on the F*CK module.
# Its version is the VERSION file here: the application's, the one in #NRFINFO, and the one the
# MCUboot image is signed with.
# Zephyr shell + mcumgr SMP (shell transport) on uart00 -> ESP32-P4, raw 2.4 GHz RADIO commands.

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(fck-nrf)

# The watchdog is stopped before the firmware resets itself (main.c says why): every caller of
# sys_arch_reboot(), sys_reboot() among them, goes through __wrap_sys_arch_reboot() first.
zephyr_ld_options(-Wl,--wrap=sys_arch_reboot)

target_sources(app PRIVATE
	src/main.c
	src/rf.c
)
