/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_link.c
 * @brief fck_drv_link: the line protocol both firmwares speak, read.
 *
 * The lines here are the firmwares' own, copied from their sources,
 * so a case that passes is a line the
 * SDK will read from a real module. What the link must never do is the
 * point of most of them: hand one command's line to another, end a command
 * on someone else's #DONE, or parse a line meant for a human.
 */
#include <stdio.h>
#include <string.h>

#include "unity.h"

#include "fck_drv_link.h"

/* ---- A spy for the three callbacks ---------------------------------------- */

#define SPY_MAX 16

typedef struct {
    unsigned n;
    char     tag[SPY_MAX][FCK_DRV_LINK_TAG_MAX + 1];
    char     body[SPY_MAX][FCK_DRV_LINK_LINE_MAX + 1];
} spy_t;

static spy_t s_reply;
static spy_t s_async;
static spy_t s_text;

static void spy_msg(void *ctx, const fck_drv_link_message_t *m)
{
    spy_t *s = ctx;
    if (s->n < SPY_MAX) {
        snprintf(s->tag[s->n], sizeof(s->tag[0]), "%s", m->tag);
        snprintf(s->body[s->n], sizeof(s->body[0]), "%s", m->body);
    }
    s->n++;
}

static void spy_text(void *ctx, const char *line)
{
    spy_t *s = ctx;
    if (s->n < SPY_MAX) {
        snprintf(s->body[s->n], sizeof(s->body[0]), "%s", line);
    }
    s->n++;
}

static const char *const ASYNC[] = { "GNSSU", "PPS", NULL };

static fck_drv_link_t s_link;

static void link_up(void)
{
    memset(&s_reply, 0, sizeof(s_reply));
    memset(&s_async, 0, sizeof(s_async));
    memset(&s_text, 0, sizeof(s_text));
    fck_drv_link_config_t cfg = {
        .async     = ASYNC,
        .on_async  = spy_msg,
        .async_ctx = &s_async,
        .on_text   = spy_text,
        .text_ctx  = &s_text,
    };
    TEST_ASSERT_EQUAL(KODE_OK, fck_drv_link_init(&s_link, &cfg));
}

static void feed(const char *s)
{
    fck_drv_link_feed(&s_link, (const uint8_t *)s, strlen(s));
}

static void begin(const char *cmd)
{
    TEST_ASSERT_EQUAL(KODE_OK, fck_drv_link_begin(&s_link, cmd, spy_msg, &s_reply));
}

/* A message built by hand, for the accessors alone. */
static fck_drv_link_message_t msg(const char *body)
{
    fck_drv_link_message_t m = { .tag = "T", .body = body };
    return m;
}

/* ---- Lines ------------------------------------------------------------------ */

TEST_CASE("a command's lines reach it, and its #DONE ends it with its rc", "[link]")
{
    link_up();
    begin("scan");

    feed("scan all\r\n");
    feed("#AP ssid=\"Kode lab\" bssid=aa:bb:cc:dd:ee:01 ch=6 band=2g rssi=-61 auth=wpa2\r\n");
    feed("#SCANDONE n=1 ms=5012\r\n");
    TEST_ASSERT_FALSE(fck_drv_link_done(&s_link, NULL));
    feed("#DONE scan rc=0\r\n");

    int32_t rc = -9;
    TEST_ASSERT_TRUE(fck_drv_link_done(&s_link, &rc));
    TEST_ASSERT_EQUAL_INT32(0, rc);
    TEST_ASSERT_EQUAL_UINT(2, s_reply.n);
    TEST_ASSERT_EQUAL_STRING("AP", s_reply.tag[0]);
    TEST_ASSERT_EQUAL_STRING("SCANDONE", s_reply.tag[1]);
    /* The echo of the command is a human line, and stays one. */
    TEST_ASSERT_EQUAL_UINT(1, s_text.n);
    TEST_ASSERT_EQUAL_STRING("scan all", s_text.body[0]);
}

TEST_CASE("a command ends on the #DONE verb it was begun with, not on another", "[link]")
{
    link_up();
    begin("rf_ping");                       /* the nRF's `rf ping 10` */
    feed("#PING24 seq=0 ok=1 rssi_fwd=-41 rssi_rev=-43\n");
    feed("#DONE rf rc=0\n");
    TEST_ASSERT_FALSE_MESSAGE(fck_drv_link_done(&s_link, NULL),
        "a #DONE for another verb ended the command");
    feed("#DONE rf_ping rc=1\n");

    int32_t rc = 0;
    TEST_ASSERT_TRUE(fck_drv_link_done(&s_link, &rc));
    TEST_ASSERT_EQUAL_INT32(1, rc);
    TEST_ASSERT_EQUAL_UINT(1, s_reply.n);
    TEST_ASSERT_EQUAL_UINT32(1, fck_drv_link_statistics(&s_link).stray_done);
}

