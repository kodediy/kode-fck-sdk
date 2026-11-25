/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file fck_flash.c
 * @brief Firmware into a chip for an app: fck_core_flash() in fck.h.
 *
 * The write is a job of the owner's (fck_aos_run_job()), which gives it the module
 * to itself: the C5 through its ROM loader (fck_drv_c5_rom.h), the nRF through
 * MCUboot (fck_drv_nrf_dfu.h). What the chip said it ran is read before the job,
 * from what the owner kept, and after it, when the write worked, from the
 * banner the chip prints when it starts.
 */
#include "fck.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "fck_drv_c5.h"
#include "fck_drv_c5_rom.h"
#include "fck_drv_nrf.h"
#include "fck_drv_nrf_dfu.h"
#include "fck_aos.h"
#include "fck_private.h"

/* How long the caller waits for both chips to be free. */
#define LOCK_MS         10000U
/* What a chip is given on top of its own start to say what it runs. */
#define GREET_MARGIN_MS 2000U

static fck_aos_chip_t own_of(fck_chip_t chip)
{
    return (chip == FCK_NRF) ? FCK_AOS_NRF : FCK_AOS_C5;
}

static void note_set(fck_flash_report_t *rep, const char *text)
{
    (void)snprintf(rep->note, sizeof(rep->note), "%s", text);
}

/* The report as a call that stopped at its own door leaves it, with what the
 * chip said it ran so far. */
static void report_start(fck_flash_report_t *rep, fck_chip_t chip)
{
    memset(rep, 0, sizeof(*rep));
    rep->step   = FCK_FLASH_CHECK;
    rep->failed = -1;
    rep->rc     = -1;
    fck_aos_firmware_t a = { 0 };
    (void)fck_aos_firmware(own_of(chip), 0U, &a);
    fck_firmware_fill(own_of(chip), &a, &rep->before);
}

/* What the chip says once it runs the new image, when it says it by itself. */
static void greeting(fck_chip_t chip, uint32_t start_ms, fck_flash_report_t *rep)
{
    fck_aos_firmware_t a = { 0 };
    (void)fck_aos_firmware(own_of(chip), start_ms + GREET_MARGIN_MS, &a);
    fck_firmware_fill(own_of(chip), &a, &rep->after);
}

/* ---- The C5 ------------------------------------------------------------------------- */

typedef struct {
    const fck_drv_c5_rom_part_t *parts;
    size_t                   n;
    fck_drv_c5_rom_report_t      rom;
    bool                     ran;     /* the job started: the chips were free */
} c5_job_t;

static kode_status_t c5_job(void *arg, const fck_aos_job_context_t *job)
{
    c5_job_t *const j = arg;
    j->ran            = true;
    kode_status_t const st = fck_drv_c5_rom_write(job->hw, job->uart[FCK_AOS_C5], j->parts, j->n,
                                              FCK_DRV_C5_ROM_BAUD, NULL, NULL, &j->rom);
    if (j->rom.reset) {
        /* The C5 went through its loader and was reset into its flash: it
         * runs, whatever it was before and however the write ended. */
        job->chip_on[FCK_AOS_C5] = true;
    }
    return st;
}

/* Where a write the ROM loader refused or lost stopped. */
static fck_flash_step_t c5_step(kode_status_t st, const fck_drv_c5_rom_report_t *r)
{
    switch (st) {
    case KODE_OK:
        return FCK_FLASH_DONE;
    case KODE_TIMEOUT:
        return FCK_FLASH_CONNECT;
    case KODE_IO_ERROR:
        /* It answered; before the first image, only the loader's own
         * questions and its speed change had been asked. */
        return (r->failed >= 0) ? FCK_FLASH_WRITE : FCK_FLASH_CONNECT;
    case KODE_DENIED:
    case KODE_NO_RESOURCES:
        return FCK_FLASH_CHECK;
    default:
        return FCK_FLASH_WRITE;
    }
}

