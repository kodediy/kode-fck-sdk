/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file fake_mcuboot.h
 * @brief The nRF54L15 behind the module's UART, as MCUboot's serial recovery
 *        and the image it boots show themselves to fck_drv_nrf_dfu.c.
 *
 * It listens to what the driver writes to the UART and to the expander's P6
 * (the nRF's reset), on the fake ESP-IDF's virtual clock, and answers the way
 * MCUboot's boot_serial does, in the same SMP framing the driver speaks
 * (fck_drv_smp.c's own, both ways):
 *
 *   - after a reset (the pin's pulse, or SMP's own reset) it is silent for
 *     ready_ms, then answers; it waits window_ms for a first frame and goes
 *     to boot the slot when none came. A frame inside the window keeps it in
 *     recovery until the next reset, and so does a slot with nothing it
 *     boots;
 *   - echo is answered (the module's one-slot loader), refused with rc 8 (a
 *     first module's loader) or not answered at all;
 *   - the image list names slot 0 with the hash its image carries, as
 *     MCUboot reads it from the TLVs, after list_ms (MCUboot validates the
 *     whole image on each list: about 0.5 ms a KB), or answers list_rc (MCUboot built for fewer sectors than its
 *     slot has); requests are answered one at a time, so whatever comes
 *     while a list is validated waits behind it; an upload's first piece erases the slot
 *     (the old image is gone from then on), each piece answers the offset the
 *     slot now holds, and the last one makes the image whole;
 *   - what boots prints its first line once boot_to_line_ms have passed since
 *     the reset, and MCUboot prints its banner when it starts.
 *
 * What it does not model: the checksum MCUboot computes over the image
 * (corrupt_store says directly that what was stored is not what was sent),
 * signatures, the time an upload really takes beyond the knobs below, a
 * running image's mcumgr beyond the OS group (image_smp_os), and lines lost
 * on the wire.
 */
#ifndef FAKE_MCUBOOT_H_
#define FAKE_MCUBOOT_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** @brief How the loader answers SMP's echo. */
typedef enum {
    FAKE_MCUBOOT_ECHO_ANSWERS = 0,   /**< The module's one-slot MCUboot.            */
    FAKE_MCUBOOT_ECHO_REFUSED,       /**< A first module's: rc 8, not supported.    */
    FAKE_MCUBOOT_ECHO_SILENT,        /**< Nothing comes back.                       */
} fake_mcuboot_echo_t;

/** @brief What a case may change after fake_mcuboot_start(), and what the
 *         driver did to it. */
typedef struct {
    /* How it behaves. */
    fake_mcuboot_echo_t echo;
    bool     silent;                  /**< Answers no frame at all.                  */
    uint32_t ready_ms;                /**< Silent for this long after a reset: 180.  */
    uint32_t window_ms;               /**< Waits this long for a frame: 3000.        */
    uint32_t answer_ms;               /**< Each answer takes this long: 5.           */
    uint32_t list_ms;                 /**< The image list takes this long: 20.       */
    int32_t  list_rc;                 /**< Not 0: the list is answered with this rc and
                                       *   no images, as by a one-slot MCUboot built
                                       *   with fewer BOOT_MAX_IMG_SECTORS than its
                                       *   slot has.                                 */
    uint32_t first_piece_ms;          /**< An upload's first piece, which erases: 800. */
    uint32_t boot_to_line_ms;         /**< From a reset to the image's line: 6300.   */
    uint32_t slot_max;                /**< The longest image it takes.               */
    unsigned drop_echoes;             /**< This many echoes get no answer...         */
    unsigned drop_echoes_after;       /**< ...after the first this many: 0.          */
    int32_t  refuse_rc;               /**< Not 0: every piece is answered with it, and
                                       *   the slot is left as it was.               */
    uint32_t stall_at;                /**< Not 0: once the slot holds this many
                                       *   bytes it answers its offset and takes no
                                       *   more.                                     */
    uint32_t mute_at;                 /**< Not 0: once the slot holds this many
                                       *   bytes it answers no piece.                */
    bool     corrupt_store;           /**< What it stores is not what was sent: its
                                       *   list names another hash and it will not
                                       *   boot it.                                  */
    bool     list_mute_when_complete; /**< A whole new image is being validated: the
                                       *   list takes longer than anyone waits.      */
    bool     image_prints;            /**< What boots prints a line: true.           */
    bool     image_smp_os;            /**< What runs answers SMP's echo and reset and
                                       *   refuses the rest (rc 8), as fck-nrf, which
                                       *   carries only the OS group.                */
    bool     pin_dead;                /**< P6 does not reset the nRF.                */
    const char *image_line;           /**< ...this one.                              */

    /* What it saw. */
    unsigned frames;                  /**< Frames it read while listening.           */
    unsigned echoes;
    unsigned lists;
    unsigned pieces;                  /**< Upload pieces it read.                    */
    unsigned smp_resets;              /**< Reset requests it took.                   */
    unsigned pin_resets;              /**< Times P6 was let go after a pulse.        */
    unsigned image_frames;            /**< Frames the running image read.            */
} fake_mcuboot_t;

extern fake_mcuboot_t g_mcuboot;

/** @brief The longest image the fake's slot holds. */
#define FAKE_MCUBOOT_SLOT_BYTES 4096U

/**
 * @brief The nRF running @p image (NULL for an empty slot) on UART @p port,
 *        its reset on P6 of the expander; the hooks of g_idf set, with every
 *        knob at its default. Call after idf_fake_reset() and opening the
 *        UART and the expander.
 */
void fake_mcuboot_start(int port, const uint8_t *image, size_t len);

/** @brief The bytes in the slot, as many as were written. */
const uint8_t *fake_mcuboot_slot(size_t *written);

/** @brief Whether the slot holds a whole image it would boot. */
bool fake_mcuboot_slot_boots(void);

/** @brief Whether the nRF is in MCUboot's recovery right now. */
bool fake_mcuboot_in_recovery(void);

#endif /* FAKE_MCUBOOT_H_ */