TEST_CASE("any split of the bytes reads the same as whole lines", "[link]")
{
    const char *wire = "#C5INFO fw=c5_fck_agent ver=2.0.0-eu chip=esp32c5 rev=1 "
                       "mac=60:55:f9:00:12:34 reg=EU lim=ok\r\n";
    link_up();
    for (size_t i = 0; wire[i] != '\0'; i++) {
        fck_drv_link_feed(&s_link, (const uint8_t *)&wire[i], 1);
    }
    TEST_ASSERT_EQUAL_UINT(1, s_async.n);
    TEST_ASSERT_EQUAL_STRING("C5INFO", s_async.tag[0]);
    TEST_ASSERT_EQUAL_STRING("fw=c5_fck_agent ver=2.0.0-eu chip=esp32c5 rev=1 "
                             "mac=60:55:f9:00:12:34 reg=EU lim=ok", s_async.body[0]);
}

TEST_CASE("the async tags reach on_async while a command is in flight", "[link]")
{
    link_up();
    begin("chan");
    feed("#CHAN ch=1 frames=12 rssi_avg=-70\n");
    feed("#GNSSU $GNRMC,123519,A,4807.038,N,01131.000,E,022.4,084.4,230394,003.1,W*6A\n");
    feed("#PPS n=3\n");
    feed("#CHAN ch=2 frames=0 rssi_avg=-127\n");

    TEST_ASSERT_EQUAL_UINT(2, s_reply.n);
    TEST_ASSERT_EQUAL_STRING("CHAN", s_reply.tag[1]);
    TEST_ASSERT_EQUAL_UINT(2, s_async.n);
    TEST_ASSERT_EQUAL_STRING("GNSSU", s_async.tag[0]);
    TEST_ASSERT_EQUAL_STRING("PPS", s_async.tag[1]);
}

TEST_CASE("a machine line with no command in flight is unsolicited: on_async", "[link]")
{
    link_up();
    feed("#NRFINFO fw=nrf_fck_agent ver=2.0.0 devid=0011223344556677 reset=0x00000001 uptime_ms=6012\n");
    TEST_ASSERT_EQUAL_UINT(1, s_async.n);
    TEST_ASSERT_EQUAL_STRING("NRFINFO", s_async.tag[0]);
    TEST_ASSERT_EQUAL_UINT(0, s_reply.n);
}

TEST_CASE("human lines are never parsed, whatever they say", "[link]")
{
    link_up();
    begin("frob");
    feed("unknown command: frob\n");
    feed("(chan: channels refused under country ES, not surveyed: 14)\n");
    feed("# a comment that starts with a hash and a space\n");
    feed("#DONE frob rc=2\n");

    TEST_ASSERT_EQUAL_UINT(0, s_reply.n);
    TEST_ASSERT_EQUAL_UINT(3, s_text.n);
    TEST_ASSERT_EQUAL_STRING("unknown command: frob", s_text.body[0]);
    int32_t rc = 0;
    TEST_ASSERT_TRUE(fck_drv_link_done(&s_link, &rc));
    TEST_ASSERT_EQUAL_INT32(2, rc);
}

TEST_CASE("ANSI escapes, carriage returns and the nRF shell's prompt are stripped", "[link]")
{
    link_up();
    begin("info");
    feed("\x1b[1;32muart:~$ \x1b[m#NRFINFO fw=nrf_fck_agent ver=2.0.0\r\n");
    feed("uart:~$ #DONE info rc=0\r\n");

    TEST_ASSERT_EQUAL_UINT(1, s_reply.n);
    TEST_ASSERT_EQUAL_STRING("NRFINFO", s_reply.tag[0]);
    TEST_ASSERT_EQUAL_STRING("fw=nrf_fck_agent ver=2.0.0", s_reply.body[0]);
    TEST_ASSERT_TRUE(fck_drv_link_done(&s_link, NULL));
}

