/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_4_flash.c
 * @brief Writing firmware through the facade: fck_core_flash() for both chips.
 *
 * The two writers are the model's (fake_writers.c): they take what a case says and
 * record what they were given. What these cases check is what the facade does
 * around them: which images it lets through, how it maps each writer's word to
 * the report, who runs the job, and what it asks the chip afterwards.
 */
#include <stdio.h>
#include <string.h>

#include "unity.h"

#include "fck.h"
#include "fck_core.h"
#include "fck_aos.h"

#include "fake_world.h"

static const uint8_t k_boot[8]  = { 1, 2, 3, 4, 5, 6, 7, 8 };
static const uint8_t k_part[4]  = { 9, 10, 11, 12 };
static const uint8_t k_app[16]  = { 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28 };
static const uint8_t k_nrf[64]  = { 0x3D, 0xB8, 0xF3, 0x96 };
static const char    k_md5[]    = "0123456789abcdef0123456789ABCDEF";

#define C5_IMAGES                                                                       \
    {                                                                                   \
        { .addr = FCK_BSP_C5_BOOTLOADER, .data = k_boot, .len = sizeof(k_boot) },       \
        { .addr = FCK_BSP_C5_PARTITIONS, .data = k_part, .len = sizeof(k_part) },       \
        { .addr = FCK_BSP_C5_APP, .data = k_app, .len = sizeof(k_app) },                \
    }

static void assert_report_blank(const fck_flash_report_t *r)
{
    TEST_ASSERT_EQUAL(FCK_FLASH_CHECK, r->step);
    TEST_ASSERT_EQUAL_INT(-1, r->failed);
    TEST_ASSERT_EQUAL_INT32(-1, r->rc);
    TEST_ASSERT_FALSE(r->same);
    TEST_ASSERT_EQUAL_UINT32(0, r->written);
    TEST_ASSERT_EQUAL_UINT32(0, r->ms);
}

/* ---- The C5 ----------------------------------------------------------------------------- */

TEST_CASE("a C5 is written by the owner through the ROM loader, and says what it runs after", "[flash][c5]")
{
    sim_begin_fck(NULL, NULL);
    sim_rom.rep.ms = 21000U;
    sim_rom.boot_ms = 4000U;               /* the ROM, the bootloader, Wi-Fi up */
    const fck_image_t  img[3] = C5_IMAGES;
    fck_flash_report_t rep;
    memset(&rep, 0x5A, sizeof(rep));
    uint32_t const t0 = sim_now_ms();
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_flash(FCK_C5, img, 3U, &rep));
    TEST_ASSERT_TRUE_MESSAGE(sim_now_ms() - t0 >= 3900U, "it waited for the C5 to start");

    TEST_ASSERT_EQUAL(FCK_FLASH_DONE, rep.step);
    TEST_ASSERT_EQUAL_INT(-1, rep.failed);
    TEST_ASSERT_EQUAL_INT32(-1, rep.rc);
    TEST_ASSERT_FALSE(rep.same);
    TEST_ASSERT_EQUAL_UINT32(8U + 4U + 16U, rep.written);
    TEST_ASSERT_EQUAL_UINT32(21000U, rep.ms);
    TEST_ASSERT_EQUAL_STRING("", rep.note);
    TEST_ASSERT_TRUE_MESSAGE(rep.before.known && rep.before.official, "what it ran before");
    TEST_ASSERT_EQUAL_UINT8(2, rep.before.ver_major);
    TEST_ASSERT_TRUE_MESSAGE(rep.after.known && rep.after.official, "and after");

    TEST_ASSERT_EQUAL_UINT32(1, sim_rom.calls);
    TEST_ASSERT_EQUAL_UINT(3, sim_rom.n);
    TEST_ASSERT_EQUAL_UINT32(FCK_BSP_C5_BOOTLOADER, sim_rom.parts[0].addr);
    TEST_ASSERT_EQUAL_UINT32(FCK_BSP_C5_PARTITIONS, sim_rom.parts[1].addr);
    TEST_ASSERT_EQUAL_UINT32(FCK_BSP_C5_APP, sim_rom.parts[2].addr);
    TEST_ASSERT_EQUAL_PTR(k_boot, sim_rom.parts[0].data);
    TEST_ASSERT_EQUAL_PTR(k_app, sim_rom.parts[2].data);
    TEST_ASSERT_EQUAL_UINT32(8, sim_rom.parts[0].size);
    TEST_ASSERT_EQUAL_UINT32(0, sim_rom.parts[0].zsize);
    TEST_ASSERT_NULL(sim_rom.parts[0].md5);
    TEST_ASSERT_EQUAL_UINT32(FCK_DRV_C5_ROM_BAUD, sim_rom.baud);
    TEST_ASSERT_TRUE_MESSAGE(sim_rom.in_owner, "the job runs in the owner's task, with the module to itself");
    TEST_ASSERT_TRUE(sim_rom.ok_args);

    fck_status_t info;
    fck_core_status(&info);
    TEST_ASSERT_TRUE(info.c5_on);
}

