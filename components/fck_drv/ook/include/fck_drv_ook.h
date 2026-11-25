/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file fck_drv_ook.h
 * @brief On-off keying as the CC1101 demodulates it onto GDO0, read back from
 *        its edges, and the codes the remotes of gates, garage doors and
 *        blinds send with it.
 *
 * In asynchronous serial mode the CC1101 puts what it hears on GDO0 as it
 * hears it: high while the carrier is there, low while it is not. The P4
 * stamps each edge (fck_drv_cc1101.h); what is here turns the stamps into runs -
 * signed microseconds, positive high and negative low - and the runs into the
 * codes of the encoder chips below.
 *
 * The CC1101 demodulates noise as well: on a quiet channel GDO0 is full of
 * edges. So a capture starts when the power rises over the channel's own
 * noise, followed here as a low percentile of what the receiver reads
 * (fck_drv_ook_floor_*): a median would sit inside a remote's pulses if it was
 * already pressed when listening began.
 *
 * Fixed codes are read and written (fck_drv_ook_decode(), _encode()). Three
 * encoder chips have a public datasheet for their timing, and those are the
 * three here:
 *  - EV1527, Everest Semiconductor / Silvan Chip, "EV1527 OTP Encoder" (the
 *    English data sheet, Output format; the Rosc table gives the typical pulse):
 *    24 bits, a sync of 1 unit high and 31 low, a bit 0 one unit high and three
 *    low and a bit 1 three high and one low.
 *  - PT2262, Princeton Technology, "PT2262 Remote Control Encoder" V3.9
 *    (January 2011, RF Operation: AD bit waveform and SYNC bit waveform): the
 *    same pulse and the same sync, 12 tri-state pins sent as two bits each.
 *  - HT12E, Holtek, "212 Series of Encoders" (15 March 1996, Fig.1, Fig.4 and
 *    Fig.5): a pilot of 36 clocks low, a start pulse of one clock high, then 12
 *    bits of three clocks each.
 * Rolling codes are only read (fck_drv_ook_decode_rolling()), to show a remote's
 * serial and button: there is no way in this file to write one, and none to
 * send one.
 *  - KeeLoq, as the Microchip HCS301 sends it (DS21143C, Fig.9-3 to Fig.9-5 and
 *    Table 9-4).
 *  - Somfy RTS (the article "Somfy RTS protocol", Pushstack,
 *    pushstack.wordpress.com).
 *
 * Plain C, like fck_drv_link: tests/host runs it as it is.
 */
#ifndef FCK_DRV_OOK_H_
#define FCK_DRV_OOK_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** A run shorter than this is a glitch, merged into the runs around it. */
#define FCK_DRV_OOK_GLITCH_US   70U
/** Fewer runs than this, once merged, is a click, not a signal. */
#define FCK_DRV_OOK_RUNS_MIN    16U
/** How far over the noise, in half-dB, the power starts a capture (10 dB)
 *  and keeps it going (7 dB). */
#define FCK_DRV_OOK_TRIGGER_HALF 20
#define FCK_DRV_OOK_HOT_HALF     14

/* ---- Edges to runs ---------------------------------------------------------- */

/**
 * @brief An edge as a capture stamps it: microseconds since the capture
 *        started, shifted left by one, ORed with the level GDO0 read after
 *        it.
 */
static inline uint32_t fck_drv_ook_edge(uint32_t us, bool level)
{
    return (us << 1) | (level ? 1U : 0U);
}

/**
 * @brief Turn edges into runs, in place: @p edges is read as uint32_t and
 *        written as int32_t, never ahead of where it is read - but for the
 *        last run, the one that ends at @p end_us, which is written after
 *        every edge was read: when @p end_us is later than the last edge,
 *        @p edges needs room for @p n + 1 values.
 *
 * The first run starts at 0 with @p level0, the level when the capture
 * started; the last ends at @p end_us. An edge after @p end_us is dropped,
 * and one whose level is the run's own - a pair of edges the stamping
 * missed between, or a glitch that was over before it looked - does not
 * end it. No run is 0 long: two edges at the same microsecond leave the run
 * before them going on to the run after them. A capture with no time in it
 * gives no run at all.
 *
 * @return The runs written.
 */
