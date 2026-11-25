/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_c5.c
 * @brief fck_drv_c5: the C5 firmware's commands, their times, and its lines read.
 *
 * Every line here has the firmware's own format, from its source, 2.0.
 * SSIDs and MACs are made up. Each line
 * reaches its reader through fck_drv_link, as it will on a module.
 */
#include <stdio.h>
#include <string.h>

#include "unity.h"

#include "fck_drv_c5.h"
#include "fck_c5_hello.h"
#include "fck_drv_link.h"

/* ---- One firmware line, as the link hands it over ------------------------------ */

static char      s_tag[FCK_DRV_LINK_TAG_MAX + 1];
static char      s_body[FCK_DRV_LINK_LINE_MAX + 1];
static fck_drv_link_message_t s_msg;

static void keep(void *ctx, const fck_drv_link_message_t *m)
{
    (void)ctx;
    snprintf(s_tag, sizeof(s_tag), "%s", m->tag);
    snprintf(s_body, sizeof(s_body), "%s", m->body);
}

/* The line split by a link with nothing in flight; valid until the next call. */
static const fck_drv_link_message_t *line(const char *text)
{
    static fck_drv_link_t l;
    fck_drv_link_config_t    cfg = { .on_async = keep };
    TEST_ASSERT_EQUAL(KODE_OK, fck_drv_link_init(&l, &cfg));
    s_tag[0]  = '\0';
    s_body[0] = '\0';
    fck_drv_link_feed(&l, (const uint8_t *)text, strlen(text));
    fck_drv_link_feed(&l, (const uint8_t *)"\r\n", 2);
    TEST_ASSERT_TRUE_MESSAGE(s_tag[0] != '\0', "not a machine line");
    s_msg.tag  = s_tag;
    s_msg.body = s_body;
    return &s_msg;
}

static void assert_cmd(const fck_drv_c5_command_t *c, const char *text, const char *verb,
                       uint32_t ms)
{
    char want[FCK_DRV_C5_COMMAND_MAX + 2];
    snprintf(want, sizeof(want), "%s\n", text);
    TEST_ASSERT_EQUAL_STRING(want, c->line);
    TEST_ASSERT_EQUAL_size_t(strlen(want), c->len);
    TEST_ASSERT_EQUAL_STRING(verb, c->verb);
    TEST_ASSERT_EQUAL_UINT32(ms, c->ms);
}

/* ---- Commands ---------------------------------------------------------------- */

TEST_CASE("info is the firmware's `info`, ended by its own verb, in the default time", "[c5]")
{
    fck_drv_c5_command_t c;
    TEST_ASSERT_EQUAL(KODE_OK, fck_drv_c5_command_info(&c));
    assert_cmd(&c, "info", "info", FCK_DRV_C5_COMMAND_MS);
}

TEST_CASE("scan, chan and ble scan get the firmware's times, worked out per band", "[c5]")
{
    fck_drv_c5_command_t c;
    TEST_ASSERT_EQUAL(KODE_OK, fck_drv_c5_command_scan(&c, FCK_DRV_C5_BAND_ALL));
    assert_cmd(&c, "scan all", "scan", 20000U);
    TEST_ASSERT_EQUAL(KODE_OK, fck_drv_c5_command_scan(&c, FCK_DRV_C5_BAND_5G));
    assert_cmd(&c, "scan 5g", "scan", 20000U);

    /* At the default dwell: 38 channels tried. */
    TEST_ASSERT_EQUAL(KODE_OK, fck_drv_c5_command_channel(&c, FCK_DRV_C5_BAND_ALL, 250));
    assert_cmd(&c, "chan all 250", "chan", 38U * 350U + 8000U);
    TEST_ASSERT_EQUAL(KODE_OK, fck_drv_c5_command_channel(&c, FCK_DRV_C5_BAND_2G, 100));
    assert_cmd(&c, "chan 2g 100", "chan", 13U * 200U + 8000U);
    TEST_ASSERT_EQUAL(KODE_OK, fck_drv_c5_command_channel(&c, FCK_DRV_C5_BAND_5G, 5000));
    assert_cmd(&c, "chan 5g 5000", "chan", 25U * 5100U + 8000U);

    /* `ble scan 5` ends with #DONE ble, not with the whole command. */
    TEST_ASSERT_EQUAL(KODE_OK, fck_drv_c5_command_ble_scan(&c, 5));
    assert_cmd(&c, "ble scan 5", "ble", 13000U);
    TEST_ASSERT_EQUAL(KODE_OK, fck_drv_c5_command_ble_scan(&c, 60));
    assert_cmd(&c, "ble scan 60", "ble", 68000U);

    TEST_ASSERT_EQUAL(KODE_OK, fck_drv_c5_command_gnss(&c, true));
    assert_cmd(&c, "gnss on", "gnss", FCK_DRV_C5_COMMAND_MS);
    TEST_ASSERT_EQUAL(KODE_OK, fck_drv_c5_command_gnss(&c, false));
    assert_cmd(&c, "gnss off", "gnss", FCK_DRV_C5_COMMAND_MS);
}

TEST_CASE("only scan transmits, whatever its band, its words or the firmware's version", "[c5]")
{
    fck_drv_c5_command_t c;
    /* Probe requests: 2.0's on 2.4 GHz inside the radio limits, 1.5's and 1.6's
     * on both bands with none. 2.0's `scan 5g` only listens and an older
     * firmware's does not, so no band clears it; nor does a word the firmware
     * would refuse. */
    static const char *const sends[] = {
        "scan", "scan all", "scan 2g", "scan 5g", "  scan 5g", "scan nonsense",
    };
    for (size_t i = 0; i < sizeof(sends) / sizeof(sends[0]); i++) {
        TEST_ASSERT_EQUAL(KODE_OK, fck_drv_c5_command(&c, sends[i]));
        TEST_ASSERT_TRUE_MESSAGE(c.emits, sends[i]);
    }
    for (int b = FCK_DRV_C5_BAND_ALL; b <= FCK_DRV_C5_BAND_5G; b++) {
        TEST_ASSERT_EQUAL(KODE_OK, fck_drv_c5_command_scan(&c, (fck_drv_c5_band_t)b));
        TEST_ASSERT_TRUE(c.emits);
    }

    /* Everything else only listens, 1.6's Marauder words included: `deauth`
     * and `probe` count what others send. */
    static const char *const quiet[] = {
        "info", "chan", "chan 5g 250", "ble scan 5", "ble raw 5", "gnss on", "gnss off",
        "gtx -n POLCFGMSG,0,2,1", "gtx loop", "wp 1", "sniff 10 5g", "pcap 10 ch=6",
        "deauth 10", "probe 10", "scanx", "rescan",
    };
    for (size_t i = 0; i < sizeof(quiet) / sizeof(quiet[0]); i++) {
        TEST_ASSERT_EQUAL(KODE_OK, fck_drv_c5_command(&c, quiet[i]));
        TEST_ASSERT_FALSE_MESSAGE(c.emits, quiet[i]);
    }
    TEST_ASSERT_EQUAL(KODE_OK, fck_drv_c5_command_info(&c));
    TEST_ASSERT_FALSE(c.emits);
    TEST_ASSERT_EQUAL(KODE_OK, fck_drv_c5_command_channel(&c, FCK_DRV_C5_BAND_5G, 250));
    TEST_ASSERT_FALSE(c.emits);
    TEST_ASSERT_EQUAL(KODE_OK, fck_drv_c5_command_ble_scan(&c, 5));
    TEST_ASSERT_FALSE(c.emits);
    TEST_ASSERT_EQUAL(KODE_OK, fck_drv_c5_command_gnss(&c, true));
    TEST_ASSERT_FALSE(c.emits);
    TEST_ASSERT_EQUAL(KODE_OK, fck_drv_c5_command_gnss_transmit(&c, "POLCFGMSG,0,2,1"));
    TEST_ASSERT_FALSE(c.emits);
}

TEST_CASE("any command is timed from its own arguments, read the firmware's way", "[c5]")
{
    fck_drv_c5_command_t c;
    TEST_ASSERT_EQUAL(KODE_OK, fck_drv_c5_command(&c, "chan dwell_ms=300 2g"));
    assert_cmd(&c, "chan dwell_ms=300 2g", "chan", 13U * 400U + 8000U);
    /* The last band word wins, as in the firmware's loop. */
    TEST_ASSERT_EQUAL(KODE_OK, fck_drv_c5_command(&c, "chan 5g 2g"));
    assert_cmd(&c, "chan 5g 2g", "chan", 13U * 350U + 8000U);
    TEST_ASSERT_EQUAL(KODE_OK, fck_drv_c5_command(&c, "chan"));
    assert_cmd(&c, "chan", "chan", 38U * 350U + 8000U);
    TEST_ASSERT_EQUAL(KODE_OK, fck_drv_c5_command(&c, "ble scan secs=10"));
    assert_cmd(&c, "ble scan secs=10", "ble", 18000U);
    TEST_ASSERT_EQUAL(KODE_OK, fck_drv_c5_command(&c, "ble scan"));
    assert_cmd(&c, "ble scan", "ble", 13000U);
    /* Numbers are read base 10 from a digit, as the firmware reads them (not
     * as strtol's base 0 would): "010" is 10, and "0x3c" is no number, which
     * the firmware answers at once with rc 2. */
    TEST_ASSERT_EQUAL(KODE_OK, fck_drv_c5_command(&c, "ble scan 010"));
    assert_cmd(&c, "ble scan 010", "ble", 18000U);
    TEST_ASSERT_EQUAL(KODE_OK, fck_drv_c5_command(&c, "ble scan 0x3c"));
    assert_cmd(&c, "ble scan 0x3c", "ble", 13000U);
    TEST_ASSERT_EQUAL(KODE_OK, fck_drv_c5_command(&c, "chan 2g dwell_ms=0300"));
    assert_cmd(&c, "chan 2g dwell_ms=0300", "chan", 13U * 400U + 8000U);
    /* `ble` alone is a usage line, at once. */
    TEST_ASSERT_EQUAL(KODE_OK, fck_drv_c5_command(&c, "ble"));
    assert_cmd(&c, "ble", "ble", FCK_DRV_C5_COMMAND_MS);
    TEST_ASSERT_EQUAL(KODE_OK, fck_drv_c5_command(&c, "wp 1"));
    assert_cmd(&c, "wp 1", "wp", FCK_DRV_C5_COMMAND_MS);
    /* The firmware skips leading spaces, and so does the verb. */
    TEST_ASSERT_EQUAL(KODE_OK, fck_drv_c5_command(&c, "  gtx loop"));
    assert_cmd(&c, "  gtx loop", "gtx", FCK_DRV_C5_COMMAND_MS);
}

TEST_CASE("a command the firmware would cut, split or refuse is never built", "[c5]")
{
    fck_drv_c5_command_t c;
    memset(&c, 0x5A, sizeof(c));
    fck_drv_c5_command_t before = c;

    TEST_ASSERT_EQUAL(KODE_DENIED, fck_drv_c5_command(NULL, "info"));
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_drv_c5_command(&c, NULL));
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_drv_c5_command(&c, ""));
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_drv_c5_command(&c, "   "));
    TEST_ASSERT_EQUAL_MESSAGE(KODE_DENIED, fck_drv_c5_command(&c, "scan\nall"),
                              "a line ending inside is two commands");
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_drv_c5_command(&c, "gtx\t-n"));
    TEST_ASSERT_EQUAL_MESSAGE(KODE_DENIED, fck_drv_c5_command(&c, "abcdefghijklmnop x"),
                              "a verb longer than the link keeps");

    char text[FCK_DRV_C5_COMMAND_MAX + 2];
    memset(text, 'x', sizeof(text));
    text[FCK_DRV_C5_COMMAND_MAX + 1] = '\0';
    text[0]                  = 'w';
    text[1]                  = 'p';
    text[2]                  = ' ';
    TEST_ASSERT_EQUAL_MESSAGE(KODE_DENIED, fck_drv_c5_command(&c, text),
                              "the firmware would run the first 255 characters");

    TEST_ASSERT_EQUAL(KODE_DENIED, fck_drv_c5_command_scan(&c, (fck_drv_c5_band_t)3));
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_drv_c5_command_channel(&c, FCK_DRV_C5_BAND_ALL, 19));
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_drv_c5_command_channel(&c, FCK_DRV_C5_BAND_ALL, 5001));
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_drv_c5_command_channel(&c, (fck_drv_c5_band_t)-1, 250));
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_drv_c5_command_ble_scan(&c, 0));
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_drv_c5_command_ble_scan(&c, 61));
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(&before, &c, sizeof(c), "a refusal wrote the command");

    text[FCK_DRV_C5_COMMAND_MAX] = '\0';
    TEST_ASSERT_EQUAL(KODE_OK, fck_drv_c5_command(&c, text));
    TEST_ASSERT_EQUAL_size_t(FCK_DRV_C5_COMMAND_MAX + 1U, c.len);
}

TEST_CASE("gtx goes without a checksum, and never moves the receiver's baud rate", "[c5]")
{
    fck_drv_c5_command_t c;
    TEST_ASSERT_EQUAL(KODE_OK, fck_drv_c5_command_gnss_transmit(&c, "POLCFGMSG,0,2,1"));
    assert_cmd(&c, "gtx -n POLCFGMSG,0,2,1", "gtx", FCK_DRV_C5_COMMAND_MS);
    TEST_ASSERT_EQUAL(KODE_OK, fck_drv_c5_command_gnss_transmit(&c, "$POLCFGMSG,0,2,0"));
    assert_cmd(&c, "gtx -n POLCFGMSG,0,2,0", "gtx", FCK_DRV_C5_COMMAND_MS);

    TEST_ASSERT_EQUAL_MESSAGE(KODE_DENIED, fck_drv_c5_command_gnss_transmit(&c, "POLCFGMSG,0,2,1*3A"),
                              "the ME31GR02 ignores a sentence with a checksum");
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_drv_c5_command_gnss_transmit(&c, "POLCFGPRT,0,9600"));
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_drv_c5_command_gnss_transmit(&c, "$POLCFGPRT,0,9600"));
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_drv_c5_command_gnss_transmit(&c, "POLCFGMSG,0, 2,1"));
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_drv_c5_command_gnss_transmit(&c, "A\"B"));
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_drv_c5_command_gnss_transmit(&c, "A\\B"));
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_drv_c5_command_gnss_transmit(&c, "A$B"));
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_drv_c5_command_gnss_transmit(&c, ""));
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_drv_c5_command_gnss_transmit(&c, "$"));
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_drv_c5_command_gnss_transmit(&c, NULL));

    char body[FCK_DRV_C5_GNSS_TRANSMIT_MAX + 2];
    memset(body, 'A', sizeof(body));
    body[FCK_DRV_C5_GNSS_TRANSMIT_MAX] = '\0';
    TEST_ASSERT_EQUAL(KODE_OK, fck_drv_c5_command_gnss_transmit(&c, body));
    body[FCK_DRV_C5_GNSS_TRANSMIT_MAX]      = 'A';
    body[FCK_DRV_C5_GNSS_TRANSMIT_MAX + 1U] = '\0';
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_drv_c5_command_gnss_transmit(&c, body));
}

TEST_CASE("a firmware's rc becomes the status the SDK's callers know", "[c5]")
{
    TEST_ASSERT_EQUAL(KODE_OK, fck_drv_c5_status(0));
    TEST_ASSERT_EQUAL(KODE_IO_ERROR, fck_drv_c5_status(1));
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_drv_c5_status(2));
    TEST_ASSERT_EQUAL_MESSAGE(KODE_IO_ERROR, fck_drv_c5_status(-1), "a #DONE without an rc");
    TEST_ASSERT_EQUAL(KODE_IO_ERROR, fck_drv_c5_status(3));
}

