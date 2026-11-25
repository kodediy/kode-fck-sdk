/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_5_rescue.c
 * @brief What a chip carries, through the facade: fck_core_rescue_probe().
 *
 * The probes are the model's (fake_writers.c): the nRF's DFU probe, and the C5's ROM
 * loader and what the C5 prints after it. They say what a case tells them to.
 * What these cases check is what the facade makes of it: who runs the job,
 * how what the probe saw becomes a state, when the image is asked what it
 * runs - and that one that did not print the firmware's banner or speak SMP is
 * never sent `info`, which another firmware could take for something else.
 */
#include <stdio.h>
#include <string.h>

#include "unity.h"

#include "fck.h"
#include "fck_core.h"

#include "fake_world.h"

/* MCUboot answered 150 ms after the reset, naming an image in slot 0. */
static void mcuboot_with_image(const char *version)
{
    sim_dfu.probe.mcuboot    = true;
    sim_dfu.probe.mcuboot_ms = 150U;
    sim_dfu.probe.image      = true;
    sim_dfu.probe.hash[0]    = 0xA1;
    sim_dfu.probe.hash[7]    = 0xA8;
    sim_dfu.probe.ms         = 6500U;
    (void)snprintf(sim_dfu.probe.version, sizeof(sim_dfu.probe.version), "%s", version);
}

TEST_CASE("the official firmware: MCUboot answers, the image prints its banner, and `info` says it is the official firmware", "[rescue]")
{
    sim_begin_fck(NULL, NULL);
    mcuboot_with_image("2.0.1");
    (void)snprintf(sim_dfu.probe.line, sizeof(sim_dfu.probe.line), "#NRFINFO fw=nrf_fck_agent ver=2.0");
    fck_rescue_report_t r;
    memset(&r, 0x5A, sizeof(r));
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_rescue_probe(FCK_NRF, &r));

    TEST_ASSERT_EQUAL(FCK_RESCUE_OFFICIAL, r.state);
    TEST_ASSERT_TRUE(r.bootloader);
    TEST_ASSERT_EQUAL_UINT32(150U, r.bootloader_ms);
    TEST_ASSERT_TRUE(r.image);
    TEST_ASSERT_EQUAL_STRING("2.0.1", r.version);
    TEST_ASSERT_EQUAL_HEX8(0xA1, r.hash[0]);
    TEST_ASSERT_EQUAL_HEX8(0xA8, r.hash[7]);
    TEST_ASSERT_EQUAL_STRING("#NRFINFO fw=nrf_fck_agent ver=2.0", r.line);
    TEST_ASSERT_TRUE(r.firmware.known && r.firmware.official);
    TEST_ASSERT_EQUAL_UINT8(2, r.firmware.ver_major);
    TEST_ASSERT_EQUAL_UINT32(6500U, r.ms);

    TEST_ASSERT_EQUAL_UINT32(1, sim_dfu.probes);
    TEST_ASSERT_TRUE(sim_dfu.had_tca);
    TEST_ASSERT_TRUE_MESSAGE(sim_dfu.in_owner, "the job runs in the owner's task");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, sim_dfu.recovers, "nothing is written");
}

TEST_CASE("the probe says which MCUboot the nRF carries: the one-slot one answers an echo", "[rescue]")
{
    sim_begin_fck(NULL, SIM_NRF_BANNER_010);
    mcuboot_with_image("0.1.0+0");
    sim_dfu.probe.echo = true;
    (void)snprintf(sim_dfu.probe.line, sizeof(sim_dfu.probe.line), "#NRFINFO fw=fck-nrf ver=0.1.0");
    fck_rescue_report_t r;
    memset(&r, 0x5A, sizeof(r));
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_rescue_probe(FCK_NRF, &r));
    TEST_ASSERT_EQUAL(FCK_RESCUE_OFFICIAL, r.state);
    TEST_ASSERT_TRUE(r.bootloader);
    TEST_ASSERT_TRUE_MESSAGE(r.one_slot, "it takes an image by its SHA-256");
    TEST_ASSERT_TRUE(r.firmware.has_version);
    TEST_ASSERT_EQUAL_UINT8(0, r.firmware.ver_major);

    /* TEMPORARY: pre-production modules. A first module's answers no echo. */
    sim_begin_fck(NULL, NULL);
    mcuboot_with_image("2.0.1");
    (void)snprintf(sim_dfu.probe.line, sizeof(sim_dfu.probe.line), "#NRFINFO fw=nrf_fck_agent ver=2.0");
    memset(&r, 0x5A, sizeof(r));
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_rescue_probe(FCK_NRF, &r));
    TEST_ASSERT_TRUE(r.bootloader);
    TEST_ASSERT_FALSE_MESSAGE(r.one_slot, "it wants a signed image");
    /* And no MCUboot at all is not the one-slot one. */
    sim_begin_fck(NULL, NULL);
    memset(&r, 0x5A, sizeof(r));
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_rescue_probe(FCK_NRF, &r));
    TEST_ASSERT_FALSE(r.bootloader);
    TEST_ASSERT_FALSE(r.one_slot);
}

