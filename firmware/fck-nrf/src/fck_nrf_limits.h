/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * fck-nrf - the radio limits of the firmware, all of them, in this one table.
 *
 * These are the limits of the F*CK module (KDFCK001) for the EU: where it may transmit, how
 * hard, and that it never leaves an unmodulated carrier on the air. The firmware
 * has no command, no build option and no second table that lifts them: rf.c reads every one of
 * these numbers from here and holds none of its own.
 *
 * A fork that needs other limits changes them here, and only here.
 */
#ifndef FCK_NRF_LIMITS_H
#define FCK_NRF_LIMITS_H

#include <stdint.h>

struct fck_nrf_limits {
	/* Transmitting (rf ping, rf echo): the centre frequencies it may use, in MHz. */
	uint16_t tx_mhz_min;
	uint16_t tx_mhz_max;
	/* The highest TX power it sets, in dBm. A request above it is brought down to it. */
	int8_t tx_dbm_max;
	/* No unmodulated carrier: a transmitter that is still on this long after its packet
	 * should have left the air is switched off, in microseconds. (After a packet the RADIO
	 * would otherwise idle on its carrier.) */
	uint16_t tx_overrun_us;
	/* Receiving (rf spec, rf sniff): the centre frequencies it may listen on, in MHz. */
	uint16_t rx_mhz_min;
	uint16_t rx_mhz_max;
};

static const struct fck_nrf_limits fck_nrf_limits = {
	.tx_mhz_min = 2402,
	.tx_mhz_max = 2480,
	.tx_dbm_max = 6,
	.tx_overrun_us = 300,
	.rx_mhz_min = 2400,
	.rx_mhz_max = 2500,
};

#endif /* FCK_NRF_LIMITS_H */
