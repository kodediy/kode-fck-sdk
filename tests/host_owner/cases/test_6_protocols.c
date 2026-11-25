/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_6_protocols.c
 * @brief The Sub-GHz facade (fck_subghz.c) over the real drivers: the fixed
 *        codes through fck.h's types, .ksub code files, and the one rule that
 *        holds above all of it - a rolling code is read, and never sent.
 *
 * The CC1101 is the model's (fake_cc1101.c): it records the runs it was asked
 * to key. What these cases check is what reaches it, and what does not.
 */
#include <string.h>

#include "unity.h"

#include "fck.h"
#include "fck_core.h"

#include "fake_world.h"

#define HZ 433920000U

typedef struct {
    fck_subghz_protocol_t proto;
    uint8_t               bits;
    uint32_t              key;
    uint16_t              te;
    const char           *name;      /* the protocol line of a .ksub code file      */
} spec_t;

static const spec_t SPECS[] = {
    { FCK_SUBGHZ_EV1527, 24U, 0xA1B2C3U, 350U, "ev1527" },
    { FCK_SUBGHZ_PT2262, 24U, 0x5155C3U, 436U, "pt2262" },
    { FCK_SUBGHZ_HT12E, 12U, 0x9C3U, 333U, "ht12e" },
};
#define SPEC_COUNT (sizeof(SPECS) / sizeof(SPECS[0]))

static fck_subghz_code_t code_of(const spec_t *s)
{
    return (fck_subghz_code_t){ .proto = s->proto, .bits = s->bits, .key = s->key, .te_us = s->te };
}

static int32_t s_us[16U * FCK_SUBGHZ_FRAME_RUNS + 1U];
static char    s_text[FCK_SUBGHZ_FILE_CODE_MAX];

TEST_CASE("the enum holds the three fixed codes and no rolling code", "[subghz]")
{
    TEST_ASSERT_EQUAL(0, FCK_SUBGHZ_EV1527);
    TEST_ASSERT_EQUAL(1, FCK_SUBGHZ_PT2262);
    TEST_ASSERT_EQUAL(2, FCK_SUBGHZ_HT12E);
    TEST_ASSERT_EQUAL(0, FCK_SUBGHZ_KEELOQ);
    TEST_ASSERT_EQUAL(1, FCK_SUBGHZ_SOMFY_RTS);
}

TEST_CASE("every fixed code encodes and decodes through the facade", "[subghz]")
{
    for (size_t i = 0U; i < SPEC_COUNT; i++) {
        fck_subghz_code_t const c = code_of(&SPECS[i]);
        size_t                  n = 0U;
        s_us[0]                   = -60000;   /* the quiet before the first frame */
        TEST_ASSERT_EQUAL(KODE_OK, fck_core_subghz_encode(&c, 3U, &s_us[1], sizeof(s_us) / sizeof(s_us[0]) - 1U, &n));
        fck_subghz_code_t out[2];
        size_t            got = 0U;
        TEST_ASSERT_EQUAL(KODE_OK, fck_core_subghz_decode(s_us, n + 1U, out, 2U, &got));
        TEST_ASSERT_EQUAL_size_t_MESSAGE(1, got, SPECS[i].name);
        TEST_ASSERT_EQUAL(SPECS[i].proto, out[0].proto);
        TEST_ASSERT_EQUAL_UINT8(SPECS[i].bits, out[0].bits);
        TEST_ASSERT_EQUAL_HEX32(SPECS[i].key, out[0].key);
        TEST_ASSERT_EQUAL_UINT16(SPECS[i].te, out[0].te_us);
        TEST_ASSERT_EQUAL_UINT8(3, out[0].repeats);
        TEST_ASSERT_LESS_OR_EQUAL_size_t(FCK_SUBGHZ_FRAMES_MAX * FCK_SUBGHZ_FRAME_RUNS, n);
    }
}

