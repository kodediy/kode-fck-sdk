/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file fck_drv_uart.h
 * @brief A UART to one of the module's chips: open, read what came, write.
 *
 * Two of them at once, one per chip, on the ports kode_sys_exp_ctrl() gives the
 * owner: kode-sdk keeps the books of which controllers an app may take for
 * J3, and answers the lowest free one - UART0 too, while the console is the
 * USB's.
 *
 * The driver's interrupt is not in IRAM unless the app sets
 * CONFIG_UART_ISR_IN_IRAM, and kode-sdk's board settings do not. While the
 * flash is written - kode_fs, NVS, an app being installed - the interrupt
 * waits, the 128-byte hardware FIFO fills in about 11 ms at 115200 baud, and
 * what arrives after that is lost: a line fck_drv_link then drops, or reads
 * joined to the next. An app that writes flash while the module talks sets
 * CONFIG_UART_ISR_IN_IRAM=y; the driver's buffers then go to internal RAM
 * (about 8 KB for the two ports) instead of PSRAM.
 *
 * Reads never block unless asked to (fck_drv_uart_read_wait()): the owner
 * reads both ports at every tick, and the driver's receive buffer holds what arrives in
 * between.
 */
#ifndef FCK_DRV_UART_H_
#define FCK_DRV_UART_H_

#include <stddef.h>
#include <stdint.h>

#include "kode_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Install the driver on @p port, on two GPIOs, at @p baud.
 * @param tx The P4's output: the chip's RX.
 * @param rx The P4's input: the chip's TX.
 * @return KODE_OK; KODE_EXISTS when the port is already installed by
 *         something else; KODE_ERROR when ESP-IDF refused.
 */
kode_status_t fck_drv_uart_open(int port, int tx, int rx, uint32_t baud);

/** @brief Remove the driver; the two GPIOs go back to inputs. A port that
 *         is not open, -1 included, is left alone. */
void fck_drv_uart_close(int port);

/** @brief Change the speed of an open port (the C5's ROM loader moves). */
kode_status_t fck_drv_uart_baud(int port, uint32_t baud);

/** @brief What has arrived, up to @p cap bytes, without waiting. */
size_t fck_drv_uart_read(int port, uint8_t *buf, size_t cap);

/** @brief Up to @p cap bytes, waiting up to @p ms for the first of them
 *         and taking what else has come with it; 0 when nothing came. For
 *         a flasher that waits for an answer. */
size_t fck_drv_uart_read_wait(int port, uint8_t *buf, size_t cap, uint32_t ms);

/** @brief Write @p n bytes; waits until they are in the driver's buffer. */
kode_status_t fck_drv_uart_write(int port, const void *data, size_t n);

/** @brief Drop whatever has arrived and was not read. */
void fck_drv_uart_flush(int port);

#ifdef __cplusplus
}
#endif

#endif /* FCK_DRV_UART_H_ */