/* ---- #C5INFO ------------------------------------------------------------------ */

/* fck-c5 0.1.0's line, with the format of firmware/fck-c5 (cmd_info(), FW_CAPS);
 * the MAC is made up. */
#define C5_010_HEAD "#C5INFO fw=fck-c5 ver=0.1 chip=esp32c5 rev=1.2 mac=aa:bb:cc:dd:ee:01 "
#define C5_010_TAIL " uptime_ms=1234 rst=poweron"
#define C5_010      C5_010_HEAD "reg=EU lim=ok caps=scan,chan,ble,gnss,wp,gtx,health" C5_010_TAIL

static const fck_drv_c5_feature_t k_every_feature[] = {
    FCK_DRV_C5_SCAN,         FCK_DRV_C5_CHANNEL,     FCK_DRV_C5_BLE_SCAN, FCK_DRV_C5_WAKE_PIN,
    FCK_DRV_C5_GNSS_TRANSMIT, FCK_DRV_C5_GNSS_REPEAT, FCK_DRV_C5_GNSS_RELAY,
    FCK_DRV_C5_HEALTH,
};
#define EVERY_FEATURE (sizeof(k_every_feature) / sizeof(k_every_feature[0]))

TEST_CASE("0.1's #C5INFO says what it does in caps=, and every feature is on", "[c5]")
{
    fck_drv_c5_info_t i;
    TEST_ASSERT_TRUE(fck_drv_c5_parse_info(line(C5_010), &i));
    TEST_ASSERT_EQUAL_STRING("fck-c5", i.fw);
    TEST_ASSERT_EQUAL_STRING("0.1", i.ver);
    TEST_ASSERT_TRUE(i.has_version);
    TEST_ASSERT_EQUAL_UINT8(0, i.ver_major);
    TEST_ASSERT_EQUAL_UINT8(1, i.ver_minor);
    TEST_ASSERT_EQUAL_UINT8(0, i.ver_patch);
    TEST_ASSERT_EQUAL_STRING("EU", i.reg);
    TEST_ASSERT_EQUAL_STRING("ok", i.lim);
    TEST_ASSERT_TRUE(i.has_caps);
    TEST_ASSERT_EQUAL_HEX16(FCK_DRV_C5_CAP_SCAN | FCK_DRV_C5_CAP_CHAN | FCK_DRV_C5_CAP_BLE
                            | FCK_DRV_C5_CAP_GNSS | FCK_DRV_C5_CAP_WP | FCK_DRV_C5_CAP_GTX
                            | FCK_DRV_C5_CAP_HEALTH, i.caps);
    for (size_t k = 0; k < EVERY_FEATURE; k++) {
        TEST_ASSERT_TRUE_MESSAGE(fck_drv_c5_has(&i, k_every_feature[k]), "a 0.1 firmware was refused a feature");
    }

    /* What reaches it: its vocabulary, `scan` included, since it keeps the limits. */
    fck_drv_c5_command_t c;
    TEST_ASSERT_EQUAL(FCK_DRV_C5_GATE_OFFICIAL, fck_drv_c5_gate(&i, "scan all", NULL, &c));
    TEST_ASSERT_TRUE(c.emits);
    TEST_ASSERT_EQUAL(FCK_DRV_C5_GATE_OFFICIAL, fck_drv_c5_gate(&i, "chan 5g 250", NULL, &c));
    TEST_ASSERT_EQUAL(FCK_DRV_C5_GATE_OFFICIAL, fck_drv_c5_gate(&i, "ble scan 5", NULL, &c));
    TEST_ASSERT_EQUAL(FCK_DRV_C5_GATE_REFUSE, fck_drv_c5_gate(&i, "ble raw 5", NULL, &c));
}

TEST_CASE("with caps= the version decides nothing: 0.1, 9.9, none or a word, the same answers", "[c5]")
{
    static const char *const vers[] = { "ver=0.1 ", "ver=0.0 ", "ver=9.9.9 ", "ver=1.0 ", "ver=beta ", "" };
    for (size_t v = 0; v < sizeof(vers) / sizeof(vers[0]); v++) {
        char g[FCK_DRV_LINK_LINE_MAX + 1];
        snprintf(g, sizeof(g), "#C5INFO fw=fck-c5 %sreg=EU lim=ok caps=scan,chan,ble,gnss,wp,gtx,health", vers[v]);
        fck_drv_c5_info_t i;
        TEST_ASSERT_TRUE(fck_drv_c5_parse_info(line(g), &i));
        for (size_t k = 0; k < EVERY_FEATURE; k++) {
            TEST_ASSERT_TRUE_MESSAGE(fck_drv_c5_has(&i, k_every_feature[k]), g);
        }
        /* ... and a firmware with caps= and nothing in it has nothing, whatever its number. */
        snprintf(g, sizeof(g), "#C5INFO fw=fck-c5 %scaps=", vers[v]);
        TEST_ASSERT_TRUE(fck_drv_c5_parse_info(line(g), &i));
        TEST_ASSERT_TRUE(i.has_caps);
        TEST_ASSERT_EQUAL_HEX16(0, i.caps);
        for (size_t k = 0; k < EVERY_FEATURE; k++) {
            TEST_ASSERT_FALSE_MESSAGE(fck_drv_c5_has(&i, k_every_feature[k]), g);
        }
    }
}

TEST_CASE("a firmware whose caps= lacks a word is not asked that command", "[c5]")
{
    static const struct {
        const char          *caps;
        fck_drv_c5_feature_t off[4];
        size_t               n_off;
    } rows[] = {
        { "chan,ble,gnss,wp,gtx,health", { FCK_DRV_C5_SCAN }, 1 },
        { "scan,ble,gnss,wp,gtx,health", { FCK_DRV_C5_CHANNEL }, 1 },
        { "scan,chan,gnss,wp,gtx,health", { FCK_DRV_C5_BLE_SCAN }, 1 },
        { "scan,chan,ble,gnss,gtx,health", { FCK_DRV_C5_WAKE_PIN }, 1 },
        { "scan,chan,ble,gnss,wp,health", { FCK_DRV_C5_GNSS_TRANSMIT }, 1 },
        { "scan,chan,ble,wp,gtx,health", { FCK_DRV_C5_GNSS_RELAY, FCK_DRV_C5_GNSS_REPEAT }, 2 },
        { "scan,chan,ble,gnss,wp,gtx", { FCK_DRV_C5_HEALTH }, 1 },
        /* A word that only begins like one, or holds one, is another word. */
        { "scanner,xscan,chan,ble,gnss,wp,gtx,health", { FCK_DRV_C5_SCAN }, 1 },
        { "scan,chan,ble,gnss2,wp,gtx,health", { FCK_DRV_C5_GNSS_RELAY, FCK_DRV_C5_GNSS_REPEAT }, 2 },
        /* A word this SDK does not know yet changes nothing. */
        { "scan,chan,ble,gnss,wp,gtx,health,future,another", { FCK_DRV_C5_SCAN }, 0 },
    };
    for (size_t r = 0; r < sizeof(rows) / sizeof(rows[0]); r++) {
        char g[FCK_DRV_LINK_LINE_MAX + 1];
        snprintf(g, sizeof(g), C5_010_HEAD "reg=EU lim=ok caps=%s" C5_010_TAIL, rows[r].caps);
        fck_drv_c5_info_t i;
        TEST_ASSERT_TRUE(fck_drv_c5_parse_info(line(g), &i));
        for (size_t k = 0; k < EVERY_FEATURE; k++) {
            bool off = false;
            for (size_t o = 0; o < rows[r].n_off; o++) {
                off = off || (rows[r].off[o] == k_every_feature[k]);
            }
            TEST_ASSERT_EQUAL_MESSAGE(!off, fck_drv_c5_has(&i, k_every_feature[k]), rows[r].caps);
        }
    }
}

TEST_CASE("0.2's caps= adds bleadv and frames: each is its own word, and a 0.1 has neither", "[c5]")
{
    static const struct {
        const char *caps;
        bool        adv;
        bool        frames;
    } rows[] = {
        { "scan,chan,ble,gnss,wp,gtx,health", false, false },                 /* 0.1.0 */
        { "scan,chan,ble,bleadv,frames,gnss,wp,gtx,health", true, true },     /* 0.2.0 */
        { "scan,chan,ble,bleadv,gnss,wp,gtx,health", true, false },
        { "scan,chan,ble,frames,gnss,wp,gtx,health", false, true },
        /* "ble" does not promise what each advertiser sent, and a word that holds one is another word. */
        { "ble,bleadvert,xframes,frames2", false, false },
        { "frames", false, true },
    };
    for (size_t r = 0; r < sizeof(rows) / sizeof(rows[0]); r++) {
        char g[FCK_DRV_LINK_LINE_MAX + 1];
        /* The version decides nothing with caps=: a 9.9 without the words has neither. */
        snprintf(g, sizeof(g), "#C5INFO fw=fck-c5 ver=9.9 reg=EU lim=ok caps=%s", rows[r].caps);
        fck_drv_c5_info_t i;
        TEST_ASSERT_TRUE(fck_drv_c5_parse_info(line(g), &i));
        TEST_ASSERT_EQUAL_MESSAGE(rows[r].adv, fck_drv_c5_has(&i, FCK_DRV_C5_BLE_ADVERTISEMENT), rows[r].caps);
        TEST_ASSERT_EQUAL_MESSAGE(rows[r].frames, fck_drv_c5_has(&i, FCK_DRV_C5_FRAMES), rows[r].caps);
        TEST_ASSERT_EQUAL_MESSAGE(rows[r].adv, (i.caps & FCK_DRV_C5_CAP_BLEADV) != 0U, rows[r].caps);
        TEST_ASSERT_EQUAL_MESSAGE(rows[r].frames, (i.caps & FCK_DRV_C5_CAP_FRAMES) != 0U, rows[r].caps);
    }
}

TEST_CASE("with caps= the limits are the firmware's own: reg= and lim= change nothing of what is sent", "[c5]")
{
    static const char *const not_kept[] = {
        "reg=EU lim=power", "reg=EU lim=country", "reg=EU lim=", "reg=EU", "lim=ok", "reg=US lim=ok",
        "reg=eu lim=ok", "reg=EU lim=OK", "",
    };
    fck_drv_c5_command_t c;
    for (size_t r = 0; r < sizeof(not_kept) / sizeof(not_kept[0]); r++) {
        char g[FCK_DRV_LINK_LINE_MAX + 1];
        snprintf(g, sizeof(g), C5_010_HEAD "%s caps=scan,chan,ble,gnss,wp,gtx,health" C5_010_TAIL, not_kept[r]);
        fck_drv_c5_info_t i;
        TEST_ASSERT_TRUE(fck_drv_c5_parse_info(line(g), &i));
        TEST_ASSERT_FALSE_MESSAGE(fck_drv_c5_is_unlimited(&i), not_kept[r]);
        TEST_ASSERT_TRUE_MESSAGE(fck_drv_c5_has(&i, FCK_DRV_C5_SCAN), "it has the command");
        /* The firmware refuses it itself (rc 1) while a limit is not in force. */
        TEST_ASSERT_EQUAL_MESSAGE(FCK_DRV_C5_GATE_OFFICIAL, fck_drv_c5_gate(&i, "scan 2g", NULL, &c), not_kept[r]);
        TEST_ASSERT_EQUAL_MESSAGE(FCK_DRV_C5_GATE_OFFICIAL, fck_drv_c5_gate(&i, "chan 2g", NULL, &c),
                                  "what only listens goes whatever the limits say");
    }
}

TEST_CASE("a version is three numbers from 0 up, and none is said apart from 0.0", "[c5]")
{
    fck_drv_c5_info_t i;
    TEST_ASSERT_TRUE(fck_drv_c5_parse_info(line("#C5INFO fw=fck-c5 ver=0.0"), &i));
    TEST_ASSERT_TRUE_MESSAGE(i.has_version, "0.0 is a version");
    TEST_ASSERT_EQUAL_UINT8(0, i.ver_major);
    TEST_ASSERT_TRUE(fck_drv_c5_parse_info(line("#C5INFO fw=fck-c5 ver=0.1.7"), &i));
    TEST_ASSERT_TRUE(i.has_version);
    TEST_ASSERT_EQUAL_UINT8(0, i.ver_major);
    TEST_ASSERT_EQUAL_UINT8(1, i.ver_minor);
    TEST_ASSERT_EQUAL_UINT8(7, i.ver_patch);
    static const char *const none[] = {
        "#C5INFO fw=fck-c5", "#C5INFO fw=fck-c5 ver=", "#C5INFO fw=fck-c5 ver=beta", "#C5INFO fw=fck-c5 ver=1",
        "#C5INFO fw=fck-c5 ver=1.", "#C5INFO fw=fck-c5 ver=1.2.3.4", "#C5INFO fw=fck-c5 ver=256.0",
        "#C5INFO fw=fck-c5 ver=0.1+3",
    };
    for (size_t r = 0; r < sizeof(none) / sizeof(none[0]); r++) {
        TEST_ASSERT_TRUE(fck_drv_c5_parse_info(line(none[r]), &i));
        TEST_ASSERT_FALSE_MESSAGE(i.has_version, none[r]);
        TEST_ASSERT_EQUAL_UINT8(0, i.ver_major);
        TEST_ASSERT_EQUAL_UINT8(0, i.ver_minor);
        TEST_ASSERT_EQUAL_UINT8(0, i.ver_patch);
        TEST_ASSERT_FALSE(i.has_caps);
    }
}

/* TEMPORARY: pre-production firmware (2.x). Remove when the production modules ship.
 * From here to the next section's cases marked so: the firmware of the first
 * modules, which prints no caps= and is asked by its version. */

TEST_CASE("without caps= a 0.x is asked nothing but the relay: no number below 1.0 had a command", "[c5]")
{
    fck_drv_c5_info_t i;
    TEST_ASSERT_TRUE(fck_drv_c5_parse_info(
        line("#C5INFO fw=fck-c5 ver=0.1 chip=esp32c5 rev=1.2 mac=aa:bb:cc:dd:ee:01 reg=EU lim=ok"), &i));
    TEST_ASSERT_FALSE(i.has_caps);
    for (size_t k = 0; k < EVERY_FEATURE; k++) {
        TEST_ASSERT_EQUAL(k_every_feature[k] == FCK_DRV_C5_GNSS_RELAY, fck_drv_c5_has(&i, k_every_feature[k]));
    }
    /* No version at all: the same. */
    TEST_ASSERT_TRUE(fck_drv_c5_parse_info(line("#C5INFO fw=c5_fck_agent reg=EU lim=ok"), &i));
    for (size_t k = 0; k < EVERY_FEATURE; k++) {
        TEST_ASSERT_EQUAL(k_every_feature[k] == FCK_DRV_C5_GNSS_RELAY, fck_drv_c5_has(&i, k_every_feature[k]));
    }
    /* 2.x never had `health`, whatever its number. */
    TEST_ASSERT_TRUE(fck_drv_c5_parse_info(line("#C5INFO fw=fck-c5 ver=2.3 reg=EU lim=ok"), &i));
    TEST_ASSERT_FALSE(fck_drv_c5_has(&i, FCK_DRV_C5_HEALTH));
    TEST_ASSERT_TRUE(fck_drv_c5_has(&i, FCK_DRV_C5_GNSS_RELAY));
    TEST_ASSERT_TRUE(!fck_drv_c5_is_unlimited(&i));
}