TEST_CASE("a line too long is dropped whole, and the next one is read", "[link]")
{
    char big[FCK_DRV_LINK_LINE_MAX + 40];
    memset(big, 'a', sizeof(big));
    memcpy(big, "#AP ssid=", 9);
    big[sizeof(big) - 1] = '\0';

    link_up();
    begin("scan");
    feed(big);
    feed("\n#DONE scan rc=0\n");

    TEST_ASSERT_EQUAL_UINT_MESSAGE(0, s_reply.n, "half a line was parsed");
    TEST_ASSERT_EQUAL_UINT32(1, fck_drv_link_statistics(&s_link).too_long);
    TEST_ASSERT_TRUE(fck_drv_link_done(&s_link, NULL));
}

/* ---- Commands --------------------------------------------------------------- */

TEST_CASE("one command at a time, with a verb of one word that fits", "[link]")
{
    link_up();
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_drv_link_begin(&s_link, "", NULL, NULL));
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_drv_link_begin(&s_link, "rf ping", NULL, NULL));
    TEST_ASSERT_EQUAL(KODE_DENIED,
                      fck_drv_link_begin(&s_link, "averbthatislongerthanfifteen", NULL, NULL));
    begin("info");
    TEST_ASSERT_FALSE(fck_drv_link_idle(&s_link));
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_drv_link_begin(&s_link, "scan", NULL, NULL));
}

TEST_CASE("after an abort the old command's lines go nowhere, and nothing begins until its #DONE",
          "[link]")
{
    link_up();
    begin("rf_ping");
    feed("#PING24 seq=0 ok=1 rssi_fwd=-41 rssi_rev=-43\n");
    fck_drv_link_abort(&s_link);
    TEST_ASSERT_FALSE(fck_drv_link_done(&s_link, NULL));
    TEST_ASSERT_FALSE(fck_drv_link_idle(&s_link));
    TEST_ASSERT_EQUAL_MESSAGE(KODE_DENIED, fck_drv_link_begin(&s_link, "rf_ping", NULL, NULL),
        "a command began while the abandoned one was still answering");

    feed("#PING24 seq=1 ok=1 rssi_fwd=-40 rssi_rev=-44\n");    /* the abandoned one's, late */
    feed("#PPS n=4\n");                      /* async: still delivered */
    feed("#DONE rf_ping rc=0\n");           /* its end */
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1, s_reply.n, "a late line reached a reader");
    TEST_ASSERT_EQUAL_UINT(1, s_async.n);
    TEST_ASSERT_EQUAL_UINT32(1, fck_drv_link_statistics(&s_link).stale);
    TEST_ASSERT_EQUAL_UINT32(0, fck_drv_link_statistics(&s_link).stray_done);
    TEST_ASSERT_TRUE(fck_drv_link_idle(&s_link));

    begin("rf_ping");
    feed("#DONE rf_ping rc=0\n");
    TEST_ASSERT_TRUE(fck_drv_link_done(&s_link, NULL));
}

TEST_CASE("a reset forgets the command in flight and the aborted verb", "[link]")
{
    link_up();
    begin("gnss");
    fck_drv_link_abort(&s_link);
    fck_drv_link_reset(&s_link);
    begin("gnss");
    feed("#DONE gnss rc=0\n");
    TEST_ASSERT_TRUE(fck_drv_link_done(&s_link, NULL));
}

TEST_CASE("a #DONE without rc ends the command with rc -1", "[link]")
{
    link_up();
    begin("wp");
    feed("#DONE wp\n");
    int32_t rc = 0;
    TEST_ASSERT_TRUE(fck_drv_link_done(&s_link, &rc));
    TEST_ASSERT_EQUAL_INT32(-1, rc);
}

TEST_CASE("more async tags than fit, or a tag too long, is refused", "[link]")
{
    static const char *const MANY[] = { "A", "B", "C", "D", "E", "F", "G", "H", "I", NULL };
    static const char *const LONG[] = { "ATAGLONGERTHAN15", NULL };
    fck_drv_link_config_t cfg = { .async = MANY };
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_drv_link_init(&s_link, &cfg));
    cfg.async = LONG;
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_drv_link_init(&s_link, &cfg));
    TEST_ASSERT_EQUAL(KODE_OK, fck_drv_link_init(&s_link, NULL));
}

/* ---- The banner ---------------------------------------------------------------- */

static spy_t s_banner;

/* The nRF's link as the owner sets it up: #NRFINFO is its banner, `info`
 * the command it answers. The lines are nrf_fck_agent's (main.c). */
