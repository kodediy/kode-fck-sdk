/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_nrf.c
 * @brief fck_drv_nrf: the nRF firmware's commands built, and its lines read.
 *
 * The lines are the firmware's own, written with the format strings of its
 * source (rf.c, main.c), so a case that passes is a line
 * the SDK will read from a real module. Most cases are about what must never
 * happen: a command that transmits outside the module's band and power,
 * a command the shell would take without a #DONE, a
 * number read in the wrong base.
 */
#include <stdio.h>
#include <string.h>

#include "unity.h"

#include "fck_drv_link.h"
#include "fck_drv_nrf.h"

static fck_drv_link_message_t msg(const char *tag, const char *body)
{
    fck_drv_link_message_t m = { .tag = tag, .body = body };
    return m;
}

static void built(kode_status_t st, const fck_drv_nrf_command_t *c, const char *line,
                  const char *verb, bool emits)
{
    TEST_ASSERT_EQUAL(KODE_OK, st);
    TEST_ASSERT_EQUAL_STRING(line, c->line);
    TEST_ASSERT_EQUAL_size_t(strlen(line), c->len);
    TEST_ASSERT_EQUAL_STRING(verb, c->verb);
    TEST_ASSERT_EQUAL(emits, c->emits);
}

/* ---- Commands --------------------------------------------------------------- */

TEST_CASE("each builder writes the firmware's line, in its channels, with its #DONE verb", "[nrf]")
{
    fck_drv_nrf_command_t c;
    built(fck_drv_nrf_command_info(&c), &c, "info\n", "info", false);
    built(fck_drv_nrf_command_spectrum(&c, 2400, 2483, 200, 1), &c, "rf spec 0 83 200 1\n", "rf_spec", false);
    built(fck_drv_nrf_command_spectrum(&c, 2400, 2500, 10, 1000), &c, "rf spec 0 100 10 1000\n", "rf_spec",
          false);
    built(fck_drv_nrf_command_off(&c), &c, "rf off\n", "rf_off", false);
    built(fck_drv_nrf_command_power(&c, -20), &c, "rf power -20\n", "rf_power", false);
    built(fck_drv_nrf_command_echo(&c, true, 2402), &c, "rf echo on 2\n", "rf_echo", true);
    built(fck_drv_nrf_command_echo(&c, false, 0), &c, "rf echo off\n", "rf_echo", false);
    built(fck_drv_nrf_command_echo_statistics(&c), &c, "rf echo\n", "rf_echo", false);
    built(fck_drv_nrf_command_ping(&c, 20, 2480, 0), &c, "rf ping 20 80 0\n", "rf_ping", true);
    built(fck_drv_nrf_command_sniff(&c, 2476, 20000, -60), &c, "rf sniff 76 20000 -60\n", "rf_sniff",
          false);

    const fck_drv_nrf_sniff_config_t phy = { .rate_mbps = 2, .crc_bytes = 2, .len = 40 };
    built(fck_drv_nrf_command_sniff_config(&c, &phy), &c, "rf scfg 2 2 40\n", "rf_scfg", false);
    const fck_drv_nrf_sniff_address_t a = { .balen = 4, .base = 0xE7E7E7E7U, .prefix = 0x0A };
    built(fck_drv_nrf_command_sniff_address(&c, &a), &c, "rf saddr 4 E7E7E7E7 0A\n", "rf_saddr", false);
}

TEST_CASE("nothing transmits outside 2402-2480 MHz or above 6 dBm, whatever the firmware takes",
          "[nrf]")
{
    fck_drv_nrf_command_t c;
    memset(&c, 0, sizeof(c));

    /* The firmware before 2.0 takes channel 0 and 1 for ping and echo, and
     * +7 and +8 dBm. */
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_drv_nrf_command_ping(&c, 1, 2400, 10));
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_drv_nrf_command_ping(&c, 1, 2401, 10));
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_drv_nrf_command_ping(&c, 1, 2481, 10));
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_drv_nrf_command_echo(&c, true, 2401));
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_drv_nrf_command_power(&c, 7));
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_drv_nrf_command_power(&c, -47));

    /* Written by hand. */
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_drv_nrf_command(&c, "rf ping 5 1"));
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_drv_nrf_command(&c, "rf ping 5 0x01"));
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_drv_nrf_command(&c, "rf echo on 0"));
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_drv_nrf_command(&c, "rf power 8"));
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_drv_nrf_command(&c, "rf power 0x7"));
    /* The firmware reads strtol, base 0, any length: each of these is a number
     * to it, +8 dBm or channel 100, that a looser reading here let by. */
    TEST_ASSERT_EQUAL_MESSAGE(KODE_DENIED,
                              fck_drv_nrf_command(&c, "rf power 0000000000000000000000010"),
                              "+8 dBm in octal, longer than a buffer, was built");
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_drv_nrf_command(&c, "rf power 010"));
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_drv_nrf_command(&c, "rf power +8"));
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_drv_nrf_command(&c, "rf power -0x14"));
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_drv_nrf_command(&c, "rf ping 5 0000000000000000000000144"));
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_drv_nrf_command(&c, "rf echo on 0x0000000000000000000000064"));
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_drv_nrf_command(&c, "rf ping 05 40"));
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_drv_nrf_command(&c, "rf ping 5 40 0x10"));
    TEST_ASSERT_EQUAL_MESSAGE(KODE_DENIED, fck_drv_nrf_command(&c, "rf cw 40"),
        "an unmodulated carrier, never declared, was built");
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_drv_nrf_command(&c, "rf cw"));
    TEST_ASSERT_EQUAL_size_t(0, c.len);

    TEST_ASSERT_EQUAL(KODE_DENIED, fck_drv_nrf_command(&c, "rf ping 5 0x02"));
    TEST_ASSERT_EQUAL_size_t(0, c.len);
    built(fck_drv_nrf_command(&c, "rf ping 5 2"), &c, "rf ping 5 2\n", "rf_ping", true);
    built(fck_drv_nrf_command(&c, "rf ping 5"), &c, "rf ping 5\n", "rf_ping", true);
    built(fck_drv_nrf_command(&c, "rf power -20"), &c, "rf power -20\n", "rf_power", false);
    built(fck_drv_nrf_command(&c, "rf power 0"), &c, "rf power 0\n", "rf_power", false);
    built(fck_drv_nrf_command(&c, "rf power"), &c, "rf power\n", "rf_power", false);
    built(fck_drv_nrf_command(&c, "rf echo on"), &c, "rf echo on\n", "rf_echo", true);
    built(fck_drv_nrf_command(&c, "rf power 6"), &c, "rf power 6\n", "rf_power", false);
    /* Listening goes everywhere the radio tunes. */
    built(fck_drv_nrf_command(&c, "rf spec 0 100"), &c, "rf spec 0 100\n", "rf_spec", false);
    built(fck_drv_nrf_command(&c, "rf sniff 0"), &c, "rf sniff 0\n", "rf_sniff", false);
}