TEST_CASE("another firmware: its line is the report's, and it is never sent `info`", "[rescue]")
{
    sim_begin_fck(NULL, NULL);
    sim_firmware(SIM_NRF, NULL);
    mcuboot_with_image("0.0.0");
    (void)snprintf(sim_dfu.probe.line, sizeof(sim_dfu.probe.line), "ready kode-nrf-test 1");
    sim_sent_clear(SIM_NRF);
    fck_rescue_report_t r;
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_rescue_probe(FCK_NRF, &r));
    TEST_ASSERT_EQUAL(FCK_RESCUE_FIRMWARE, r.state);
    TEST_ASSERT_EQUAL_STRING("ready kode-nrf-test 1", r.line);
    TEST_ASSERT_FALSE(r.firmware.known);
    sim_sleep(500U);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0, sim_sent(SIM_NRF, ""), "nothing was sent to it");

    /* A boot banner alone says an image started, as much as a line. */
    sim_dfu.probe.line[0] = '\0';
    sim_dfu.probe.banner  = true;
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_rescue_probe(FCK_NRF, &r));
    TEST_ASSERT_EQUAL(FCK_RESCUE_FIRMWARE, r.state);
    sim_sleep(500U);
    TEST_ASSERT_EQUAL_UINT(0, sim_sent(SIM_NRF, ""));
}

TEST_CASE("an image that answers SMP is asked what it runs, whatever it printed", "[rescue]")
{
    sim_begin_fck(NULL, NULL);
    sim_dfu.probe.late    = true;
    sim_dfu.probe.image   = true;
    sim_dfu.probe.running = true;
    fck_rescue_report_t r;
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_rescue_probe(FCK_NRF, &r));
    TEST_ASSERT_EQUAL(FCK_RESCUE_OFFICIAL, r.state);
    TEST_ASSERT_FALSE_MESSAGE(r.bootloader, "the window was missed");

    /* One that answers `info` with another name runs, and is not the official firmware. */
    sim_firmware(SIM_NRF, "#NRFINFO fw=my_own ver=0.1");
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_rescue_probe(FCK_NRF, &r));
    TEST_ASSERT_EQUAL(FCK_RESCUE_FIRMWARE, r.state);
    TEST_ASSERT_TRUE(r.firmware.known);
    TEST_ASSERT_FALSE(r.firmware.official);
}

TEST_CASE("an image MCUboot took that says nothing is QUIET; with only MCUboot answering, BOOTLOADER", "[rescue]")
{
    sim_begin_fck(NULL, NULL);
    sim_firmware(SIM_NRF, NULL);
    mcuboot_with_image("2.0.1");
    fck_rescue_report_t r;
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_rescue_probe(FCK_NRF, &r));
    TEST_ASSERT_EQUAL(FCK_RESCUE_QUIET, r.state);
    TEST_ASSERT_TRUE(r.image);
    TEST_ASSERT_EQUAL_STRING("", r.line);

    /* The last knock: MCUboot answered it, so nothing ran. */
    sim_dfu.probe.answered = true;
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_rescue_probe(FCK_NRF, &r));
    TEST_ASSERT_EQUAL(FCK_RESCUE_BOOTLOADER, r.state);
    TEST_ASSERT_TRUE_MESSAGE(r.image, "an image it holds and does not start");

    sim_dfu.probe.image      = false;
    sim_dfu.probe.version[0] = '\0';
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_rescue_probe(FCK_NRF, &r));
    TEST_ASSERT_EQUAL(FCK_RESCUE_BOOTLOADER, r.state);
    TEST_ASSERT_FALSE_MESSAGE(r.image, "slot 0 holds nothing it boots");

    /* An image that runs answered the last knock. */
    sim_dfu.probe.image   = true;
    sim_dfu.probe.running = true;
    sim_firmware(SIM_NRF, SIM_NRF_BANNER_10);
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_rescue_probe(FCK_NRF, &r));
    TEST_ASSERT_EQUAL(FCK_RESCUE_OFFICIAL, r.state);
    TEST_ASSERT_EQUAL_UINT8(1, r.firmware.ver_major);
}

