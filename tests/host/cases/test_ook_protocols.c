/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_ook_protocols.c
 * @brief fck_drv_ook: the fixed codes EV1527, PT2262 and HT12E written and
 *        read, and the two rolling codes KeeLoq and Somfy RTS read.
 *
 * Two kinds of case. The first holds the fixed codes' reader and encoder, and
 * the sending path's detector, to golden vectors (ook_gold.inc): what the
 * implementation that this one replaced answered, through its public header
 * only, for 693 captures and 158 calls of the encoder (ook_scenarios.h
 * rebuilds the captures). That is a non-regression check, with one
 * difference by design: a te under 100 us is no longer read or written. The
 * detector refuses every capture the old one refused and no other. Nothing of
 * the old KeeLoq and Somfy readers is
 * kept: the second kind of case states each protocol's rules from its data
 * sheet or article (ook_frames.h lays the frames out independently of the code
 * under test, from the sources named there) and holds the readers to those.
 * What must never happen: a frame read as the wrong protocol, a rolling code
 * that can be written, a frame read with a bit count the protocol does not
 * send, a Somfy frame whose checksum is wrong taken whole, and a capture that
 * is a rolling code or looks like one let through the sending path's
 * detector.
 */
#include <string.h>

#include "unity.h"

#include "fck_drv_ook.h"

#include "ook_frames.h"
#include "ook_gold.h"
#include "ook_gold.inc"
#include "ook_scenarios.h"

static int32_t s_runs[4096];

/* ---- The golden vectors ---------------------------------------------------------------- */

static uint32_t fnv1a(const int32_t *runs, size_t n)
{
    uint32_t h = 2166136261U;
    for (size_t i = 0U; i < n; i++) {
        uint32_t const x = (uint32_t)runs[i];
        for (unsigned k = 0U; k < 4U; k++) {
            h ^= (x >> (8U * k)) & 0xFFU;
            h *= 16777619U;
        }
    }
    return h;
}

TEST_CASE("golden: the encoder writes, for 158 calls, the runs the previous one wrote (a te under 100 us, nothing)",
          "[ook][gold]")
{
    size_t const count = sizeof(GOLD_ENCODE) / sizeof(GOLD_ENCODE[0]);
    TEST_ASSERT_EQUAL_UINT32(158U, count);
    size_t refused = 0U;
    for (size_t i = 0U; i < count; i++) {
        gold_encode_t const *e = &GOLD_ENCODE[i];
        if (e->te_us < FCK_DRV_OOK_TE_MIN_US) {
            /* The one difference by design: under 100 us a frame's sync is shorter than the gap that ends it. */
            TEST_ASSERT_EQUAL_UINT32(0U, fck_drv_ook_encode((fck_drv_ook_protocol_t)e->proto, (uint8_t)e->bits,
                                                            e->key, (uint16_t)e->te_us, e->frames, s_runs, e->cap));
            refused++;
            continue;
        }
        for (size_t k = 0U; k < 2048U; k++) {
            s_runs[k] = 0x7EADBEEF;
        }
        size_t const n = fck_drv_ook_encode((fck_drv_ook_protocol_t)e->proto, (uint8_t)e->bits, e->key,
                                            (uint16_t)e->te_us, e->frames, s_runs, e->cap);
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(e->runs, n, "the number of runs");
        if (n != 0U) {
            TEST_ASSERT_EQUAL_HEX32_MESSAGE(e->hash, fnv1a(s_runs, n), "the runs");
        }
        if (n < 2048U) {
            TEST_ASSERT_EQUAL_INT32_MESSAGE(0x7EADBEEF, s_runs[n], "nothing is written past the runs");
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(refused > 0U, "the golden calls do include te under 100 us");
}

static int s_scenario;
static int s_fixed_bad, s_shape_lost, s_shape_gained, s_checked;

static void check_scenario(void *context, const char *what, const int32_t *runs, size_t n)
{
    (void)context;
    gold_scenario_t const *g = &GOLD_SCENARIO[s_scenario];
    s_checked++;
    if (!g->skip_fixed) {
        /* The answers of the previous reader. A te under FCK_DRV_OOK_TE_MIN_US is no longer read, and a capture
         * whose frames are within 15 % of it (jitter) has some frames over the line and some under, so for those
         * the new reader may read fewer frames, and fewer codes, but nothing the old one did not read. */
        bool near_edge = false;
        for (size_t i = 0U; i < g->fixed_n; i++) {
            near_edge = near_edge || g->fixed[i].te_us < (FCK_DRV_OOK_TE_MIN_US * 115U) / 100U;
        }
        fck_drv_ook_code_t got[4];
        memset(got, 0, sizeof(got));
        size_t const k  = fck_drv_ook_decode(runs, n, got, 4U);
        bool         ok = near_edge ? (k <= g->fixed_n) : (k == g->fixed_n);
        for (size_t i = 0U; ok && i < k; i++) {
            if (near_edge) {
                bool found = false;
                for (size_t w = 0U; w < g->fixed_n; w++) {
                    found = found || (((int)got[i].proto == g->fixed[w].proto) && got[i].bits == g->fixed[w].bits
                                      && got[i].key == g->fixed[w].key && got[i].frames <= g->fixed[w].frames);
                }
                ok = found;
            } else {
                ok = ((int)got[i].proto == g->fixed[i].proto) && got[i].bits == g->fixed[i].bits
                     && got[i].key == g->fixed[i].key && got[i].te_us == g->fixed[i].te_us
                     && got[i].frames == g->fixed[i].frames;
            }
        }
        if (!ok) {
            s_fixed_bad++;
            TEST_MESSAGE(what);
        }
    }
    int const shaped = fck_drv_ook_rolling(runs, n) ? 1 : 0;
    if (g->shaped == 1 && shaped == 0) {
        s_shape_lost++;   /* a capture the previous one refused to send and this one lets through */
        TEST_MESSAGE(what);
    }
    if (g->shaped == 0 && shaped == 1) {
        s_shape_gained++;
        TEST_MESSAGE(what);
    }
    s_scenario++;
}

TEST_CASE("golden: 693 captures read as before, fixed codes, and the sending path's detector refuses all it refused",
          "[ook][gold][rolling][shape]")
{
    TEST_ASSERT_EQUAL_UINT32(693U, sizeof(GOLD_SCENARIO) / sizeof(GOLD_SCENARIO[0]));
    s_scenario = s_fixed_bad = s_shape_lost = s_shape_gained = s_checked = 0;
    ook_scenarios(check_scenario, NULL);
    TEST_ASSERT_EQUAL_INT(693, s_checked);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, s_fixed_bad, "captures whose fixed codes differ");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, s_shape_lost, "captures the previous detector refused and this one lets through");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, s_shape_gained, "captures this detector refuses and the previous one let through");
}