TEST_CASE("2.0's #C5INFO reads whole, and its limits count only with reg=EU and lim=ok", "[c5]")
{
    fck_drv_c5_info_t i;
    TEST_ASSERT_TRUE(fck_drv_c5_parse_info(
        line("#C5INFO fw=c5_fck_agent ver=2.0 chip=esp32c5 rev=1.2 "
             "mac=aa:bb:cc:dd:ee:01 reg=EU lim=ok"), &i));
    TEST_ASSERT_EQUAL_STRING("c5_fck_agent", i.fw);
    TEST_ASSERT_EQUAL_STRING("2.0", i.ver);
    TEST_ASSERT_EQUAL_UINT8(2, i.ver_major);
    TEST_ASSERT_EQUAL_UINT8(0, i.ver_minor);
    TEST_ASSERT_EQUAL_UINT8(0, i.ver_patch);
    TEST_ASSERT_EQUAL_STRING("esp32c5", i.chip);
    TEST_ASSERT_EQUAL_STRING("1.2", i.rev);
    const uint8_t mac[6] = { 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0x01 };
    TEST_ASSERT_EQUAL_HEX8_ARRAY(mac, i.mac, 6);
    TEST_ASSERT_EQUAL_STRING("EU", i.reg);
    TEST_ASSERT_EQUAL_STRING("ok", i.lim);

    TEST_ASSERT_TRUE(fck_drv_c5_has(&i, FCK_DRV_C5_SCAN));
    TEST_ASSERT_TRUE(fck_drv_c5_has(&i, FCK_DRV_C5_CHANNEL));
    TEST_ASSERT_TRUE(fck_drv_c5_has(&i, FCK_DRV_C5_BLE_SCAN));
    TEST_ASSERT_TRUE(fck_drv_c5_has(&i, FCK_DRV_C5_WAKE_PIN));
    TEST_ASSERT_TRUE(fck_drv_c5_has(&i, FCK_DRV_C5_GNSS_TRANSMIT));
    TEST_ASSERT_TRUE(fck_drv_c5_has(&i, FCK_DRV_C5_GNSS_REPEAT));
    TEST_ASSERT_TRUE(!fck_drv_c5_is_unlimited(&i));

    /* The Wi-Fi driver refused a limit: the firmware runs, outside the tests. */
    TEST_ASSERT_TRUE(fck_drv_c5_parse_info(
        line("#C5INFO fw=c5_fck_agent ver=2.0 chip=esp32c5 rev=1.2 "
             "mac=aa:bb:cc:dd:ee:01 reg=EU lim=power"), &i));
    TEST_ASSERT_EQUAL_STRING("power", i.lim);
    TEST_ASSERT_TRUE(fck_drv_c5_has(&i, FCK_DRV_C5_SCAN));
    TEST_ASSERT_FALSE(!fck_drv_c5_is_unlimited(&i));

    TEST_ASSERT_TRUE(fck_drv_c5_parse_info(
        line("#C5INFO fw=c5_fck_agent ver=2.0 lim=ok"), &i));
    TEST_ASSERT_FALSE_MESSAGE(!fck_drv_c5_is_unlimited(&i), "no reg= is no market");
}

TEST_CASE("1.5's #C5INFO, as a first module printed it, has no market and no limits", "[c5]")
{
    /* A first module (MAC made up). */
    fck_drv_c5_info_t i;
    TEST_ASSERT_TRUE(fck_drv_c5_parse_info(
        line("#C5INFO fw=c5_fck_agent ver=1.5 chip=esp32c5 rev=1.2 mac=aa:bb:cc:dd:ee:02"), &i));
    TEST_ASSERT_EQUAL_UINT8(1, i.ver_major);
    TEST_ASSERT_EQUAL_UINT8(5, i.ver_minor);
    TEST_ASSERT_EQUAL_STRING("", i.reg);
    TEST_ASSERT_EQUAL_STRING("", i.lim);
    TEST_ASSERT_TRUE(fck_drv_c5_has(&i, FCK_DRV_C5_GNSS_REPEAT));
    TEST_ASSERT_FALSE(!fck_drv_c5_is_unlimited(&i));

    TEST_ASSERT_TRUE(fck_drv_c5_parse_info(line("#C5INFO fw=c5_fck_agent ver=1.3"), &i));
    TEST_ASSERT_TRUE(fck_drv_c5_has(&i, FCK_DRV_C5_WAKE_PIN));
    TEST_ASSERT_FALSE_MESSAGE(fck_drv_c5_has(&i, FCK_DRV_C5_GNSS_TRANSMIT),
                              "1.3's gtx adds the checksum the receiver ignores");
    TEST_ASSERT_FALSE(fck_drv_c5_has(&i, FCK_DRV_C5_GNSS_REPEAT));

    TEST_ASSERT_TRUE(fck_drv_c5_parse_info(line("#C5INFO fw=c5_fck_agent ver=1.0"), &i));
    TEST_ASSERT_TRUE(fck_drv_c5_has(&i, FCK_DRV_C5_SCAN));
    TEST_ASSERT_FALSE(fck_drv_c5_has(&i, FCK_DRV_C5_CHANNEL));
    TEST_ASSERT_FALSE(fck_drv_c5_has(&i, FCK_DRV_C5_BLE_SCAN));
}

TEST_CASE("another firmware's #C5INFO is read, and that firmware is asked nothing", "[c5]")
{
    fck_drv_c5_info_t i;
    TEST_ASSERT_TRUE(fck_drv_c5_parse_info(line("#C5INFO fw=my_firmware ver=0.3"), &i));
    TEST_ASSERT_EQUAL_STRING("my_firmware", i.fw);
    TEST_ASSERT_FALSE(fck_drv_c5_has(&i, FCK_DRV_C5_SCAN));
    TEST_ASSERT_FALSE(fck_drv_c5_has(NULL, FCK_DRV_C5_SCAN));

    TEST_ASSERT_TRUE(fck_drv_c5_parse_info(line("#C5INFO fw=c5_fck_agent ver=2.0.1"), &i));
    TEST_ASSERT_EQUAL_UINT8(2, i.ver_major);
    TEST_ASSERT_EQUAL_UINT8(0, i.ver_minor);
    TEST_ASSERT_EQUAL_UINT8(1, i.ver_patch);

    /* A version that is not one reads as 0.0, which is asked nothing. */
    TEST_ASSERT_TRUE(fck_drv_c5_parse_info(line("#C5INFO fw=c5_fck_agent ver=beta"), &i));
    TEST_ASSERT_EQUAL_STRING("beta", i.ver);
    TEST_ASSERT_EQUAL_UINT8(0, i.ver_major);
    TEST_ASSERT_FALSE(fck_drv_c5_has(&i, FCK_DRV_C5_SCAN));
    TEST_ASSERT_TRUE(fck_drv_c5_parse_info(line("#C5INFO fw=c5_fck_agent ver=2."), &i));
    TEST_ASSERT_EQUAL_UINT8(0, i.ver_major);
    TEST_ASSERT_TRUE(fck_drv_c5_parse_info(line("#C5INFO fw=c5_fck_agent ver=1.2.3.4"), &i));
    TEST_ASSERT_EQUAL_UINT8(0, i.ver_major);

    memset(&i, 0x5A, sizeof(i));
    fck_drv_c5_info_t before = i;
    TEST_ASSERT_FALSE_MESSAGE(fck_drv_c5_parse_info(line("#C5INFO ver=2.0"), &i), "no fw=");
    TEST_ASSERT_FALSE(fck_drv_c5_parse_info(line("#NRFINFO fw=nrf_fck_agent ver=2.0"), &i));
    TEST_ASSERT_EQUAL_MEMORY(&before, &i, sizeof(i));
}

/* ---- What may reach the C5 ---------------------------------------------------- */

static fck_drv_c5_info_t info_of(const char *text)
{
    fck_drv_c5_info_t i;
    TEST_ASSERT_TRUE_MESSAGE(fck_drv_c5_parse_info(line(text), &i), text);
    return i;
}

TEST_CASE("what may reach the C5 depends on what it said it runs", "[c5]")
{
    fck_drv_c5_info_t const eu  = info_of("#C5INFO fw=c5_fck_agent ver=2.0 chip=esp32c5 rev=1.2 "
                                      "mac=aa:bb:cc:dd:ee:01 reg=EU lim=ok");
    fck_drv_c5_info_t const pwr = info_of("#C5INFO fw=c5_fck_agent ver=2.0 chip=esp32c5 rev=1.2 "
                                      "mac=aa:bb:cc:dd:ee:01 reg=EU lim=power");
    fck_drv_c5_info_t const old = info_of("#C5INFO fw=c5_fck_agent ver=1.5 chip=esp32c5 rev=1.2 "
                                      "mac=aa:bb:cc:dd:ee:02");
    fck_drv_c5_info_t const own = info_of("#C5INFO fw=my_firmware ver=0.3");
    fck_drv_c5_command_t        c;

    /* Nothing said: nothing goes, not even `info`. */
    TEST_ASSERT_EQUAL(FCK_DRV_C5_GATE_REFUSE, fck_drv_c5_gate(NULL, "info", NULL, &c));
    TEST_ASSERT_EQUAL(FCK_DRV_C5_GATE_REFUSE, fck_drv_c5_gate(NULL, "info", "info", &c));
    TEST_ASSERT_EQUAL(FCK_DRV_C5_GATE_REFUSE, fck_drv_c5_gate(&eu, NULL, NULL, &c));
    TEST_ASSERT_EQUAL(FCK_DRV_C5_GATE_REFUSE, fck_drv_c5_gate(&eu, "info", NULL, NULL));

    /* The firmware: its vocabulary, built as fck_drv_c5_command() builds it, with the
     * verb its #DONE carries and the time it takes. */
    TEST_ASSERT_EQUAL(FCK_DRV_C5_GATE_OFFICIAL, fck_drv_c5_gate(&eu, "info", NULL, &c));
    assert_cmd(&c, "info", "info", FCK_DRV_C5_COMMAND_MS);
    TEST_ASSERT_FALSE(c.emits);
    TEST_ASSERT_EQUAL(FCK_DRV_C5_GATE_OFFICIAL, fck_drv_c5_gate(&eu, "chan 5g 250", NULL, &c));
    assert_cmd(&c, "chan 5g 250", "chan", 25U * 350U + 8000U);
    TEST_ASSERT_EQUAL(FCK_DRV_C5_GATE_OFFICIAL, fck_drv_c5_gate(&eu, "ble scan 5", NULL, &c));
    assert_cmd(&c, "ble scan 5", "ble", 13000U);
    TEST_ASSERT_EQUAL(FCK_DRV_C5_GATE_OFFICIAL, fck_drv_c5_gate(&eu, "ble scan", "ble", &c));
    static const char *const listens[] = {
        "gnss on", "gnss off", "wp", "wp 1", "wp in", "gtx loop", "gtx -n POLCFGMSG,0,2,1",
        "gtx POLCFGMSG,0,2,1", "  info", "chan", "chan dwell_ms=300 2g", "gtx -n $POLCFGMSG,0,2,1",
    };
    for (size_t i = 0; i < sizeof(listens) / sizeof(listens[0]); i++) {
        TEST_ASSERT_EQUAL_MESSAGE(FCK_DRV_C5_GATE_OFFICIAL, fck_drv_c5_gate(&eu, listens[i], NULL, &c),
                                  listens[i]);
        TEST_ASSERT_FALSE_MESSAGE(c.emits, listens[i]);
    }

    /* What 1.6 added, `ble raw`, a usage line, a word the firmware does not
     * have, and a receiver moved off the baud rate the firmware follows. */
    static const char *const refused[] = {
        "ble raw 5", "ble", "ble  raw", "ble scanx", "sniff 10 5g", "pcap 10 ch=6",
        "deauth 10", "probe 10", "scanx", "rescan", "SCAN", "Info", "reboot", "restart", "help",
        "gtx -n POLCFGPRT,0,9600", "gtx POLCFGPRT,0,9600", "gtx $POLCFGPRT,0,9600",
        "gtx -n $POLCFGPRT,1,230400", "  gtx    -n   POLCFGPRT,0,2",
        "info\nscan", "scan\tall", "info\r", "\tinfo", "", "   ",
    };
    for (size_t i = 0; i < sizeof(refused) / sizeof(refused[0]); i++) {
        TEST_ASSERT_EQUAL_MESSAGE(FCK_DRV_C5_GATE_REFUSE, fck_drv_c5_gate(&eu, refused[i], NULL, &c),
                                  refused[i]);
    }
    TEST_ASSERT_EQUAL_MESSAGE(FCK_DRV_C5_GATE_REFUSE, fck_drv_c5_gate(&eu, "gtx -n POLCFGPRT,0,9600", "gtx", &c),
                              "a verb does not open what the vocabulary shuts");
    char text[FCK_DRV_C5_COMMAND_MAX + 2];
    memset(text, 'x', sizeof(text));
    text[FCK_DRV_C5_COMMAND_MAX + 1] = '\0';
    memcpy(text, "wp ", 3U);
    TEST_ASSERT_EQUAL_MESSAGE(FCK_DRV_C5_GATE_REFUSE, fck_drv_c5_gate(&eu, text, NULL, &c),
                              "the firmware would run the first 255 characters");

    /* What transmits: `scan`, in any words, only to a firmware that keeps the
     * radio limits itself. The rest of the vocabulary is for every version. */
    static const char *const sends[] = {
        "scan", "scan all", "scan 2g", "scan 5g", "  scan 5g", "scan nonsense",
    };
    for (size_t i = 0; i < sizeof(sends) / sizeof(sends[0]); i++) {
        TEST_ASSERT_EQUAL_MESSAGE(FCK_DRV_C5_GATE_OFFICIAL, fck_drv_c5_gate(&eu, sends[i], NULL, &c), sends[i]);
        TEST_ASSERT_TRUE_MESSAGE(c.emits, sends[i]);
        TEST_ASSERT_EQUAL_STRING("scan", c.verb);
        TEST_ASSERT_EQUAL_UINT32(FCK_DRV_C5_SCAN_MS, c.ms);
        TEST_ASSERT_EQUAL_MESSAGE(FCK_DRV_C5_GATE_REFUSE, fck_drv_c5_gate(&pwr, sends[i], NULL, &c),
                                  "a driver that refused a limit was let scan");
        TEST_ASSERT_EQUAL_MESSAGE(FCK_DRV_C5_GATE_REFUSE, fck_drv_c5_gate(&old, sends[i], NULL, &c),
                                  "a firmware 1.5, which scans both bands with no limit, was let scan");
        TEST_ASSERT_EQUAL_MESSAGE(FCK_DRV_C5_GATE_REFUSE, fck_drv_c5_gate(&old, sends[i], "scan", &c),
                                  "a verb let a 1.5 scan");
    }
    TEST_ASSERT_EQUAL(FCK_DRV_C5_GATE_OFFICIAL, fck_drv_c5_gate(&pwr, "chan 2g", NULL, &c));
    TEST_ASSERT_EQUAL(FCK_DRV_C5_GATE_OFFICIAL, fck_drv_c5_gate(&pwr, "ble scan 5", NULL, &c));
    TEST_ASSERT_EQUAL(FCK_DRV_C5_GATE_OFFICIAL, fck_drv_c5_gate(&old, "ble scan 5", NULL, &c));
    TEST_ASSERT_EQUAL(FCK_DRV_C5_GATE_OFFICIAL, fck_drv_c5_gate(&old, "info", NULL, &c));
    TEST_ASSERT_EQUAL(FCK_DRV_C5_GATE_OFFICIAL, fck_drv_c5_gate(&old, "gnss on", NULL, &c));
    TEST_ASSERT_EQUAL(FCK_DRV_C5_GATE_REFUSE, fck_drv_c5_gate(&old, "ble raw 5", NULL, &c));
    TEST_ASSERT_EQUAL(FCK_DRV_C5_GATE_REFUSE, fck_drv_c5_gate(&old, "deauth 10", NULL, &c));

    /* The verb: the one the command ends with, or none. */
    TEST_ASSERT_EQUAL(FCK_DRV_C5_GATE_OFFICIAL, fck_drv_c5_gate(&eu, "ble scan 5", "ble", &c));
    TEST_ASSERT_EQUAL(FCK_DRV_C5_GATE_OFFICIAL, fck_drv_c5_gate(&eu, "scan 2g", "scan", &c));
    TEST_ASSERT_EQUAL(FCK_DRV_C5_GATE_REFUSE, fck_drv_c5_gate(&eu, "ble scan 5", "ble_scan", &c));
    TEST_ASSERT_EQUAL(FCK_DRV_C5_GATE_REFUSE, fck_drv_c5_gate(&eu, "info", "scan", &c));
    TEST_ASSERT_EQUAL(FCK_DRV_C5_GATE_REFUSE, fck_drv_c5_gate(&eu, "info", "", &c));

    /* Another firmware: as written, with the caller's verb, or not at all.
     * Its words are its programmer's, `scan` and `deauth` included. */
    memset(&c, 0x5A, sizeof(c));
    fck_drv_c5_command_t const kept = c;
    TEST_ASSERT_EQUAL(FCK_DRV_C5_GATE_AS_IS, fck_drv_c5_gate(&own, "blink 3", "blink", &c));
    TEST_ASSERT_EQUAL(FCK_DRV_C5_GATE_AS_IS, fck_drv_c5_gate(&own, "scan 2g", "scan", &c));
    TEST_ASSERT_EQUAL(FCK_DRV_C5_GATE_AS_IS, fck_drv_c5_gate(&own, "deauth 10", "deauth", &c));
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(&kept, &c, sizeof(c), "c was written for another firmware");
    TEST_ASSERT_EQUAL(FCK_DRV_C5_GATE_REFUSE, fck_drv_c5_gate(&own, "blink 3", NULL, &c));
    TEST_ASSERT_EQUAL_MEMORY(&kept, &c, sizeof(c));
    /* To the letter: a name that only begins like the firmware's is another
     * firmware, and a refusal writes nothing either. */
    fck_drv_c5_info_t const lookalike = info_of("#C5INFO fw=c5_fck_agent2 ver=2.0 reg=EU lim=ok");
    TEST_ASSERT_EQUAL(FCK_DRV_C5_GATE_AS_IS, fck_drv_c5_gate(&lookalike, "scan", "scan", &c));
    TEST_ASSERT_EQUAL(FCK_DRV_C5_GATE_REFUSE, fck_drv_c5_gate(&lookalike, "scan", NULL, &c));
    TEST_ASSERT_EQUAL(FCK_DRV_C5_GATE_REFUSE, fck_drv_c5_gate(&eu, "ble raw 5", NULL, &c));
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(&kept, &c, sizeof(c), "a refusal wrote the command");
}

