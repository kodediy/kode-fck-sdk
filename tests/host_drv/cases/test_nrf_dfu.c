/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_nrf_dfu.c
 * @brief fck_drv_nrf_dfu: a new image into the nRF over SMP, and the probe
 *        that looks at what it carries, against a fake MCUboot
 *        (fake_mcuboot.h) behind the fake UART and the fake expander's P6.
 *
 * The driver's own packets go out through fck_drv_smp.c and are read back by
 * the fake, which answers in the framing MCUboot's serial recovery uses; time
 * is the fake ESP-IDF's virtual clock, so the 15 s waits cost nothing. What
 * each case checks is what the driver reported, what the fake MCUboot saw
 * (frames, pieces, resets) and what it was left holding.
 */
#include <string.h>

#include "unity.h"

#include "fck_bsp.h"
#include "fck_drv_nrf_dfu.h"
#include "fck_drv_uart.h"

#include "fake_idf.h"
#include "fake_mcuboot.h"

#define PORT 1

#define CODE_BYTES 500U

static fck_drv_tca6408a_t   s_tca;
static fck_drv_nrf_dfu_io_t s_io;
static fck_drv_nrf_dfu_report_t s_rep;
static fck_drv_nrf_dfu_probe_t  s_probe;

/* The images: 32 bytes of header, a vector table for the slot at 0xE000 and
 * CODE_BYTES of code, then the unprotected TLVs as imgtool writes them. The
 * module's one-slot MCUboot takes the SHA-256 and no signature; a first
 * module's wants the SHA-512 and an ED25519 signature. */
typedef enum { HASHED, SIGNED, NO_HASH } kind_t;

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static size_t put_tlv(uint8_t *img, size_t n, uint16_t type, size_t len, uint8_t seed)
{
    img[n++] = (uint8_t)type;
    img[n++] = (uint8_t)(type >> 8);
    img[n++] = (uint8_t)len;
    img[n++] = (uint8_t)(len >> 8);
    for (size_t k = 0U; k < len; k++) {
        img[n++] = (uint8_t)(seed + k);
    }
    return n;
}

static size_t make_image(uint8_t *img, kind_t kind, uint8_t minor, uint8_t seed)
{
    (void)memset(img, 0, 32U);
    put32(img, FCK_BSP_NRF_IMAGE_MAGIC);
    img[8]  = 32U;                          /* hdr_size */
    put32(img + 12, CODE_BYTES);            /* img_size */
    img[20] = 1U;                           /* version 1.<minor>.3 */
    img[21] = minor;
    img[22] = 3U;
    for (size_t k = 0U; k < CODE_BYTES; k++) {
        img[32U + k] = (uint8_t)(seed * 3U + k);
    }
    put32(img + 32, FCK_BSP_NRF_RAM + FCK_BSP_NRF_RAM_SIZE);
    put32(img + 36, FCK_BSP_NRF_SLOT0 + 0x21U);
    put32(img + 40, FCK_BSP_NRF_SLOT0 + 0x25U);
    put32(img + 44, FCK_BSP_NRF_SLOT0 + 0x29U);

    size_t n = 32U + CODE_BYTES;
    img[n++] = 0x07U;                       /* IMAGE_TLV_INFO_MAGIC 0x6907 */
    img[n++] = 0x69U;
    size_t const total_at = n;
    n += 2U;
    if (kind == NO_HASH) {
        n = put_tlv(img, n, 0x01U, 64U, seed);          /* a key hash, and no image hash */
    } else {
        n = put_tlv(img, n, (kind == HASHED) ? 0x10U : 0x12U, (kind == HASHED) ? 32U : 64U,
                    (uint8_t)(0x80U + seed));
        if (kind == SIGNED) {
            n = put_tlv(img, n, 0x24U, 64U, 0x33U);
        }
    }
    size_t const total = n - (total_at - 2U);   /* the area counts its own head */
    img[total_at]      = (uint8_t)total;
    img[total_at + 1U] = (uint8_t)(total >> 8);
    return n;
}