/* ---- The fixed codes: round trips and rules ---------------------------------------------- */

TEST_CASE("every fixed code, encoded and read back: its protocol, bits, key and te", "[ook][protocols]")
{
    static const struct {
        fck_drv_ook_protocol_t proto;
        uint8_t                bits;
        uint32_t               key;
        uint16_t               te;
    } CASES[] = {
        { FCK_DRV_OOK_EV1527, 24U, 0x5155C3U + 0x100000U, 390U },
        { FCK_DRV_OOK_EV1527, 24U, 0xA1B2C3U, 100U },   /* at 80 us the sync is 2.5 ms: no gap, one frame */
        { FCK_DRV_OOK_EV1527, 24U, 0x123456U, 1200U },
        { FCK_DRV_OOK_PT2262, 24U, 0x5155C3U, 436U },
        { FCK_DRV_OOK_PT2262, 24U, 0x555555U, 250U },
        { FCK_DRV_OOK_HT12E, 12U, 0x9C3U, 333U },
        { FCK_DRV_OOK_HT12E, 12U, 0x001U, 150U },
        { FCK_DRV_OOK_HT12E, 12U, 0xFFFU, 449U },
    };
    for (size_t i = 0U; i < sizeof(CASES) / sizeof(CASES[0]); i++) {
        size_t const n = fck_drv_ook_encode(CASES[i].proto, CASES[i].bits, CASES[i].key, CASES[i].te, 3U, s_runs, 4096U);
        TEST_ASSERT_GREATER_THAN_UINT32(0U, n);
        int32_t capture[4096];
        size_t  m = 0U;
        capture[m++] = -60000;   /* the quiet before the first frame */
        for (size_t k = 0U; k < n; k++) {
            capture[m++] = s_runs[k];
        }
        fck_drv_ook_code_t code;
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(1U, fck_drv_ook_decode(capture, m, &code, 1U), fck_drv_ook_name(CASES[i].proto));
        TEST_ASSERT_EQUAL_INT(CASES[i].proto, code.proto);
        TEST_ASSERT_EQUAL_UINT8(CASES[i].bits, code.bits);
        TEST_ASSERT_EQUAL_HEX32(CASES[i].key, code.key);
        TEST_ASSERT_EQUAL_UINT16(CASES[i].te, code.te_us);
        TEST_ASSERT_EQUAL_UINT8(3U, code.frames);
    }
}

TEST_CASE("every fixed code with 15 % jitter", "[ook][protocols]")
{
    for (unsigned seed = 0U; seed < 6U; seed++) {
        ookb_t b;
        ookb_init(&b, s_runs, 4096U, 15U);
        b.lcg = seed * 31U + 5U;
        ookb_raw(&b, -60000);
        for (unsigned f = 0U; f < 4U; f++) {
            ookb_ht12e_word(&b, 333U, 0x5A5U, 36U);
        }
        fck_drv_ook_code_t code;
        TEST_ASSERT_EQUAL_UINT32(1U, fck_drv_ook_decode(s_runs, b.n, &code, 1U));
        TEST_ASSERT_EQUAL_INT(FCK_DRV_OOK_HT12E, code.proto);
        TEST_ASSERT_EQUAL_HEX32(0x5A5U, code.key);
    }
}

TEST_CASE("HT12E: a pilot of 36 te comes before the word, 15 te or more is one, 14 is not, and a word at the "
          "capture's first run has none",
          "[ook][protocols]")
{
    static const uint32_t pilots[] = { 36U, 15U, 20U, 60U, 400U };
    for (size_t i = 0U; i < sizeof(pilots) / sizeof(pilots[0]); i++) {
        ookb_t b;
        ookb_init(&b, s_runs, 4096U, 0U);
        for (unsigned f = 0U; f < 2U; f++) {
            ookb_ht12e_word(&b, 333U, 0x9C3U, pilots[i]);
        }
        fck_drv_ook_code_t code;
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(1U, fck_drv_ook_decode(s_runs, b.n, &code, 1U), "a pilot");
        TEST_ASSERT_EQUAL_HEX32(0x9C3U, code.key);
        TEST_ASSERT_EQUAL_UINT8(2U, code.frames);
    }
    ookb_t b;
    ookb_init(&b, s_runs, 4096U, 0U);
    for (unsigned f = 0U; f < 3U; f++) {
        ookb_ht12e_word(&b, 333U, 0x9C3U, 14U);
    }
    fck_drv_ook_code_t code;
    TEST_ASSERT_EQUAL_UINT32(0U, fck_drv_ook_decode(s_runs, b.n, &code, 1U));
}

TEST_CASE("HT12E: a word of 11 or 13 bits is not read, and a start pulse of 1.5 or 0.5 te is not a start",
          "[ook][protocols]")
{
    for (unsigned bits = 10U; bits <= 14U; bits++) {
        ookb_t b;
        ookb_init(&b, s_runs, 4096U, 0U);
        ookb_raw(&b, -60000);
        for (unsigned f = 0U; f < 3U; f++) {
            ookb_level(&b, false, 36U * 333U);
            ookb_level(&b, true, 333U);
            for (unsigned i = 0U; i < bits; i++) {
                bool const one = (i & 1U) != 0U;
                ookb_level(&b, false, one ? 2U * 333U : 333U);
                ookb_level(&b, true, one ? 333U : 2U * 333U);
            }
        }
        fck_drv_ook_code_t code;
        TEST_ASSERT_EQUAL_UINT32_MESSAGE((bits == 12U) ? 1U : 0U, fck_drv_ook_decode(s_runs, b.n, &code, 1U), "bits");
    }
    static const uint32_t starts[] = { 500U, 166U };   /* 1.5 te and 0.5 te */
    for (size_t i = 0U; i < 2U; i++) {
        ookb_t b;
        ookb_init(&b, s_runs, 4096U, 0U);
        ookb_raw(&b, -60000);
        for (unsigned f = 0U; f < 3U; f++) {
            ookb_level(&b, false, 36U * 333U);
            ookb_level(&b, true, starts[i]);
            for (unsigned k = 0U; k < 12U; k++) {
                bool const one = (k & 1U) != 0U;
                ookb_level(&b, false, one ? 2U * 333U : 333U);
                ookb_level(&b, true, one ? 333U : 2U * 333U);
            }
        }
        fck_drv_ook_code_t code;
        TEST_ASSERT_EQUAL_UINT32(0U, fck_drv_ook_decode(s_runs, b.n, &code, 1U));
    }
}