TEST_CASE("protocol_info: names, bits and a te that encodes and reads back, for a code written by hand", "[subghz]")
{
    fck_subghz_protocol_info_t info;
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_subghz_protocol_info(FCK_SUBGHZ_HT12E, &info));
    TEST_ASSERT_EQUAL_STRING("HT12E", info.name);
    TEST_ASSERT_EQUAL_STRING("ht12e", info.file_name);
    TEST_ASSERT_EQUAL_UINT8(12, info.bits[0]);
    TEST_ASSERT_EQUAL_UINT8(0, info.bits[1]);
    TEST_ASSERT_EQUAL_UINT16(333, info.te_us);
    TEST_ASSERT_EQUAL_UINT16(150, info.te_min_us);
    TEST_ASSERT_EQUAL_UINT16(449, info.te_max_us);
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_subghz_protocol_info(FCK_SUBGHZ_EV1527, &info));
    TEST_ASSERT_EQUAL_STRING("EV1527", info.name);
    TEST_ASSERT_EQUAL_STRING("ev1527", info.file_name);
    TEST_ASSERT_EQUAL_UINT8(24, info.bits[0]);
    TEST_ASSERT_EQUAL_UINT8(0, info.bits[1]);
    TEST_ASSERT_EQUAL_UINT16(390, info.te_us);
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_subghz_protocol_info(FCK_SUBGHZ_PT2262, &info));
    TEST_ASSERT_EQUAL_STRING("pt2262", info.file_name);

    /* Each protocol's bits and default te, as an app would fill a code in. */
    for (size_t i = 0U; i < SPEC_COUNT; i++) {
        TEST_ASSERT_EQUAL(KODE_OK, fck_core_subghz_protocol_info(SPECS[i].proto, &info));
        TEST_ASSERT_EQUAL_STRING(SPECS[i].name, info.file_name);
        TEST_ASSERT_TRUE((info.bits[0] == SPECS[i].bits) || (info.bits[1] == SPECS[i].bits));
        TEST_ASSERT_TRUE((info.te_us >= info.te_min_us) && (info.te_us <= info.te_max_us));
        fck_subghz_code_t c = code_of(&SPECS[i]);
        size_t            n = 0U;
        c.te_us             = info.te_us;
        s_us[0]             = -60000;
        TEST_ASSERT_EQUAL(KODE_OK, fck_core_subghz_encode(&c, 3U, &s_us[1], sizeof(s_us) / sizeof(s_us[0]) - 1U, &n));
        fck_subghz_code_t out[2];
        size_t            got = 0U;
        TEST_ASSERT_EQUAL(KODE_OK, fck_core_subghz_decode(s_us, n + 1U, out, 2U, &got));
        TEST_ASSERT_EQUAL_size_t_MESSAGE(1, got, info.name);
        TEST_ASSERT_EQUAL(SPECS[i].proto, out[0].proto);
        TEST_ASSERT_EQUAL_HEX32(SPECS[i].key, out[0].key);
        /* Just outside the window is refused. */
        c.te_us = (uint16_t)(info.te_max_us + 1U);
        TEST_ASSERT_EQUAL(KODE_DENIED, fck_core_subghz_encode(&c, 1U, s_us, sizeof(s_us) / sizeof(s_us[0]), &n));
        c.te_us = (uint16_t)(info.te_min_us - 1U);
        TEST_ASSERT_EQUAL(KODE_DENIED, fck_core_subghz_encode(&c, 1U, s_us, sizeof(s_us) / sizeof(s_us[0]), &n));
    }

    TEST_ASSERT_EQUAL(KODE_DENIED, fck_core_subghz_protocol_info((fck_subghz_protocol_t)3, &info));
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_core_subghz_protocol_info(FCK_SUBGHZ_HT12E, NULL));
}

