/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file ook_frames.h
 * @brief Test support: the frames of the protocols fck_drv_ook reads, laid out
 *        from the published specifications, independently of the code under
 *        test.
 *
 * Sources (one per protocol, the same ones fck_drv_ook.c cites):
 *  - HT12E: Holtek, "212 Series of Encoders", 15 Mar 1996, Fig.4 and Fig.5
 *    (36-clock pilot, a one-clock start pulse, 3 clocks a bit: "1" is two low
 *    and one high, "0" one low and two high).
 *  - HCS301: Microchip DS21143C, Fig.9-3 to Fig.9-5 and Table 9-4 (TE 400 us,
 *    preamble 23 TE, header 10 TE, 66 bits LSB first, logic "1" one TE high and
 *    two low, logic "0" two high and one low, guard 39 TE).
 *  - Somfy RTS: "Somfy RTS protocol", Pushstack (pushstack.wordpress.com):
 *    half symbol 604 us, hardware sync 2 pairs of 4 symbols' halves, software
 *    sync 4550 us high, Manchester, 7 bytes obfuscated byte over byte, a
 *    nibble-xor checksum.
 *
 * Plain C, header only: every function is static inline so that a suite that
 * does not use one pays nothing for it.
 */
#ifndef OOK_FRAMES_H_
#define OOK_FRAMES_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** @brief A run buffer being filled; adjacent runs of one level are merged. */
typedef struct {
    int32_t *runs;
    size_t   n;
    size_t   cap;
    uint32_t lcg;   /**< The state of the jitter, the same every run.   */
    uint32_t pct;   /**< Jitter, in percent of each duration.           */
} ookb_t;

static inline void ookb_init(ookb_t *b, int32_t *runs, size_t cap, uint32_t pct)
{
    b->runs = runs;
    b->n    = 0U;
    b->cap  = cap;
    b->lcg  = 7U;
    b->pct  = pct;
}

static inline uint32_t ookb_random(ookb_t *b)
{
    b->lcg = b->lcg * 1103515245U + 12345U;
    return (b->lcg >> 8) & 0xFFFFU;
}

/** @brief @p d with up to +/- the buffer's percent of jitter. */
static inline int32_t ookb_jitter(ookb_t *b, uint32_t d)
{
    if (b->pct == 0U) {
        return (int32_t)d;
    }
    int32_t const span = (int32_t)(d * b->pct / 100U);
    return (int32_t)d + (int32_t)(ookb_random(b) % (uint32_t)(2 * span + 1)) - span;
}

/** @brief One run, @p high or low, @p us long, jittered; the same level as the last run extends it. */
static inline void ookb_level(ookb_t *b, bool high, uint32_t us)
{
    int32_t const v = high ? ookb_jitter(b, us) : -ookb_jitter(b, us);
    if (b->n > 0U && ((b->runs[b->n - 1U] > 0) == high)) {
        b->runs[b->n - 1U] += v;
        return;
    }
    if (b->n < b->cap) {
        b->runs[b->n++] = v;
    }
}

/** @brief A run as it is, no jitter and no merging (a damaged capture). */
static inline void ookb_raw(ookb_t *b, int32_t v)
{
    if (b->n < b->cap) {
        b->runs[b->n++] = v;
    }
}

/* ---- Holtek HT12E ---------------------------------------------------------------------- */

/**
 * @brief One HT12E word as the Holtek datasheet draws it, preceded by its pilot:
 *        the pilot low, one clock high (the start pulse), then 12 bits, the
 *        first one A0: a "1" is two clocks low and one high, a "0" one low and
 *        two high. The chip sends 4 words in a row (Fig.1), so the pilot of the
 *        next one follows the last high.
 * @param pilot_te The pilot, in clocks (36 in the datasheet).
 */
static inline void ookb_ht12e_word(ookb_t *b, uint32_t te, uint32_t key12, uint32_t pilot_te)
{
    ookb_level(b, false, pilot_te * te);
    ookb_level(b, true, te);
    for (unsigned i = 0U; i < 12U; i++) {
        bool const one = ((key12 >> (11U - i)) & 1U) != 0U;
        ookb_level(b, false, one ? 2U * te : te);
        ookb_level(b, true, one ? te : 2U * te);
    }
}