TEST_CASE("a bit count the protocol does not send is not read: 23 and 25 pairs of an EV1527", "[ook][protocols]")
{
    for (unsigned bits = 22U; bits <= 26U; bits++) {
        ookb_t b;
        ookb_init(&b, s_runs, 4096U, 0U);
        ookb_raw(&b, -60000);
        for (unsigned f = 0U; f < 3U; f++) {
            for (unsigned i = 0U; i < bits; i++) {
                bool const one = (i & 1U) != 0U;
                ookb_level(&b, true, one ? 3U * 390U : 390U);
                ookb_level(&b, false, one ? 390U : 3U * 390U);
            }
            ookb_level(&b, true, 390U);
            ookb_level(&b, false, 31U * 390U);
        }
        fck_drv_ook_code_t code;
        TEST_ASSERT_EQUAL_UINT32_MESSAGE((bits == 24U) ? 1U : 0U, fck_drv_ook_decode(s_runs, b.n, &code, 1U), "pairs");
    }
}

TEST_CASE("two remotes of two protocols in one capture: the code most frames carry comes first", "[ook][protocols]")
{
    ookb_t b;
    ookb_init(&b, s_runs, 4096U, 0U);
    ookb_raw(&b, -60000);
    int32_t ev[200];
    size_t  evn = fck_drv_ook_encode(FCK_DRV_OOK_EV1527, 24U, 0xA1B2C3U, 390U, 2U, ev, 200U);
    int32_t pt[200];
    size_t  ptn = fck_drv_ook_encode(FCK_DRV_OOK_PT2262, 24U, 0x5155C3U, 436U, 4U, pt, 200U);
    size_t  n   = 0U;
    s_runs[n++] = -60000;
    for (size_t i = 0U; i < evn; i++) {
        s_runs[n++] = ev[i];
    }
    for (size_t i = 0U; i < ptn; i++) {
        s_runs[n++] = pt[i];
    }
    fck_drv_ook_code_t codes[3];
    TEST_ASSERT_EQUAL_UINT32(2U, fck_drv_ook_decode(s_runs, n, codes, 3U));
    TEST_ASSERT_EQUAL_INT(FCK_DRV_OOK_PT2262, codes[0].proto);
    TEST_ASSERT_EQUAL_UINT8(4U, codes[0].frames);
    TEST_ASSERT_EQUAL_INT(FCK_DRV_OOK_EV1527, codes[1].proto);
    TEST_ASSERT_EQUAL_UINT8(2U, codes[1].frames);
}

TEST_CASE("the protocols' names, bits and te windows", "[ook][protocols]")
{
    TEST_ASSERT_EQUAL_STRING("ev1527", fck_drv_ook_name(FCK_DRV_OOK_EV1527));
    TEST_ASSERT_EQUAL_STRING("pt2262", fck_drv_ook_name(FCK_DRV_OOK_PT2262));
    TEST_ASSERT_EQUAL_STRING("ht12e", fck_drv_ook_name(FCK_DRV_OOK_HT12E));
    TEST_ASSERT_NULL(fck_drv_ook_name((fck_drv_ook_protocol_t)3));
    TEST_ASSERT_NULL(fck_drv_ook_name((fck_drv_ook_protocol_t)-1));

    fck_drv_ook_protocol_t p = FCK_DRV_OOK_EV1527;
    TEST_ASSERT_TRUE(fck_drv_ook_from_name("pt2262", 24U, 0x5155C3U, &p));
    TEST_ASSERT_EQUAL_INT(FCK_DRV_OOK_PT2262, p);
    TEST_ASSERT_TRUE(fck_drv_ook_from_name("ht12e", 12U, 0xFFFU, &p));
    TEST_ASSERT_EQUAL_INT(FCK_DRV_OOK_HT12E, p);
    TEST_ASSERT_FALSE(fck_drv_ook_from_name("ht12e", 24U, 1U, &p));     /* a bit count it does not send  */
    TEST_ASSERT_FALSE(fck_drv_ook_from_name("ev1527", 24U, 0x1000000U, &p));   /* a key over its bits */
    TEST_ASSERT_FALSE(fck_drv_ook_from_name("pt2262", 24U, 0x000002U, &p));    /* a pair 10: no PT2262 sends it */
    TEST_ASSERT_FALSE(fck_drv_ook_from_name("EV1527", 24U, 1U, &p));    /* the names are lowercase        */
    TEST_ASSERT_FALSE(fck_drv_ook_from_name("other", 12U, 1U, &p));     /* a protocol the SDK does not have */
    TEST_ASSERT_FALSE(fck_drv_ook_from_name(NULL, 24U, 1U, &p));
    TEST_ASSERT_FALSE(fck_drv_ook_from_name("ev1527", 24U, 1U, NULL));

    uint16_t lo, hi, typical;
    TEST_ASSERT_TRUE(fck_drv_ook_te_range(FCK_DRV_OOK_EV1527, &lo, &hi, &typical));
    TEST_ASSERT_EQUAL_UINT16(100U, lo);
    TEST_ASSERT_EQUAL_UINT16(1200U, hi);
    TEST_ASSERT_EQUAL_UINT16(390U, typical);
    TEST_ASSERT_TRUE(fck_drv_ook_te_range(FCK_DRV_OOK_HT12E, &lo, &hi, &typical));
    TEST_ASSERT_EQUAL_UINT16(150U, lo);
    TEST_ASSERT_EQUAL_UINT16(449U, hi);
    TEST_ASSERT_EQUAL_UINT16(333U, typical);   /* 3 kHz, the datasheet's typical oscillator */
    TEST_ASSERT_FALSE(fck_drv_ook_te_range((fck_drv_ook_protocol_t)7, &lo, &hi, &typical));
    TEST_ASSERT_FALSE(fck_drv_ook_te_range(FCK_DRV_OOK_EV1527, NULL, &hi, &typical));

    TEST_ASSERT_TRUE(fck_drv_ook_bits_ok(FCK_DRV_OOK_HT12E, 12U));
    TEST_ASSERT_FALSE(fck_drv_ook_bits_ok(FCK_DRV_OOK_HT12E, 24U));
    TEST_ASSERT_TRUE(fck_drv_ook_bits_ok(FCK_DRV_OOK_PT2262, 24U));
    TEST_ASSERT_FALSE(fck_drv_ook_bits_ok(FCK_DRV_OOK_PT2262, 12U));
    TEST_ASSERT_FALSE(fck_drv_ook_bits_ok((fck_drv_ook_protocol_t)9, 24U));
    TEST_ASSERT_TRUE(fck_drv_ook_te_ok(FCK_DRV_OOK_HT12E, 449U));
    TEST_ASSERT_FALSE(fck_drv_ook_te_ok(FCK_DRV_OOK_HT12E, 450U));
    TEST_ASSERT_FALSE(fck_drv_ook_te_ok(FCK_DRV_OOK_HT12E, 149U));
    TEST_ASSERT_TRUE(fck_drv_ook_te_ok(FCK_DRV_OOK_EV1527, 100U));
    TEST_ASSERT_FALSE(fck_drv_ook_te_ok(FCK_DRV_OOK_EV1527, 99U));   /* 31 te under the 3 ms of a gap */
    TEST_ASSERT_FALSE(fck_drv_ook_te_ok(FCK_DRV_OOK_EV1527, 1201U));
}

