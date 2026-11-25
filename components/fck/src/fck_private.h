/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file fck_private.h
 * @brief What the facade's files share. Not an app's header, and not
 *        installed: it lives beside them.
 */
#ifndef FCK_PRIVATE_H_
#define FCK_PRIVATE_H_

#include "kode_defs.h"

#include "fck.h"
#include "fck_core.h"
#include "fck_drv_c5.h"
#include "fck_aos.h"

/* How long the facade waits for a chip to say what it runs, when it has not
 * since it started: the nRF's 6.3 s from reset to its firmware
 * (FCK_BSP_NRF_BOOT_TO_FIRMWARE_MS) and the C5's 5 s
 * (FCK_DRV_C5_BOOT_MS), with room. */
#define FCK_FIRMWARE_WAIT_MS 7500U

/** @brief What fck_error() says of a call fck_cancel() ended. */
#define FCK_WHY_CANCELLED "cancelled by fck_cancel()"

/** @brief What fck_error() says of a send the way home, a rest or fck_end() cut. */
#define FCK_WHY_CUT "cancelled: the send was cut by the way home, a rest or fck_end()"

/**
 * @brief Record @p why (a literal) as the reason of the calling task's failure, and
 *        return @p st: `return fck_fail(KODE_DENIED, "...")`.
 */
static inline kode_status_t fck_fail(kode_status_t st, const char *why)
{
    fck_aos_error_set_for(st, why);
    return st;
}

/**
 * @brief What @p chip runs, waited for as the facade waits for it.
 * @return KODE_OK; KODE_TIMEOUT when it did not say in FCK_FIRMWARE_WAIT_MS;
 *         KODE_DENIED for no module or the chip held in reset; KODE_IO_ERROR
 *         when the module left.
 */
kode_status_t fck_firmware_wait(fck_aos_chip_t chip, fck_aos_firmware_t *out);

/**
 * @brief A chip's banner in an app's terms. @p out is cleared first; known
 *        stays false for a chip that had not said, and fw empty for one whose
 *        banner could not be read.
 */
void fck_firmware_fill(fck_aos_chip_t chip, const fck_aos_firmware_t *in, fck_firmware_t *out);

/**
 * @brief The C5, and the firmware on it, as the typed calls need it: waited for
 *        as fck_firmware_wait() does, and read.
 * @param info Out.
 * @param seq  Out: what fck_aos_command_if_same_firmware() checks the line against.
 * @return KODE_OK; KODE_TIMEOUT when it did not say; KODE_DENIED when it is
 *         not compatible (those calls go only to a firmware that speaks their words) or there
 *         is no module; KODE_IO_ERROR when the module left.
 */
kode_status_t fck_firmware_c5(fck_drv_c5_info_t *info, uint32_t *seq);

#endif /* FCK_PRIVATE_H_ */