TEST_CASE("a deflated image goes to the loader as its stream and what it inflates to", "[flash][c5]")
{
    sim_begin_fck(NULL, NULL);
    const fck_image_t img[1] = {
        { .addr = FCK_BSP_C5_APP, .data = k_app, .len = 12U, .inflated_len = 64U, .md5 = k_md5 },
    };
    fck_flash_report_t rep;
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_flash(FCK_C5, img, 1U, &rep));
    TEST_ASSERT_EQUAL_UINT32(1, sim_rom.calls);
    TEST_ASSERT_EQUAL_UINT32(64, sim_rom.parts[0].size);
    TEST_ASSERT_EQUAL_UINT32(12, sim_rom.parts[0].zsize);
    TEST_ASSERT_EQUAL_PTR(k_md5, sim_rom.parts[0].md5);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(64, rep.written, "by what the images hold once inflated");

    /* A raw image may carry its digest, too. */
    sim_begin_fck(NULL, NULL);
    const fck_image_t raw[1] = { { .addr = FCK_BSP_C5_APP, .data = k_app, .len = 16U, .md5 = k_md5 } };
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_flash(FCK_C5, raw, 1U, &rep));
    TEST_ASSERT_EQUAL_UINT32(16, sim_rom.parts[0].size);
    TEST_ASSERT_EQUAL_UINT32(0, sim_rom.parts[0].zsize);
    TEST_ASSERT_EQUAL_PTR(k_md5, sim_rom.parts[0].md5);
}

TEST_CASE("images the loader would not take are refused before anything is touched", "[flash][c5]")
{
    sim_begin_fck(NULL, NULL);
    static const uint8_t big[8192];
    fck_flash_report_t   rep;

    const fck_image_t none[1] = { { .addr = FCK_BSP_C5_APP, .data = k_app, .len = 16U } };
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_core_flash(FCK_C5, none, 0U, &rep));
    assert_report_blank(&rep);
    TEST_ASSERT_NOT_EQUAL('\0', rep.note[0]);

    fck_image_t nine[9];
    for (size_t i = 0U; i < 9U; i++) {
        nine[i] = (fck_image_t){ .addr = (uint32_t)(0x10000U + 0x1000U * i), .data = k_part, .len = 4U };
    }
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_core_flash(FCK_C5, nine, 9U, &rep));
    assert_report_blank(&rep);
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_flash(FCK_C5, nine, 8U, &rep));
    TEST_ASSERT_EQUAL_UINT32(1, sim_rom.calls);
    sim_rom.calls = 0U;

    const struct {
        const char *why;
        fck_image_t img[2];
        size_t      n;
    } bad[] = {
        { "an address off a sector", { { .addr = 0x2001U, .data = k_boot, .len = 8U } }, 1U },
        { "a length that is not a multiple of 4", { { .addr = 0x10000U, .data = k_boot, .len = 5U } }, 1U },
        { "nothing in it", { { .addr = 0x10000U, .data = k_boot, .len = 0U } }, 1U },
        { "no data", { { .addr = 0x10000U, .data = NULL, .len = 8U } }, 1U },
        { "two that share a byte",
          { { .addr = 0x10000U, .data = big, .len = 8192U }, { .addr = 0x11000U, .data = k_part, .len = 4U } },
          2U },
        { "a deflated one with no digest",
          { { .addr = 0x10000U, .data = k_boot, .len = 8U, .inflated_len = 64U } }, 1U },
        { "a digest that is not 32 hex digits",
          { { .addr = 0x10000U, .data = k_boot, .len = 8U, .md5 = "0123456789abcdef0123456789abcde" } }, 1U },
        { "a digest with a letter past f",
          { { .addr = 0x10000U, .data = k_boot, .len = 8U, .md5 = "0123456789abcdef0123456789abcdeg" } }, 1U },
        { "past the end of the flash",
          { { .addr = 0xFFF000U, .data = big, .len = 8192U } }, 1U },
    };
    for (size_t i = 0U; i < sizeof(bad) / sizeof(bad[0]); i++) {
        memset(&rep, 0x5A, sizeof(rep));
        TEST_ASSERT_EQUAL_MESSAGE(KODE_DENIED, fck_core_flash(FCK_C5, bad[i].img, bad[i].n, &rep), bad[i].why);
        TEST_ASSERT_EQUAL_MESSAGE(FCK_FLASH_CHECK, rep.step, bad[i].why);
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, rep.written, bad[i].why);
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, sim_rom.calls, bad[i].why);
    }
}

