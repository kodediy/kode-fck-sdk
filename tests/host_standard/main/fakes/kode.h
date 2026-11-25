/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kode.h
 * @brief kode-sdk's standard-layer header, as far as the standard layer uses it: kode_init().
 */
#ifndef KODE_H_
#define KODE_H_

#include "kode_defs.h"

/** kode.h's kode_init(): counted by the suite (g_kode_inits). */
void kode_init(void);

#endif /* KODE_H_ */