static uint8_t s_old[1024];
static size_t  s_old_len;
static uint8_t s_new[1024];
static size_t  s_new_len;

/* An nRF in MCUboot's recovery holding an old image (0.9.0 as a first byte
 * pair would say), the module's expander and UART open, and a new image. */
static void begin(kind_t old_kind, kind_t new_kind)
{
    idf_fake_reset();
    idf_fake_expander(0xF7U);
    (void)memset(&s_tca, 0, sizeof(s_tca));
    fck_drv_tca6408a_forget(&s_tca);
    TEST_ASSERT_EQUAL(KODE_OK, fck_drv_tca6408a_open(&s_tca, idf_fake_hw()));
    TEST_ASSERT_EQUAL(KODE_OK, fck_drv_uart_open(PORT, 21, 22, FCK_BSP_NRF_BAUD));
    s_old_len = make_image(s_old, old_kind, 9U, 1U);
    s_new_len = make_image(s_new, new_kind, 2U, 2U);
    s_io.uart = PORT;
    s_io.tca  = &s_tca;
    fake_mcuboot_start(PORT, s_old, s_old_len);
    (void)memset(&s_rep, 0xA5, sizeof(s_rep));
    (void)memset(&s_probe, 0xA5, sizeof(s_probe));
}

static unsigned pieces_of(size_t len)
{
    return (unsigned)((len + FCK_DRV_SMP_CHUNK - 1U) / FCK_DRV_SMP_CHUNK);
}

static void assert_slot_holds(const uint8_t *img, size_t len)
{
    size_t         held;
    const uint8_t *slot = fake_mcuboot_slot(&held);
    TEST_ASSERT_EQUAL_size_t(len, held);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(img, slot, len);
}

/* ---- A write that goes right -------------------------------------------------------- */

TEST_CASE("a new image goes into the one-slot loader piece by piece and the nRF starts it", "[nrf_dfu]")
{
    begin(HASHED, HASHED);
    TEST_ASSERT_EQUAL(KODE_OK, fck_drv_nrf_dfu_recover(&s_io, s_new, s_new_len, &s_rep));
    TEST_ASSERT_EQUAL(FCK_DRV_NRF_DFU_STEP_NONE, s_rep.failed);
    TEST_ASSERT_EQUAL_INT32(-1, s_rep.rc);
    TEST_ASSERT_TRUE(s_rep.reset);
    TEST_ASSERT_FALSE(s_rep.same);
    TEST_ASSERT_TRUE(s_rep.answered);
    TEST_ASSERT_EQUAL_UINT32(s_new_len, s_rep.written);
    TEST_ASSERT_TRUE(s_rep.loader_known);
    TEST_ASSERT_EQUAL(FCK_DRV_SMP_LOADER_HASH, s_rep.loader);
    TEST_ASSERT_EQUAL_STRING("1.9.3", s_rep.before);
    TEST_ASSERT_EQUAL_STRING("#NRFINFO fw=fck-nrf ver=2.0.1", s_rep.line);
    TEST_ASSERT_EQUAL(FCK_DRV_SMP_IMAGE_FITS, s_rep.image);

    assert_slot_holds(s_new, s_new_len);
    TEST_ASSERT_TRUE(fake_mcuboot_slot_boots());
    TEST_ASSERT_EQUAL_UINT(pieces_of(s_new_len), g_mcuboot.pieces);
    TEST_ASSERT_EQUAL_UINT(1U, g_mcuboot.pin_resets);
    TEST_ASSERT_EQUAL_UINT(1U, g_mcuboot.smp_resets);
}

TEST_CASE("an image already in the slot is not written again, and the nRF is let go on it", "[nrf_dfu]")
{
    begin(HASHED, HASHED);
    s_new_len = make_image(s_new, HASHED, 9U, 1U);   /* the old one, byte for byte */
    TEST_ASSERT_EQUAL(KODE_OK, fck_drv_nrf_dfu_recover(&s_io, s_new, s_new_len, &s_rep));
    TEST_ASSERT_TRUE(s_rep.same);
    TEST_ASSERT_EQUAL_UINT32(0U, s_rep.written);
    TEST_ASSERT_TRUE(s_rep.answered);
    TEST_ASSERT_EQUAL_UINT(0U, g_mcuboot.pieces);
    TEST_ASSERT_EQUAL_UINT(1U, g_mcuboot.smp_resets);
    assert_slot_holds(s_old, s_old_len);
}

