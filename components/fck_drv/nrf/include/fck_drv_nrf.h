/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file fck_drv_nrf.h
 * @brief The nRF firmware's vocabulary: what to send it, how long it takes, and
 *        what its lines say.
 *
 * The module's firmware on its nRF54L15 is fck-nrf (nrf_fck_agent up to 2.0), behind
 * MCUboot (the official firmware contract): a Zephyr shell with no prompt
 * and no echo on the nRF's one UART at FCK_BSP_NRF_BAUD, and back the lines
 * fck_drv_link.h describes. The same UART carries mcumgr SMP, which is how a new
 * image goes in; that is not this file's. This file knows the firmware's text
 * commands and nothing else. It builds a command the shell will take, says
 * which #DONE verb ends it and how long the firmware may take, and turns each of
 * the firmware's machine lines into a struct.
 *
 * Like fck_drv_link and fck_drv_c5, it does not write, wait or own a UART: it is
 * plain C, and tests/host runs it as it is. The owner writes, feeds and
 * waits:
 *
 *     fck_drv_nrf_command_t c;
 *     fck_drv_nrf_command_spectrum(&c, 2400, 2483, 200, 1);
 *     fck_drv_link_begin(&link, c.verb, on_line, &ctx);  // on_line: fck_drv_nrf_parse_spectrum()
 *     // write c.line, c.len bytes, to the nRF's UART
 *     // feed the link until fck_drv_link_done(&link, &rc), or c.ms runs out
 *     kode_status_t st = fck_drv_nrf_status(rc);
 *
 * Every frequency here is in MHz. The firmware counts channels from 2400 MHz
 * (`ch` = MHz - 2400); the builders and the readers convert.
 *
 * WHAT THE RADIO MAY DO. Two commands make the nRF transmit: `rf ping` and
 * `rf echo on` (the responder answers every ping it hears, until `rf off`).
 * The builders, and fck_drv_nrf_command(), keep both inside the module's
 * radio limits: 2402 to 2480 MHz, at most
 * FCK_DRV_NRF_TRANSMIT_DBM_MAX. `rf cw`, an unmodulated carrier, is refused
 * outright. The firmware keeps the same limits in its
 * own image (firmware/fck-nrf, fck_nrf_limits.h) and says so with the word
 * `eu` in its #NRFINFO's caps=; this file keeps them a second time, whatever
 * runs. No command, word or option of this file lifts a limit.
 * fck_drv_nrf_command_t.emits says which commands transmit, for a caller
 * that has to allow them.
 *
 * WHAT RUNS ON THE nRF IS NOT ASSUMED. The module is open: anyone can write
 * their own image from the Kode, and MCUboot boots it. The
 * firmware prints #NRFINFO about FCK_BSP_NRF_BOOT_TO_FIRMWARE_MS after a reset,
 * once MCUboot's recovery window has closed - what is sent before then goes
 * to MCUboot - and again on `info`. An nRF whose #NRFINFO names any fw=
 * but FCK_DRV_NRF_OFFICIAL_FIRMWARE, to the letter, runs something else: fck_drv_nrf_gate()
 * then lets a line through as its caller wrote it, checked for nothing, the
 * radio included, since its words are its programmer's. One that has said
 * nothing is sent nothing.
 *
 * The numbers are the firmware's, read in its source (firmware/fck-nrf:
 * rf.c, main.c, prj.conf). The times are worked
 * out from that source and FCK_BSP_NRF_BAUD.
 */
#ifndef FCK_DRV_NRF_H_
#define FCK_DRV_NRF_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "kode_defs.h"
#include "fck_drv_link.h"