TEST_CASE("a protocol, bits or key the SDK does not send is refused by encode and send_code", "[subghz]")
{
    size_t n = 0U;
    static const struct {
        fck_subghz_protocol_t proto;
        uint8_t               bits;
        uint32_t              key;
        uint16_t              te;
        const char           *why;
    } bad[] = {
        { FCK_SUBGHZ_HT12E, 24U, 1U, 333U, "HT12E 24" },
        { FCK_SUBGHZ_HT12E, 13U, 1U, 333U, "HT12E 13" },
        { FCK_SUBGHZ_HT12E, 10U, 1U, 333U, "HT12E 10" },
        { FCK_SUBGHZ_EV1527, 12U, 1U, 350U, "EV1527 12" },
        { FCK_SUBGHZ_PT2262, 12U, 1U, 350U, "PT2262 12" },
        { FCK_SUBGHZ_HT12E, 12U, 0x1000U, 333U, "a key over its bits" },
        /* The protocols this SDK no longer has, and KeeLoq and Somfy, have no
         * value in the enum: any number put there is refused whatever the bits. */
        { (fck_subghz_protocol_t)3, 12U, 1U, 320U, "a value 3: no protocol of the SDK's" },
        { (fck_subghz_protocol_t)7, 10U, 1U, 500U, "a value 7" },
        { (fck_subghz_protocol_t)8, 64U, 1U, 400U, "a value 8: KeeLoq's place" },
        { (fck_subghz_protocol_t)9, 56U, 1U, 640U, "a value 9: Somfy's place" },
        { (fck_subghz_protocol_t)255, 24U, 1U, 320U, "a value 255" },
    };
    sim_begin_fck(NULL, NULL);
    sim_cc1101_reset();
    for (size_t i = 0U; i < sizeof(bad) / sizeof(bad[0]); i++) {
        fck_subghz_code_t const c = { .proto = bad[i].proto, .bits = bad[i].bits, .key = bad[i].key, .te_us = bad[i].te };
        TEST_ASSERT_EQUAL_MESSAGE(KODE_DENIED, fck_core_subghz_encode(&c, 2U, s_us, sizeof(s_us) / sizeof(s_us[0]), &n), bad[i].why);
        TEST_ASSERT_EQUAL_MESSAGE(KODE_DENIED, fck_core_subghz_send_code(HZ, &c, 2U), bad[i].why);
        size_t len = 0U;
        TEST_ASSERT_EQUAL_MESSAGE(KODE_DENIED, fck_core_subghz_code_to_file(HZ, &c, s_text, sizeof(s_text), &len), bad[i].why);
    }
    /* A te out of the protocol's band, and a PT2262 pair 10, are refused where
     * the runs are made and where the file is written. */
    fck_subghz_code_t const slow = { .proto = FCK_SUBGHZ_HT12E, .bits = 12U, .key = 1U, .te_us = 500U };
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_core_subghz_encode(&slow, 2U, s_us, sizeof(s_us) / sizeof(s_us[0]), &n));
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_core_subghz_send_code(HZ, &slow, 2U));
    fck_subghz_code_t const pair10 = { .proto = FCK_SUBGHZ_PT2262, .bits = 24U, .key = 0xA1B2C3U, .te_us = 350U };
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_core_subghz_encode(&pair10, 2U, s_us, sizeof(s_us) / sizeof(s_us[0]), &n));
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_core_subghz_send_code(HZ, &pair10, 2U));
    size_t length = 0U;
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_core_subghz_code_to_file(HZ, &slow, s_text, sizeof(s_text), &length));
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_core_subghz_code_to_file(HZ, &pair10, s_text, sizeof(s_text), &length));
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, sim_cc1101.sends, "something reached the transmitter");
}