TEST_CASE("an echo lost on the way is not a loader that has none", "[nrf_dfu]")
{
    begin(HASHED, HASHED);
    g_mcuboot.drop_echoes_after = 1U;   /* the knock's echo is answered, the next one lost */
    g_mcuboot.drop_echoes       = 1U;
    TEST_ASSERT_EQUAL(KODE_OK, fck_drv_nrf_dfu_recover(&s_io, s_new, s_new_len, &s_rep));
    TEST_ASSERT_EQUAL_UINT(3U, g_mcuboot.echoes);
    TEST_ASSERT_EQUAL(FCK_DRV_SMP_LOADER_HASH, s_rep.loader);
    assert_slot_holds(s_new, s_new_len);
}

TEST_CASE("a first module's loader, which refuses the echo, takes an image signed for it", "[nrf_dfu]")
{
    begin(SIGNED, SIGNED);
    g_mcuboot.echo = FAKE_MCUBOOT_ECHO_REFUSED;
    TEST_ASSERT_EQUAL(KODE_OK, fck_drv_nrf_dfu_recover(&s_io, s_new, s_new_len, &s_rep));
    TEST_ASSERT_TRUE(s_rep.loader_known);
    TEST_ASSERT_EQUAL(FCK_DRV_SMP_LOADER_SIGNED, s_rep.loader);
    TEST_ASSERT_EQUAL_UINT32(s_new_len, s_rep.written);
    assert_slot_holds(s_new, s_new_len);
}

/* ---- An image the loader would not boot is not written ------------------------------- */

TEST_CASE("a first module's loader refuses a hash-only image, with nothing written and MCUboot let go", "[nrf_dfu]")
{
    begin(SIGNED, HASHED);
    g_mcuboot.echo = FAKE_MCUBOOT_ECHO_REFUSED;
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_drv_nrf_dfu_recover(&s_io, s_new, s_new_len, &s_rep));
    TEST_ASSERT_EQUAL(FCK_DRV_NRF_DFU_STEP_IMAGE, s_rep.failed);
    TEST_ASSERT_EQUAL(FCK_DRV_SMP_IMAGE_LOADER, s_rep.image);
    TEST_ASSERT_EQUAL(FCK_DRV_SMP_LOADER_SIGNED, s_rep.loader);
    TEST_ASSERT_EQUAL_UINT32(0U, s_rep.written);
    TEST_ASSERT_EQUAL_UINT(0U, g_mcuboot.pieces);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, g_mcuboot.smp_resets, "MCUboot is let go to boot what it had");
    assert_slot_holds(s_old, s_old_len);
}

TEST_CASE("the one-slot loader refuses an image with a signature beside its hash", "[nrf_dfu]")
{
    begin(HASHED, SIGNED);
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_drv_nrf_dfu_recover(&s_io, s_new, s_new_len, &s_rep));
    TEST_ASSERT_EQUAL(FCK_DRV_NRF_DFU_STEP_IMAGE, s_rep.failed);
    TEST_ASSERT_EQUAL(FCK_DRV_SMP_IMAGE_LOADER, s_rep.image);
    TEST_ASSERT_EQUAL_UINT(0U, g_mcuboot.pieces);
    TEST_ASSERT_EQUAL_UINT(1U, g_mcuboot.smp_resets);
    assert_slot_holds(s_old, s_old_len);
}