static void link_up_banner(void)
{
    link_up();
    memset(&s_banner, 0, sizeof(s_banner));
    fck_drv_link_config_t cfg = {
        .async       = ASYNC,
        .on_async    = spy_msg,
        .async_ctx   = &s_async,
        .on_text     = spy_text,
        .text_ctx    = &s_text,
        .banner      = "NRFINFO",
        .banner_verb = "info",
        .on_banner   = spy_msg,
        .banner_ctx  = &s_banner,
    };
    TEST_ASSERT_EQUAL(KODE_OK, fck_drv_link_init(&s_link, &cfg));
}

#define NRF_BANNER "#NRFINFO fw=nrf_fck_agent ver=1.0 devid=0123456789ABCDEF " \
                   "reset=0x00000001 uptime_ms=412\n"

TEST_CASE("a banner with nothing in flight reaches on_banner and on_async", "[link]")
{
    link_up_banner();
    feed(NRF_BANNER);
    TEST_ASSERT_EQUAL_UINT(1, s_banner.n);
    TEST_ASSERT_EQUAL_STRING("NRFINFO", s_banner.tag[0]);
    TEST_ASSERT_EQUAL_UINT(1, s_async.n);
    TEST_ASSERT_EQUAL_UINT32(0, fck_drv_link_statistics(&s_link).restarts);
}

TEST_CASE("the banner is info's answer: on_banner and on_reply, and its #DONE ends it", "[link]")
{
    link_up_banner();
    begin("info");
    feed(NRF_BANNER);
    TEST_ASSERT_FALSE(fck_drv_link_done(&s_link, NULL));
    feed("#DONE info rc=0\n");

    int32_t rc = -9;
    TEST_ASSERT_TRUE(fck_drv_link_done(&s_link, &rc));
    TEST_ASSERT_EQUAL_INT32(0, rc);
    TEST_ASSERT_FALSE(fck_drv_link_restarted(&s_link));
    TEST_ASSERT_EQUAL_UINT(1, s_banner.n);
    TEST_ASSERT_EQUAL_UINT(1, s_reply.n);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0, s_async.n, "info's answer reached on_async too");
}

TEST_CASE("a banner in the middle of another command ends it: the chip restarted", "[link]")
{
    link_up_banner();
    begin("rf_ping");
    feed("#PING24 seq=0 ok=1 rssi_fwd=-41 rssi_rev=-43\n");
    feed(NRF_BANNER);

    int32_t rc = 0;
    TEST_ASSERT_TRUE_MESSAGE(fck_drv_link_done(&s_link, &rc),
        "a command went on waiting for a #DONE that a restarted chip never sends");
    TEST_ASSERT_EQUAL_INT32(-1, rc);
    TEST_ASSERT_TRUE(fck_drv_link_restarted(&s_link));
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1, s_reply.n,
        "the banner reached the command as its answer: only #PING24 should have");
    TEST_ASSERT_EQUAL_UINT(1, s_banner.n);
    TEST_ASSERT_EQUAL_UINT(1, s_async.n);
    TEST_ASSERT_EQUAL_UINT32(1, fck_drv_link_statistics(&s_link).restarts);
    TEST_ASSERT_TRUE(fck_drv_link_idle(&s_link));

    /* The next command is a new one: nothing of the restart is left on it. */
    begin("rf_spec");
    TEST_ASSERT_FALSE(fck_drv_link_done(&s_link, NULL));
    TEST_ASSERT_FALSE(fck_drv_link_restarted(&s_link));
    feed("#DONE rf_spec rc=0\n");
    TEST_ASSERT_TRUE(fck_drv_link_done(&s_link, NULL));
    TEST_ASSERT_FALSE(fck_drv_link_restarted(&s_link));
}

TEST_CASE("a banner ends the wait for an abandoned command's #DONE", "[link]")
{
    link_up_banner();
    begin("rf_sniff");
    fck_drv_link_abort(&s_link);
    TEST_ASSERT_FALSE(fck_drv_link_idle(&s_link));
    feed(NRF_BANNER);
    TEST_ASSERT_TRUE_MESSAGE(fck_drv_link_idle(&s_link),
        "the link waited on for the #DONE of a command the restart wiped");
    TEST_ASSERT_EQUAL_UINT(1, s_async.n);
    TEST_ASSERT_EQUAL_UINT32(1, fck_drv_link_statistics(&s_link).restarts);
}