size_t fck_drv_ook_runs(uint32_t *edges, size_t n, bool level0, uint32_t end_us);

/**
 * @brief Merge each run shorter than @p min_us, and the one after it, into
 *        the one before: [a][short][b] becomes [a + short + b]. In place.
 * @return The runs left.
 */
size_t fck_drv_ook_deglitch(int32_t *runs, size_t n, uint32_t min_us);

/* ---- The channel's noise ------------------------------------------------------ */

/** A reading below this, in half-dB (-125 dBm), is a bad read, not the
 *  channel. */
#define FCK_DRV_OOK_BOGUS_HALF  (-250)

/** @brief The noise followed: in sixteenths of a half-dB, and the readings
 *         since its last step. */
typedef struct {
    int32_t floor16;
    uint8_t k;
} fck_drv_ook_floor_t;

/**
 * @brief Where to start following the noise: the 19th percentile of the
 *        readings sorted (the 7th of 32). Bad reads count as
 *        FCK_DRV_OOK_BOGUS_HALF.
 * @param half Readings in half-dB; @p n of them, 1 to 64. Reordered.
 */
void fck_drv_ook_floor_start(fck_drv_ook_floor_t *f, int16_t *half, size_t n);

/**
 * @brief Follow the noise with one more reading. Every 16th, it steps: up a
 *        sixteenth of a half-dB when the reading is above it, down four
 *        when it is not. It settles where four readings in five are above
 *        it, and takes in a carrier that stays at about 8 dB a second, at a
 *        reading every 200 us. A bad read is not counted.
 */
void fck_drv_ook_floor_step(fck_drv_ook_floor_t *f, int16_t half);

/** @brief The noise followed, in half-dB. */
static inline int16_t fck_drv_ook_floor_half(const fck_drv_ook_floor_t *f)
{
    return (int16_t)(f->floor16 / 16);
}

/* ---- Fixed codes --------------------------------------------------------------- */

/** @brief The fixed codes. */
typedef enum {
    FCK_DRV_OOK_EV1527 = 0,   /**< 24 bits.   */
    FCK_DRV_OOK_PT2262 = 1,   /**< 24 bits.   */
    FCK_DRV_OOK_HT12E  = 2,   /**< 12 bits.   */
} fck_drv_ook_protocol_t;

/** The bits of one PT2262 or EV1527 frame. */
#define FCK_DRV_OOK_BITS        24U
/** The base pulse a PT2262 or EV1527 frame is read and written with. The
 *  HT12E has its own band (fck_drv_ook_te_ok()). */
#define FCK_DRV_OOK_TE_MIN_US   100U
#define FCK_DRV_OOK_TE_MAX_US   1200U
/** The most runs any frame takes: 24 bits and the two a frame adds - the
 *  sync of a PT2262 or EV1527, the start pulse and the pilot of an HT12E. */
#define FCK_DRV_OOK_FRAME_RUNS  (2U * FCK_DRV_OOK_BITS + 2U)

/** @brief One code a capture carries. */
typedef struct {
    fck_drv_ook_protocol_t proto;
    uint8_t         bits;      /**< 12 or 24, as the protocol has it
     *                              (fck_drv_ook_bits_ok()).               */
    uint32_t        key;       /**< MSB first, as sent.                    */
    uint16_t        te_us;     /**< The base pulse, averaged over the
                                *   frames that carried this code.        */
    uint8_t         frames;    /**< Frames that carried it.               */
} fck_drv_ook_code_t;

/** The frames one capture is read for, at most. */
#define FCK_DRV_OOK_FRAMES_MAX  16U