TEST_CASE("encode holds to each protocol's bits, key, te and room, and writes nothing then", "[ook][protocols]")
{
    int32_t out[200];
    for (size_t i = 0U; i < 200U; i++) {
        out[i] = 0x7EADBEEF;
    }
    TEST_ASSERT_EQUAL_UINT32(0U, fck_drv_ook_encode(FCK_DRV_OOK_HT12E, 24U, 1U, 333U, 1U, out, 200U));
    TEST_ASSERT_EQUAL_UINT32(0U, fck_drv_ook_encode(FCK_DRV_OOK_HT12E, 12U, 0x1000U, 333U, 1U, out, 200U));
    TEST_ASSERT_EQUAL_UINT32(0U, fck_drv_ook_encode(FCK_DRV_OOK_HT12E, 12U, 1U, 450U, 1U, out, 200U));
    TEST_ASSERT_EQUAL_UINT32(0U, fck_drv_ook_encode(FCK_DRV_OOK_EV1527, 24U, 1U, 79U, 1U, out, 200U));
    TEST_ASSERT_EQUAL_UINT32(0U, fck_drv_ook_encode(FCK_DRV_OOK_PT2262, 24U, 0x000002U, 390U, 1U, out, 200U));
    TEST_ASSERT_EQUAL_UINT32(0U, fck_drv_ook_encode(FCK_DRV_OOK_EV1527, 24U, 1U, 390U, 0U, out, 200U));
    TEST_ASSERT_EQUAL_UINT32(0U, fck_drv_ook_encode(FCK_DRV_OOK_EV1527, 24U, 1U, 390U, 4U, out, 199U));   /* 4 frames of 50 */
    TEST_ASSERT_EQUAL_UINT32(0U, fck_drv_ook_encode(FCK_DRV_OOK_EV1527, 24U, 1U, 390U, 1U, NULL, 200U));
    TEST_ASSERT_EQUAL_UINT32(0U, fck_drv_ook_encode((fck_drv_ook_protocol_t)5, 24U, 1U, 390U, 1U, out, 200U));
    for (size_t i = 0U; i < 200U; i++) {
        TEST_ASSERT_EQUAL_INT32(0x7EADBEEF, out[i]);
    }
    TEST_ASSERT_EQUAL_UINT32(200U, fck_drv_ook_encode(FCK_DRV_OOK_EV1527, 24U, 1U, 390U, 4U, out, 200U));
    TEST_ASSERT_EQUAL_UINT32(4U * 26U, fck_drv_ook_encode(FCK_DRV_OOK_HT12E, 12U, 1U, 333U, 4U, out, 200U));
}

/* ---- The sources' own frames: KeeLoq and Somfy RTS -------------------------------------- */

static size_t keeloq_capture(uint32_t te, uint64_t word, unsigned bits, bool vlow, unsigned words, unsigned pct)
{
    ookb_t b;
    ookb_init(&b, s_runs, 4096U, pct);
    ookb_raw(&b, -60000);
    for (unsigned w = 0U; w < words; w++) {
        ookb_keeloq_word(&b, te, word, bits, vlow, 11U);
    }
    return b.n;
}

TEST_CASE("KeeLoq: serial, button and the hop as it came, from a frame laid out as the datasheet draws it",
          "[ook][rolling]")
{
    /* hop 0xDEADBEEF, serial 0x1234567, button 0b0101, the first bit sent the lowest. */
    uint64_t const word = ((uint64_t)0x5U << 60) | ((uint64_t)0x1234567U << 32) | 0xDEADBEEFU;
    size_t const   n    = keeloq_capture(400U, word, 66U, false, 3U, 0U);
    fck_drv_ook_rolling_t c[2];
    TEST_ASSERT_EQUAL_UINT32(1U, fck_drv_ook_decode_rolling(s_runs, n, c, 2U));
    TEST_ASSERT_EQUAL_INT(FCK_DRV_OOK_KEELOQ, c[0].proto);
    TEST_ASSERT_EQUAL_UINT8(66U, c[0].bits);
    TEST_ASSERT_EQUAL_HEX32(0x1234567U, c[0].serial);
    TEST_ASSERT_EQUAL_UINT8(0x5U, c[0].button);
    TEST_ASSERT_EQUAL_HEX32(0xDEADBEEFU, c[0].hop);
    TEST_ASSERT_FALSE(c[0].counter_read);
    TEST_ASSERT_FALSE(c[0].battery_low);
    TEST_ASSERT_EQUAL_UINT16(400U, c[0].te_us);
    TEST_ASSERT_EQUAL_UINT8(3U, c[0].frames);
    TEST_ASSERT_TRUE(fck_drv_ook_rolling(s_runs, n));
}