TEST_CASE("an abandoned info is forgotten: its late banner is heard, never a restart",
          "[link]")
{
    link_up_banner();
    begin("info");
    fck_drv_link_abort(&s_link);
    TEST_ASSERT_TRUE_MESSAGE(fck_drv_link_idle(&s_link),
        "an info sent while the chip was starting kept the link waiting for a #DONE");
    feed(NRF_BANNER);
    TEST_ASSERT_EQUAL_UINT(1, s_banner.n);
    TEST_ASSERT_EQUAL_UINT32(0, fck_drv_link_statistics(&s_link).restarts);
    feed("#DONE info rc=0\n");
    TEST_ASSERT_TRUE(fck_drv_link_idle(&s_link));
    TEST_ASSERT_EQUAL(KODE_OK, fck_drv_link_begin(&s_link, "rf_spec", NULL, NULL));
}

TEST_CASE("a forgotten command leaves the link idle, and its late lines are unsolicited",
          "[link]")
{
    link_up_banner();
    begin("info");
    feed("#PPS n=1\n#GNSS");                 /* a line cut in half is kept */
    fck_drv_link_forget(&s_link);
    TEST_ASSERT_TRUE(fck_drv_link_idle(&s_link));
    TEST_ASSERT_FALSE(fck_drv_link_done(&s_link, NULL));
    feed("U $GNRMC\n");
    feed(NRF_BANNER);
    feed("#DONE info rc=0\n");
    TEST_ASSERT_EQUAL_UINT(0, s_reply.n);
    TEST_ASSERT_EQUAL_UINT(3, s_async.n);
    TEST_ASSERT_EQUAL_STRING("GNSSU", s_async.tag[1]);
    TEST_ASSERT_EQUAL_UINT32(1, fck_drv_link_statistics(&s_link).stray_done);
    TEST_ASSERT_EQUAL_UINT32(0, fck_drv_link_statistics(&s_link).restarts);
}

TEST_CASE("without a banner named, a banner tag is any other line", "[link]")
{
    link_up();
    begin("rf_ping");
    feed(NRF_BANNER);
    TEST_ASSERT_FALSE(fck_drv_link_done(&s_link, NULL));
    TEST_ASSERT_EQUAL_UINT(1, s_reply.n);
}

TEST_CASE("a banner without its verb, or either not one word that fits, is refused", "[link]")
{
    fck_drv_link_config_t cfg = { .banner = "NRFINFO" };
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_drv_link_init(&s_link, &cfg));
    cfg = (fck_drv_link_config_t){ .banner_verb = "info" };
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_drv_link_init(&s_link, &cfg));
    cfg = (fck_drv_link_config_t){ .banner = "NRF INFO", .banner_verb = "info" };
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_drv_link_init(&s_link, &cfg));
    cfg = (fck_drv_link_config_t){ .banner = "", .banner_verb = "info" };
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_drv_link_init(&s_link, &cfg));
    cfg = (fck_drv_link_config_t){ .banner = "NRFINFO", .banner_verb = "ATAGLONGERTHAN15" };
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_drv_link_init(&s_link, &cfg));
    cfg = (fck_drv_link_config_t){ .banner = "NRFINFO", .banner_verb = "info" };
    TEST_ASSERT_EQUAL(KODE_OK, fck_drv_link_init(&s_link, &cfg));
}

/* ---- Values ------------------------------------------------------------------ */

TEST_CASE("a quoted value keeps its spaces and loses its escapes", "[link]")
{
    fck_drv_link_message_t m = msg("ssid=\"Kode \\\"lab\\\" \\\\ 2\\x00\\xFF\" ch=6");
    char      out[40];
    size_t    len = 0;
    TEST_ASSERT_TRUE(fck_drv_link_message_text(&m, "ssid", out, sizeof(out), &len));
    TEST_ASSERT_EQUAL_UINT(16, len);
    TEST_ASSERT_EQUAL_MEMORY("Kode \"lab\" \\ 2\x00\xFF", out, 16);
    int32_t ch = 0;
    TEST_ASSERT_TRUE(fck_drv_link_message_integer(&m, "ch", &ch));
    TEST_ASSERT_EQUAL_INT32(6, ch);
}