TEST_CASE("only the firmware's commands are built, and only as the shell will take them", "[nrf]")
{
    fck_drv_nrf_command_t c;
    /* Zephyr's own commands end with no #DONE. */
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_drv_nrf_command(&c, "help"));
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_drv_nrf_command(&c, "kernel reboot cold"));
    /* The shell would split these otherwise than here. */
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_drv_nrf_command(&c, "rf spec \"0\""));
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_drv_nrf_command(&c, "rf spec 0\\ 1"));
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_drv_nrf_command(&c, "rf spec 'x'"));
    /* Two commands in one. */
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_drv_nrf_command(&c, "rf off\nrf ping 5 0"));
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_drv_nrf_command(&c, ""));
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_drv_nrf_command(&c, "   "));
    /* Nine arguments after a sub-command: "wrong parameter count", no #DONE. */
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_drv_nrf_command(&c, "rf spec 1 2 3 4 5 6 7 8 9"));
    built(fck_drv_nrf_command(&c, "rf spec 1 2 3 4 5 6 7 8"), &c, "rf spec 1 2 3 4 5 6 7 8\n",
          "rf_spec", false);

    char big[FCK_DRV_NRF_COMMAND_MAX + 2];
    memset(big, ' ', sizeof(big));
    memcpy(big, "rf off", 6);
    big[sizeof(big) - 1] = '\0';
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_drv_nrf_command(&c, big));
    big[sizeof(big) - 2] = '\0';
    TEST_ASSERT_EQUAL(KODE_OK, fck_drv_nrf_command(&c, big));

    /* `rf` alone ends with its usage: #DONE rf. A sub-command not known
     * here may end with a #DONE nothing waits for (an old firmware had one),
     * or none: refused. */
    built(fck_drv_nrf_command(&c, "rf"), &c, "rf\n", "rf", false);
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_drv_nrf_command(&c, "rf adv 40"));
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_drv_nrf_command(&c, "rf pulses 433"));
    /* Zephyr's shell takes -h and --help anywhere: help, nothing run, no
     * #DONE. */
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_drv_nrf_command(&c, "rf spec -h"));
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_drv_nrf_command(&c, "rf -h"));
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_drv_nrf_command(&c, "rf sniff 76 --help"));
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_drv_nrf_command(&c, "info -h"));
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_drv_nrf_command(&c, "rf sniff 76 1000 -"));
    built(fck_drv_nrf_command(&c, "rf sniff 76 1000 -60"), &c, "rf sniff 76 1000 -60\n", "rf_sniff",
          false);
    built(fck_drv_nrf_command(&c, "  rf   off  "), &c, "  rf   off  \n", "rf_off", false);
}

TEST_CASE("the builders refuse what the firmware would refuse", "[nrf]")
{
    fck_drv_nrf_command_t c;
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_drv_nrf_command_spectrum(&c, 2399, 2483, 200, 1));
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_drv_nrf_command_spectrum(&c, 2400, 2501, 200, 1));
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_drv_nrf_command_spectrum(&c, 2450, 2440, 200, 1));
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_drv_nrf_command_spectrum(&c, 2400, 2483, 9, 1));
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_drv_nrf_command_spectrum(&c, 2400, 2483, 100001, 1));
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_drv_nrf_command_spectrum(&c, 2400, 2483, 200, 0));
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_drv_nrf_command_spectrum(&c, 2400, 2483, 200, 1001));
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_drv_nrf_command_ping(&c, 0, 2440, 10));
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_drv_nrf_command_ping(&c, 1001, 2440, 10));
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_drv_nrf_command_ping(&c, 1, 2440, 1001));
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_drv_nrf_command_sniff(&c, 2501, 1000, -60));
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_drv_nrf_command_sniff(&c, 2440, 99, -60));
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_drv_nrf_command_sniff(&c, 2440, 600001, -60));
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_drv_nrf_command_sniff(&c, 2440, 1000, 1));
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_drv_nrf_command_sniff(&c, 2440, 1000, -128));

    fck_drv_nrf_sniff_config_t phy = { .rate_mbps = 1, .crc_bytes = 1, .len = 32 };
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_drv_nrf_command_sniff_config(&c, &phy));
    phy.crc_bytes = 0;
    phy.len       = 41;
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_drv_nrf_command_sniff_config(&c, &phy));
    phy.len       = 32;
    phy.rate_mbps = 3;
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_drv_nrf_command_sniff_config(&c, &phy));
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_drv_nrf_command_sniff_config(&c, NULL));
    fck_drv_nrf_sniff_address_t a = { .balen = 5 };
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_drv_nrf_command_sniff_address(&c, &a));
    a.balen = 1;
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_drv_nrf_command_sniff_address(&c, &a));
}

TEST_CASE("a command is given the time its own arguments take", "[nrf]")
{
    fck_drv_nrf_command_t c;
    TEST_ASSERT_EQUAL(KODE_OK, fck_drv_nrf_command_info(&c));
    TEST_ASSERT_EQUAL_UINT32(FCK_DRV_NRF_COMMAND_MS, c.ms);

    /* 84 MHz at 200 us is 17 ms of listening and a 450-byte line. */
    TEST_ASSERT_EQUAL(KODE_OK, fck_drv_nrf_command_spectrum(&c, 2400, 2483, 200, 1));
    uint32_t const one = c.ms;
    TEST_ASSERT_TRUE(one > FCK_DRV_NRF_SLACK_MS + 17U && one < FCK_DRV_NRF_COMMAND_MS);
    TEST_ASSERT_EQUAL(KODE_OK, fck_drv_nrf_command_spectrum(&c, 2400, 2483, 200, 100));
    TEST_ASSERT_TRUE(c.ms > 100U * (one - FCK_DRV_NRF_SLACK_MS));
    /* The longest the firmware takes: 1000 sweeps of 101 MHz at 100 ms. */
    TEST_ASSERT_EQUAL(KODE_OK, fck_drv_nrf_command_spectrum(&c, 2400, 2500, 100000, 1000));
    TEST_ASSERT_TRUE(c.ms > 1000U * 101U * 100U);

    /* Each ping waits 20 ms for its echo, plus its gap. */
    TEST_ASSERT_EQUAL(KODE_OK, fck_drv_nrf_command_ping(&c, 1000, 2440, 1000));
    TEST_ASSERT_TRUE(c.ms > 1000U * (20U + 1000U));
    TEST_ASSERT_EQUAL(KODE_OK, fck_drv_nrf_command(&c, "rf ping 10"));
    TEST_ASSERT_TRUE(c.ms > 10U * (20U + 10U) && c.ms < FCK_DRV_NRF_COMMAND_MS);

    TEST_ASSERT_EQUAL(KODE_OK, fck_drv_nrf_command_sniff(&c, 2440, 600000, -60));
    TEST_ASSERT_TRUE(c.ms > 600000U + FCK_DRV_NRF_SLACK_MS);
    TEST_ASSERT_EQUAL(KODE_OK, fck_drv_nrf_command(&c, "rf sniff 40"));
    TEST_ASSERT_TRUE(c.ms > 20000U);

    /* An argument the firmware refuses is answered at once, and so is a ping with no count. */
    TEST_ASSERT_EQUAL(KODE_OK, fck_drv_nrf_command(&c, "rf ping"));
    TEST_ASSERT_EQUAL_UINT32(FCK_DRV_NRF_COMMAND_MS, c.ms);
    TEST_ASSERT_EQUAL(KODE_OK, fck_drv_nrf_command(&c, "rf sniff 40 99"));
    TEST_ASSERT_EQUAL_UINT32(FCK_DRV_NRF_COMMAND_MS, c.ms);
    TEST_ASSERT_EQUAL(KODE_OK, fck_drv_nrf_command(&c, "rf spec 50 40"));
    TEST_ASSERT_EQUAL_UINT32(FCK_DRV_NRF_COMMAND_MS, c.ms);
}

TEST_CASE("a firmware's rc becomes the caller's status", "[nrf]")
{
    TEST_ASSERT_EQUAL(KODE_OK, fck_drv_nrf_status(0));
    TEST_ASSERT_EQUAL(KODE_IO_ERROR, fck_drv_nrf_status(1));
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_drv_nrf_status(2));
    TEST_ASSERT_EQUAL(KODE_IO_ERROR, fck_drv_nrf_status(-1));
    TEST_ASSERT_EQUAL(KODE_IO_ERROR, fck_drv_nrf_status(3));
}

/* ---- Lines ------------------------------------------------------------------ */

/* fck-nrf 0.1.0's line, with the format of firmware/fck-nrf (INFO_FORMAT,
 * FCK_NRF_CAPS); the DEVICEID is made up. */
