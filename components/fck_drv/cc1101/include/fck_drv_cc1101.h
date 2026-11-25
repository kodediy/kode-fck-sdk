/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file fck_drv_cc1101.h
 * @brief The module's CC1101: put to sleep, listening and sending.
 *
 * The chip has no reset line and no firmware of its own, and a hard reset of
 * the P4 leaves it as it was: a carrier left on by a firmware that died stays
 * on until its FIFO runs dry. So the first thing done for a module just found
 * is the datasheet's manual reset, and the second is SPWD - power down, about
 * 200 nA - until an app asks for the radio.
 *
 * Listening is a receiver of level
 * alone for sweeps, through a 650 kHz or a 58 kHz filter, and OOK in
 * asynchronous serial mode, what it demodulates on GDO0, for a remote's
 * frames. Each call opens the chip, does its one thing and resets it back to
 * sleep, so between calls the CC1101 is as fck_drv_cc1101_sleep() leaves it.
 * Listening strobes no STX and writes no PATABLE; sending, below, does.
 *
 * The leg is picked from the frequency with fck_bsp.h's table: SW1 through
 * IOCFG2, SW0 through the expander, and SW0 back at rest (high) when the call
 * ends. Without the expander (@p tca NULL: the GNSS receiver holds the bus)
 * only the legs SW0 rests for can be used - the middle one, 433's, and the
 * bottom one, 868 and 915's - and a frequency on the top leg, 315's, is
 * refused.
 *
 * Every call blocks for as long as it listens, and belongs to the owner's
 * task: a job's (fck_aos_run_job()), with the module to itself.
 *
 * SPI3: kode-sdk's NFC reader has SPI2, and SPI3 is the controller the Mesh
 * module's radio uses, which is never fitted together with this one.
 */
#ifndef FCK_DRV_CC1101_H_
#define FCK_DRV_CC1101_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "kode_defs.h"

#include "fck_drv_ook.h"
#include "fck_drv_pins.h"
#include "fck_drv_subghz_modem.h"
#include "fck_drv_subghz_scan.h"
#include "fck_drv_tca6408a.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Open the SPI bus, reset the chip, say what it is, power it down,
 *        and close the bus again with its chip select held high.
 * @param partnum PARTNUM read back, 0x00 on a CC1101; may be NULL.
 * @param version VERSION read back, never 0x00 or 0xFF; may be NULL.
 * @return KODE_OK; KODE_TIMEOUT when the chip never raised its ready line;
 *         KODE_IO_ERROR when what answered is not a CC1101; KODE_ERROR when
 *         the bus would not come up (SPI3 taken by something else).
 */
kode_status_t fck_drv_cc1101_sleep(const fck_drv_pins_t *hw, uint8_t *partnum, uint8_t *version);

/* ---- Listening ---------------------------------------------------------------
 *
 * Levels are in half-dB, as the chip reads them: dBm * 2. A level that is not
 * there - a frequency the chip does not tune, a leg it cannot take, a point
 * that never reached RX - is FCK_DRV_SUBGHZ_SCAN_NO_RECEIVE (fck_drv_subghz_scan.h).
 *
 * Every call returns, besides what it says: KODE_ERROR when SPI3 would not
 * come up; KODE_TIMEOUT when the chip never became ready; KODE_IO_ERROR when
 * what answered is not a CC1101, when it would not reach RX, or when SW0
 * could not be set or put back. Their outputs are written on KODE_OK alone,
 * but fck_drv_cc1101_spectrum()'s, which it fills as it sweeps. */

/** @brief The receiver's channel filter. */
typedef enum {
    FCK_DRV_CC1101_WIDE   = 0,   /**< 650 kHz: a remote whose exact frequency is not known. */
    FCK_DRV_CC1101_NARROW = 1,   /**< 58 kHz: one frequency, closely.                      */
} fck_drv_cc1101_filter_t;

/** @brief Whether the CC1101 tunes @p hz: 300-348, 387-464 or 779-928 MHz. */
bool fck_drv_cc1101_tunes(uint32_t hz);

/**
 * @brief The power on @p hz now: the highest of 3 readings over 0.5 ms with
 *        the wide filter, of 8 over 1.7 ms with the narrow one.
 * @return KODE_OK; KODE_DENIED for a frequency it does not tune, or one on
 *         a leg SW0 does not rest for without the expander.
 */
kode_status_t fck_drv_cc1101_rssi(const fck_drv_pins_t *hw, fck_drv_tca6408a_t *tca, uint32_t hz,
                              fck_drv_cc1101_filter_t f, int16_t *half);

