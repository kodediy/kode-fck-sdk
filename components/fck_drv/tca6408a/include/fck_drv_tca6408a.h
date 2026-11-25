/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file fck_drv_tca6408a.h
 * @brief The module's I2C bus and its TCA6408A expander.
 *
 * One bus on the I2C pair (positions 13 and 14), one device on it at 0x21,
 * and the four registers of the chip. Every transfer is asked again on a
 * miss, because the bus can lose about one transaction in twenty.
 *
 * The bus must be created from a task on core 1. ESP-IDF allocates its
 * interrupt on the core that creates it, and with the bus created on core 0 -
 * where kode-sdk's Active Objects run - a write to an absent address right
 * after a NAK'd probe can come back as ESP_OK. The owner's task is pinned to
 * core 1 for this reason.
 *
 * The expander comes up with every pin an input and keeps no copy of what it
 * was told, and its registers can be lost under the caller: the GNSS
 * receiver's standard firmware takes the bus away, and a power glitch puts the
 * chip back at its power-on values. So what was last written to the output
 * and configuration registers is kept here, and a read that misses falls back
 * on it, never on the power-on value - that would turn every output the owner
 * drives, the antenna switch's among them, back into an input.
 *
 * One caller: the owner's task. Nothing here locks.
 */
#ifndef FCK_DRV_TCA6408A_H_
#define FCK_DRV_TCA6408A_H_

#include <stdbool.h>
#include <stdint.h>

#include "kode_defs.h"

#include "fck_drv_pins.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief The bus and the chip. Its fields are private. */
typedef struct {
    void          *bus;
    void          *dev;
    int            scl;
    int            sda;
    uint8_t        out;   /* last written to OUTPUT, from 0xFF */
    uint8_t        cfg;   /* last written to CONFIG, from 0xFF */
} fck_drv_tca6408a_t;

/**
 * @brief Open the bus on the module's pair and attach the expander.
 * @return KODE_OK; KODE_ERROR when ESP-IDF would not create the bus.
 *         Nothing is asked of the chip: fck_drv_tca6408a_probe() does that.
 */
kode_status_t fck_drv_tca6408a_open(fck_drv_tca6408a_t *t, const fck_drv_pins_t *hw);

/**
 * @brief Remove the device and delete the bus. The shadows stay.
 * @return true when both went. false when ESP-IDF refused: the handles are
 *         kept then, so that nothing opens a second bus, or drives the pins
 *         by hand, over a controller that still holds them.
 */
bool fck_drv_tca6408a_close(fck_drv_tca6408a_t *t);

/** @brief Whether the bus is open. */
bool fck_drv_tca6408a_is_open(const fck_drv_tca6408a_t *t);

/**
 * @brief Ask the bus whether anything answers at 0x21: an address and a
 *        stop, nothing written to any register.
 * @param tries  How many times, at most, until one answers.
 * @param nacked How many tries came back as a plain absence; may be NULL.
 * @param failed How many came back as anything else - a held line, lost
 *               arbitration, a timeout; may be NULL. Both stay 0 with the
 *               bus closed.
 * @return true when one try did answer.
 */
bool fck_drv_tca6408a_probe(fck_drv_tca6408a_t *t, uint32_t tries, uint32_t *nacked, uint32_t *failed);

/** @brief Forget what was written: the shadows back to the power-on 0xFF. */
void fck_drv_tca6408a_forget(fck_drv_tca6408a_t *t);

/** @brief One register read: the register's number is written first, as a
 *         byte. A chip with no registers (a PCF8574) takes that byte for its
 *         port, so this is a write to one.
 *  @return KODE_OK; KODE_DENIED with the bus closed; KODE_IO_ERROR after
 *          every retry missed. */
kode_status_t fck_drv_tca6408a_read(fck_drv_tca6408a_t *t, uint8_t reg, uint8_t *val);

/** @brief One register written. A write of OUTPUT or CONFIG that the chip
 *         took becomes its shadow, so a job's own writes are not undone by
 *         the next fck_drv_tca6408a_blind_gnss_reset().
 *  @return As fck_drv_tca6408a_read(). */
kode_status_t fck_drv_tca6408a_write(fck_drv_tca6408a_t *t, uint8_t reg, uint8_t val);

/**
 * @brief Drive several pins to one level, or let them go, in one write of
 *        each register. Driving, the level goes first and the direction
 *        after, so a pin becomes an output already at its level; letting go,
 *        the direction goes first, so a pin on its way to an input is never
 *        driven to the level written for it.
 * @param mask  The pins, bit n for Pn.
 * @param drive true to drive them; false to make them inputs.
 * @param level What they are driven to.
 * @return As fck_drv_tca6408a_read(); KODE_DENIED for an empty mask.
 */
kode_status_t fck_drv_tca6408a_mask(fck_drv_tca6408a_t *t, uint8_t mask, bool drive, bool level);

/**
 * @brief fck_drv_tca6408a_mask(), asked again - reopening the bus between tries -
 *        for up to @p ms.
 *
 * After the GNSS receiver is put in reset the bus takes up to about 500 ms
 * to answer again; without these retries SW0 would be left unset. 600 ms
 * covers it.
 */
kode_status_t fck_drv_tca6408a_mask_patient(fck_drv_tca6408a_t *t, const fck_drv_pins_t *hw, uint8_t mask,
                                   bool drive, bool level, uint32_t ms);

/**
 * @brief Put the GNSS receiver in reset when the bus cannot be used to do it.
 *
 * With its standard firmware the receiver drives SDA, the expander drops off
 * the bus, and the expander holds the receiver's reset: the ordinary write
 * cannot get through. This one is written by hand on the two GPIOs at their
 * strongest drive, without looking at the acknowledgements: OUTPUT and
 * CONFIG with P2 low and an output, and every other pin as the shadows say.
 * The bus is closed for it and opened again after. The chip may
 * need up to 500 ms to answer again: follow with fck_drv_tca6408a_mask_patient().
 * Nothing is driven when the bus could not be closed (fck_drv_tca6408a_close()).
 *
 * Only for a module already identified as an F*CK (fck_aos.h, "Finding a
 * module"): on anything else it is two writes to whatever sits at 0x21, and a
 * strong drive of whatever else holds the pair - a Mesh module's receiver
 * holds it the same way. A bus that is merely held proves nothing.
 */
void fck_drv_tca6408a_blind_gnss_reset(fck_drv_tca6408a_t *t, const fck_drv_pins_t *hw);

#ifdef __cplusplus
}
#endif

#endif /* FCK_DRV_TCA6408A_H_ */