TEST_CASE("send_code keys a fixed code of any protocol, once, as encode writes it", "[subghz]")
{
    sim_begin_fck(NULL, NULL);
    for (size_t i = 0U; i < SPEC_COUNT; i++) {
        fck_subghz_code_t const c = code_of(&SPECS[i]);
        sim_cc1101_reset();
        TEST_ASSERT_EQUAL_MESSAGE(KODE_OK, fck_core_subghz_send_code(HZ, &c, 2U), SPECS[i].name);
        size_t n = 0U;
        TEST_ASSERT_EQUAL(KODE_OK, fck_core_subghz_encode(&c, 2U, s_us, sizeof(s_us) / sizeof(s_us[0]), &n));
        TEST_ASSERT_EQUAL_UINT32(1, sim_cc1101.sends);
        TEST_ASSERT_EQUAL_UINT32(HZ, sim_cc1101.hz);
        TEST_ASSERT_EQUAL_UINT8(1, sim_cc1101.repeats);
        TEST_ASSERT_EQUAL_size_t(n, sim_cc1101.runs);
        TEST_ASSERT_EQUAL_INT32_ARRAY(s_us, sim_cc1101.run, n);
    }
    /* EV1527 at te 100, 16 frames: the sync's low (31 te, 3.1 ms) only just reaches a gap, and the
     * job's rolling-code check still cuts the frames apart and sends them. */
    fck_subghz_code_t const fast = { .proto = FCK_SUBGHZ_EV1527, .bits = 24U, .key = 0xA1B2C3U, .te_us = 100U };
    sim_cc1101_reset();
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_subghz_send_code(HZ, &fast, FCK_SUBGHZ_FRAMES_MAX));
    TEST_ASSERT_EQUAL_UINT32(1, sim_cc1101.sends);
    /* Out of the bands the module may transmit on, nothing is keyed. */
    fck_subghz_code_t const c = code_of(&SPECS[2]);
    sim_cc1101_reset();
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_core_subghz_send_code(315000000U, &c, 2U));
    TEST_ASSERT_EQUAL_UINT32(0, sim_cc1101.sends);
}

TEST_CASE("every fixed code as a .ksub code file and back through the facade", "[subghz]")
{
    for (size_t i = 0U; i < SPEC_COUNT; i++) {
        fck_subghz_code_t const c   = code_of(&SPECS[i]);
        size_t                  len = 0U;
        TEST_ASSERT_EQUAL_MESSAGE(KODE_OK, fck_core_subghz_code_to_file(HZ, &c, s_text, sizeof(s_text), &len), SPECS[i].name);
        TEST_ASSERT_EQUAL_size_t(strlen(s_text), len);
        TEST_ASSERT_EQUAL_UINT32(0U, (uint32_t)(strncmp(s_text, "kode-subghz 1\n", 14U)));
        char want[40];
        (void)strcpy(want, "\nprotocol: ");
        (void)strcat(want, SPECS[i].name);
        (void)strcat(want, "\n");
        TEST_ASSERT_NOT_NULL_MESSAGE(strstr(s_text, want), s_text);
        TEST_ASSERT_NOT_NULL_MESSAGE(strstr(s_text, "\npulse: "), s_text);
        TEST_ASSERT_NOT_NULL_MESSAGE(strstr(s_text, "\nkind: code\n"), s_text);
        fck_subghz_file_t f;
        size_t            n = 77U;
        TEST_ASSERT_EQUAL(KODE_OK, fck_core_subghz_from_file(s_text, len, &f, NULL, 0U, &n));
        TEST_ASSERT_TRUE(f.is_code);
        TEST_ASSERT_EQUAL_UINT32(HZ, f.hz);
        TEST_ASSERT_EQUAL_size_t(0, n);
        TEST_ASSERT_EQUAL(SPECS[i].proto, f.code.proto);
        TEST_ASSERT_EQUAL_UINT8(SPECS[i].bits, f.code.bits);
        TEST_ASSERT_EQUAL_HEX32(SPECS[i].key, f.code.key);
        TEST_ASSERT_EQUAL_UINT16(SPECS[i].te, f.code.te_us);
    }
}

