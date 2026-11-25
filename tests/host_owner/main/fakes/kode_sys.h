/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kode_sys.h
 * @brief kode-sdk's extended-layer header, as far as the owner uses it: the loan of J3's positions
 *        and controllers, its supply, the clock and the task priorities. The model in
 *        fake_world.c and fake_cc1101.c answers it, and fake_fck_ao.c delivers EXP_POWER_SIG.
 */
#ifndef KODE_SYS_H_
#define KODE_SYS_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "kode_defs.h"

/** @brief As kode_sys.h's. */
typedef enum {
    KODE_SYS_EXP_I2C = 0,
    KODE_SYS_EXP_SPI,
    KODE_SYS_EXP_UART,
} kode_sys_exp_bus_t;

kode_status_t kode_sys_exp_lend(const uint8_t *pos, size_t n, const char *tag, uint8_t *gpio);
kode_status_t kode_sys_exp_return(const char *tag);
kode_status_t kode_sys_exp_ctrl(kode_sys_exp_bus_t bus, const char *tag, uint8_t *id);
/** A command: posts to the radio owner; the answer is EXP_POWER_SIG. */
kode_status_t kode_sys_exp_power(bool on);

/** Milliseconds since boot, wrapping after 49 days. */
uint32_t kode_sys_time_ms(void);

#define KODE_PRIO_USER_LOW 3

#endif /* KODE_SYS_H_ */