#ifdef __cplusplus
extern "C" {
#endif

/** The firmware's name, as #NRFINFO's fw= carries it. */
#define FCK_DRV_NRF_OFFICIAL_FIRMWARE       "fck-nrf"

/** Another name the official firmware prints, taken as the same:
 *  fck_drv_nrf_is_official(). */
#define FCK_DRV_NRF_OFFICIAL_FIRMWARE_OLD   "nrf_fck_agent"

/** The longest command built: the firmware's longest is under 40 characters,
 *  and the shell's own buffer (CONFIG_SHELL_CMD_BUFF_SIZE) is 256. */
#define FCK_DRV_NRF_COMMAND_MAX       127U

/** Any command not timed below, `info` included: the firmware answers at
 *  once. */
#define FCK_DRV_NRF_COMMAND_MS        3000U

/** What every time below adds for the shell, the UART and the owner. */
#define FCK_DRV_NRF_SLACK_MS      2000U

/** What the radio listens to (`rf spec`, `rf sniff`): the firmware's channels
 *  0 to 100. */
#define FCK_DRV_NRF_RECEIVE_MHZ_MIN    2400U
#define FCK_DRV_NRF_RECEIVE_MHZ_MAX    2500U
/** Where it may transmit (`rf ping`, `rf echo on`): the module's
 *  band. */
#define FCK_DRV_NRF_TRANSMIT_MHZ_MIN    2402U
#define FCK_DRV_NRF_TRANSMIT_MHZ_MAX    2480U
/** The power it may transmit at, in dBm. The firmware snaps a request down to
 *  a step of the nRF54L15's table; 6 is one. Below -46 it gives -46. */
#define FCK_DRV_NRF_TRANSMIT_DBM_MIN    (-46)
#define FCK_DRV_NRF_TRANSMIT_DBM_MAX    6
/** Where ping and the echo responder go when no frequency is given. */
#define FCK_DRV_NRF_LINK_MHZ_DEFAULT  2440U

/** `rf spec`: the dwell per channel, and how many sweeps. */
#define FCK_DRV_NRF_DWELL_MIN_US  10U
#define FCK_DRV_NRF_DWELL_MAX_US  100000U
#define FCK_DRV_NRF_SWEEPS_MAX    1000U
/** The most channels one #SPEC24 carries: all of 2400 to 2500. */
#define FCK_DRV_NRF_SPECTRUM_MAX      101U

/** `rf ping`: how many packets, and the pause after each. */
#define FCK_DRV_NRF_PING_MAX      1000U
#define FCK_DRV_NRF_GAP_MAX_MS    1000U

/** `rf sniff`: how long it listens, and the weakest frame it keeps. */
#define FCK_DRV_NRF_SNIFF_MIN_MS  100U
#define FCK_DRV_NRF_SNIFF_MAX_MS  600000U
#define FCK_DRV_NRF_RSSI_MIN_DEFAULT  (-60)
/** The bytes kept of each frame heard (`rf scfg`'s len), and how many
 *  different frames one `rf sniff` remembers. */
#define FCK_DRV_NRF_FRAME_MAX     40U
#define FCK_DRV_NRF_FRAMES_MAX    48U

/** An RSSI a line did not carry. The firmware's own readings run from -127
 *  (nothing heard) to 0. */
#define FCK_DRV_NRF_RSSI_NONE     INT8_MIN
/** What the firmware reads where nothing was heard. */
#define FCK_DRV_NRF_RSSI_QUIET    (-127)

/**
 * @brief A command, checked, with what it takes to run it.
 *
 * Filled by the builders below; the owner reads it and changes nothing.
 */
typedef struct {
    char     line[FCK_DRV_NRF_COMMAND_MAX + 2U];   /**< The command and its '\n'.       */
    size_t   len;                          /**< The bytes of @c line to write.  */
    char     verb[FCK_DRV_LINK_TAG_MAX + 1U];  /**< The word its #DONE carries, for
                                            *   fck_drv_link_begin(): "info",
                                            *   "rf_spec", "rf_ping"...       */
    uint32_t ms;                           /**< The longest the firmware takes to
                                            *   answer it.                     */
    bool     emits;                        /**< It makes the nRF transmit.     */
} fck_drv_nrf_command_t;

/**
 * @brief Any command, for what has no builder of its own.
 *
 * Only the firmware's two commands, `info` and `rf <sub> ...`, and only the
 * sub-commands this file knows: an image may have sub-commands of its own,
 * which end with a #DONE nothing here would wait for. A firmware with
 * "done" in its caps= ends every line with a #DONE, whatever it held; what
 * is refused below for want of one is refused all the same, for an image
 * without "done" whose refusals end with no #DONE. The verb is the firmware's: `rf_<sub>`,
 * or `rf` for `rf` alone (its usage line, rc 2). The time is worked out from
 * the arguments.
 *
 * Where a number decides what the radio does - the power of `rf power`, the
 * channel and every number of `rf ping`, the channel of `rf echo on` - it
 * must be written plainly: an optional '-' and 1 to 6 decimal digits, no
 * leading zero. The firmware reads numbers with strtol, base 0, so "010" is 8
 * to it; a number written otherwise is refused, not read.
 *
 * @param c    Filled in on KODE_OK; left alone otherwise.
 * @param text The command, without the line ending: printable ASCII with no
 *             quote and no backslash (the shell would split it otherwise),
 *             1 to FCK_DRV_NRF_COMMAND_MAX characters, at most 8 arguments after the
 *             sub-command (the shell refuses more without a #DONE), and no
 *             word that starts with '-' but a plain negative number (`-h`
 *             and `--help` print the shell's help and run nothing).
 * @return KODE_OK; KODE_DENIED when @p text breaks any of the above, names
 *         a sub-command this file does not know, or asks the radio for what
 *         it may not do: `rf cw`, `rf power` above FCK_DRV_NRF_TRANSMIT_DBM_MAX,
 *         `rf ping` or `rf echo on` outside FCK_DRV_NRF_TRANSMIT_MHZ_MIN to
 *         FCK_DRV_NRF_TRANSMIT_MHZ_MAX.
 */
kode_status_t fck_drv_nrf_command(fck_drv_nrf_command_t *c, const char *text);

/** @brief `info`: the firmware says what it is, in a #NRFINFO. */
kode_status_t fck_drv_nrf_command_info(fck_drv_nrf_command_t *c);

/**
 * @brief `rf spec`: the peak RSSI on each 1 MHz from @p mhz_first to
 *        @p mhz_last, one #SPEC24 per sweep. Only listens.
 *
 * @param dwell_us How long each MHz is listened to, FCK_DRV_NRF_DWELL_MIN_US to
 *                 FCK_DRV_NRF_DWELL_MAX_US. The firmware's default is 200.
 * @param sweeps   1 to FCK_DRV_NRF_SWEEPS_MAX.
 * @return KODE_DENIED for a range outside FCK_DRV_NRF_RECEIVE_MHZ_MIN to
 *         FCK_DRV_NRF_RECEIVE_MHZ_MAX, reversed, or a bad dwell or count.
 */
kode_status_t fck_drv_nrf_command_spectrum(fck_drv_nrf_command_t *c, uint16_t mhz_first, uint16_t mhz_last,
                               uint32_t dwell_us, uint16_t sweeps);

/** @brief `rf off`: the radio idle, the echo responder stopped. #RFOFF. */
kode_status_t fck_drv_nrf_command_off(fck_drv_nrf_command_t *c);

/**
 * @brief `rf power`: the level `rf ping` and `rf echo` transmit at. #POWER24
 *        says the step the firmware took. Nothing is sent until one of them
 *        runs. At boot the firmware is at 0 dBm.
 * @return KODE_DENIED outside FCK_DRV_NRF_TRANSMIT_DBM_MIN to FCK_DRV_NRF_TRANSMIT_DBM_MAX.
 */
kode_status_t fck_drv_nrf_command_power(fck_drv_nrf_command_t *c, int8_t dbm);

/**
 * @brief `rf echo on` or `rf echo off`: the ping responder. TRANSMITS when on.
 *
 * On, it answers every ping it hears on @p mhz with the RSSI it measured,
 * in the background, until `rf echo off`, `rf off`, or any other radio
 * command - or until no ping has reached it for the seconds its #ECHO24
 * says (idle_s: 60 in fck-nrf 0.1.0), when it stops by itself, radio off.
 * Off, it says what it saw (#ECHOSTAT24) and stops.
 *
 * @param mhz Ignored when off. FCK_DRV_NRF_TRANSMIT_MHZ_MIN to FCK_DRV_NRF_TRANSMIT_MHZ_MAX.
 * @return KODE_DENIED for a frequency outside them.
 */
kode_status_t fck_drv_nrf_command_echo(fck_drv_nrf_command_t *c, bool on, uint16_t mhz);

/** @brief `rf echo`: what the responder has seen, as a #ECHOSTAT24. */
kode_status_t fck_drv_nrf_command_echo_statistics(fck_drv_nrf_command_t *c);

/**
 * @brief `rf ping`: @p n packets to a responder in `rf echo on` on @p mhz,
 *        one #PING24 each, then #PINGDONE24. TRANSMITS.
 *
 * Only an echo answers, and each echo carries its responder's node id, so
 * a ping counts one F*CK and never the one beside it (#PINGDONE24 peer=).
 * rc 1 when no echo came back at all.
 *
 * @param n      1 to FCK_DRV_NRF_PING_MAX.
 * @param mhz    FCK_DRV_NRF_TRANSMIT_MHZ_MIN to FCK_DRV_NRF_TRANSMIT_MHZ_MAX.
 * @param gap_ms The pause after each packet, 0 to FCK_DRV_NRF_GAP_MAX_MS. The
 *               firmware's default is 10.
 * @return KODE_DENIED for any of them out of range.
 */
kode_status_t fck_drv_nrf_command_ping(fck_drv_nrf_command_t *c, uint16_t n, uint16_t mhz, uint16_t gap_ms);

/**
 * @brief `rf sniff`: the 2.4 GHz bursts heard on @p mhz, of a remote, a
 *        mouse, a toy; no address needs to be known. Only listens.
 *
 * Each frame different from the ones before arrives at once as a #SNIFF24;
 * at the end, every one again as a #SNIFFTOP24, the most repeated first, and
 * #SNIFFDONE24. What it locks on to is fck_drv_nrf_command_sniff_config()'s and
 * fck_drv_nrf_command_sniff_address()'s.
 *
 * @param ms       FCK_DRV_NRF_SNIFF_MIN_MS to FCK_DRV_NRF_SNIFF_MAX_MS.
 * @param rssi_min Frames weaker than this, in dBm, are counted and dropped:
 *                 -127 to 0. The firmware's default is FCK_DRV_NRF_RSSI_MIN_DEFAULT.
 * @return KODE_DENIED for any of them out of range.
 */
kode_status_t fck_drv_nrf_command_sniff(fck_drv_nrf_command_t *c, uint16_t mhz, uint32_t ms, int8_t rssi_min);

/** @brief How `rf sniff` reads the air: #SCFG24, and part of #SNIFFSTART24. */
typedef struct {
    uint8_t rate_mbps;   /**< 1 or 2 (GFSK). At boot, 1.                    */
    uint8_t crc_bytes;   /**< 0, every frame kept; or 2, only frames whose
                          *   CRC-16 checks. At boot, 0.                    */
    uint8_t len;         /**< The bytes kept of each frame, 1 to
                          *   FCK_DRV_NRF_FRAME_MAX. At boot, 32.              */
} fck_drv_nrf_sniff_config_t;

/** @brief What `rf sniff` locks on to: #SADDR24, and part of #SNIFFSTART24.
 *  The radio's own registers, BASE0 and PREFIX0. */
typedef struct {
    uint8_t  balen;      /**< BASE0's bytes in the address, 2 to 4; the
                          *   address is one more. At boot, 2.              */
    uint32_t base;       /**< At boot 0xAAAAAAAA: a preamble run.          */
    uint8_t  prefix;     /**< At boot 0xAA.                                */
} fck_drv_nrf_sniff_address_t;

/**
 * @brief `rf scfg`: set how `rf sniff` reads the air. Kept until the nRF
 *        resets.
 * @return KODE_DENIED for a rate, CRC or length the firmware does not take.
 */
kode_status_t fck_drv_nrf_command_sniff_config(fck_drv_nrf_command_t *c, const fck_drv_nrf_sniff_config_t *phy);

/**
 * @brief `rf saddr`: set what `rf sniff` locks on to. Kept until the nRF
 *        resets.
 * @return KODE_DENIED for a balen outside 2 to 4.
 */
kode_status_t fck_drv_nrf_command_sniff_address(fck_drv_nrf_command_t *c, const fck_drv_nrf_sniff_address_t *addr);

/**
 * @brief What a firmware's rc means for the caller.
 * @return KODE_OK for 0. KODE_IO_ERROR for 1: the nRF tried and could not -
 *         its clock did not start, a channel would not listen, no echo came
 *         back. KODE_DENIED for 2: an argument the firmware does not take, or a
 *         sub-command it does not have. KODE_IO_ERROR for anything else, a
 *         #DONE without an rc included: the firmware did not do it, in a way
 *         this SDK does not know, and the typed calls answer no code that
 *         fck.h does not list.
 */
kode_status_t fck_drv_nrf_status(int32_t rc);

/* ---- What comes back ------------------------------------------------------
 *
 * Each reader takes one machine line and fills its struct. It returns false
 * when the line has another tag or lacks a key the struct needs, and writes
 * nothing then. Keys it does not know are ignored, so a firmware that adds
 * one breaks nothing. A frequency the firmware prints as a channel comes back
 * in MHz. */

/** @name The words of #NRFINFO's caps= this file knows, a bit each in
 *        fck_drv_nrf_info_t.caps. What each promises is the firmware's to
 *        say (firmware/fck-nrf, FCK_NRF_CAPS). */
/** @{ */
#define FCK_DRV_NRF_CAP_SPEC      (1U << 0)   /**< "spec": `rf spec`.                */
#define FCK_DRV_NRF_CAP_OFF       (1U << 1)   /**< "off": `rf off`.                  */
#define FCK_DRV_NRF_CAP_POWER     (1U << 2)   /**< "power": `rf power`.              */
#define FCK_DRV_NRF_CAP_ECHO      (1U << 3)   /**< "echo": `rf echo`.                */
#define FCK_DRV_NRF_CAP_PING      (1U << 4)   /**< "ping": `rf ping`.                */
#define FCK_DRV_NRF_CAP_SNIFF     (1U << 5)   /**< "sniff": `rf sniff`.              */
#define FCK_DRV_NRF_CAP_SCFG      (1U << 6)   /**< "scfg": `rf scfg`.                */
#define FCK_DRV_NRF_CAP_SADDR     (1U << 7)   /**< "saddr": `rf saddr`.              */
#define FCK_DRV_NRF_CAP_EU        (1U << 8)   /**< "eu": it keeps the module's
                                               *   radio limits itself, and nothing
                                               *   in it lifts them.                 */
#define FCK_DRV_NRF_CAP_DONE      (1U << 9)   /**< "done": every command line ends
                                               *   with #DONE, whatever it held.     */
#define FCK_DRV_NRF_CAP_WDT       (1U << 10)  /**< "wdt": a watchdog resets a
                                               *   firmware that stops serving
                                               *   commands.                         */
#define FCK_DRV_NRF_CAP_ECHOIDLE  (1U << 11)  /**< "echoidle": the echo responder
                                               *   stops by itself when no ping
                                               *   reaches it for a while.           */
#define FCK_DRV_NRF_CAP_SMP       (1U << 12)  /**< "smp": mcumgr SMP on this UART.
                                               *   Nothing here decides by it: how
                                               *   an image is written is the
                                               *   bootloader's (fck_drv_nrf_dfu.h). */
/** @} */

/** @brief #NRFINFO: what runs on the nRF, at boot and on `info`. */
typedef struct {
    char     fw[24];      /**< "fck-nrf" (or "nrf_fck_agent"), or another's.  */
    char     ver[12];     /**< As printed: "0.1.0". "" when absent.         */
    bool     has_version; /**< ver= was there and reads as a version: the
                           *   three numbers below are it. When false they
                           *   are 0, and so is the first of "0.1.0": a 0
                           *   there is a number, never "no version".       */
    uint8_t  ver_major;   /**< 0 for "0.1.0".                               */
    uint8_t  ver_minor;
    uint8_t  ver_patch;   /**< 0 when there is none.                        */
    uint64_t devid;       /**< FICR DEVICEID, the high word first as printed;
                           *   0 when absent.                               */
    uint8_t  node;        /**< Its low byte: the node id ping and echo carry. */
    uint32_t reset;       /**< RESETREAS as the boot latched it; 0 when absent. */
    uint32_t uptime_ms;   /**< 0 when absent.                               */
    bool     has_map;     /**< It printed map=: it says where it was linked. */
    uint32_t map;         /**< Where its code starts in the nRF's flash, the
                           *   address it was linked for: 0x0000E800 for the
                           *   module's map (FCK_BSP_NRF_SLOT0 plus the
                           *   image's header). The image carries the same
                           *   number in its FCKMAP tag
                           *   (fck_drv_smp_image_map()). 0 without map=.    */
    bool     has_caps;    /**< It printed caps=: what it can be asked is read
                           *   there (fck_drv_nrf_has()), and its version
                           *   decides nothing.                             */
    uint16_t caps;        /**< The words of caps= this file knows,
                           *   FCK_DRV_NRF_CAP_*; a word it does not know is
                           *   left out. 0 without caps=.                   */
} fck_drv_nrf_info_t;

/**
 * @brief Read a #NRFINFO.
 *
 * Only fw= is required, because an image other than the official firmware may print a
 * #NRFINFO of its own. One that arrives in the middle of any command but
 * `info` is a boot banner: the nRF restarted, and that command will not end.
 * A link that names #NRFINFO its banner (fck_drv_link_config_t.banner) ends it.
 */
bool fck_drv_nrf_parse_info(const fck_drv_link_message_t *m, fck_drv_nrf_info_t *out);

/**
 * @brief Whether a fw= names the official firmware, by either of its names
 *        (FCK_DRV_NRF_OFFICIAL_FIRMWARE, FCK_DRV_NRF_OFFICIAL_FIRMWARE_OLD).
 * @param fw The fw= as read; NULL is no firmware.
 * @return true for either name, to the letter.
 */
bool fck_drv_nrf_is_official(const char *fw);

/**
 * @brief Whether the image @p info describes speaks this file's words, so
 *        that the typed commands can be sent to it: its #NRFINFO carries a
 *        caps=, whatever its fw= is. An image of someone's own that prints
 *        `#NRFINFO fw=<its name> caps=<the words it takes>` is compatible,
 *        and is asked each command its caps= lists (fck_drv_nrf_has()).
 *
 * An official image that prints no caps= is compatible by its name.
 *
 * @return false for NULL.
 */
bool fck_drv_nrf_is_compatible(const fck_drv_nrf_info_t *info);

/**
 * @brief Whether @p info is an official image that does not keep the radio
 *        limits itself: no caps=, and older than 2.0 (it keeps the power
 *        another app set, up to 8 dBm). What makes it transmit is never
 *        sent. false for every image that prints a caps=.
 */
bool fck_drv_nrf_is_unlimited(const fck_drv_nrf_info_t *info);

/** @brief What an image can be asked: each by its word in caps=. */
typedef enum {
    FCK_DRV_NRF_SPECTRUM = 0,  /**< `rf spec`: "spec".                         */
    FCK_DRV_NRF_PING,          /**< `rf ping` at a level set with `rf power`:
                            *   "ping" and "power".                        */
    FCK_DRV_NRF_ECHO,          /**< `rf echo` at a level set with `rf power`,
                            *   stopped with `rf off`: "echo", "power" and
                            *   "off".                                     */
    FCK_DRV_NRF_SNIFF,         /**< `rf sniff`, set with `rf scfg` and `rf
                            *   saddr`: "sniff", "scfg" and "saddr".       */
} fck_drv_nrf_feature_t;

/**
 * @brief Whether the image @p info describes can be asked @p f.
 *
 * Decided by the caps= of its #NRFINFO. Its version and its name are not
 * looked at: the public line starts again at 0.x, a number there says
 * nothing of what an image does, and an image of someone's own lists its
 * words the same way. The commands this file builds stay inside the
 * module's radio limits for any image: at most 6 dBm, 2402 to 2480 MHz, no `rf cw`.
 *
 * An official image that prints no caps= has the four radio commands
 * whatever its version.
 *
 * @return false for an image that is not compatible
 *         (fck_drv_nrf_is_compatible()), and for NULL.
 */
bool fck_drv_nrf_has(const fck_drv_nrf_info_t *info, fck_drv_nrf_feature_t f);

/** @brief How a line an app wrote for the nRF goes out (fck_drv_nrf_gate()). */
typedef enum {
    FCK_DRV_NRF_GATE_REFUSE = 0, /**< Not at all.                                */
    FCK_DRV_NRF_GATE_OFFICIAL,      /**< As fck_drv_nrf_command() built it, with its verb
                              *   and its time.                             */
    FCK_DRV_NRF_GATE_AS_IS,      /**< As written, with the caller's verb.       */
} fck_drv_nrf_gate_t;

/**
 * @brief Whether a line an app wrote may reach the nRF, by what the nRF
 *        said it runs: the SDK sends nothing that transmits beyond the
 *        module's radio limits, and what a user writes into the nRF is theirs.
 *
 *  - The firmware: only its vocabulary, as fck_drv_nrf_command() takes it, which
 *    keeps the radio inside the module's limits. What makes it transmit is not
 *    sent to one that keeps no limits itself, fck_drv_nrf_is_unlimited():
 *    it keeps the power another app set, up to 8 dBm.
 *  - Another image, one whose #NRFINFO names any fw= but
 *    FCK_DRV_NRF_OFFICIAL_FIRMWARE to the letter: the line as written, checked for
 *    nothing, `rf cw` and any power included, since its words are its
 *    programmer's. The caller names the verb its #DONE carries.
 *  - Nothing said yet: refused.
 *
 * @param info What the nRF said it runs; NULL when it has said nothing since
 *             it last started.
 * @param text The line, without its ending.
 * @param verb The verb its #DONE carries. For the official firmware NULL, or the one
 *             fck_drv_nrf_command() works out; for another image, required.
 * @param c    Filled in for FCK_DRV_NRF_GATE_OFFICIAL.
 */
fck_drv_nrf_gate_t fck_drv_nrf_gate(const fck_drv_nrf_info_t *info, const char *text, const char *verb,
                            fck_drv_nrf_command_t *c);

/** @brief #ERR: what a command could not do, before its #DONE. */
typedef struct {
    char    cmd[16];   /**< "rf_spec", "rf_init"...: cmd= or what=.        */
    char    why[16];   /**< "hfxo", "not_in_rx"; "" when absent.           */
    int32_t code;      /**< err=, rc= or points=, whichever it carries:
                        *   the clock driver's error, or how many channels
                        *   would not listen. 0 when none.                 */
} fck_drv_nrf_error_t;

/** @brief Read a #ERR. Requires cmd= or what=. */
bool fck_drv_nrf_parse_error(const fck_drv_link_message_t *m, fck_drv_nrf_error_t *out);

/** @brief #SPEC24: one sweep of `rf spec`. */
typedef struct {
    uint16_t f0_mhz;                    /**< dbm[0]'s frequency.            */
    uint8_t  n;                         /**< Channels, 1 MHz apart.         */
    uint16_t sweep;                     /**< From 0.                        */
    int8_t   dbm[FCK_DRV_NRF_SPECTRUM_MAX];     /**< The peak RSSI of each, dBm:
                                         *   -127 for nothing heard, and for
                                         *   a channel the radio would not
                                         *   listen on (a #ERR follows).    */
} fck_drv_nrf_spectrum_t;

/** @brief Read a #SPEC24. Requires f0, n, sweep, and exactly n values. */
bool fck_drv_nrf_parse_spectrum(const fck_drv_link_message_t *m, fck_drv_nrf_spectrum_t *out);

/**
 * @brief Fold one sweep into the peak of each MHz: @p dbm[i] is
 *        @p first_mhz + i, started at FCK_DRV_NRF_RSSI_QUIET by the caller.
 *        The sweep's channels outside the @p n kept are left out.
 */
void fck_drv_nrf_spectrum_peak(const fck_drv_nrf_spectrum_t *sweep, uint16_t first_mhz, int8_t *dbm, size_t n);

/** @brief #POWER24: the step `rf power` took. */
typedef struct {
    int16_t req;   /**< What was asked.                                    */
    int8_t  dbm;   /**< The step: the largest at or below it.              */
} fck_drv_nrf_power_t;

/** @brief Read a #POWER24. Requires req and dbm. */
bool fck_drv_nrf_parse_power(const fck_drv_link_message_t *m, fck_drv_nrf_power_t *out);

/** @brief #ECHO24: the responder turned on (with where) or off. */
typedef struct {
    bool     on;
    uint16_t mhz;    /**< 0 when off.                                       */
    int8_t   dbm;    /**< The level it answers at; 0 when off.              */
    uint8_t  node;   /**< The node id its echoes carry; 0 when off.         */
    uint16_t idle_s; /**< It stops by itself, radio off, when no ping has
                      *   reached it for this many seconds (the firmware's
                      *   "echoidle"); 0 when off, and for a firmware that
                      *   does not say: that one answers until told to stop. */
} fck_drv_nrf_echo_t;

/** @brief Read a #ECHO24. Requires on, and mhz, dbm and node when on. */
bool fck_drv_nrf_parse_echo(const fck_drv_link_message_t *m, fck_drv_nrf_echo_t *out);

/** @brief #ECHOSTAT24: what the responder has seen since it was turned on. */
typedef struct {
    bool     on;
    uint16_t mhz;
    uint32_t rx;        /**< Packets heard.                                */
    uint32_t crcbad;    /**< Heard with a bad CRC.                         */
    uint32_t replied;   /**< Echoes sent.                                  */
    uint32_t foreign;   /**< Heard, but not a ping.                        */
    uint32_t txslow;    /**< Transmissions the radio did not end by itself:
                         *   the firmware switched it off about 0.3 ms
                         *   after the packet, before it could idle on an
                         *   unmodulated carrier. Not 0 is a fault of the
                         *   nRF's radio.                                   */
    bool     idle;      /**< It is off because it stopped by itself, no
                         *   ping having reached it in time; false when the
                         *   firmware does not say.                         */
} fck_drv_nrf_echo_statistics_t;

/** @brief Read a #ECHOSTAT24. Requires on, ch and the four counts. */
bool fck_drv_nrf_parse_echo_statistics(const fck_drv_link_message_t *m, fck_drv_nrf_echo_statistics_t *out);

/** @brief #PINGSTART24: what `rf ping` is about to do. */
typedef struct {
    uint16_t n;
    uint16_t mhz;
    int8_t   dbm;
    uint8_t  node;         /**< This nRF's node id.                         */
    uint16_t timeout_ms;   /**< How long it waits for each echo.            */
} fck_drv_nrf_ping_start_t;

/** @brief Read a #PINGSTART24. Requires n, ch, dbm and node. */
bool fck_drv_nrf_parse_ping_start(const fck_drv_link_message_t *m, fck_drv_nrf_ping_start_t *out);

/** @brief Why a ping got no good echo. */
typedef enum {
    FCK_DRV_NRF_PING_OK = 0,      /**< It did.                                  */
    FCK_DRV_NRF_PING_TRANSMIT_FAILED,      /**< The packet did not go out.               */
    FCK_DRV_NRF_PING_TIMEOUT,     /**< Nothing came back in time.               */
    FCK_DRV_NRF_PING_CRC,         /**< Something came back, with a bad CRC.     */
    FCK_DRV_NRF_PING_MISMATCH,    /**< Something came back that was not its echo. */
    FCK_DRV_NRF_PING_OTHER,       /**< A why= this file does not know yet.      */
} fck_drv_nrf_ping_why_t;

/** @brief #PING24: one packet of `rf ping`. */
typedef struct {
    uint16_t           seq;        /**< From 1.                              */
    bool               ok;
    fck_drv_nrf_ping_why_t why;
    int8_t             rssi_fwd;   /**< What the responder heard, dBm;
                                    *   FCK_DRV_NRF_RSSI_NONE without an echo.   */
    int8_t             rssi_rev;   /**< What this nRF heard back;
                                    *   FCK_DRV_NRF_RSSI_NONE when nothing came. */
} fck_drv_nrf_ping_t;

/** @brief Read a #PING24. Requires seq and ok, and both RSSIs when ok. */
bool fck_drv_nrf_parse_ping(const fck_drv_link_message_t *m, fck_drv_nrf_ping_t *out);

/** @brief #PINGDONE24: the whole of `rf ping`. */
typedef struct {
    uint16_t mhz;
    uint16_t sent;
    uint16_t ok;
    int16_t  fwd_mean_x10;   /**< The mean rssi_fwd, in tenths of a dBm:
                              *   -415 for -41.5. 0 when ok is 0.           */
    int16_t  rev_mean_x10;
    int8_t   fwd_min, fwd_max, rev_min, rev_max;  /**< 0 when ok is 0.      */
    int8_t   dbm;            /**< The level it sent at.                     */
    uint16_t timeout, crcbad, mismatch, txfail;   /**< Why the rest failed. */
    uint8_t  peer;           /**< The node id the echoes carried, the last's. */
    int8_t   peer_dbm;       /**< The level the responder said it sent at.  */
    uint32_t txslow;
} fck_drv_nrf_ping_done_t;

/** @brief Read a #PINGDONE24. Requires ch, sent, ok and both means. */
bool fck_drv_nrf_parse_ping_done(const fck_drv_link_message_t *m, fck_drv_nrf_ping_done_t *out);

/** @brief #SNIFFSTART24: what `rf sniff` listens with. */
typedef struct {
    uint16_t        mhz;
    fck_drv_nrf_sniff_config_t  phy;
    fck_drv_nrf_sniff_address_t addr;
    int8_t          rssi_min;
    uint32_t        ms;
} fck_drv_nrf_sniff_start_t;

/** @brief Read a #SNIFFSTART24. Requires every key. */
bool fck_drv_nrf_parse_sniff_start(const fck_drv_link_message_t *m, fck_drv_nrf_sniff_start_t *out);

/** @brief #SNIFF24 or #SNIFFTOP24: one frame `rf sniff` heard. */
typedef struct {
    bool     top;                        /**< A #SNIFFTOP24, from the end.   */
    uint8_t  i;                          /**< Its place in the firmware's table:
                                          *   the same frame has the same i in
                                          *   both, until the table is
                                          *   sorted for #SNIFFTOP24.        */
    uint16_t cnt;                        /**< Times heard: 1 for a #SNIFF24. */
    int8_t   rssi;                       /**< dBm: as heard for a #SNIFF24,
                                          *   the strongest for #SNIFFTOP24. */
    uint8_t  len;
    uint8_t  data[FCK_DRV_NRF_FRAME_MAX];
} fck_drv_nrf_frame_t;

/** @brief Read a #SNIFF24 or a #SNIFFTOP24. Requires i, rssi and data, cnt
 *         for a #SNIFFTOP24, and a len= that matches data when there is one. */
bool fck_drv_nrf_parse_frame(const fck_drv_link_message_t *m, fck_drv_nrf_frame_t *out);

/** @brief The different frames of one `rf sniff`, as fck_drv_nrf_frames_add()
 *         keeps them. Zeroed before the sniff. */
typedef struct {
    fck_drv_nrf_frame_t f[FCK_DRV_NRF_FRAMES_MAX];
    uint8_t         n;
    bool            top;   /**< Taken from the #SNIFFTOP24s: with their counts,
                            *   the most repeated first.                    */
} fck_drv_nrf_frames_t;

/**
 * @brief Keep one frame of `rf sniff`'s answer.
 *
 * A #SNIFF24 is kept as it arrives, once for each place in the firmware's
 * table. The first #SNIFFTOP24 starts the list again, since the table at
 * the end carries how often each frame came, the most repeated first; a
 * #SNIFF24 after it is left out. Nothing beyond FCK_DRV_NRF_FRAMES_MAX.
 *
 * @return true when kept.
 */
bool fck_drv_nrf_frames_add(fck_drv_nrf_frames_t *t, const fck_drv_nrf_frame_t *fr);

/** @brief #SNIFFDONE24: the whole of `rf sniff`. */
typedef struct {
    uint16_t mhz;
    uint32_t rx;      /**< Frames heard.                                    */
    uint32_t gated;   /**< Of them, weaker than rssi_min, dropped.          */
    uint8_t  uniq;    /**< Different frames kept, up to FCK_DRV_NRF_FRAMES_MAX. */
    uint32_t ms;
} fck_drv_nrf_sniff_done_t;

/** @brief Read a #SNIFFDONE24. Requires every key. */
bool fck_drv_nrf_parse_sniff_done(const fck_drv_link_message_t *m, fck_drv_nrf_sniff_done_t *out);

/** @brief Read a #SCFG24. Requires rate, crc and len. */
bool fck_drv_nrf_parse_sniff_config(const fck_drv_link_message_t *m, fck_drv_nrf_sniff_config_t *out);

/** @brief Read a #SADDR24. Requires balen, base and prefix. */
bool fck_drv_nrf_parse_sniff_address(const fck_drv_link_message_t *m, fck_drv_nrf_sniff_address_t *out);

#ifdef __cplusplus
}
#endif

#endif /* FCK_DRV_NRF_H_ */