TEST_CASE("a .ksub code file: the pulse is needed and in its protocol's band, and only the SDK's protocols are read",
          "[subghz]")
{
    static const char HEAD[] = "kode-subghz 1\nkind: code\nfrequency: 433920000\nmodulation: ook\n";
    static const struct {
        const char *rest;
        bool        ok;
        uint16_t    te;
        const char *why;
    } cases[] = {
        { "protocol: ev1527\nbits: 24\ncode: a1b2c3\npulse: 350\n", true, 350U, "ev1527" },
        { "protocol: pt2262\nbits: 24\ncode: 5155c3\npulse: 436\n", true, 436U, "pt2262" },
        { "protocol: ht12e\nbits: 12\ncode: 9c3\npulse: 333\n", true, 333U, "ht12e" },
        { "# a comment\r\nprotocol: ht12e\r\nbits: 12\r\ncode: 9c3\r\npulse: 449\r\nnote: later\r\n", true, 449U, "comments, CRLF and a key it does not know" },
        { "protocol: ev1527\nbits: 24\ncode: a1b2c3\npulse: 100\n", true, 100U, "ev1527 at the shortest te" },
        { "protocol: ev1527\nbits: 24\ncode: a1b2c3\n", false, 0U, "ev1527 with no pulse" },
        { "protocol: ht12e\nbits: 12\ncode: 9c3\n", false, 0U, "ht12e with no pulse" },
        { "protocol: ht12e\nbits: 12\ncode: 9c3\npulse: 450\n", false, 0U, "ht12e over its band" },
        { "protocol: ht12e\nbits: 12\ncode: 9c3\npulse: 149\n", false, 0U, "ht12e under its band" },
        { "protocol: ev1527\nbits: 24\ncode: a1b2c3\npulse: 99\n", false, 0U, "ev1527 under its band" },
        { "protocol: ev1527\nbits: 24\ncode: a1b2c3\npulse: 80\n", false, 0U, "ev1527 at 80, the old shortest te" },
        { "protocol: ev1527\nbits: 24\ncode: a1b2c3\npulse: 1201\n", false, 0U, "ev1527 over its band" },
        { "protocol: ev1527\nbits: 12\ncode: 9c3\npulse: 350\n", false, 0U, "ev1527 12 bits" },
        { "protocol: ht12e\nbits: 24\ncode: a1b2c3\npulse: 333\n", false, 0U, "ht12e 24 bits" },
        { "protocol: pt2262\nbits: 24\ncode: 000002\npulse: 436\n", false, 0U, "a pt2262 pair 10" },
        { "protocol: ht12e\nbits: 12\ncode: 1abc\npulse: 333\n", false, 0U, "a code over its bits" },
        { "protocol: keeloq\nbits: 32\ncode: 12345678\npulse: 400\n", false, 0U, "keeloq" },
        { "protocol: somfy_rts\nbits: 32\ncode: 12345678\npulse: 640\n", false, 0U, "somfy_rts" },
        { "protocol: other\nbits: 12\ncode: abc\npulse: 320\n", false, 0U, "a protocol the SDK does not have" },
        { "protocol: EV1527\nbits: 24\ncode: a1b2c3\npulse: 350\n", false, 0U, "a name in capitals" },
        { "protocol: ev1527\nprotocol: ev1527\nbits: 24\ncode: a1b2c3\npulse: 350\n", false, 0U, "a key twice" },
    };
    for (size_t i = 0U; i < sizeof(cases) / sizeof(cases[0]); i++) {
        char t[400];
        TEST_ASSERT_LESS_THAN_size_t(sizeof(t), strlen(HEAD) + strlen(cases[i].rest) + 1U);
        (void)strcpy(t, HEAD);
        (void)strcat(t, cases[i].rest);
        fck_subghz_file_t   f  = { .hz = 12345U };
        size_t              n  = 4242U;
        kode_status_t const st = fck_core_subghz_from_file(t, strlen(t), &f, NULL, 0U, &n);
        TEST_ASSERT_EQUAL_MESSAGE(cases[i].ok ? KODE_OK : KODE_DENIED, st, cases[i].why);
        if (cases[i].ok) {
            TEST_ASSERT_TRUE_MESSAGE(f.is_code, cases[i].why);
            TEST_ASSERT_EQUAL_UINT16_MESSAGE(cases[i].te, f.code.te_us, cases[i].why);
        } else {
            TEST_ASSERT_EQUAL_UINT32_MESSAGE(12345U, f.hz, "a refused file wrote its output");
            TEST_ASSERT_EQUAL_size_t_MESSAGE(4242U, n, "a refused file wrote its output");
        }
    }
    /* Another version of the format is not one this reader knows. */
    static const char V2[] = "kode-subghz 2\nkind: code\nfrequency: 433920000\nmodulation: ook\n"
                             "protocol: ev1527\nbits: 24\ncode: a1b2c3\npulse: 350\n";
    fck_subghz_file_t f;
    size_t            n;
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_core_subghz_from_file(V2, strlen(V2), &f, NULL, 0U, &n));
    /* And a code with no te, written: all three need one. */
    size_t len;
    for (size_t i = 0U; i < SPEC_COUNT; i++) {
        fck_subghz_code_t c = code_of(&SPECS[i]);
        c.te_us             = 0U;
        TEST_ASSERT_EQUAL_MESSAGE(KODE_DENIED, fck_core_subghz_code_to_file(HZ, &c, s_text, sizeof(s_text), &len), SPECS[i].name);
    }
}