TEST_CASE("nothing answering after the reset is NOTHING, and the nRF runs again after a probe", "[rescue]")
{
    sim_begin_fck(NULL, NULL);
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_power(FCK_NRF, false));
    sim_firmware(SIM_NRF, NULL);
    fck_rescue_report_t r;
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_rescue_probe(FCK_NRF, &r));
    TEST_ASSERT_EQUAL(FCK_RESCUE_NOTHING, r.state);
    TEST_ASSERT_FALSE(r.bootloader);
    TEST_ASSERT_FALSE(r.image);
    fck_status_t info;
    fck_core_status(&info);
    TEST_ASSERT_TRUE_MESSAGE(info.nrf_on, "the probe's reset let it go");
    TEST_ASSERT_TRUE(sim_chip_running(SIM_NRF));
}

TEST_CASE("another chip, no report, no module or the receiver on the bus: DENIED, nothing reset", "[rescue]")
{
    sim_begin_fck(NULL, NULL);
    fck_rescue_report_t r;
    memset(&r, 0x5A, sizeof(r));
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_core_rescue_probe((fck_chip_t)2, &r));
    TEST_ASSERT_EQUAL(FCK_RESCUE_NOTHING, r.state);
    TEST_ASSERT_FALSE(r.bootloader);
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_core_rescue_probe(FCK_NRF, NULL));
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_core_rescue_probe(FCK_C5, NULL));
    TEST_ASSERT_EQUAL_UINT32(0, sim_dfu.probes);
    TEST_ASSERT_EQUAL_UINT32(0, sim_rom.probes + sim_rom.listens);

    sim_script(SIM_C5, "gnss", "#DONE gnss rc=0\r\n", 5U);
    sim_gnss_standard_fw(true);
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_gnss_begin());
    sim_sleep(1200U);
    TEST_ASSERT_TRUE(sim_bus_dead());
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_core_rescue_probe(FCK_NRF, &r));
    TEST_ASSERT_EQUAL_UINT32(1, sim_dfu.probes);
    TEST_ASSERT_FALSE_MESSAGE(sim_dfu.had_tca, "no expander for the reset");
    /* The C5's reset is its own line: the receiver on the bus does not stop it. */
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_rescue_probe(FCK_C5, &r));
    TEST_ASSERT_EQUAL_UINT32(1, sim_rom.listens);

    sim_begin_empty();
    uint32_t const t0 = sim_now_ms();
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_core_rescue_probe(FCK_NRF, &r));
    TEST_ASSERT_TRUE(sim_now_ms() - t0 < 1000U);
    TEST_ASSERT_EQUAL(FCK_RESCUE_NOTHING, r.state);
    TEST_ASSERT_EQUAL_UINT32(0, sim_dfu.probes);
}

TEST_CASE("a UART that failed half way is the call's word, with what was seen by then", "[rescue]")
{
    sim_begin_fck(NULL, NULL);
    mcuboot_with_image("2.0.1");
    sim_dfu.probe_st = KODE_IO_ERROR;
    fck_rescue_report_t r;
    TEST_ASSERT_EQUAL(KODE_IO_ERROR, fck_core_rescue_probe(FCK_NRF, &r));
    TEST_ASSERT_TRUE(r.bootloader);
    TEST_ASSERT_EQUAL_STRING("2.0.1", r.version);
    TEST_ASSERT_EQUAL_MESSAGE(FCK_RESCUE_NOTHING, r.state, "no state from a probe that did not finish");
    TEST_ASSERT_FALSE(r.firmware.known);
}

/* ---- The C5 ------------------------------------------------------------------ */

/* What the C5 printed from its reset, as fck_drv_c5_boot_t keeps it. */
static void c5_heard(uint32_t restarts, uint32_t no_image, const char *line)
{
    fck_drv_c5_boot_init(&sim_rom.heard);
    sim_rom.heard.restarts = restarts;
    sim_rom.heard.no_image = no_image;
    sim_rom.heard.entry    = (no_image == 0U) && (restarts > 0U);
    (void)snprintf(sim_rom.heard.line, sizeof(sim_rom.heard.line), "%s", line);
}