/**
 * @brief The power on @p count frequencies, @p first_hz + i * @p step_hz,
 *        each read as fck_drv_cc1101_rssi() reads one; the legs change as the
 *        sweep crosses them.
 * @param half @p count readings; FCK_DRV_SUBGHZ_SCAN_NO_RECEIVE for a frequency it does not
 *             tune or cannot take.
 * @return KODE_OK; KODE_DENIED when it could take none of them;
 *         KODE_IO_ERROR when none it could take reached RX.
 */
kode_status_t fck_drv_cc1101_spectrum(const fck_drv_pins_t *hw, fck_drv_tca6408a_t *tca, uint32_t first_hz,
                                  uint32_t step_hz, size_t count, fck_drv_cc1101_filter_t f,
                                  int16_t *half);

/** @brief What fck_drv_cc1101_find() heard. */
typedef struct {
    bool     found;
    uint32_t hz;          /**< Where the narrow sweep peaked, on the list's
                           *   frequency when it is within 40 kHz of one. */
    int16_t  half;        /**< The peak's reading.                        */
    int16_t  noise_half;  /**< What that list frequency had read before.  */
} fck_drv_cc1101_hit_t;

/**
 * @brief Sweep the frequencies remotes use (fck_drv_subghz_scan_remotes()) for up to
 *        @p ms, and say where a remote pressed while it sweeps sends.
 *
 * The rule: the first 3 sweeps learn each frequency's noise. After
 * that, the frequency that rises most, if 10 dB or more, gets a narrow sweep
 * at once, while the remote is still pressed. It is found when a narrow sweep
 * has peaked and either the same rise came twice, within 150 kHz and 1.5 s,
 * or it rose 25 dB.
 *
 * @return KODE_OK, found or not; KODE_IO_ERROR when a sweep reached RX on no
 *         frequency at all.
 */
kode_status_t fck_drv_cc1101_find(const fck_drv_pins_t *hw, fck_drv_tca6408a_t *tca, uint32_t ms,
                              fck_drv_cc1101_hit_t *hit);

/** The edges one capture holds: 16 KB of internal RAM, taken for the call. */
#define FCK_DRV_CC1101_EDGES     4096U
/** A capture closes this long after the power last stood over the noise... */
#define FCK_DRV_CC1101_QUIET_MS  60U
/** ...or this long after it began. */
#define FCK_DRV_CC1101_BURST_MS  500U

/**
 * @brief Listen on @p hz with OOK for up to @p ms, and record the first
 *        remote's frames heard.
 *
 * The channel's noise is measured on arrival and followed (fck_drv_ook.h). A
 * capture starts when the power rises 10 dB over it and closes after
 * FCK_DRV_CC1101_QUIET_MS under 7 dB over it, or FCK_DRV_CC1101_BURST_MS after it
 * began; what came after its last moment over 7 dB, past 3 ms, is noise and
 * is dropped. Runs under 70 us join their neighbours, and a capture of fewer
 * than 16 runs is a click: it keeps listening.
 *
 * @param runs Signed microseconds (fck_drv_ook_runs()); the first @p cap, when
 *             there are more.
 * @param n    Runs written; 0 when nothing was heard in @p ms.
 * @return KODE_OK; KODE_DENIED as fck_drv_cc1101_rssi(); KODE_NO_RESOURCES
 *         without 16 KB of internal RAM; KODE_ERROR when GDO0's interrupt
 *         could not be had.
 */
kode_status_t fck_drv_cc1101_record(const fck_drv_pins_t *hw, fck_drv_tca6408a_t *tca, uint32_t hz, uint32_t ms,
                                int32_t *runs, size_t cap, size_t *n);

/* ---- Sending ------------------------------------------------------------------ */

/** The most OOK runs one transmission plays: a long RAW capture, as record. */
#define FCK_DRV_CC1101_SEND_RUNS_MAX    FCK_DRV_CC1101_EDGES
/** The most times fck_drv_cc1101_send() repeats the whole signal. */
#define FCK_DRV_CC1101_SEND_REPEATS_MAX 32U
/** The longest single run it plays, in microseconds (1 s): a sane bound on a
 *  gap, so the symbol buffer stays small. */
#define FCK_DRV_CC1101_SEND_RUN_US_MAX  1000000U