TEST_CASE("a bad chip, no images or no report: refused or tolerated, never a crash", "[flash][c5]")
{
    sim_begin_fck(NULL, NULL);
    const fck_image_t  img[1] = { { .addr = FCK_BSP_C5_APP, .data = k_app, .len = 16U } };
    fck_flash_report_t rep;
    memset(&rep, 0x5A, sizeof(rep));
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_core_flash((fck_chip_t)2, img, 1U, &rep));
    assert_report_blank(&rep);
    memset(&rep, 0x5A, sizeof(rep));
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_core_flash(FCK_C5, NULL, 1U, &rep));
    assert_report_blank(&rep);
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_flash(FCK_C5, img, 1U, NULL));
    TEST_ASSERT_EQUAL_UINT32(1, sim_rom.calls);
}

TEST_CASE("no module: DENIED, with the report as a call that stopped at the door", "[flash][c5]")
{
    sim_begin_empty();
    const fck_image_t  img[1] = { { .addr = FCK_BSP_C5_APP, .data = k_app, .len = 16U } };
    fck_flash_report_t rep;
    memset(&rep, 0x5A, sizeof(rep));
    uint32_t const t0 = sim_now_ms();
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_core_flash(FCK_C5, img, 1U, &rep));
    TEST_ASSERT_TRUE(sim_now_ms() - t0 < 1000U);
    assert_report_blank(&rep);
    TEST_ASSERT_FALSE(rep.before.known);
    TEST_ASSERT_EQUAL_UINT32(0, sim_rom.calls);
}

TEST_CASE("the loader's words become the report's step, and the C5 runs again after", "[flash][c5]")
{
    static const struct {
        const char   *why;
        kode_status_t st;
        int           failed;       /* the region */
        int           loader_error;
        bool          reset;
        fck_flash_step_t step;
        uint32_t      written;
    } rows[] = {
        { "the second image failed its check", KODE_IO_ERROR, 1, 7, true, FCK_FLASH_WRITE, 8U },
        { "the first image failed", KODE_IO_ERROR, 0, 5, true, FCK_FLASH_WRITE, 0U },
        { "the loader answered, then nothing", KODE_IO_ERROR, -1, 3, true, FCK_FLASH_CONNECT, 0U },
        { "the loader never answered", KODE_TIMEOUT, -1, 1, true, FCK_FLASH_CONNECT, 0U },
        { "secure boot is on", KODE_DENIED, -1, 0, false, FCK_FLASH_CHECK, 0U },
        { "no room for the block buffer", KODE_NO_RESOURCES, -1, 0, false, FCK_FLASH_CHECK, 0U },
        { "something else", KODE_ERROR, 2, 9, true, FCK_FLASH_WRITE, 12U },
    };
    const fck_image_t img[3] = C5_IMAGES;
    for (size_t i = 0U; i < sizeof(rows) / sizeof(rows[0]); i++) {
        sim_begin_fck(NULL, NULL);
        sim_rom.st                = rows[i].st;
        sim_rom.rep.failed        = rows[i].failed;
        sim_rom.rep.loader_error  = rows[i].loader_error;
        sim_rom.rep.reset         = rows[i].reset;
        sim_rom.rep.connected     = (rows[i].st != KODE_TIMEOUT);
        sim_rom.rep.ms            = 777U;
        (void)snprintf(sim_rom.rep.note, sizeof(sim_rom.rep.note), "note %zu", i);
        fck_flash_report_t rep;
        memset(&rep, 0x5A, sizeof(rep));
        TEST_ASSERT_EQUAL_MESSAGE(rows[i].st, fck_core_flash(FCK_C5, img, 3U, &rep), rows[i].why);
        TEST_ASSERT_EQUAL_MESSAGE(rows[i].step, rep.step, rows[i].why);
        TEST_ASSERT_EQUAL_INT_MESSAGE(rows[i].failed, rep.failed, rows[i].why);
        TEST_ASSERT_EQUAL_INT32_MESSAGE(rows[i].loader_error != 0 ? rows[i].loader_error : -1, rep.rc,
                                        rows[i].why);
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(rows[i].written, rep.written, rows[i].why);
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(777U, rep.ms, rows[i].why);
        char want[16];
        (void)snprintf(want, sizeof(want), "note %zu", i);
        TEST_ASSERT_EQUAL_STRING_MESSAGE(want, rep.note, rows[i].why);
        TEST_ASSERT_FALSE_MESSAGE(rep.after.known, "no word of what it runs after a write that failed");
        TEST_ASSERT_TRUE_MESSAGE(rep.before.known, rows[i].why);

        /* Whatever happened, the owner has the C5 on again, and it says what it runs. */
        fck_status_t info;
        fck_core_status(&info);
        TEST_ASSERT_TRUE_MESSAGE(info.c5_on, rows[i].why);
        fck_firmware_t a;
        TEST_ASSERT_EQUAL_MESSAGE(KODE_OK, fck_core_firmware(FCK_C5, &a), rows[i].why);
        TEST_ASSERT_TRUE_MESSAGE(a.known && a.official, rows[i].why);
    }
}