/**
 * @brief The codes a capture carries, the one most of its frames carry
 *        first; frames with the same protocol, bits and key are one code.
 *
 * A bit is a long and a short pulse, the long at least 1.4 times the short
 * (EV1527 and PT2262: 1.8). te, the base pulse, is each frame's own: the length
 * of its pulses over what the protocol has in them. A frame starts at each run
 * that follows a low of at least 3 ms, and at the capture's first high run too,
 * but for the HT12E. The readers are tried in this order, and the first to take
 * the frame keeps it:
 *
 * HT12E (Holtek, Fig.4 and Fig.5). The low before the frame is its pilot, at
 * least 15 te (the datasheet has 36 clocks; a capture sees the pilot cut, or the
 * quiet before the first word); a start pulse follows, te long; then 12 pairs of
 * a low and a high, to the next low of 3 ms or the capture's end. A bit 1 is two
 * clocks low and one high, a bit 0 one low and two high: each pair is 2.2 to
 * 3.8 te, the longer 1.4 to 2.8 times the shorter. te is from 150 to 449 us (the
 * oscillator of the datasheet runs from about 1 to 7 kHz; 3 kHz is typical). A
 * frame at the capture's first run has no pilot and is not read by this reader:
 * a capture carries several words (the chip sends four), and the rest are.
 *
 * PT2262 and EV1527. 24 bits and a sync after them: bit 0
 * is te high and 3 te low, bit 1 3 te high and te low; the sync te high and 31
 * te low. Each bit's high and low together must be 2.5 to 5.5 te, the longer at
 * least 1.8 times the shorter; the sync's high 0.5 to 2 te and its low 8 to
 * 60 te. PT2262 and EV1527 send the same bits; a code is called PT2262 when
 * each of its 12 pairs of bits is a trit (00, 11, or 01 for an open pin - the
 * datasheet's "0", "1" and "f") and at least one is open. A PT2262 with every pin
 * tied reads as EV1527.
 *
 * @param runs Signed microseconds, as fck_drv_ook_runs() writes them.
 * @param out  Up to @p cap codes, written whatever it returns.
 * @return The codes written; 0 when no frame decoded.
 */
size_t fck_drv_ook_decode(const int32_t *runs, size_t n, fck_drv_ook_code_t *out, size_t cap);

/**
 * @brief Which of the two a 24-bit @p key reads as: PT2262 when each of its
 *        12 pairs of bits is a trit (00, 11, or 01 for an open pin) and one
 *        at least is open; EV1527 otherwise.
 */
fck_drv_ook_protocol_t fck_drv_ook_protocol_of(uint32_t key);

/**
 * @brief Whether @p bits is a count @p proto sends: 24 for PT2262 and EV1527,
 *        12 for HT12E.
 */
bool fck_drv_ook_bits_ok(fck_drv_ook_protocol_t proto, uint8_t bits);

/**
 * @brief Whether @p te_us is a base pulse @p proto is read and written at:
 *        100 to 1200 us for PT2262 and EV1527 (the Rosc table of the EV1527 data
 *        sheet runs from about 85 to 830 us; under 97 us the sync's low of 31
 *        te is shorter than the 3 ms that ends a frame, so a frame could not be
 *        told from the next); 150 to 449 for HT12E.
 */
bool fck_drv_ook_te_ok(fck_drv_ook_protocol_t proto, uint16_t te_us);

/**
 * @brief The base pulses @p proto is read and written at, from @p min_us to
 *        @p max_us (as fck_drv_ook_te_ok() says), and the typical one:
 *        390 us for PT2262 and EV1527 (the 330 kOhm the EV1527 data sheet
 *        recommends, at 5 V: a bit of 1.56 ms, four pulses), 333 for HT12E (its
 *        3 kHz at 1.1 MOhm).
 * @return false, nothing written, for a value that is none of them or a null
 *         argument.
 */
bool fck_drv_ook_te_range(fck_drv_ook_protocol_t proto, uint16_t *min_us, uint16_t *max_us,
                          uint16_t *typical_us);