static kode_status_t flash_c5(const fck_image_t *img, size_t n, fck_flash_report_t *rep)
{
    if (n == 0U || n > FCK_DRV_C5_ROM_PARTS_MAX) {
        note_set(rep, "the C5 takes one to eight images");
        return fck_fail(KODE_DENIED, "the C5 takes one to eight images");
    }
    fck_drv_c5_rom_part_t parts[FCK_DRV_C5_ROM_PARTS_MAX];
    for (size_t i = 0U; i < n; i++) {
        bool const deflated = (img[i].inflated_len != 0U);
        parts[i] = (fck_drv_c5_rom_part_t){
            .name  = NULL,
            .addr  = img[i].addr,
            .data  = img[i].data,
            .size  = deflated ? img[i].inflated_len : img[i].len,
            .zsize = deflated ? img[i].len : 0U,
            .md5   = img[i].md5,
        };
    }
    if (!fck_drv_c5_rom_parts_ok(parts, n)) {
        note_set(rep, "the images are not ones the ROM loader takes");
        return fck_fail(KODE_DENIED, "the images are not ones the ROM loader takes");
    }

    c5_job_t j = { .parts = parts, .n = n };
    kode_status_t st = fck_aos_run_job(c5_job, &j, LOCK_MS);
    if (!j.ran) {
        /* The job never started: no module, or the chips stayed busy, which
         * is a TIMEOUT of the owner's and not the loader's. */
        if (st == KODE_TIMEOUT) {
            note_set(rep, "the chips stayed busy");
            fck_aos_error_set("the chips stayed busy: another task holds the module");
        }
        return st;
    }

    rep->step = c5_step(st, &j.rom);
    rep->ms   = j.rom.ms;
    rep->rc   = (j.rom.loader_error != 0) ? (int32_t)j.rom.loader_error : -1;
    if (st == KODE_OK) {
        for (size_t i = 0U; i < n; i++) {
            rep->written += parts[i].size;
        }
    } else {
        rep->failed = j.rom.failed;
        for (int i = 0; i < j.rom.failed && (size_t)i < n; i++) {
            rep->written += parts[i].size;
        }
    }
    note_set(rep, j.rom.note);
    if (st == KODE_OK) {
        greeting(FCK_C5, FCK_DRV_C5_BOOT_MS, rep);
    }
    return st;
}

/* ---- The nRF ------------------------------------------------------------------------ */

typedef struct {
    const fck_image_t   *img;
    fck_drv_nrf_dfu_report_t dfu;
    bool                 ran;
} nrf_job_t;

static kode_status_t nrf_job(void *arg, const fck_aos_job_context_t *job)
{
    nrf_job_t *const         j  = arg;
    const fck_drv_nrf_dfu_io_t   io = { .uart = job->uart[FCK_AOS_NRF], .tca = job->tca };
    j->ran = true;
    kode_status_t const st = fck_drv_nrf_dfu_recover(&io, j->img->data, j->img->len, &j->dfu);
    if (j->dfu.reset) {
        /* Past its reset, the nRF runs: MCUboot's, the new image's or the
         * old one's. */
        job->chip_on[FCK_AOS_NRF] = true;
    }
    return st;
}

/* Why the nRF's driver refused an image, before anything was sent. */
static const char *image_note(fck_drv_smp_image_check_t why)
{
    switch (why) {
    case FCK_DRV_SMP_IMAGE_NOT_MCUBOOT:
        return "not an MCUboot image";
    case FCK_DRV_SMP_IMAGE_SIZES:
        return "the image's header and its length do not agree";
    case FCK_DRV_SMP_IMAGE_LARGER:
        return "larger than slot 0";
    case FCK_DRV_SMP_IMAGE_STACK:
        return "its first stack pointer is not in the nRF's RAM";
    case FCK_DRV_SMP_IMAGE_ADDRESS:
        return "its vectors point outside it in the slot: built for another map";
    case FCK_DRV_SMP_IMAGE_MAP:
        return "its FCKMAP tag names another address than the slot's: built for another map";
    default:
        return "no hash of its own: MCUboot would not boot it";
    }
}

static fck_flash_step_t nrf_step(fck_drv_nrf_dfu_step_t step)
{
    switch (step) {
    case FCK_DRV_NRF_DFU_STEP_NONE:
        return FCK_FLASH_DONE;
    case FCK_DRV_NRF_DFU_STEP_IMAGE:
        return FCK_FLASH_CHECK;
    case FCK_DRV_NRF_DFU_STEP_RESET:
    case FCK_DRV_NRF_DFU_STEP_SERVER:
        return FCK_FLASH_CONNECT;
    case FCK_DRV_NRF_DFU_STEP_UPLOAD:
        return FCK_FLASH_WRITE;
    case FCK_DRV_NRF_DFU_STEP_BOOT:
        return FCK_FLASH_BOOT;
    default:
        return FCK_FLASH_WRITE;
    }
}