TEST_CASE("a C5 held in reset is left in reset by a loader that did not touch it", "[flash][c5]")
{
    sim_begin_fck(NULL, NULL);
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_power(FCK_C5, false));
    sim_rom.st         = KODE_DENIED;
    sim_rom.rep.reset  = false;
    const fck_image_t  img[1] = { { .addr = FCK_BSP_C5_APP, .data = k_app, .len = 16U } };
    fck_flash_report_t rep;
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_core_flash(FCK_C5, img, 1U, &rep));
    fck_status_t info;
    fck_core_status(&info);
    TEST_ASSERT_FALSE_MESSAGE(info.c5_on, "its lines were not touched");
    TEST_ASSERT_FALSE(rep.before.known);

    /* One that did write, and reset it into its flash, leaves it running. */
    sim_rom.st        = KODE_OK;
    sim_rom.rep.reset = true;
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_flash(FCK_C5, img, 1U, &rep));
    fck_core_status(&info);
    TEST_ASSERT_TRUE(info.c5_on);
}

TEST_CASE("what the C5 ran before is what the owner kept, and an image that is silent leaves after unknown", "[flash][c5][slow]")
{
    sim_begin_fck(NULL, NULL);
    sim_firmware(SIM_C5, NULL);         /* the new image prints no banner and answers no `info` */
    const fck_image_t  img[1] = { { .addr = FCK_BSP_C5_APP, .data = k_app, .len = 16U } };
    fck_flash_report_t rep;
    uint32_t const     t0 = sim_now_ms();
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_flash(FCK_C5, img, 1U, &rep));
    uint32_t const took = sim_now_ms() - t0;
    TEST_ASSERT_TRUE_MESSAGE(rep.before.known && rep.before.official, "the 2.0 firmware it ran");
    TEST_ASSERT_FALSE_MESSAGE(rep.after.known, "it said nothing");
    TEST_ASSERT_EQUAL(FCK_FLASH_DONE, rep.step);
    TEST_ASSERT_TRUE_MESSAGE(took >= 6900U && took < 9000U, "the C5's 5 s and 2 s more");
    fck_status_t info;
    fck_core_status(&info);
    TEST_ASSERT_TRUE(info.c5_on);
}

TEST_CASE("a C5 that has not said what it runs yet has no `before`, and the write still works", "[flash][c5]")
{
    sim_begin_fck(NULL, NULL);
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_reset(FCK_C5));      /* its banner comes 60 ms later */
    const fck_image_t  img[1] = { { .addr = FCK_BSP_C5_APP, .data = k_app, .len = 16U } };
    fck_flash_report_t rep;
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_flash(FCK_C5, img, 1U, &rep));
    TEST_ASSERT_FALSE(rep.before.known);
    TEST_ASSERT_EQUAL_UINT8(0, rep.before.ver_major);
    TEST_ASSERT_EQUAL_STRING("", rep.before.fw);
    TEST_ASSERT_TRUE_MESSAGE(rep.after.known, "the new image said what it runs");
}

static void hold_nrf_for_a_while(void)
{
    if (fck_aos_hold(FCK_AOS_NRF, 2000U) == KODE_OK) {
        sim_sleep(12000U);
        (void)fck_aos_release(FCK_AOS_NRF);
    }
}