#define NRF_010_HEAD "fw=fck-nrf ver=0.1.0 devid=0123456789ABCDEF reset=0x00000001 uptime_ms=3100 map=0x0000E800"
#define NRF_010_CAPS "spec,off,power,echo,ping,sniff,scfg,saddr,eu,done,wdt,echoidle,smp"
#define NRF_010      NRF_010_HEAD " caps=" NRF_010_CAPS

static const fck_drv_nrf_feature_t k_every_feature[] = {
    FCK_DRV_NRF_SPECTRUM, FCK_DRV_NRF_PING, FCK_DRV_NRF_ECHO, FCK_DRV_NRF_SNIFF,
};
#define EVERY_FEATURE (sizeof(k_every_feature) / sizeof(k_every_feature[0]))

TEST_CASE("0.1.0's #NRFINFO says what it does in caps=, and `eu` is its word for the limits", "[nrf]")
{
    fck_drv_nrf_info_t i;
    fck_drv_link_message_t m = msg("NRFINFO", NRF_010);
    TEST_ASSERT_TRUE(fck_drv_nrf_parse_info(&m, &i));
    TEST_ASSERT_EQUAL_STRING("fck-nrf", i.fw);
    TEST_ASSERT_EQUAL_STRING("0.1.0", i.ver);
    TEST_ASSERT_TRUE(i.has_version);
    TEST_ASSERT_EQUAL_UINT8(0, i.ver_major);
    TEST_ASSERT_EQUAL_UINT8(1, i.ver_minor);
    TEST_ASSERT_EQUAL_UINT8(0, i.ver_patch);
    TEST_ASSERT_TRUE(i.devid == 0x0123456789ABCDEFULL);
    TEST_ASSERT_EQUAL_HEX32(0x1, i.reset);
    TEST_ASSERT_EQUAL_UINT32(3100, i.uptime_ms);
    TEST_ASSERT_TRUE(i.has_caps);
    TEST_ASSERT_EQUAL_HEX16_MESSAGE(0x1FFF, i.caps, "every one of its thirteen words");
    for (size_t k = 0; k < EVERY_FEATURE; k++) {
        TEST_ASSERT_TRUE_MESSAGE(fck_drv_nrf_has(&i, k_every_feature[k]), "a 0.1.0 firmware was refused a feature");
    }

    /* What transmits reaches it, inside the limits this file keeps a second time. */
    fck_drv_nrf_command_t c;
    TEST_ASSERT_EQUAL(FCK_DRV_NRF_GATE_OFFICIAL, fck_drv_nrf_gate(&i, "rf ping 10 40", NULL, &c));
    TEST_ASSERT_TRUE(c.emits);
    TEST_ASSERT_EQUAL(FCK_DRV_NRF_GATE_OFFICIAL, fck_drv_nrf_gate(&i, "rf echo on 40", NULL, &c));
    TEST_ASSERT_EQUAL(FCK_DRV_NRF_GATE_REFUSE, fck_drv_nrf_gate(&i, "rf ping 10 81", NULL, &c));
    TEST_ASSERT_EQUAL(FCK_DRV_NRF_GATE_REFUSE, fck_drv_nrf_gate(&i, "rf power 7", NULL, &c));
    TEST_ASSERT_EQUAL(FCK_DRV_NRF_GATE_REFUSE, fck_drv_nrf_gate(&i, "rf cw 40", NULL, &c));

    /* The version decides nothing: the same words under any number, or none. */
    static const char *const vers[] = { "ver=0.0.0 ", "ver=9.9 ", "ver=1.0 ", "ver=beta ", "" };
    for (size_t v = 0; v < sizeof(vers) / sizeof(vers[0]); v++) {
        char body[256];
        snprintf(body, sizeof(body), "fw=fck-nrf %scaps=" NRF_010_CAPS, vers[v]);
        m = msg("NRFINFO", body);
        TEST_ASSERT_TRUE(fck_drv_nrf_parse_info(&m, &i));
        for (size_t k = 0; k < EVERY_FEATURE; k++) {
            TEST_ASSERT_TRUE_MESSAGE(fck_drv_nrf_has(&i, k_every_feature[k]), body);
        }
    }
    m = msg("NRFINFO", "fw=fck-nrf ver=0.0");
    TEST_ASSERT_TRUE(fck_drv_nrf_parse_info(&m, &i));
    TEST_ASSERT_TRUE_MESSAGE(i.has_version, "0.0 is a version");
    m = msg("NRFINFO", "fw=fck-nrf");
    TEST_ASSERT_TRUE(fck_drv_nrf_parse_info(&m, &i));
    TEST_ASSERT_FALSE(i.has_version);
    m = msg("NRFINFO", "fw=fck-nrf ver=0.1.0+0");
    TEST_ASSERT_TRUE(fck_drv_nrf_parse_info(&m, &i));
    TEST_ASSERT_FALSE_MESSAGE(i.has_version, "the +tweak form is not the one read");
}

TEST_CASE("an nRF firmware whose caps= lacks a word is not asked that command", "[nrf]")
{
    static const struct {
        const char           *caps;
        fck_drv_nrf_feature_t off[3];
        size_t                n_off;
    } rows[] = {
        /* The word `eu` is no longer looked at: the limits are the image's own. */
        { "spec,off,power,echo,ping,sniff,scfg,saddr,done,wdt,echoidle,smp", { FCK_DRV_NRF_SPECTRUM }, 0 },
        { "off,power,echo,ping,sniff,scfg,saddr,eu,done,wdt,echoidle,smp", { FCK_DRV_NRF_SPECTRUM }, 1 },
        { "spec,off,power,echo,sniff,scfg,saddr,eu,done,wdt,echoidle,smp", { FCK_DRV_NRF_PING }, 1 },
        /* `echoidle` is not `echo`. */
        { "spec,off,power,ping,sniff,scfg,saddr,eu,done,wdt,echoidle,smp", { FCK_DRV_NRF_ECHO }, 1 },
        { "spec,off,power,echo,ping,scfg,saddr,eu,done,wdt,echoidle,smp", { FCK_DRV_NRF_SNIFF }, 1 },
        { "spec,off,power,echo,ping,sniff,saddr,eu,done,wdt,echoidle,smp", { FCK_DRV_NRF_SNIFF }, 1 },
        /* What transmits sets its level first: without `power`, neither. */
        { "spec,off,echo,ping,sniff,scfg,saddr,eu,done,wdt,echoidle,smp",
          { FCK_DRV_NRF_PING, FCK_DRV_NRF_ECHO }, 2 },
        /* `europe` is not `eu`, and a word this SDK does not know changes nothing. */
        { "spec,off,power,echo,ping,sniff,scfg,saddr,europe,done,wdt,echoidle,smp,future",
          { FCK_DRV_NRF_SPECTRUM }, 0 },
        { "", { FCK_DRV_NRF_SPECTRUM, FCK_DRV_NRF_SNIFF }, 2 },
    };
    fck_drv_nrf_command_t c;
    for (size_t r = 0; r < sizeof(rows) / sizeof(rows[0]); r++) {
        char body[256];
        snprintf(body, sizeof(body), NRF_010_HEAD " caps=%s", rows[r].caps);
        fck_drv_link_message_t const m = msg("NRFINFO", body);
        fck_drv_nrf_info_t           i;
        TEST_ASSERT_TRUE(fck_drv_nrf_parse_info(&m, &i));
        TEST_ASSERT_TRUE(i.has_caps);
        for (size_t k = 0; k < EVERY_FEATURE; k++) {
            bool off = (rows[r].caps[0] == '\0');
            for (size_t o = 0; o < rows[r].n_off; o++) {
                off = off || (rows[r].off[o] == k_every_feature[k]);
            }
            TEST_ASSERT_EQUAL_MESSAGE(!off, fck_drv_nrf_has(&i, k_every_feature[k]), rows[r].caps);
        }
        /* An image that prints a caps= keeps its limits itself: what transmits goes out by hand too. */
        bool const eu = !fck_drv_nrf_is_unlimited(&i);
        TEST_ASSERT_EQUAL_MESSAGE(eu ? FCK_DRV_NRF_GATE_OFFICIAL : FCK_DRV_NRF_GATE_REFUSE,
                                  fck_drv_nrf_gate(&i, "rf ping 10 40", NULL, &c), rows[r].caps);
        TEST_ASSERT_EQUAL_MESSAGE(eu ? FCK_DRV_NRF_GATE_OFFICIAL : FCK_DRV_NRF_GATE_REFUSE,
                                  fck_drv_nrf_gate(&i, "rf echo on 40", NULL, &c), rows[r].caps);
        TEST_ASSERT_EQUAL(FCK_DRV_NRF_GATE_OFFICIAL, fck_drv_nrf_gate(&i, "rf spec 0 83", NULL, &c));
        TEST_ASSERT_EQUAL(FCK_DRV_NRF_GATE_OFFICIAL, fck_drv_nrf_gate(&i, "rf echo off", NULL, &c));
    }
}

