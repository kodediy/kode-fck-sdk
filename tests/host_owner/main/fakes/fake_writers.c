/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file fake_writers.c
 * @brief The two writers, as the model has them: the C5's ROM loader
 *        (fck_drv_c5_rom.h) and the nRF's DFU (fck_drv_nrf_dfu.h), its probe
 *        among them.
 *
 * Both are jobs of the owner's: they run in its task, with the module to
 * themselves, and they take seconds on a board. The model takes what a case
 * says (sim_rom, sim_dfu), records what it was given, and reboots the chip
 * the way the real writers do, so that what the facade does afterwards - the
 * greeting it waits for - is the real facade's.
 */
#include "fake_world.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

sim_rom_t sim_rom;
sim_dfu_t sim_dfu;

void sim_writers_reset(void)
{
    memset(&sim_rom, 0, sizeof(sim_rom));
    sim_rom.st            = KODE_OK;
    sim_rom.ms            = 50U;
    sim_rom.boot_ms       = 300U;
    sim_rom.rep.reset     = true;
    sim_rom.rep.connected = true;
    sim_rom.rep.failed    = -1;
    sim_rom.rep.baud      = FCK_DRV_C5_ROM_BAUD;
    sim_rom.rep.ms        = 1234U;
    sim_rom.probe_st         = KODE_OK;
    sim_rom.probe.reset      = true;
    sim_rom.probe.connected  = true;
    sim_rom.probe.failed     = -1;
    sim_rom.probe.baud       = FCK_BSP_C5_BAUD;
    sim_rom.probe.ms         = 1100U;
    sim_rom.probe.flash_size = 4U * 1024U * 1024U;
    static const uint8_t MAC[6] = { 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xC5 };
    memcpy(sim_rom.probe.mac, MAC, sizeof(MAC));
    fck_drv_c5_boot_init(&sim_rom.heard);
    sim_rom.heard.restarts = 1U;
    sim_rom.heard.entry    = true;
    sim_rom.heard_ms       = 900U;
    memset(&sim_dfu, 0, sizeof(sim_dfu));
    sim_dfu.st         = KODE_OK;
    sim_dfu.ms         = 50U;
    sim_dfu.boot_ms    = 300U;
    sim_dfu.rep.failed = FCK_DRV_NRF_DFU_STEP_NONE;
    sim_dfu.rep.rc     = -1;
    sim_dfu.rep.holds_start = true;
    sim_dfu.probe_st        = KODE_OK;
    sim_dfu.probe.reset     = true;
}

/* fck_drv_c5_rom.c's checks of the regions, as that file makes them: the model of
 * the loader must refuse what the loader refuses. */