TEST_CASE("another task keeping a chip for 10 s: TIMEOUT, nothing written, and a word why", "[flash][c5][slow]")
{
    sim_begin_fck(NULL, NULL);
    sim_later(0U, hold_nrf_for_a_while);
    sim_sleep(200U);
    const fck_image_t  img[1] = { { .addr = FCK_BSP_C5_APP, .data = k_app, .len = 16U } };
    fck_flash_report_t rep;
    uint32_t const     t0 = sim_now_ms();
    TEST_ASSERT_EQUAL_MESSAGE(KODE_TIMEOUT, fck_core_flash(FCK_C5, img, 1U, &rep), "the owner's wait ran out");
    uint32_t const took = sim_now_ms() - t0;
    TEST_ASSERT_TRUE_MESSAGE(took >= 9900U && took < 11500U, "the 10 s it waits for both chips");
    TEST_ASSERT_EQUAL_MESSAGE(FCK_FLASH_CHECK, rep.step, "before the loader: told apart from its TIMEOUT by the step");
    TEST_ASSERT_EQUAL_UINT32(0, sim_rom.calls);
    TEST_ASSERT_NOT_EQUAL('\0', rep.note[0]);
    sim_sleep(2500U);                       /* until the other task lets go */
}

TEST_CASE("a loader that gave up at the door is the loader's TIMEOUT, not the owner's", "[flash][c5]")
{
    sim_begin_fck(NULL, NULL);
    sim_rom.st                = KODE_TIMEOUT;
    sim_rom.rep.connected     = false;
    const fck_image_t  img[1] = { { .addr = FCK_BSP_C5_APP, .data = k_app, .len = 16U } };
    fck_flash_report_t rep;
    TEST_ASSERT_EQUAL_MESSAGE(KODE_TIMEOUT, fck_core_flash(FCK_C5, img, 1U, &rep), "the job ran: its word stands");
    TEST_ASSERT_EQUAL(FCK_FLASH_CONNECT, rep.step);
    TEST_ASSERT_EQUAL_UINT32(1, sim_rom.calls);
}

/* ---- The nRF ------------------------------------------------------------------------------ */

TEST_CASE("an nRF image is written by the owner through MCUboot's recovery, and the report is the writer's", "[flash][nrf]")
{
    sim_begin_fck(NULL, NULL);
    sim_dfu.rep.written = 64U;
    sim_dfu.rep.ms      = 2222U;
    sim_dfu.boot_ms     = 6300U;      /* MCUboot, then the new image's banner */
    (void)snprintf(sim_dfu.rep.before, sizeof(sim_dfu.rep.before), "1.0.0");
    const fck_image_t  img = { .addr = FCK_BSP_NRF_SLOT0, .data = k_nrf, .len = sizeof(k_nrf) };
    fck_flash_report_t rep;
    memset(&rep, 0x5A, sizeof(rep));
    uint32_t const t0 = sim_now_ms();
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_flash(FCK_NRF, &img, 1U, &rep));
    TEST_ASSERT_TRUE_MESSAGE(sim_now_ms() - t0 >= 6200U, "it waited for the image's own banner");

    TEST_ASSERT_EQUAL(FCK_FLASH_DONE, rep.step);
    TEST_ASSERT_EQUAL_INT(-1, rep.failed);
    TEST_ASSERT_EQUAL_INT32(-1, rep.rc);
    TEST_ASSERT_FALSE(rep.same);
    TEST_ASSERT_EQUAL_UINT32(64U, rep.written);
    TEST_ASSERT_EQUAL_UINT32(2222U, rep.ms);
    TEST_ASSERT_EQUAL_STRING("the slot held 1.0.0", rep.note);
    TEST_ASSERT_TRUE(rep.before.known && rep.before.official);
    TEST_ASSERT_TRUE_MESSAGE(rep.after.known && rep.after.official, "the image said what it runs");

    TEST_ASSERT_EQUAL_UINT32(1, sim_dfu.recovers);
    TEST_ASSERT_EQUAL_PTR(k_nrf, sim_dfu.img);
    TEST_ASSERT_EQUAL_UINT(sizeof(k_nrf), sim_dfu.len);
    TEST_ASSERT_TRUE(sim_dfu.had_tca);
    TEST_ASSERT_TRUE_MESSAGE(sim_dfu.in_owner, "the job runs in the owner's task");
}

TEST_CASE("an image the nRF held already is `same`, and nothing is said to have been written", "[flash][nrf]")
{
    sim_begin_fck(NULL, NULL);
    sim_dfu.rep.same    = true;
    sim_dfu.rep.written = 0U;
    (void)snprintf(sim_dfu.rep.before, sizeof(sim_dfu.rep.before), "1.0.0");
    const fck_image_t  img = { .addr = FCK_BSP_NRF_SLOT0, .data = k_nrf, .len = sizeof(k_nrf) };
    fck_flash_report_t rep;
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_flash(FCK_NRF, &img, 1U, &rep));
    TEST_ASSERT_TRUE(rep.same);
    TEST_ASSERT_EQUAL_UINT32(0U, rep.written);
    TEST_ASSERT_EQUAL_STRING("the slot held 1.0.0", rep.note);
}