/* TEMPORARY: pre-production firmware (2.x). Remove when the production modules ship.
 * The three cases below: the firmware of the first modules, which prints no
 * caps= and is asked by its version. */

TEST_CASE("without caps= a 0.x keeps no limits of its own: only 2.x said so by its number", "[nrf]")
{
    fck_drv_nrf_info_t i;
    fck_drv_link_message_t m = msg("NRFINFO", NRF_010_HEAD);
    TEST_ASSERT_TRUE(fck_drv_nrf_parse_info(&m, &i));
    TEST_ASSERT_FALSE(i.has_caps);
    TEST_ASSERT_FALSE(!fck_drv_nrf_is_unlimited(&i));
    m = msg("NRFINFO", "fw=nrf_fck_agent");
    TEST_ASSERT_TRUE(fck_drv_nrf_parse_info(&m, &i));
    TEST_ASSERT_FALSE(!fck_drv_nrf_is_unlimited(&i));
    /* The four radio commands were in every image of that line. */
    TEST_ASSERT_TRUE(fck_drv_nrf_has(&i, FCK_DRV_NRF_SPECTRUM));
    TEST_ASSERT_TRUE(fck_drv_nrf_has(&i, FCK_DRV_NRF_PING));
    TEST_ASSERT_TRUE(fck_drv_nrf_has(&i, FCK_DRV_NRF_ECHO));
    TEST_ASSERT_TRUE(fck_drv_nrf_has(&i, FCK_DRV_NRF_SNIFF));
    m = msg("NRFINFO", "fw=my_own ver=2.0");
    TEST_ASSERT_TRUE(fck_drv_nrf_parse_info(&m, &i));
    TEST_ASSERT_FALSE_MESSAGE(fck_drv_nrf_has(&i, FCK_DRV_NRF_SPECTRUM), "another image is asked nothing");
}

TEST_CASE("#NRFINFO says what runs, and only the official firmware 2.0 keeps the EU's limits", "[nrf]")
{
    fck_drv_nrf_info_t i;
    fck_drv_link_message_t m = msg("NRFINFO", "fw=nrf_fck_agent ver=1.0 devid=0123456789ABCD15 "
                                 "reset=0x00000004 uptime_ms=6301");
    TEST_ASSERT_TRUE(fck_drv_nrf_parse_info(&m, &i));
    TEST_ASSERT_EQUAL_STRING("nrf_fck_agent", i.fw);
    TEST_ASSERT_EQUAL_STRING("1.0", i.ver);
    TEST_ASSERT_EQUAL_UINT8(1, i.ver_major);
    TEST_ASSERT_EQUAL_UINT8(0, i.ver_minor);
    TEST_ASSERT_TRUE(i.devid == 0x0123456789ABCD15ULL);
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x15, i.node, "the node id was read in decimal");
    TEST_ASSERT_EQUAL_HEX32(0x4, i.reset);
    TEST_ASSERT_EQUAL_UINT32(6301, i.uptime_ms);
    TEST_ASSERT_FALSE(!fck_drv_nrf_is_unlimited(&i));

    m = msg("NRFINFO", "fw=nrf_fck_agent ver=2.0 devid=0123456789ABCDEF reset=0x0 uptime_ms=1");
    TEST_ASSERT_TRUE(fck_drv_nrf_parse_info(&m, &i));
    TEST_ASSERT_TRUE(!fck_drv_nrf_is_unlimited(&i));

    /* Another image: read, and never taken for the official firmware. */
    m = msg("NRFINFO", "fw=my_own ver=9.9");
    TEST_ASSERT_TRUE(fck_drv_nrf_parse_info(&m, &i));
    TEST_ASSERT_FALSE_MESSAGE(fck_drv_nrf_is_compatible(&i), "no caps=: it does not speak the SDK's words");
    TEST_ASSERT_FALSE(fck_drv_nrf_is_unlimited(&i));
    m = msg("NRFINFO", "fw=my_own ver=9.9 caps=spec");
    TEST_ASSERT_TRUE(fck_drv_nrf_parse_info(&m, &i));
    TEST_ASSERT_TRUE_MESSAGE(fck_drv_nrf_is_compatible(&i), "with a caps= it does, whatever its name");
    TEST_ASSERT_TRUE(fck_drv_nrf_has(&i, FCK_DRV_NRF_SPECTRUM));
    TEST_ASSERT_FALSE(fck_drv_nrf_has(&i, FCK_DRV_NRF_PING));
    TEST_ASSERT_TRUE(i.devid == 0U);
    TEST_ASSERT_FALSE(fck_drv_nrf_is_unlimited(NULL));

    /* No fw=, or another tag: nothing written. */
    memset(&i, 0x5A, sizeof(i));
    m = msg("NRFINFO", "ver=2.0");
    TEST_ASSERT_FALSE(fck_drv_nrf_parse_info(&m, &i));
    m = msg("C5INFO", "fw=nrf_fck_agent ver=2.0");
    TEST_ASSERT_FALSE(fck_drv_nrf_parse_info(&m, &i));
    TEST_ASSERT_EQUAL_HEX8(0x5A, i.ver_major);
    /* A devid that is not 16 hex digits is left out, not half read. */
    m = msg("NRFINFO", "fw=nrf_fck_agent ver=2.0 devid=0123");
    TEST_ASSERT_TRUE(fck_drv_nrf_parse_info(&m, &i));
    TEST_ASSERT_TRUE(i.devid == 0U);
}

TEST_CASE("the nRF firmware goes by fck-nrf, and by nrf_fck_agent up to 2.1", "[nrf]")
{
    TEST_ASSERT_TRUE(fck_drv_nrf_is_official("fck-nrf"));
    TEST_ASSERT_TRUE(fck_drv_nrf_is_official("nrf_fck_agent"));
    TEST_ASSERT_FALSE(fck_drv_nrf_is_official("fck-nrf2"));
    TEST_ASSERT_FALSE(fck_drv_nrf_is_official("fck-c5"));
    TEST_ASSERT_FALSE(fck_drv_nrf_is_official(NULL));
    fck_drv_nrf_info_t i;
    fck_drv_link_message_t m = msg("NRFINFO", "fw=fck-nrf ver=2.2 devid=0123456789ABCDEF reset=0x4 uptime_ms=1");
    TEST_ASSERT_TRUE(fck_drv_nrf_parse_info(&m, &i));
    TEST_ASSERT_EQUAL_STRING("fck-nrf", i.fw);
    TEST_ASSERT_TRUE(!fck_drv_nrf_is_unlimited(&i));
}