/* ---- Rolling codes ---------------------------------------------------------------------- */

static int32_t s_cap[1200];
static size_t  s_cap_n;

static void put(int32_t v)
{
    if (s_cap_n < sizeof(s_cap) / sizeof(s_cap[0])) {
        s_cap[s_cap_n++] = v;
    }
}

/* KeeLoq as Microchip's HCS301 datasheet draws it, 66 bits, te 400. */
static void keeloq_capture(uint32_t fix, uint32_t hop)
{
    uint64_t const word = ((uint64_t)fix << 32) | hop;
    put(-60000);
    for (unsigned i = 0U; i < 11U; i++) {
        put(400);
        put(-400);
    }
    put(400);
    put(-4000);
    for (unsigned i = 0U; i < 66U; i++) {
        bool const one = (i < 64U) ? (((word >> i) & 1U) != 0U) : (i == 64U);
        put(one ? 400 : 800);
        put((i == 65U) ? -16000 : (one ? -800 : -400));
    }
}

/* A level of @p us microseconds, joined to the run before it when that is the same level, as a
 * capture has it. */
static void put_level(bool high, uint32_t us)
{
    if (s_cap_n > 0U && ((s_cap[s_cap_n - 1U] > 0) == high)) {
        s_cap[s_cap_n - 1U] += high ? (int32_t)us : -(int32_t)us;
    } else {
        put(high ? (int32_t)us : -(int32_t)us);
    }
}

/* The frame of a Somfy RTS remote on the air, laid out as the Pushstack article describes it:
 * key 0xA7, button 2, counter 0x0102, serial 0x123456, the nibble checksum in the button's byte, each
 * byte then XORed with the one before. */
static uint64_t somfy_frame(void)
{
    uint8_t f[7] = { 0xA7U, 0x20U, 0x01U, 0x02U, 0x12U, 0x34U, 0x56U };
    uint8_t checksum = 0U;
    for (unsigned i = 0U; i < 7U; i++) {
        checksum ^= (uint8_t)(f[i] ^ (f[i] >> 4));
    }
    f[1] |= (uint8_t)(checksum & 0x0FU);
    for (unsigned i = 1U; i < 7U; i++) {
        f[i] ^= f[i - 1U];
    }
    uint64_t v = 0U;
    for (unsigned i = 0U; i < 7U; i++) {
        v = (v << 8) | f[i];
    }
    return v;
}

/* Two frames at te 640: the first with 2 hardware sync pulses, the second with 7. */
static void somfy_capture(void)
{
    uint64_t const data = somfy_frame();
    put(-60000);
    for (unsigned f = 0U; f < 2U; f++) {
        for (unsigned i = 0U; i < ((f == 0U) ? 2U : 7U); i++) {
            put_level(true, 2560U);
            put_level(false, 2560U);
        }
        put_level(true, 4800U);
        put_level(false, 640U);
        for (unsigned b = 56U; b > 0U; b--) {
            bool const one = ((data >> (b - 1U)) & 1U) != 0U;
            put_level(!one, 640U);
            put_level(one, 640U);
        }
        put_level(false, 32000U);
    }
}

