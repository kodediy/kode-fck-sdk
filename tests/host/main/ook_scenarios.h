/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file ook_scenarios.h
 * @brief Test support: the captures the golden vectors are about, laid out in a
 *        fixed order from the specifications (ook_frames.h) and from the encoder
 *        of fck_drv_ook, with jitter from a fixed seed.
 *
 * The golden vectors (ook_gold.inc) hold what the implementation this one
 * replaced answered for each of these captures; the cases that check them walk
 * the same list, so the data is the answers only, a few kilobytes, and the
 * captures are rebuilt by code a person can read.
 */
#ifndef OOK_SCENARIOS_H_
#define OOK_SCENARIOS_H_

#include <string.h>

#include "fck_drv_ook.h"
#include "ook_frames.h"

typedef void (*ook_scenario_visit_t)(void *ctx, const char *what, const int32_t *runs, size_t n);

static uint32_t s_scenario_jitter_state;

static inline uint32_t ook_scenario_random(void)
{
    s_scenario_jitter_state = s_scenario_jitter_state * 1103515245U + 12345U;
    return (s_scenario_jitter_state >> 8) & 0xFFFFU;
}

static inline void ook_scenario_jitter(int32_t *r, size_t n, unsigned pct)
{
    for (size_t i = 0; i < n; i++) {
        int32_t const a    = r[i] < 0 ? -r[i] : r[i];
        int32_t const span = a * (int32_t)pct / 100;
        int32_t const v    = a + (int32_t)(ook_scenario_random() % (uint32_t)(2 * span + 1)) - span;
        r[i]               = r[i] < 0 ? -v : v;
    }
}

/* A quiet channel and then the frames of the encoder, jittered, as a capture has. */
static inline size_t ook_scenario_capture(fck_drv_ook_protocol_t proto, uint8_t bits, uint32_t key, uint16_t te_us,
                                          size_t frames, unsigned pct, int32_t *r)
{
    int32_t tmp[2048];
    /* The encoder refuses a te under FCK_DRV_OOK_TE_MIN_US, and the golden vectors are of captures with such a te
     * too (a reader must not take them): lay the frame out at the least te the encoder takes and scale its runs, so
     * that every scenario after this one draws the same jitter as in the reference run. */
    uint16_t const enc_te = (te_us < FCK_DRV_OOK_TE_MIN_US) ? (uint16_t)FCK_DRV_OOK_TE_MIN_US : te_us;
    size_t const n = fck_drv_ook_encode(proto, bits, key, enc_te, frames, tmp, 2048);
    if (n == 0U) {
        return 0U;
    }
    if (enc_te != te_us) {
        for (size_t i = 0; i < n; i++) {
            tmp[i] = tmp[i] / (int32_t)enc_te * (int32_t)te_us;
        }
    }
    r[0] = -8000;
    memcpy(&r[1], tmp, n * sizeof(int32_t));
    ook_scenario_jitter(&r[1], n, pct);
    return n + 1U;
}