TEST_CASE("what may reach the nRF depends on what it said it runs", "[nrf]")
{
    fck_drv_nrf_info_t one;
    fck_drv_nrf_info_t two;
    fck_drv_nrf_info_t own;
    fck_drv_link_message_t m = msg("NRFINFO", "fw=nrf_fck_agent ver=1.0");
    TEST_ASSERT_TRUE(fck_drv_nrf_parse_info(&m, &one));
    m = msg("NRFINFO", "fw=nrf_fck_agent ver=2.0");
    TEST_ASSERT_TRUE(fck_drv_nrf_parse_info(&m, &two));
    m = msg("NRFINFO", "fw=my_own ver=1.0");
    TEST_ASSERT_TRUE(fck_drv_nrf_parse_info(&m, &own));
    fck_drv_nrf_command_t c;

    /* Nothing said: nothing goes, not even `info`. */
    TEST_ASSERT_EQUAL(FCK_DRV_NRF_GATE_REFUSE, fck_drv_nrf_gate(NULL, "info", NULL, &c));

    /* The firmware: its vocabulary, with its verb and its time worked out. */
    TEST_ASSERT_EQUAL(FCK_DRV_NRF_GATE_OFFICIAL, fck_drv_nrf_gate(&one, "rf spec 0 83", NULL, &c));
    TEST_ASSERT_EQUAL_STRING("rf spec 0 83\n", c.line);
    TEST_ASSERT_EQUAL_STRING("rf_spec", c.verb);
    TEST_ASSERT_EQUAL(FCK_DRV_NRF_GATE_OFFICIAL, fck_drv_nrf_gate(&one, "info", "info", &c));
    TEST_ASSERT_EQUAL(FCK_DRV_NRF_GATE_REFUSE, fck_drv_nrf_gate(&one, "info", "rf_spec", &c));
    TEST_ASSERT_EQUAL(FCK_DRV_NRF_GATE_REFUSE, fck_drv_nrf_gate(&one, "kernel reboot", NULL, &c));
    /* The firmware 2.0.0's shell still carries Zephyr's devmem, which writes any
     * address: the UICR, the RRAM controller, the radio. */
    TEST_ASSERT_EQUAL(FCK_DRV_NRF_GATE_REFUSE,
                      fck_drv_nrf_gate(&one, "devmem 0x00FFD080 32 0", NULL, &c));
    TEST_ASSERT_EQUAL(FCK_DRV_NRF_GATE_REFUSE, fck_drv_nrf_gate(&two, "rf cw 40", NULL, &c));
    TEST_ASSERT_EQUAL(FCK_DRV_NRF_GATE_REFUSE, fck_drv_nrf_gate(&two, "rf power 8", NULL, &c));

    /* What transmits: only to a firmware that keeps the EU's limits itself. */
    TEST_ASSERT_EQUAL_MESSAGE(FCK_DRV_NRF_GATE_REFUSE, fck_drv_nrf_gate(&one, "rf ping 10 40", NULL, &c),
        "a firmware 1.0, which keeps another app's 8 dBm, was let transmit");
    TEST_ASSERT_EQUAL(FCK_DRV_NRF_GATE_REFUSE, fck_drv_nrf_gate(&one, "rf echo on 40", NULL, &c));
    TEST_ASSERT_EQUAL(FCK_DRV_NRF_GATE_OFFICIAL, fck_drv_nrf_gate(&one, "rf echo off", NULL, &c));
    TEST_ASSERT_EQUAL(FCK_DRV_NRF_GATE_OFFICIAL, fck_drv_nrf_gate(&two, "rf ping 10 40", NULL, &c));
    TEST_ASSERT_TRUE(c.emits);
    TEST_ASSERT_EQUAL(FCK_DRV_NRF_GATE_REFUSE, fck_drv_nrf_gate(&two, "rf ping 10 81", NULL, &c));

    /* Another image: as written, with the caller's verb, or not at all. */
    memset(&c, 0x5A, sizeof(c));
    TEST_ASSERT_EQUAL(FCK_DRV_NRF_GATE_AS_IS, fck_drv_nrf_gate(&own, "blink 3", "blink", &c));
    TEST_ASSERT_EQUAL(FCK_DRV_NRF_GATE_AS_IS, fck_drv_nrf_gate(&own, "rf cw 40", "rf", &c));
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x5A, (uint8_t)c.line[0], "c was written for another image");
    TEST_ASSERT_EQUAL(FCK_DRV_NRF_GATE_REFUSE, fck_drv_nrf_gate(&own, "blink 3", NULL, &c));
}

TEST_CASE("sweeps fold into the peak of each MHz, and only the MHz kept", "[nrf]")
{
    int8_t dbm[4] = { FCK_DRV_NRF_RSSI_QUIET, FCK_DRV_NRF_RSSI_QUIET, FCK_DRV_NRF_RSSI_QUIET,
                      FCK_DRV_NRF_RSSI_QUIET };
    fck_drv_nrf_spectrum_t sw = { .f0_mhz = 2439, .n = 6, .dbm = { -90, -80, -127, -60, -70, -10 } };
    fck_drv_nrf_spectrum_peak(&sw, 2440, dbm, 4);    /* 2439 and 2444 are outside */
    TEST_ASSERT_EQUAL_INT8(-80, dbm[0]);
    TEST_ASSERT_EQUAL_INT8(-127, dbm[1]);
    TEST_ASSERT_EQUAL_INT8(-60, dbm[2]);
    TEST_ASSERT_EQUAL_INT8(-70, dbm[3]);
    fck_drv_nrf_spectrum_t sw2 = { .f0_mhz = 2440, .n = 4, .dbm = { -95, -50, -65, -75 } };
    fck_drv_nrf_spectrum_peak(&sw2, 2440, dbm, 4);
    TEST_ASSERT_EQUAL_INT8_MESSAGE(-80, dbm[0], "a weaker sweep lowered a peak");
    TEST_ASSERT_EQUAL_INT8(-50, dbm[1]);
    TEST_ASSERT_EQUAL_INT8(-60, dbm[2]);
    TEST_ASSERT_EQUAL_INT8(-70, dbm[3]);
}

TEST_CASE("a sniff keeps each frame once, then the table at the end in its place", "[nrf]")
{
    static fck_drv_nrf_frames_t t;
    memset(&t, 0, sizeof(t));
    fck_drv_nrf_frame_t f;
    fck_drv_link_message_t m = msg("SNIFF24", "i=0 rssi=-31 len=2 data=0a1b");
    TEST_ASSERT_TRUE(fck_drv_nrf_parse_frame(&m, &f));
    TEST_ASSERT_TRUE(fck_drv_nrf_frames_add(&t, &f));
    TEST_ASSERT_FALSE_MESSAGE(fck_drv_nrf_frames_add(&t, &f), "the same place was kept twice");
    m = msg("SNIFF24", "i=1 rssi=-40 data=ff");
    TEST_ASSERT_TRUE(fck_drv_nrf_parse_frame(&m, &f));
    TEST_ASSERT_TRUE(fck_drv_nrf_frames_add(&t, &f));
    TEST_ASSERT_EQUAL_UINT8(2, t.n);
    TEST_ASSERT_FALSE(t.top);

    m = msg("SNIFFTOP24", "i=0 cnt=37 rssi=-24 data=ff");
    TEST_ASSERT_TRUE(fck_drv_nrf_parse_frame(&m, &f));
    TEST_ASSERT_TRUE(fck_drv_nrf_frames_add(&t, &f));
    TEST_ASSERT_TRUE(t.top);
    TEST_ASSERT_EQUAL_UINT8(1, t.n);
    TEST_ASSERT_EQUAL_UINT16(37, t.f[0].cnt);
    TEST_ASSERT_EQUAL_HEX8(0xFF, t.f[0].data[0]);
    m = msg("SNIFFTOP24", "i=1 cnt=2 rssi=-31 data=0a1b");
    TEST_ASSERT_TRUE(fck_drv_nrf_parse_frame(&m, &f));
    TEST_ASSERT_TRUE(fck_drv_nrf_frames_add(&t, &f));
    m = msg("SNIFF24", "i=5 rssi=-20 data=00");
    TEST_ASSERT_TRUE(fck_drv_nrf_parse_frame(&m, &f));
    TEST_ASSERT_FALSE_MESSAGE(fck_drv_nrf_frames_add(&t, &f), "a #SNIFF24 got into the final table");
    TEST_ASSERT_EQUAL_UINT8(2, t.n);

    /* Never more than the firmware keeps. */
    memset(&t, 0, sizeof(t));
    fck_drv_nrf_frame_t g = { .len = 1, .rssi = -50 };
    for (unsigned k = 0; k < FCK_DRV_NRF_FRAMES_MAX; k++) {
        g.i = (uint8_t)k;
        TEST_ASSERT_TRUE(fck_drv_nrf_frames_add(&t, &g));
    }
    g.i = 0;
    g.top = true;
    g.cnt = 1;
    t.top = true;
    TEST_ASSERT_FALSE(fck_drv_nrf_frames_add(&t, &g));
    TEST_ASSERT_EQUAL_UINT8(FCK_DRV_NRF_FRAMES_MAX, t.n);
}

