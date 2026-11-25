/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file fake_cc1101.h
 * @brief A CC1101 on the other end of the SPI: its registers, its state, the
 *        strobes that move it and the status registers it answers, enough for
 *        fck_drv_cc1101.c to identify it, tune it and read its level.
 *
 * It keeps a log of every strobe and every register write, in order, so that
 * a case can say what the driver did to the chip. Its data output (MISO) is
 * low while the chip select is low, as long as a chip is there: the crystal
 * runs. Nothing is ever transmitted: the driver's RMT calls are fake_idf.c's,
 * which clock nothing out.
 */
#ifndef FAKE_CC1101_H_
#define FAKE_CC1101_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define CC_LOG_MAX 512U

typedef enum {
    CC_EV_STROBE = 1,    /**< val: the strobe's address (0x30..0x3D)               */
    CC_EV_WRITE,         /**< reg and val                                         */
} fake_cc_kind_t;

typedef struct {
    fake_cc_kind_t kind;
    uint8_t        reg;
    uint8_t        val;
} fake_cc_event_t;

typedef struct {
    bool      present;        /**< A chip answers.                                   */
    uint8_t   partnum;        /**< PARTNUM, 0x00 on a CC1101.                         */
    uint8_t   version;        /**< VERSION, 0x14 on a CC1101.                         */
    bool      never_rx;       /**< SRX does not take it to RX.                        */
    uint8_t   rssi_floor;     /**< The raw RSSI of a quiet channel.                   */
    uint32_t  peak_hz;        /**< A carrier at this frequency ...                    */
    uint8_t   peak_raw;       /**< ... reads this raw.                                */
    uint32_t  peak_width_hz;  /**< Within this much of it.                            */

    esp_err_t bus_rc;         /**< spi_bus_initialize()'s answer.                     */
    esp_err_t add_rc;         /**< spi_bus_add_device()'s answer.                     */
    esp_err_t transmit_rc;    /**< spi_device_polling_transmit()'s answer.            */
    bool      write_fails;    /**< Writing the register below fails, and leaves it.   */
    uint8_t   write_fails_reg;

    int       cs_gpio;        /**< The pins the driver was given (set by the case).   */
    int       miso_gpio;

    /* What the chip is. */
    uint8_t   regs[0x40];
    uint8_t   state;          /**< MARCSTATE: 0x01 IDLE, 0x0D RX, 0x13 TX.            */
    bool      powered_down;
    uint8_t   txbytes;        /**< The TX FIFO's fill: a burst write to it sets it, SFTX clears it. */

    /* A hook a case sets, called as MARCSTATE is read for the nth time since
     * the reset: a stand-in for something happening while the driver waits. */
    unsigned  marc_reads;
    void    (*on_marc)(unsigned n);

    /* The same as RSSI is read for the nth time, before the reading is made:
     * a stand-in for a signal arriving while the driver listens. */
    unsigned  rssi_reads;
    void    (*on_rssi)(unsigned n);

    /* What was done to it. */
    int       buses;          /**< spi_bus_initialize()d and not yet freed.           */
    int       devices;
    unsigned  transfers;
    fake_cc_event_t log[CC_LOG_MAX];
    unsigned  log_n;
} fake_cc1101_t;

extern fake_cc1101_t g_cc;

/** @brief A CC1101 (PARTNUM 0x00, VERSION 0x14) on the header, its pins named by @p cs_gpio and @p miso_gpio. */
void fake_cc1101_reset(int cs_gpio, int miso_gpio);

/** @brief The MISO line's level while a chip is there: low with CS low. Returns whether @p gpio is MISO. */
bool fake_cc1101_miso(int gpio, int *level);

/** @brief How many times the strobe @p s was sent. */
unsigned fake_cc1101_strobes(uint8_t s);

/** @brief How many writes of @p reg were made, and the last value (0 when none). */
unsigned fake_cc1101_writes(uint8_t reg, uint8_t *last);

/** @brief The frequency FREQ2/1/0 hold, in hertz (26 MHz crystal). */
uint32_t fake_cc1101_hz(void);

#endif /* FAKE_CC1101_H_ */