TEST_CASE("KeeLoq: 64 and 65 bits, the battery bit, the te of the datasheet's three rates and its table, and 15 % jitter",
          "[ook][rolling]")
{
    uint64_t const word = ((uint64_t)0xAU << 60) | ((uint64_t)0x0ABCDEFU << 32) | 0x12345678U;
    for (unsigned bits = 64U; bits <= 66U; bits++) {
        size_t const n = keeloq_capture(400U, word, bits, true, 2U, 0U);
        fck_drv_ook_rolling_t c[2];
        TEST_ASSERT_EQUAL_UINT32(1U, fck_drv_ook_decode_rolling(s_runs, n, c, 2U));
        TEST_ASSERT_EQUAL_UINT8((uint8_t)bits, c[0].bits);
        TEST_ASSERT_EQUAL_HEX32(0x0ABCDEFU, c[0].serial);
        TEST_ASSERT_EQUAL((bits > 64U) ? 1 : 0, c[0].battery_low ? 1 : 0);
    }
    static const uint32_t tes[] = { 100U, 130U, 200U, 260U, 330U, 400U, 660U };
    for (size_t i = 0U; i < sizeof(tes) / sizeof(tes[0]); i++) {
        for (unsigned seed = 0U; seed < 4U; seed++) {
            ookb_t b;
            ookb_init(&b, s_runs, 4096U, 15U);
            b.lcg = seed * 17U + 3U;
            ookb_raw(&b, -60000);
            for (unsigned w = 0U; w < 3U; w++) {
                ookb_keeloq_word(&b, tes[i], word, 66U, false, 11U);
            }
            fck_drv_ook_rolling_t c[2];
            TEST_ASSERT_GREATER_OR_EQUAL_UINT32(1U, fck_drv_ook_decode_rolling(s_runs, b.n, c, 2U));
            TEST_ASSERT_EQUAL_HEX32(0x12345678U, c[0].hop);
            TEST_ASSERT_TRUE(fck_drv_ook_rolling(s_runs, b.n));
        }
    }
}

TEST_CASE("KeeLoq's edges: 63 and 67 bits, a short preamble, no header, a bit that is neither", "[ook][rolling]")
{
    uint64_t const word = 0x3123456AAAA5555ULL;
    fck_drv_ook_rolling_t c[2];
    TEST_ASSERT_EQUAL_UINT32(0U, fck_drv_ook_decode_rolling(s_runs, keeloq_capture(400U, word, 63U, false, 3U, 0U), c, 2U));
    for (unsigned pre = 0U; pre <= 5U; pre++) {   /* a preamble of fewer than 3 pairs is none */
        ookb_t b;
        ookb_init(&b, s_runs, 4096U, 0U);
        ookb_raw(&b, -60000);
        for (unsigned w = 0U; w < 3U; w++) {
            ookb_keeloq_word(&b, 400U, word, 66U, false, pre);
        }
        TEST_ASSERT_EQUAL_UINT32_MESSAGE((pre >= 3U) ? 1U : 0U, fck_drv_ook_decode_rolling(s_runs, b.n, c, 2U), "the preamble");
    }
    ookb_t b;
    ookb_init(&b, s_runs, 4096U, 0U);
    ookb_raw(&b, -60000);
    for (unsigned w = 0U; w < 3U; w++) {   /* a header of 3 te is not one */
        for (unsigned i = 0U; i < 11U; i++) {   /* 23 te: 11 pairs and the header's high */
            ookb_level(&b, true, 400U);
            ookb_level(&b, false, 400U);
        }
        ookb_level(&b, true, 400U);
        ookb_level(&b, false, 3U * 400U);
        for (unsigned i = 0U; i < 66U; i++) {
            bool const one = (i < 64U) ? (((word >> i) & 1U) != 0U) : true;
            ookb_level(&b, true, one ? 400U : 800U);
            ookb_level(&b, false, (i + 1U < 66U) ? (one ? 800U : 400U) : 15600U);
        }
    }
    TEST_ASSERT_EQUAL_UINT32(0U, fck_drv_ook_decode_rolling(s_runs, b.n, c, 2U));
    /* A bit whose high and low are both 1.5 te is neither a 1 nor a 0. */
    ookb_init(&b, s_runs, 4096U, 0U);
    ookb_raw(&b, -60000);
    for (unsigned w = 0U; w < 3U; w++) {
        for (unsigned i = 0U; i < 11U; i++) {   /* 23 te: 11 pairs and the header's high */
            ookb_level(&b, true, 400U);
            ookb_level(&b, false, 400U);
        }
        ookb_level(&b, true, 400U);
        ookb_level(&b, false, 4000U);
        for (unsigned i = 0U; i < 66U; i++) {
            bool const one = (i < 64U) ? (((word >> i) & 1U) != 0U) : true;
            uint32_t   h = one ? 400U : 800U, l = (i + 1U < 66U) ? (one ? 800U : 400U) : 15600U;
            if (w == 1U && i == 20U) {
                h = 600U;
                l = 600U;
            }
            ookb_level(&b, true, h);
            ookb_level(&b, false, l);
        }
    }
    TEST_ASSERT_EQUAL_UINT32(1U, fck_drv_ook_decode_rolling(s_runs, b.n, c, 2U));
    TEST_ASSERT_EQUAL_UINT8(2U, c[0].frames);   /* the damaged frame is the one not counted */
}

static size_t somfy_capture(uint8_t key, uint8_t control, uint16_t counter, uint32_t serial, uint8_t checksum_error,
                            uint32_t te, unsigned frames, unsigned pct)
{
    ookb_t b;
    ookb_init(&b, s_runs, 4096U, pct);
    ookb_raw(&b, -60000);
    uint64_t const v = ookb_somfy_frame(key, control, counter, serial, checksum_error);
    for (unsigned f = 0U; f < frames; f++) {
        ookb_somfy_frame_runs(&b, te, v, 56U, (f != 0U) ? 7U : 2U);
    }
    return b.n;
}

TEST_CASE("Somfy RTS: button, counter and serial, from a frame obfuscated and checksummed as the article says",
          "[ook][rolling]")
{
    size_t const n = somfy_capture(0xA7U, 0x2U, 0x0102U, 0x123456U, 0U, 604U, 3U, 0U);
    fck_drv_ook_rolling_t c[2];
    TEST_ASSERT_EQUAL_UINT32(1U, fck_drv_ook_decode_rolling(s_runs, n, c, 2U));
    TEST_ASSERT_EQUAL_INT(FCK_DRV_OOK_SOMFY_RTS, c[0].proto);
    TEST_ASSERT_EQUAL_UINT8(56U, c[0].bits);
    TEST_ASSERT_EQUAL_HEX32(0x123456U, c[0].serial);
    TEST_ASSERT_EQUAL_UINT8(0x2U, c[0].button);
    TEST_ASSERT_EQUAL_UINT16(0x0102U, c[0].counter);
    TEST_ASSERT_TRUE(c[0].counter_read);
    TEST_ASSERT_EQUAL_UINT32(0U, c[0].hop);
    TEST_ASSERT_EQUAL_UINT16(604U, c[0].te_us);
    TEST_ASSERT_EQUAL_UINT8(3U, c[0].frames);
    TEST_ASSERT_TRUE(fck_drv_ook_rolling(s_runs, n));
}

