/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file fck_rescue.c
 * @brief What a chip carries: fck_core_rescue_probe() in fck.h.
 *
 * The probe is a job of the owner's (fck_aos_run_job()), as a write is. The
 * nRF is reset and what answers is read (fck_drv_nrf_dfu_probe()). The C5's
 * ROM loader is asked what the chip is (fck_drv_c5_rom_probe()), and then
 * what the C5 prints from a reset is read (fck_drv_c5_rom_listen()). What
 * the image says it runs is asked for after the job, through the owner, only
 * of an image that printed the firmware's banner or, the nRF, answers SMP: any
 * other could take `info` for something else.
 */
#include "fck.h"

#include <string.h>

#include "fck_drv_c5_rom.h"
#include "fck_drv_nrf_dfu.h"
#include "fck_aos.h"
#include "fck_private.h"

/* How long the caller waits for both chips to be free. */
#define LOCK_MS  10000U
/* How long an image that printed its line or answered SMP has to answer
 * `info`: it is up already. */
#define FIRMWARE_MS 3000U

typedef struct {
    fck_chip_t              chip;
    fck_drv_nrf_dfu_probe_t probe;    /* the nRF                              */
    kode_status_t           rom_st;   /* the C5: what its ROM loader said, */
    fck_drv_c5_rom_report_t rom;
    fck_drv_c5_boot_t       boot;     /* and what it printed from the reset */
    uint32_t                boot_ms;
    bool                    ran;
} rescue_job_t;

/* The ROM loader on the C5's lines answered as another chip. */
static bool not_a_c5(kode_status_t st, const fck_drv_c5_rom_report_t *rom)
{
    return (st == KODE_DENIED) && rom->connected && !rom->secure_boot && !rom->flash_encryption;
}

static kode_status_t rescue_job(void *arg, const fck_aos_job_context_t *job)
{
    /* cppcheck-suppress misra-c2012-11.5 ; a job's argument comes back as fck_aos_run_job() was given it: this file's rescue_job_t */
    rescue_job_t *const j = arg;
    j->ran = true;
    if (j->chip == FCK_NRF) {
        const fck_drv_nrf_dfu_io_t io = { .uart = job->uart[FCK_AOS_NRF], .tca = job->tca };
        kode_status_t const        st = fck_drv_nrf_dfu_probe(&io, &j->probe);
        if (j->probe.reset) {
            job->chip_on[FCK_AOS_NRF] = true;
        }
        return st;
    }
    j->rom_st = fck_drv_c5_rom_probe(job->hw, job->uart[FCK_AOS_C5], &j->rom);
    if (j->rom.reset) {
        job->chip_on[FCK_AOS_C5] = true;
    }
    if (not_a_c5(j->rom_st, &j->rom)) {
        return KODE_DENIED;
    }
    kode_status_t const st = fck_drv_c5_rom_listen(job->hw, job->uart[FCK_AOS_C5],
                                                   FCK_DRV_C5_ROM_LISTEN_MS, &j->boot, &j->boot_ms);
    if (st == KODE_OK) {
        job->chip_on[FCK_AOS_C5] = true;
    }
    return st;
}

static fck_rescue_state_t nrf_state(const fck_drv_nrf_dfu_probe_t *p, const fck_firmware_t *firmware)
{
    if (firmware->known && firmware->official) {
        return FCK_RESCUE_OFFICIAL;
    }
    if ((p->line[0] != '\0') || p->banner || p->late || p->running) {
        return FCK_RESCUE_FIRMWARE;
    }
    if (p->answered) {
        /* With no image running, only MCUboot answers the list. */
        return FCK_RESCUE_BOOTLOADER;
    }
    return p->mcuboot ? FCK_RESCUE_QUIET : FCK_RESCUE_NOTHING;
}

static fck_rescue_state_t c5_state(const rescue_job_t *j, const fck_firmware_t *firmware)
{
    if (firmware->known && firmware->official) {
        return FCK_RESCUE_OFFICIAL;
    }
    if (fck_drv_c5_boot_line(&j->boot)[0] != '\0') {
        return FCK_RESCUE_FIRMWARE;
    }
    if ((j->boot.restarts >= FCK_DRV_C5_BOOT_LOOP) || (j->boot.no_image >= FCK_DRV_C5_BOOT_LOOP)) {
        return FCK_RESCUE_BOOTLOADER;
    }
    return (j->rom.connected || (j->boot.restarts > 0U)) ? FCK_RESCUE_QUIET : FCK_RESCUE_NOTHING;
}