TEST_CASE("an image that is not one, or has no hash, is refused before the nRF is touched", "[nrf_dfu]")
{
    begin(HASHED, NO_HASH);
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_drv_nrf_dfu_recover(&s_io, s_new, s_new_len, &s_rep));
    TEST_ASSERT_EQUAL(FCK_DRV_NRF_DFU_STEP_IMAGE, s_rep.failed);
    TEST_ASSERT_EQUAL(FCK_DRV_SMP_IMAGE_LOADER, s_rep.image);
    TEST_ASSERT_FALSE(s_rep.reset);

    s_new[0] ^= 0xFFU;
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_drv_nrf_dfu_recover(&s_io, s_new, s_new_len, &s_rep));
    TEST_ASSERT_EQUAL(FCK_DRV_SMP_IMAGE_NOT_MCUBOOT, s_rep.image);
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_drv_nrf_dfu_recover(&s_io, s_new, s_new_len, NULL));
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_drv_nrf_dfu_recover(NULL, s_new, s_new_len, &s_rep));

    TEST_ASSERT_EQUAL_UINT(0U, g_mcuboot.pin_resets);
    TEST_ASSERT_EQUAL_UINT(0U, g_mcuboot.frames);
    TEST_ASSERT_EQUAL_UINT(0U, g_idf.i2c_transfers);
}

TEST_CASE("with no expander for the nRF's reset nothing is tried, and an expander that fails is an I/O error", "[nrf_dfu]")
{
    begin(HASHED, HASHED);
    s_io.tca = NULL;
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_drv_nrf_dfu_recover(&s_io, s_new, s_new_len, &s_rep));
    TEST_ASSERT_EQUAL(FCK_DRV_NRF_DFU_STEP_RESET, s_rep.failed);
    TEST_ASSERT_FALSE(s_rep.reset);

    s_io.tca = &s_tca;
    g_idf.i2c_always_fail = 1U;
    TEST_ASSERT_EQUAL(KODE_IO_ERROR, fck_drv_nrf_dfu_recover(&s_io, s_new, s_new_len, &s_rep));
    TEST_ASSERT_EQUAL(FCK_DRV_NRF_DFU_STEP_RESET, s_rep.failed);
    TEST_ASSERT_FALSE(s_rep.reset);
    TEST_ASSERT_EQUAL_UINT(0U, g_mcuboot.frames);
}

/* ---- MCUboot that does not answer as it should --------------------------------------- */

TEST_CASE("no answer at all is a timeout at the server, after the reset", "[nrf_dfu]")
{
    begin(HASHED, HASHED);
    g_mcuboot.silent = true;
    TEST_ASSERT_EQUAL(KODE_TIMEOUT, fck_drv_nrf_dfu_recover(&s_io, s_new, s_new_len, &s_rep));
    TEST_ASSERT_EQUAL(FCK_DRV_NRF_DFU_STEP_SERVER, s_rep.failed);
    TEST_ASSERT_TRUE(s_rep.reset);
    TEST_ASSERT_FALSE(s_rep.loader_known);
    TEST_ASSERT_EQUAL_UINT32(0U, s_rep.written);
    TEST_ASSERT_EQUAL_UINT(0U, g_mcuboot.pieces);
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32_MESSAGE(4000U, s_rep.ms, "it knocked for MCUboot's whole window and more");
}

TEST_CASE("an answer only after MCUboot's window is the image's, and is an error at the server", "[nrf_dfu]")
{
    begin(HASHED, HASHED);
    g_mcuboot.ready_ms  = 3100U;
    g_mcuboot.window_ms = 20000U;   /* it is the image that answers, long after MCUboot let it go */
    TEST_ASSERT_EQUAL(KODE_ERROR, fck_drv_nrf_dfu_recover(&s_io, s_new, s_new_len, &s_rep));
    TEST_ASSERT_EQUAL(FCK_DRV_NRF_DFU_STEP_SERVER, s_rep.failed);
    TEST_ASSERT_TRUE(s_rep.reset);
    TEST_ASSERT_FALSE(s_rep.loader_known);
    TEST_ASSERT_EQUAL_UINT(0U, g_mcuboot.pieces);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, g_mcuboot.echoes, "only the knock the image answered");
    TEST_ASSERT_EQUAL_UINT(0U, g_mcuboot.lists);
}