TEST_CASE("Somfy RTS: other buttons, counters and serials, at a te of 500 to 720 and with 15 % jitter",
          "[ook][rolling]")
{
    static const uint32_t tes[] = { 500U, 560U, 604U, 640U, 720U };
    for (size_t i = 0U; i < 5U; i++) {
        for (unsigned pct = 0U; pct <= 15U; pct += 15U) {
            size_t const n = somfy_capture(0xA0U | (unsigned)i, (uint8_t)(1U << (i % 4U)), (uint16_t)(i * 1237U),
                                           0x654321U + (unsigned)i, 0U, tes[i], 3U, pct);
            fck_drv_ook_rolling_t c[2];
            TEST_ASSERT_GREATER_OR_EQUAL_UINT32(1U, fck_drv_ook_decode_rolling(s_runs, n, c, 2U));
            TEST_ASSERT_EQUAL_HEX32(0x654321U + (unsigned)i, c[0].serial);
            TEST_ASSERT_EQUAL_UINT8((uint8_t)(1U << (i % 4U)), c[0].button);
            TEST_ASSERT_EQUAL_UINT16((uint16_t)(i * 1237U), c[0].counter);
        }
    }
    fck_drv_ook_rolling_t c[2];   /* a te outside the window is none */
    TEST_ASSERT_EQUAL_UINT32(0U, fck_drv_ook_decode_rolling(s_runs, somfy_capture(0xA7U, 2U, 1U, 1U, 0U, 440U, 3U, 0U), c, 2U));
    TEST_ASSERT_EQUAL_UINT32(0U, fck_drv_ook_decode_rolling(s_runs, somfy_capture(0xA7U, 2U, 1U, 1U, 0U, 780U, 3U, 0U), c, 2U));
}

TEST_CASE("Somfy RTS: a wrong checksum is refused, so is a bit flipped, a mirrored frame and a key that is not 0xA_",
          "[ook][rolling]")
{
    fck_drv_ook_rolling_t c[2];
    for (unsigned err = 1U; err < 16U; err++) {
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(0U, fck_drv_ook_decode_rolling(s_runs, somfy_capture(0xA7U, 2U, 77U, 0xBEEFU, (uint8_t)err, 640U, 3U, 0U), c, 2U), "checksum");
    }
    TEST_ASSERT_EQUAL_UINT32(1U, fck_drv_ook_decode_rolling(s_runs, somfy_capture(0xA7U, 2U, 77U, 0xBEEFU, 0U, 640U, 3U, 0U), c, 2U));
    uint64_t const v = ookb_somfy_frame(0xA7U, 4U, 300U, 0xABCDEU, 0U);
    /* A bit flipped on the air flips the same bit of two bytes once the obfuscation is undone, which the
     * nibble checksum does not see unless it is in the first byte or the last: those are refused. */
    static const unsigned flips[] = { 0U, 3U, 7U, 52U, 55U };
    for (size_t k = 0U; k < sizeof(flips) / sizeof(flips[0]); k++) {
        unsigned const bit = flips[k];
        ookb_t b;
        ookb_init(&b, s_runs, 4096U, 0U);
        ookb_raw(&b, -60000);
        for (unsigned f = 0U; f < 3U; f++) {
            ookb_somfy_frame_runs(&b, 640U, v ^ (1ULL << bit), 56U, 2U);
        }
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(0U, fck_drv_ook_decode_rolling(s_runs, b.n, c, 2U), "a flipped bit");
    }
    uint64_t mirrored = 0U;
    for (unsigned i = 0U; i < 56U; i++) {
        mirrored |= ((v >> i) & 1U) << (55U - i);
    }
    ookb_t b;
    ookb_init(&b, s_runs, 4096U, 0U);
    ookb_raw(&b, -60000);
    for (unsigned f = 0U; f < 3U; f++) {
        ookb_somfy_frame_runs(&b, 640U, mirrored, 56U, 2U);
    }
    TEST_ASSERT_EQUAL_UINT32(0U, fck_drv_ook_decode_rolling(s_runs, b.n, c, 2U));
    TEST_ASSERT_EQUAL_UINT32(0U, fck_drv_ook_decode_rolling(s_runs, somfy_capture(0x37U, 2U, 77U, 0xBEEFU, 0U, 640U, 3U, 0U), c, 2U));
}

TEST_CASE("Somfy RTS: one hardware sync pair, a software sync of 4 to 8 half symbols, 55 and 57 bits are refused",
          "[ook][rolling]")
{
    fck_drv_ook_rolling_t c[2];
    uint64_t const        v = ookb_somfy_frame(0xA7U, 4U, 300U, 0xABCDEU, 0U);
    for (unsigned hw = 0U; hw <= 4U; hw++) {
        ookb_t b;
        ookb_init(&b, s_runs, 4096U, 0U);
        ookb_raw(&b, -60000);
        for (unsigned f = 0U; f < 3U; f++) {
            ookb_somfy_frame_runs(&b, 640U, v, 56U, hw);
        }
        TEST_ASSERT_EQUAL_UINT32_MESSAGE((hw >= 2U) ? 1U : 0U, fck_drv_ook_decode_rolling(s_runs, b.n, c, 2U), "hardware sync pairs");
    }
    for (unsigned half = 3U; half <= 24U; half += 1U) {   /* the software sync, in half te of 320 us at a te of 640 */
        ookb_t b;
        ookb_init(&b, s_runs, 4096U, 0U);
        ookb_raw(&b, -60000);
        for (unsigned f = 0U; f < 3U; f++) {
            for (unsigned i = 0U; i < 2U; i++) {
                ookb_level(&b, true, 4U * 640U);
                ookb_level(&b, false, 4U * 640U);
            }
            ookb_level(&b, true, half * 320U);
            ookb_level(&b, false, 640U);
            for (unsigned i = 0U; i < 56U; i++) {
                bool const one = ((v >> (55U - i)) & 1U) != 0U;
                ookb_level(&b, !one, 640U);
                ookb_level(&b, one, 640U);
            }
            ookb_level(&b, false, 32000U);
        }
        TEST_ASSERT_EQUAL_UINT32_MESSAGE((half >= 10U && half <= 20U) ? 1U : 0U, fck_drv_ook_decode_rolling(s_runs, b.n, c, 2U), "software sync");
    }
    for (unsigned nb = 55U; nb <= 57U; nb++) {
        ookb_t b;
        ookb_init(&b, s_runs, 4096U, 0U);
        ookb_raw(&b, -60000);
        for (unsigned f = 0U; f < 3U; f++) {
            ookb_somfy_frame_runs(&b, 640U, (nb <= 56U) ? (v >> (56U - nb)) : (v << (nb - 56U)), nb, 2U);
        }
        TEST_ASSERT_EQUAL_UINT32_MESSAGE((nb == 56U) ? 1U : 0U, fck_drv_ook_decode_rolling(s_runs, b.n, c, 2U), "bits");
    }
}

