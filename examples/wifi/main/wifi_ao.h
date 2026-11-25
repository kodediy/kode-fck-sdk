/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file wifi_ao.h
 * @brief The example's own Active Object: the C5 listening: what each Wi-Fi channel carries, and the BLE devices around.
 */
#ifndef WIFI_AO_H_
#define WIFI_AO_H_

#include "kode_sys.h"

/** @brief The singleton, as an opaque QActive - the pattern every kodeOS AO
 *         follows, so nothing outside wifi_ao.c can reach its members. */
extern QActive * const AO_Wifi;

/**
 * @brief Construct it.
 *
 * Call after kode_init() and before Wifi_start(). It only initialises
 * memory: nothing is posted and the module is not looked for until the
 * initial transition runs.
 */
void Wifi_ctor(void);

/**
 * @brief Give it a thread, a queue and a stack, and let it run.
 *
 * The initial transition runs here, in the caller's thread - the one that
 * called kode_init(): it subscribes to the module's news and calls fck_sys_begin().
 */
void Wifi_start(void);

#endif /* WIFI_AO_H_ */