TEST_CASE("c5/ the firmware goes by fck-c5, and by c5_fck_agent up to 2.1", "[c5]")
{
    TEST_ASSERT_TRUE(fck_drv_c5_is_official("fck-c5"));
    TEST_ASSERT_TRUE(fck_drv_c5_is_official("c5_fck_agent"));
    TEST_ASSERT_FALSE(fck_drv_c5_is_official("fck-c52"));
    TEST_ASSERT_FALSE(fck_drv_c5_is_official("fck-nrf"));
    TEST_ASSERT_FALSE(fck_drv_c5_is_official(""));
    TEST_ASSERT_FALSE(fck_drv_c5_is_official(NULL));
    fck_drv_c5_info_t i;
    TEST_ASSERT_TRUE(fck_drv_c5_parse_info(
        line("#C5INFO fw=fck-c5 ver=2.2 chip=esp32c5 rev=1.2 mac=aa:bb:cc:dd:ee:01 reg=EU lim=ok"), &i));
    TEST_ASSERT_EQUAL_STRING("fck-c5", i.fw);
    TEST_ASSERT_TRUE(fck_drv_c5_has(&i, FCK_DRV_C5_SCAN));
    TEST_ASSERT_TRUE(!fck_drv_c5_is_unlimited(&i));
}

TEST_CASE("c5/ greets as another firmware, compatible and with no word listed yet", "[c5]")
{
    /* With the template's own format (fck_c5_hello.h), as its cmd_info()
     * prints it; the MAC is made up. */
    char g[FCK_DRV_LINK_LINE_MAX + 1];
    snprintf(g, sizeof(g), FCK_C5_HELLO_FMT, "c5_firmware", "0.1.0", 1, 0,
             0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0x03, "EU", "ok");
    fck_drv_c5_info_t i;
    TEST_ASSERT_TRUE(fck_drv_c5_parse_info(line(g), &i));
    TEST_ASSERT_EQUAL_STRING("c5_firmware", i.fw);
    TEST_ASSERT_EQUAL_STRING("0.1.0", i.ver);
    TEST_ASSERT_EQUAL_UINT8(0, i.ver_major);
    TEST_ASSERT_EQUAL_UINT8(1, i.ver_minor);
    TEST_ASSERT_EQUAL_UINT8(0, i.ver_patch);
    TEST_ASSERT_EQUAL_STRING("esp32c5", i.chip);
    TEST_ASSERT_EQUAL_STRING("1.0", i.rev);
    const uint8_t mac[6] = { 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0x03 };
    TEST_ASSERT_EQUAL_HEX8_ARRAY(mac, i.mac, 6);
    TEST_ASSERT_EQUAL_STRING("EU", i.reg);
    TEST_ASSERT_EQUAL_STRING("ok", i.lim);
    /* Its caps= is empty until its programmer writes a command: compatible, and asked nothing. */
    TEST_ASSERT_FALSE(fck_drv_c5_has(&i, FCK_DRV_C5_SCAN));
    TEST_ASSERT_TRUE(fck_drv_c5_is_compatible(&i));
    TEST_ASSERT_FALSE(fck_drv_c5_is_official(i.fw));
    TEST_ASSERT_FALSE(fck_drv_c5_is_unlimited(&i));

    /* With a limit its Wi-Fi driver refused, under the longest name the SDK keeps. */
    snprintf(g, sizeof(g), FCK_C5_HELLO_FMT, "a_firmware_of_23_chars_", "12.34.56",
             1, 2, 0, 0, 0, 0, 0, 0, "EU", "no11b");
    TEST_ASSERT_TRUE(fck_drv_c5_parse_info(line(g), &i));
    TEST_ASSERT_EQUAL_STRING("a_firmware_of_23_chars_", i.fw);
    TEST_ASSERT_EQUAL_UINT8(12, i.ver_major);
    TEST_ASSERT_EQUAL_UINT8(34, i.ver_minor);
    TEST_ASSERT_EQUAL_UINT8(56, i.ver_patch);
    TEST_ASSERT_EQUAL_STRING("EU", i.reg);
    TEST_ASSERT_EQUAL_STRING("no11b", i.lim);

    /* One character more and the greeting cannot be read: the 23 that
     * FCK_C5_FW_NAME's help asks for. */
    snprintf(g, sizeof(g), FCK_C5_HELLO_FMT, "a_firmware_of_24_chars__", "0.1.0",
             1, 0, 0, 0, 0, 0, 0, 0, "EU", "ok");
    TEST_ASSERT_FALSE(fck_drv_c5_parse_info(line(g), &i));
}

/* ---- Listings ---------------------------------------------------------------- */

TEST_CASE("an #AP reads its escaped SSID whole, bytes and all", "[c5]")
{
    fck_drv_c5_network_t ap;
    TEST_ASSERT_TRUE(fck_drv_c5_parse_network(
        line("#AP ssid=\"Kode \\\"lab\\\" \\\\ \\xC3\\xB1\" bssid=aa:bb:cc:dd:ee:03 "
             "ch=6 band=2g rssi=-61 auth=wpa2_psk"), &ap));
    TEST_ASSERT_EQUAL_STRING("Kode \"lab\" \\ \xC3\xB1", ap.ssid);
    TEST_ASSERT_EQUAL_UINT8(15, ap.ssid_len);
    const uint8_t bssid[6] = { 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0x03 };
    TEST_ASSERT_EQUAL_HEX8_ARRAY(bssid, ap.bssid, 6);
    TEST_ASSERT_EQUAL_UINT8(6, ap.ch);
    TEST_ASSERT_EQUAL(FCK_DRV_C5_BAND_2G, ap.band);
    TEST_ASSERT_EQUAL_INT8(-61, ap.rssi);
    TEST_ASSERT_EQUAL(FCK_DRV_C5_SECURITY_WPA2_PSK, ap.auth);
    TEST_ASSERT_EQUAL_STRING("wpa2_psk", fck_drv_c5_security_name(ap.auth));

    /* A first module's strongest (SSID and BSSID made up). */
    TEST_ASSERT_TRUE(fck_drv_c5_parse_network(
        line("#AP ssid=\"\" bssid=aa:bb:cc:dd:ee:04 ch=149 band=5g rssi=-45 "
             "auth=wpa_wpa2_psk"), &ap));
    TEST_ASSERT_EQUAL_STRING("", ap.ssid);
    TEST_ASSERT_EQUAL_UINT8(0, ap.ssid_len);
    TEST_ASSERT_EQUAL(FCK_DRV_C5_BAND_5G, ap.band);
    TEST_ASSERT_EQUAL(FCK_DRV_C5_SECURITY_WPA_WPA2_PSK, ap.auth);

    TEST_ASSERT_TRUE(fck_drv_c5_parse_network(
        line("#AP ssid=x bssid=aa:bb:cc:dd:ee:05 ch=1 band=2g rssi=-90 auth=wpa4_psk"), &ap));
    TEST_ASSERT_EQUAL_MESSAGE(FCK_DRV_C5_SECURITY_UNKNOWN, ap.auth, "a name from a newer firmware");
    TEST_ASSERT_TRUE(fck_drv_c5_parse_network(
        line("#AP ssid=x bssid=aa:bb:cc:dd:ee:05 ch=1 band=2g rssi=-90"), &ap));
    TEST_ASSERT_EQUAL(FCK_DRV_C5_SECURITY_UNKNOWN, ap.auth);
    TEST_ASSERT_EQUAL_STRING("unknown", fck_drv_c5_security_name((fck_drv_c5_security_t)99));

    memset(&ap, 0x5A, sizeof(ap));
    fck_drv_c5_network_t before = ap;
    TEST_ASSERT_FALSE(fck_drv_c5_parse_network(line("#AP ssid=x ch=1 band=2g rssi=-90"), &ap));
    TEST_ASSERT_FALSE(fck_drv_c5_parse_network(
        line("#AP ssid=x bssid=aa:bb:cc:dd:ee:05 ch=1 band=6g rssi=-90"), &ap));
    TEST_ASSERT_FALSE(fck_drv_c5_parse_network(
        line("#AP ssid=x bssid=aa:bb:cc:dd:ee:05 ch=0 band=2g rssi=-90"), &ap));
    TEST_ASSERT_FALSE(fck_drv_c5_parse_network(
        line("#BLE addr=aa:bb:cc:dd:ee:05 rssi=-90"), &ap));
    TEST_ASSERT_EQUAL_MEMORY(&before, &ap, sizeof(ap));
}

TEST_CASE("a #CHAN, busy or quiet, as a first module printed them", "[c5]")
{
    /* A first module: counts and RSSI only, nothing private. */
    fck_drv_c5_channel_t c;
    TEST_ASSERT_TRUE(fck_drv_c5_parse_channel(
        line("#CHAN ch=1 band=2g frames=23 mgmt=22 data=0 ctrl=1 rssi_max=-55 "
             "rssi_avg=-78 dwell_ms=250"), &c));
    TEST_ASSERT_EQUAL_UINT8(1, c.ch);
    TEST_ASSERT_EQUAL(FCK_DRV_C5_BAND_2G, c.band);
    TEST_ASSERT_EQUAL_UINT32(23, c.frames);
    TEST_ASSERT_EQUAL_UINT32(22, c.mgmt);
    TEST_ASSERT_EQUAL_UINT32(0, c.data);
    TEST_ASSERT_EQUAL_UINT32(1, c.ctrl);
    TEST_ASSERT_EQUAL_INT8(-55, c.rssi_max);
    TEST_ASSERT_EQUAL_INT8(-78, c.rssi_avg);
    TEST_ASSERT_EQUAL_UINT16(250, c.dwell_ms);

    TEST_ASSERT_TRUE(fck_drv_c5_parse_channel(
        line("#CHAN ch=100 band=5g frames=0 mgmt=0 data=0 ctrl=0 rssi_max=-127 "
             "rssi_avg=-127 dwell_ms=250"), &c));
    TEST_ASSERT_EQUAL(FCK_DRV_C5_BAND_5G, c.band);
    TEST_ASSERT_EQUAL_UINT32(0, c.frames);
    TEST_ASSERT_EQUAL_INT8(FCK_DRV_C5_RSSI_NONE, c.rssi_max);
    TEST_ASSERT_EQUAL_INT8(FCK_DRV_C5_RSSI_NONE, c.rssi_avg);

    TEST_ASSERT_FALSE(fck_drv_c5_parse_channel(
        line("#CHAN ch=1 band=2g mgmt=22 data=0 ctrl=1 rssi_max=-55 rssi_avg=-78"), &c));
    TEST_ASSERT_FALSE(fck_drv_c5_parse_channel(
        line("#CHAN ch=1 band=2g frames=-1 mgmt=22 data=0 ctrl=1 rssi_max=-55 rssi_avg=-78"), &c));
}

TEST_CASE("a #BLE: random or public, with a name or none", "[c5]")
{
    fck_drv_c5_ble_t b;
    TEST_ASSERT_TRUE(fck_drv_c5_parse_ble(
        line("#BLE addr=aa:bb:cc:dd:ee:06 type=rnd rssi=-70 name=\"\" adv=12"), &b));
    const uint8_t addr[6] = { 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0x06 };
    TEST_ASSERT_EQUAL_HEX8_ARRAY(addr, b.addr, 6);
    TEST_ASSERT_TRUE(b.random);
    TEST_ASSERT_EQUAL_INT8(-70, b.rssi);
    TEST_ASSERT_EQUAL_STRING("", b.name);
    TEST_ASSERT_EQUAL_UINT16(12, b.adv);

    TEST_ASSERT_TRUE(fck_drv_c5_parse_ble(
        line("#BLE addr=aa:bb:cc:dd:ee:07 type=pub rssi=-127 name=\"Kode \\\"Dot\\\"\" adv=3"), &b));
    TEST_ASSERT_FALSE(b.random);
    TEST_ASSERT_EQUAL_INT8(FCK_DRV_C5_RSSI_NONE, b.rssi);
    TEST_ASSERT_EQUAL_STRING("Kode \"Dot\"", b.name);
    TEST_ASSERT_EQUAL_UINT8(10, b.name_len);

    TEST_ASSERT_FALSE(fck_drv_c5_parse_ble(line("#BLE type=pub rssi=-60"), &b));
    TEST_ASSERT_FALSE(fck_drv_c5_parse_ble(line("#BLE addr=aa:bb:cc:dd:ee:07 type=pub"), &b));
}