static kode_status_t nrf_report(const rescue_job_t *j, kode_status_t st, fck_rescue_report_t *out)
{
    /* The firmware's banner, as it prints it when it starts. */
    static const char BANNER[] = "#NRFINFO";

    const fck_drv_nrf_dfu_probe_t *const p = &j->probe;
    out->bootloader    = p->mcuboot;
    out->bootloader_ms = p->mcuboot_ms;
    out->one_slot      = p->echo;
    out->image         = p->image;
    out->ms            = p->ms;
    (void)memcpy(out->version, p->version, sizeof(out->version));
    (void)memcpy(out->hash, p->hash, sizeof(out->hash));
    _Static_assert(sizeof(out->line) == sizeof(p->line), "fck_rescue_report_t.line holds the probe's");
    (void)memcpy(out->line, p->line, sizeof(out->line));
    if (st != KODE_OK) {
        return st;
    }
    if (p->late || p->running || (strncmp(p->line, BANNER, sizeof(BANNER) - 1U) == 0)) {
        fck_aos_firmware_t a = { 0 };
        (void)fck_aos_firmware(FCK_AOS_NRF, FIRMWARE_MS, &a);
        fck_firmware_fill(FCK_AOS_NRF, &a, &out->firmware);
    }
    out->state = nrf_state(p, &out->firmware);
    return KODE_OK;
}

static kode_status_t c5_report(const rescue_job_t *j, kode_status_t st, fck_rescue_report_t *out)
{
    /* The firmware's banner, as it prints it when it starts. */
    static const char BANNER[] = "#C5INFO";

    out->bootloader = j->rom.connected;
    out->locked     = j->rom.secure_boot || j->rom.flash_encryption;
    out->flash_size = j->rom.flash_size;
    out->restarts   = j->boot.restarts;
    out->ms         = j->rom.ms + j->boot_ms;
    (void)memcpy(out->mac, j->rom.mac, sizeof(out->mac));
    _Static_assert(sizeof(out->line) == sizeof(j->boot.line), "fck_rescue_report_t.line holds the C5's first line");
    (void)strncpy(out->line, fck_drv_c5_boot_line(&j->boot), sizeof(out->line) - 1U);
    out->line[sizeof(out->line) - 1U] = '\0';
    if (st != KODE_OK) {
        return st;
    }
    if (j->rom_st == KODE_IO_ERROR) {
        /* The loader answered and a question failed, or the UART refused. */
        return KODE_IO_ERROR;
    }
    if (strncmp(out->line, BANNER, sizeof(BANNER) - 1U) == 0) {
        fck_aos_firmware_t a = { 0 };
        (void)fck_aos_firmware(FCK_AOS_C5, FIRMWARE_MS, &a);
        fck_firmware_fill(FCK_AOS_C5, &a, &out->firmware);
    }
    out->state = c5_state(j, &out->firmware);
    return KODE_OK;
}

/* cppcheck-suppress misra-c2012-8.7 ; an app's call, declared in fck.h: the SDK itself never makes it */
kode_status_t fck_core_rescue_probe(fck_chip_t chip, fck_rescue_report_t *out)
{
    if (out == NULL) {
        return fck_fail(KODE_DENIED, "out is NULL");
    }
    (void)memset(out, 0, sizeof(*out));
    out->state = FCK_RESCUE_NOTHING;
    if ((chip != FCK_NRF) && (chip != FCK_C5)) {
        return fck_fail(KODE_DENIED, "chip is not FCK_C5 or FCK_NRF");
    }

    rescue_job_t  j  = { .chip = chip };
    kode_status_t st = fck_aos_run_job(rescue_job, &j, LOCK_MS);
    if (!j.ran) {
        /* No module, or the chips stayed busy: nothing was touched. */
        return (st == KODE_TIMEOUT) ? fck_fail(st, "the chips stayed busy: another task holds the module") : st;
    }
    return (chip == FCK_NRF) ? nrf_report(&j, st, out) : c5_report(&j, st, out);
}