/* The link hands the callback each line; the spy keeps the last #SPEC24. */
static fck_drv_nrf_spectrum_t s_spec;
static unsigned       s_specs;

static void on_spec(void *ctx, const fck_drv_link_message_t *m)
{
    (void)ctx;
    if (fck_drv_nrf_parse_spectrum(m, &s_spec)) {
        s_specs++;
    }
}

TEST_CASE("a #SPEC24 of all 101 MHz goes through the link whole and reads back", "[nrf]")
{
    /* The longest the firmware prints: every value -127, 545 bytes. */
    char line[700];
    int  k = snprintf(line, sizeof(line), "#SPEC24 f0=2400 step=1 n=101 sweep=999 v=");
    for (int i = 0; i < 101; i++) {
        k += snprintf(line + k, sizeof(line) - (size_t)k, "%s%d", i ? "," : "",
                      (i == 50) ? -40 : -127);
    }
    TEST_ASSERT_TRUE(strlen(line) > 512U);
    k += snprintf(line + k, sizeof(line) - (size_t)k, "\r\n#DONE rf_spec rc=0\r\n");

    fck_drv_link_t link;
    TEST_ASSERT_EQUAL(KODE_OK, fck_drv_link_init(&link, NULL));
    TEST_ASSERT_EQUAL(KODE_OK, fck_drv_link_begin(&link, "rf_spec", on_spec, NULL));
    s_specs = 0U;
    fck_drv_link_feed(&link, (const uint8_t *)line, (size_t)k);

    int32_t rc = -1;
    TEST_ASSERT_TRUE(fck_drv_link_done(&link, &rc));
    TEST_ASSERT_EQUAL_INT32(0, rc);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, fck_drv_link_statistics(&link).too_long,
        "the link dropped the firmware's longest line");
    TEST_ASSERT_EQUAL_UINT(1, s_specs);
    TEST_ASSERT_EQUAL_UINT16(2400, s_spec.f0_mhz);
    TEST_ASSERT_EQUAL_UINT8(101, s_spec.n);
    TEST_ASSERT_EQUAL_UINT16(999, s_spec.sweep);
    TEST_ASSERT_EQUAL_INT8(-127, s_spec.dbm[0]);
    TEST_ASSERT_EQUAL_INT8(-40, s_spec.dbm[50]);
    TEST_ASSERT_EQUAL_INT8(-127, s_spec.dbm[100]);
}

TEST_CASE("a #SPEC24 whose values do not match n= is not read", "[nrf]")
{
    fck_drv_nrf_spectrum_t s;
    fck_drv_link_message_t m = msg("SPEC24", "f0=2440 step=1 n=3 sweep=0 v=-90,-91,-92");
    TEST_ASSERT_TRUE(fck_drv_nrf_parse_spectrum(&m, &s));
    TEST_ASSERT_EQUAL_INT8(-92, s.dbm[2]);

    memset(&s, 0x5A, sizeof(s));
    const char *const bad[] = {
        "f0=2440 step=1 n=3 sweep=0 v=-90,-91",          /* fewer   */
        "f0=2440 step=1 n=3 sweep=0 v=-90,-91,-92,-93",  /* more    */
        "f0=2440 step=1 n=3 sweep=0 v=-90,,-92",         /* a hole  */
        "f0=2440 step=1 n=3 sweep=0 v=-90,-91,x",        /* a word  */
        "f0=2440 step=1 n=3 sweep=0 v=-90,-91,-92,",     /* a tail  */
        "f0=2499 step=1 n=3 sweep=0 v=-90,-91,-92",      /* past 2500 MHz */
        "f0=2440 step=1 n=0 sweep=0 v=",
        "f0=2440 step=1 n=3 v=-90,-91,-92",              /* no sweep */
    };
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        m = msg("SPEC24", bad[i]);
        TEST_ASSERT_FALSE_MESSAGE(fck_drv_nrf_parse_spectrum(&m, &s), bad[i]);
    }
    TEST_ASSERT_EQUAL_HEX8(0x5A, s.n);
}

TEST_CASE("#ERR names the command and why, whichever key the firmware used", "[nrf]")
{
    fck_drv_nrf_error_t e;
    fck_drv_link_message_t m = msg("ERR", "cmd=rf_spec why=not_in_rx points=3");
    TEST_ASSERT_TRUE(fck_drv_nrf_parse_error(&m, &e));
    TEST_ASSERT_EQUAL_STRING("rf_spec", e.cmd);
    TEST_ASSERT_EQUAL_STRING("not_in_rx", e.why);
    TEST_ASSERT_EQUAL_INT32(3, e.code);

    m = msg("ERR", "cmd=rf_ping why=hfxo err=-11");
    TEST_ASSERT_TRUE(fck_drv_nrf_parse_error(&m, &e));
    TEST_ASSERT_EQUAL_STRING("hfxo", e.why);
    TEST_ASSERT_EQUAL_INT32(-11, e.code);

    m = msg("ERR", "what=rf_init rc=-5");      /* main.c, at boot */
    TEST_ASSERT_TRUE(fck_drv_nrf_parse_error(&m, &e));
    TEST_ASSERT_EQUAL_STRING("rf_init", e.cmd);
    TEST_ASSERT_EQUAL_STRING("", e.why);
    TEST_ASSERT_EQUAL_INT32(-5, e.code);

    m = msg("ERR", "why=hfxo");
    TEST_ASSERT_FALSE(fck_drv_nrf_parse_error(&m, &e));
}