/**
 * @brief The name @p proto has in a file (fck_drv_subghz_file.h): "ev1527",
 *        "pt2262" or "ht12e". NULL for a value that is none of them.
 */
const char *fck_drv_ook_name(fck_drv_ook_protocol_t proto);

/**
 * @brief The protocol a file's protocol line, bits and key say.
 * @return false, @p proto untouched, for a name that is not one of
 *         fck_drv_ook_name()'s, a count of bits the protocol does not send, a
 *         key over its bits, or a PT2262 key with a pair of bits 10 (no PT2262
 *         sends one).
 */
bool fck_drv_ook_from_name(const char *name, uint8_t bits, uint32_t key, fck_drv_ook_protocol_t *proto);

/**
 * @brief @p frames frames of @p key, as a remote sends them and
 *        fck_drv_ook_decode() reads them.
 *
 * An HT12E word is the start pulse (te high), 12 pairs of a low and a high - the
 * first bit highest; a bit 1 is 2 te low and te high, a bit 0 te low and 2 te
 * high - and then the pilot, 36 te low: the pilot at the end, so that the first
 * frame starts with its pulse and each frame after it is preceded by one. A
 * PT2262 or EV1527 frame is its 24 bits - bit 0 te high and 3 te low, bit 1
 * 3 te high and te low - and the sync after them, te high and 31 te low.
 * Nothing here transmits.
 * @param proto For PT2262, every pair of bits of @p key a trit: 00, 11 or
 *              01, never 10.
 * @param bits  As fck_drv_ook_bits_ok() says for @p proto.
 * @param te_us As fck_drv_ook_te_ok() says for @p proto.
 * @param runs  At least @p frames * FCK_DRV_OOK_FRAME_RUNS of @p cap suffice;
 *              a frame of fewer runs takes fewer.
 * @return The runs written; 0, with nothing written, for a bad @p bits, a key
 *         over its bits, a PT2262 key with a pair 10, a te out of its band,
 *         no frames or too small a @p cap.
 */
size_t fck_drv_ook_encode(fck_drv_ook_protocol_t proto, uint8_t bits, uint32_t key, uint16_t te_us,
                          size_t frames, int32_t *runs, size_t cap);

/* ---- Rolling codes: read, never written ------------------------------------------ */

/** @brief The rolling codes read. */
typedef enum {
    FCK_DRV_OOK_KEELOQ    = 0,   /**< HCS200, HCS300, HCS301...: 64 to 66 bits.  */
    FCK_DRV_OOK_SOMFY_RTS = 1,   /**< Somfy RTS: 56 bits.                        */
} fck_drv_ook_rolling_protocol_t;

/** @brief One rolling code a capture carries. */
typedef struct {
    fck_drv_ook_rolling_protocol_t proto;
    uint8_t   bits;           /**< Bits read: KeeLoq 64 to 66, Somfy 56.     */
    uint64_t  data;           /**< The bits as they were received, the first
                               *   one highest - KeeLoq's first 64, Somfy's
                               *   56.                                       */
    uint32_t  serial;         /**< KeeLoq's 28 bits, Somfy's 24.             */
    uint8_t   button;         /**< 4 bits.                                   */
    uint32_t  hop;            /**< KeeLoq's encrypted half, as it came; 0 for
                               *   Somfy.                                    */
    uint16_t  counter;        /**< Somfy's rolling counter; 0 for KeeLoq,
                               *   whose counter is in the hop.             */
    bool      counter_read;   /**< counter is the remote's: Somfy.           */
    bool      battery_low;    /**< KeeLoq's VLOW bit, when 65 bits came.     */
    uint16_t  te_us;          /**< The base pulse, averaged over the frames. */
    uint8_t   frames;         /**< Frames that carried it.                   */
} fck_drv_ook_rolling_t;

