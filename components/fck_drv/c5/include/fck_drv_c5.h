/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file fck_drv_c5.h
 * @brief The C5 firmware's vocabulary: what to send it, how long it takes, and
 *        what its lines say.
 *
 * The module's firmware on its ESP32-C5 is fck-c5 (c5_fck_agent up to 2.0)
 * (the official firmware contract): one command per line on the C5's
 * UART0 at FCK_BSP_C5_BAUD, and back the lines fck_drv_link.h describes. This
 * file knows that firmware and nothing else. It builds a command the firmware will
 * take whole, says which #DONE verb ends it and how long the firmware may take,
 * and turns each of the firmware's machine lines into a struct.
 *
 * Like fck_drv_link, it does not write, wait or own a UART, for the same reason:
 * it is plain C, and tests/host runs it as it is. The owner writes, feeds
 * and waits:
 *
 *     fck_drv_c5_command_t c;
 *     fck_drv_c5_command_scan(&c, FCK_DRV_C5_BAND_ALL);
 *     fck_drv_link_begin(&link, c.verb, on_line, &ctx);   // on_line: fck_drv_c5_parse_network()
 *     // write c.line, c.len bytes, to the C5's UART
 *     // feed the link until fck_drv_link_done(&link, &rc), or c.ms runs out
 *     kode_status_t st = fck_drv_c5_status(rc);
 *
 * When c.ms runs out, the firmware may still be busy with the command, and it
 * carries out one at a time. So: fck_drv_link_abort(), and then either wait for
 * fck_drv_link_idle() or reset the C5 and call fck_drv_link_reset().
 *
 * WHAT THE RADIO MAY DO. Of the firmware's commands only `scan` makes the C5
 * transmit: probe requests, on 2.4 GHz only, inside the radio limits the
 * firmware applies itself (firmware/fck-c5, fck_c5_limits.h): with one of
 * them not in force (lim= of its #C5INFO is not "ok") it refuses `scan`
 * itself, rc 1. Everything else only listens: `chan`, `frames` and `ble scan`
 * (passive). fck_drv_c5_command_t.emits marks `scan`, whatever its band and
 * words. The limits are the firmware's to keep, not this file's: it does not
 * read reg= or lim= to decide anything, and a firmware of someone's own that
 * speaks these words (fck_drv_c5_is_compatible()) is asked the same. No
 * command, word or option of this file lifts a limit.
 *
 * An official firmware that prints no caps= is asked by its name and
 * version (fck_drv_c5_has()); one that keeps no radio limits itself
 * (fck_drv_c5_is_unlimited()) is sent nothing that transmits, and its
 * `sniff`, `pcap`, `deauth` and `probe` never reach it.
 *
 * WHAT RUNS ON THE C5 IS NOT ASSUMED. The module is open: anyone can write
 * their own firmware into the C5 from the Kode. So the first
 * thing to send is fck_drv_c5_command_info(). A C5 that does not answer it with a
 * #C5INFO naming the official firmware within FCK_DRV_C5_COMMAND_MS runs something else, and
 * the C5's client - the layer above the owner that sends these commands -
 * refuses every firmware command with KODE_DENIED at once rather than let it
 * run out its own, longer time. A firmware answers a command it does not have
 * with rc 2, and fck_drv_c5_has() says what one can be asked: by the caps=
 * of its #C5INFO, never by its version. The public line of versions starts
 * again at 0.x, so a version says which build runs and nothing about what
 * it can do.
 *
 * The numbers are the firmware's, read in its source (firmware/fck-c5,
 * fck_c5.c).
 */
#ifndef FCK_DRV_C5_H_
#define FCK_DRV_C5_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "kode_defs.h"
#include "fck_drv_link.h"