TEST_CASE("a key is matched whole: rssi is not rssi_avg", "[link]")
{
    fck_drv_link_message_t m = msg("rssi_avg=-70 frames=3");
    int32_t   v = 1;
    TEST_ASSERT_FALSE(fck_drv_link_message_integer(&m, "rssi", &v));
    TEST_ASSERT_EQUAL_INT32(1, v);
    TEST_ASSERT_TRUE(fck_drv_link_message_integer(&m, "rssi_avg", &v));
    TEST_ASSERT_EQUAL_INT32(-70, v);
}

TEST_CASE("numbers: decimal, negative, 0x hex, and nothing that is not all number", "[link]")
{
    fck_drv_link_message_t m = msg("a=-127 b=0xCB c=12x d=0x d2=- e=2147483648 f=0x7fffffff g=\"5\" h=");
    int32_t   v = 0;
    TEST_ASSERT_TRUE(fck_drv_link_message_integer(&m, "a", &v));
    TEST_ASSERT_EQUAL_INT32(-127, v);
    TEST_ASSERT_TRUE(fck_drv_link_message_integer(&m, "b", &v));
    TEST_ASSERT_EQUAL_INT32(0xCB, v);
    TEST_ASSERT_FALSE(fck_drv_link_message_integer(&m, "c", &v));
    TEST_ASSERT_FALSE(fck_drv_link_message_integer(&m, "d", &v));
    TEST_ASSERT_FALSE(fck_drv_link_message_integer(&m, "d2", &v));
    TEST_ASSERT_FALSE(fck_drv_link_message_integer(&m, "e", &v));
    TEST_ASSERT_TRUE(fck_drv_link_message_integer(&m, "f", &v));
    TEST_ASSERT_EQUAL_INT32(0x7fffffff, v);
    TEST_ASSERT_FALSE(fck_drv_link_message_integer(&m, "g", &v));
    TEST_ASSERT_FALSE(fck_drv_link_message_integer(&m, "h", &v));
    TEST_ASSERT_FALSE(fck_drv_link_message_integer(&m, "missing", &v));
}

TEST_CASE("unsigned numbers reach 2^32 - 1, in decimal or hex, and refuse a sign", "[link]")
{
    fck_drv_link_message_t m = msg("hz=2480000000 up=0xFFFFFFFF big=4294967296 neg=-1 x=0x h=12a q=\"7\" e=");
    uint32_t  v = 7U;
    TEST_ASSERT_TRUE(fck_drv_link_message_unsigned(&m, "hz", &v));
    TEST_ASSERT_EQUAL_UINT32(2480000000UL, v);
    TEST_ASSERT_TRUE(fck_drv_link_message_unsigned(&m, "up", &v));
    TEST_ASSERT_EQUAL_UINT32(0xFFFFFFFFUL, v);
    v = 7U;
    TEST_ASSERT_FALSE(fck_drv_link_message_unsigned(&m, "big", &v));
    TEST_ASSERT_FALSE(fck_drv_link_message_unsigned(&m, "neg", &v));
    TEST_ASSERT_FALSE(fck_drv_link_message_unsigned(&m, "x", &v));
    TEST_ASSERT_FALSE(fck_drv_link_message_unsigned(&m, "h", &v));
    TEST_ASSERT_FALSE(fck_drv_link_message_unsigned(&m, "q", &v));
    TEST_ASSERT_FALSE(fck_drv_link_message_unsigned(&m, "e", &v));
    TEST_ASSERT_FALSE(fck_drv_link_message_unsigned(&m, "missing", &v));
    TEST_ASSERT_EQUAL_UINT32(7U, v);
}

TEST_CASE("a MAC address is six hex bytes and nothing else", "[link]")
{
    fck_drv_link_message_t m = msg("mac=60:55:F9:00:12:34 bad=60:55:f9:00:12 bad2=60-55-f9-00-12-34");
    uint8_t   mac[6] = { 0 };
    TEST_ASSERT_TRUE(fck_drv_link_message_mac(&m, "mac", mac));
    const uint8_t want[6] = { 0x60, 0x55, 0xF9, 0x00, 0x12, 0x34 };
    TEST_ASSERT_EQUAL_UINT8_ARRAY(want, mac, 6);
    TEST_ASSERT_FALSE(fck_drv_link_message_mac(&m, "bad", mac));
    TEST_ASSERT_FALSE(fck_drv_link_message_mac(&m, "bad2", mac));
}