/**
 * @brief The rolling codes a capture carries, the one most of its frames
 *        carry first, to show a remote's serial and button. Read only: the
 *        hop of a KeeLoq is shown as it came, never decrypted, and nothing
 *        here writes a rolling code as runs or sends one.
 *
 * A frame starts at the capture's first run or at a high run after a low of
 * at least 3 ms.
 *
 * KeeLoq (HCS301, DS21143C Fig.9-3 to Fig.9-5): at least 3 pairs of a high and a
 * low of te (400 typical, 260 to 660 in the table; 100 to 660 us are read; the
 * datasheet's preamble is 12 pulses), then a high of te and a low of about 10
 * te, then the bits, each a high and a low: bit 1 is te high and 2 te low, bit
 * 0 2 te high and te low. The last bit's low is the guard gap, so it is read
 * from its high alone. 64 to 66 bits are taken, sent least significant first:
 * the first 32 are the hop, the next 28 the serial, then four bits - S3, S0, S1
 * and S2 in the datasheet's order - read as a number with the first received
 * lowest, the button; the 65th is the battery (VLOW).
 *
 * Somfy RTS (Pushstack): 2 or more hardware sync pulses, each 4 te high and 4 te
 * low (te 480 to 730 us, a half symbol of 604 us in the article); a high of
 * about 7.5 te, the software sync (4550 us in the article); a low of te; and 56
 * bits in Manchester, two half bits of te each, the bit 1 a low and then a high,
 * the bit 0 a high and then a low, the first bit highest. The article obfuscates
 * the bytes by XORing each with the one sent before it; the frame is read back
 * as d = data ^ (data >> 8). Its 4-bit checksum, the XOR of its nibbles with the
 * checksum's own cleared, must be the one it carries (bits 40 to 43), or the
 * frame is not read: the button is d bits 44 to 47, the counter bits 24 to 39
 * and the serial bits 0 to 23, read as one number with the first byte sent
 * highest.
 *
 * @param runs Signed microseconds, as fck_drv_ook_runs() writes them.
 * @param out  Up to @p cap codes, written whatever it returns.
 * @return The codes written; 0 when no frame decoded.
 */
size_t fck_drv_ook_decode_rolling(const int32_t *runs, size_t n, fck_drv_ook_rolling_t *out, size_t cap);

/**
 * @brief Whether @p runs are SHAPED like a rolling code's signal. For the sending
 *        path to refuse a capture of one: replaying a rolling code is never
 *        done here. A detector, not a decoder: it needs no bit count, no
 *        checksum and no whole frame, so a capture that lost its first pulse,
 *        its preamble or a bit is still refused. Nothing is written; no frame
 *        buffer is kept.
 *
 * True when any of these is found, anywhere in @p runs:
 *  - KeeLoq-like: two or more even pairs (high and low each 150 to 900 us, the
 *    longer within 30 % of the shorter), then a high of 0.6 to 1.4 te and a low
 *    of 6 to 16 te, then 40 or more runs of 3 te at most before the next low of
 *    3 ms or more (one damaged bit does not stop the count);
 *  - Somfy-like: one or more even pairs (each 1500 to 3600 us, within 35 %),
 *    then a high 1.25 to 2.6 times a pulse of those, then 30 or more runs of
 *    2.6 times a quarter of that pulse at most. A Manchester FIXED remote of
 *    that timing may be refused too;
 *  - a long PWM frame: more than 60 runs in a row, with none longer than 6 te,
 *    nine in ten of them 1, 2 or 3 te (te: the mean of the runs under the
 *    mean). This is what refuses the rolling codes this file does not read, and
 *    it refuses the FIXED codes of that length as well, and a fixed remote whose
 *    frames follow each other 6 te apart or closer, until they have a decoder.
 *    The fixed codes this file reads send 24 bits at most, 49 runs from one gap
 *    to the next, and are not refused;
 *  - a frame fck_drv_ook_decode_rolling() reads.
 *
 * Noise is not refused: its runs are not 1, 2 and 3 te.
 * @return false for no runs.
 */
bool fck_drv_ook_rolling(const int32_t *runs, size_t n);

#ifdef __cplusplus
}
#endif

#endif /* FCK_DRV_OOK_H_ */