TEST_CASE("a loader that answers neither the echo nor a refusal is not found, and nothing is written", "[nrf_dfu]")
{
    /* MCUboot's boot_serial answers every request, a refusal at the least:
     * one that answers no echo is not taken for MCUboot. */
    begin(HASHED, HASHED);
    g_mcuboot.echo = FAKE_MCUBOOT_ECHO_SILENT;
    TEST_ASSERT_EQUAL(KODE_TIMEOUT, fck_drv_nrf_dfu_recover(&s_io, s_new, s_new_len, &s_rep));
    TEST_ASSERT_EQUAL(FCK_DRV_NRF_DFU_STEP_SERVER, s_rep.failed);
    TEST_ASSERT_FALSE(s_rep.loader_known);
    TEST_ASSERT_GREATER_THAN_UINT(2U, g_mcuboot.echoes);
    TEST_ASSERT_EQUAL_UINT(0U, g_mcuboot.lists);
    TEST_ASSERT_EQUAL_UINT(0U, g_mcuboot.pieces);
    assert_slot_holds(s_old, s_old_len);
}

/* ---- An upload that goes wrong ------------------------------------------------------- */

TEST_CASE("an upload whose server stops progressing fails at the upload after UPLOAD_TRIES answers", "[nrf_dfu]")
{
    begin(HASHED, HASHED);
    g_mcuboot.stall_at = 256U;
    TEST_ASSERT_EQUAL(KODE_ERROR, fck_drv_nrf_dfu_recover(&s_io, s_new, s_new_len, &s_rep));
    TEST_ASSERT_EQUAL(FCK_DRV_NRF_DFU_STEP_UPLOAD, s_rep.failed);
    TEST_ASSERT_EQUAL_INT32(0, s_rep.rc);
    TEST_ASSERT_EQUAL_UINT32(256U, s_rep.written);
    TEST_ASSERT_FALSE(s_rep.answered);
    /* Two pieces taken, then the third asked again 5 times. */
    TEST_ASSERT_EQUAL_UINT(2U + 5U, g_mcuboot.pieces);
    TEST_ASSERT_FALSE(fake_mcuboot_slot_boots());
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, g_mcuboot.smp_resets,
                                   "something was written: the slot holds nothing to boot, so it stays in recovery");
}

TEST_CASE("an upload whose server goes quiet half way times out at the upload, and stays in recovery", "[nrf_dfu]")
{
    begin(HASHED, HASHED);
    g_mcuboot.mute_at = 256U;
    TEST_ASSERT_EQUAL(KODE_TIMEOUT, fck_drv_nrf_dfu_recover(&s_io, s_new, s_new_len, &s_rep));
    TEST_ASSERT_EQUAL(FCK_DRV_NRF_DFU_STEP_UPLOAD, s_rep.failed);
    TEST_ASSERT_EQUAL_UINT32(256U, s_rep.written);
    TEST_ASSERT_EQUAL_UINT(0U, g_mcuboot.smp_resets);
    TEST_ASSERT_TRUE(fake_mcuboot_in_recovery());
}

TEST_CASE("an image that is written and prints nothing fails at the boot", "[nrf_dfu]")
{
    begin(HASHED, HASHED);
    g_mcuboot.image_prints = false;
    TEST_ASSERT_EQUAL(KODE_ERROR, fck_drv_nrf_dfu_recover(&s_io, s_new, s_new_len, &s_rep));
    TEST_ASSERT_EQUAL(FCK_DRV_NRF_DFU_STEP_BOOT, s_rep.failed);
    TEST_ASSERT_EQUAL_UINT32(s_new_len, s_rep.written);
    TEST_ASSERT_FALSE(s_rep.answered);
    TEST_ASSERT_EQUAL_STRING("", s_rep.line);
    assert_slot_holds(s_new, s_new_len);
}