TEST_CASE("a value that does not fit, or a quote that never closes, gives nothing", "[link]")
{
    fck_drv_link_message_t m = msg("name=\"abcdef\" open=\"abc");
    char      out[4] = "zzz";
    TEST_ASSERT_FALSE(fck_drv_link_message_text(&m, "name", out, sizeof(out), NULL));
    TEST_ASSERT_EQUAL_STRING("", out);
    char big[16];
    TEST_ASSERT_FALSE(fck_drv_link_message_text(&m, "open", big, sizeof(big), NULL));
    TEST_ASSERT_TRUE(fck_drv_link_message_text(&m, "name", big, sizeof(big), NULL));
    TEST_ASSERT_EQUAL_STRING("abcdef", big);
}

TEST_CASE("the words that are not key=value are counted from 0", "[link]")
{
    fck_drv_link_message_t m = msg("ble rc=0 tail");
    char      w[8];
    TEST_ASSERT_TRUE(fck_drv_link_message_word(&m, 0, w, sizeof(w)));
    TEST_ASSERT_EQUAL_STRING("ble", w);
    TEST_ASSERT_TRUE(fck_drv_link_message_word(&m, 1, w, sizeof(w)));
    TEST_ASSERT_EQUAL_STRING("tail", w);
    TEST_ASSERT_FALSE(fck_drv_link_message_word(&m, 2, w, sizeof(w)));
}

TEST_CASE("two banners say the same image when only their uptime differs", "[link]")
{
    TEST_ASSERT_TRUE(fck_drv_link_same_banner(
        "fw=nrf_fck_agent ver=2.0 devid=0123456789ABCDEF reset=0x4 uptime_ms=6301",
        "fw=nrf_fck_agent ver=2.0 devid=0123456789ABCDEF reset=0x4 uptime_ms=90210"));
    TEST_ASSERT_TRUE(fck_drv_link_same_banner("fw=c5_fck_agent ver=2.0 reg=EU",
                                          "fw=c5_fck_agent  ver=2.0 reg=EU "));
    TEST_ASSERT_TRUE(fck_drv_link_same_banner("uptime_ms=1 fw=x", "fw=x"));

    TEST_ASSERT_FALSE_MESSAGE(fck_drv_link_same_banner("fw=nrf_fck_agent ver=2.0 uptime_ms=1",
                                                   "fw=nrf_fck_agent ver=1.0 uptime_ms=2"),
                              "an image MCUboot swapped back was taken for the same");
    TEST_ASSERT_FALSE(fck_drv_link_same_banner("fw=nrf_fck_agent ver=2.0 reg=EU",
                                           "fw=nrf_fck_agent ver=2.0"));
    TEST_ASSERT_FALSE(fck_drv_link_same_banner("fw=a", "fw=ab"));
    TEST_ASSERT_FALSE(fck_drv_link_same_banner("fw=a", NULL));
}

TEST_CASE("the 0.1 firmwares' banners are the same image from `info` to `info`, and another after a restart", "[link]")
{
    /* fck-c5 and fck-nrf 0.1.0, as their sources print them: nothing moves
     * within a boot but uptime_ms=. */
    TEST_ASSERT_TRUE(fck_drv_link_same_banner(
        "fw=fck-c5 ver=0.1 chip=esp32c5 rev=1.2 mac=aa:bb:cc:dd:ee:01 reg=EU lim=ok "
        "caps=scan,chan,ble,gnss,wp,gtx,health uptime_ms=1234 rst=poweron",
        "fw=fck-c5 ver=0.1 chip=esp32c5 rev=1.2 mac=aa:bb:cc:dd:ee:01 reg=EU lim=ok "
        "caps=scan,chan,ble,gnss,wp,gtx,health uptime_ms=987654 rst=poweron"));
    TEST_ASSERT_TRUE(fck_drv_link_same_banner(
        "fw=fck-nrf ver=0.1.0 devid=0123456789ABCDEF reset=0x00000001 uptime_ms=3100 map=0x0000E800 "
        "caps=spec,off,power,echo,ping,sniff,scfg,saddr,eu,done,wdt,echoidle,smp",
        "fw=fck-nrf ver=0.1.0 devid=0123456789ABCDEF reset=0x00000001 uptime_ms=70000 map=0x0000E800 "
        "caps=spec,off,power,echo,ping,sniff,scfg,saddr,eu,done,wdt,echoidle,smp"));
    /* The watchdog restarted the C5, or its Wi-Fi came up: no longer what was judged. */
    TEST_ASSERT_FALSE(fck_drv_link_same_banner(
        "fw=fck-c5 ver=0.1 reg=EU lim=ok caps=scan uptime_ms=1 rst=poweron",
        "fw=fck-c5 ver=0.1 reg=EU lim=ok caps=scan uptime_ms=1 rst=task_wdt"));
    TEST_ASSERT_FALSE(fck_drv_link_same_banner(
        "fw=fck-c5 ver=0.1 reg=EU lim=wifi-start caps=scan uptime_ms=1 rst=poweron",
        "fw=fck-c5 ver=0.1 reg=EU lim=ok caps=scan uptime_ms=900 rst=poweron"));
}

