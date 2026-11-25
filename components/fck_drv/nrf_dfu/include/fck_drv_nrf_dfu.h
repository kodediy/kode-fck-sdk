/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file fck_drv_nrf_dfu.h
 * @brief A new image into the nRF54L15, over its UART and SMP (fck_drv_smp.h).
 *
 * One way in, MCUboot's serial recovery, so no image is ever written where
 * MCUboot would not check it before it runs: for the 3 s after a reset
 * (FCK_BSP_NRF_RECOVERY_MS), and for good while the slot holds nothing it
 * will boot, MCUboot takes an upload itself, straight into the slot. For any
 * image, and for an nRF whose image does not answer; it needs the nRF's
 * reset (the expander's P6), so not while the GNSS holds the bus. The module
 * has one slot: nothing is swapped back, an upload cut short leaves the nRF
 * in recovery, and it is written over the same way. An image that starts and
 * misbehaves stays, and is written over the same way too.
 *
 * WHICH MCUBOOT ANSWERS IS ASKED FIRST, before a byte is uploaded. The
 * module's (fck_bsp.h: one slot, no key, an image taken by its SHA-256)
 * answers SMP's echo; the image must carry a SHA-256 and no signature.
 * A loader that answers no echo takes only an image signed with Nordic's
 * public development key (ED25519 and SHA-512). An image for one would upload into the other and
 * never boot, leaving the nRF in recovery, so it is refused instead
 * (fck_drv_smp_image_suits()) and MCUboot is let go to boot what it had.
 *
 * WHERE THE IMAGE WAS LINKED. MCUboot checks a hash, not an address: an
 * image built for another map is taken, started, and hangs without a word.
 * fck-nrf carries where it was linked in its FCKMAP tag
 * (fck_drv_smp_image_map()); one whose tag names another address than
 * FCK_BSP_NRF_IMAGE_START is refused before the nRF is touched. An image
 * with no tag - anyone's own firmware - is not refused for it: it is
 * written, with a warning when it does not hold that address
 * (fck_drv_smp_image_holds()).
 *
 * THE NEW IMAGE MUST SAY SOMETHING. After the upload MCUboot is reset and
 * nothing is sent while the image starts: a line of text, or Zephyr's boot
 * banner, says it runs. One that prints nothing in FCK_DRV_NRF_DFU_LISTEN_MS
 * fails the write, though it was written: MCUboot found it wanting and stays
 * in recovery, or it hangs, or it is a firmware that never prints - which
 * this file cannot tell from one that hangs.
 *
 * These run inside a job (fck_aos_run_job()): they use the nRF's UART and its
 * reset as they like, and block for the length of it: tens of seconds for
 * an image of some tens of KB.
 */
#ifndef FCK_DRV_NRF_DFU_H_
#define FCK_DRV_NRF_DFU_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "fck_drv_smp.h"
#include "fck_drv_tca6408a.h"
#include "kode_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief What a job hands over: the nRF's port, and the expander for its
 *         reset. */
/** The most of the image's first line a report or a probe keeps: all the text reader puts
 *  together (fck_drv_smp.h), which is a `#NRFINFO` up to its `devid=`, `reset=` and `uptime_ms=`. */
#define FCK_DRV_NRF_DFU_LINE_MAX  FCK_DRV_SMP_TEXT_MAX

typedef struct {
    int        uart;   /**< fck_aos_job_context_t.uart[FCK_AOS_NRF].             */
    fck_drv_tca6408a_t *tca;    /**< fck_aos_job_context_t.tca; NULL while the GNSS holds
                        *   the bus: no recovery then.                       */
} fck_drv_nrf_dfu_io_t;

/** How long the new image is given to print its first line once MCUboot
 *  lets it go: MCUboot's 3 s, and the firmware's start. */
#define FCK_DRV_NRF_DFU_LISTEN_MS  9000U

/** @brief Where a write stopped. */
typedef enum {
    FCK_DRV_NRF_DFU_STEP_NONE = 0,   /**< It did not.                            */
    FCK_DRV_NRF_DFU_STEP_IMAGE,      /**< The image: not MCUboot's, no hash, one
                                  *   that cannot start in the slot, one
                                  *   linked for another map, or one the
                                  *   MCUboot that answered would not boot
                                  *   (the report's image says which).
                                  *   Nothing was written.                   */
    FCK_DRV_NRF_DFU_STEP_RESET,      /**< The nRF's reset: no expander, or it
                                  *   did not take it.                       */
    FCK_DRV_NRF_DFU_STEP_SERVER,     /**< MCUboot's recovery did not answer in
                                  *   its window, or did not say which it
                                  *   is.                                    */
    FCK_DRV_NRF_DFU_STEP_UPLOAD,     /**< The upload stopped, or the slot holds another
                                  *   image than the one sent.               */
    FCK_DRV_NRF_DFU_STEP_BOOT,       /**< The image was written and said nothing
                                  *   once MCUboot let it go.                */
} fck_drv_nrf_dfu_step_t;

/** @brief What a write did. Written whatever it returns. */
typedef struct {
    bool               reset;       /**< The nRF was reset: it runs again,
                                     *   MCUboot's, the new image's or the
                                     *   old one's.                         */
    bool               same;        /**< The slot held that image already:
                                     *   nothing was written.               */
    bool               answered;    /**< The image that runs at the end said
                                     *   something: a line, or its boot
                                     *   banner.                            */
    fck_drv_nrf_dfu_step_t failed;      /**< Where it stopped.                  */
    int32_t            rc;          /**< The server's rc there; -1 for none. */
    uint32_t           written;     /**< Image bytes the server took.       */
    uint32_t           ms;          /**< How long it all took.              */
    bool               loader_known; /**< MCUboot answered, and said which it
                                     *   is; loader is it.                  */
    fck_drv_smp_loader_t loader;    /**< The module's one-slot MCUboot, or
                                     *   a signing one.                     */
    char               before[24];  /**< The slot's version before; "" when
                                     *   not known.                          */
    char               line[FCK_DRV_NRF_DFU_LINE_MAX + 1U]; /**< The first line the image printed
                                     *   once MCUboot let it go, MCUboot's
                                     *   and Zephyr's banners left out; ""
                                     *   when none came.                    */
    fck_drv_smp_image_check_t image; /**< Why the image was refused at
                                     *   FCK_DRV_NRF_DFU_STEP_IMAGE;
                                     *   FCK_DRV_SMP_IMAGE_FITS otherwise.   */
    bool               has_map;     /**< The image carries an FCKMAP tag...  */
    uint32_t           map;         /**< ...which names this address.        */
    bool               holds_start; /**< The image holds the address where it
                                     *   starts in the slot
                                     *   (fck_drv_smp_image_holds()). false
                                     *   warns that it may be built for
                                     *   another map, and refuses nothing.   */
} fck_drv_nrf_dfu_report_t;

/** @brief What fck_drv_nrf_dfu_probe() saw. Written whatever it returns. */
typedef struct {
    bool     reset;         /**< The nRF was reset: it runs again, whatever it
                             *   runs.                                       */
    bool     mcuboot;       /**< MCUboot's recovery answered, inside its window
                             *   after the reset.                            */
    bool     echo;          /**< It answered SMP's echo too: the module's
                             *   one-slot MCUboot, which takes an image by
                             *   its SHA-256. false for one that wants it
                             *   signed.                                     */
    uint32_t mcuboot_ms;    /**< How long after the reset it answered.       */
    bool     late;          /**< SMP's echo answered only past MCUboot's
                             *   window: an image that speaks it, the window
                             *   missed.                                     */
    bool     image;         /**< The list names an image in slot 0. From
                             *   MCUboot: one it checked and took.          */
    char     version[24];   /**< That image's version; "" for none.          */
    uint8_t  hash[8];       /**< The first bytes of its hash.                */
    char     line[FCK_DRV_NRF_DFU_LINE_MAX + 1U]; /**< The first line the nRF printed once MCUboot
                             *   let it go, MCUboot's and Zephyr's banners
                             *   left out; "" when none came.                */
    bool     banner;        /**< A Zephyr image's boot banner came: an image
                             *   started.                                    */
    bool     answered;      /**< With no line, SMP answered a last knock...  */
    bool     running;       /**< ...from an image that runs (slot 0 active),
                             *   not from MCUboot staying in recovery.       */
    uint32_t ms;            /**< How long it all took.                       */
} fck_drv_nrf_dfu_probe_t;

/**
 * @brief Write @p img through MCUboot's serial recovery, straight into the
 *        slot, and let the nRF start it.
 *
 * The image is looked at first, and nothing is touched for one that cannot
 * start in the slot or whose FCKMAP tag names another address. Then the nRF
 * is reset and knocked on with SMP's echo every 150 ms until something
 * answers, within MCUboot's 3 s; it is taken for MCUboot only if it then gives
 * its image list (2 s: MCUboot validates the image on each list, ~0.5 ms a
 * KB). MCUboot is asked which it is (the echo's answer): an image it
 * would not boot is refused there, nothing written, and MCUboot let go. An
 * upload cut short leaves the slot with nothing MCUboot boots, so it stays
 * in recovery and takes this again. After the upload MCUboot is asked what
 * the slot holds: another image than the one sent fails the write there,
 * with MCUboot left in recovery (no answer within 2 s is not a failure, a
 * large image takes long to validate). Then MCUboot is reset, and nothing
 * is sent for FCK_DRV_NRF_DFU_LISTEN_MS while the image starts.
 *
 * @return KODE_OK when the image was written (or was there already) and
 *         said something once started; KODE_DENIED for an image refused
 *         (rep->image), or no expander for the reset; KODE_TIMEOUT when
 *         MCUboot never answered, or did not say which it is; KODE_ERROR
 *         when the answer came too late to be MCUboot's, a step was
 *         refused, or the image was written and said nothing (rep->failed
 *         is FCK_DRV_NRF_DFU_STEP_BOOT: MCUboot found it wanting and stays
 *         in recovery, or it hangs, or it never prints); KODE_IO_ERROR when
 *         the UART or the expander refused.
 */
kode_status_t fck_drv_nrf_dfu_recover(const fck_drv_nrf_dfu_io_t *io, const uint8_t *img, size_t len,
                                  fck_drv_nrf_dfu_report_t *rep);

/**
 * @brief What the nRF carries, with nothing written: MCUboot, the image it
 *        holds, and what that image says once it runs.
 *
 * The nRF is reset and knocked on with SMP's echo every 150 ms, as
 * fck_drv_nrf_dfu_recover() does. When MCUboot answers in its window, the
 * image in the slot is read from its list, MCUboot is asked which it is (an
 * echo) and then to reset, so that it boots that image; then nothing is sent while the nRF prints its
 * first line, for up to 9 s. With no line, one last knock: an image that
 * runs answers it (or prints something back, which counts as its line), and
 * so does an MCUboot that stays in recovery, told apart by its image list
 * (MCUboot gives one; fck-nrf carries no image group), and then reset to go
 * on as it was. An image that restarts over and over can look like that MCUboot,
 * if the knock falls in its window.
 *
 * @return KODE_OK once it looked, whatever it found (rep says what);
 *         KODE_DENIED for no expander for the reset; KODE_IO_ERROR when the
 *         UART or the expander refused.
 */
kode_status_t fck_drv_nrf_dfu_probe(const fck_drv_nrf_dfu_io_t *io, fck_drv_nrf_dfu_probe_t *rep);

#ifdef __cplusplus
}
#endif

#endif /* FCK_DRV_NRF_DFU_H_ */