TEST_CASE("an upload refused on its first piece, with nothing written, lets MCUboot go to boot what it had", "[nrf_dfu]")
{
    begin(HASHED, HASHED);
    g_mcuboot.refuse_rc = 3;   /* the slot is left as it was */
    TEST_ASSERT_EQUAL(KODE_ERROR, fck_drv_nrf_dfu_recover(&s_io, s_new, s_new_len, &s_rep));
    TEST_ASSERT_EQUAL(FCK_DRV_NRF_DFU_STEP_UPLOAD, s_rep.failed);
    TEST_ASSERT_EQUAL_INT32(3, s_rep.rc);
    TEST_ASSERT_EQUAL_UINT32(0U, s_rep.written);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, g_mcuboot.smp_resets, "MCUboot was asked to reset: no recovery left behind");
    TEST_ASSERT_TRUE(fake_mcuboot_slot_boots());
    assert_slot_holds(s_old, s_old_len);
}

TEST_CASE("an image that was stored corrupted is found out before the reset, and MCUboot stays in recovery", "[nrf_dfu]")
{
    begin(HASHED, HASHED);
    g_mcuboot.corrupt_store = true;
    TEST_ASSERT_EQUAL(KODE_ERROR, fck_drv_nrf_dfu_recover(&s_io, s_new, s_new_len, &s_rep));
    TEST_ASSERT_EQUAL(FCK_DRV_NRF_DFU_STEP_UPLOAD, s_rep.failed);
    TEST_ASSERT_EQUAL_UINT32(s_new_len, s_rep.written);
    TEST_ASSERT_FALSE(s_rep.answered);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, g_mcuboot.smp_resets, "no reset: it would boot nothing");
    TEST_ASSERT_EQUAL_UINT(1U, g_mcuboot.pin_resets);
    TEST_ASSERT_TRUE(fake_mcuboot_in_recovery());
    TEST_ASSERT_FALSE(fake_mcuboot_slot_boots());
    TEST_ASSERT_LESS_THAN_UINT32_MESSAGE(FCK_DRV_NRF_DFU_LISTEN_MS, s_rep.ms, "it did not wait for a line");
}

TEST_CASE("a list that is too slow to answer after the upload does not fail the write", "[nrf_dfu]")
{
    begin(HASHED, HASHED);
    g_mcuboot.list_mute_when_complete = true;   /* MCUboot validates the whole image on each list */
    TEST_ASSERT_EQUAL(KODE_OK, fck_drv_nrf_dfu_recover(&s_io, s_new, s_new_len, &s_rep));
    TEST_ASSERT_EQUAL(FCK_DRV_NRF_DFU_STEP_NONE, s_rep.failed);
    TEST_ASSERT_EQUAL_UINT32(s_new_len, s_rep.written);
    TEST_ASSERT_TRUE(s_rep.answered);
    TEST_ASSERT_EQUAL_UINT(1U, g_mcuboot.smp_resets);
    assert_slot_holds(s_new, s_new_len);
}

/* ---- A list that is slow or fails ------------------------------------------------------ */

TEST_CASE("MCUboot is found by its echo although each list takes longer than a knock", "[nrf_dfu]")
{
    begin(HASHED, HASHED);
    g_mcuboot.list_ms = 700U;   /* a 1.3 MB image takes 650 ms to validate on each list */
    TEST_ASSERT_EQUAL(KODE_OK, fck_drv_nrf_dfu_recover(&s_io, s_new, s_new_len, &s_rep));
    TEST_ASSERT_EQUAL(FCK_DRV_NRF_DFU_STEP_NONE, s_rep.failed);
    TEST_ASSERT_EQUAL_STRING_MESSAGE("1.9.3", s_rep.before, "the list was waited for once, long enough");
    TEST_ASSERT_EQUAL_UINT32(s_new_len, s_rep.written);
    TEST_ASSERT_TRUE(s_rep.answered);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(2U, g_mcuboot.lists, "one list before the upload and one after it");
    assert_slot_holds(s_new, s_new_len);
}

