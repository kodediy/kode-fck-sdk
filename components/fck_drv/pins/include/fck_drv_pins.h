/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file fck_drv_pins.h
 * @brief The module's fourteen lines as plain GPIOs: at rest, looked at, and
 *        the two the C5 is reset and strapped through.
 *
 * The GPIO behind each position comes from kode_sys_exp_lend(), and the
 * controllers from kode_sys_exp_ctrl(), which the owner calls once; every driver
 * here is handed the table they filled. Nothing here
 * keeps state, and nothing here drives a line high that the module holds
 * with a resistor: EN and BOOT of the C5 are pulled down or let go, never
 * pushed to 1 (fck_bsp.h).
 */
#ifndef FCK_DRV_PINS_H_
#define FCK_DRV_PINS_H_

#include <stdbool.h>
#include <stdint.h>

#include "fck_bsp.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief What kode-sdk lent the module: the P4 GPIO behind each position
 *         (kode_sys_exp_lend()), and the I2C and SPI controllers its drivers run
 *         on (kode_sys_exp_ctrl()). */
typedef struct {
    uint8_t gpio[FCK_BSP_POSITIONS]; /**< Index 0 for position 1.            */
    int     i2c_port;                /**< An i2c_port_num_t: the expander's. */
    int     spi_host;                /**< A spi_host_device_t: the CC1101's.  */
} fck_drv_pins_t;

/** @brief The GPIO behind position @p pos (1 to 14). */
static inline int fck_drv_pins_gpio(const fck_drv_pins_t *hw, uint8_t pos)
{
    return (int)hw->gpio[pos - 1U];
}

/**
 * @brief Every line an input with no pull, but the CC1101's chip select,
 *        which gets a pull-up so the chip never takes noise for a command.
 *
 * The C5's EN is left to R502, so a C5 that is there runs. This is what the
 * header looks like with nothing plugged in and with a module nobody uses.
 */
void fck_drv_pins_rest(const fck_drv_pins_t *hw);

/**
 * @brief Does something hold the I2C pair high: is a module there?
 *
 * The module's 2.2 k pull-ups beat the GPIOs' own pull-downs (about 45 k),
 * so both lines read 1 with a module and 0 without. The only thing touched
 * before a module is known to be an F*CK: two inputs with pull-downs, for
 * 300 us, left as inputs with no pull afterwards.
 */
bool fck_drv_pins_pair_held(const fck_drv_pins_t *hw);

/**
 * @brief The same question asked of SCL alone, for while the GNSS
 *        receiver's standard firmware holds SDA (fck_bsp.h).
 */
bool fck_drv_pins_scl_held(const fck_drv_pins_t *hw);

/**
 * @brief Pull the C5's EN low, and let it go.
 *
 * @param hw        The lines.
 * @param to_loader BOOT is pulled low through the reset, so the C5 comes up
 *                  in its ROM loader; let go otherwise, so it boots its flash.
 *                  BOOT is let go by fck_drv_pins_c5_release_boot() once the ROM
 *                  has sampled it.
 * @param hold      true leaves EN low: the C5 stays in reset, which is what
 *                  it costs least in (R502 draws 0.33 mA through it) until
 *                  fck_drv_pins_c5_run().
 */
void fck_drv_pins_c5_reset(const fck_drv_pins_t *hw, bool to_loader, bool hold);

/** @brief Let EN go: R502 and the GPIO's pull-up bring the C5 out of reset. */
void fck_drv_pins_c5_run(const fck_drv_pins_t *hw);

/** @brief Let BOOT go, after a reset into the loader. */
void fck_drv_pins_c5_release_boot(const fck_drv_pins_t *hw);

/**
 * @brief Hold the C5 in reset with BOOT let go, for good: for the way home.
 *
 * From here on fck_drv_pins_c5_reset(), fck_drv_pins_c5_run() and
 * fck_drv_pins_c5_release_boot() leave both lines as they are, so that whatever
 * still runs on the module - a job writing the C5 through its ROM - cannot
 * put the C5 back in its loader before the P4 restarts. kodeOS's kode_init()
 * floats EN, and the C5 boots its flash. Safe from any task.
 */
void fck_drv_pins_c5_park(const fck_drv_pins_t *hw);

#ifdef __cplusplus
}
#endif

#endif /* FCK_DRV_PINS_H_ */