/* The MCUboot that answered would not boot the image: which it is, and what
 * it wants. */
static const char *loader_note(const fck_drv_nrf_dfu_report_t *r)
{
    if (r->image == FCK_DRV_SMP_IMAGE_LARGER) {
        return "larger than the first modules' slot";
    }
    if (r->loader == FCK_DRV_SMP_LOADER_HASH) {
        return "this MCUboot takes an image by its SHA-256, with no signature: this one is signed for another";
    }
    /* TEMPORARY: pre-production modules. Remove when the production modules ship. */
    return "this module carries a first MCUboot, which takes only an image signed ED25519 over SHA-512";
}

static kode_status_t flash_nrf(const fck_image_t *img, size_t n, fck_flash_report_t *rep)
{
    if (n != 1U || img->data == NULL || img->len == 0U || img->inflated_len != 0U
        || img->md5 != NULL || img->addr != FCK_BSP_NRF_SLOT0) {
        note_set(rep, "the nRF takes one image, at FCK_BSP_NRF_SLOT0");
        return fck_fail(KODE_DENIED, "the nRF takes one image, at FCK_BSP_NRF_SLOT0: it is written through MCUboot's recovery only");
    }

    nrf_job_t j = { .img = img };
    kode_status_t st = fck_aos_run_job(nrf_job, &j, LOCK_MS);
    if (!j.ran) {
        if (st == KODE_TIMEOUT) {
            note_set(rep, "the chips stayed busy");
            fck_aos_error_set("the chips stayed busy: another task holds the module");
        }
        return st;
    }

    rep->step    = nrf_step(j.dfu.failed);
    rep->ms      = j.dfu.ms;
    rep->rc      = j.dfu.rc;
    rep->same    = j.dfu.same;
    rep->written = j.dfu.written;
    if (j.dfu.failed == FCK_DRV_NRF_DFU_STEP_IMAGE) {
        note_set(rep, j.dfu.loader_known ? loader_note(&j.dfu) : image_note(j.dfu.image));
        fck_aos_error_set("the nRF's image was refused before anything was written: the report's note says why");
    } else {
        if (j.dfu.failed == FCK_DRV_NRF_DFU_STEP_BOOT) {
            note_set(rep, "written, and it said nothing once started");
            fck_aos_error_set("the nRF's new image was written and said nothing once started: MCUboot will not boot it, it hangs, or it never prints");
        } else if (j.dfu.before[0] != '\0') {
            (void)snprintf(rep->note, sizeof(rep->note), "the slot held %s", j.dfu.before);
        }
        if (!j.dfu.has_map && !j.dfu.holds_start) {
            /* A warning, not a refusal: see fck_drv_smp_image_holds(). */
            size_t const used = strlen(rep->note);
            (void)snprintf(&rep->note[used], sizeof(rep->note) - used,
                           "%sno 0x%08lX in the image: built for another map?",
                           (used != 0U) ? "; " : "", (unsigned long)FCK_BSP_NRF_IMAGE_START);
        }
    }
    if (st == KODE_OK) {
        greeting(FCK_NRF, FCK_BSP_NRF_BOOT_TO_FIRMWARE_MS, rep);
    }
    return st;
}

/* ---- The call ----------------------------------------------------------------------- */

kode_status_t fck_core_flash(fck_chip_t chip, const fck_image_t *img, size_t n,
                        fck_flash_report_t *rep)
{
    fck_flash_report_t dummy;
    rep = (rep != NULL) ? rep : &dummy;
    if ((chip != FCK_C5 && chip != FCK_NRF) || img == NULL) {
        memset(rep, 0, sizeof(*rep));
        rep->failed = -1;
        rep->rc     = -1;
        return fck_fail(KODE_DENIED, "chip is not FCK_C5 or FCK_NRF, or img is NULL");
    }
    report_start(rep, chip);
    return (chip == FCK_C5) ? flash_c5(img, n, rep) : flash_nrf(img, n, rep);
}