TEST_CASE("an answer to the echo with no list is not taken for MCUboot: nothing is written, and it is let go", "[nrf_dfu]")
{
    begin(HASHED, HASHED);
    g_mcuboot.list_rc = 1;   /* MCUboot built for 256 sectors with a slot of 343 */
    TEST_ASSERT_EQUAL(KODE_ERROR, fck_drv_nrf_dfu_recover(&s_io, s_new, s_new_len, &s_rep));
    TEST_ASSERT_EQUAL(FCK_DRV_NRF_DFU_STEP_SERVER, s_rep.failed);
    TEST_ASSERT_EQUAL_INT32(1, s_rep.rc);
    TEST_ASSERT_FALSE(s_rep.loader_known);
    TEST_ASSERT_EQUAL_UINT(0U, g_mcuboot.pieces);
    TEST_ASSERT_EQUAL_UINT(1U, g_mcuboot.smp_resets);
    assert_slot_holds(s_old, s_old_len);
}

TEST_CASE("an image that kept running through a reset that did nothing is not written to", "[nrf_dfu]")
{
    begin(HASHED, HASHED);
    g_mcuboot.pin_dead     = true;   /* the nRF goes on running fck-nrf */
    g_mcuboot.image_smp_os = true;   /* which answers the echo, and refuses the list */
    TEST_ASSERT_EQUAL(KODE_ERROR, fck_drv_nrf_dfu_recover(&s_io, s_new, s_new_len, &s_rep));
    TEST_ASSERT_EQUAL(FCK_DRV_NRF_DFU_STEP_SERVER, s_rep.failed);
    TEST_ASSERT_EQUAL_INT32(8, s_rep.rc);
    TEST_ASSERT_FALSE(s_rep.loader_known);
    TEST_ASSERT_EQUAL_UINT(0U, g_mcuboot.pieces);
    TEST_ASSERT_GREATER_THAN_UINT(0U, g_mcuboot.image_frames);
    assert_slot_holds(s_old, s_old_len);
}

TEST_CASE("the probe of an image that answers the echo and not the list says it runs, and does not reset it", "[nrf_dfu]")
{
    begin(HASHED, HASHED);
    g_mcuboot.image_prints = false;   /* no line: the probe knocks a last time */
    g_mcuboot.image_smp_os = true;    /* fck-nrf: SMP's OS group only */
    TEST_ASSERT_EQUAL(KODE_OK, fck_drv_nrf_dfu_probe(&s_io, &s_probe));
    TEST_ASSERT_TRUE(s_probe.mcuboot);
    TEST_ASSERT_TRUE(s_probe.image);
    TEST_ASSERT_TRUE(s_probe.answered);
    TEST_ASSERT_TRUE(s_probe.running);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, g_mcuboot.smp_resets, "only the reset that let MCUboot boot it");
    TEST_ASSERT_FALSE(fake_mcuboot_in_recovery());
}

TEST_CASE("the probe finds MCUboot by its echo and reads a list that takes long", "[nrf_dfu]")
{
    begin(HASHED, HASHED);
    g_mcuboot.list_ms = 700U;
    TEST_ASSERT_EQUAL(KODE_OK, fck_drv_nrf_dfu_probe(&s_io, &s_probe));
    TEST_ASSERT_TRUE(s_probe.mcuboot);
    TEST_ASSERT_TRUE(s_probe.echo);
    TEST_ASSERT_FALSE(s_probe.late);
    TEST_ASSERT_LESS_OR_EQUAL_UINT32(2900U, s_probe.mcuboot_ms);
    TEST_ASSERT_TRUE(s_probe.image);
    TEST_ASSERT_EQUAL_STRING("1.9.3", s_probe.version);
    assert_slot_holds(s_old, s_old_len);
}

/* ---- The probe ----------------------------------------------------------------------- */