TEST_CASE("the slot is written through MCUboot's recovery, and a slot that held nothing leaves no note", "[flash][nrf]")
{
    sim_begin_fck(NULL, NULL);
    const fck_image_t  img = { .addr = FCK_BSP_NRF_SLOT0, .data = k_nrf, .len = sizeof(k_nrf) };
    fck_flash_report_t rep;
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_flash(FCK_NRF, &img, 1U, &rep));
    TEST_ASSERT_EQUAL_UINT32(1, sim_dfu.recovers);
    TEST_ASSERT_EQUAL_STRING("", rep.note);
}

TEST_CASE("an image built for another map is refused with its reason, or warned about", "[flash][nrf]")
{
    sim_begin_fck(NULL, NULL);
    const fck_image_t  img = { .addr = FCK_BSP_NRF_SLOT0, .data = k_nrf, .len = sizeof(k_nrf) };
    fck_flash_report_t rep;
    sim_dfu.st          = KODE_DENIED;
    sim_dfu.rep.failed  = FCK_DRV_NRF_DFU_STEP_IMAGE;
    sim_dfu.rep.image   = FCK_DRV_SMP_IMAGE_ADDRESS;
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_core_flash(FCK_NRF, &img, 1U, &rep));
    TEST_ASSERT_EQUAL(FCK_FLASH_CHECK, rep.step);
    TEST_ASSERT_EQUAL_STRING("its vectors point outside it in the slot: built for another map", rep.note);

    sim_dfu.rep.image = FCK_DRV_SMP_IMAGE_STACK;
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_core_flash(FCK_NRF, &img, 1U, &rep));
    TEST_ASSERT_EQUAL_STRING("its first stack pointer is not in the nRF's RAM", rep.note);
    sim_dfu.rep.image = FCK_DRV_SMP_IMAGE_LOADER;      /* it fit, and had no hash */
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_core_flash(FCK_NRF, &img, 1U, &rep));
    TEST_ASSERT_EQUAL_STRING("no hash of its own: MCUboot would not boot it", rep.note);
    /* fck-nrf says where it was linked: a tag for another address is a refusal, not a warning. */
    sim_dfu.rep.image   = FCK_DRV_SMP_IMAGE_MAP;
    sim_dfu.rep.has_map = true;
    sim_dfu.rep.map     = 0x00010800U;
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_core_flash(FCK_NRF, &img, 1U, &rep));
    TEST_ASSERT_EQUAL(FCK_FLASH_CHECK, rep.step);
    TEST_ASSERT_EQUAL_STRING("its FCKMAP tag names another address than the slot's: built for another map", rep.note);
    fck_status_t info;
    fck_core_status(&info);
    TEST_ASSERT_TRUE_MESSAGE(info.nrf_on, "the nRF was not touched: it runs what it ran");

    /* Written: a warning on top of the version, and alone without it. One with a tag for the
     * slot needs none, whatever else it holds. */
    sim_writers_reset();
    sim_dfu.rep.holds_start = false;
    (void)snprintf(sim_dfu.rep.before, sizeof(sim_dfu.rep.before), "2.0.1");
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_flash(FCK_NRF, &img, 1U, &rep));
    TEST_ASSERT_EQUAL_STRING("the slot held 2.0.1; no 0x0000E800 in the image: built for another map?",
                             rep.note);
    sim_dfu.rep.before[0] = '\0';
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_flash(FCK_NRF, &img, 1U, &rep));
    TEST_ASSERT_EQUAL_STRING("no 0x0000E800 in the image: built for another map?", rep.note);
    sim_dfu.rep.has_map = true;
    sim_dfu.rep.map     = 0x0000E800U;
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_flash(FCK_NRF, &img, 1U, &rep));
    TEST_ASSERT_EQUAL_STRING("", rep.note);
}