static bool hex_digit(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

static bool md5_ok(const char *md5)
{
    for (size_t i = 0U; i < FCK_DRV_C5_ROM_MD5_LENGTH; i++) {
        if (!hex_digit(md5[i])) {
            return false;
        }
    }
    return md5[FCK_DRV_C5_ROM_MD5_LENGTH] == '\0';
}

static bool part_ok(const fck_drv_c5_rom_part_t *p)
{
    if (p->data == NULL || p->size == 0U || (p->size % 4U) != 0U
        || (p->addr % FCK_DRV_C5_ROM_SECTOR) != 0U || p->addr >= FCK_DRV_C5_ROM_REACH
        || p->size > FCK_DRV_C5_ROM_REACH - p->addr || p->zsize > FCK_DRV_C5_ROM_REACH) {
        return false;
    }
    if (p->md5 != NULL && !md5_ok(p->md5)) {
        return false;
    }
    return p->zsize == 0U || p->md5 != NULL;
}

bool fck_drv_c5_rom_parts_ok(const fck_drv_c5_rom_part_t *parts, size_t n)
{
    if (parts == NULL || n == 0U || n > FCK_DRV_C5_ROM_PARTS_MAX) {
        return false;
    }
    for (size_t i = 0U; i < n; i++) {
        if (!part_ok(&parts[i])) {
            return false;
        }
        for (size_t j = 0U; j < i; j++) {
            if (parts[i].addr < parts[j].addr + parts[j].size
                && parts[j].addr < parts[i].addr + parts[i].size) {
                return false;
            }
        }
    }
    return true;
}

kode_status_t fck_drv_c5_rom_write(const fck_drv_pins_t *hw, int uart, const fck_drv_c5_rom_part_t *parts,
                               size_t n, uint32_t baud, fck_drv_c5_rom_progress_fn cb, void *ctx,
                               fck_drv_c5_rom_report_t *report)
{
    (void)cb;
    (void)ctx;
    sim_rom.calls++;
    sim_rom.n = (n <= FCK_DRV_C5_ROM_PARTS_MAX) ? n : FCK_DRV_C5_ROM_PARTS_MAX;
    if (parts != NULL) {
        memcpy(sim_rom.parts, parts, sim_rom.n * sizeof(parts[0]));
    }
    sim_rom.baud     = baud;
    sim_rom.in_owner = !sim_in_case_task();
    sim_rom.ok_args  = (hw != NULL) && (uart >= 0);
    sim_sleep(sim_rom.ms);
    if (report != NULL) {
        *report = sim_rom.rep;
    }
    if (sim_rom.rep.reset) {
        sim_chip_boot(SIM_C5, sim_rom.boot_ms);
    }
    return sim_rom.st;
}

kode_status_t fck_drv_c5_rom_probe(const fck_drv_pins_t *hw, int uart, fck_drv_c5_rom_report_t *report)
{
    sim_rom.probes++;
    sim_rom.in_owner = !sim_in_case_task();
    sim_rom.ok_args  = (hw != NULL) && (uart >= 0);
    sim_sleep(sim_rom.ms);
    if (report != NULL) {
        *report = sim_rom.probe;
    }
    if (sim_rom.probe.reset) {
        sim_chip_boot(SIM_C5, sim_rom.boot_ms);
    }
    return sim_rom.probe_st;
}

kode_status_t fck_drv_c5_rom_listen(const fck_drv_pins_t *hw, int uart, uint32_t ms,
                                    fck_drv_c5_boot_t *out, uint32_t *listened_ms)
{
    sim_rom.listens++;
    if (listened_ms != NULL) {
        *listened_ms = 0U;
    }
    if (out == NULL) {
        return KODE_DENIED;
    }
    fck_drv_c5_boot_init(out);
    if ((hw == NULL) || (uart < 0)) {
        return KODE_DENIED;
    }
    sim_rom.listen_limit_ms = ms;
    sim_rom.in_owner        = !sim_in_case_task();
    sim_sleep(sim_rom.heard_ms);
    *out = sim_rom.heard;
    if (listened_ms != NULL) {
        *listened_ms = sim_rom.heard_ms;
    }
    /* What it heard was the C5 starting: it runs, and answers as the model
     * has it. */
    sim_chip_boot(SIM_C5, 0U);
    return KODE_OK;
}

kode_status_t fck_drv_nrf_dfu_recover(const fck_drv_nrf_dfu_io_t *io, const uint8_t *img, size_t len,
                                  fck_drv_nrf_dfu_report_t *rep)
{
    sim_dfu.recovers++;
    sim_dfu.img      = img;
    sim_dfu.len      = len;
    sim_dfu.had_tca  = (io != NULL) && (io->tca != NULL);
    sim_dfu.in_owner = !sim_in_case_task();
    sim_sleep(sim_dfu.ms);
    /* As the real one: the nRF is not reset for an image refused by what it
     * says of itself, nor when the reset failed; it is for one MCUboot was
     * asked about and would not boot. */
    fck_drv_nrf_dfu_report_t r = sim_dfu.rep;
    r.reset = (r.failed != FCK_DRV_NRF_DFU_STEP_RESET)
              && (r.failed != FCK_DRV_NRF_DFU_STEP_IMAGE || r.loader_known);
    if (rep != NULL) {
        *rep = r;
    }
    if (r.reset) {
        sim_chip_boot(SIM_NRF, sim_dfu.boot_ms);
    }
    return sim_dfu.st;
}

kode_status_t fck_drv_nrf_dfu_probe(const fck_drv_nrf_dfu_io_t *io, fck_drv_nrf_dfu_probe_t *rep)
{
    sim_dfu.probes++;
    sim_dfu.had_tca  = (io != NULL) && (io->tca != NULL);
    sim_dfu.in_owner = !sim_in_case_task();
    if (rep != NULL) {
        memset(rep, 0, sizeof(*rep));
    }
    if (!sim_dfu.had_tca) {
        return KODE_DENIED;       /* as the real one: no expander, no reset */
    }
    sim_sleep(sim_dfu.ms);
    if (rep != NULL) {
        *rep = sim_dfu.probe;
    }
    if (sim_dfu.probe.reset) {
        sim_chip_boot(SIM_NRF, 0U);
    }
    return sim_dfu.probe_st;
}