TEST_CASE("the probe reads the one-slot MCUboot, its image and what the image prints", "[nrf_dfu]")
{
    begin(HASHED, HASHED);
    TEST_ASSERT_EQUAL(KODE_OK, fck_drv_nrf_dfu_probe(&s_io, &s_probe));
    TEST_ASSERT_TRUE(s_probe.reset);
    TEST_ASSERT_TRUE(s_probe.mcuboot);
    TEST_ASSERT_TRUE(s_probe.echo);
    TEST_ASSERT_FALSE(s_probe.late);
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32(150U, s_probe.mcuboot_ms);
    TEST_ASSERT_LESS_OR_EQUAL_UINT32(2900U, s_probe.mcuboot_ms);
    TEST_ASSERT_TRUE(s_probe.image);
    TEST_ASSERT_FALSE(s_probe.running);
    TEST_ASSERT_EQUAL_STRING("1.9.3", s_probe.version);
    TEST_ASSERT_EQUAL_STRING("#NRFINFO fw=fck-nrf ver=2.0.1", s_probe.line);
    TEST_ASSERT_EQUAL_UINT(1U, g_mcuboot.smp_resets);
    TEST_ASSERT_EQUAL_UINT(0U, g_mcuboot.pieces);
    assert_slot_holds(s_old, s_old_len);
}

/* A 48-byte line cuts fck-nrf's `devid=` after 11 of its 16
 * digits. The line kept holds the whole of it, with reset= and uptime_ms=, and is cut after.
 * The devid here is made up. */
TEST_CASE("the probe keeps the image's first line up to its devid, reset and uptime, and cuts a longer one", "[nrf_dfu]")
{
    static const char LINE[] = "#NRFINFO fw=fck-nrf ver=0.1.0 devid=0123456789ABCDEF reset=0x00000001 uptime_ms=4294967295"
                               " map=0x0000E800 caps=spectrum,sniff,ping,echo";
    begin(HASHED, HASHED);
    g_mcuboot.image_line = LINE;
    TEST_ASSERT_EQUAL(KODE_OK, fck_drv_nrf_dfu_probe(&s_io, &s_probe));
    TEST_ASSERT_EQUAL_size_t(FCK_DRV_NRF_DFU_LINE_MAX, strlen(s_probe.line));
    TEST_ASSERT_EQUAL_STRING_LEN(LINE, s_probe.line, FCK_DRV_NRF_DFU_LINE_MAX);
    TEST_ASSERT_NOT_NULL(strstr(s_probe.line, "devid=0123456789ABCDEF reset=0x00000001 uptime_ms=4294967295"));
}

TEST_CASE("the probe says a first module's loader has no echo", "[nrf_dfu]")
{
    begin(SIGNED, SIGNED);
    g_mcuboot.echo = FAKE_MCUBOOT_ECHO_REFUSED;
    TEST_ASSERT_EQUAL(KODE_OK, fck_drv_nrf_dfu_probe(&s_io, &s_probe));
    TEST_ASSERT_TRUE(s_probe.mcuboot);
    TEST_ASSERT_FALSE(s_probe.echo);
    TEST_ASSERT_TRUE(s_probe.image);
}

TEST_CASE("the probe of an image that prints nothing sees no line and no answer", "[nrf_dfu]")
{
    begin(HASHED, HASHED);
    g_mcuboot.image_prints = false;
    TEST_ASSERT_EQUAL(KODE_OK, fck_drv_nrf_dfu_probe(&s_io, &s_probe));
    TEST_ASSERT_TRUE(s_probe.mcuboot);
    TEST_ASSERT_EQUAL_STRING("", s_probe.line);
    TEST_ASSERT_FALSE(s_probe.answered);
    TEST_ASSERT_FALSE(s_probe.running);
}

TEST_CASE("the probe of a nRF nothing answers is OK and finds nothing", "[nrf_dfu]")
{
    begin(HASHED, HASHED);
    g_mcuboot.silent = true;
    TEST_ASSERT_EQUAL(KODE_OK, fck_drv_nrf_dfu_probe(&s_io, &s_probe));
    TEST_ASSERT_TRUE(s_probe.reset);
    TEST_ASSERT_FALSE(s_probe.mcuboot);
    TEST_ASSERT_FALSE(s_probe.image);

    s_io.tca = NULL;
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_drv_nrf_dfu_probe(&s_io, &s_probe));
}