TEST_CASE("ping: start, each packet, the summary, and the node ids in hex", "[nrf]")
{
    fck_drv_nrf_ping_start_t st;
    fck_drv_link_message_t m = msg("PINGSTART24", "n=20 ch=40 mhz=2440 dbm=0 node=1A timeout_ms=20");
    TEST_ASSERT_TRUE(fck_drv_nrf_parse_ping_start(&m, &st));
    TEST_ASSERT_EQUAL_UINT16(20, st.n);
    TEST_ASSERT_EQUAL_UINT16(2440, st.mhz);
    TEST_ASSERT_EQUAL_HEX8(0x1A, st.node);
    TEST_ASSERT_EQUAL_UINT16(20, st.timeout_ms);

    fck_drv_nrf_ping_t p;
    m = msg("PING24", "seq=1 ok=1 rssi_fwd=-41 rssi_rev=-43");
    TEST_ASSERT_TRUE(fck_drv_nrf_parse_ping(&m, &p));
    TEST_ASSERT_TRUE(p.ok);
    TEST_ASSERT_EQUAL(FCK_DRV_NRF_PING_OK, p.why);
    TEST_ASSERT_EQUAL_INT8(-41, p.rssi_fwd);
    TEST_ASSERT_EQUAL_INT8(-43, p.rssi_rev);

    m = msg("PING24", "seq=2 ok=0 why=timeout");
    TEST_ASSERT_TRUE(fck_drv_nrf_parse_ping(&m, &p));
    TEST_ASSERT_FALSE(p.ok);
    TEST_ASSERT_EQUAL(FCK_DRV_NRF_PING_TIMEOUT, p.why);
    TEST_ASSERT_EQUAL_INT8(FCK_DRV_NRF_RSSI_NONE, p.rssi_fwd);
    TEST_ASSERT_EQUAL_INT8(FCK_DRV_NRF_RSSI_NONE, p.rssi_rev);

    m = msg("PING24", "seq=3 ok=0 why=crc rssi_rev=-88");
    TEST_ASSERT_TRUE(fck_drv_nrf_parse_ping(&m, &p));
    TEST_ASSERT_EQUAL(FCK_DRV_NRF_PING_CRC, p.why);
    TEST_ASSERT_EQUAL_INT8(-88, p.rssi_rev);

    m = msg("PING24", "seq=4 ok=0 why=mismatch type=0xA5 seq_rx=3");
    TEST_ASSERT_TRUE(fck_drv_nrf_parse_ping(&m, &p));
    TEST_ASSERT_EQUAL(FCK_DRV_NRF_PING_MISMATCH, p.why);
    m = msg("PING24", "seq=5 ok=0 why=txfail");
    TEST_ASSERT_TRUE(fck_drv_nrf_parse_ping(&m, &p));
    TEST_ASSERT_EQUAL(FCK_DRV_NRF_PING_TRANSMIT_FAILED, p.why);
    m = msg("PING24", "seq=6 ok=0 why=moon");
    TEST_ASSERT_TRUE(fck_drv_nrf_parse_ping(&m, &p));
    TEST_ASSERT_EQUAL(FCK_DRV_NRF_PING_OTHER, p.why);
    /* ok=1 without both RSSIs is not a ping that worked. */
    m = msg("PING24", "seq=7 ok=1 rssi_fwd=-41");
    TEST_ASSERT_FALSE(fck_drv_nrf_parse_ping(&m, &p));

    fck_drv_nrf_ping_done_t d;
    m = msg("PINGDONE24", "ch=40 sent=20 ok=19 fwd_mean=-41.5 rev_mean=-0.4 mhz=2440 dbm=0 "
                          "timeout=1 crcbad=0 mismatch=0 txfail=0 fwd_min=-45 fwd_max=-38 "
                          "rev_min=-47 rev_max=-39 peer=0A peer_dbm=-4 txslow=2");
    TEST_ASSERT_TRUE(fck_drv_nrf_parse_ping_done(&m, &d));
    TEST_ASSERT_EQUAL_UINT16(2440, d.mhz);
    TEST_ASSERT_EQUAL_UINT16(20, d.sent);
    TEST_ASSERT_EQUAL_UINT16(19, d.ok);
    TEST_ASSERT_EQUAL_INT16(-415, d.fwd_mean_x10);
    TEST_ASSERT_EQUAL_INT16(-4, d.rev_mean_x10);
    TEST_ASSERT_EQUAL_UINT16(1, d.timeout);
    TEST_ASSERT_EQUAL_INT8(-45, d.fwd_min);
    TEST_ASSERT_EQUAL_INT8(-39, d.rev_max);
    TEST_ASSERT_EQUAL_HEX8(0x0A, d.peer);
    TEST_ASSERT_EQUAL_INT8(-4, d.peer_dbm);
    TEST_ASSERT_EQUAL_UINT32(2, d.txslow);

    /* No echo at all: the firmware's means are "0.0". */
    m = msg("PINGDONE24", "ch=40 sent=5 ok=0 fwd_mean=0.0 rev_mean=0.0");
    TEST_ASSERT_TRUE(fck_drv_nrf_parse_ping_done(&m, &d));
    TEST_ASSERT_EQUAL_INT16(0, d.fwd_mean_x10);
    /* A mean without its decimal, or more ok than sent, is not read. */
    m = msg("PINGDONE24", "ch=40 sent=5 ok=0 fwd_mean=-41 rev_mean=0.0");
    TEST_ASSERT_FALSE(fck_drv_nrf_parse_ping_done(&m, &d));
    m = msg("PINGDONE24", "ch=40 sent=5 ok=6 fwd_mean=0.0 rev_mean=0.0");
    TEST_ASSERT_FALSE(fck_drv_nrf_parse_ping_done(&m, &d));
}

TEST_CASE("echo, its statistics and the power step read back", "[nrf]")
{
    fck_drv_nrf_echo_t e;
    fck_drv_link_message_t m = msg("ECHO24", "on=1 ch=2 mhz=2402 dbm=6 node=15");
    TEST_ASSERT_TRUE(fck_drv_nrf_parse_echo(&m, &e));
    TEST_ASSERT_TRUE(e.on);
    TEST_ASSERT_EQUAL_UINT16(2402, e.mhz);
    TEST_ASSERT_EQUAL_INT8(6, e.dbm);
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x15, e.node, "node=15 was read as fifteen");
    m = msg("ECHO24", "on=0");
    TEST_ASSERT_TRUE(fck_drv_nrf_parse_echo(&m, &e));
    TEST_ASSERT_FALSE(e.on);
    TEST_ASSERT_EQUAL_UINT16(0, e.mhz);
    m = msg("ECHO24", "on=1 ch=2");
    TEST_ASSERT_FALSE(fck_drv_nrf_parse_echo(&m, &e));

    fck_drv_nrf_echo_statistics_t es;
    m = msg("ECHOSTAT24", "on=1 ch=40 rx=120 crcbad=3 replied=117 foreign=0 txslow=1");
    TEST_ASSERT_TRUE(fck_drv_nrf_parse_echo_statistics(&m, &es));
    TEST_ASSERT_EQUAL_UINT16(2440, es.mhz);
    TEST_ASSERT_EQUAL_UINT32(117, es.replied);
    TEST_ASSERT_EQUAL_UINT32(1, es.txslow);

    fck_drv_nrf_power_t p;
    m = msg("POWER24", "req=-21 dbm=-28");
    TEST_ASSERT_TRUE(fck_drv_nrf_parse_power(&m, &p));
    TEST_ASSERT_EQUAL_INT16(-21, p.req);
    TEST_ASSERT_EQUAL_INT8(-28, p.dbm);
}