TEST_CASE("the C5's firmware: its ROM answers, it prints #C5INFO, and `info` says it is the official firmware", "[rescue]")
{
    sim_begin_fck(NULL, NULL);
    c5_heard(1U, 0U, "#C5INFO fw=c5_fck_agent ver=2.0 chip=esp32c5");
    fck_rescue_report_t r;
    memset(&r, 0x5A, sizeof(r));
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_rescue_probe(FCK_C5, &r));

    TEST_ASSERT_EQUAL(FCK_RESCUE_OFFICIAL, r.state);
    TEST_ASSERT_TRUE(r.bootloader);
    TEST_ASSERT_FALSE(r.locked);
    TEST_ASSERT_EQUAL_HEX8(0xAA, r.mac[0]);
    TEST_ASSERT_EQUAL_HEX8(0xC5, r.mac[5]);
    TEST_ASSERT_EQUAL_UINT32(4U * 1024U * 1024U, r.flash_size);
    TEST_ASSERT_EQUAL_UINT32(1U, r.restarts);
    TEST_ASSERT_EQUAL_STRING("#C5INFO fw=c5_fck_agent ver=2.0 chip=esp32c5", r.line);
    TEST_ASSERT_TRUE(r.firmware.known && r.firmware.official);
    TEST_ASSERT_EQUAL_UINT8(2, r.firmware.ver_major);
    TEST_ASSERT_EQUAL_UINT32(1100U + 900U, r.ms);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0U, r.bootloader_ms, "the nRF's alone");
    TEST_ASSERT_FALSE(r.image);
    TEST_ASSERT_EQUAL_STRING("", r.version);

    TEST_ASSERT_EQUAL_UINT32(1, sim_rom.probes);
    TEST_ASSERT_EQUAL_UINT32(1, sim_rom.listens);
    TEST_ASSERT_EQUAL_UINT32(FCK_DRV_C5_ROM_LISTEN_MS, sim_rom.listen_limit_ms);
    TEST_ASSERT_TRUE_MESSAGE(sim_rom.in_owner, "the job runs in the owner's task");
    TEST_ASSERT_TRUE(sim_rom.ok_args);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, sim_rom.calls, "nothing is written");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, sim_dfu.probes, "the nRF is left alone");
    fck_status_t info;
    fck_core_status(&info);
    TEST_ASSERT_TRUE_MESSAGE(info.c5_on, "the C5 runs after its reset");
}

TEST_CASE("another firmware on the C5: its line is the report's, and it is never sent `info`", "[rescue]")
{
    sim_begin_fck(NULL, NULL);
    sim_firmware(SIM_C5, NULL);
    c5_heard(1U, 0U, "ready kode-c5-test-fck 3");
    (void)snprintf(sim_rom.heard.log_line, sizeof(sim_rom.heard.log_line), "%s",
                   "W (163) spi_flash: CPU frequency is set to 240M");
    sim_sent_clear(SIM_C5);
    fck_rescue_report_t r;
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_rescue_probe(FCK_C5, &r));
    TEST_ASSERT_EQUAL(FCK_RESCUE_FIRMWARE, r.state);
    TEST_ASSERT_EQUAL_STRING("ready kode-c5-test-fck 3", r.line);
    TEST_ASSERT_FALSE(r.firmware.known);
    sim_sleep(500U);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0, sim_sent(SIM_C5, ""), "nothing was sent to it");

    /* One that prints the banner with another name runs, and is not the official firmware. */
    sim_firmware(SIM_C5, SIM_C5_BANNER_OWN);
    c5_heard(1U, 0U, SIM_C5_BANNER_OWN);
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_rescue_probe(FCK_C5, &r));
    TEST_ASSERT_EQUAL(FCK_RESCUE_FIRMWARE, r.state);
    TEST_ASSERT_TRUE(r.firmware.known);
    TEST_ASSERT_FALSE(r.firmware.official);
}