TEST_CASE("a key is there or not, and a list holds a word only to the letter", "[link]")
{
    fck_drv_link_message_t m = msg("fw=fck-nrf ver=0.1.0 caps=spec,echoidle,eu,smp tail quoted=\"a b\"");
    TEST_ASSERT_TRUE(fck_drv_link_message_has(&m, "caps"));
    TEST_ASSERT_TRUE(fck_drv_link_message_has(&m, "fw"));
    TEST_ASSERT_TRUE(fck_drv_link_message_has(&m, "quoted"));
    TEST_ASSERT_FALSE(fck_drv_link_message_has(&m, "cap"));
    TEST_ASSERT_FALSE(fck_drv_link_message_has(&m, "capss"));
    TEST_ASSERT_FALSE_MESSAGE(fck_drv_link_message_has(&m, "tail"), "a bare word is not a key");
    TEST_ASSERT_FALSE(fck_drv_link_message_has(&m, NULL));
    TEST_ASSERT_FALSE(fck_drv_link_message_has(NULL, "caps"));

    TEST_ASSERT_TRUE(fck_drv_link_message_listed(&m, "caps", "spec"));
    TEST_ASSERT_TRUE(fck_drv_link_message_listed(&m, "caps", "echoidle"));
    TEST_ASSERT_TRUE(fck_drv_link_message_listed(&m, "caps", "eu"));
    TEST_ASSERT_TRUE(fck_drv_link_message_listed(&m, "caps", "smp"));
    TEST_ASSERT_FALSE_MESSAGE(fck_drv_link_message_listed(&m, "caps", "echo"), "echo is not echoidle");
    TEST_ASSERT_FALSE(fck_drv_link_message_listed(&m, "caps", "idle"));
    TEST_ASSERT_FALSE(fck_drv_link_message_listed(&m, "caps", "sp"));
    TEST_ASSERT_FALSE(fck_drv_link_message_listed(&m, "caps", "smp2"));
    TEST_ASSERT_FALSE(fck_drv_link_message_listed(&m, "caps", "spec,echoidle"));
    TEST_ASSERT_FALSE(fck_drv_link_message_listed(&m, "caps", ""));
    TEST_ASSERT_FALSE(fck_drv_link_message_listed(&m, "caps", NULL));
    TEST_ASSERT_FALSE(fck_drv_link_message_listed(&m, "nokey", "spec"));
    TEST_ASSERT_FALSE(fck_drv_link_message_listed(NULL, "caps", "spec"));
    TEST_ASSERT_TRUE_MESSAGE(fck_drv_link_message_listed(&m, "fw", "fck-nrf"), "a list of one");

    /* An empty list, empty words, and one far longer than any buffer. */
    m = msg("caps= x=1");
    TEST_ASSERT_TRUE(fck_drv_link_message_has(&m, "caps"));
    TEST_ASSERT_FALSE(fck_drv_link_message_listed(&m, "caps", "x"));
    m = msg("caps=,scan,,chan,");
    TEST_ASSERT_TRUE(fck_drv_link_message_listed(&m, "caps", "scan"));
    TEST_ASSERT_TRUE(fck_drv_link_message_listed(&m, "caps", "chan"));
    char long_line[FCK_DRV_LINK_LINE_MAX + 1];
    size_t n = (size_t)snprintf(long_line, sizeof(long_line), "caps=");
    while (n + 8U < sizeof(long_line)) {
        n += (size_t)snprintf(&long_line[n], sizeof(long_line) - n, "word%02u,", (unsigned)(n % 97U));
    }
    (void)snprintf(&long_line[n], sizeof(long_line) - n, "last");
    m = msg(long_line);
    TEST_ASSERT_TRUE(fck_drv_link_message_listed(&m, "caps", "last"));
    TEST_ASSERT_FALSE(fck_drv_link_message_listed(&m, "caps", "las"));
}