TEST_CASE("an image the MCUboot that answered would not boot is refused with nothing written, and the nRF runs on", "[flash][nrf]")
{
    const fck_image_t  img = { .addr = FCK_BSP_NRF_SLOT0, .data = k_nrf, .len = sizeof(k_nrf) };
    fck_flash_report_t rep;

    /* The module's one-slot MCUboot, and an image signed for a first module's. */
    sim_begin_fck(NULL, NULL);
    sim_dfu.st               = KODE_DENIED;
    sim_dfu.rep.failed       = FCK_DRV_NRF_DFU_STEP_IMAGE;
    sim_dfu.rep.image        = FCK_DRV_SMP_IMAGE_LOADER;
    sim_dfu.rep.loader_known = true;
    sim_dfu.rep.loader       = FCK_DRV_SMP_LOADER_HASH;
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_core_flash(FCK_NRF, &img, 1U, &rep));
    TEST_ASSERT_EQUAL(FCK_FLASH_CHECK, rep.step);
    TEST_ASSERT_EQUAL_UINT32(0U, rep.written);
    TEST_ASSERT_EQUAL_STRING(
        "this MCUboot takes an image by its SHA-256, with no signature: this one is signed for another", rep.note);
    fck_status_t info;
    fck_core_status(&info);
    TEST_ASSERT_TRUE_MESSAGE(info.nrf_on, "it was reset to ask, and let go: it runs what it had");

    /* TEMPORARY: pre-production modules. A first module's MCUboot, and the image the production
     * one takes: uploaded, it would leave the nRF in recovery. */
    sim_begin_fck(NULL, NULL);
    sim_dfu.st               = KODE_DENIED;
    sim_dfu.rep.failed       = FCK_DRV_NRF_DFU_STEP_IMAGE;
    sim_dfu.rep.image        = FCK_DRV_SMP_IMAGE_LOADER;
    sim_dfu.rep.loader_known = true;
    sim_dfu.rep.loader       = FCK_DRV_SMP_LOADER_SIGNED;
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_core_flash(FCK_NRF, &img, 1U, &rep));
    TEST_ASSERT_EQUAL(FCK_FLASH_CHECK, rep.step);
    TEST_ASSERT_EQUAL_STRING(
        "this module carries a first MCUboot, which takes only an image signed ED25519 over SHA-512", rep.note);
    sim_dfu.rep.image = FCK_DRV_SMP_IMAGE_LARGER;
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_core_flash(FCK_NRF, &img, 1U, &rep));
    TEST_ASSERT_EQUAL_STRING("larger than the first modules' slot", rep.note);
}

TEST_CASE("an image that was written and says nothing once started fails the write", "[flash][nrf]")
{
    sim_begin_fck(NULL, NULL);
    sim_dfu.st               = KODE_ERROR;
    sim_dfu.rep.failed       = FCK_DRV_NRF_DFU_STEP_BOOT;
    sim_dfu.rep.written      = sizeof(k_nrf);
    sim_dfu.rep.loader_known = true;
    const fck_image_t  img = { .addr = FCK_BSP_NRF_SLOT0, .data = k_nrf, .len = sizeof(k_nrf) };
    fck_flash_report_t rep;
    TEST_ASSERT_EQUAL(KODE_ERROR, fck_core_flash(FCK_NRF, &img, 1U, &rep));
    TEST_ASSERT_EQUAL(FCK_FLASH_BOOT, rep.step);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(sizeof(k_nrf), rep.written, "it was written all the same");
    TEST_ASSERT_EQUAL_STRING("written, and it said nothing once started", rep.note);
    TEST_ASSERT_NOT_NULL(strstr(fck_aos_error(), "said nothing"));
    TEST_ASSERT_FALSE(rep.after.known);
}

TEST_CASE("the nRF takes one image at its one slot, and nothing else", "[flash][nrf]")
{
    sim_begin_fck(NULL, NULL);
    fck_flash_report_t rep;
    const fck_image_t  ok = { .addr = FCK_BSP_NRF_SLOT0, .data = k_nrf, .len = sizeof(k_nrf) };
    const fck_image_t  two[2] = { ok, ok };
    const fck_image_t  bad[] = {
        { .addr = FCK_BSP_NRF_SLOT0, .data = NULL, .len = sizeof(k_nrf) },
        { .addr = FCK_BSP_NRF_SLOT0, .data = k_nrf, .len = 0U },
        { .addr = FCK_BSP_NRF_SLOT0, .data = k_nrf, .len = sizeof(k_nrf), .inflated_len = 128U },
        { .addr = FCK_BSP_NRF_SLOT0, .data = k_nrf, .len = sizeof(k_nrf), .md5 = k_md5 },
        { .addr = 0U, .data = k_nrf, .len = sizeof(k_nrf) },
        { .addr = FCK_BSP_NRF_SLOT0 + 4096U, .data = k_nrf, .len = sizeof(k_nrf) },
        /* Where the first modules' second slot was: there is no swapping in any more. */
        { .addr = 0xB9000U, .data = k_nrf, .len = sizeof(k_nrf) },
        { .addr = FCK_BSP_C5_APP, .data = k_nrf, .len = sizeof(k_nrf) },
    };
    memset(&rep, 0x5A, sizeof(rep));
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_core_flash(FCK_NRF, two, 2U, &rep));
    assert_report_blank(&rep);
    TEST_ASSERT_NOT_EQUAL('\0', rep.note[0]);
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_core_flash(FCK_NRF, &ok, 0U, &rep));
    for (size_t i = 0U; i < sizeof(bad) / sizeof(bad[0]); i++) {
        memset(&rep, 0x5A, sizeof(rep));
        TEST_ASSERT_EQUAL_INT_MESSAGE(KODE_DENIED, fck_core_flash(FCK_NRF, &bad[i], 1U, &rep), "refused");
        TEST_ASSERT_EQUAL(FCK_FLASH_CHECK, rep.step);
    }
    TEST_ASSERT_EQUAL_UINT32(0, sim_dfu.recovers);
}