/* What a tracker left alone sends, in shape: one AD structure of 30 bytes, manufacturer data (0xFF)
 * of company 0x004C. The 25 bytes after the two that follow the company are made up. */
#define ADV_TRACKER \
    "1eff4c001219" "00a1a2a3a4a5a6a7a8a9aaabacadaeafb0b1b2b3b4b5b6" "0102"

TEST_CASE("a #BLE of 2.2 says how it was sent, when it was heard and what it carried", "[c5]")
{
    fck_drv_c5_ble_t b;
    TEST_ASSERT_TRUE(fck_drv_c5_parse_ble(
        line("#BLE addr=c6:11:22:33:44:55 type=rnd rssi=-58 name=\"\" adv=3 evt=3 first=140 last=4130 data="
             ADV_TRACKER), &b));
    TEST_ASSERT_EQUAL(FCK_DRV_C5_BLE_EVENT_NOT_CONNECTABLE, b.event);
    TEST_ASSERT_EQUAL_UINT32(140, b.first_ms);
    TEST_ASSERT_EQUAL_UINT32(4130, b.last_ms);
    TEST_ASSERT_EQUAL_UINT8(31, b.data_len);
    const uint8_t head[6] = { 0x1e, 0xff, 0x4c, 0x00, 0x12, 0x19 };
    TEST_ASSERT_EQUAL_HEX8_ARRAY(head, b.data, 6);
    TEST_ASSERT_EQUAL_HEX8(0x01, b.data[29]);
    TEST_ASSERT_EQUAL_HEX8(0x02, b.data[30]);
    /* What was there before 2.2 reads as before. */
    TEST_ASSERT_TRUE(b.random);
    TEST_ASSERT_EQUAL_INT8(-58, b.rssi);
    TEST_ASSERT_EQUAL_UINT16(3, b.adv);

    /* The four kinds a passive scan hears, by the controller's number; upper case reads too. */
    TEST_ASSERT_TRUE(fck_drv_c5_parse_ble(
        line("#BLE addr=aa:bb:cc:dd:ee:10 type=pub rssi=-60 name=\"\" adv=1 evt=0 first=0 last=0 data=0201FF"), &b));
    TEST_ASSERT_EQUAL(FCK_DRV_C5_BLE_EVENT_CONNECTABLE, b.event);
    TEST_ASSERT_EQUAL_UINT8(3, b.data_len);
    TEST_ASSERT_EQUAL_HEX8(0xff, b.data[2]);
    TEST_ASSERT_TRUE(fck_drv_c5_parse_ble(line("#BLE addr=aa:bb:cc:dd:ee:10 rssi=-60 evt=1 first=9 last=9"), &b));
    TEST_ASSERT_EQUAL(FCK_DRV_C5_BLE_EVENT_DIRECTED, b.event);
    TEST_ASSERT_EQUAL_UINT8(0, b.data_len);      /* a directed advertisement carries none: no data= */
    TEST_ASSERT_TRUE(fck_drv_c5_parse_ble(line("#BLE addr=aa:bb:cc:dd:ee:10 rssi=-60 evt=2 first=9 last=9"), &b));
    TEST_ASSERT_EQUAL(FCK_DRV_C5_BLE_EVENT_SCANNABLE, b.event);
}

TEST_CASE("a #BLE of a firmware before 2.2 carries none of it", "[c5]")
{
    fck_drv_c5_ble_t b;
    memset(&b, 0x5A, sizeof(b));
    TEST_ASSERT_TRUE(fck_drv_c5_parse_ble(
        line("#BLE addr=aa:bb:cc:dd:ee:06 type=rnd rssi=-70 name=\"\" adv=12"), &b));
    TEST_ASSERT_EQUAL(FCK_DRV_C5_BLE_EVENT_UNKNOWN, b.event);
    TEST_ASSERT_EQUAL_UINT32(0, b.first_ms);
    TEST_ASSERT_EQUAL_UINT32(0, b.last_ms);
    TEST_ASSERT_EQUAL_UINT8(0, b.data_len);
}

TEST_CASE("a #BLE whose data= is not whole bytes of one advertisement is read without it", "[c5]")
{
    static const char *const bad[] = {
        "0201f",                                                               /* half a byte        */
        "0201zz",                                                              /* not hexadecimal    */
        "0x0201",                                                              /* a prefix           */
        "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f",    /* 32: one too many   */
    };
    for (size_t i = 0U; i < sizeof(bad) / sizeof(bad[0]); i++) {
        char text[160];
        snprintf(text, sizeof(text), "#BLE addr=aa:bb:cc:dd:ee:11 type=pub rssi=-61 name=\"\" adv=2 evt=0 first=5 last=6 data=%s",
                 bad[i]);
        fck_drv_c5_ble_t b;
        TEST_ASSERT_TRUE_MESSAGE(fck_drv_c5_parse_ble(line(text), &b), bad[i]);
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(0, b.data_len, bad[i]);
        TEST_ASSERT_EQUAL_INT8(-61, b.rssi);
        TEST_ASSERT_EQUAL_UINT32(6, b.last_ms);
    }
    /* A kind this SDK does not know - a scan response is never heard by a scan that only
     * listens - and a time that is not a count. */
    fck_drv_c5_ble_t b;
    TEST_ASSERT_TRUE(fck_drv_c5_parse_ble(line("#BLE addr=aa:bb:cc:dd:ee:11 rssi=-61 evt=4 first=-3 last=x"), &b));
    TEST_ASSERT_EQUAL(FCK_DRV_C5_BLE_EVENT_UNKNOWN, b.event);
    TEST_ASSERT_EQUAL_UINT32(0, b.first_ms);
    TEST_ASSERT_EQUAL_UINT32(0, b.last_ms);
    TEST_ASSERT_TRUE(fck_drv_c5_parse_ble(line("#BLE addr=aa:bb:cc:dd:ee:11 rssi=-61 evt=77"), &b));
    TEST_ASSERT_EQUAL(FCK_DRV_C5_BLE_EVENT_UNKNOWN, b.event);
    /* The key with nothing after it: no bytes. */
    TEST_ASSERT_TRUE(fck_drv_c5_parse_ble(line("#BLE addr=aa:bb:cc:dd:ee:11 rssi=-61 evt=0 first=5 last=6 data="), &b));
    TEST_ASSERT_EQUAL_UINT8(0, b.data_len);
    TEST_ASSERT_EQUAL(FCK_DRV_C5_BLE_EVENT_CONNECTABLE, b.event);
}

TEST_CASE("a device named like the keys of a #BLE does not fill them", "[c5]")
{
    /* A name is whatever its device says, and anybody near can say this one. */
    fck_drv_c5_ble_t b;
    TEST_ASSERT_TRUE(fck_drv_c5_parse_ble(
        line("#BLE addr=aa:bb:cc:dd:ee:11 type=rnd rssi=-61 name=\"a evt=3 last=9 data=0201\" adv=2"), &b));
    TEST_ASSERT_EQUAL_STRING("a evt=3 last=9 data=0201", b.name);
    TEST_ASSERT_EQUAL(FCK_DRV_C5_BLE_EVENT_UNKNOWN, b.event);
    TEST_ASSERT_EQUAL_UINT32(0, b.last_ms);
    TEST_ASSERT_EQUAL_UINT8(0, b.data_len);
    /* And with the firmware's own after it, the firmware's are the ones read. */
    TEST_ASSERT_TRUE(fck_drv_c5_parse_ble(
        line("#BLE addr=aa:bb:cc:dd:ee:11 type=rnd rssi=-61 name=\"a evt=3 last=9 data=0201\" adv=2 evt=0 first=4 last=7 "
             "data=03035afd"),
        &b));
    TEST_ASSERT_EQUAL(FCK_DRV_C5_BLE_EVENT_CONNECTABLE, b.event);
    TEST_ASSERT_EQUAL_UINT32(7, b.last_ms);
    TEST_ASSERT_EQUAL_UINT8(4, b.data_len);
    TEST_ASSERT_EQUAL_HEX8(0x5A, b.data[2]);
}

TEST_CASE("with no caps=, what each advertiser sent is promised from 2.4: 2.2 and 2.3 numbered two lines", "[c5]")
{
    fck_drv_c5_info_t i;
    /* One 2.2 printed the fields and another 2.2 and 2.3 did not: a version cannot tell, so neither is
     * promised them. What a #BLE carries is read by its keys all the same. */
    TEST_ASSERT_TRUE(fck_drv_c5_parse_info(line("#C5INFO fw=fck-c5 ver=2.2 chip=esp32c5 reg=EU lim=ok"), &i));
    TEST_ASSERT_FALSE(fck_drv_c5_has(&i, FCK_DRV_C5_BLE_ADVERTISEMENT));
    TEST_ASSERT_TRUE(fck_drv_c5_parse_info(line("#C5INFO fw=fck-c5 ver=2.3 chip=esp32c5 reg=EU lim=ok"), &i));
    TEST_ASSERT_FALSE(fck_drv_c5_has(&i, FCK_DRV_C5_BLE_ADVERTISEMENT));
    TEST_ASSERT_TRUE(fck_drv_c5_parse_info(line("#C5INFO fw=fck-c5 ver=2.4 chip=esp32c5 reg=EU lim=ok"), &i));
    TEST_ASSERT_TRUE(fck_drv_c5_has(&i, FCK_DRV_C5_BLE_ADVERTISEMENT));
    TEST_ASSERT_TRUE(fck_drv_c5_parse_info(line("#C5INFO fw=fck-c5 ver=3.0 chip=esp32c5 reg=EU lim=ok"), &i));
    TEST_ASSERT_TRUE(fck_drv_c5_has(&i, FCK_DRV_C5_BLE_ADVERTISEMENT));
    TEST_ASSERT_TRUE(fck_drv_c5_parse_info(line("#C5INFO fw=fck-c5 ver=2.1 chip=esp32c5 reg=EU lim=ok"), &i));
    TEST_ASSERT_FALSE(fck_drv_c5_has(&i, FCK_DRV_C5_BLE_ADVERTISEMENT));
    TEST_ASSERT_TRUE(fck_drv_c5_has(&i, FCK_DRV_C5_BLE_SCAN));
    TEST_ASSERT_TRUE(fck_drv_c5_parse_info(line("#C5INFO fw=my_firmware ver=9.9"), &i));
    TEST_ASSERT_FALSE(fck_drv_c5_has(&i, FCK_DRV_C5_BLE_ADVERTISEMENT));
}

/* ---- `frames`, from 2.4 ------------------------------------------------------- */

/* The four records of the contract, as the firmware prints them (`#F ` and the body). */
#define F_BEACON    "gAAGzLQA////////qrvM3e4BqrvM3e4BAQIDBAUGBwhkABEEAARLb2Rl"
#define F_PROBE     "QAAGujwA////////AhEiM0RV////////AAA="
#define F_DEAUTH    "wAALwx4AAhEiM0RVqrvM3e4BqrvM3e4BBwA="
#define F_DATA      "iAEk0AQGqrvM3e4BAhEiM0RVCgsMDQ4P"

static fck_drv_c5_frame_t frame_of(const char *body)
{
    char text[160];
    snprintf(text, sizeof(text), "#F %s", body);
    fck_drv_c5_frame_t f;
    memset(&f, 0x5A, sizeof(f));
    TEST_ASSERT_TRUE_MESSAGE(fck_drv_c5_parse_frame(line(text), &f), body);
    return f;
}

TEST_CASE("frames is the firmware's `frames`, ended by its own verb, in its time and the UART's slack", "[c5][frames]")
{
    fck_drv_c5_command_t c;
    TEST_ASSERT_EQUAL(KODE_OK, fck_drv_c5_command_frames(&c, 6, 250));
    assert_cmd(&c, "frames ch=6 ms=250", "frames", 250U + FCK_DRV_C5_FRAMES_SLACK_MS);
    TEST_ASSERT_FALSE_MESSAGE(c.emits, "it never transmits");
    TEST_ASSERT_EQUAL(KODE_OK, fck_drv_c5_command_frames(&c, 177, 5000));
    assert_cmd(&c, "frames ch=177 ms=5000", "frames", 8000U);
    TEST_ASSERT_EQUAL(KODE_OK, fck_drv_c5_command_frames(&c, 1, 20));
    assert_cmd(&c, "frames ch=1 ms=20", "frames", 3020U);
    TEST_ASSERT_FALSE(c.emits);

    /* Written by hand: the time is the same, and a default ms is the firmware's 250. */
    TEST_ASSERT_EQUAL(KODE_OK, fck_drv_c5_command(&c, "frames ms=1000 ch=36"));
    assert_cmd(&c, "frames ms=1000 ch=36", "frames", 4000U);
    TEST_ASSERT_EQUAL(KODE_OK, fck_drv_c5_command(&c, "frames ch=36"));
    assert_cmd(&c, "frames ch=36", "frames", 250U + FCK_DRV_C5_FRAMES_SLACK_MS);
    TEST_ASSERT_EQUAL(KODE_OK, fck_drv_c5_command(&c, "frames ch=36 ms=999999"));
    assert_cmd(&c, "frames ch=36 ms=999999", "frames", 5000U + FCK_DRV_C5_FRAMES_SLACK_MS);
    TEST_ASSERT_EQUAL(KODE_OK, fck_drv_c5_command(&c, "frames ch=36 700"));
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(250U + FCK_DRV_C5_FRAMES_SLACK_MS, c.ms, "a bare number is no ms=");
    TEST_ASSERT_FALSE(c.emits);
}

TEST_CASE("frames takes channels 1 to 177 and 20 to 5000 ms, and nothing else", "[c5][frames]")
{
    fck_drv_c5_command_t c;
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_drv_c5_command_frames(&c, 0, 250));
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_drv_c5_command_frames(&c, 178, 250));
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_drv_c5_command_frames(&c, 255, 250));
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_drv_c5_command_frames(&c, 6, 19));
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_drv_c5_command_frames(&c, 6, 5001));
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_drv_c5_command_frames(&c, 6, 0));
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_drv_c5_command_frames(NULL, 6, 250));
}