TEST_CASE("a KeeLoq and a Somfy in one capture are two codes, the one with more frames first", "[ook][rolling]")
{
    ookb_t b;
    ookb_init(&b, s_runs, 4096U, 0U);
    ookb_raw(&b, -60000);
    for (unsigned w = 0U; w < 2U; w++) {
        ookb_keeloq_word(&b, 400U, 0x1ABCDEFDEADBEEFULL, 66U, false, 11U);
    }
    for (unsigned f = 0U; f < 4U; f++) {
        ookb_somfy_frame_runs(&b, 640U, ookb_somfy_frame(0xA7U, 2U, 5U, 0x123456U, 0U), 56U, (f != 0U) ? 7U : 2U);
    }
    fck_drv_ook_rolling_t c[3];
    TEST_ASSERT_EQUAL_UINT32(2U, fck_drv_ook_decode_rolling(s_runs, b.n, c, 3U));
    TEST_ASSERT_EQUAL_INT(FCK_DRV_OOK_SOMFY_RTS, c[0].proto);
    TEST_ASSERT_EQUAL_UINT8(4U, c[0].frames);
    TEST_ASSERT_EQUAL_INT(FCK_DRV_OOK_KEELOQ, c[1].proto);
    TEST_ASSERT_EQUAL_UINT8(2U, c[1].frames);
    TEST_ASSERT_EQUAL_UINT32(1U, fck_drv_ook_decode_rolling(s_runs, b.n, c, 1U));   /* cap 1: the first only */
    TEST_ASSERT_EQUAL_UINT32(0U, fck_drv_ook_decode_rolling(NULL, 10U, c, 1U));
    TEST_ASSERT_EQUAL_UINT32(0U, fck_drv_ook_decode_rolling(s_runs, b.n, NULL, 1U));
    TEST_ASSERT_EQUAL_UINT32(0U, fck_drv_ook_decode_rolling(s_runs, b.n, c, 0U));
}

/* ---- The sending path's detector ---------------------------------------------------------- */

TEST_CASE("fck_drv_ook_rolling: a fixed code's capture is not refused, at 15 % jitter or with noise before it",
          "[ook][rolling][shape]")
{
    for (unsigned pct = 0U; pct <= 15U; pct += 5U) {
        for (unsigned seed = 0U; seed < 5U; seed++) {
            ookb_t b;
            ookb_init(&b, s_runs, 4096U, pct);
            b.lcg = seed * 13U + 1U;
            for (unsigned i = 0U; i < 12U; i++) {   /* noise before the first frame, short enough to be no frame */
                ookb_level(&b, (i & 1U) == 0U, 100U + ookb_random(&b) % 700U);
            }
            ookb_raw(&b, -60000);
            int32_t frame[200];
            size_t  n = fck_drv_ook_encode(FCK_DRV_OOK_EV1527, 24U, 0xA1B2C3U, 300U + seed * 40U, 4U, frame, 200U);
            for (size_t i = 0U; i < n; i++) {
                ookb_level(&b, frame[i] > 0, (uint32_t)(frame[i] > 0 ? frame[i] : -frame[i]));
            }
            TEST_ASSERT_FALSE(fck_drv_ook_rolling(s_runs, b.n));
        }
    }
    TEST_ASSERT_FALSE(fck_drv_ook_rolling(NULL, 10U));
    TEST_ASSERT_FALSE(fck_drv_ook_rolling(s_runs, 0U));
}

TEST_CASE("fck_drv_ook_rolling: a long PWM frame is refused, the rolling codes this file does not read among them",
          "[ook][rolling][shape]")
{
    static const uint32_t tes[] = { 120U, 250U, 400U, 700U };
    for (size_t t = 0U; t < 4U; t++) {
        for (unsigned bits = 31U; bits <= 70U; bits += 13U) {
            ookb_t b;
            ookb_init(&b, s_runs, 4096U, 10U);
            unsigned x = 5U + (unsigned)t;
            for (unsigned i = 0U; i < bits; i++) {   /* each bit one te and three te, whichever way round */
                x          = x * 1103515245U + 12345U;
                bool one   = ((x >> 16) & 1U) != 0U;
                ookb_level(&b, true, one ? 3U * tes[t] : tes[t]);
                ookb_level(&b, false, one ? tes[t] : 3U * tes[t]);
            }
            TEST_ASSERT_EQUAL_MESSAGE((bits >= 31U && b.n > 60U) ? 1 : 0, fck_drv_ook_rolling(s_runs, b.n) ? 1 : 0,
                                      "a PWM frame of more than 60 runs");
        }
    }
    /* Up to 60 runs, whatever they are, are no long frame. */
    ookb_t b;
    ookb_init(&b, s_runs, 4096U, 0U);
    for (unsigned i = 0U; i < 60U; i++) {
        ookb_level(&b, (i & 1U) == 0U, 300U);
    }
    TEST_ASSERT_FALSE(fck_drv_ook_rolling(s_runs, b.n));
}

