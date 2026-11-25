/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file radio_ao.h
 * @brief The example's own Active Object: the nRF listening: the 2.4 GHz band's levels, and the frames where it is busiest.
 */
#ifndef RADIO_AO_H_
#define RADIO_AO_H_

#include "kode_sys.h"

/** @brief The singleton, as an opaque QActive - the pattern every kodeOS AO
 *         follows, so nothing outside radio_ao.c can reach its members.
 *         AO_Radio is the Dot's own radio, in kode-sdk: hence the prefix. */
extern QActive * const AO_FckRadio;

/**
 * @brief Construct it.
 *
 * Call after kode_init() and before FckRadio_start(). It only initialises
 * memory: nothing is posted and the module is not looked for until the
 * initial transition runs.
 */
void FckRadio_ctor(void);

/**
 * @brief Give it a thread, a queue and a stack, and let it run.
 *
 * The initial transition runs here, in the caller's thread - the one that
 * called kode_init(): it subscribes to the module's news and calls fck_sys_begin().
 */
void FckRadio_start(void);

#endif /* RADIO_AO_H_ */
