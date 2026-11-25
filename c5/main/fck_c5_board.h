/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file fck_c5_board.h
 * @brief The C5's own pins on the F*CK (KDFCK001 rev 1.0).
 *
 * The C5's GPIO numbers, as the official firmware uses them
 * (firmware/fck-c5). Which J3 position each one reaches on the Kode is the
 * SDK's business, in fck_bsp.h.
 */
#ifndef FCK_C5_BOARD_H_
#define FCK_C5_BOARD_H_

/* The link to the Kode: UART0, which is also what the ROM loader listens on,
 * so the Kode can always write this chip whatever runs on it. */
#define FCK_C5_HOST_UART    0        /**< UART_NUM_0.                        */
#define FCK_C5_HOST_TX      11       /**< U0TXD: the Kode listens here (J3 5). */
#define FCK_C5_HOST_RX      12       /**< U0RXD: the Kode talks here (J3 3).  */
#define FCK_C5_HOST_BAUD    115200

/* The GNSS receiver (Minew ME31GR02) talks to the C5 only. Its reset is not
 * the C5's: the Kode holds it on the module's expander and lets it go when an
 * app asks for a fix. */
#define FCK_C5_GNSS_UART    1        /**< UART_NUM_1.                        */
#define FCK_C5_GNSS_RX      4        /**< From the receiver's TXD.           */
#define FCK_C5_GNSS_TX      5        /**< To the receiver's RXD.             */
#define FCK_C5_GNSS_BAUD    115200
#define FCK_C5_GNSS_PPS     6        /**< Its 1PPS: about 100 ms high, once a second. */

/* The wake lines to the module's TCA6408A. */
#define FCK_C5_WAKE_IN      0        /**< From the expander's P1 (HOST_WP_C5): an input. */
#define FCK_C5_WAKE_OUT     23       /**< To its P0 (C5_WP_HOST): every change
                                      *   raises the expander's INT to the Kode. */

#endif /* FCK_C5_BOARD_H_ */