TEST_CASE("fck_subghz_decode_rolling shows a KeeLoq's serial and button and a Somfy's counter", "[subghz][rolling]")
{
    fck_subghz_rolling_t r[3];
    size_t               got = 0U;
    s_cap_n = 0U;
    keeloq_capture((0x5U << 28) | 0x0ABCDEFU, 0xDEADBEEFU);
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_subghz_decode_rolling(s_cap, s_cap_n, r, 3U, &got));
    TEST_ASSERT_EQUAL_size_t(1, got);
    TEST_ASSERT_EQUAL(FCK_SUBGHZ_KEELOQ, r[0].proto);
    TEST_ASSERT_EQUAL_UINT8(66, r[0].bits);
    TEST_ASSERT_EQUAL_HEX32(0x0ABCDEFU, r[0].serial);
    TEST_ASSERT_EQUAL_UINT8(5, r[0].button);
    TEST_ASSERT_EQUAL_HEX32(0xDEADBEEFU, r[0].hop);
    TEST_ASSERT_TRUE(r[0].battery_low);
    TEST_ASSERT_FALSE(r[0].counter_read);
    TEST_ASSERT_EQUAL_UINT8(1, r[0].repeats);
    s_cap_n = 0U;
    somfy_capture();
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_subghz_decode_rolling(s_cap, s_cap_n, r, 3U, &got));
    TEST_ASSERT_EQUAL_size_t(1, got);
    TEST_ASSERT_EQUAL(FCK_SUBGHZ_SOMFY_RTS, r[0].proto);
    TEST_ASSERT_EQUAL_UINT8(56, r[0].bits);
    TEST_ASSERT_EQUAL_HEX32(0x123456U, r[0].serial);
    TEST_ASSERT_EQUAL_UINT8(2, r[0].button);
    TEST_ASSERT_EQUAL_UINT16(0x0102U, r[0].counter);
    TEST_ASSERT_TRUE(r[0].counter_read);
    TEST_ASSERT_EQUAL_HEX64(somfy_frame(), r[0].data);
    TEST_ASSERT_EQUAL_UINT8(2, r[0].repeats);
    /* The fixed reader finds nothing in either. */
    fck_subghz_code_t c[2];
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_subghz_decode(s_cap, s_cap_n, c, 2U, &got));
    TEST_ASSERT_EQUAL_size_t(0, got);
    /* Arguments. */
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_core_subghz_decode_rolling(s_cap, s_cap_n, NULL, 3U, &got));
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_core_subghz_decode_rolling(s_cap, s_cap_n, r, 0U, &got));
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_core_subghz_decode_rolling(s_cap, s_cap_n, r, 3U, NULL));
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_core_subghz_decode_rolling(NULL, 5U, r, 3U, &got));
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_subghz_decode_rolling(NULL, 0U, r, 3U, &got));
    TEST_ASSERT_EQUAL_size_t(0, got);
}

TEST_CASE("send_raw refuses a capture that reads as a rolling code, and keys the rest", "[subghz][rolling]")
{
    sim_begin_fck(NULL, NULL);
    sim_cc1101_reset();
    s_cap_n = 0U;
    keeloq_capture((0x5U << 28) | 0x0ABCDEFU, 0xDEADBEEFU);
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_core_subghz_send_raw(HZ, s_cap, s_cap_n, 1U));
    s_cap_n = 0U;
    somfy_capture();
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_core_subghz_send_raw(HZ, s_cap, s_cap_n, 1U));
    /* Behind a fixed code in the same capture it is refused too. */
    size_t n = 0U;
    fck_subghz_code_t const fixed = code_of(&SPECS[0]);
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_subghz_encode(&fixed, 2U, s_us, sizeof(s_us) / sizeof(s_us[0]), &n));
    memcpy(s_cap, s_us, n * sizeof(int32_t));
    s_cap_n = n;
    keeloq_capture((0x5U << 28) | 0x0ABCDEFU, 0xDEADBEEFU);
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_core_subghz_send_raw(HZ, s_cap, s_cap_n, 1U));
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, sim_cc1101.sends, "a rolling code reached the transmitter");
    /* The fixed codes' captures are sent. */
    for (size_t i = 0U; i < SPEC_COUNT; i++) {
        fck_subghz_code_t const c = code_of(&SPECS[i]);
        s_us[0]                   = -60000;
        TEST_ASSERT_EQUAL(KODE_OK, fck_core_subghz_encode(&c, 3U, &s_us[1], sizeof(s_us) / sizeof(s_us[0]) - 1U, &n));
        sim_cc1101_reset();
        TEST_ASSERT_EQUAL_MESSAGE(KODE_OK, fck_core_subghz_send_raw(HZ, s_us, n + 1U, 2U), SPECS[i].name);
        TEST_ASSERT_EQUAL_UINT32(1, sim_cc1101.sends);
        TEST_ASSERT_EQUAL_size_t(n + 1U, sim_cc1101.runs);
        TEST_ASSERT_EQUAL_UINT8(2, sim_cc1101.repeats);
    }
}

