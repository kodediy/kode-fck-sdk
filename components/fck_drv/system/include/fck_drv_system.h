/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file fck_drv_system.h
 * @brief What the owner needs from the system that is not a chip: being told
 *        the P4 is about to restart.
 *
 * An app goes home by restarting into kodeOS - the bottom button held, a
 * kode_exit_to_os(), a kode_home_now(): every one of them is an esp_restart()
 * from kode-sdk's AO_Boot - and kodeOS knows nothing about the module on
 * J3. Whatever the module was doing when the app
 * left, it may keep doing under kodeOS: +3.3V_EXT may stay on.
 * And through a restart the expander keeps its outputs and the CC1101 its
 * state.
 *
 * Only esp_restart() runs these hooks, in the task that asked for the
 * restart, the last registered first, before the scheduler stops - kode-sdk
 * puts the panel in reset the same way (kdrv_display.c). A panic, a
 * watchdog, a brownout, the host opening the USB serial port or the bottom
 * and left buttons together run none, so a module may still come to kodeOS
 * awake.
 */
#ifndef FCK_DRV_SYSTEM_H_
#define FCK_DRV_SYSTEM_H_

#include "kode_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*fck_drv_system_fn)(void);

/**
 * @brief Have @p fn run when the P4 restarts on purpose.
 * @return KODE_OK; KODE_NO_RESOURCES when ESP-IDF's five places are taken.
 *         When this was counted, in an app on kode-sdk, esp_timer and the
 *         display took two and Wi-Fi (esp_wifi_remote) a third: with this
 *         one, two were left, one with Wi-Fi.
 */
kode_status_t fck_drv_system_on_restart(fck_drv_system_fn fn);

#ifdef __cplusplus
}
#endif

#endif /* FCK_DRV_SYSTEM_H_ */