TEST_CASE("a C5 that starts over and over, or finds no image, is BOOTLOADER; one that says nothing, QUIET", "[rescue]")
{
    sim_begin_fck(NULL, NULL);
    sim_firmware(SIM_C5, NULL);
    fck_rescue_report_t r;

    c5_heard(FCK_DRV_C5_BOOT_LOOP, 0U, "");
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_rescue_probe(FCK_C5, &r));
    TEST_ASSERT_EQUAL(FCK_RESCUE_BOOTLOADER, r.state);
    TEST_ASSERT_EQUAL_UINT32(FCK_DRV_C5_BOOT_LOOP, r.restarts);
    TEST_ASSERT_TRUE_MESSAGE(r.bootloader, "its ROM loader takes a write");

    c5_heard(1U, FCK_DRV_C5_BOOT_LOOP, "");
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_rescue_probe(FCK_C5, &r));
    TEST_ASSERT_EQUAL(FCK_RESCUE_BOOTLOADER, r.state);

    c5_heard(1U, 0U, "");
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_rescue_probe(FCK_C5, &r));
    TEST_ASSERT_EQUAL(FCK_RESCUE_QUIET, r.state);
    TEST_ASSERT_EQUAL_STRING("", r.line);

    /* One that only logs through ESP-IDF runs: its first log line says so. */
    c5_heard(1U, 0U, "");
    (void)snprintf(sim_rom.heard.log_line, sizeof(sim_rom.heard.log_line), "%s",
                   "I (130) app_init: Project name:     my_app");
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_rescue_probe(FCK_C5, &r));
    TEST_ASSERT_EQUAL(FCK_RESCUE_FIRMWARE, r.state);
    TEST_ASSERT_EQUAL_STRING("I (130) app_init: Project name:     my_app", r.line);

    /* ... and one that restarts after it logged, too: it said something. */
    c5_heard(FCK_DRV_C5_BOOT_LOOP, 0U, "");
    (void)snprintf(sim_rom.heard.log_line, sizeof(sim_rom.heard.log_line), "%s",
                   "E (210) my_app: no config");
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_rescue_probe(FCK_C5, &r));
    TEST_ASSERT_EQUAL(FCK_RESCUE_FIRMWARE, r.state);
    TEST_ASSERT_EQUAL_STRING("E (210) my_app: no config", r.line);
}

TEST_CASE("a C5 whose ROM loader never answers: NOTHING, or the firmware with no loader to write through", "[rescue]")
{
    sim_begin_fck(NULL, NULL);
    sim_rom.probe_st = KODE_TIMEOUT;
    memset(sim_rom.probe.mac, 0, sizeof(sim_rom.probe.mac));
    sim_rom.probe.connected  = false;
    sim_rom.probe.flash_size = 0U;
    sim_firmware(SIM_C5, NULL);
    c5_heard(0U, 0U, "");
    fck_rescue_report_t r;
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_rescue_probe(FCK_C5, &r));
    TEST_ASSERT_EQUAL(FCK_RESCUE_NOTHING, r.state);
    TEST_ASSERT_FALSE(r.bootloader);
    TEST_ASSERT_EQUAL_HEX8(0, r.mac[0]);

    /* Download mode off: the loader is gone, and the firmware still boots. */
    sim_firmware(SIM_C5, SIM_C5_BANNER_20);
    c5_heard(1U, 0U, "#C5INFO fw=c5_fck_agent ver=2.0");
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_rescue_probe(FCK_C5, &r));
    TEST_ASSERT_EQUAL(FCK_RESCUE_OFFICIAL, r.state);
    TEST_ASSERT_FALSE(r.bootloader);
}

TEST_CASE("a locked C5 says so; a chip that answers as another is DENIED and not listened to", "[rescue]")
{
    sim_begin_fck(NULL, NULL);
    sim_rom.probe_st          = KODE_DENIED;
    sim_rom.probe.secure_boot = true;
    c5_heard(1U, 0U, "#C5INFO fw=c5_fck_agent ver=2.0");
    fck_rescue_report_t r;
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_rescue_probe(FCK_C5, &r));
    TEST_ASSERT_TRUE(r.locked);
    TEST_ASSERT_TRUE(r.bootloader);
    TEST_ASSERT_EQUAL(FCK_RESCUE_OFFICIAL, r.state);

    sim_rom.probe.secure_boot      = false;
    sim_rom.probe.flash_encryption = true;
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_rescue_probe(FCK_C5, &r));
    TEST_ASSERT_TRUE(r.locked);
    TEST_ASSERT_EQUAL_UINT32(2, sim_rom.listens);

    sim_rom.probe.flash_encryption = false;
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_core_rescue_probe(FCK_C5, &r));
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(2, sim_rom.listens, "not listened to");
    TEST_ASSERT_EQUAL(FCK_RESCUE_NOTHING, r.state);
}

TEST_CASE("a C5 UART that failed is the call's word, with what was seen by then", "[rescue]")
{
    sim_begin_fck(NULL, NULL);
    sim_rom.probe_st = KODE_IO_ERROR;
    c5_heard(1U, 0U, "#C5INFO fw=c5_fck_agent ver=2.0");
    fck_rescue_report_t r;
    TEST_ASSERT_EQUAL(KODE_IO_ERROR, fck_core_rescue_probe(FCK_C5, &r));
    TEST_ASSERT_TRUE(r.bootloader);
    TEST_ASSERT_EQUAL_STRING("#C5INFO fw=c5_fck_agent ver=2.0", r.line);
    TEST_ASSERT_EQUAL_MESSAGE(FCK_RESCUE_NOTHING, r.state, "no state from a probe that did not finish");
    TEST_ASSERT_FALSE(r.firmware.known);
}