TEST_CASE("a #F of a beacon reads field by field, with its name", "[c5][frames]")
{
    fck_drv_c5_frame_t f = frame_of(F_BEACON);
    TEST_ASSERT_EQUAL_HEX8(0x80, f.fc[0]);
    TEST_ASSERT_EQUAL_HEX8(0x00, f.fc[1]);
    TEST_ASSERT_EQUAL_UINT8(6, f.channel);
    TEST_ASSERT_EQUAL_INT8(-52, f.rssi);
    TEST_ASSERT_EQUAL_UINT16(180, f.len);
    const uint8_t all[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
    const uint8_t sender[6] = { 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0x01 };
    TEST_ASSERT_EQUAL_HEX8_ARRAY(all, f.addr1, 6);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(sender, f.addr2, 6);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(sender, f.addr3, 6);
    /* 8 bytes of timestamp, interval 100 and capabilities 0x0411, then the tag 0 of length 4. */
    TEST_ASSERT_EQUAL_UINT8(18, f.head_len);
    const uint8_t head[18] = { 1, 2, 3, 4, 5, 6, 7, 8, 0x64, 0x00, 0x11, 0x04, 0x00, 0x04, 'K', 'o', 'd', 'e' };
    TEST_ASSERT_EQUAL_HEX8_ARRAY(head, f.head, 18);
    for (size_t i = 18U; i < sizeof(f.head); i++) {
        TEST_ASSERT_EQUAL_HEX8_MESSAGE(0, f.head[i], "zeros past head_len");
    }
}

TEST_CASE("a #F of a wildcard probe request has the name's empty tag, and a deauthentication has its reason", "[c5][frames]")
{
    fck_drv_c5_frame_t f = frame_of(F_PROBE);
    TEST_ASSERT_EQUAL_HEX8(0x40, f.fc[0]);
    TEST_ASSERT_EQUAL_UINT8(6, f.channel);
    TEST_ASSERT_EQUAL_INT8(-70, f.rssi);
    TEST_ASSERT_EQUAL_UINT16(60, f.len);
    const uint8_t all[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
    const uint8_t station[6] = { 0x02, 0x11, 0x22, 0x33, 0x44, 0x55 };
    TEST_ASSERT_EQUAL_HEX8_ARRAY(all, f.addr1, 6);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(station, f.addr2, 6);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(all, f.addr3, 6);
    /* The name's tag: id 0, length 0 - the wildcard. */
    TEST_ASSERT_EQUAL_UINT8(2, f.head_len);
    for (size_t i = 0U; i < sizeof(f.head); i++) {
        TEST_ASSERT_EQUAL_HEX8(0, f.head[i]);
    }

    f = frame_of(F_DEAUTH);
    TEST_ASSERT_EQUAL_HEX8(0xc0, f.fc[0]);
    TEST_ASSERT_EQUAL_UINT8(11, f.channel);
    TEST_ASSERT_EQUAL_INT8(-61, f.rssi);
    TEST_ASSERT_EQUAL_UINT16(30, f.len);
    const uint8_t sender[6] = { 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0x01 };
    TEST_ASSERT_EQUAL_HEX8_ARRAY(station, f.addr1, 6);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(sender, f.addr2, 6);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(sender, f.addr3, 6);
    TEST_ASSERT_EQUAL_UINT8(2, f.head_len);
    TEST_ASSERT_EQUAL_HEX8(0x07, f.head[0]);
    TEST_ASSERT_EQUAL_HEX8(0x00, f.head[1]);
    TEST_ASSERT_EQUAL_HEX8(0x00, f.head[2]);
}

TEST_CASE("a #F of a data frame on 5 GHz: its length passes 255 and its payload is not there", "[c5][frames]")
{
    fck_drv_c5_frame_t f = frame_of(F_DATA);
    TEST_ASSERT_EQUAL_HEX8(0x88, f.fc[0]);
    TEST_ASSERT_EQUAL_HEX8(0x01, f.fc[1]);
    TEST_ASSERT_EQUAL_UINT8(36, f.channel);
    TEST_ASSERT_EQUAL_INT8(-48, f.rssi);
    TEST_ASSERT_EQUAL_UINT16(1540, f.len);
    const uint8_t bssid[6]   = { 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0x01 };
    const uint8_t station[6] = { 0x02, 0x11, 0x22, 0x33, 0x44, 0x55 };
    const uint8_t third[6]   = { 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f };
    TEST_ASSERT_EQUAL_HEX8_ARRAY(bssid, f.addr1, 6);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(station, f.addr2, 6);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(third, f.addr3, 6);
    TEST_ASSERT_EQUAL_UINT8(0, f.head_len);
}

TEST_CASE("a #F is read whole or not at all: bad characters, padding, groups", "[c5][frames]")
{
    static const char *const bad[] = {
        "",                                                     /* nothing                          */
        "gAAGzLQA////////qrvM3e4BqrvM3e4BAQIDBAUGBwhkABEEAARLb2R", /* not a multiple of 4          */
        "gAAGzLQA////////qrvM3e4BqrvM3e4BAQIDBAUGBwhkABEEAARLb2=l", /* padding, then a character  */
        "iAEk0AQGqrvM3e4BAhEiM0RVCgsMDQ4=",                     /* 23 bytes: too short, well padded */
        "iAEk0AQG qrvM3e4BAhEiM0RVCgsMDQ4P",                    /* a space                          */
        "iAEk0AQG\xc3\xa9rvM3e4BAhEiM0RVCgsMDQ4P",              /* a byte over 127                  */
        "iAEk0AQGqrvM3e4BAhEiM0RVCgsM-Q4P",                     /* the URL alphabet's character     */
        "iAEk0AQGqrvM3e4BAhEiM0RVCgsM_Q4P",
        "iAEk0AQGqrvM3e4BAhEiM0RVCgsMDQ4P====",                 /* a group of only padding          */
        "iAEk0AQGqrvM3e4BAhEiM0RVCgsMDQ4P=",                    /* five characters over a group     */
        "iAEk0AQGqrvM3e4BAhEiM0RVCg==DQ4P",                     /* padding in the middle            */
        "=iEk0AQGqrvM3e4BAhEiM0RVCgsMDQ4P",                     /* padding first                    */
        "iAEk0AQGqrvM3e4BAhEiM0RVCgsMD=4P",                     /* padding then a character         */
        "iAEk0AQGqrvM3e4BAhEiM0RVCgsMDQ=P",
    };
    fck_drv_c5_frame_t f;
    memset(&f, 0x5A, sizeof(f));
    for (size_t i = 0U; i < sizeof(bad) / sizeof(bad[0]); i++) {
        char text[200];
        snprintf(text, sizeof(text), "#F %s", bad[i]);
        TEST_ASSERT_FALSE_MESSAGE(fck_drv_c5_parse_frame(line(text), &f), bad[i]);
    }
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x5A, f.fc[0], "a refused line wrote nothing");
    TEST_ASSERT_EQUAL_UINT8(0x5A, f.head_len);
    /* The tag alone, another tag, and no output. */
    TEST_ASSERT_FALSE(fck_drv_c5_parse_frame(line("#F"), &f));
    TEST_ASSERT_FALSE(fck_drv_c5_parse_frame(line("#FRAMES " F_DATA), &f));
    TEST_ASSERT_FALSE(fck_drv_c5_parse_frame(line("#BLE " F_DATA), &f));
    TEST_ASSERT_FALSE(fck_drv_c5_parse_frame(line("#F " F_DATA), NULL));
    TEST_ASSERT_FALSE(fck_drv_c5_parse_frame(NULL, &f));
}

TEST_CASE("a #F with bits past its data that are not zero is not a whole record", "[c5][frames]")
{
    /* F_PROBE ends in "AAA=": its last character holds two spare bits. "AAB=" sets one. */
    fck_drv_c5_frame_t f;
    TEST_ASSERT_FALSE(fck_drv_c5_parse_frame(line("#F QAAGujwA////////AhEiM0RV////////AAB="), &f));
    /* And "AA==" ending a group of one byte: 25 bytes, with the spare bits of its second character. */
    TEST_ASSERT_TRUE(fck_drv_c5_parse_frame(line("#F iAEk0AQGqrvM3e4BAhEiM0RVCgsMDQ4PAA=="), &f));
    TEST_ASSERT_EQUAL_UINT8(1, f.head_len);
    TEST_ASSERT_FALSE(fck_drv_c5_parse_frame(line("#F iAEk0AQGqrvM3e4BAhEiM0RVCgsMDQ4PAB=="), &f));
}

TEST_CASE("a #F record of 23 bytes is too short, 24 and 70 are the ends, 71 and over are too long", "[c5][frames]")
{
    /* Records of 23..72 bytes of counting values, made here, encoded by base64 below. */
    static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    for (size_t n = 23U; n <= 72U; n++) {
        uint8_t rec[72];
        for (size_t i = 0U; i < n; i++) {
            rec[i] = (uint8_t)(0x10U + i);
        }
        char   text[160] = "#F ";
        size_t at        = 3U;
        for (size_t i = 0U; i < n; i += 3U) {
            uint32_t const b = ((uint32_t)rec[i] << 16) | ((i + 1U < n) ? ((uint32_t)rec[i + 1U] << 8) : 0U)
                               | ((i + 2U < n) ? rec[i + 2U] : 0U);
            text[at++] = alphabet[(b >> 18) & 63U];
            text[at++] = alphabet[(b >> 12) & 63U];
            text[at++] = (i + 1U < n) ? alphabet[(b >> 6) & 63U] : '=';
            text[at++] = (i + 2U < n) ? alphabet[b & 63U] : '=';
        }
        text[at] = '\0';
        fck_drv_c5_frame_t f;
        bool const         ok = fck_drv_c5_parse_frame(line(text), &f);
        char               why[48];
        snprintf(why, sizeof(why), "a record of %u bytes", (unsigned)n);
        TEST_ASSERT_EQUAL_MESSAGE(n >= 24U && n <= 70U, ok, why);
        if (ok) {
            TEST_ASSERT_EQUAL_UINT8_MESSAGE((uint8_t)(n - 24U), f.head_len, why);
            TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x10, f.fc[0], why);
            TEST_ASSERT_EQUAL_UINT16_MESSAGE(0x1514U, f.len, why);
            if (n == 70U) {
                TEST_ASSERT_EQUAL_HEX8(0x10U + 69U, f.head[45]);
            }
        }
    }
    /* A body longer than the longest record's 96 characters is refused before it is read. */
    char over[160] = "#F ";
    memset(over + 3, 'A', 100U);
    over[103] = '\0';
    fck_drv_c5_frame_t f;
    TEST_ASSERT_FALSE(fck_drv_c5_parse_frame(line(over), &f));
}

TEST_CASE("a #FRAMES gives the channel, the time and every count", "[c5][frames]")
{
    fck_drv_c5_frames_summary_t s;
    memset(&s, 0x5A, sizeof(s));
    TEST_ASSERT_TRUE(fck_drv_c5_parse_frames_summary(
        line("#FRAMES ch=6 ms=251 heard=412 mgmt=300 data=100 ctrl=12 beacon=40 probe_req=22 probe_resp=18 deauth=3 "
             "disassoc=1 sent=57 dropped=2"),
        &s));
    TEST_ASSERT_EQUAL_UINT8(6, s.ch);
    TEST_ASSERT_EQUAL_UINT32(251, s.ms);
    TEST_ASSERT_EQUAL_UINT32(412, s.heard);
    TEST_ASSERT_EQUAL_UINT32(300, s.mgmt);
    TEST_ASSERT_EQUAL_UINT32(100, s.data);
    TEST_ASSERT_EQUAL_UINT32(12, s.ctrl);
    TEST_ASSERT_EQUAL_UINT32(40, s.beacon);
    TEST_ASSERT_EQUAL_UINT32(22, s.probe_request);
    TEST_ASSERT_EQUAL_UINT32(18, s.probe_response);
    TEST_ASSERT_EQUAL_UINT32(3, s.deauth);
    TEST_ASSERT_EQUAL_UINT32(1, s.disassoc);
    TEST_ASSERT_EQUAL_UINT32(57, s.sent);
    TEST_ASSERT_EQUAL_UINT32(2, s.dropped);
    /* A key a firmware adds later breaks nothing; a key it lacks does. */
    TEST_ASSERT_TRUE(fck_drv_c5_parse_frames_summary(
        line("#FRAMES ch=36 ms=5003 heard=0 mgmt=0 data=0 ctrl=0 beacon=0 probe_req=0 probe_resp=0 deauth=0 disassoc=0 "
             "sent=0 dropped=0 more=9"),
        &s));
    TEST_ASSERT_EQUAL_UINT8(36, s.ch);
    TEST_ASSERT_EQUAL_UINT32(5003, s.ms);
}

TEST_CASE("a #FRAMES missing a key, with a bad one, or another tag, is not read", "[c5][frames]")
{
    static const char *const bad[] = {
        "#FRAMES ch=6 ms=251 heard=412 mgmt=300 data=100 ctrl=12 beacon=40 probe_req=22 probe_resp=18 deauth=3 disassoc=1 sent=57",
        "#FRAMES ms=251 heard=412 mgmt=300 data=100 ctrl=12 beacon=40 probe_req=22 probe_resp=18 deauth=3 disassoc=1 sent=57 dropped=2",
        "#FRAMES ch=0 ms=251 heard=412 mgmt=300 data=100 ctrl=12 beacon=40 probe_req=22 probe_resp=18 deauth=3 disassoc=1 sent=57 dropped=2",
        "#FRAMES ch=6 ms=251 heard=-1 mgmt=300 data=100 ctrl=12 beacon=40 probe_req=22 probe_resp=18 deauth=3 disassoc=1 sent=57 dropped=2",
        /* probe_req is not probe_resp's prefix to the reader: it needs its own key. */
        "#FRAMES ch=6 ms=251 heard=412 mgmt=300 data=100 ctrl=12 beacon=40 probe_resp=18 deauth=3 disassoc=1 sent=57 dropped=2",
        "#CHAN ch=6 ms=251 heard=412 mgmt=300 data=100 ctrl=12 beacon=40 probe_req=22 probe_resp=18 deauth=3 disassoc=1 sent=57 dropped=2",
    };
    fck_drv_c5_frames_summary_t s;
    memset(&s, 0x5A, sizeof(s));
    for (size_t i = 0U; i < sizeof(bad) / sizeof(bad[0]); i++) {
        TEST_ASSERT_FALSE_MESSAGE(fck_drv_c5_parse_frames_summary(line(bad[i]), &s), bad[i]);
    }
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(0x5A, s.ch, "a refused line wrote nothing");
    TEST_ASSERT_FALSE(fck_drv_c5_parse_frames_summary(NULL, &s));
}

TEST_CASE("with no caps=, frames come from 2.4, and 2.3 has none, nor has a firmware of someone's own", "[c5][frames]")
{
    fck_drv_c5_info_t i;
    TEST_ASSERT_TRUE(fck_drv_c5_parse_info(line("#C5INFO fw=fck-c5 ver=2.2 chip=esp32c5 reg=EU lim=ok"), &i));
    TEST_ASSERT_FALSE(fck_drv_c5_has(&i, FCK_DRV_C5_FRAMES));
    TEST_ASSERT_TRUE(fck_drv_c5_parse_info(line("#C5INFO fw=fck-c5 ver=2.3 chip=esp32c5 reg=EU lim=ok"), &i));
    TEST_ASSERT_FALSE_MESSAGE(fck_drv_c5_has(&i, FCK_DRV_C5_FRAMES), "2.3 is another branch's: it has no `frames`");
    TEST_ASSERT_TRUE(fck_drv_c5_parse_info(line("#C5INFO fw=fck-c5 ver=2.3.9 chip=esp32c5 reg=EU lim=ok"), &i));
    TEST_ASSERT_FALSE(fck_drv_c5_has(&i, FCK_DRV_C5_FRAMES));
    TEST_ASSERT_TRUE(fck_drv_c5_parse_info(line("#C5INFO fw=fck-c5 ver=2.4 chip=esp32c5 reg=EU lim=ok"), &i));
    TEST_ASSERT_TRUE(fck_drv_c5_has(&i, FCK_DRV_C5_FRAMES));
    TEST_ASSERT_TRUE(fck_drv_c5_parse_info(line("#C5INFO fw=fck-c5 ver=2.4.1 chip=esp32c5"), &i));
    TEST_ASSERT_TRUE_MESSAGE(fck_drv_c5_has(&i, FCK_DRV_C5_FRAMES), "it listens: no EU limits needed");
    TEST_ASSERT_TRUE(fck_drv_c5_parse_info(line("#C5INFO fw=fck-c5 ver=3.0 chip=esp32c5 reg=EU lim=ok"), &i));
    TEST_ASSERT_TRUE(fck_drv_c5_has(&i, FCK_DRV_C5_FRAMES));
    TEST_ASSERT_TRUE(fck_drv_c5_parse_info(line("#C5INFO fw=c5_fck_agent ver=2.0 chip=esp32c5 reg=EU lim=ok"), &i));
    TEST_ASSERT_FALSE(fck_drv_c5_has(&i, FCK_DRV_C5_FRAMES));
    TEST_ASSERT_TRUE(fck_drv_c5_parse_info(line("#C5INFO fw=my_firmware ver=9.9"), &i));
    TEST_ASSERT_FALSE(fck_drv_c5_has(&i, FCK_DRV_C5_FRAMES));
    TEST_ASSERT_FALSE(fck_drv_c5_has(NULL, FCK_DRV_C5_FRAMES));
}

