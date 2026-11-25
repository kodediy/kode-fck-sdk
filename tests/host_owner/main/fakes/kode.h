/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kode.h
 * @brief kode-sdk's standard-layer header, as far as the facade uses it: the clock.
 *        The model in fake_cc1101.c answers it.
 */
#ifndef KODE_H_
#define KODE_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "kode_defs.h"

/** Milliseconds since boot (kode.h's), wrapping after 49 days. */
uint32_t kode_uptime_ms(void);

/** kode.h's kode_init(): the suite has nothing to start. */
void kode_init(void);

#endif /* KODE_H_ */