/** @brief Every scenario, in order, to @p visit. */
static inline void ook_scenarios(ook_scenario_visit_t visit, void *ctx)
{
    s_scenario_jitter_state = 99U;
    static const uint32_t ev_keys[] = { 0x000000U, 0xFFFFFFU, 0x5155C3U, 0xA1B2C3U, 0x123456U, 0x800001U, 0x0F0F0FU };
    static const uint32_t pt_keys[] = { 0x000000U, 0xFFFFFFU, 0x555555U, 0x5155C3U, 0x5C1C5CU, 0xC3C3C3U, 0x010101U, 0xD4D4D4U };
    static const uint16_t tes[]     = { 80U, 100U, 250U, 390U, 436U, 800U, 1200U };
    static const uint32_t ht_keys[] = { 0x000U, 0xFFFU, 0x9C3U, 0x5A5U, 0x001U, 0x800U };
    static const uint16_t ht_tes[]  = { 150U, 200U, 320U, 333U, 449U };
    static const unsigned pcts[]    = { 0U, 10U, 15U };
    static int32_t       rbuf[4096];
    int32_t *const       r = rbuf;
    /* ---- decode: every encoded protocol, jittered, with a quiet before ---- */
        for (size_t k = 0; k < 7; k++) {
        for (size_t t = 0; t < 7; t++) {
            size_t n = ook_scenario_capture(FCK_DRV_OOK_EV1527, 24, ev_keys[k], tes[t], 2U + (k + t) % 3U, pcts[(k + t) % 3], r);
            visit(ctx, "ev1527 capture", r, n);
        }
    }
    for (size_t k = 0; k < 8; k++) {
        for (size_t t = 0; t < 7; t++) {
            size_t n = ook_scenario_capture(FCK_DRV_OOK_PT2262, 24, pt_keys[k], tes[t], 2U + (k + t) % 4U, pcts[(k + 2 * t) % 3], r);
            visit(ctx, "pt2262 capture", r, n);
        }
    }
    for (size_t k = 0; k < 6; k++) {
        for (size_t t = 0; t < 5; t++) {
            size_t n = ook_scenario_capture(FCK_DRV_OOK_HT12E, 12, ht_keys[k], ht_tes[t], 2U + (k + t) % 3U, pcts[(k + t) % 3], r);
            visit(ctx, "ht12e capture", r, n);
        }
    }
    /* a PT2262 frame at the capture's start, the last sync cut, no quiet before */
    for (unsigned cut = 0; cut < 4; cut++) {
        int32_t tmp[512];
        size_t n = fck_drv_ook_encode(FCK_DRV_OOK_PT2262, 24, 0x5155C3U, 436U, 3, tmp, 512);
        visit(ctx, "pt2262 sync cut", tmp, n - cut);
        visit(ctx, "pt2262 whole frames", tmp, n - 25 - cut);
    }
    /* HT12E built from the datasheet, the pilot swept */
    static const uint32_t pilots[] = { 1U, 5U, 10U, 14U, 15U, 16U, 20U, 30U, 36U, 40U, 41U, 42U, 47U, 62U, 63U, 100U, 400U };
    for (size_t p = 0; p < sizeof(pilots) / sizeof(pilots[0]); p++) {
        for (unsigned f = 0; f < 2; f++) {
            ookb_t b; ookb_init(&b, r, 4096, f ? 10U : 0U);
            ookb_raw(&b, -9000);
            ookb_ht12e_word(&b, 320U, 0x9C3U, pilots[p]);
            ookb_ht12e_word(&b, 320U, 0x9C3U, pilots[p]);
            ookb_ht12e_word(&b, 320U, 0x9C3U, pilots[p]);
            ookb_level(&b, false, 12000U);
            visit(ctx, "ht12e pilot sweep", r, b.n);
        }
    }
    /* HT12E te sweep with the datasheet pilot */
    for (uint32_t te = 100U; te <= 520U; te += 20U) {
        ookb_t b; ookb_init(&b, r, 4096, 0U);
        ookb_raw(&b, -9000);
        for (unsigned f = 0; f < 3; f++) ookb_ht12e_word(&b, te, 0x5A5U, 36U);
        ookb_level(&b, false, 12000U);
        visit(ctx, "ht12e te sweep", r, b.n);
    }
    for (uint32_t te = 145U; te <= 155U; te++) {
        ookb_t b; ookb_init(&b, r, 4096, 0U);
        ookb_raw(&b, -9000);
        for (unsigned f = 0; f < 3; f++) ookb_ht12e_word(&b, te, 0xA5AU, 36U);
        visit(ctx, "ht12e te 150 edge", r, b.n);
    }
    for (uint32_t te = 444U; te <= 454U; te++) {
        ookb_t b; ookb_init(&b, r, 4096, 0U);
        ookb_raw(&b, -9000);
        for (unsigned f = 0; f < 3; f++) ookb_ht12e_word(&b, te, 0xA5AU, 36U);
        visit(ctx, "ht12e te 449 edge", r, b.n);
    }
    /* PT2262 / EV1527 te edges, by hand: 24 bits then sync */
    for (uint32_t te = 70U; te <= 90U; te += 1U) {
        ookb_t b; ookb_init(&b, r, 4096, 0U);
        ookb_raw(&b, -8000);
        for (unsigned f = 0; f < 3; f++) {
            for (int bit = 23; bit >= 0; bit--) {
                bool one = ((0xA1B2C3U >> bit) & 1U) != 0U;
                ookb_level(&b, true, one ? 3U * te : te);
                ookb_level(&b, false, one ? te : 3U * te);
            }
            ookb_level(&b, true, te);
            ookb_level(&b, false, 31U * te);
        }
        visit(ctx, "ev1527 te low edge", r, b.n);
    }
    for (uint32_t te = 1190U; te <= 1215U; te += 1U) {
        ookb_t b; ookb_init(&b, r, 4096, 0U);
        ookb_raw(&b, -8000);
        for (unsigned f = 0; f < 3; f++) {
            for (int bit = 23; bit >= 0; bit--) {
                bool one = ((0x123456U >> bit) & 1U) != 0U;
                ookb_level(&b, true, one ? 3U * te : te);
                ookb_level(&b, false, one ? te : 3U * te);
            }
            ookb_level(&b, true, te);
            ookb_level(&b, false, 31U * te);
        }
        visit(ctx, "ev1527 te high edge", r, b.n);
    }
    /* sync shape sweep: the sync low from 4 to 70 te, the sync high 0.3 to 2.5 te */
    {
        static const uint32_t lows[] = { 4U, 7U, 8U, 9U, 20U, 31U, 60U, 61U, 70U };
        static const uint32_t highs10[] = { 3U, 5U, 6U, 10U, 20U, 25U };   /* tenths of te */
        for (size_t l = 0; l < 9; l++) {
            for (size_t h = 0; h < 6; h++) {
                ookb_t b; ookb_init(&b, r, 4096, 0U);
                ookb_raw(&b, -8000);
                for (unsigned f = 0; f < 3; f++) {
                    for (int bit = 23; bit >= 0; bit--) {
                        bool one = ((0x5155C3U >> bit) & 1U) != 0U;
                        ookb_level(&b, true, one ? 3U * 400U : 400U);
                        ookb_level(&b, false, one ? 400U : 3U * 400U);
                    }
                    ookb_level(&b, true, highs10[h] * 40U);
                    ookb_level(&b, false, lows[l] * 400U);
                }
                visit(ctx, "pt2262 sync shape", r, b.n);
            }
        }
    }
    /* bit shape sweep: one bit's high+low from 2 to 6 te, ratio */
    {
        static const uint32_t hi[] = { 100U, 150U, 200U, 250U, 300U, 400U, 500U };
        for (size_t h = 0; h < 7; h++) {
            for (size_t l = 0; l < 7; l++) {
                ookb_t b; ookb_init(&b, r, 4096, 0U);
                ookb_raw(&b, -8000);
                for (unsigned f = 0; f < 3; f++) {
                    for (int bit = 23; bit >= 0; bit--) {
                        bool one = ((0x2D1B7AU >> bit) & 1U) != 0U;
                        ookb_level(&b, true, one ? hi[h] * 4U : hi[l] * 4U / 2U);
                        ookb_level(&b, false, one ? hi[l] * 4U / 2U : hi[h] * 4U);
                    }
                    ookb_level(&b, true, 400U);
                    ookb_level(&b, false, 400U * 31U);
                }
                visit(ctx, "bit shape sweep", r, b.n);
            }
        }
    }
    /* what is not a frame: too few runs, 23 and 25 bits, no sync, noise */
    {
        for (int bits = 20; bits <= 28; bits++) {
            ookb_t b; ookb_init(&b, r, 4096, 0U);
            ookb_raw(&b, -8000);
            for (unsigned f = 0; f < 3; f++) {
                for (int bit = bits - 1; bit >= 0; bit--) {
                    bool one = ((0x5155C3U >> (bit % 24)) & 1U) != 0U;
                    ookb_level(&b, true, one ? 1200U : 400U);
                    ookb_level(&b, false, one ? 400U : 1200U);
                }
                ookb_level(&b, true, 400U);
                ookb_level(&b, false, 400U * 31U);
            }
            visit(ctx, "ev1527 bit count", r, b.n);
        }
        for (unsigned seed = 0; seed < 12; seed++) {
            ookb_t b; ookb_init(&b, r, 4096, 0U);
            b.lcg = seed * 7919U + 1U;
            for (unsigned i = 0; i < 300; i++) {
                uint32_t d = 80U + ookb_random(&b) % 3000U;
                ookb_raw(&b, (i & 1U) ? -(int32_t)d : (int32_t)d);
            }
            visit(ctx, "noise", r, b.n);
        }
    }
    /* two remotes in one capture */
    {
        int32_t a[1200], c[1200];
        size_t na = ook_scenario_capture(FCK_DRV_OOK_EV1527, 24, 0xA1B2C3U, 390U, 4, 5U, a);
        size_t nc = ook_scenario_capture(FCK_DRV_OOK_HT12E, 12, 0x9C3U, 320U, 2, 5U, c);
        memcpy(r, a, na * 4); memcpy(&r[na], c, nc * 4);
        visit(ctx, "two remotes ev1527 first", r, na + nc);
        memcpy(r, c, nc * 4); memcpy(&r[nc], a, na * 4);
        visit(ctx, "two remotes ht12e first", r, na + nc);
        size_t nd = ook_scenario_capture(FCK_DRV_OOK_PT2262, 24, 0x5155C3U, 436U, 3, 5U, a);
        memcpy(r, a, nd * 4); memcpy(&r[nd], c, nc * 4);
        visit(ctx, "pt2262 and ht12e", r, nd + nc);
    }
    /* a KeeLoq or a Somfy has no fixed reading */
    {
        ookb_t b; ookb_init(&b, r, 4096, 0U);
        ookb_raw(&b, -60000);
        for (int f = 0; f < 3; f++) ookb_keeloq_word(&b, 400U, 0x5ABCDEFDEADBEEFULL, 66U, true, 11U);
        visit(ctx, "keeloq has no fixed reading", r, b.n);
        ookb_init(&b, r, 4096, 0U);
        ookb_raw(&b, -60000);
        for (int f = 0; f < 3; f++) ookb_somfy_frame_runs(&b, 640U, ookb_somfy_frame(0xA7U, 2U, 5U, 0x123456U, 0U), 56U, f ? 7U : 2U);
        visit(ctx, "somfy has no fixed reading", r, b.n);
    }

    /* ---- rolling ---- */
    {
        static const uint32_t tes_k[] = { 200U, 250U, 400U, 650U, 700U, 750U };
        static const unsigned bitsk[] = { 64U, 65U, 66U };
        unsigned id = 0;
        for (size_t t = 0; t < 6; t++) {
            for (size_t bi = 0; bi < 3; bi++) {
                for (unsigned pct = 0; pct <= 15; pct += 15) {
                    uint64_t serial = 0x0ABCDEFU + id * 0x111U;
                    uint64_t word = ((uint64_t)((id * 3U + 1U) & 0xFU) << 60) | (serial << 32) | (0xDEADBEEFU ^ (id * 0x01010101U));
                    ookb_t b; ookb_init(&b, r, 4096, pct);
                    ookb_raw(&b, -60000);
                    unsigned frames = 1U + id % 4U;
                    for (unsigned f = 0; f < frames; f++) ookb_keeloq_word(&b, tes_k[t], word, bitsk[bi], (id & 1U) != 0U, 11U + (id % 3U) * 6U);
                    visit(ctx, "rolling", r, b.n);
                    id++;
                }
            }
        }
        /* a short preamble, a missing one, no header, an odd bit count */
        static const unsigned pre[] = { 0U, 1U, 2U, 3U, 4U, 23U, 30U };
        for (size_t p = 0; p < 7; p++) {
            ookb_t b; ookb_init(&b, r, 4096, 0U);
            ookb_raw(&b, -60000);
            for (unsigned f = 0; f < 3; f++) ookb_keeloq_word(&b, 400U, 0x7123456AAAA5555ULL, 66U, false, pre[p]);
            visit(ctx, "rolling", r, b.n);
        }
        for (unsigned bits = 60; bits <= 70; bits++) {
            ookb_t b; ookb_init(&b, r, 4096, 0U);
            ookb_raw(&b, -60000);
            for (unsigned f = 0; f < 3; f++) ookb_keeloq_word(&b, 400U, 0x3123456AAAA5555ULL, bits > 66 ? 66U : bits, false, 11U);
            if (bits > 66) { /* extra bits, 1 each */
                b.n -= 0;
                for (unsigned x = 66; x < bits; x++) { ookb_level(&b, true, 400U); ookb_level(&b, false, 800U); }
                ookb_level(&b, false, 16000U);
            }
            visit(ctx, "rolling", r, b.n);
        }
        /* the header sweep */
        static const uint32_t hdr[] = { 3U, 5U, 7U, 8U, 9U, 10U, 12U, 14U, 16U, 18U, 25U };
        for (size_t h = 0; h < 11; h++) {
            ookb_t b; ookb_init(&b, r, 4096, 0U);
            ookb_raw(&b, -60000);
            for (unsigned f = 0; f < 3; f++) {
                for (unsigned i = 0; i < 11; i++) { ookb_level(&b, true, 400U); ookb_level(&b, false, 400U); }
                ookb_level(&b, true, 400U);
                ookb_level(&b, false, hdr[h] * 400U);
                uint64_t word = 0x4123456AAAA5555ULL;
                for (unsigned i = 0; i < 66; i++) {
                    bool one = (i < 64U) ? (((word >> i) & 1U) != 0U) : true;
                    ookb_level(&b, true, one ? 400U : 800U);
                    ookb_level(&b, false, (i + 1U < 66U) ? (one ? 800U : 400U) : 15600U);
                }
            }
            visit(ctx, "rolling", r, b.n);
        }
        /* a bit that is neither: a 1.5 te / 1.5 te one, a 4 te high */
        for (unsigned bad = 0; bad < 6; bad++) {
            ookb_t b; ookb_init(&b, r, 4096, 0U);
            ookb_raw(&b, -60000);
            for (unsigned f = 0; f < 3; f++) {
                uint64_t word = 0x2123456AAAA5555ULL;
                for (unsigned i = 0; i < 11; i++) { ookb_level(&b, true, 400U); ookb_level(&b, false, 400U); }
                ookb_level(&b, true, 400U); ookb_level(&b, false, 4000U);
                for (unsigned i = 0; i < 66; i++) {
                    bool one = (i < 64U) ? (((word >> i) & 1U) != 0U) : true;
                    uint32_t h = one ? 400U : 800U, l = (i + 1U < 66U) ? (one ? 800U : 400U) : 15600U;
                    if (f == 1 && i == 20 + bad) { if (bad == 0) { h = 600U; l = 600U; } else if (bad == 1) { h = 1600U; } else if (bad == 2) { l = 1600U; } else if (bad == 3) { h = 100U; } else if (bad == 4) { h = 800U; l = 800U; } else { h = 300U; l = 900U; } }
                    ookb_level(&b, true, h); ookb_level(&b, false, l);
                }
            }
            visit(ctx, "rolling", r, b.n);
        }
    }
    {
        /* Somfy */
        static const uint32_t tes_s[] = { 400U, 450U, 500U, 560U, 604U, 640U, 720U, 740U, 760U, 800U };
        unsigned id = 0;
        for (size_t t = 0; t < 10; t++) {
            for (unsigned pct = 0; pct <= 15; pct += 15) {
                uint8_t ctrl = (uint8_t)(1U << (id % 4U));
                uint64_t v = ookb_somfy_frame(0xA0U | (id & 0xFU), (uint8_t)(id % 9U + (id % 3U ? 0U : 8U)), (uint16_t)(id * 1237U), 0x123456U + id * 0x10101U, 0U);
                (void)ctrl;
                ookb_t b; ookb_init(&b, r, 4096, pct);
                ookb_raw(&b, -60000);
                unsigned frames = 1U + id % 4U;
                for (unsigned f = 0; f < frames; f++) ookb_somfy_frame_runs(&b, tes_s[t], v, 56U, f ? 7U : 2U);
                visit(ctx, "rolling", r, b.n);
                id++;
            }
        }
        /* variants: hardware sync counts, checksum errors, bit counts, mirrored, software sync length */
        for (unsigned hw = 0; hw <= 9; hw++) {
            ookb_t b; ookb_init(&b, r, 4096, 0U);
            ookb_raw(&b, -60000);
            uint64_t v = ookb_somfy_frame(0xA7U, 8U, 1U, 0x00BEEFU, 0U);
            for (unsigned f = 0; f < 3; f++) ookb_somfy_frame_runs(&b, 640U, v, 56U, hw);
            visit(ctx, "rolling", r, b.n);
        }
        for (unsigned err = 0; err < 16; err++) {
            ookb_t b; ookb_init(&b, r, 4096, 0U);
            ookb_raw(&b, -60000);
            uint64_t v = ookb_somfy_frame(0xA7U, 2U, 77U, 0x00BEEFU, (uint8_t)err);
            for (unsigned f = 0; f < 3; f++) ookb_somfy_frame_runs(&b, 640U, v, 56U, 2U);
            visit(ctx, "rolling", r, b.n);
        }
        for (unsigned bit = 0; bit < 56; bit += 3) {
            ookb_t b; ookb_init(&b, r, 4096, 0U);
            ookb_raw(&b, -60000);
            uint64_t v = ookb_somfy_frame(0xA7U, 4U, 300U, 0x0ABCDEU, 0U) ^ (1ULL << bit);
            for (unsigned f = 0; f < 3; f++) ookb_somfy_frame_runs(&b, 640U, v, 56U, 2U);
            visit(ctx, "rolling", r, b.n);
        }
        for (unsigned nb = 50; nb <= 62; nb++) {
            ookb_t b; ookb_init(&b, r, 4096, 0U);
            ookb_raw(&b, -60000);
            uint64_t v = ookb_somfy_frame(0xA7U, 4U, 300U, 0x0ABCDEU, 0U);
            for (unsigned f = 0; f < 3; f++) {
                if (nb <= 56) ookb_somfy_frame_runs(&b, 640U, v >> (56U - nb), nb, 2U);
                else ookb_somfy_frame_runs(&b, 640U, v << (nb - 56U), nb, 2U);
            }
            visit(ctx, "rolling", r, b.n);
        }
        for (unsigned ss = 3; ss <= 12; ss++) {   /* software sync, in half te */
            ookb_t b; ookb_init(&b, r, 4096, 0U);
            ookb_raw(&b, -60000);
            uint64_t v = ookb_somfy_frame(0xA7U, 4U, 300U, 0x0ABCDEU, 0U);
            for (unsigned f = 0; f < 3; f++) {
                for (unsigned i = 0; i < 2; i++) { ookb_level(&b, true, 4U * 640U); ookb_level(&b, false, 4U * 640U); }
                ookb_level(&b, true, ss * 320U);
                ookb_level(&b, false, 640U);
                for (unsigned i = 0; i < 56; i++) {
                    bool one = ((v >> (55U - i)) & 1U) != 0U;
                    ookb_level(&b, !one, 640U); ookb_level(&b, one, 640U);
                }
                ookb_level(&b, false, 32000U);
            }
            visit(ctx, "rolling", r, b.n);
        }
        /* a mirrored frame: the bits read the other way */
        {
            ookb_t b; ookb_init(&b, r, 4096, 0U);
            ookb_raw(&b, -60000);
            uint64_t v = ookb_somfy_frame(0xA7U, 4U, 300U, 0x0ABCDEU, 0U), m = 0;
            for (unsigned i = 0; i < 56; i++) m |= ((v >> i) & 1U) << (55U - i);
            for (unsigned f = 0; f < 3; f++) ookb_somfy_frame_runs(&b, 640U, m, 56U, 2U);
            visit(ctx, "rolling", r, b.n);
        }
        /* a Keeloq and a Somfy in one capture */
        {
            ookb_t b; ookb_init(&b, r, 4096, 0U);
            ookb_raw(&b, -60000);
            for (unsigned f = 0; f < 2; f++) ookb_keeloq_word(&b, 400U, 0x1ABCDEFDEADBEEFULL, 66U, false, 11U);
            ookb_raw(&b, -60000);
            for (unsigned f = 0; f < 4; f++) ookb_somfy_frame_runs(&b, 640U, ookb_somfy_frame(0xA7U, 2U, 5U, 0x123456U, 0U), 56U, f ? 7U : 2U);
            visit(ctx, "rolling", r, b.n);
            ookb_init(&b, r, 4096, 0U);
            ookb_raw(&b, -60000);
            for (unsigned f = 0; f < 4; f++) ookb_keeloq_word(&b, 400U, 0x1ABCDEFDEADBEEFULL, 66U, false, 11U);
            ookb_raw(&b, -60000);
            for (unsigned f = 0; f < 2; f++) ookb_somfy_frame_runs(&b, 640U, ookb_somfy_frame(0xA7U, 2U, 5U, 0x123456U, 0U), 56U, f ? 7U : 2U);
            visit(ctx, "rolling", r, b.n);
        }
    }
    /* ---- the shape detector on fixed codes, noise, long PWM frames ---- */
    {
        for (size_t t = 0; t < 7; t++) {
            size_t n = ook_scenario_capture(FCK_DRV_OOK_EV1527, 24, 0xA1B2C3U, tes[t], 4, 15U, r);
            visit(ctx, "shape", r, n);
            n = ook_scenario_capture(FCK_DRV_OOK_PT2262, 24, 0x5155C3U, tes[t], 4, 15U, r);
            visit(ctx, "shape", r, n);
        }
        for (size_t t = 0; t < 5; t++) {
            size_t n = ook_scenario_capture(FCK_DRV_OOK_HT12E, 12, 0x9C3U, ht_tes[t], 4, 15U, r);
            visit(ctx, "shape", r, n);
        }
        for (unsigned seed = 0; seed < 20; seed++) {
            ookb_t b; ookb_init(&b, r, 4096, 0U);
            b.lcg = seed * 104729U + 3U;
            for (unsigned i = 0; i < 40U + seed * 20U; i++) {
                uint32_t d = 60U + ookb_random(&b) % 4000U;
                ookb_raw(&b, (i & 1U) ? -(int32_t)d : (int32_t)d);
            }
            visit(ctx, "shape", r, b.n);
        }
        /* a long PWM frame: runs of 1, 2 and 3 te, n runs, with a gap at the ends */
        static const unsigned lens[] = { 20U, 40U, 59U, 60U, 61U, 62U, 80U, 100U, 120U };
        for (size_t l = 0; l < 9; l++) {
            for (unsigned te = 200U; te <= 800U; te += 300U) {
                for (unsigned maxu = 3U; maxu <= 7U; maxu += 2U) {
                    ookb_t b; ookb_init(&b, r, 4096, 0U);
                    b.lcg = (unsigned)l * 31U + te + maxu;
                    ookb_raw(&b, -9000);
                    for (unsigned i = 0; i < lens[l]; i++) {
                        unsigned u = 1U + ookb_random(&b) % (maxu > 3U && i % 17U == 5U ? maxu : 3U);
                        ookb_raw(&b, (i & 1U) ? -(int32_t)(u * te) : (int32_t)(u * te));
                    }
                    ookb_raw(&b, (lens[l] & 1U) ? 9000 : -9000);
                    visit(ctx, "shape", r, b.n);
                }
            }
        }
        /* a long PWM frame of 4 te runs mixed in, and not even */
        for (unsigned bad = 0; bad < 12; bad++) {
            ookb_t b; ookb_init(&b, r, 4096, 0U);
            b.lcg = 555U + bad;
            ookb_raw(&b, -9000);
            for (unsigned i = 0; i < 90U; i++) {
                unsigned u = 1U + ookb_random(&b) % 3U;
                if (i % 10U == bad % 10U && bad >= 6U) u = 4U + bad % 3U;
                if (i % 9U == bad % 9U && bad < 6U) u = 7U;
                ookb_raw(&b, (i & 1U) ? -(int32_t)(u * 300U) : (int32_t)(u * 300U));
            }
            visit(ctx, "shape", r, b.n);
        }
        visit(ctx, "shape", r, 0);
        r[0] = 500;
        visit(ctx, "shape", r, 1);
    }

}

#endif /* OOK_SCENARIOS_H_ */