/* ---- Microchip HCS301 (KeeLoq) -------------------------------------------------------- */

/**
 * @brief One HCS301 code word: @p preamble_pairs pairs of TE high and TE low (the
 *        data sheet's preamble is 23 TE: 11 pairs, and the TE high that
 *        follows), TE high and a 10 TE low (the header), then @p bits bits of @p word, the
 *        least significant first (hop 0 to 31, serial 32 to 59, the button's
 *        four bits 60 to 63, VLOW 64), a "1" one TE high and two low, a "0" two
 *        high and one low, the last low stretched into the guard time.
 * @param bits 64 to 66; bit 64 is @p vlow and bit 65 the repeat bit, set.
 */
static inline void ookb_keeloq_word(ookb_t *b, uint32_t te, uint64_t word, unsigned bits, bool vlow,
                                    unsigned preamble_pairs)
{
    for (unsigned i = 0U; i < preamble_pairs; i++) {
        ookb_level(b, true, te);
        ookb_level(b, false, te);
    }
    ookb_level(b, true, te);
    ookb_level(b, false, 10U * te);
    for (unsigned i = 0U; i < bits; i++) {
        bool const one = (i < 64U) ? (((word >> i) & 1U) != 0U) : ((i == 64U) ? vlow : true);
        ookb_level(b, true, one ? te : 2U * te);
        ookb_level(b, false, (i + 1U < bits) ? (one ? 2U * te : te) : 39U * te);
    }
}

/* ---- Somfy RTS ------------------------------------------------------------------------- */

/**
 * @brief The 7 bytes of an RTS frame as Pushstack lays them out, obfuscated for
 *        the air: key, control nibble and checksum nibble, the rolling counter
 *        (two bytes), the address (three bytes), each byte after the first
 *        XORed with the one before it as sent.
 * @param serial Its three bytes in the order they are sent, the first highest.
 * @param checksum_error XORed into the checksum nibble: 0 for a good frame.
 * @return The frame as the 56-bit number the first bit of which is the highest.
 */
static inline uint64_t ookb_somfy_frame(uint8_t key, uint8_t control, uint16_t counter, uint32_t serial,
                                        uint8_t checksum_error)
{
    uint8_t f[7];
    f[0] = key;
    f[1] = (uint8_t)(control << 4);
    f[2] = (uint8_t)(counter >> 8);
    f[3] = (uint8_t)counter;
    f[4] = (uint8_t)(serial >> 16);
    f[5] = (uint8_t)(serial >> 8);
    f[6] = (uint8_t)serial;
    uint8_t cks = 0U;
    for (unsigned i = 0U; i < 7U; i++) {
        cks ^= (uint8_t)(f[i] ^ (f[i] >> 4));
    }
    f[1] |= (uint8_t)((cks ^ checksum_error) & 0x0FU);
    for (unsigned i = 1U; i < 7U; i++) {
        f[i] ^= f[i - 1U];
    }
    uint64_t v = 0U;
    for (unsigned i = 0U; i < 7U; i++) {
        v = (v << 8) | f[i];
    }
    return v;
}

/**
 * @brief One RTS frame on the air: @p hardware_sync pairs of 4 half symbols high
 *        and 4 low, the software sync (4550 us high at a half symbol of 604, so
 *        7.5 of them), a half symbol low, then @p bits Manchester bits of @p v,
 *        the first highest - a "1" a low half and then a high one, a "0" the
 *        other way round - and the gap between frames, 50 half symbols.
 */
static inline void ookb_somfy_frame_runs(ookb_t *b, uint32_t te, uint64_t v, unsigned bits,
                                         unsigned hardware_sync)
{
    for (unsigned i = 0U; i < hardware_sync; i++) {
        ookb_level(b, true, 4U * te);
        ookb_level(b, false, 4U * te);
    }
    ookb_level(b, true, te * 15U / 2U);
    ookb_level(b, false, te);
    for (unsigned i = 0U; i < bits; i++) {
        bool const one = ((v >> (bits - 1U - i)) & 1U) != 0U;
        ookb_level(b, !one ? true : false, te);
        ookb_level(b, one ? true : false, te);
    }
    ookb_level(b, false, 50U * te);
}

#endif /* OOK_FRAMES_H_ */