TEST_CASE("sniff: what it listens with, each frame, and the end", "[nrf]")
{
    fck_drv_nrf_sniff_start_t st;
    fck_drv_link_message_t m = msg("SNIFFSTART24", "ch=76 mhz=2476 rate=2M crc=0 len=32 balen=2 "
                                      "base=0xaaaaaaaa prefix=0xaa rssi_min=-60 ms=20000");
    TEST_ASSERT_TRUE(fck_drv_nrf_parse_sniff_start(&m, &st));
    TEST_ASSERT_EQUAL_UINT16(2476, st.mhz);
    TEST_ASSERT_EQUAL_UINT8(2, st.phy.rate_mbps);
    TEST_ASSERT_EQUAL_UINT8(0, st.phy.crc_bytes);
    TEST_ASSERT_EQUAL_UINT8(32, st.phy.len);
    TEST_ASSERT_EQUAL_UINT8(2, st.addr.balen);
    TEST_ASSERT_EQUAL_HEX32(0xAAAAAAAAU, st.addr.base);
    TEST_ASSERT_EQUAL_HEX8(0xAA, st.addr.prefix);
    TEST_ASSERT_EQUAL_INT8(-60, st.rssi_min);
    TEST_ASSERT_EQUAL_UINT32(20000, st.ms);

    fck_drv_nrf_frame_t f;
    m = msg("SNIFF24", "i=0 rssi=-31 len=4 data=0a1bff00");
    TEST_ASSERT_TRUE(fck_drv_nrf_parse_frame(&m, &f));
    TEST_ASSERT_FALSE(f.top);
    TEST_ASSERT_EQUAL_UINT16(1, f.cnt);
    TEST_ASSERT_EQUAL_UINT8(4, f.len);
    const uint8_t want[] = { 0x0A, 0x1B, 0xFF, 0x00 };
    TEST_ASSERT_EQUAL_HEX8_ARRAY(want, f.data, 4);

    m = msg("SNIFFTOP24", "i=0 cnt=37 rssi=-24 data=0a1bff00");
    TEST_ASSERT_TRUE(fck_drv_nrf_parse_frame(&m, &f));
    TEST_ASSERT_TRUE(f.top);
    TEST_ASSERT_EQUAL_UINT16(37, f.cnt);
    TEST_ASSERT_EQUAL_UINT8(4, f.len);

    memset(&f, 0x5A, sizeof(f));
    const char *const bad[] = {
        "i=0 rssi=-31 len=3 data=0a1bff00",   /* len= that does not match */
        "i=0 rssi=-31 data=0a1bf",            /* half a byte              */
        "i=0 rssi=-31 data=0a1bzz",           /* not hex                  */
        "i=48 rssi=-31 data=0a",              /* past the firmware's table   */
        "i=0 rssi=-31 data=",
    };
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        m = msg("SNIFF24", bad[i]);
        TEST_ASSERT_FALSE_MESSAGE(fck_drv_nrf_parse_frame(&m, &f), bad[i]);
    }
    m = msg("SNIFFTOP24", "i=0 rssi=-24 data=0a");   /* a table row needs its count */
    TEST_ASSERT_FALSE(fck_drv_nrf_parse_frame(&m, &f));
    TEST_ASSERT_EQUAL_HEX8(0x5A, f.len);

    fck_drv_nrf_sniff_done_t d;
    m = msg("SNIFFDONE24", "ch=76 rx=812 gated=640 uniq=9 ms=20000");
    TEST_ASSERT_TRUE(fck_drv_nrf_parse_sniff_done(&m, &d));
    TEST_ASSERT_EQUAL_UINT16(2476, d.mhz);
    TEST_ASSERT_EQUAL_UINT32(640, d.gated);
    TEST_ASSERT_EQUAL_UINT8(9, d.uniq);

    fck_drv_nrf_sniff_config_t phy;
    m = msg("SCFG24", "rate=1M crc=2 len=40");
    TEST_ASSERT_TRUE(fck_drv_nrf_parse_sniff_config(&m, &phy));
    TEST_ASSERT_EQUAL_UINT8(1, phy.rate_mbps);
    TEST_ASSERT_EQUAL_UINT8(2, phy.crc_bytes);
    m = msg("SCFG24", "rate=3M crc=2 len=40");
    TEST_ASSERT_FALSE(fck_drv_nrf_parse_sniff_config(&m, &phy));

    fck_drv_nrf_sniff_address_t a;
    m = msg("SADDR24", "balen=4 addr_bytes=5 base=0xe7e7e7e7 prefix=0x0a");
    TEST_ASSERT_TRUE(fck_drv_nrf_parse_sniff_address(&m, &a));
    TEST_ASSERT_EQUAL_UINT8(4, a.balen);
    TEST_ASSERT_EQUAL_HEX32(0xE7E7E7E7U, a.base);
    TEST_ASSERT_EQUAL_HEX8(0x0A, a.prefix);
}

/* ---- fck-nrf 0.1.0: map=, the responder that stops by itself, txslow ----------------- */

TEST_CASE("0.1.0's #NRFINFO says where the image was linked, in map=", "[nrf]")
{
    fck_drv_nrf_info_t     i;
    fck_drv_link_message_t m = msg("NRFINFO", NRF_010);
    TEST_ASSERT_TRUE(fck_drv_nrf_parse_info(&m, &i));
    TEST_ASSERT_TRUE(i.has_map);
    TEST_ASSERT_EQUAL_HEX32(0x0000E800, i.map);
    m = msg("NRFINFO", "fw=fck-nrf ver=0.1.0 map=0x00010800 caps=" NRF_010_CAPS);
    TEST_ASSERT_TRUE(fck_drv_nrf_parse_info(&m, &i));
    TEST_ASSERT_TRUE(i.has_map);
    TEST_ASSERT_EQUAL_HEX32(0x00010800, i.map);
    /* The first modules' 2.x says none, and a word that is no number is none. */
    m = msg("NRFINFO", "fw=fck-nrf ver=2.1.0 devid=0123456789ABCDEF reset=0x1 uptime_ms=5");
    TEST_ASSERT_TRUE(fck_drv_nrf_parse_info(&m, &i));
    TEST_ASSERT_FALSE(i.has_map);
    TEST_ASSERT_EQUAL_HEX32(0, i.map);
    m = msg("NRFINFO", "fw=fck-nrf ver=0.1.0 map=here");
    TEST_ASSERT_TRUE(fck_drv_nrf_parse_info(&m, &i));
    TEST_ASSERT_FALSE(i.has_map);
}

TEST_CASE("the responder says after how long it stops by itself, and that it did", "[nrf]")
{
    fck_drv_nrf_echo_t     e;
    fck_drv_link_message_t m = msg("ECHO24", "on=1 ch=40 mhz=2440 dbm=0 node=EF idle_s=60");
    TEST_ASSERT_TRUE(fck_drv_nrf_parse_echo(&m, &e));
    TEST_ASSERT_TRUE(e.on);
    TEST_ASSERT_EQUAL_UINT16(2440, e.mhz);
    TEST_ASSERT_EQUAL_UINT16(60, e.idle_s);
    /* 2.x says none: that one answers until it is told to stop. */
    m = msg("ECHO24", "on=1 ch=40 mhz=2440 dbm=0 node=EF");
    TEST_ASSERT_TRUE(fck_drv_nrf_parse_echo(&m, &e));
    TEST_ASSERT_EQUAL_UINT16(0, e.idle_s);
    m = msg("ECHO24", "on=0");
    TEST_ASSERT_TRUE(fck_drv_nrf_parse_echo(&m, &e));
    TEST_ASSERT_EQUAL_UINT16(0, e.idle_s);

    fck_drv_nrf_echo_statistics_t s;
    m = msg("ECHOSTAT24", "on=0 ch=40 rx=12 crcbad=0 replied=12 foreign=1 txslow=2 idle=1");
    TEST_ASSERT_TRUE(fck_drv_nrf_parse_echo_statistics(&m, &s));
    TEST_ASSERT_FALSE(s.on);
    TEST_ASSERT_TRUE_MESSAGE(s.idle, "it stopped by itself");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(2, s.txslow, "transmissions the firmware had to cut");
    m = msg("ECHOSTAT24", "on=1 ch=40 rx=12 crcbad=0 replied=12 foreign=1 txslow=0 idle=0");
    TEST_ASSERT_TRUE(fck_drv_nrf_parse_echo_statistics(&m, &s));
    TEST_ASSERT_FALSE(s.idle);
    m = msg("ECHOSTAT24", "on=0 ch=40 rx=12 crcbad=0 replied=12 foreign=1 txslow=0");
    TEST_ASSERT_TRUE(fck_drv_nrf_parse_echo_statistics(&m, &s));
    TEST_ASSERT_FALSE_MESSAGE(s.idle, "a firmware that does not say did not");
}