static char    s_sub[8192];
static int32_t s_back[1200];

TEST_CASE("a KeeLoq RAW .ksub file, read back through the facade, is refused by send_raw", "[subghz][rolling]")
{
    sim_begin_fck(NULL, NULL);
    sim_cc1101_reset();
    s_cap_n = 0U;
    keeloq_capture((0x5U << 28) | 0x0ABCDEFU, 0xDEADBEEFU);
    size_t        length = 0U;
    kode_status_t st     = fck_core_subghz_to_file(HZ, s_cap, s_cap_n, s_sub, sizeof(s_sub), &length);
    TEST_ASSERT_EQUAL(KODE_OK, st);
    fck_subghz_file_t file;
    size_t                    n = 0U;
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_subghz_from_file(s_sub, length, &file, s_back,
                                                             sizeof(s_back) / sizeof(s_back[0]), &n));
    TEST_ASSERT_FALSE(file.is_code);
    TEST_ASSERT_EQUAL_size_t(s_cap_n, n);
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_core_subghz_send_raw(file.hz, s_back, n, 1U));
    /* Cut at the head - the preamble gone, the header gone - it is a long PWM frame. */
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_core_subghz_send_raw(file.hz, &s_back[26], n - 26U, 1U));
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, sim_cc1101.sends, "a rolling code reached the transmitter");
}

TEST_CASE("is_rolling says what send_raw refuses: a KeeLoq, whole or cut, and no fixed code", "[subghz][rolling]")
{
    s_cap_n = 0U;
    keeloq_capture((0x5U << 28) | 0x0ABCDEFU, 0xDEADBEEFU);
    TEST_ASSERT_TRUE(fck_subghz_is_rolling(s_cap, s_cap_n));
    TEST_ASSERT_TRUE(fck_subghz_is_rolling(&s_cap[26], s_cap_n - 26U));
    for (size_t i = 0U; i < SPEC_COUNT; i++) {
        fck_subghz_code_t const c = code_of(&SPECS[i]);
        size_t                  n = 0U;
        TEST_ASSERT_EQUAL(KODE_OK, fck_core_subghz_encode(&c, 5U, s_us, sizeof(s_us) / sizeof(s_us[0]), &n));
        TEST_ASSERT_FALSE_MESSAGE(fck_subghz_is_rolling(s_us, n), SPECS[i].name);
    }
    TEST_ASSERT_FALSE(fck_subghz_is_rolling(NULL, 10U));
    TEST_ASSERT_FALSE(fck_subghz_is_rolling(s_cap, 0U));
}

TEST_CASE("send_code refuses a te outside its protocol's band before it allocates or keys", "[subghz]")
{
    sim_begin_fck(NULL, NULL);
    sim_cc1101_reset();
    fck_subghz_code_t c = code_of(&SPECS[2]);
    TEST_ASSERT_EQUAL(KODE_OK, fck_core_subghz_send_code(HZ, &c, 1U));
    TEST_ASSERT_EQUAL_UINT32(1, sim_cc1101.sends);
    sim_cc1101_reset();
    static const uint16_t bad[] = { 0U, 5U, 79U, 149U, 450U, 1201U, 65535U };
    for (size_t i = 0U; i < sizeof(bad) / sizeof(bad[0]); i++) {
        c.te_us = bad[i];
        TEST_ASSERT_EQUAL_MESSAGE(KODE_DENIED, fck_core_subghz_send_code(HZ, &c, 1U), "a bad te");
    }
    TEST_ASSERT_EQUAL_UINT32(0, sim_cc1101.sends);
}