TEST_CASE("each step the DFU stops at is the report's, in the pact's mapping", "[flash][nrf]")
{
    static const struct {
        fck_drv_nrf_dfu_step_t dfu;
        kode_status_t      st;
        fck_flash_step_t   step;
        bool               touched;     /* past its reset: the nRF runs */
    } rows[] = {
        { FCK_DRV_NRF_DFU_STEP_IMAGE, KODE_DENIED, FCK_FLASH_CHECK, false },
        { FCK_DRV_NRF_DFU_STEP_RESET, KODE_IO_ERROR, FCK_FLASH_CONNECT, false },
        { FCK_DRV_NRF_DFU_STEP_SERVER, KODE_TIMEOUT, FCK_FLASH_CONNECT, true },
        { FCK_DRV_NRF_DFU_STEP_UPLOAD, KODE_IO_ERROR, FCK_FLASH_WRITE, true },
        { FCK_DRV_NRF_DFU_STEP_BOOT, KODE_ERROR, FCK_FLASH_BOOT, true },
    };
    const fck_image_t img = { .addr = FCK_BSP_NRF_SLOT0, .data = k_nrf, .len = sizeof(k_nrf) };
    for (size_t i = 0U; i < sizeof(rows) / sizeof(rows[0]); i++) {
        sim_begin_fck(NULL, NULL);
        TEST_ASSERT_EQUAL(KODE_OK, fck_core_power(FCK_NRF, false));
        sim_dfu.st             = rows[i].st;
        sim_dfu.rep.failed     = rows[i].dfu;
        sim_dfu.rep.rc         = 4;
        sim_dfu.rep.written    = 10U * (uint32_t)(i + 1U);
        fck_flash_report_t rep;
        memset(&rep, 0x5A, sizeof(rep));
        TEST_ASSERT_EQUAL_INT_MESSAGE(rows[i].st, fck_core_flash(FCK_NRF, &img, 1U, &rep), "the writer's word");
        TEST_ASSERT_EQUAL_INT_MESSAGE(rows[i].step, rep.step, "the step");
        TEST_ASSERT_EQUAL_INT32_MESSAGE(4, rep.rc, "the server's rc");
        TEST_ASSERT_EQUAL_INT_MESSAGE(-1, rep.failed, "the nRF has no region");
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(10U * (uint32_t)(i + 1U), rep.written, "what the server took");
        TEST_ASSERT_FALSE_MESSAGE(rep.after.known, "no word of what it runs after a write that failed");
        fck_status_t info;
        fck_core_status(&info);
        TEST_ASSERT_EQUAL_MESSAGE(rows[i].touched, info.nrf_on,
                                  "the nRF runs once the writer got past its reset");
    }
}

TEST_CASE("with the receiver holding the bus the writer is given no expander", "[flash][nrf]")
{
    sim_begin_fck(NULL, NULL);
    sim_script(SIM_C5, "gnss", "#DONE gnss rc=0\r\n", 5U);
    sim_gnss_standard_fw(true);
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_gnss_begin());
    sim_sleep(1200U);
    TEST_ASSERT_TRUE(sim_bus_dead());
    sim_dfu.st         = KODE_IO_ERROR;
    sim_dfu.rep.failed = FCK_DRV_NRF_DFU_STEP_RESET;
    const fck_image_t  img = { .addr = FCK_BSP_NRF_SLOT0, .data = k_nrf, .len = sizeof(k_nrf) };
    fck_flash_report_t rep;
    TEST_ASSERT_EQUAL(KODE_IO_ERROR, fck_core_flash(FCK_NRF, &img, 1U, &rep));
    TEST_ASSERT_EQUAL(FCK_FLASH_CONNECT, rep.step);
    TEST_ASSERT_EQUAL_UINT32(1, sim_dfu.recovers);
    TEST_ASSERT_FALSE_MESSAGE(sim_dfu.had_tca, "the expander is out of reach");
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_gnss_end());
}