TEST_CASE("an app's own line for `frames` does not reach the C5 through the gate", "[c5][frames]")
{
    /* The typed call builds its own command; the gate's vocabulary is the words an app may write by hand, and `frames` is not one. */
    fck_drv_c5_info_t eu;
    TEST_ASSERT_TRUE(fck_drv_c5_parse_info(line("#C5INFO fw=fck-c5 ver=2.4 chip=esp32c5 reg=EU lim=ok"), &eu));
    fck_drv_c5_command_t c;
    TEST_ASSERT_EQUAL(FCK_DRV_C5_GATE_REFUSE, fck_drv_c5_gate(&eu, "frames ch=6 ms=250", NULL, &c));
}

TEST_CASE("#SCANDONE, #CHANDONE and #BLEDONE give n and ms, and a #DONE does not", "[c5]")
{
    /* A first module. */
    fck_drv_c5_summary_t s;
    TEST_ASSERT_TRUE(fck_drv_c5_parse_summary(line("#SCANDONE n=31 ms=8399"), &s));
    TEST_ASSERT_EQUAL_UINT32(31, s.n);
    TEST_ASSERT_EQUAL_UINT32(8399, s.ms);
    TEST_ASSERT_TRUE(fck_drv_c5_parse_summary(line("#CHANDONE n=37 ms=9989"), &s));
    TEST_ASSERT_EQUAL_UINT32(37, s.n);
    TEST_ASSERT_TRUE(fck_drv_c5_parse_summary(line("#BLEDONE n=46 ms=7013"), &s));
    TEST_ASSERT_EQUAL_UINT32(7013, s.ms);
    /* The link keeps #DONE for itself, so this one is built by hand. */
    const fck_drv_link_message_t done = { .tag = "DONE", .body = "scan rc=0" };
    TEST_ASSERT_FALSE(fck_drv_c5_parse_summary(&done, &s));
    TEST_ASSERT_FALSE(fck_drv_c5_parse_summary(line("#SCANDONE n=31"), &s));
}

/* ---- Wake lines and the GNSS relay ----------------------------------------- */

TEST_CASE("#WP, #PPS, #GTX and #GTXLOOP", "[c5]")
{
    /* A first module: after `wp in`. */
    fck_drv_c5_wake_pin_t w;
    TEST_ASSERT_TRUE(fck_drv_c5_parse_wake_pin(line("#WP host_wp_c5=0 c5_wp_host=1 driven=0"), &w));
    TEST_ASSERT_FALSE(w.host_wp_c5);
    TEST_ASSERT_TRUE(w.c5_wp_host);
    TEST_ASSERT_FALSE(w.driven);
    TEST_ASSERT_FALSE_MESSAGE(fck_drv_c5_parse_wake_pin(line("#WP host_wp_c5=2 c5_wp_host=1"), &w),
                              "a level is 0 or 1");

    /* The other first module, with a fix. */
    fck_drv_c5_pps_t p;
    TEST_ASSERT_TRUE(fck_drv_c5_parse_pps(line("#PPS n=20 t_ms=4331051"), &p));
    TEST_ASSERT_EQUAL_UINT32(20, p.n);
    TEST_ASSERT_TRUE(p.t_ms == 4331051);
    TEST_ASSERT_TRUE_MESSAGE(fck_drv_c5_parse_pps(line("#PPS n=1 t_ms=2147483648000"), &p),
                             "a C5 up for 24.8 days and more");
    TEST_ASSERT_TRUE(p.t_ms == 2147483648000LL);
    TEST_ASSERT_FALSE(fck_drv_c5_parse_pps(line("#PPS n=1 t_ms=-5"), &p));

    /* The other first module: GST back on. */
    char sent[32];
    TEST_ASSERT_TRUE(fck_drv_c5_parse_gnss_transmit(line("#GTX sent=\"$POLCFGMSG,0,12,1\""), sent, sizeof(sent)));
    TEST_ASSERT_EQUAL_STRING("$POLCFGMSG,0,12,1", sent);
    TEST_ASSERT_FALSE(fck_drv_c5_parse_gnss_transmit(line("#GTX sent=\"$POLCFGMSG,0,12,1\""), sent, 17));

    bool     ok  = false;
    uint32_t got = 0;
    TEST_ASSERT_TRUE(fck_drv_c5_parse_gnss_transmit_loop(line("#GTXLOOP ok=1 got=12"), &ok, &got));
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL_UINT32(12, got);
    TEST_ASSERT_TRUE(fck_drv_c5_parse_gnss_transmit_loop(line("#GTXLOOP ok=0 got=0"), &ok, &got));
    TEST_ASSERT_FALSE(ok);
}

/* ---- Through the link, as the owner will run them ------------------------------ */

typedef struct {
    fck_drv_c5_network_t      ap[4];
    unsigned         n_ap;
    fck_drv_c5_summary_t sum;
    bool             summed;
    unsigned         other;
} scan_t;

static void on_scan(void *ctx, const fck_drv_link_message_t *m)
{
    scan_t *s = ctx;
    if (s->n_ap < 4U && fck_drv_c5_parse_network(m, &s->ap[s->n_ap])) {
        s->n_ap++;
    } else if (fck_drv_c5_parse_summary(m, &s->sum)) {
        s->summed = true;
    } else {
        s->other++;
    }
}

typedef struct {
    unsigned     gnssu;
    unsigned     pps;
    fck_drv_c5_pps_t last;
} relay_t;

static void on_relay(void *ctx, const fck_drv_link_message_t *m)
{
    relay_t *r = ctx;
    if (strcmp(m->tag, "GNSSU") == 0) {
        r->gnssu++;
    } else if (fck_drv_c5_parse_pps(m, &r->last)) {
        r->pps++;
    }
}

static void feed(fck_drv_link_t *l, const char *s)
{
    fck_drv_link_feed(l, (const uint8_t *)s, strlen(s));
}

TEST_CASE("a scan through the link: the APs to the command, the relay to its reader", "[c5]")
{
    static fck_drv_link_t l;
    relay_t           relay = { 0 };
    fck_drv_link_config_t    cfg   = { .async = fck_drv_c5_async, .on_async = on_relay, .async_ctx = &relay };
    TEST_ASSERT_EQUAL(KODE_OK, fck_drv_link_init(&l, &cfg));

    fck_drv_c5_command_t c;
    scan_t       scan = { 0 };
    TEST_ASSERT_EQUAL(KODE_OK, fck_drv_c5_command_scan(&c, FCK_DRV_C5_BAND_ALL));
    TEST_ASSERT_EQUAL(KODE_OK, fck_drv_link_begin(&l, c.verb, on_scan, &scan));

    feed(&l, "#AP ssid=\"one\" bssid=aa:bb:cc:dd:ee:08 ch=6 band=2g rssi=-56 auth=wpa2_psk\r\n");
    feed(&l, "#GNSSU $GNRMC,,V,,,,,,,,,,N,V*37\r\n");
    feed(&l, "#PPS n=3 t_ms=1000\r\n");
    feed(&l, "#AP ssid=\"two\" bssid=aa:bb:cc:dd:ee:09 ch=100 band=5g rssi=-80 auth=open\r\n");
    feed(&l, "(2 more APs not listed)\r\n");
    feed(&l, "#SCANDONE n=2 ms=5012\r\n");
    TEST_ASSERT_FALSE(fck_drv_link_done(&l, NULL));
    feed(&l, "#DONE scan rc=0\r\n");

    int32_t rc = -9;
    TEST_ASSERT_TRUE(fck_drv_link_done(&l, &rc));
    TEST_ASSERT_EQUAL(KODE_OK, fck_drv_c5_status(rc));
    TEST_ASSERT_EQUAL_UINT(2, scan.n_ap);
    TEST_ASSERT_EQUAL_STRING("two", scan.ap[1].ssid);
    TEST_ASSERT_EQUAL(FCK_DRV_C5_SECURITY_OPEN, scan.ap[1].auth);
    TEST_ASSERT_TRUE(scan.summed);
    TEST_ASSERT_EQUAL_UINT32(2, scan.sum.n);
    TEST_ASSERT_EQUAL_UINT(0, scan.other);
    TEST_ASSERT_EQUAL_UINT(1, relay.gnssu);
    TEST_ASSERT_EQUAL_UINT(1, relay.pps);
    TEST_ASSERT_EQUAL_UINT32(3, relay.last.n);
}

TEST_CASE("`ble scan` ends on #DONE ble, and a command too new for the firmware on rc 2", "[c5]")
{
    static fck_drv_link_t l;
    TEST_ASSERT_EQUAL(KODE_OK, fck_drv_link_init(&l, NULL));

    fck_drv_c5_command_t c;
    scan_t       none = { 0 };
    TEST_ASSERT_EQUAL(KODE_OK, fck_drv_c5_command_ble_scan(&c, 5));
    TEST_ASSERT_EQUAL(KODE_OK, fck_drv_link_begin(&l, c.verb, on_scan, &none));
    feed(&l, "#BLE addr=aa:bb:cc:dd:ee:0a type=rnd rssi=-66 name=\"\" adv=4\r\n");
    feed(&l, "#BLEDONE n=1 ms=5003\r\n#DONE ble rc=0\r\n");
    int32_t rc = -9;
    TEST_ASSERT_TRUE(fck_drv_link_done(&l, &rc));
    TEST_ASSERT_EQUAL(KODE_OK, fck_drv_c5_status(rc));

    /* 1.0 knows no `chan`: a human usage line, then rc 2. */
    TEST_ASSERT_EQUAL(KODE_OK, fck_drv_c5_command_channel(&c, FCK_DRV_C5_BAND_2G, 250));
    TEST_ASSERT_EQUAL(KODE_OK, fck_drv_link_begin(&l, c.verb, on_scan, &none));
    feed(&l, "unknown command: chan (info | scan [2g|5g|all] | gnss on|off)\r\n");
    feed(&l, "#DONE chan rc=2\r\n");
    TEST_ASSERT_TRUE(fck_drv_link_done(&l, &rc));
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_drv_c5_status(rc));
}

/* ---- What the C5 prints from a reset on ------------------------------------ */

/* A KDFCK001 rev 1.0, the firmware 2.0 EU in its flash: the ROM's lines
 * and the firmware's, as they came over the C5's UART0. The MAC is made up. */
static const char FIRMWARE_BOOT[] =
    "ESP-ROM:esp32c5-eco3-20250704\r\n"
    "Build:Jul  4 2025\r\n"
    "rst:0x1 (POWERON),boot:0x38 (SPI_FAST_FLASH_BOOT)\r\n"
    "SPI mode:DIO, clock div:1\r\n"
    "load:0x408556c0,len:0x578\r\n"
    "load:0x4084bba0,len:0xad4\r\n"
    "load:0x4084e5a0,len:0x2954\r\n"
    "entry 0x4084bba0\r\n"
    "#C5INFO fw=c5_fck_agent ver=2.0 chip=esp32c5 rev=1.2 mac=aa:bb:cc:dd:ee:c5 reg=EU lim=ok\r\n";

/* The same module after a write cut short at 40 %: the ROM, again every 170 ms. */
static const char LOOP_ONCE[] =
    "ESP-ROM:esp32c5-eco3-20250704\r\n"
    "Build:Jul  4 2025\r\n"
    "rst:0x3 (RTC_SW_HPSYS),boot:0x38 (SPI_FAST_FLASH_BOOT)\r\n"
    "SPI mode:DIO, clock div:1\r\n"
    "load:0x408556c0,len:0x578\r\n"
    "load:0x4084bba0,len:0xad4\r\n"
    "load:0x4084e5a0,len:0x2954\r\n"
    "entry 0x4084bba0\r\n";

static void feed_text(fck_drv_c5_boot_t *b, const char *text)
{
    fck_drv_c5_boot_feed(b, (const uint8_t *)text, strlen(text));
}

TEST_CASE("the firmware's boot gives its line after one start of the ROM", "[c5][boot]")
{
    fck_drv_c5_boot_t b;
    fck_drv_c5_boot_init(&b);
    TEST_ASSERT_FALSE(fck_drv_c5_boot_done(&b));
    feed_text(&b, FIRMWARE_BOOT);
    TEST_ASSERT_TRUE(fck_drv_c5_boot_done(&b));
    TEST_ASSERT_EQUAL_UINT32(1U, b.restarts);
    TEST_ASSERT_EQUAL_UINT32(0U, b.no_image);
    TEST_ASSERT_TRUE(b.entry);
    TEST_ASSERT_FALSE_MESSAGE(b.bootloader, "its second-stage bootloader is silent");
    /* Whole: FCK_DRV_C5_BOOT_LINE_MAX holds a #C5INFO up to its lim=. The cut is the glued case below. */
    TEST_ASSERT_EQUAL_STRING("#C5INFO fw=c5_fck_agent ver=2.0 chip=esp32c5 rev=1.2 mac=aa:bb:cc:dd:ee:c5 reg=EU lim=ok", b.line);
    TEST_ASSERT_LESS_THAN_size_t(FCK_DRV_C5_BOOT_LINE_MAX, strlen(b.line));
}

TEST_CASE("a write cut short: the ROM over and over is done at its third start, with no line", "[c5][boot]")
{
    fck_drv_c5_boot_t b;
    fck_drv_c5_boot_init(&b);
    feed_text(&b, LOOP_ONCE);
    feed_text(&b, LOOP_ONCE);
    TEST_ASSERT_FALSE(fck_drv_c5_boot_done(&b));
    feed_text(&b, LOOP_ONCE);
    TEST_ASSERT_TRUE(fck_drv_c5_boot_done(&b));
    TEST_ASSERT_EQUAL_UINT32(FCK_DRV_C5_BOOT_LOOP, b.restarts);
    TEST_ASSERT_EQUAL_STRING("", b.line);
}

TEST_CASE("nothing where the bootloader goes: invalid header, again and again", "[c5][boot]")
{
    fck_drv_c5_boot_t b;
    fck_drv_c5_boot_init(&b);
    feed_text(&b, "ESP-ROM:esp32c5-eco3-20250704\r\nBuild:Jul  4 2025\r\n"
                  "rst:0x1 (POWERON),boot:0x38 (SPI_FAST_FLASH_BOOT)\r\n");
    for (int i = 0; i < (int)FCK_DRV_C5_BOOT_LOOP; i++) {
        TEST_ASSERT_FALSE(fck_drv_c5_boot_done(&b));
        feed_text(&b, "invalid header: 0xffffffff\r\n");
    }
    TEST_ASSERT_TRUE(fck_drv_c5_boot_done(&b));
    TEST_ASSERT_EQUAL_UINT32(1U, b.restarts);
    TEST_ASSERT_EQUAL_UINT32(FCK_DRV_C5_BOOT_LOOP, b.no_image);
    TEST_ASSERT_FALSE(b.entry);
    TEST_ASSERT_EQUAL_STRING("", b.line);
}