/**
 * @brief Play @p n OOK runs (fck_drv_ook, signed microseconds, positive the
 *        carrier on and negative off) on @p hz, @p repeats times: the inverse
 *        of fck_drv_cc1101_record(), for cloning a captured remote or
 *        emulating a decoded one. The CC1101 keys its carrier in asynchronous
 *        serial mode to the waveform the P4's RMT clocks onto GDO0;
 *        PATABLE is the band's power (fck_drv_cc1101.c). The chip is
 *        opened, sends, and is put back to sleep, as in every call here.
 * @param runs    @p n of them, none 0, each at most
 *                FCK_DRV_CC1101_SEND_RUN_US_MAX us.
 * @param n       1 to FCK_DRV_CC1101_SEND_RUNS_MAX.
 * @param repeats 1 to FCK_DRV_CC1101_SEND_REPEATS_MAX.
 * @return KODE_OK; KODE_DENIED when the module may not transmit on @p hz
 *         (fck_drv_subghz_scan_may_transmit()), the leg cannot be had, the
 *         carrier would be on for more than one call may keep it
 *         (FCK_DRV_SUBGHZ_SCAN_SEND_MS_MAX: the positive runs times
 *         @p repeats, checked before the PA is keyed), or an argument is
 *         bad; KODE_NO_RESOURCES without internal RAM for the
 *         symbols; KODE_ERROR when the bus or the RMT would not come up;
 *         KODE_TIMEOUT when the chip never became ready; KODE_IO_ERROR when
 *         it would not enter TX, or when fck_drv_cc1101_abort() cut it short
 *         (the carrier is off). KODE_DENIED also when it was called already.
 */
kode_status_t fck_drv_cc1101_send(const fck_drv_pins_t *hw, fck_drv_tca6408a_t *tca, uint32_t hz,
                                  const int32_t *runs, size_t n, uint8_t repeats);

/**
 * @brief A listen kept from one fck_drv_cc1101_listen() to the next, by its
 *        caller: where, and the noise each frequency was last left at.
 */
typedef struct {
    fck_drv_subghz_scan_hold_t hold; /**< Where, or hopping (esp_timer time). */
    size_t              next;   /**< Hopping: the hop listened to next.          */
    bool                known[FCK_DRV_SUBGHZ_SCAN_HOPS + 1U];
    fck_drv_ook_floor_t noise[FCK_DRV_SUBGHZ_SCAN_HOPS + 1U];   /**< The hops',
                                 *   and the last for @c hold.hz.                */
} fck_drv_cc1101_listen_t;

/** @brief Start a listen on @p hz, or hopping with 0: nothing learned yet. No
 *         radio. */
void fck_drv_cc1101_listen_start(fck_drv_cc1101_listen_t *l, uint32_t hz);

/** @brief The frequency fck_drv_cc1101_listen() is listening on right now;
 *         0 when none is running. One 32-bit word, read from any task. */
uint32_t fck_drv_cc1101_listen_now(void);

/** @brief What fck_drv_cc1101_listen() heard. */
typedef struct {
    bool     heard;
    uint32_t hz;           /**< Where; hopping, where it stays when a code
                            *   was read there.                            */
    int16_t  half;         /**< Its loudest reading, in half-dB.              */
    int16_t  noise_half;   /**< The channel's noise before it.                */
} fck_drv_cc1101_heard_t;

/**
 * @brief Listen for up to @p ms, as @p l says, and record the first remote
 *        heard, as fck_drv_cc1101_record() does.
 *
 * On one frequency, its noise is learned the first time and followed from
 * there, read after read. Hopping, each hop is listened to for
 * FCK_DRV_SUBGHZ_SCAN_HOP_MS with its own noise, in turn, those SW0 does not
 * rest for skipped when @p tca is NULL. What is heard on a hop is said
 * where it sounded loudest: there or on the neighbour
 * (fck_drv_subghz_scan_neighbours()) louder by 6 dB, each listened to for
 * FCK_DRV_SUBGHZ_SCAN_NEIGHBOUR_MS - a strong remote bleeds into the hops
 * beside it. Where it stays is fck_drv_subghz_scan_hold_t's rule: a code
 * read holds it there until FCK_DRV_SUBGHZ_SCAN_STAY_MS pass with no code
 * read - the read ends there and hops on for the rest of @p ms -, a capture
 * with no code does not hold it. Either way, a capture no code is read from and of fewer
 * than FCK_DRV_SUBGHZ_SCAN_WORTH_RUNS runs is a click: it listens on.
 *
 * The chip is opened and put back to sleep in the call, as in every call
 * here: between two, it hears nothing.
 *
 * @param l     Kept by the caller; updated on KODE_OK alone.
 * @param runs  As fck_drv_cc1101_record()'s.
 * @param n     Runs written; 0 when nothing was heard in @p ms.
 * @param heard Written on KODE_OK; heard false with @p n 0.
 * @return KODE_OK; KODE_DENIED for a frequency the CC1101 does not tune or
 *         whose leg cannot be had, and a bad argument; KODE_NO_RESOURCES
 *         without 16 KB of internal RAM; KODE_IO_ERROR when the chip would
 *         not listen; KODE_ERROR when GDO0's interrupt could not be had.
 */
kode_status_t fck_drv_cc1101_listen(const fck_drv_pins_t *hw, fck_drv_tca6408a_t *tca,
                                    fck_drv_cc1101_listen_t *l, uint32_t ms, int32_t *runs, size_t cap,
                                    size_t *n, fck_drv_cc1101_heard_t *heard);