#ifdef __cplusplus
extern "C" {
#endif

/** The firmware's name, as #C5INFO's fw= carries it. */
#define FCK_DRV_C5_OFFICIAL_FIRMWARE       "fck-c5"

/** Another name the official firmware prints, taken as the same:
 *  fck_drv_c5_is_official(). */
#define FCK_DRV_C5_OFFICIAL_FIRMWARE_OLD   "c5_fck_agent"

/** The longest command the firmware keeps. A longer line is not cut and run: it
 *  refuses it whole with rc 2, so a builder refuses one. */
#define FCK_DRV_C5_COMMAND_MAX        255U

/** Any command not timed below: the firmware answers at once. It is also how
 *  long `info` gets before the C5 counts as running something else. */
#define FCK_DRV_C5_COMMAND_MS         3000U
/** `scan`. The firmware's protocol puts `scan all` at about 5 s; this leaves
 *  room for a firmware that scans more channels. */
#define FCK_DRV_C5_SCAN_MS        20000U
/** `chan`, per channel tried, on top of its dwell; and its margin. */
#define FCK_DRV_C5_CHANNEL_HOP_MS    100U
#define FCK_DRV_C5_CHANNEL_SLACK_MS  8000U
/** `frames`, on top of its listening time. The firmware prints each frame of its
 *  digest as it comes, and a busy channel fills the queue to the UART faster
 *  than 115200 baud empties it (11.5 kB/s, 8.8 ms for the longest line, `#F `
 *  and 96 characters and the line ending): when the listening ends there may
 *  still be that queue to print, then `#FRAMES` and `#DONE`. 3 s empties 340
 *  lines of the longest kind, more than a queue of the firmware's can hold, and
 *  leaves what the channel settling and the summary take; it is only a limit,
 *  since the command ends at its `#DONE`. Worked out from the line lengths. */
#define FCK_DRV_C5_FRAMES_SLACK_MS   3000U
/** `ble scan`, on top of its seconds. The first one also starts the BLE
 *  stack, and the firmware waits up to 2 s for the scan to report its end. */
#define FCK_DRV_C5_BLE_SLACK_MS   8000U
/** From EN let go to the firmware's #C5INFO: the ROM, the bootloader, Wi-Fi
 *  up. */
#define FCK_DRV_C5_BOOT_MS        5000U

/** `chan`'s dwell per channel: what the firmware takes, and its default. */
#define FCK_DRV_C5_DWELL_MIN_MS   20U
#define FCK_DRV_C5_DWELL_MAX_MS   5000U
#define FCK_DRV_C5_DWELL_DEFAULT_MS   250U
/** `frames`' listening time is `chan`'s dwell: the same bounds and default. */
#define FCK_DRV_C5_FRAMES_MS_MIN     FCK_DRV_C5_DWELL_MIN_MS
#define FCK_DRV_C5_FRAMES_MS_MAX     FCK_DRV_C5_DWELL_MAX_MS
#define FCK_DRV_C5_FRAMES_MS_DEFAULT FCK_DRV_C5_DWELL_DEFAULT_MS
/** The channels `frames` takes: the highest 5 GHz channel is 177. The firmware
 *  keeps to its own table, and answers any other with rc 1 or 2. */
#define FCK_DRV_C5_FRAMES_CHANNEL_MAX 177U
/** `ble scan`'s seconds: what the firmware takes, and its default. */
#define FCK_DRV_C5_BLE_SECONDS_MIN   1U
#define FCK_DRV_C5_BLE_SECONDS_MAX   60U
#define FCK_DRV_C5_BLE_SECONDS_DEFAULT   5U
/** The longest NMEA body `gtx` sends. */
#define FCK_DRV_C5_GNSS_TRANSMIT_MAX        190U

/** At most this many APs and advertisers are listed per command; a human
 *  line says how many more there were. */
#define FCK_DRV_C5_NETWORK_MAX         64U
#define FCK_DRV_C5_BLE_MAX        128U
/** A frame of `frames` is its 24-byte MAC header and a head of 0 to 46 bytes
 *  after it; the room for the head has two more. */
#define FCK_DRV_C5_FRAME_HEADER_LEN  24U
#define FCK_DRV_C5_FRAME_HEAD_MAX    48U
#define FCK_DRV_C5_FRAME_HEAD_SENT_MAX 46U
/** The bytes of one advertisement, which is what a #BLE's data= holds at most:
 *  the firmware scans with the legacy PDUs only, and one of those carries 31. */
#define FCK_DRV_C5_BLE_DATA_MAX   31U
/** The most channels `chan` lists per band: the firmware's candidates, before
 *  it drops those its country code refuses (2.4 GHz 1 to 13; 5 GHz 36 to
 *  64, 100 to 144 and 149 to 165). */
#define FCK_DRV_C5_CHANNEL_2G_MAX    13U
#define FCK_DRV_C5_CHANNEL_5G_MAX    25U
/** The RSSI of a channel where nothing was heard, and of an advertiser none
 *  of whose reports carried one. */
#define FCK_DRV_C5_RSSI_NONE      (-127)

/** @brief A band, for `scan` and `chan`, and the band an AP or channel is on. */
typedef enum {
    FCK_DRV_C5_BAND_ALL = 0,  /**< 2.4 GHz, then 5 GHz. Only in a command.  */
    FCK_DRV_C5_BAND_2G,       /**< 2.4 GHz.                                 */
    FCK_DRV_C5_BAND_5G,       /**< 5 GHz.                                   */
} fck_drv_c5_band_t;

/**
 * @brief A command, checked, with what it takes to run it.
 *
 * Filled by the builders below; the owner reads it and changes nothing.
 */
typedef struct {
    char     line[FCK_DRV_C5_COMMAND_MAX + 2U]; /**< The command and its '\n'.          */
    size_t   len;                       /**< The bytes of @c line to write.      */
    char     verb[FCK_DRV_LINK_TAG_MAX + 1U]; /**< The word its #DONE carries, for
                                         *   fck_drv_link_begin().                  */
    uint32_t ms;                        /**< The longest the firmware takes to
                                         *   answer it.                         */
    bool     emits;                     /**< It may make the C5 transmit: `scan`.
                                         *   Never for a C5 that
                                         *   fck_drv_c5_is_unlimited().          */
} fck_drv_c5_command_t;

/**
 * @brief Any command, for what has no builder of its own.
 *
 * The verb is the first word; the time is the firmware's for `scan`, `chan`
 * and `ble scan`, worked out from the command's own arguments, and
 * FCK_DRV_C5_COMMAND_MS for anything else; emits is set for `scan`. Nothing is
 * checked against what the firmware knows: an unknown command comes back with
 * rc 2.
 *
 * @param c    Filled in on KODE_OK; left alone otherwise.
 * @param text The command, without the line ending: printable ASCII,
 *             1 to FCK_DRV_C5_COMMAND_MAX characters, a first word of at most
 *             FCK_DRV_LINK_TAG_MAX.
 * @return KODE_OK; KODE_DENIED when @p text breaks any of the above.
 */
kode_status_t fck_drv_c5_command(fck_drv_c5_command_t *c, const char *text);

/** @brief `info`: the firmware says what it is, in a #C5INFO. */
kode_status_t fck_drv_c5_command_info(fck_drv_c5_command_t *c);

/**
 * @brief `health`: what changes while the firmware runs, in a #C5HEALTH -
 *        how long it has run, why it started, its free heap.
 *
 * Only for a firmware with FCK_DRV_C5_HEALTH ("health" in its caps=); any
 * other answers rc 2.
 */
kode_status_t fck_drv_c5_command_health(fck_drv_c5_command_t *c);

/**
 * @brief `scan`: the APs in range, one #AP each, then #SCANDONE.
 *
 * 2.4 GHz is scanned actively on channels 1 to 13 and 5 GHz passively on
 * 100 to 140 only: the firmware applies the limits, and this file never lists
 * a channel. TRANSMITS: emits is set, whatever the band.
 *
 * @return KODE_DENIED for a band that is not one of the three.
 */
kode_status_t fck_drv_c5_command_scan(fck_drv_c5_command_t *c, fck_drv_c5_band_t band);

/**
 * @brief `chan`: how busy each channel is, one #CHAN each, then #CHANDONE.
 *
 * The C5 only listens, in promiscuous mode, and counts frames; no address
 * and no payload leave it.
 *
 * @param dwell_ms How long each channel is listened to,
 *                 FCK_DRV_C5_DWELL_MIN_MS to FCK_DRV_C5_DWELL_MAX_MS.
 * @return KODE_DENIED for a bad band or dwell.
 */
kode_status_t fck_drv_c5_command_channel(fck_drv_c5_command_t *c, fck_drv_c5_band_t band,
                              uint16_t dwell_ms);

/**
 * @brief `ble scan`: the advertisers heard, strongest first, one #BLE each,
 *        then #BLEDONE.
 *
 * Passive: the C5 sends no scan request, so a name that only a scan
 * response carries is never seen.
 *
 * @param secs FCK_DRV_C5_BLE_SECONDS_MIN to FCK_DRV_C5_BLE_SECONDS_MAX.
 * @return KODE_DENIED for seconds out of range.
 */
kode_status_t fck_drv_c5_command_ble_scan(fck_drv_c5_command_t *c, uint8_t secs);

/**
 * @brief `frames ch=<channel> ms=<ms>`: the frames of a digest on one channel,
 *        one #F each as they come, then #FRAMES.
 *
 * The C5 only listens, in promiscuous mode, and tells one frame per sender and
 * every deauthentication; see fck_drv_c5_parse_frame(). Never transmits:
 * emits is clear. The time given is @p ms and FCK_DRV_C5_FRAMES_SLACK_MS for
 * the UART to empty what was queued. A C5 before 2.4 answers it with rc 2
 * (FCK_DRV_C5_FRAMES).
 *
 * @param channel 1 to FCK_DRV_C5_FRAMES_CHANNEL_MAX.
 * @param ms      FCK_DRV_C5_FRAMES_MS_MIN to FCK_DRV_C5_FRAMES_MS_MAX.
 * @return KODE_DENIED for a channel or time out of range.
 */
kode_status_t fck_drv_c5_command_frames(fck_drv_c5_command_t *c, uint8_t channel, uint16_t ms);

/**
 * @brief `gnss on` or `gnss off`: the GNSS receiver's UART relayed.
 *
 * While it is on, every sentence arrives as #GNSSU and every 1PPS edge as
 * #PPS, at any moment: route them with fck_drv_c5_async. A second `gnss on`
 * is sent only to a firmware that takes it (fck_drv_c5_has(),
 * FCK_DRV_C5_GNSS_REPEAT).
 */
kode_status_t fck_drv_c5_command_gnss(fck_drv_c5_command_t *c, bool on);

/**
 * @brief `gtx -n`: one sentence to the GNSS receiver, as its datasheet
 *        prints them.
 *
 * The firmware adds the '$' and the line ending, and no checksum: the ME31GR02
 * ignores a command that carries one. The change does not survive a reset of the receiver.
 *
 * @param body The sentence without '$' and without "*CS", for example
 *             "POLCFGMSG,0,2,1" (GSV on). A leading '$' is dropped.
 * @return KODE_DENIED when @p body is empty, longer than FCK_DRV_C5_GNSS_TRANSMIT_MAX,
 *         holds a space, '$', '*', '"' or '\\', or is a POLCFGPRT: the
 *         receiver would move to a baud rate the firmware does not follow,
 *         and the module would lose it. fck_drv_c5_command() still sends one.
 */
kode_status_t fck_drv_c5_command_gnss_transmit(fck_drv_c5_command_t *c, const char *body);

/**
 * @brief What a firmware's rc means for the caller.
 * @return KODE_OK for 0. KODE_IO_ERROR for 1: the C5 tried and could not,
 *         for example Wi-Fi or BLE not up. KODE_DENIED for 2: the firmware did
 *         not understand, either an argument out of its range or a command
 *         it is too old for. KODE_IO_ERROR for anything else, a #DONE without
 *         an rc included: the firmware did not do it, in a way this SDK does not
 *         know, and the typed calls answer no code that fck.h does not list.
 *
 * This KODE_DENIED does not pass with time. Some of kode-sdk's callers take
 * DENIED for "busy, ask again" (nfc_ao.c); the same command to the same
 * firmware is refused the same way, so it is not asked again.
 */
kode_status_t fck_drv_c5_status(int32_t rc);

/**
 * @brief The tags that answer no command: route them with
 *        fck_drv_link_config_t.async. NULL-terminated.
 *
 * #GNSSU and #PPS, from the GNSS relay. #C5INFO is not one: it is `info`'s
 * answer, and a boot banner reaches on_async anyway when nothing is in
 * flight.
 */
extern const char *const fck_drv_c5_async[];

/* ---- What comes back ------------------------------------------------------
 *
 * Each reader takes one machine line and fills its struct. It returns false
 * when the line has another tag or lacks a key the struct needs, and writes
 * nothing then. Keys it does not know are ignored, so a firmware that adds
 * one breaks nothing. */

/** @name The words of #C5INFO's caps= this file knows, a bit each in
 *        fck_drv_c5_info_t.caps. What each promises is the firmware's to say
 *        (firmware/fck-c5, FW_CAPS). */
/** @{ */
#define FCK_DRV_C5_CAP_SCAN    (1U << 0)   /**< "scan": `scan`, inside its limits.   */
#define FCK_DRV_C5_CAP_CHAN    (1U << 1)   /**< "chan": `chan`.                      */
#define FCK_DRV_C5_CAP_BLE     (1U << 2)   /**< "ble": `ble scan`.                   */
#define FCK_DRV_C5_CAP_GNSS    (1U << 3)   /**< "gnss": `gnss on|off`, and a `gnss
                                            *   on` while it is on is harmless.      */
#define FCK_DRV_C5_CAP_WP      (1U << 4)   /**< "wp": `wp`.                          */
#define FCK_DRV_C5_CAP_GTX     (1U << 5)   /**< "gtx": `gtx`, `gtx -n`, `gtx loop`.  */
#define FCK_DRV_C5_CAP_HEALTH  (1U << 6)   /**< "health": `health`.                  */
#define FCK_DRV_C5_CAP_BLEADV  (1U << 7)   /**< "bleadv": `ble scan` says what each
                                            *   advertiser sent.                    */
#define FCK_DRV_C5_CAP_FRAMES  (1U << 8)   /**< "frames": `frames`.                  */
/** @} */

/** @brief #C5INFO: what runs on the C5, at boot and on `info`. */
typedef struct {
    char    fw[24];     /**< "fck-c5" (or "c5_fck_agent"), or another's.  */
    char    ver[12];    /**< As printed: "0.1". "" when absent.             */
    bool    has_version; /**< ver= was there and reads as a version: the
                         *   three numbers below are it. When false they
                         *   are 0, and so is the first of "0.1": a 0 there
                         *   is a number, never "no version".               */
    uint8_t ver_major;  /**< 0 for "0.1".                                   */
    uint8_t ver_minor;  /**< 1 for "0.1".                                   */
    uint8_t ver_patch;  /**< 2 for "0.1.2"; 0 when there is none.           */
    char    chip[12];   /**< "esp32c5".                                      */
    char    rev[8];     /**< The chip's revision, "1.2".                    */
    uint8_t mac[6];     /**< Its base MAC; all zero when absent.            */
    char    reg[8];     /**< The region the radio limits are for:
                         *   "EU". "" before 2.0.                           */
    char    lim[16];    /**< "ok" once Wi-Fi is up with every limit in
                         *   force. Otherwise what stopped it: the step of
                         *   the bring-up it has not passed ("wifi-loop",
                         *   "wifi-init", "wifi-mode", "wifi-start") or the
                         *   first limit the driver refused ("country",
                         *   "bw20", "no11b", "power", "power-readback").
                         *   Anything but "ok" and `scan` is not sent, and
                         *   the firmware refuses it too. "" before 2.0.    */
    bool    has_caps;   /**< It printed caps=: what it can be asked is read
                         *   there (fck_drv_c5_has()), and its version
                         *   decides nothing.                               */
    uint16_t caps;      /**< The words of caps= this file knows,
                         *   FCK_DRV_C5_CAP_*; a word it does not know is
                         *   left out. 0 without caps=.                     */
    uint32_t uptime_ms; /**< Since it started; 0 when absent. The one
                         *   word that moves between two `info` of a boot. */
    char    rst[12];    /**< Why it started: "poweron", "ext", "sw",
                         *   "panic", "task_wdt" (its watchdog found the
                         *   command loop hung), "brownout"... "" when
                         *   absent.                                        */
} fck_drv_c5_info_t;

/**
 * @brief Read a #C5INFO.
 *
 * Only fw= is required, because a firmware other than the official firmware may print
 * a #C5INFO of its own, as the C5 firmware template does.
 *
 * A #C5INFO that arrives in the middle of any command but `info` is a boot
 * banner: the C5 restarted, and that command will not end.
 */
bool fck_drv_c5_parse_info(const fck_drv_link_message_t *m, fck_drv_c5_info_t *out);

/** @brief #C5HEALTH: what `health` answers. */
typedef struct {
    uint32_t uptime_ms; /**< Since the C5 started.                          */
    char     rst[12];   /**< Why it started, as fck_drv_c5_info_t.rst.      */
    uint32_t heap;      /**< Free heap now, in bytes.                       */
    uint32_t heap_min;  /**< The least it has been since the C5 started.    */
} fck_drv_c5_health_t;

/** @brief Read a #C5HEALTH. Requires uptime_ms, heap and heap_min. */
bool fck_drv_c5_parse_health(const fck_drv_link_message_t *m, fck_drv_c5_health_t *out);

/**
 * @brief Whether a fw= names the official firmware, by either of its names
 *        (FCK_DRV_C5_OFFICIAL_FIRMWARE, FCK_DRV_C5_OFFICIAL_FIRMWARE_OLD).
 * @param fw The fw= as read; NULL is no firmware.
 * @return true for either name, to the letter.
 */
bool fck_drv_c5_is_official(const char *fw);

/**
 * @brief Whether the firmware @p info describes speaks this file's words, so
 *        that the typed commands can be sent to it: its #C5INFO carries a
 *        caps=, whatever its fw= is. A firmware of someone's own that prints
 *        `#C5INFO fw=<its name> caps=<the words it takes>` is compatible, and
 *        is asked each command its caps= lists (fck_drv_c5_has()).
 *
 * An official firmware that prints no caps= is compatible by its name.
 *
 * @return false for NULL.
 */
bool fck_drv_c5_is_compatible(const fck_drv_c5_info_t *info);

/**
 * @brief Whether @p info is an official firmware that does not keep the
 *        radio limits itself: no caps=, and older than 2.0 or without reg=EU
 *        and lim=ok. Its `scan` is never sent. false for every firmware that
 *        prints a caps=.
 */
bool fck_drv_c5_is_unlimited(const fck_drv_c5_info_t *info);

/** @brief What a firmware can be asked: each by its word in caps=. */
typedef enum {
    FCK_DRV_C5_SCAN = 0,      /**< `scan`: caps has "scan".                     */
    FCK_DRV_C5_CHANNEL,       /**< `chan`: "chan".                              */
    FCK_DRV_C5_BLE_SCAN,      /**< `ble scan`: "ble".                           */
    FCK_DRV_C5_WAKE_PIN,      /**< `wp`: "wp".                                  */
    FCK_DRV_C5_GNSS_TRANSMIT, /**< `gtx -n` and `gtx loop`: "gtx".              */
    FCK_DRV_C5_GNSS_REPEAT,   /**< A `gnss on` while it is on is harmless:
                           *   "gnss" promises it. Without it, send `gnss
                           *   off` first.                                  */
    FCK_DRV_C5_GNSS_RELAY,    /**< `gnss on` and `gnss off`: "gnss".            */
    FCK_DRV_C5_HEALTH,        /**< `health`: "health".                          */
    FCK_DRV_C5_BLE_ADVERTISEMENT, /**< `ble scan` lists what each advertiser sent
                           *   (#BLE's evt=, first=, last= and data=):
                           *   "bleadv". Without it a #BLE carries none.   */
    FCK_DRV_C5_FRAMES,        /**< `frames`: the frames of a digest, one #F each,
                           *   and a #FRAMES: "frames". It only listens.   */
} fck_drv_c5_feature_t;

/**
 * @brief Whether the firmware @p info describes can be asked @p f.
 *
 * Decided by the caps= of its #C5INFO. Its version and its name are not
 * looked at: the public line starts again at 0.x, a number there says
 * nothing of what a firmware does, and a firmware of someone's own lists its
 * words the same way.
 *
 * An official firmware that prints no caps= is asked by its version:
 * `scan` from 1.0, `chan` and `ble scan` from 1.1, `wp` from 1.2, `gtx -n`
 * from 1.4 (1.3's `gtx` adds the checksum the receiver ignores), a harmless
 * second `gnss on` from 1.5, what each advertiser sent and `frames` from 2.4
 * (a #BLE is read by its keys all the same), the relay in every one,
 * `health` in none.
 *
 * @return false for a firmware that is not compatible
 *         (fck_drv_c5_is_compatible()), and for NULL.
 */
bool fck_drv_c5_has(const fck_drv_c5_info_t *info, fck_drv_c5_feature_t f);

/** @brief How a line an app wrote for the C5 goes out (fck_drv_c5_gate()). */
typedef enum {
    FCK_DRV_C5_GATE_REFUSE = 0, /**< Not at all.                                */
    FCK_DRV_C5_GATE_OFFICIAL,      /**< As fck_drv_c5_command() built it, with its verb and
     *                           its time.                                  */
    FCK_DRV_C5_GATE_AS_IS,      /**< As written, with the caller's verb.        */
} fck_drv_c5_gate_t;

/**
 * @brief Whether a line an app wrote may reach the C5, by what the C5 said
 *        it runs: the SDK sends nothing that transmits beyond the radio limits,
 *        and what a user writes into the C5 is theirs.
 *
 *  - The firmware, whatever its version: only its vocabulary - `info`, `scan`,
 *    `chan`, `ble scan`, `gnss`, `wp` and `gtx` - so that `ble raw` does not
 *    get through (nor `sniff`, `pcap`, `deauth` and `probe`, which a
 *    firmware without caps= may have); and a `gtx` that would move the receiver's baud rate
 *    (POLCFGPRT) does not either, as fck_drv_c5_command_gnss_transmit()
 *    already refuses it. What transmits, `scan`
 *    (fck_drv_c5_command_t.emits), goes out: the firmware keeps the radio
 *    limits itself and refuses it when one is not in force; not to one that
 *    keeps no limits itself (fck_drv_c5_is_unlimited()).
 *  - Another firmware, one whose #C5INFO names any fw= but
 *    FCK_DRV_C5_OFFICIAL_FIRMWARE to the letter, the C5 firmware template included: the
 *    line as written, checked for nothing, since its words are its
 *    programmer's. The caller names the verb its #DONE carries.
 *  - Nothing said yet: refused.
 *
 * @param info What the C5 said it runs; NULL when it has said nothing since
 *             it last started.
 * @param text The line, without its ending.
 * @param verb The verb its #DONE carries. For the official firmware NULL, or the one
 *             fck_drv_c5_command() works out (the first word: "ble" for "ble scan
 *             5"); for another firmware, required.
 * @param c    Filled in for FCK_DRV_C5_GATE_OFFICIAL: send c->line and wait for
 *             c->verb for c->ms. Left alone otherwise.
 */
fck_drv_c5_gate_t fck_drv_c5_gate(const fck_drv_c5_info_t *info, const char *text, const char *verb,
                          fck_drv_c5_command_t *c);

/** @brief An AP's security, with the firmware's names (fck_drv_c5_security_name()). */
typedef enum {
    FCK_DRV_C5_SECURITY_OPEN = 0,       /**< "open"          */
    FCK_DRV_C5_SECURITY_WEP,            /**< "wep"           */
    FCK_DRV_C5_SECURITY_WPA_PSK,        /**< "wpa_psk"       */
    FCK_DRV_C5_SECURITY_WPA2_PSK,       /**< "wpa2_psk"      */
    FCK_DRV_C5_SECURITY_WPA_WPA2_PSK,   /**< "wpa_wpa2_psk"  */
    FCK_DRV_C5_SECURITY_WPA2_ENTERPRISE,       /**< "wpa2_ent"      */
    FCK_DRV_C5_SECURITY_WPA3_PSK,       /**< "wpa3_psk"      */
    FCK_DRV_C5_SECURITY_WPA2_WPA3_PSK,  /**< "wpa2_wpa3_psk" */
    FCK_DRV_C5_SECURITY_WAPI_PSK,       /**< "wapi_psk"      */
    FCK_DRV_C5_SECURITY_OWE,            /**< "owe"           */
    FCK_DRV_C5_SECURITY_WPA3_ENTERPRISE_192,   /**< "wpa3_ent_192"  */
    FCK_DRV_C5_SECURITY_DPP,            /**< "dpp"           */
    FCK_DRV_C5_SECURITY_WPA3_ENTERPRISE,       /**< "wpa3_ent"      */
    FCK_DRV_C5_SECURITY_WPA2_WPA3_ENTERPRISE,  /**< "wpa2_wpa3_ent" */
    FCK_DRV_C5_SECURITY_WPA_ENTERPRISE,        /**< "wpa_ent"       */
    FCK_DRV_C5_SECURITY_UNKNOWN,        /**< "unknown", or a name this file does not
                                 *   know yet.                              */
} fck_drv_c5_security_t;

/** @brief The firmware's name for @p a; "unknown" for a value out of range. */
const char *fck_drv_c5_security_name(fck_drv_c5_security_t a);

/** @brief #AP: one access point from `scan`. */
typedef struct {
    char          ssid[33];  /**< NUL-terminated; "" for a hidden network.  */
    uint8_t       ssid_len;  /**< Its bytes, without the terminator.         */
    uint8_t       bssid[6];
    uint8_t       ch;        /**< The primary channel.                       */
    fck_drv_c5_band_t band;      /**< FCK_DRV_C5_BAND_2G or FCK_DRV_C5_BAND_5G.          */
    int8_t        rssi;      /**< dBm.                                       */
    fck_drv_c5_security_t auth;
} fck_drv_c5_network_t;

/** @brief Read an #AP. Requires ssid, bssid, ch and rssi. */
bool fck_drv_c5_parse_network(const fck_drv_link_message_t *m, fck_drv_c5_network_t *out);

/** @brief #CHAN: one channel from `chan`. */
typedef struct {
    uint8_t       ch;
    fck_drv_c5_band_t band;      /**< FCK_DRV_C5_BAND_2G or FCK_DRV_C5_BAND_5G.          */
    uint32_t      frames;    /**< mgmt + data + ctrl, every frame heard while
                              *   tuned there: on 2.4 GHz, the neighbouring
                              *   channels' too.                            */
    uint32_t      mgmt;
    uint32_t      data;
    uint32_t      ctrl;
    int8_t        rssi_max;  /**< dBm; FCK_DRV_C5_RSSI_NONE when frames is 0.    */
    int8_t        rssi_avg;  /**< The mean of the frames' dBm.               */
    uint16_t      dwell_ms;  /**< How long it counted, by its own 100 Hz tick:
                              *   240 to 250 for 250. 0 when absent.         */
} fck_drv_c5_channel_t;

/** @brief Read a #CHAN. Requires ch, the four counts and both RSSIs. */
bool fck_drv_c5_parse_channel(const fck_drv_link_message_t *m, fck_drv_c5_channel_t *out);

/** @brief The advertising PDU a report came in (#BLE's evt=), numbered as the
 *         controller numbers them. A scan response (4) is not here: the firmware
 *         only listens, and never asks for one. */
typedef enum {
    FCK_DRV_C5_BLE_EVENT_CONNECTABLE = 0,   /**< ADV_IND.                        */
    FCK_DRV_C5_BLE_EVENT_DIRECTED,          /**< ADV_DIRECT_IND: it carries no data. */
    FCK_DRV_C5_BLE_EVENT_SCANNABLE,         /**< ADV_SCAN_IND.                   */
    FCK_DRV_C5_BLE_EVENT_NOT_CONNECTABLE,   /**< ADV_NONCONN_IND.                */
    FCK_DRV_C5_BLE_EVENT_UNKNOWN,           /**< No evt=, as before 2.2, or a
                                             *   number that is none of these. */
} fck_drv_c5_ble_event_t;

/** @brief #BLE: one advertiser from `ble scan`. */
typedef struct {
    uint8_t  addr[6];    /**< As printed: the most significant byte first.  */
    bool     random;     /**< A random address, which many devices rotate.  */
    int8_t   rssi;       /**< dBm, of its latest report; FCK_DRV_C5_RSSI_NONE
                          *   when none carried one.                         */
    char     name[32];   /**< The complete local name, or else the short one,
                          *   NUL-terminated; "" when it sends none.         */
    uint8_t  name_len;
    uint16_t adv;        /**< Its reports heard; no duplicate filter.       */
    fck_drv_c5_ble_event_t event;   /**< Of its latest report (evt=).        */
    uint32_t first_ms;   /**< When its first report was heard, from the scan's
                          *   start (first=); 0 when absent.                 */
    uint32_t last_ms;    /**< When its latest was (last=); 0 when absent.    */
    uint8_t  data[FCK_DRV_C5_BLE_DATA_MAX];  /**< What its latest report that
                          *   carried anything carried, as sent (data=, in
                          *   hexadecimal on the line): AD structures.       */
    uint8_t  data_len;   /**< 0 with no data=, and with one that is not whole
                          *   bytes of one advertisement.                    */
} fck_drv_c5_ble_t;

/** @brief Read a #BLE. Requires addr and rssi; what 2.2 added is read when it
 *         is there, and a data= that cannot be an advertisement is left out. */
bool fck_drv_c5_parse_ble(const fck_drv_link_message_t *m, fck_drv_c5_ble_t *out);

/** @brief #F: one Wi-Fi frame of `frames`' digest, summarised.
 *
 * Everything past the 24-byte MAC header that the firmware lets out is in
 * @c head: what it is depends on the kind of frame (fc[0]: type is bits 2 and
 * 3, subtype bits 4 to 7). A beacon or probe response (type 0, subtype 8 or 5)
 * has its 12 fixed bytes (timestamp, interval, capabilities) and, when it is
 * the first tag and fits, the network's name as its tag (id 0, length, name);
 * a probe request (subtype 4), the name's tag or nothing; a deauthentication
 * or disassociation (subtype 12 or 10), its 2 bytes of reason; a data frame
 * (type 2), nothing. Nothing past that ever leaves the C5. */
typedef struct {
    uint8_t  fc[2];        /**< Frame control, as on the air.                 */
    uint8_t  channel;      /**< The channel the radio says it was received on. */
    int8_t   rssi;         /**< dBm.                                           */
    uint16_t len;          /**< Its length on the air.                         */
    uint8_t  addr1[6];     /**< As on the air: the first byte first.           */
    uint8_t  addr2[6];
    uint8_t  addr3[6];
    uint8_t  head_len;     /**< 0 to FCK_DRV_C5_FRAME_HEAD_SENT_MAX.           */
    uint8_t  head[FCK_DRV_C5_FRAME_HEAD_MAX];  /**< After the MAC header; zeros
                            *   past head_len.                                 */
} fck_drv_c5_frame_t;

/**
 * @brief Read a #F: its body is the base64 (standard alphabet, `=` padding) of
 *        a record of 24 to 70 bytes: frame control (2), channel (1), RSSI (1),
 *        length on the air (2, little endian), addresses 1, 2 and 3 (18), and
 *        the head.
 *
 * Whole or nothing: false, and @p out untouched, for a character outside the
 * alphabet, padding that is not the end of a whole group (or bits past the data
 * that are not zero), a body that is not a multiple of 4 characters, or a
 * record shorter than 24 or longer than 70 bytes. No key=value: the body is the
 * line.
 */
bool fck_drv_c5_parse_frame(const fck_drv_link_message_t *m, fck_drv_c5_frame_t *out);

/** @brief #FRAMES: the end of `frames`, with the counts of everything it heard. */
typedef struct {
    uint8_t  ch;           /**< The channel it listened on.                    */
    uint32_t ms;           /**< How long it listened, by the C5's clock.       */
    uint32_t heard;        /**< mgmt + data + ctrl: every frame the radio handed over,
                            *   whatever its channel says.                     */
    uint32_t mgmt;
    uint32_t data;
    uint32_t ctrl;
    uint32_t beacon;       /**< From here, every one of that kind, told or not. */
    uint32_t probe_request;
    uint32_t probe_response;
    uint32_t deauth;
    uint32_t disassoc;
    uint32_t sent;         /**< The #F lines the firmware printed.                 */
    uint32_t dropped;      /**< Frames its digest chose and got no line: the queue
                            *   to the UART or its table of senders was full.  */
} fck_drv_c5_frames_summary_t;

/** @brief Read a #FRAMES. Requires ch and all of its counts (the keys are ch,
 *         ms, heard, mgmt, data, ctrl, beacon, probe_req, probe_resp, deauth,
 *         disassoc, sent and dropped). */
bool fck_drv_c5_parse_frames_summary(const fck_drv_link_message_t *m, fck_drv_c5_frames_summary_t *out);

/** @brief #SCANDONE, #CHANDONE and #BLEDONE: the end of a listing. */
typedef struct {
    uint32_t n;    /**< How many lines it listed.                           */
    uint32_t ms;   /**< How long it took, by the C5's clock.                */
} fck_drv_c5_summary_t;

/** @brief Read any of the three. Requires n and ms. */
bool fck_drv_c5_parse_summary(const fck_drv_link_message_t *m, fck_drv_c5_summary_t *out);

/** @brief #WP: the two wake lines between the C5 and the expander. */
typedef struct {
    bool host_wp_c5;   /**< C5 IO0: what the expander's P1 drives to it.     */
    bool c5_wp_host;   /**< C5 IO23: what it drives to the expander's P0.    */
    bool driven;       /**< IO23 is an output (`wp 0`, `wp 1`), not let go.  */
} fck_drv_c5_wake_pin_t;

/** @brief Read a #WP. Requires host_wp_c5 and c5_wp_host. */
bool fck_drv_c5_parse_wake_pin(const fck_drv_link_message_t *m, fck_drv_c5_wake_pin_t *out);

/** @brief #PPS: one 1PPS rising edge of the GNSS receiver, while relayed. */
typedef struct {
    uint32_t n;      /**< Pulses since the `gnss on` that found the relay
                      *   off, from 1. Only with a fix.                    */
    int64_t  t_ms;   /**< The C5's uptime at the edge, taken in its
                      *   interrupt: consecutive edges are 1000 ms apart.   */
} fck_drv_c5_pps_t;

/** @brief Read a #PPS. Requires n and t_ms. */
bool fck_drv_c5_parse_pps(const fck_drv_link_message_t *m, fck_drv_c5_pps_t *out);

/**
 * @brief Read a #GTX: the sentence `gtx` wrote, without its line ending.
 * @param sent Where it goes, NUL-terminated: "$POLCFGMSG,0,2,1".
 */
bool fck_drv_c5_parse_gnss_transmit(const fck_drv_link_message_t *m, char *sent, size_t cap);

/**
 * @brief Read a #GTXLOOP: whether `gtx loop` read its own bytes back.
 * @param ok  true when the C5 drives the receiver's RX line.
 * @param got The bytes read back, of 12.
 */
bool fck_drv_c5_parse_gnss_transmit_loop(const fck_drv_link_message_t *m, bool *ok, uint32_t *got);

/* ---- What the C5 prints from a reset on --------------------------------------- */

/** The most of the image's first line fck_drv_c5_boot_t keeps. */
#define FCK_DRV_C5_BOOT_LINE_MAX  95U

/** The ROM starting this many times with no line from an image in between, or
 *  finding no image this many times, says that nothing in the flash starts. */
#define FCK_DRV_C5_BOOT_LOOP      3U

/**
 * @brief What the C5 printed on its UART0 from a reset on: its ROM, ESP-IDF's
 *        second-stage bootloader, and the image's first line.
 *
 * The ESP32-C5's ROM prints at FCK_BSP_C5_BAUD every time it starts. On
 * a KDFCK001 rev 1.0:
 *
 *     ESP-ROM:esp32c5-eco3-20250704
 *     Build:Jul  4 2025
 *     rst:0x1 (POWERON),boot:0x38 (SPI_FAST_FLASH_BOOT)
 *     SPI mode:DIO, clock div:1
 *     load:0x408556c0,len:0x578
 *     load:0x4084bba0,len:0xad4
 *     load:0x4084e5a0,len:0x2954
 *     entry 0x4084bba0
 *     #C5INFO fw=fck-c5 ver=2.1 chip=esp32c5 rev=1.2 mac=... reg=EU lim=ok
 *
 * the firmware's line 0.8 s after the ROM's last, its second-stage bootloader
 * silent. After a write cut short, the ROM's lines came alone, again every
 * 170 ms, with rst:0x3 (RTC_SW_HPSYS). A flash with nothing where the
 * bootloader goes makes Espressif's ROMs say "invalid header" instead of
 * jumping. An image may log through ESP-IDF before it prints a line of its
 * own, so its own line is waited for and its first log line kept for one
 * that prints nothing else. An image whose early log warning ("W (163)
 * spi_flash: CPU frequency is set to 240MHz...") is cut short by its own
 * UART setup, with its first line glued behind it on the same line, is read
 * by that log line.
 *
 * This reads those lines as they come, in pieces of any size, and keeps what
 * they say. Plain C: fck_drv_c5_rom_listen() reads the UART and feeds it.
 */
typedef struct {
    uint32_t restarts;    /**< Times the ROM started ("ESP-ROM:").            */
    uint32_t no_image;    /**< Times it found no image ("invalid header").    */
    bool     entry;       /**< It jumped into a second-stage bootloader.     */
    bool     bootloader;  /**< ESP-IDF's second-stage bootloader printed.    */
    char     line[FCK_DRV_C5_BOOT_LINE_MAX + 1U]; /**< The image's first line
                           *   of its own - not an ESP-IDF log line - cut to
                           *   fit, escape sequences left out; "" until one
                           *   comes.                                        */
    char     log_line[FCK_DRV_C5_BOOT_LINE_MAX + 1U]; /**< Its first ESP-IDF
                           *   log line, the bootloader's left out, kept the
                           *   same way; "" until one comes.                 */
    /* The line being put together: not for the caller. */
    char     part[128];
    size_t   n;
    bool     escape;
} fck_drv_c5_boot_t;

/** @brief Start reading from a reset. */
void fck_drv_c5_boot_init(fck_drv_c5_boot_t *b);

/** @brief Read @p n more bytes of what the C5 printed. Once
 *         fck_drv_c5_boot_done(), what comes changes nothing. */
void fck_drv_c5_boot_feed(fck_drv_c5_boot_t *b, const uint8_t *data, size_t n);

/** @brief Whether there is nothing left to wait for: the image printed a
 *         line of its own, or the ROM started, or found no image,
 *         FCK_DRV_C5_BOOT_LOOP times. A log line does not end the wait.
 *         true for NULL. */
bool fck_drv_c5_boot_done(const fck_drv_c5_boot_t *b);

/** @brief What the image said first: its first line of its own, or its first
 *         log line when it printed none; "" for neither, and for NULL. */
const char *fck_drv_c5_boot_line(const fck_drv_c5_boot_t *b);

#ifdef __cplusplus
}
#endif

#endif /* FCK_DRV_C5_H_ */