TEST_CASE("fck_drv_ook_rolling: a KeeLoq or Somfy the decoder cannot read is refused all the same", "[ook][rolling][shape]")
{
    uint64_t const word = 0x3123456AAAA5555ULL;
    /* a KeeLoq whose bits are off by a third of a te, with a header that is long */
    ookb_t b;
    ookb_init(&b, s_runs, 4096U, 0U);
    ookb_raw(&b, -60000);
    for (unsigned w = 0U; w < 3U; w++) {
        for (unsigned i = 0U; i < 11U; i++) {   /* 23 te: 11 pairs and the header's high */
            ookb_level(&b, true, 400U);
            ookb_level(&b, false, 400U);
        }
        ookb_level(&b, true, 400U);
        ookb_level(&b, false, 4000U);
        for (unsigned i = 0U; i < 66U; i++) {
            bool const one = (i < 64U) ? (((word >> i) & 1U) != 0U) : true;
            ookb_level(&b, true, one ? 520U : 900U);   /* neither */
            ookb_level(&b, false, (i + 1U < 66U) ? (one ? 700U : 480U) : 15600U);
        }
    }
    fck_drv_ook_rolling_t c[2];
    TEST_ASSERT_EQUAL_UINT32(0U, fck_drv_ook_decode_rolling(s_runs, b.n, c, 2U));
    TEST_ASSERT_TRUE(fck_drv_ook_rolling(s_runs, b.n));
    /* a Somfy with a checksum that is wrong */
    size_t const n = somfy_capture(0xA7U, 2U, 77U, 0xBEEFU, 3U, 640U, 3U, 0U);
    TEST_ASSERT_EQUAL_UINT32(0U, fck_drv_ook_decode_rolling(s_runs, n, c, 2U));
    TEST_ASSERT_TRUE(fck_drv_ook_rolling(s_runs, n));
}

/* ---- The channel's runs and the glitch rule are in test_ook.c ----------------------------- */

TEST_CASE("a capture's first run decides: a gap-led frame at the first run of an EV1527 is read, one of an "
          "HT12E is not",
          "[ook][protocols]")
{
    int32_t frame[100];
    size_t  n = fck_drv_ook_encode(FCK_DRV_OOK_EV1527, 24U, 0xA1B2C3U, 390U, 2U, frame, 100U);
    fck_drv_ook_code_t code;
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32(1U, fck_drv_ook_decode(frame, n, &code, 1U));
    n = fck_drv_ook_encode(FCK_DRV_OOK_HT12E, 12U, 0x5A5U, 333U, 1U, frame, 100U);
    TEST_ASSERT_EQUAL_UINT32(0U, fck_drv_ook_decode(frame, n, &code, 1U));
}

TEST_CASE("KeeLoq: a te of 100 us (the fastest rate) is read, 90 and 700 us are not", "[ook][rolling]")
{
    uint64_t const word = 0x3123456AAAA5555ULL;
    fck_drv_ook_rolling_t c[2];
    TEST_ASSERT_EQUAL_UINT32(1U, fck_drv_ook_decode_rolling(s_runs, keeloq_capture(100U, word, 66U, false, 3U, 0U), c, 2U));
    TEST_ASSERT_EQUAL_UINT16(100U, c[0].te_us);
    TEST_ASSERT_EQUAL_UINT32(1U, fck_drv_ook_decode_rolling(s_runs, keeloq_capture(660U, word, 66U, false, 3U, 0U), c, 2U));
    TEST_ASSERT_EQUAL_UINT32(0U, fck_drv_ook_decode_rolling(s_runs, keeloq_capture(90U, word, 66U, false, 3U, 0U), c, 2U));
    TEST_ASSERT_EQUAL_UINT32(0U, fck_drv_ook_decode_rolling(s_runs, keeloq_capture(700U, word, 66U, false, 3U, 0U), c, 2U));
}

TEST_CASE("a rolling code the readers no longer read, for its te, is still refused by the sending path's detector",
          "[ook][rolling]")
{
    /* The readers' windows are the sheet's and the article's; the detector does not need to read a frame to
     * refuse it. A KeeLoq or a Somfy RTS frame just outside the windows must never be let through. */
    uint64_t const word = 0x3123456AAAA5555ULL;
    TEST_ASSERT_TRUE(fck_drv_ook_rolling(s_runs, keeloq_capture(90U, word, 66U, false, 3U, 0U)));
    TEST_ASSERT_TRUE(fck_drv_ook_rolling(s_runs, keeloq_capture(700U, word, 66U, false, 3U, 0U)));
    TEST_ASSERT_TRUE(fck_drv_ook_rolling(s_runs, somfy_capture(0xA7U, 2U, 1U, 1U, 0U, 440U, 3U, 0U)));
    TEST_ASSERT_TRUE(fck_drv_ook_rolling(s_runs, somfy_capture(0xA7U, 2U, 1U, 1U, 0U, 780U, 3U, 0U)));
}

TEST_CASE("Somfy RTS: a frame counts once, with any jitter, however its sync's low ends", "[ook][rolling]")
{
    for (unsigned seed = 0U; seed < 24U; seed++) {
        ookb_t b;
        ookb_init(&b, s_runs, 4096U, 15U);
        b.lcg = seed * 7919U + 1U;
        ookb_raw(&b, -60000);
        uint64_t const v = ookb_somfy_frame(0xA7U, 2U, 0x0102U, 0x123456U, 0U);
        for (unsigned f = 0U; f < 3U; f++) {
            ookb_somfy_frame_runs(&b, 604U, v, 56U, (f != 0U) ? 7U : 2U);
        }
        fck_drv_ook_rolling_t c[3];
        size_t const          n = fck_drv_ook_decode_rolling(s_runs, b.n, c, 3U);
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(1U, n, "one code");
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(3U, c[0].frames, "three frames sent, three counted");
    }
}

TEST_CASE("the readers keep to 32 bits whatever a run holds: runs of 2^31 - 1 us break nothing", "[ook][rolling]")
{
    for (unsigned shape = 0U; shape < 4U; shape++) {
        for (size_t i = 0U; i < 200U; i++) {
            int32_t v = (shape == 0U) ? 0x7FFFFFFF : (int32_t)(400U + 200U * (i % 3U));
            if (shape == 2U && (i % 7U) == 0U) {
                v = 0x7FFFFFFF;
            }
            if (shape == 3U && i < 6U) {
                v = 0x7FFFFFFF;
            }
            s_runs[i] = ((i & 1U) == 0U) ? v : -v;
        }
        fck_drv_ook_rolling_t c[2];
        fck_drv_ook_code_t    f[2];
        (void)fck_drv_ook_decode_rolling(s_runs, 200U, c, 2U);
        (void)fck_drv_ook_decode(s_runs, 200U, f, 2U);
        (void)fck_drv_ook_rolling(s_runs, 200U);
        s_runs[0] = -0x7FFFFFFF - 1;   /* the smallest value an int32_t holds */
        (void)fck_drv_ook_decode_rolling(s_runs, 200U, c, 2U);
        (void)fck_drv_ook_rolling(s_runs, 200U);
    }
}