/** @brief One packet received. */
typedef struct {
    uint8_t                       bytes[FCK_DRV_SUBGHZ_MODEM_LENGTH_MAX];
    uint8_t                       length;
    fck_drv_subghz_modem_status_t status;
} fck_drv_cc1101_packet_t;

/**
 * @brief Receive 2-FSK or GFSK packets on @p hz for up to @p ms, or until
 *        @p cap of them: the packet receiver, with @p modem's numbers
 *        (fck_drv_subghz_modem.h). A packet with a bad CRC is kept, with
 *        crc_ok false, so that a caller can count them.
 * @param got Packets written, on KODE_OK; 0 when none came in @p ms.
 * @return KODE_OK; KODE_DENIED as fck_drv_cc1101_rssi() and for a @p modem
 *         the chip cannot be set to; KODE_IO_ERROR when it would not
 *         receive.
 */
kode_status_t fck_drv_cc1101_packets(const fck_drv_pins_t *hw, fck_drv_tca6408a_t *tca, uint32_t hz,
                                     const fck_drv_subghz_modem_t *modem, uint32_t ms,
                                     fck_drv_cc1101_packet_t *out, size_t cap, size_t *got);

/**
 * @brief Send one 2-FSK or GFSK packet, @p repeats times, that
 *        fck_drv_cc1101_packets() with the same @p modem receives: the same
 *        registers (fck_drv_subghz_modem.h), the preamble, the sync word, the
 *        fixed length, the CRC and the whitening @p modem says. PATABLE index
 *        0 is set to the power OOK's "on" level has on that band; the chip is
 *        left to send the packet and park in IDLE (TXOFF_MODE), which is
 *        read from MARCSTATE, once per repeat, within the packet's air time.
 *        The chip is opened, sends, and is put back to sleep, as in every
 *        call here. GDO2 stays SW1.
 * @param bytes  @p length of them.
 * @param length @p modem's length, which is 1 to FCK_DRV_SUBGHZ_MODEM_LENGTH_MAX.
 * @param repeats 1 to FCK_DRV_CC1101_SEND_REPEATS_MAX, a few milliseconds apart.
 * @return KODE_OK; KODE_DENIED when the module may not transmit on @p hz
 *         (fck_drv_subghz_scan_may_transmit()) or the packet's occupied
 *         bandwidth leaves the sub-band (fck_drv_subghz_modem_may_transmit()),
 *         the packets' air time times @p repeats is more than one call may
 *         keep the carrier on (FCK_DRV_SUBGHZ_SCAN_SEND_MS_MAX), the leg
 *         cannot be had, @p modem cannot be set, or an argument is
 *         bad; KODE_ERROR when the bus would not come up; KODE_TIMEOUT when
 *         the chip never became ready, or a packet did not end in the time
 *         it needs; KODE_IO_ERROR when it would not enter TX or its FIFO
 *         underflowed, or when fck_drv_cc1101_abort() cut it short (the chip
 *         is in IDLE). KODE_DENIED also when it was called already.
 */
kode_status_t fck_drv_cc1101_packet_send(const fck_drv_pins_t *hw, fck_drv_tca6408a_t *tca, uint32_t hz,
                                         const fck_drv_subghz_modem_t *modem, const uint8_t *bytes,
                                         uint8_t length, uint8_t repeats);

/* ---- Leaving ------------------------------------------------------------------ */

/**
 * @brief Stop transmitting, and refuse to start again: the app is going home,
 *        to sleep or to a restart; or stop what is running now: fck_cancel().
 *
 * Callable from any task at any moment, and never blocks. A send in progress
 * (fck_drv_cc1101_send(), fck_drv_cc1101_packet_send()) looks at it between
 * its repeats and every 10 ms while one plays: it puts the chip in IDLE (SIDLE,
 * which ends the carrier whatever the RMT is still clocking out), stops the RMT,
 * leaves GDO0 low and returns KODE_IO_ERROR. A send that has not keyed the PA
 * yet returns KODE_DENIED and touches nothing. The calls that listen
 * (fck_drv_cc1101_spectrum(), _find(), _record(), _listen(), _packets(), the
 * fine sweep) look at it at each step too, and return what they have so far
 * with the status they would give; a listening that never got a signal is
 * KODE_OK with nothing. It stays set until fck_drv_cc1101_abort_clear(): the
 * owner clears it when the next job starts, unless the app is leaving.
 */
void fck_drv_cc1101_abort(void);

/** @brief Allow sends again: a new start of the owner, and the suites. */
void fck_drv_cc1101_abort_clear(void);

/** @brief Whether fck_drv_cc1101_abort() was called since the last fck_drv_cc1101_abort_clear(). */
bool fck_drv_cc1101_aborted(void);

#ifdef __cplusplus
}
#endif

#endif /* FCK_DRV_CC1101_H_ */