TEST_CASE("the second-stage bootloader's log and its colours are left out; an image's log line waits for a line of its own", "[c5][boot]")
{
    fck_drv_c5_boot_t b;
    fck_drv_c5_boot_init(&b);
    feed_text(&b, LOOP_ONCE);
    feed_text(&b, "\x1b[0;32mI (35) boot: ESP-IDF v6.1 2nd stage bootloader\x1b[0m\r\n"
                  "\x1b[0;32mI (41) boot.esp32c5: SPI Speed      : 80MHz\x1b[0m\r\n"
                  "W (52) esp_image: image at 0x10000 has invalid magic byte\r\n");
    TEST_ASSERT_TRUE(b.bootloader);
    TEST_ASSERT_EQUAL_STRING("", fck_drv_c5_boot_line(&b));
    feed_text(&b, "\x1b[0;32mI (130) app_init: Project name:     c5_test_fck\x1b[0m\r\n"
                  "I (135) app_init: App version:      1\r\n");
    TEST_ASSERT_FALSE_MESSAGE(fck_drv_c5_boot_done(&b), "a log line does not end the wait");
    TEST_ASSERT_EQUAL_STRING("", b.line);
    TEST_ASSERT_EQUAL_STRING_MESSAGE("I (130) app_init: Project name:     c5_test_fck", b.log_line,
                                     "the first is kept");
    TEST_ASSERT_EQUAL_STRING("I (130) app_init: Project name:     c5_test_fck", fck_drv_c5_boot_line(&b));
}

/* A KDFCK001 rev 1.0 with an image in its
 * flash whose bootloader logs, the image warns early, and its own UART setup
 * cuts the warning short - its end and line ending never come - so the
 * image's ready line comes glued behind it. */
static const char GLUED_BOOT[] =
    "ESP-ROM:esp32c5-eco3-20250704\r\n"
    "Build:Jul  4 2025\r\n"
    "rst:0x1 (POWERON),boot:0x38 (SPI_FAST_FLASH_BOOT)\r\n"
    "SPI mode:DIO, clock div:1\r\n"
    "load:0x408556c0,len:0x15c0\r\n"
    "load:0x4084bba0,len:0xde0\r\n"
    "load:0x4084e5a0,len:0x3278\r\n"
    "entry 0x4084bbaa\r\n"
    "\x1b[0;32mI (23) boot: ESP-IDF v6.1 2nd stage bootloader\x1b[0m\r\n"
    "\x1b[0;32mI (34) boot.esp32c5: SPI Flash Size : 4MB\x1b[0m\r\n"
    "\x1b[0;32mI (65) boot:  2 factory          factory app      00 00 00010000 00100000\x1b[0m\r\n"
    "\x1b[0;32mI (75) esp_image: segment 0: paddr=00010020 vaddr=42030020 size=0a160h ( 41312) map\x1b[0m\r\n"
    "\x1b[0;32mI (140) boot: Loaded app from partition at offset 0x10000\x1b[0m\r\n"
    "\x1b[0;33mW (163) spi_flash: CPU frequency is set to 240MHz. esp_flash_write_encrypted() will automat"
    "ready kode-c5-test-fck 3\n";

TEST_CASE("an image that warns early: it runs, and says so by the warning its ready line is glued to", "[c5][boot]")
{
    fck_drv_c5_boot_t b;
    fck_drv_c5_boot_init(&b);
    feed_text(&b, GLUED_BOOT);
    TEST_ASSERT_FALSE_MESSAGE(fck_drv_c5_boot_done(&b), "no line of its own: it is waited for");
    TEST_ASSERT_EQUAL_UINT32(1U, b.restarts);
    TEST_ASSERT_TRUE(b.entry);
    TEST_ASSERT_TRUE(b.bootloader);
    TEST_ASSERT_EQUAL_STRING("", b.line);
    TEST_ASSERT_EQUAL_STRING("W (163) spi_flash: CPU frequency is set to 240MHz. esp_flash_write_encrypted() will automatread", b.log_line);
    TEST_ASSERT_EQUAL_STRING("W (163) spi_flash: CPU frequency is set to 240MHz. esp_flash_write_encrypted() will automatread", fck_drv_c5_boot_line(&b));

    /* An image that ends its warning and then prints its own line is read by
     * that line. */
    fck_drv_c5_boot_init(&b);
    feed_text(&b, "W (163) spi_flash: CPU frequency is set to 240MHz.\r\nready my_image 1\r\n");
    TEST_ASSERT_TRUE(fck_drv_c5_boot_done(&b));
    TEST_ASSERT_EQUAL_STRING("ready my_image 1", fck_drv_c5_boot_line(&b));
}

TEST_CASE("a line longer than what is put together is cut, and the next one reads whole", "[c5][boot]")
{
    fck_drv_c5_boot_t b;
    fck_drv_c5_boot_init(&b);
    char long_line[300];
    (void)memset(long_line, 'x', sizeof(long_line));
    (void)memcpy(long_line, "W (9) my_tag: ", 14U);
    long_line[sizeof(long_line) - 3U] = '\r';
    long_line[sizeof(long_line) - 2U] = '\n';
    long_line[sizeof(long_line) - 1U] = '\0';
    feed_text(&b, long_line);
    TEST_ASSERT_FALSE(fck_drv_c5_boot_done(&b));
    TEST_ASSERT_EQUAL_size_t(FCK_DRV_C5_BOOT_LINE_MAX, strlen(b.log_line));
    feed_text(&b, "mine\r\n");
    TEST_ASSERT_TRUE(fck_drv_c5_boot_done(&b));
    TEST_ASSERT_EQUAL_STRING("mine", b.line);
    TEST_ASSERT_EQUAL_STRING("", fck_drv_c5_boot_line(NULL));
}

TEST_CASE("bytes one at a time, noise from the reset and blank lines: as if all at once", "[c5][boot]")
{
    fck_drv_c5_boot_t b;
    fck_drv_c5_boot_init(&b);
    static const uint8_t NOISE[] = { 0x00, 0xFF, 0x80, '\r', '\n', ' ', ' ', '\r', '\n' };
    fck_drv_c5_boot_feed(&b, NOISE, sizeof(NOISE));
    for (size_t i = 0U; i < strlen(FIRMWARE_BOOT); i++) {
        fck_drv_c5_boot_feed(&b, (const uint8_t *)&FIRMWARE_BOOT[i], 1U);
    }
    TEST_ASSERT_EQUAL_UINT32(1U, b.restarts);
    TEST_ASSERT_TRUE(b.entry);
    TEST_ASSERT_EQUAL_STRING("#C5INFO fw=c5_fck_agent ver=2.0 chip=esp32c5 rev=1.2 mac=aa:bb:cc:dd:ee:c5 reg=EU lim=ok", b.line);
}

TEST_CASE("after the image's first line nothing changes; NULL is no harm", "[c5][boot]")
{
    fck_drv_c5_boot_t b;
    fck_drv_c5_boot_init(&b);
    feed_text(&b, "  hello from my own firmware   \r\n");
    TEST_ASSERT_EQUAL_STRING("hello from my own firmware", b.line);
    feed_text(&b, LOOP_ONCE);
    feed_text(&b, "another line\r\n");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0U, b.restarts, "a restart after the line is not counted");
    TEST_ASSERT_EQUAL_STRING("hello from my own firmware", b.line);

    fck_drv_c5_boot_init(NULL);
    fck_drv_c5_boot_feed(NULL, (const uint8_t *)"x\n", 2U);
    fck_drv_c5_boot_feed(&b, NULL, 5U);
    TEST_ASSERT_TRUE(fck_drv_c5_boot_done(NULL));
}

/* ---- fck-c5 0.1.0: `health`, and the lim= of a Wi-Fi that is not up ------------------ */

TEST_CASE("`health` is built, read whole, and sent only to a firmware that says it has it", "[c5]")
{
    fck_drv_c5_command_t c;
    TEST_ASSERT_EQUAL(KODE_OK, fck_drv_c5_command_health(&c));
    TEST_ASSERT_EQUAL_STRING("health\n", c.line);
    TEST_ASSERT_EQUAL_STRING("health", c.verb);
    TEST_ASSERT_EQUAL_UINT32(FCK_DRV_C5_COMMAND_MS, c.ms);
    TEST_ASSERT_FALSE(c.emits);

    fck_drv_c5_health_t h;
    TEST_ASSERT_TRUE(fck_drv_c5_parse_health(
        line("#C5HEALTH uptime_ms=123456 rst=task_wdt heap=181234 heap_min=150000"), &h));
    TEST_ASSERT_EQUAL_UINT32(123456, h.uptime_ms);
    TEST_ASSERT_EQUAL_STRING("task_wdt", h.rst);
    TEST_ASSERT_EQUAL_UINT32(181234, h.heap);
    TEST_ASSERT_EQUAL_UINT32(150000, h.heap_min);
    TEST_ASSERT_FALSE_MESSAGE(fck_drv_c5_parse_health(line("#C5HEALTH uptime_ms=1 rst=sw heap=2"), &h),
                              "heap_min is required");
    TEST_ASSERT_FALSE(fck_drv_c5_parse_health(line("#C5INFO uptime_ms=1 heap=2 heap_min=1"), &h));
    TEST_ASSERT_FALSE(fck_drv_c5_parse_health(line("#C5HEALTH uptime_ms=1 heap=2 heap_min=1"), NULL));

    fck_drv_c5_info_t i;
    TEST_ASSERT_TRUE(fck_drv_c5_parse_info(line(C5_010), &i));
    TEST_ASSERT_EQUAL(FCK_DRV_C5_GATE_OFFICIAL, fck_drv_c5_gate(&i, "health", NULL, &c));
    TEST_ASSERT_EQUAL_STRING("health", c.verb);
    /* One that lists no `health`, and the first modules' 2.x, are not sent the word. */
    TEST_ASSERT_TRUE(fck_drv_c5_parse_info(
        line(C5_010_HEAD "reg=EU lim=ok caps=scan,chan,ble,gnss,wp,gtx" C5_010_TAIL), &i));
    TEST_ASSERT_EQUAL(FCK_DRV_C5_GATE_REFUSE, fck_drv_c5_gate(&i, "health", NULL, &c));
    TEST_ASSERT_TRUE(fck_drv_c5_parse_info(line("#C5INFO fw=fck-c5 ver=2.3 reg=EU lim=ok"), &i));
    TEST_ASSERT_EQUAL(FCK_DRV_C5_GATE_REFUSE, fck_drv_c5_gate(&i, "health", NULL, &c));
}

TEST_CASE("0.1's #C5INFO says how long it has run and why it started", "[c5]")
{
    fck_drv_c5_info_t i;
    TEST_ASSERT_TRUE(fck_drv_c5_parse_info(line(C5_010), &i));
    TEST_ASSERT_EQUAL_UINT32(1234, i.uptime_ms);
    TEST_ASSERT_EQUAL_STRING("poweron", i.rst);
    TEST_ASSERT_TRUE(fck_drv_c5_parse_info(
        line(C5_010_HEAD "reg=EU lim=ok caps=scan uptime_ms=9 rst=task_wdt"), &i));
    TEST_ASSERT_EQUAL_STRING("task_wdt", i.rst);
    /* The first modules' 2.x says neither. */
    TEST_ASSERT_TRUE(fck_drv_c5_parse_info(line("#C5INFO fw=fck-c5 ver=2.3 reg=EU lim=ok"), &i));
    TEST_ASSERT_EQUAL_UINT32(0, i.uptime_ms);
    TEST_ASSERT_EQUAL_STRING("", i.rst);
}

TEST_CASE("a Wi-Fi that is not up yet says where it stopped in lim=, and `scan` is left to the firmware to refuse", "[c5]")
{
    static const char *const steps[] = { "wifi-loop", "wifi-init", "wifi-mode", "wifi-start",
                                         "country",   "bw20",      "no11b",     "power",
                                         "power-readback" };
    fck_drv_c5_command_t c;
    for (size_t r = 0; r < sizeof(steps) / sizeof(steps[0]); r++) {
        char g[FCK_DRV_LINK_LINE_MAX + 1];
        snprintf(g, sizeof(g), C5_010_HEAD "reg=EU lim=%s caps=scan,chan,ble,gnss,wp,gtx,health" C5_010_TAIL,
                 steps[r]);
        fck_drv_c5_info_t i;
        TEST_ASSERT_TRUE(fck_drv_c5_parse_info(line(g), &i));
        TEST_ASSERT_EQUAL_STRING_MESSAGE(steps[r], i.lim, "the word is kept whole");
        TEST_ASSERT_FALSE_MESSAGE(fck_drv_c5_is_unlimited(&i), steps[r]);
        TEST_ASSERT_EQUAL_MESSAGE(FCK_DRV_C5_GATE_OFFICIAL, fck_drv_c5_gate(&i, "scan all", NULL, &c), steps[r]);
        TEST_ASSERT_EQUAL_MESSAGE(FCK_DRV_C5_GATE_OFFICIAL, fck_drv_c5_gate(&i, "ble scan 5", NULL, &c), steps[r]);
        TEST_ASSERT_EQUAL_MESSAGE(FCK_DRV_C5_GATE_OFFICIAL, fck_drv_c5_gate(&i, "health", NULL, &c), steps[r]);
    }
}

TEST_CASE("compatible is a caps=, whatever the name; the first modules' firmware is compatible by its name", "[c5]")
{
    fck_drv_c5_info_t i;
    TEST_ASSERT_TRUE(fck_drv_c5_parse_info(line("#C5INFO fw=my_firmware ver=0.3 caps=scan,ble"), &i));
    TEST_ASSERT_TRUE(fck_drv_c5_is_compatible(&i));
    TEST_ASSERT_FALSE(fck_drv_c5_is_official(i.fw));
    TEST_ASSERT_TRUE(fck_drv_c5_has(&i, FCK_DRV_C5_SCAN));
    TEST_ASSERT_TRUE(fck_drv_c5_has(&i, FCK_DRV_C5_BLE_SCAN));
    TEST_ASSERT_FALSE_MESSAGE(fck_drv_c5_has(&i, FCK_DRV_C5_CHANNEL), "a word it does not list");
    TEST_ASSERT_FALSE(fck_drv_c5_is_unlimited(&i));
    /* An empty caps= is still one: compatible, and asked nothing. */
    TEST_ASSERT_TRUE(fck_drv_c5_parse_info(line("#C5INFO fw=my_firmware caps="), &i));
    TEST_ASSERT_TRUE(fck_drv_c5_is_compatible(&i));
    TEST_ASSERT_FALSE(fck_drv_c5_has(&i, FCK_DRV_C5_SCAN));
    /* No caps=: only the names of the first modules' firmware. */
    TEST_ASSERT_TRUE(fck_drv_c5_parse_info(line("#C5INFO fw=my_firmware ver=0.3"), &i));
    TEST_ASSERT_FALSE(fck_drv_c5_is_compatible(&i));
    TEST_ASSERT_TRUE(fck_drv_c5_parse_info(line("#C5INFO fw=c5_fck_agent ver=1.5"), &i));
    TEST_ASSERT_TRUE(fck_drv_c5_is_compatible(&i));
    TEST_ASSERT_TRUE_MESSAGE(fck_drv_c5_is_unlimited(&i), "before 2.0 it keeps no limits itself");
    TEST_ASSERT_TRUE(fck_drv_c5_parse_info(line("#C5INFO fw=fck-c5 ver=2.3 reg=EU lim=ok"), &i));
    TEST_ASSERT_TRUE(fck_drv_c5_is_compatible(&i));
    TEST_ASSERT_FALSE(fck_drv_c5_is_unlimited(&i));
    TEST_ASSERT_FALSE(fck_drv_c5_is_compatible(NULL));
    TEST_ASSERT_FALSE(fck_drv_c5_is_unlimited(NULL));
}
