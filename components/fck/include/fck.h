/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file fck.h
 * @brief The F*CK module (KDFCK001) for a Kode Dot app, in tasks: the standard layer.
 *
 *     fck_begin();                                 // takes J3, looks for the module
 *     if (fck_wait_present(3000)) {                // plugged in and set up
 *         int32_t rc;
 *         fck_command(FCK_C5, "info", "info", on_line, NULL, 3000, &rc);
 *     }
 *
 * Written against kode-sdk v0.1.5 (API v4). Two layers reach the same owner, as kode-sdk's
 * kode.h and kode_sys.h do:
 *
 *   | | fck.h (this header)                  | fck_sys.h                                |
 *   |-|--------------------------------------|------------------------------------------|
 *   | For      | app_main() and tasks of the app's own | an Active Object of the app's own |
 *   | A call   | WAITS for the owner's answer          | NEVER waits: asks, and is answered |
 *   | Returns  | bool, and fck_error() says why        | kode_status_t; the answer has rc and why |
 *   | In an AO | refused: false, and fck_error() says  | everything is allowed              |
 *   |          | "fck_foo() was called from an Active  |                                    |
 *   |          | Object (<name>); fck.h calls block -  |                                    |
 *   |          | call fck_sys_* instead"               |                                    |
 *   | Events   | named: fck_event_wait() and           | signals posted to the subscribed   |
 *   |          | fck_event_add_listener()              | Active Objects (FCK_*_SIG)         |
 *
 * One Active Object owns the module (AO_Fck, in fck_aos); every call here asks it and waits for
 * its answer, so these BLOCK - call them from a task of the app's own, never from an Active
 * Object (fck_sys.h is for those). The module may be plugged in or pulled out at any time:
 * fck_is_present() says whether it is, and a call made while it is not answers false.
 *
 * A call that answers false says why in fck_error(), a text in English for a person, and a call
 * that goes well clears it. The reasons come in classes, the same ones fck_sys.h's answers carry
 * as fck_why_t, in the order a call meets them:
 *
 *   | Cause                     | When                                                   |
 *   |---------------------------|--------------------------------------------------------|
 *   | Not started               | before fck_begin(), after fck_end(), or while one runs |
 *   | Called from an AO's task  | a call that BLOCKS, from an Active Object's step       |
 *   | Leaving                   | the way home, sleep or a restart has begun             |
 *   | No module                 | not on J3, powered off, or just pulled out             |
 *   | Chip held                 | fck_power(chip, false) or its reset is in force        |
 *   | Chip not ready            | it has not said what it runs; the GNSS holds the bus   |
 *   | Bad argument              | NULL, 0 or a value out of the range the call states    |
 *   | Beyond the limits         | a band, power, carrier length or hour's air time the   |
 *   |                           | module does not allow; a rolling code; a gate's refusal|
 *   | Firmware too old          | the firmware lacks the command: caps= does not list it |
 *   | Wrong order               | a read before its start, a stop with nothing running   |
 *   | Cancelled                 | fck_cancel(); a send cut by the way home, a rest or    |
 *   |                           | fck_end()                                              |
 *   | Busy                      | a chip another task keeps (7.5 s, 10 s for a flash), or|
 *   |                           | the owner's queue or kode-sdk's pools had no room      |
 *   | Timeout                   | a wait that ran out: 7.5 s for a chip to say what it   |
 *   |                           | runs, the call's own @p ms                             |
 *   | I/O                       | the module or a bus failing under the call, or a firmware|
 *   |                           | saying it did not do what it was asked (a #DONE rc of  |
 *   |                           | 1, or one this SDK does not know)                      |
 *
 * Nothing waits without a bound, and no call asserts.
 *
 * The module's chips run firmwares that answer lines of text with tagged lines
 * and a closing `#DONE <verb> rc=<n>` (firmware/README.md).
 * fck_command() is that conversation, one command at a time per chip: a free GATEWAY to
 * the C5's and the nRF's firmwares for what the typed calls do not cover. The SDK keeps the
 * radio limits on it (see fck_command()) and nothing else. The typed calls do the same in an
 * app's terms: fck_wifi_*() and fck_ble_scan() for the C5's radio, fck_gnss_*() for the GNSS
 * receiver behind it, and fck_radio_*() for the nRF's 2.4 GHz radio. fck_firmware() says what a
 * chip runs, and fck_flash() writes a chip's firmware. The CC1101 runs no firmware:
 * fck_subghz_*() listen with it from the P4.
 *
 * Nothing here transmits or records by itself: no call emits unless an app asks it to, and the
 * module coming back or the board waking restores only what listens (the receiver, the
 * sub-GHz listening). The console's `fck_arm` stays the key to emit from the console.
 */
#ifndef FCK_H_
#define FCK_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "fck_bsp.h"    /* the module's flash maps: where fck_flash() writes */

#ifdef __cplusplus
extern "C" {
#endif

#define FCK_VERSION_MAJOR 0U /**< Major version of the F*CK SDK: the first number of its release, v0.1.x. */
#define FCK_VERSION_MINOR 1U /**< Minor version of the F*CK SDK: the second number of its release. */

/** @brief The module's two chips that run firmware. */
typedef enum {
    FCK_C5  = 0,    /**< ESP32-C5: Wi-Fi 2.4/5 GHz, BLE, the GNSS relay. */
    FCK_NRF = 1,    /**< nRF54L15: 2.4 GHz.                              */
} fck_chip_t;

/** @brief One tagged line of a firmware's answer, valid during the callback. */
typedef struct {
    const char *tag;    /**< Without the '#': "AP", "C5INFO".            */
    const char *body;   /**< After the tag and one space; "" for none.   */
} fck_line_t;

/** @brief Called with each tagged line of a command's answer. Runs in the
 *         owner's task: short, no blocking, no fck_* calls. */
typedef void (*fck_line_fn)(void *ctx, const fck_line_t *line);

/** @brief What is known about the module without asking its chips. */
typedef struct {
    bool     present;
    uint32_t generation;     /**< Changes each time a module is set up: one
                              *   plugged in, or one that lost power.       */
    bool     c5_on;          /**< Running, not held in reset (fck_power()). */
    bool     nrf_on;
    bool     cc1101_ok;      /**< A CC1101 answered, and sleeps.            */
    uint8_t  cc1101_part;
    uint8_t  cc1101_version;
    bool     gnss_running;   /**< The receiver is out of reset.            */
    bool     listening;      /**< A sub-GHz listening is in force and the module is there. */
    uint8_t  j3_supply;      /**< FCK_J3_*: what J3's +3V3_EXT switch came to.            */
    uint32_t events_dropped; /**< Events the listeners' queue had no room for (fck_event_wait(), listeners). */
    uint32_t news_dropped;   /**< News fck_sys.h could not post to a subscriber.          */
    uint32_t answers_dropped; /**< Answers fck_sys.h could not post: the asker's queue stayed full. */
} fck_status_t;

/** @brief fck_status_t.j3_supply: J3's supply, as the SDK asked for it and kode-sdk answered. */
#define FCK_J3_UNKNOWN  0U  /**< Not asked yet, or fck_end() was called.                       */
#define FCK_J3_ASKED    1U  /**< Asked on; kode-sdk has not answered yet.                      */
#define FCK_J3_ON       2U  /**< On.                                                           */
#define FCK_J3_NO_LINK  3U  /**< The Dot's C5 link did not come up: the wish stays, and the next link carries it out. */
#define FCK_J3_FAILED   4U  /**< The C5 refused: the wish was dropped.                         */
#define FCK_J3_ALWAYS   5U  /**< No radio in the image (CONFIG_KODEOS_RADIO=n): J3 is always powered, and fck_end() cannot switch it off. */
#define FCK_J3_OFF      6U  /**< Switched off by fck_end().                                    */

/* ---- What happens -------------------------------------------------------------
 *
 * Events are named, as kode.h's are, in lower case, a noun and a participle:
 *
 *   | Name             | value                          | text      | When                                   |
 *   |------------------|--------------------------------|-----------|----------------------------------------|
 *   | module_found     | 1 (held upright)               | ""        | An F*CK was found and set up.          |
 *   | module_lost      | 0                              | ""        | It left J3, or lost power (it is set up|
 *   |                  |                                |           | again, and told as module_found, when  |
 *   |                  |                                |           | it is back).                           |
 *   | wrong_module     | an FCK_WRONG_MODULE_*          | ""        | Not an F*CK on J3, ruled out after two |
 *   |                  |                                |           | looks; told once per module.           |
 *   | setting_lost     | the FCK_SETTING_* in force     | ""        | Right after module_lost: what the app  |
 *   |                  |                                |           | had set went with the module. Not told |
 *   |                  |                                |           | when none.                             |
 *   | setting_restored | the FCK_SETTING_* set again    | ""        | The SDK set again what does not emit.  |
 *   |                  |                                |           | The listening is told right after      |
 *   |                  |                                |           | module_found, the receiver when the C5 |
 *   |                  |                                |           | relays it again (about a second later, |
 *   |                  |                                |           | once the C5 has said what it runs):    |
 *   |                  |                                |           | two events, then.                      |
 *   | packet_received  | the capture's count            | ""        | The sub-GHz listening heard a remote;  |
 *   |                  |                                |           | read it with fck_subghz_listen_read(). |
 *   | send_cancelled   | 1 fck_cancel(), 2 leaving      | "cancel"  | A sub-GHz send was cut short: by       |
 *   |                  |                                | "leaving" | fck_cancel(), or by the way home, a    |
 *   |                  |                                |           | rest or fck_end().                     |
 *
 * Listeners run on ONE task of the SDK (fck_events), one at a time and in order, never on the
 * owner's: any call of fck.h is allowed in a listener but fck_event_wait() and fck_end(). The owner hands the
 * event over without blocking, through a queue of FCK_EVENT_QUEUE: a full one loses the event
 * and counts it in fck_status().events_dropped. A listener that takes longer than 10 ms is
 * logged (one in a hundred) with its time. The strings of an event are valid until the
 * listener returns, or until the calling task's next fck_event_wait(). Only an event that
 * arrives after fck_event_wait() began counts.
 */

/** @brief The most listeners at once, over all the names. */
#define FCK_LISTENERS_MAX    8U
/** @brief The longest name of an event, its terminator included. */
#define FCK_EVENT_NAME_MAX   32U
/** @brief Events the owner may hand to the listeners' task before it loses one. */
#define FCK_EVENT_QUEUE      16U

/** @brief One event: see the table above. */
typedef struct {
    const char *name;     /**< "module_found", ...                                  */
    int32_t     value;    /**< What the table says.                                 */
    const char *text;     /**< What the table says; "" for none. Never NULL.        */
} fck_event_t;

/** @brief A listener. Runs on the SDK's events task: see above. */
typedef void (*fck_listener_t)(const fck_event_t *e);

/**
 * @brief FCK_SETTING_* in setting_lost's and setting_restored's value: what the module forgets
 *        when it leaves J3 or loses power.
 *
 * A setting lost and not restored (lost minus restored) is the app's to set again. The SDK sets
 * again only what cannot emit: the GNSS receiver (it is started again, and has no fix until it
 * makes one) and the sub-GHz listening (fck_subghz_listen_start()). It never switches a chip off
 * again, the C5's echo or anything that transmits, and a receiver that does not start is not
 * retried: ask for it with fck_gnss_begin(). fck_gnss_end(), fck_subghz_listen_stop() and
 * fck_end() forget what was to be set again.
 */
#define FCK_SETTING_GNSS       1UL  /**< fck_gnss_begin(): the receiver was running.            */
#define FCK_SETTING_LISTEN     2UL  /**< fck_subghz_listen_start(): the listening.              */
#define FCK_SETTING_C5_POWER   4UL  /**< fck_power(FCK_C5, false) had been called: it is on again.  */
#define FCK_SETTING_NRF_POWER  8UL  /**< fck_power(FCK_NRF, false) had been called: it is on again. */

/** @brief wrong_module's value: what is on J3 instead. */
#define FCK_WRONG_MODULE_HOLDS_PAIR  1UL  /**< Something holds the I2C pair, and nothing answers at the expander's address. */
#define FCK_WRONG_MODULE_OTHER_CHIP  2UL  /**< A chip answers at the expander's address but is not the expander.             */

/** @brief send_cancelled's value: what cut the send. */
#define FCK_CANCEL_CALL     1L  /**< fck_cancel().                                  */
#define FCK_CANCEL_LEAVING  2L  /**< The way home, a rest or fck_end().             */

/**
 * @brief Returns at once. Call @p fn every time @p event happens, from now on.
 * @param event One of the names above, in lower case ("module_found").
 * @param fn    The listener; the same @p fn on the same @p event once.
 * @return true; false when @p event is not a name above or is too long, @p fn is NULL, it is
 *         already listening to it, or FCK_LISTENERS_MAX listeners are taken. Allowed before
 *         fck_begin() too: the listeners stay across fck_end() and fck_begin(). From an Active
 *         Object: false (fck_sys_subscribe() is for those).
 */
bool fck_event_add_listener(const char *event, fck_listener_t fn);

/**
 * @brief Blocks (up to 2 s). Stop calling @p fn for @p event; when it returns true, @p fn is
 *        not running and is not called again. From a listener itself it returns at once, and
 *        the listener finishes.
 * @return true; false when @p fn was not listening to @p event, or from an Active Object;
 *         false ("the listener is still running" in fck_error()) when @p fn was still running
 *         after the 2 s: it is not called again, but what it uses must stay until it returns.
 */
bool fck_event_remove_listener(const char *event, fck_listener_t fn);

/**
 * @brief Blocks. Wait for the next @p event: only one that arrives after the wait begins counts.
 * @param event One of the names above.
 * @param ms    How long to wait; 0 does not wait and is false.
 * @param out   Out: the event; may be NULL.
 * @return true when it came; false after @p ms, for a name that is none, from an Active Object
 *         or from a listener (it would wait for itself), before fck_begin(), or when
 *         fck_cancel() ended it.
 */
bool fck_event_wait(const char *event, uint32_t ms, fck_event_t *out);

/**
 * @brief Blocks. Take J3 for the module and start looking for it. Calls kode_init() itself
 *        (idempotent), so an app may call this first. Idempotent. Returns as soon as the owner
 *        runs: it does not wait for the module (fck_wait_present() does).
 *
 * J3's supply is asked on, and what came of it is in fck_status().j3_supply; with no radio in
 * the image it is always on. Nothing is promised of the state J3 starts in.
 * @return true; false when the console took J3 by hand (`exp`), something else in the image
 *         borrowed some of its positions, another task is starting it for more than 5 s,
 *         the owner's task would not start, kode-sdk has no controller left, or this was
 *         called from an Active Object (fck_sys_begin() is for those). A start that fails
 *         leaves everything as it was: the loan given back.
 */
bool fck_begin(void);

/**
 * @brief Blocks. Put the module down and let go of it, so that fck_begin() can start
 *        again: what is on the air is cut, the GNSS receiver goes to reset, the CC1101 to
 *        sleep, both chips are held, J3's supply is asked off and the loan is given back.
 *        Blocks until that is done.
 *
 * What was asked of the module and not finished is answered false (a send cut is "cancelled").
 * Nothing is looked for afterwards, and every call here but fck_begin() answers false. The Active
 * Object that owns the module stays (QP/C cannot give it back) and idles. Never from an Active
 * Object or a listener.
 *
 * @return true, also when fck_begin() was not called; false from the owner's own task, from an
 *         Active Object, from a listener, while fck_begin() is in progress, or when kode-sdk's
 *         pools or the owner's queue had no room (nothing was done; try again).
 */
bool fck_end(void);

/**
 * @brief Returns at once. Why the last call of the calling task that answered false failed.
 *
 * A text for a person, as kode_error() is for kode-sdk's: the exact reason ("hz is not a
 * frequency the SDK transmits on", "the chips stayed busy: another task holds the module").
 * Every false of an fck_*() call sets it; a call that goes well clears it. It is kept for eight
 * tasks at a time (a ninth takes the place of the one that failed longest ago) and is "" for a task
 * that has not failed yet. Valid until the task's next fck.h call: copy it to keep it. Never
 * blocks; safe from any task, an Active Object included.
 *
 * @return The text; "" when the calling task has not failed. Never NULL.
 */
const char *fck_error(void);

/**
 * @brief Returns at once. Cancel what the app is waiting for in the F*CK: the long listenings
 *        (fck_radio_spectrum(), fck_radio_sniff(), fck_radio_ping(),
 *        fck_subghz_capture(), fck_subghz_find(), fck_subghz_listen_read(),
 *        fck_subghz_packet_receive(), fck_subghz_spectrum()), a send in the
 *        air, a fix being waited for (fck_gnss_read()), a wait for the module
 *        (fck_wait_present()) or for an event (fck_event_wait()), a chip another task
 *        holds, and a command on its way.
 *
 * It is a command, not a mode: it ends the calls that BEGAN before it, in
 * whatever task they run, and a call begun after it is not touched. Each of
 * those calls answers false, and fck_error() says it was cancelled; what
 * they would have written is left as it was. A send that was cut is told as the event
 * send_cancelled. The time to stop is a step of the call, never its whole length: a wait is
 * cut within 20 ms; a listening or a send of the CC1101 within a few milliseconds (a send puts
 * the chip in IDLE first); fck_radio_spectrum(), fck_radio_sniff() and fck_radio_ping()
 * are run in slices of 1 s at most, and stop at the next boundary. A command
 * given to the chip with fck_command() is let go of by the caller; the chip
 * finishes it, within the time it was given, and nothing else is sent to it
 * until it has. Not cancellable: fck_flash(), which would leave a chip with
 * half an image.
 *
 * Safe from any task, a listener and an Active Object. Never needed to leave: the way home, a
 * rest and fck_end() cut a send on their own.
 *
 * @return true; false before fck_begin() or after fck_end().
 */
bool fck_cancel(void);

/**
 * @brief Returns at once. Whether an F*CK is plugged in and set up.
 * @return true while the module is found and set up; false before fck_begin(), after fck_end()
 *         and while it is not there. Safe from an Active Object.
 */
bool fck_is_present(void);

/**
 * @brief Blocks. Wait for an F*CK to be found.
 * @return true once one is, at once when it already is; false after @p ms, or before
 *         fck_begin().
 *
 * fck_cancel() ends it: false, "cancelled by fck_cancel()" in fck_error().
 */
bool fck_wait_present(uint32_t ms);

/**
 * @brief Returns at once. What is known about the module, copied.
 * @param out Out: what is known; it is not touched when this answers false.
 * @return true; false when @p out is NULL.
 */
bool fck_status(fck_status_t *out);

/**
 * @brief Returns at once. The J3 position of one of the module's lines, for kode_sys_exp_*().
 *        0 while the module is not there, or for a position J3 does
 *        not have. (The module is held upright: a position is itself.)
 * @param pos An FCK_BSP_POSITION_* name.
 * @return @p pos while the module is there and J3 has the position (1 to 14); 0 otherwise.
 */
uint8_t fck_header_position(uint8_t pos);

/**
 * @brief Blocks. Send one command to a chip's firmware and wait for its #DONE.
 *        It TRANSMITS when the line does: the C5's `scan`, the nRF's `rf ping`
 *        and `rf echo on`, and whatever another firmware's lines do.
 *
 * What each chip runs decides how a line reaches it. A chip says what it
 * runs in the banner it prints when it starts; the SDK keeps it, asks for
 * it with `info` when it has none, and waits up to 7.5 s for a chip still
 * starting.
 *
 *  - The nRF's official firmware: only its own words, as fck_drv_nrf_command() takes them
 *    (`info` and `rf ...`, fck_drv_nrf.h), and a line that asks the radio for
 *    more than the module's EU limits allow is refused before it is
 *    written: `rf cw`, `rf power` above 6 dBm.
 *  - The C5's official firmware, any version: only its own words, as fck_drv_c5_command()
 *    takes them (`info`, `health`, `scan`, `chan`, `ble scan`, `gnss`, `wp` and
 *    `gtx`, fck_drv_c5.h; `frames` is fck_wifi_frames()'s, and refused here). `scan`, the one that makes the C5 transmit, is sent: the
 *    firmware keeps the radio limits itself, and refuses it (rc 1) while one
 *    is not in force - Wi-Fi still coming up, a limit its driver refused.
 *    `ble raw` is refused, and so is a `gtx` that would move the GNSS
 *    receiver's baud rate.
 *
 * A version decides none of this: the firmware's public line starts again
 * at 0.x, and what one can be asked is what its caps= lists
 * (fck_firmware_t.caps). TEMPORARY: pre-production firmware (2.x), which prints
 * no caps=, is still told by its version - nothing that transmits sent to
 * one before 2.0, which keeps no limits itself; 1.6's `sniff`, `pcap`,
 * `deauth` and `probe` refused - until the production modules ship.
 *
 *  - Another firmware on either chip, any whose banner names it otherwise
 *    than the official firmware, to the letter: the line as written, with the caller's
 *    verb, checked for nothing. What it does, the radio included, is its
 *    programmer's.
 *  - A chip that has not said what it runs: nothing.
 *
 * NOTE: this is the free gateway to the chips, for what the typed calls do not cover. The SDK
 * keeps the radio limits (above) and nothing else: it does not know what a line did. A
 * `gnss on`, an `rf echo on`, a `wp` or a `gtx` given here is not a setting the SDK keeps
 * (no setting_lost event for it, none restored): use fck_gnss_begin(), fck_radio_echo_start() and the others for what the SDK should
 * know, and put back yourself what a line here changed. Another firmware's lines go out
 * as written, and what they do is its programmer's.
 *
 * @param chip    Which chip.
 * @param line    The command, without the line ending, under 256 bytes; for
 *                the nRF's firmware, at most 127.
 * @param verb    The word its #DONE carries. For the official firmware, NULL: the SDK
 *                works it out (the first word for the C5, "ble" for "ble
 *                scan 5"; "rf_spec" for the nRF's "rf spec 0 83"), and one
 *                given must be that. For another firmware, required.
 * @param on_line Each tagged line of the answer; may be NULL.
 * @param ctx     Handed back to @p on_line.
 * @param ms      How long the firmware is given. For the official firmware, 0 gives it what
 *                the command takes by the firmware's own numbers.
 * @param rc      The #DONE's rc - 0 done, 1 failed, 2 not understood - or -1
 *                if it carried none; may be NULL.
 * @return true when the #DONE came (look at @p rc); false (a timeout) when it
 *         did not in time, or the chip did not say what it runs in 7.5 s, or
 *         another task kept it busy for the 7.5 s; false with no module,
 *         the chip held in reset, a bad argument, or a line refused as
 *         above; false (an I/O failure) when the module left or lost
 *         power on the way, or the chip restarted in the middle of it.
 */
bool fck_command(fck_chip_t chip, const char *line, const char *verb,
                          fck_line_fn on_line, void *ctx, uint32_t ms, int32_t *rc);

/**
 * @brief Blocks. Reset a chip and let it boot its own firmware again: the way out of
 *        a firmware that stopped answering. The nRF talks again about 6.3 s
 *        later.
 * @return true; false with no module, or for the nRF while the GNSS
 *         receiver holds the module's bus; false (an I/O failure) when the expander
 *         did not take it.
 */
bool fck_reset(fck_chip_t chip);

/**
 * @brief Blocks. Hold a chip in reset, where it draws least, or let it run again.
 * @return As fck_reset().
 */
bool fck_power(fck_chip_t chip, bool on);

/* ---- What a chip runs, and writing it ---------------------------------------
 *
 * Every chip of the module is open: anyone can write their own firmware into
 * either, and the Kode can write it again (the C5 through its ROM loader, the
 * nRF through MCUboot), so a firmware that does not start is never the end of
 * the module. Only fuses close a chip for good - Secure Boot, flash
 * encryption or the ROM loader off on the C5, which c5/'s template refuses to
 * build; the UICR on the nRF - and nothing this SDK sends to a firmware burns
 * them: fck_command() gives each firmware its own words only. */

/** @brief What a chip said it runs: the banner it prints when it starts and
 *         answers `info` with, in an app's terms. */
typedef struct {
    bool    known;      /**< The chip has said. In a report, false when it
     *                       had not said (yet): nothing else is set then.   */
    bool    compatible; /**< It speaks the SDK's words, so the typed calls
     *                       (fck_wifi_scan(), fck_radio_spectrum()...) can
     *                       be sent to it: its banner carries a caps=,
     *                       whatever its name. Each call is asked only if
     *                       caps lists its word. A firmware of your own is
     *                       compatible by printing its caps=. (TEMPORARY:
     *                       the first modules' firmware, 2.x, prints none
     *                       and is compatible by its name.)                  */
    bool    official;   /**< Its name is the one the module ships with:
     *                       fck-c5 or fck-nrf (TEMPORARY: or c5_fck_agent
     *                       or nrf_fck_agent, their names before 2.1). Only
     *                       a name, which any firmware can print: it tells
     *                       fck_command() to hold a line to that firmware's
     *                       own words, and nothing else.                     */
    char    fw[24];     /**< Its name, as it printed it; "" when the banner
     *                       could not be read whole.                         */
    bool    has_version;/**< It printed a version, and the three numbers
     *                       below are it. When false they are 0 - and so is
     *                       the first of "0.1.0": a 0 there is a number,
     *                       never "no version".                              */
    uint8_t ver_major;  /**< 0 for "0.1.0". Which build runs, and nothing
     *                       about what it can do: see caps.                  */
    uint8_t ver_minor;
    uint8_t ver_patch;
    uint8_t id[8];      /**< The C5's base MAC (6 bytes); the nRF's DEVICEID
     *                       (8 bytes), most significant first.               */
    uint8_t id_len;     /**< 6, 8, or 0 when it printed none.                */
    char    caps[96];   /**< What it says it does, as it printed it: words
     *                       with commas between them, one per command or
     *                       ability ("scan,chan,ble,bleadv,frames,gnss,wp,
     *                       gtx,health" on the C5; "spec,off,power,echo,ping,sniff,scfg,
     *                       saddr,eu,done,wdt,echoidle,smp" on the nRF).
     *                       What the SDK asks a firmware is decided by
     *                       these words, never by its version; a word is
     *                       never taken to mean something else, so a new
     *                       ability is a new word. "" when it printed none:
     *                       another firmware, or (TEMPORARY) a
     *                       pre-production one (2.x).                        */
} fck_firmware_t;

/**
 * @brief Blocks. What a chip runs. Asked for with `info`, and waited for up to 7.5 s,
 *        if it has not said since it last started.
 *
 * @param chip Which chip.
 * @param out  Out: filled on true only, with known set.
 * @return true; false (a timeout) when no banner came in time; false with
 *         no module, the chip held in reset, or a bad argument;
 *         false (an I/O failure) when the module left on the way.
 */
bool fck_firmware(fck_chip_t chip, fck_firmware_t *out);

/** @brief One piece of firmware and where it goes. */
typedef struct {
    uint32_t       addr;         /**< The C5: where it goes in the chip's flash,
     *                              a multiple of 4096 (FCK_BSP_C5_BOOTLOADER,
     *                              FCK_BSP_C5_PARTITIONS and FCK_BSP_C5_APP are
     *                              where an ESP-IDF build puts its three).
     *                              The nRF: FCK_BSP_NRF_SLOT0, its one slot,
     *                              written through MCUboot's recovery;
     *                              nothing else.                             */
    const uint8_t *data;
    uint32_t       len;          /**< The bytes at @c data.                  */
    uint32_t       inflated_len; /**< The C5 only. 0: @c data is the image. Else
     *                              @c data is a zlib stream - what esptool
     *                              compresses to - and this is
     *                              what it inflates to, a multiple of 4.     */
    const char    *md5;          /**< The C5 only. 32 hex digits, either case,
     *                              of the image once inflated. Required for a
     *                              deflated one, whose flash can be checked
     *                              against nothing else; optional for a raw
     *                              one, which is checked against it before the
     *                              C5 is touched.                            */
} fck_image_t;

/** @brief Where a write stopped; the same for both chips. */
typedef enum {
    FCK_FLASH_CHECK = 0,  /**< The images and the chip were looked at: refused
     *                         here, nothing was written.                     */
    FCK_FLASH_CONNECT,    /**< Reaching the C5's loader or the nRF's
     *                         MCUboot in its recovery.                       */
    FCK_FLASH_WRITE,      /**< Erasing, writing and checking.                 */
    FCK_FLASH_BOOT,       /**< The nRF's new image starting: it was written
     *                         and said nothing. MCUboot will not boot it and
     *                         stays in recovery, or it hangs, or it never
     *                         prints. The C5 has no such step.               */
    FCK_FLASH_DONE,       /**< It did not stop.                               */
} fck_flash_step_t;

/** @brief What a write did. Filled whether it worked or not, once the
 *         arguments were looked at. */
typedef struct {
    fck_flash_step_t step;     /**< Where it stopped; FCK_FLASH_DONE if not.   */
    int              failed;   /**< The C5: the image, by its place in the
     *                              array, that failed; -1 for none. The nRF:
     *                              -1.                                       */
    int32_t          rc;       /**< Why, in the chip's words: the C5's loader
     *                              error (esp_loader_error_t), the nRF's SMP
     *                              rc. -1 when there is none.                */
    bool             same;     /**< The nRF held that image already, and
     *                              nothing was written.                       */
    uint32_t         written;  /**< Bytes of image the chip took: the C5, the
     *                              images written and checked, by what they
     *                              hold once inflated; the nRF, what the
     *                              server took.                               */
    uint32_t         ms;       /**< How long the write took, the chip's start
     *                              afterwards excluded.                        */
    fck_firmware_t      before;   /**< What the chip said it ran before; known
     *                              false when it had not said.                 */
    fck_firmware_t      after;    /**< ... and after, when the write worked and
     *                              it said by itself in time: an image that
     *                              prints no banner leaves known false, and
     *                              the write still worked.                     */
    char             note[112];/**< The worst the writer saw, in its own words
     *                              (a block sent again, an MD5 that did not
     *                              match); for the nRF, the version the slot
     *                              held before, why the image was refused,
     *                              that it said nothing once started, or a
     *                              warning that it never names where its
     *                              code starts in the slot (built for
     *                              another map? fck_rescue_probe() says).
     *                              "" if nothing.                            */
} fck_flash_report_t;

/**
 * @brief Blocks. Write firmware into a chip, check it, and let the chip start it.
 *
 * The module is the call's for as long as it runs: no command reaches either
 * chip meanwhile, and the call first waits up to 10 s for both to be free.
 * Writing transmits nothing. A way home - the app restarting - does not wait
 * for it: after 250 ms the C5 is held in reset and the P4 restarts. A C5 cut
 * off half written does not start its firmware, and is written again.
 *
 *  - The C5, through its ROM loader, whatever it runs now: BOOT is held low
 *    through a reset, the images are erased, written at 460800 baud and each
 *    checked against the MD5 the flash gives, and the C5 is reset into its
 *    flash, whether the write worked or not. A C5 with secure boot or flash
 *    encryption on, a chip that is not an ESP32-C5 and an image past the end
 *    of its flash are refused before anything is erased. The firmware 2.0 EU,
 *    its three images raw (917552 bytes), took 27.1 s on a KDFCK001 rev 1.0.
 *    @p img holds 1 to 8 images, none sharing a byte of the flash.
 *  - The nRF, through MCUboot's serial recovery and nothing else, so no
 *    image runs that MCUboot did not check. @p n is 1, at FCK_BSP_NRF_SLOT0,
 *    the module's one slot: the nRF is reset, MCUboot takes the image
 *    straight into the slot in the 3 s after it, and it needs the expander,
 *    so not while the GNSS receiver holds the bus. The module's MCUboot
 *    takes an image by its SHA-256, with no key and no signature: what a
 *    build of firmware/fck-nrf gives, and anyone's own built the same way.
 *    Refused before the nRF is touched: an image whose stack pointer is not
 *    in the nRF's RAM, whose vectors point outside it once in the slot, or
 *    whose FCKMAP tag (fck-nrf carries one) names another address than the
 *    slot's - built for another map. Refused once MCUboot has said which it
 *    is, with nothing written and what it had booted again: an image that
 *    MCUboot would not boot. One with no tag that may be built for another
 *    map is written with a warning in the note. Then the image must say
 *    something - a line, or Zephyr's boot banner - within 9 s of its start:
 *    one that stays silent fails the call at FCK_FLASH_BOOT, written as it
 *    is, and fck_rescue_probe() says what the nRF carries.
 *    TEMPORARY, pre-production modules: a first module's MCUboot, which
 *    answers no echo, is still written, and only with an image signed for
 *    it (ED25519 and SHA-512, Nordic's public development key).
 *
 * The P4's UART interrupt must be in IRAM (CONFIG_UART_ISR_IN_IRAM=y) if the
 * app writes the P4's own flash - kode_fs, NVS - while this runs: a byte lost
 * in the loader's conversation fails the write.
 *
 * @param chip Which chip.
 * @param img  The images.
 * @param n    How many.
 * @param rep  Out: what happened; may be NULL.
 * @return true when every image was written and checked - the C5 - or the
 *         new image was written and said something - the nRF; the report
 *         says what the chip said afterwards. false for an argument the chip cannot take,
 *         a chip that refuses (see above) or no module: nothing was written.
 *         false (a timeout) when either chip was kept busy for 10 s by another
 *         task (nothing was written either), or the loader or the server
 *         never answered. false (an I/O failure) when it
 *         answered and a command or a check failed (the report says where).
 *         false for the nRF when a step was refused, or the new image
 *         said nothing once started. false (no memory) when the C5's block buffer could
 *         not be had.
 */
bool fck_flash(fck_chip_t chip, const fck_image_t *img, size_t n,
                        fck_flash_report_t *rep);

/** @brief What fck_rescue_probe() found a chip to carry. */
typedef enum {
    FCK_RESCUE_OFFICIAL = 0,  /**< The firmware the module ships with runs.          */
    FCK_RESCUE_FIRMWARE,   /**< Another image runs: it printed a line (the
     *                          nRF: or its boot banner, or answers SMP).      */
    FCK_RESCUE_QUIET,      /**< An image started and says nothing. The nRF:
     *                          MCUboot let it go, and it neither prints nor
     *                          speaks SMP, or does not start - built for
     *                          another MCUboot's map, as the firmware built for
     *                          FCK_BSP_NRF_SLOT0 hangs on the one-slot MCUboot
     *                          at 0x10000. The C5: its ROM started and
     *                          nothing followed.                              */
    FCK_RESCUE_BOOTLOADER, /**< Only the boot code runs. The nRF: only MCUboot
     *                          answers, slot 0 holding nothing it boots or an
     *                          image that restarts the nRF before it says
     *                          anything. The C5: its ROM starts over and over,
     *                          as after a write cut short,
     *                          or finds no image; whether its loader takes
     *                          fck_flash(), bootloader says.                  */
    FCK_RESCUE_NOTHING,    /**< Nothing answered after a reset. The nRF: no
     *                          MCUboot and no image, and only SWD brings it
     *                          back. The C5: neither its ROM loader nor its
     *                          ROM's lines.                                   */
} fck_rescue_state_t;

/** @brief The most of an image's first line fck_rescue_report_t keeps: a whole `#NRFINFO` up to
 *         its `devid=`, `reset=` and `uptime_ms=`. A longer line is cut there. */
#define FCK_RESCUE_LINE_MAX 95U

/** @brief What a chip carries. */
typedef struct {
    fck_rescue_state_t state;
    bool        bootloader;    /**< What fck_flash() writes through answered
     *                              after the reset: MCUboot's recovery, or the
     *                              C5's ROM loader. false for a C5 whose
     *                              download mode is off.                      */
    uint32_t    bootloader_ms; /**< The nRF: how long after the reset MCUboot
     *                              answered. 0 for the C5.                    */
    bool        one_slot;      /**< The nRF: MCUboot answered SMP's echo too,
     *                              so it is the module's one-slot MCUboot,
     *                              which takes an image by its SHA-256.
     *                              false with bootloader set (TEMPORARY,
     *                              pre-production): a first module's, which
     *                              takes only a signed image. false for the
     *                              C5.                                        */
    bool        image;         /**< The nRF: the image list names an image in
     *                              slot 0; MCUboot names only one whose
     *                              hash or signature it took. false for the C5. */
    char        version[24];   /**< The nRF: that image's version; "" for
     *                              none, and for the C5.                      */
    uint8_t     hash[8];       /**< The nRF: the first bytes of its hash.     */
    char        line[FCK_RESCUE_LINE_MAX + 1U]; /**< The first line the image printed, the boot
     *                              code's left out; "" for none. The C5: its
     *                              first line of its own, or its first
     *                              ESP-IDF log line when it printed no other.  */
    fck_firmware_t firmware;         /**< What it said it runs, asked for with
     *                              `info` when it printed the official firmware's banner
     *                              (or, the nRF, answers SMP); known false
     *                              otherwise.                                 */
    uint8_t     mac[6];        /**< The C5's factory MAC, from its ROM; zeros
     *                              when the ROM did not answer, and for the
     *                              nRF.                                       */
    bool        locked;        /**< The C5 has secure boot or flash encryption
     *                              on: fck_flash() refuses it.                */
    uint32_t    flash_size;    /**< The C5's flash, from its ROM; 0 when the
     *                              ROM did not answer, and for the nRF.       */
    uint32_t    restarts;      /**< The C5: how many times its ROM started
     *                              while it was listened to, 1 for the reset
     *                              and more for an image that does not start.
     *                              0 for the nRF.                             */
    uint32_t    ms;            /**< How long the probe took, `info` left out. */
} fck_rescue_report_t;

/**
 * @brief Blocks. What a chip carries, with nothing written and nothing transmitted:
 *        what fck_flash() would write through, and what the image does once
 *        it starts.
 *
 * Before a chip is written, this says whether its boot code is there to take
 * the image; afterwards, whether the image runs.
 *
 * The nRF. MCUboot checks an image's signature and not the map it was built
 * for: a first module's MCUboot with one slot at 0x10000 takes an image
 * built for FCK_BSP_NRF_SLOT0 and does not
 * start it (FCK_RESCUE_QUIET or FCK_RESCUE_BOOTLOADER, with image set). The
 * nRF is reset - what it ran starts again - and its image list asked for
 * every 150 ms. Once MCUboot answers in its window, it is reset again and
 * nothing is sent while the image starts and prints, for up to 9 s; with no
 * line, the list is asked for again, for up to 3.5 s. Needs the expander
 * for the reset, so not while the
 * GNSS receiver holds the bus.
 *
 * The C5. Its ROM loader is asked what the chip is - its MAC, its flash, and
 * whether secure boot or flash encryption lock it - as fck_flash() asks
 * before it writes; then the C5 is reset into its flash, and nothing is sent
 * while it prints, for up to 5 s. The C5 is reset twice. Its reset and boot
 * lines are the module's own, so the expander is not needed.
 *
 * Either chip is sent `info` only after it printed the official firmware's banner (or,
 * the nRF, answers SMP): another firmware could take it for something else.
 *
 * @param chip FCK_C5 or FCK_NRF.
 * @param out  Out: filled whether it found something or not, once the job
 *             ran.
 * @return true once it looked, whatever it found; false for another
 *         chip, a NULL @p out, no module, a chip on the C5's lines that
 *         answers as another chip, the GNSS receiver holding the bus (the
 *         nRF); false (a timeout) when either chip was kept busy for 10 s by
 *         another task; false (an I/O failure) when the UART or the expander refused.
 */
bool fck_rescue_probe(fck_chip_t chip, fck_rescue_report_t *out);

/* ---- The C5's Wi-Fi and Bluetooth LE ----------------------------------------
 *
 * What the C5's firmware does, in an app's terms. Each call blocks until the
 * firmware has answered, and speaks to any firmware that is compatible
 * (fck_firmware_t.compatible) and lists the command in its caps=: fck-c5, or
 * one of your own. A C5 whose firmware is not, or that has not said what it
 * runs, is sent nothing (false).
 *
 * Results follow kode.h: an array of the caller's with its capacity, filled
 * and counted only when the call returns true, which it also does with
 * nothing to report. false (a timeout) is the firmware not answering in time;
 * false (an I/O failure) the firmware trying and failing - Wi-Fi or Bluetooth not up - or
 * the C5 restarting in the middle.
 *
 * Nothing here is logged, and an app should not log it either: a network's
 * name or address, or a device's, says who is near a person.
 *
 * One call transmits: fck_wifi_scan(). fck_wifi_frames() only listens. */

/** A scan lists at most this many networks, and a survey this many channels
 *  (2.4 GHz 13, 5 GHz 25), and a BLE scan this many advertisers: what the
 *  firmware lists. */
#define FCK_WIFI_NETWORK_MAX        64U
#define FCK_WIFI_CHANNEL_2G_MAX   13U
#define FCK_WIFI_CHANNEL_5G_MAX   25U
#define FCK_WIFI_CHANNEL_MAX      (FCK_WIFI_CHANNEL_2G_MAX + FCK_WIFI_CHANNEL_5G_MAX)
#define FCK_WIFI_CHANNELS_CAP_MAX 256U   /**< The most fck_wifi_channels() takes as @p cap: above it, false. */
#define FCK_BLE_DEVICE_MAX        128U
/** The room of fck_wifi_frame_t.head (the firmware sends at most 46 bytes), and the most frames
 *  one fck_wifi_frames() keeps. */
#define FCK_WIFI_FRAME_HEAD_MAX   48U
#define FCK_WIFI_FRAME_MAX        128U
/** The most bytes one advertisement carries, and the room of fck_ble_device_t.data:
 *  the firmware hears the legacy advertisements, which hold 31. */
#define FCK_BLE_DATA_MAX          31U
/** An RSSI where nothing was heard. */
#define FCK_WIFI_RSSI_NONE     (-127)

/** @brief A band. */
typedef enum {
    FCK_BAND_ALL = 0,   /**< 2.4 GHz, then 5 GHz: only as a request. */
    FCK_BAND_2G,
    FCK_BAND_5G,
} fck_band_t;

/** @brief How a network is secured, with the firmware's names. */
typedef enum {
    FCK_WIFI_SECURITY_OPEN = 0,
    FCK_WIFI_SECURITY_WEP,
    FCK_WIFI_SECURITY_WPA_PSK,
    FCK_WIFI_SECURITY_WPA2_PSK,
    FCK_WIFI_SECURITY_WPA_WPA2_PSK,
    FCK_WIFI_SECURITY_WPA2_ENTERPRISE,
    FCK_WIFI_SECURITY_WPA3_PSK,
    FCK_WIFI_SECURITY_WPA2_WPA3_PSK,
    FCK_WIFI_SECURITY_WAPI_PSK,
    FCK_WIFI_SECURITY_OWE,
    FCK_WIFI_SECURITY_WPA3_ENTERPRISE_192,
    FCK_WIFI_SECURITY_DPP,
    FCK_WIFI_SECURITY_WPA3_ENTERPRISE,
    FCK_WIFI_SECURITY_WPA2_WPA3_ENTERPRISE,
    FCK_WIFI_SECURITY_WPA_ENTERPRISE,
    FCK_WIFI_SECURITY_UNKNOWN,   /**< Or one this SDK does not know yet. */
} fck_wifi_security_t;

/** @brief One network. */
typedef struct {
    char            ssid[33];  /**< NUL-terminated; "" for a hidden network.   */
    uint8_t         ssid_len;  /**< Its bytes, without the terminator.          */
    uint8_t         bssid[6];
    uint8_t         channel;   /**< The primary channel.                        */
    fck_band_t      band;      /**< FCK_BAND_2G or FCK_BAND_5G.                 */
    int8_t          rssi;      /**< dBm.                                        */
    fck_wifi_security_t auth;
} fck_wifi_network_t;

/** @brief One channel of a survey. */
typedef struct {
    uint8_t    channel;
    fck_band_t band;       /**< FCK_BAND_2G or FCK_BAND_5G.                    */
    uint32_t   frames;     /**< Every frame heard while tuned there - on
     *                          2.4 GHz the neighbouring channels' too.        */
    uint32_t   mgmt;
    uint32_t   data;
    uint32_t   ctrl;
    int8_t     rssi_max;   /**< dBm; FCK_WIFI_RSSI_NONE when frames is 0.      */
    int8_t     rssi_avg;
    uint16_t   dwell_ms;   /**< How long it counted, by the C5's own tick.     */
} fck_wifi_channel_t;

/**
 * @brief One Wi-Fi frame the C5 told, summarised: its MAC header and a short head.
 *
 * The frame control's first byte says what it is: the type is bits 2 and 3 (0 management, 1
 * control, 2 data) and the subtype bits 4 to 7 (for a management frame 4 a probe request, 5 a
 * probe response, 8 a beacon, 10 a disassociation, 12 a deauthentication). @c head is what follows
 * the 24-byte MAC header on the air, and is all of the frame's body that ever leaves the C5:
 *  - beacon, probe response: the 12 fixed bytes (timestamp, interval, capabilities) and, when it
 *    is the first tag and lies whole in the frame (0 to 32 bytes), the network's name as its tag:
 *    id 0, its length, the name. A frame that has not the 12 fixed bytes has no head;
 *  - probe request: the name's tag under the same rule (the wildcard, of length 0, too), or none;
 *  - deauthentication, disassociation: the 2 bytes of the reason, when the frame has them;
 *  - data: none; its payload is never read.
 */
typedef struct {
    uint8_t  fc[2];        /**< Frame control, as on the air.                    */
    uint8_t  channel;      /**< The channel the radio says it received it on.    */
    int8_t   rssi;         /**< dBm.                                              */
    uint16_t len;          /**< Its length on the air.                            */
    uint8_t  addr1[6];     /**< Receiver; the first byte on the air first.        */
    uint8_t  addr2[6];     /**< Transmitter.                                      */
    uint8_t  addr3[6];
    uint8_t  head_len;     /**< The bytes of @c head: 0 to 46.                    */
    uint8_t  head[FCK_WIFI_FRAME_HEAD_MAX];  /**< What follows the 24-byte MAC header (and the
     *                          4 bytes of HT Control of a management frame with Order set);
     *                          zeros past @c head_len. A management frame with Protected set
     *                          (802.11w) has its body encrypted: it has no head. */
} fck_wifi_frame_t;

/** @brief What a fck_wifi_frames() heard in all, told or not. */
typedef struct {
    uint8_t  channel;
    uint32_t ms;             /**< How long it listened, by the C5's clock.        */
    uint32_t heard;          /**< Every frame the radio handed over: mgmt + data + ctrl, the
     *                            neighbouring channels' too.                      */
    uint32_t mgmt;
    uint32_t data;
    uint32_t ctrl;
    uint32_t beacon;         /**< From here, every one of that kind heard, told or not. */
    uint32_t probe_request;
    uint32_t probe_response;
    uint32_t deauth;
    uint32_t disassoc;
    uint32_t sent;           /**< Frames the firmware told: more than the call kept when @p cap
     *                            was smaller.                                     */
    uint32_t dropped;        /**< Frames it chose to tell and could not: its queue to the
     *                            UART, or its table of senders, was full.         */
} fck_wifi_frames_statistics_t;

/** @brief How a Bluetooth LE device advertised: the kind of its latest report. */
typedef enum {
    FCK_BLE_ADVERTISING_CONNECTABLE = 0,  /**< It takes a connection from anybody
     *                                         (ADV_IND).                          */
    FCK_BLE_ADVERTISING_DIRECTED,         /**< It calls back one device it knows
     *                                         (ADV_DIRECT_IND), and carries no
     *                                         data.                               */
    FCK_BLE_ADVERTISING_SCANNABLE,        /**< It takes no connection, and would
     *                                         answer a scan request (ADV_SCAN_IND),
     *                                         which this SDK never sends.         */
    FCK_BLE_ADVERTISING_NOT_CONNECTABLE,  /**< It only broadcasts (ADV_NONCONN_IND),
     *                                         as a beacon does.                   */
    FCK_BLE_ADVERTISING_UNKNOWN,          /**< The firmware did not say: one
     *                                         without "bleadv".                   */
} fck_ble_advertising_t;

/**
 * @brief One Bluetooth LE advertiser.
 *
 * @c advertising, @c first_ms, @c last_ms and @c data come from a fck-c5 that
 * says "bleadv" in its caps= (0.2.0), and are read all the same from one of
 * the first modules' line that printed them without the word (2.2, 2.4).
 * fck_firmware() says what a module runs, and fck_flash() writes a newer firmware.
 * With a firmware without it the scan works as it always did, and for every
 * device @c advertising is FCK_BLE_ADVERTISING_UNKNOWN and the other three
 * are 0.
 */
typedef struct {
    uint8_t  addr[6];    /**< The most significant byte first.                 */
    bool     random;     /**< A random address, which many devices rotate.     */
    int8_t   rssi;       /**< dBm, of its latest report.                       */
    char     name[32];   /**< The complete local name, else the short one,
     *                        NUL-terminated; "" when it sends none.            */
    uint8_t  name_len;
    uint16_t adv;        /**< Its reports heard.                               */
    fck_ble_advertising_t advertising;  /**< How its latest report was sent.   */
    uint32_t first_ms;   /**< When its first report was heard, counted from
     *                        the start of the scan.                            */
    uint32_t last_ms;    /**< When its latest was.                             */
    uint8_t  data[FCK_BLE_DATA_MAX];  /**< What its latest report that carried
     *                        anything carried, as it was sent: one field
     *                        after another, each a length, a type and its
     *                        bytes (Bluetooth's AD structures).
     *                        fck_ble_manufacturer_data(),
     *                        fck_ble_service_data(), fck_ble_services() and
     *                        fck_ble_appearance() read the four most used;
     *                        the rest is here for an app to read.             */
    uint8_t  data_len;   /**< Its bytes; 0 when no report carried any.         */
} fck_ble_device_t;

/**
 * @brief Blocks. The networks in range, strongest first. TRANSMITS.
 *
 * On 2.4 GHz the C5 scans actively - probe requests - on channels 1 to 13,
 * inside the radio limits the firmware keeps itself; 5 GHz is only listened to, on
 * channels 100 to 140. Those limits are the firmware's own: while one of them
 * is not in force - its Wi-Fi still coming up, a limit its driver refused -
 * it refuses the command itself, and this call answers false. A firmware of
 * your own that takes `scan` is asked the same, and what it sends is yours.
 *
 * The firmware lists at most FCK_WIFI_NETWORK_MAX networks, 2.4 GHz first, so with
 * that many on 2.4 GHz FCK_BAND_ALL shows no 5 GHz network: ask for it alone.
 * Equal strengths keep the firmware's order.
 *
 * @param band FCK_BAND_ALL, FCK_BAND_2G or FCK_BAND_5G.
 * @param out  Out: the strongest @p cap, at most FCK_WIFI_NETWORK_MAX.
 * @param cap  How many @p out holds; at least 1.
 * @param n    Out: how many were written; may be NULL.
 * @return true, with no network too; false for a bad argument, or as
 *         above; false (no memory) when the call could not get memory for its
 *         list (@p cap networks); the rest as above.
 */
bool fck_wifi_scan(fck_band_t band, fck_wifi_network_t *out, size_t cap, size_t *n);

/**
 * @brief Blocks. How busy each channel is. Only listens: the C5 counts the frames it
 *        hears and keeps no address and no payload.
 *
 * @param band     FCK_BAND_ALL, FCK_BAND_2G or FCK_BAND_5G.
 * @param dwell_ms How long each channel is listened to, 20 to 5000. A survey
 *                 of both bands at the firmware's default of 250 takes about
 *                 10 s.
 * @param out      Out: the channels in the firmware's order - 2.4 GHz first, by
 *                 number - without those the country code does not allow.
 * @param cap      How many @p out holds: at least the band's channels,
 *                 FCK_WIFI_CHANNEL_2G_MAX, FCK_WIFI_CHANNEL_5G_MAX or
 *                 FCK_WIFI_CHANNEL_MAX, or nothing is sent; and at most
 *                 FCK_WIFI_CHANNELS_CAP_MAX, or nothing is sent either.
 * @param n        Out: how many were written; may be NULL.
 * @return true; false for a bad argument, a @p cap too small or above
 *         FCK_WIFI_CHANNELS_CAP_MAX, or a firmware without the command; false (no memory) for a firmware that lists more
 *         channels than @p cap; the rest as above.
 */
bool fck_wifi_channels(fck_band_t band, uint16_t dwell_ms, fck_wifi_channel_t *out,
                                size_t cap, size_t *n);

/**
 * @brief Blocks. The frames the C5 hears on one Wi-Fi channel, a digest of them. Only listens:
 *        it transmits nothing, and nothing past a network's name leaves the C5.
 *
 * The UART carries 11.5 kB/s and a busy channel has hundreds of frames a second, so the firmware
 * tells each sender once and counts the rest:
 *  - a beacon, and a probe response: the first of each, by kind and sender (address 2), in this
 *    call: a sender that sends both is told twice;
 *  - a probe request: the first of each address 2 and name;
 *  - a deauthentication or a disassociation: every one;
 *  - a data frame: the first of each pair of addresses 1 and 2;
 *  - any other management frame, and every control frame: none, only counted.
 * A frame shorter than a MAC header is counted and never told. @p stats has the counts of
 * everything heard, told or not (@c sent and @c dropped say how many of the digest got a line);
 * @p out has the frames told, in the order heard, the first @p cap and at most FCK_WIFI_FRAME_MAX.
 * A later one is not kept (@c stats->sent says how many there were). Each call starts from
 * nothing: a sender heard in two calls is a frame in each.
 *
 * It blocks for @p ms and the time the UART takes to empty what was queued - up to 3 s more,
 * worked out from the lines' lengths. fck_cancel() ends it, and then
 * nothing is written. To go over several channels, call it once per channel.
 *
 * Nothing here is logged, and an app should not log it either: the addresses and names it
 * returns say who is near a person.
 *
 * @param channel 1 to 177. One the firmware does not list (14, 32, 68, 169-177...) is refused
 *                (false, KODE_DENIED: rc 2); one its radio driver will not tune to fails
 *                (false, KODE_IO_ERROR: rc 1).
 * @param ms      How long it listens, 20 to 5000.
 * @param out     Out: the frames told, the first @p cap, at most FCK_WIFI_FRAME_MAX.
 * @param cap     How many @p out holds; at least 1.
 * @param n       Out: how many were written; may be NULL.
 * @param stats   Out: the counts; may be NULL.
 * @return true, with no frame too; false for a bad argument or a firmware without "frames" in
 *         its caps= (fck-c5 0.2.0 has it; 2.4 of the first modules' line too): nothing is sent,
 *         and fck_error() says the firmware has no frame listener; false when the
 *         firmware answers that it does not know the command or takes the channel or the time for
 *         bad (fck_error() says rc 2) or that it could not listen on that channel (rc 1); false
 *         (no memory) when the call could not get memory for its list; the rest as above.
 */
bool fck_wifi_frames(uint8_t channel, uint16_t ms, fck_wifi_frame_t *out, size_t cap, size_t *n,
                     fck_wifi_frames_statistics_t *stats);

/**
 * @brief Blocks. The Bluetooth LE advertisers heard for @p duration_s seconds, strongest
 *        first. Only listens: the C5 sends no scan request, so a name that
 *        only a scan response carries is never seen.
 *
 * The first scan also starts the C5's Bluetooth stack: add about 2 s.
 *
 * Many devices put their name only in that response, so here they come with
 * none. The Kode's own Bluetooth does ask for it (kode-sdk's
 * kode_ble_scan()): an app that wants the names matches that list with this
 * one by address.
 *
 * With "bleadv" in the firmware's caps= each device comes with what it advertised
 * (fck_ble_device_t.data) and when it was heard; telling what a device is
 * from that is the app's work.
 *
 * To listen for minutes, call it again and again. Between one scan and the
 * next the C5 hears nothing for as long as its list takes to cross to the
 * Kode at 115200 baud: 8 ms for a device that carried nothing to 14 ms for
 * one with a whole advertisement, so about 0.4 s with 30 devices around and
 * up to 2 s with the 128 a scan lists at most (worked out from the lines'
 * lengths). Each scan starts from nothing: a device
 * heard in two is an entry in each, with its times counted from that scan's
 * start, and a random address may have changed in between.
 *
 * @param duration_s 1 to 60.
 * @param out  Out: the strongest @p cap, at most FCK_BLE_DEVICE_MAX.
 * @param cap  How many @p out holds; at least 1.
 * @param n    Out: how many were written; may be NULL.
 * @return true, with none too; false for a bad argument or a firmware
 *         without the command; false (no memory) when the call could not get memory
 *         for its list; false (I/O) when the firmware could not start the
 *         scan or cut it short, and then @p out and @p n hold what it heard
 *         until then; the rest as above.
 */
bool fck_ble_scan(uint8_t duration_s, fck_ble_device_t *out, size_t cap, size_t *n);

/**
 * @brief Returns at once. The manufacturer's data @p device advertised: whose it is, and its
 *        bytes. Plain C, no radio and no module: it reads fck_ble_device_t.data, so it
 *        serves a device an app kept as well as one a scan just gave.
 *
 * A manufacturer's data is the field of type 0xFF: the number the Bluetooth SIG
 * assigned to a company (0x004C is Apple's) and bytes whose meaning is that
 * company's. Of two such fields the first is read; one too short to hold a
 * company's number is not one.
 *
 * @param device  One of fck_ble_scan()'s.
 * @param company Out: the company's number; may be NULL.
 * @param out     Out: the bytes after the company's number, 27 at most; may be NULL
 *                when @p cap is 0, to ask only whether and whose.
 * @param cap     How many @p out holds: with fewer than there are, the first.
 * @param n       Out: how many were written; may be NULL.
 * @return true when it advertised one; false when it did not - no such field,
 *         or no data at all, as from a firmware without "bleadv" - and for a NULL
 *         @p device or a NULL @p out with a @p cap above 0. On false nothing
 *         is written.
 */
bool fck_ble_manufacturer_data(const fck_ble_device_t *device, uint16_t *company, uint8_t *out, size_t cap,
                               size_t *n);

/**
 * @brief Returns at once. The data of one service that @p device advertised, by the service's
 *        16-bit UUID. Plain C, no radio and no module, like
 *        fck_ble_manufacturer_data().
 *
 * A service's data is the field of type 0x16: the service's UUID and bytes whose
 * meaning is that service's (0xFEAA is the one an Eddystone beacon fills). A
 * device may carry a service's data without listing the service:
 * fck_ble_services() is the list, and this is the data. Of two fields of the
 * same service the first is read.
 *
 * @param device One of fck_ble_scan()'s.
 * @param uuid   The service.
 * @param out    Out: the bytes after the UUID, 27 at most; may be NULL when
 *               @p cap is 0, to ask only whether.
 * @param cap    How many @p out holds: with fewer than there are, the first.
 * @param n      Out: how many were written; may be NULL.
 * @return true when it advertised data of that service, were it of no bytes;
 *         false as fck_ble_manufacturer_data().
 */
bool fck_ble_service_data(const fck_ble_device_t *device, uint16_t uuid, uint8_t *out, size_t cap, size_t *n);

/**
 * @brief Returns at once. The 16-bit services @p device lists, in its order: those of the list it
 *        calls incomplete (the field of type 0x02) and of the one it calls
 *        complete (0x03). Plain C, no radio and no module, like
 *        fck_ble_manufacturer_data().
 *
 * @param device One of fck_ble_scan()'s.
 * @param out    Out: the UUIDs (0x180F is the battery service), 14 at most; may
 *               be NULL when @p cap is 0, to ask only whether.
 * @param cap    How many @p out holds: with fewer than there are, the first.
 * @param n      Out: how many were written; may be NULL.
 * @return true when it lists at least one; false as
 *         fck_ble_manufacturer_data().
 */
bool fck_ble_services(const fck_ble_device_t *device, uint16_t *out, size_t cap, size_t *n);

/**
 * @brief Returns at once. What @p device says it is: the appearance it advertised. Plain C, no
 *        radio and no module, like fck_ble_manufacturer_data().
 *
 * The appearance is the field of type 0x19: one of the numbers the Bluetooth SIG
 * assigned to kinds of device (0x0040 is a phone, 0x00C0 a watch, 0x03C1 a
 * keyboard). Most devices advertise none, and say what they are with a
 * manufacturer's data or a service instead. Of two such fields the first is
 * read; one that is not two bytes is not one.
 *
 * @param device     One of fck_ble_scan()'s.
 * @param appearance Out: the number; may be NULL, to ask only whether.
 * @return true when it advertised one; false when it did not - no such field,
 *         or no data at all, as from a firmware without "bleadv" - and for a NULL
 *         @p device. On false nothing is written.
 */
bool fck_ble_appearance(const fck_ble_device_t *device, uint16_t *appearance);

/* ---- The GNSS receiver ------------------------------------------------------
 *
 * The module's receiver talks to the C5, whose firmware relays what it says; the
 * SDK reads the sentences as they come and keeps the latest fix. Speaks only
 * to the firmware, like the calls above. A position is private: nothing here
 * writes one, or a sentence, to the log, and an app should not either.
 *
 * Running, the receiver costs energy and, with its standard firmware, takes
 * the module's I2C bus away about 420 ms after it starts: the expander is out
 * of reach until fck_gnss_end(), and what needs it - fck_reset() and
 * fck_power() of the nRF, fck_flash() through MCUboot's recovery - answers
 * false meanwhile. fck_status().gnss_running says whether it is on.
 *
 * The firmware does not stop the receiver, or its relay, while the C5 scans: the
 * sentences come between the lines of a Wi-Fi or a Bluetooth scan's list, and
 * the latest fix goes on being kept. To say where a scan was, read the fix as
 * the scan answers - fck_gnss_read(&fix, 0) - and look at its age_ms: with one
 * fix a second it is under a second, and one much older says the position
 * stopped coming, not where the scan was. The C5 has one radio, so the scans
 * themselves go one after another: a round of Wi-Fi 2.4 GHz, Wi-Fi 5 GHz and
 * Bluetooth is the sum of the three, some 1.5 s, 3.3 s and the Bluetooth
 * scan's own duration, and up to a second more for each list to cross.
 *
 * What the C5 transmits on 2.4 GHz may cost the receiver a fix now and
 * then, and age_ms is what tells. */

/** @brief The latest fix: one GGA and one RMC of the receiver's. */
typedef struct {
    uint8_t  quality;    /**< 0 none, 1 GNSS, 2 differential, 4 RTK fixed,
     *                       5 RTK float, 6 estimated.                        */
    uint8_t  sats;       /**< The satellites used.                            */
    bool     valid;      /**< The receiver vouches for the fix.               */
    double   lat;        /**< Degrees, north positive. NAN without a fix.      */
    double   lon;        /**< Degrees, east positive. NAN without a fix.       */
    double   alt_m;      /**< Above mean sea level. NAN without one.           */
    double   hdop;       /**< Horizontal dilution of precision; the receiver
     *                       says 100 with no fix.                             */
    double   speed_kn;   /**< Over the ground, in knots.                      */
    double   course_deg; /**< Over the ground, from true north.               */
    bool     has_time;   /**< hour, min, sec and ms hold a UTC time.          */
    uint8_t  hour;
    uint8_t  min;
    uint8_t  sec;
    uint16_t ms;
    bool     has_date;   /**< day, month and year hold a UTC date.            */
    uint8_t  day;
    uint8_t  month;
    uint16_t year;
    uint32_t age_ms;     /**< How long ago the Kode read the sentence that
     *                       completed this fix, by the Kode's own clock. The
     *                       receiver measured some tenths of a second before:
     *                       hour, min, sec and ms say when, in UTC.          */
} fck_gnss_fix_t;

/**
 * @brief Blocks. Take the receiver out of reset and turn the C5's relay on.
 *
 * Calling it again does nothing. If the relay cannot be turned on the
 * receiver is put back in reset.
 *
 * @return true; false with no module, a C5 whose firmware is not
 *         compatible, or the expander out of reach; false (a timeout) when the firmware
 *         did not answer; false (an I/O failure) when the expander did not take it or
 *         the firmware refused.
 */
bool fck_gnss_begin(void);

/**
 * @brief Blocks. The latest fix, or the next one.
 *
 * The receiver sends one fix a second, a GGA and an RMC. Without a sky view it
 * sends them with quality 0 and no position: look at @c valid. The latest fix
 * stays the latest for as long as the receiver is on, however old: look at
 * @c age_ms before taking it for where the Kode is now.
 *
 * @param out Out: filled on true only.
 * @param ms  0: the latest fix, if there is one. Else: wait up to @p ms for a
 *            fix that comes after this call.
 * @return true; false when the receiver is not on (fck_gnss_begin())
 *         or, with @p ms 0, nothing has come yet; false (a timeout) when nothing
 *         came in @p ms; false (an I/O failure) when the module left on the way.
 *
 * fck_cancel() ends it: false, "cancelled by fck_cancel()" in fck_error().
 */
bool fck_gnss_read(fck_gnss_fix_t *out, uint32_t ms);

/**
 * @brief Blocks. Put the receiver back in reset, where it costs nothing, and turn
 *        the relay off.
 * @return true once the receiver is in reset, even when the C5 would not
 *         say it turned the relay off; false with no module;
 *         false (an I/O failure) when the expander did not take it (the receiver is
 *         counted as in reset all the same: the owner wrote it by hand).
 */
bool fck_gnss_end(void);

/* ---- The nRF's 2.4 GHz radio ------------------------------------------------
 *
 * What the nRF54L15's firmware does, in an app's terms. Each call blocks until
 * the firmware has answered, as fck_command() does, and speaks to any firmware
 * that is compatible (fck_firmware_t.compatible) and lists the command in its
 * caps=: fck-nrf, or one of your own. An nRF whose firmware is not, or that
 * has not said what it runs, is sent nothing (false).
 *
 * Results follow kode.h: an array of the caller's with its capacity, filled
 * and counted only when the call returns true, which it also does with
 * nothing to report. false (a timeout) is the firmware not answering in time;
 * false (an I/O failure) the firmware trying and failing - its clock would not start -
 * or the nRF restarting in the middle.
 *
 * Two calls transmit: fck_radio_ping() and fck_radio_echo_start(). Both stay
 * inside the module's EU limits, 2402 to 2480 MHz and at most 6 dBm, or are refused before anything is written; and both set
 * the power they were given, and transmit at it, with no other task's
 * command between the two. */

/** What fck_radio_spectrum() and fck_radio_sniff() listen to, in MHz. */
#define FCK_RADIO_RECEIVE_MHZ_MIN   2400U
#define FCK_RADIO_RECEIVE_MHZ_MAX   2500U
/** Where fck_radio_ping() and fck_radio_echo_start() may transmit, and how
 *  strongly: the module's EU band and level. */
#define FCK_RADIO_TRANSMIT_MHZ_MIN   2402U
#define FCK_RADIO_TRANSMIT_MHZ_MAX   2480U
#define FCK_RADIO_TRANSMIT_DBM_MAX   6
/** The most frames fck_radio_sniff() returns, and the bytes kept of each:
 *  what the firmware keeps. */
#define FCK_RADIO_FRAMES_MAX   48U
#define FCK_RADIO_FRAME_MAX    40U
/** An RSSI where nothing was heard. */
#define FCK_RADIO_QUIET_DBM    (-127)

/**
 * @brief Blocks. The peak RSSI on each MHz from @p first_mhz to @p last_mhz, over
 *        @p sweeps sweeps. Only listens.
 *
 * @param dwell_us How long each MHz is listened to in a sweep, 10 to 100000;
 *                 the firmware's default is 200.
 * @param sweeps   1 to 1000.
 * @param dbm      Out: dbm[i] for first_mhz + i, FCK_RADIO_QUIET_DBM where
 *                 nothing was heard.
 * @param cap      How many @p dbm holds: at least the range, or nothing is
 *                 sent.
 * @param n        Out: last_mhz - first_mhz + 1; may be NULL.
 * @return true; false for a range outside FCK_RADIO_RECEIVE_MHZ_MIN to
 *         FCK_RADIO_RECEIVE_MHZ_MAX, reversed, too big for @p cap, or a dwell or a
 *         count out of range; the rest as above.
 *
 * fck_cancel() ends it: false, "cancelled by fck_cancel()" in fck_error().
 */
bool fck_radio_spectrum(uint16_t first_mhz, uint16_t last_mhz, uint32_t dwell_us,
                              uint16_t sweeps, int8_t *dbm, size_t cap, size_t *n);

/** @brief One different frame fck_radio_sniff() heard. */
typedef struct {
    uint8_t  len;                      /**< Bytes in data.                   */
    uint8_t  data[FCK_RADIO_FRAME_MAX];
    int8_t   rssi;                     /**< The strongest it was heard at.   */
    uint16_t seen;                     /**< How many times.                  */
} fck_radio_frame_t;

/** @brief What one fck_radio_sniff() heard in all. */
typedef struct {
    uint32_t heard;      /**< Frames heard.                                  */
    uint32_t weak;       /**< Of them, weaker than rssi_min: left out.       */
    uint8_t  distinct;   /**< Different frames the firmware kept; more than the
                          *   call returned when its cap was smaller.        */
    uint32_t ms;         /**< How long it listened.                          */
} fck_radio_sniff_statistics_t;

/**
 * @brief Blocks. The bursts heard on @p mhz for @p ms - a remote, a mouse, a toy -
 *        with no address to know beforehand. Only listens.
 *
 * Frames are told apart by their bytes; the most repeated come first. How
 * the air is read and what is locked on to is fck_radio_sniff_tune()'s.
 *
 * @param mhz      FCK_RADIO_RECEIVE_MHZ_MIN to FCK_RADIO_RECEIVE_MHZ_MAX.
 * @param ms       100 to 600000.
 * @param rssi_min Frames weaker than this, in dBm, are counted and left out:
 *                 -127 to 0. -60 is the firmware's default.
 * @param out      Out: the frames, at most FCK_RADIO_FRAMES_MAX.
 * @param cap      How many @p out holds; at least 1.
 * @param n        Out: how many were written; may be NULL.
 * @param stats    Out: what was heard in all; may be NULL.
 * @return true, with 0 frames too; false for an argument out of
 *         range; the rest as above.
 *
 * fck_cancel() ends it: false, "cancelled by fck_cancel()" in fck_error().
 */
bool fck_radio_sniff(uint16_t mhz, uint32_t ms, int8_t rssi_min, fck_radio_frame_t *out,
                           size_t cap, size_t *n, fck_radio_sniff_statistics_t *stats);

/** @brief How fck_radio_sniff() reads the air, and what it locks on to. Each
 *         field is given with its value at the nRF's start. */
typedef struct {
    uint8_t  rate_mbps;   /**< 1 or 2 (GFSK). 1.                            */
    uint8_t  crc_bytes;   /**< 0, every frame kept; or 2, only those whose
                           *   CRC-16 checks. 0.                            */
    uint8_t  len;         /**< Bytes kept of each frame, 1 to
                           *   FCK_RADIO_FRAME_MAX. 32.                        */
    uint8_t  base_len;    /**< The address's bytes after its prefix, 2 to 4.
                           *   2.                                           */
    uint32_t base;        /**< 0xAAAAAAAA: a run of preamble.               */
    uint8_t  prefix;      /**< 0xAA.                                        */
} fck_radio_sniff_config_t;

/**
 * @brief Blocks. Set how fck_radio_sniff() listens, until the nRF next starts.
 * @return true; false for a value the firmware does not take, with
 *         nothing sent; the rest as above.
 */
bool fck_radio_sniff_tune(const fck_radio_sniff_config_t *cfg);

/** @brief What one fck_radio_ping() got back. */
typedef struct {
    uint16_t sent;
    uint16_t ok;            /**< Echoes back.                              */
    int16_t  fwd_mean_x10;  /**< The mean RSSI the responder heard, in
                             *   tenths of a dBm; 0 when ok is 0.          */
    int16_t  rev_mean_x10;  /**< The mean RSSI heard back.                 */
    int8_t   fwd_min, fwd_max, rev_min, rev_max;   /**< 0 when ok is 0.    */
    int8_t   dbm;           /**< The level sent at: the step of the
                             *   nRF54L15 at or below the one asked for.   */
    uint16_t timeout;       /**< Pings that got nothing back.              */
    uint16_t crc_bad;       /**< ... something with a bad CRC.             */
    uint16_t mismatch;      /**< ... something that was not their echo.    */
    uint16_t tx_fail;       /**< ... that did not go out.                  */
    uint8_t  peer;          /**< The responder's node id, from its echoes. */
    int8_t   peer_dbm;      /**< The level it said it answered at.         */
} fck_radio_ping_statistics_t;

/**
 * @brief Blocks. Ping another F*CK whose nRF is in fck_radio_echo_start(). TRANSMITS.
 *
 * @p count packets on @p mhz at @p dbm, each answered by the responder with
 * what it heard. Only an echo counts, and each carries its responder's node
 * id, so a ping never counts the F*CK beside it.
 *
 * @param count  1 to 1000.
 * @param mhz    FCK_RADIO_TRANSMIT_MHZ_MIN to FCK_RADIO_TRANSMIT_MHZ_MAX.
 * @param gap_ms The pause after each packet, 0 to 1000.
 * @param dbm    -46 to FCK_RADIO_TRANSMIT_DBM_MAX.
 * @param stats  Out: required.
 * @return true, with no echo at all too (stats->ok 0); false for an
 *         argument out of range, with nothing sent; the rest as above.
 *
 * fck_cancel() ends it: false, "cancelled by fck_cancel()" in fck_error().
 */
bool fck_radio_ping(uint16_t count, uint16_t mhz, uint16_t gap_ms, int8_t dbm,
                          fck_radio_ping_statistics_t *stats);

/** @brief What the responder saw while it was on. */
typedef struct {
    uint32_t heard;      /**< Packets heard.                                */
    uint32_t crc_bad;    /**< Of them, with a bad CRC.                      */
    uint32_t replied;    /**< Echoes sent.                                  */
    uint32_t foreign;    /**< Heard, but not a ping.                        */
    uint32_t tx_slow;    /**< Echoes the nRF's radio did not end by itself:
     *                        the firmware switched it off right after the
     *                        packet. Not 0 is a fault of the radio.        */
    bool     idle;       /**< The responder had stopped by itself, no ping
     *                        having reached it for a minute; false for a
     *                        firmware that does not say.                   */
} fck_radio_echo_statistics_t;

/**
 * @brief Blocks. Answer every ping heard on @p mhz, at @p dbm, until
 *        fck_radio_echo_stop(). TRANSMITS, in the background.
 *
 * Any other call that uses the nRF's radio stops it too, as do a reset and
 * the way home - and it stops by itself, radio off, when no ping has reached
 * it for a minute (the firmware's `echoidle`): fck_radio_echo_stop() then
 * says so in fck_radio_echo_statistics_t.idle. Start it again to go on.
 * (TEMPORARY, pre-production: 2.x answers until it is told to stop.)
 *
 * @param mhz FCK_RADIO_TRANSMIT_MHZ_MIN to FCK_RADIO_TRANSMIT_MHZ_MAX.
 * @param dbm -46 to FCK_RADIO_TRANSMIT_DBM_MAX.
 * @return true once it answers; false for an argument out of range,
 *         with nothing sent; the rest as above.
 */
bool fck_radio_echo_start(uint16_t mhz, int8_t dbm);

/**
 * @brief Blocks. Stop answering pings, and say what was seen.
 * @param stats Out: may be NULL.
 * @return true, the radio idle; the rest as above. An nRF that restarted
 *         since fck_radio_echo_start() is not answering anyway.
 */
bool fck_radio_echo_stop(fck_radio_echo_statistics_t *stats);

/* ---- Sub-GHz: the CC1101 -------------------------------------------------------
 *
 * The CC1101 hangs from the P4 itself. It listens on what the chip tunes:
 * 300-348, 387-464 and 779-928 MHz; it transmits only where the module may,
 * 433.05-434.79 and 868.0-868.6 MHz (fck_subghz_may_transmit()), and only
 * from fck_subghz_send_raw(), fck_subghz_send_code() and
 * fck_subghz_packet_send(). Each call but the listening's has the whole module for its length,
 * so a command to the C5 or the nRF waits for it to end, while what they say
 * goes on being read: a GNSS fix keeps coming. A call waits in turn for what
 * another task asked of them, up to 3 s for each chip, and answers
 * false (a timeout) after that (fck_error() says the chips stayed busy); a BLE
 * scan or a channel survey holds the C5 longer. The CC1101 sleeps
 * between calls.
 *
 * The module has neither listen-before-talk nor adaptive frequency agility,
 * so its time on air is limited: at most
 * 10 % of any hour in 433.05-434.79 MHz (360 s) and 1 % in 868.0-868.6 MHz
 * (36 s). The SDK keeps the account of the last hour per band, from the
 * first transmission since the app started - nothing survives a restart -
 * and refuses with false a send that would go over;
 * fck_subghz_air_time_left() says what is left. A send counts whole, from
 * the moment it is asked for: the air time is the signal's full length, off
 * runs included, times its repeats (a packet's, preamble to CRC), and it
 * stays in the account for the next 60 to 61 minutes.
 *
 * One call may also keep the carrier on for FCK_SUBGHZ_SEND_MS_MAX at most
 * (3 s): the positive runs of an OOK signal, or a packet's whole air time,
 * times the repeats, worked out before the PA is keyed. A call over it is
 * refused with false and nothing is transmitted; 16 EV1527 frames at
 * the slowest te are about 1.5 s, so a remote's own signal always fits.
 *
 * The antenna leg follows the frequency, each band through its own leg
 * (fck_bsp.h): 433 MHz's leg (349-600 MHz) and
 * 868/915's (above 600 MHz) need SW0 at the level it rests at, and 315's
 * (up to 348 MHz) needs SW0 moved through the module's expander. A GNSS
 * receiver whose firmware holds the module's bus (fck_status()'s
 * gnss_running) takes the expander with it: meanwhile a frequency on 315's
 * leg answers false, and a listen that hops skips it.
 *
 * Every dBm here is the chip's own: its RSSI register with the datasheet's
 * offset of 74 dB, uncalibrated. The module's front end before it - the
 * filter after the balun and the antenna switches - takes a few dB the
 * figure does not add back, different on each leg. Compare levels with each other, on one
 * frequency; do not read them as what the antenna received.
 *
 * Some calls are plain C and touch no radio and no owner: decoding and encoding a code,
 * decoding a rolling one, the list of frequencies, and Kode .ksub files. They never wait,
 * so they are allowed anywhere - a listener, an Active Object, an interrupt-free task - and a
 * false from them says why in fck_error() as any other. Reading and writing the files is the
 * app's, on a task, with kode.h's kode_file_*() (the .ksub files are text).
 *
 * What a remote sends belongs to its owner: the SDK writes no capture, key
 * or level to the log.
 */

/** @brief The receiver's channel filter. */
typedef enum {
    FCK_SUBGHZ_WIDE   = 0,   /**< 650 kHz: a remote whose exact frequency is
                              *   not known.                                 */
    FCK_SUBGHZ_NARROW = 1,   /**< 58 kHz: one frequency, closely.            */
} fck_subghz_bandwidth_t;

/** A frequency of a spectrum the CC1101 does not tune, or could not take. */
#define FCK_SUBGHZ_NO_DBM      INT16_MIN
/** The most frequencies one spectrum reads. */
#define FCK_SUBGHZ_POINTS_MAX  1024U
/** The longest fck_subghz_find() and fck_subghz_capture() listen. */
#define FCK_SUBGHZ_MS_MAX      60000U

/**
 * @brief Blocks. The power on one frequency, now: the highest of 3 readings over
 *        0.5 ms with the wide filter, of 8 over 1.7 ms with the narrow one.
 * @param dbm Written on true.
 * @return true; false for a frequency the CC1101 does not tune or
 *         whose leg cannot be had (above), with no module, or with one whose
 *         CC1101 did not answer when it was found; false (a timeout) while
 *         another task keeps the C5 or the nRF busy (above); false (an I/O failure)
 *         when the chip would not listen.
 */
bool fck_subghz_level(uint32_t hz, fck_subghz_bandwidth_t bw, int16_t *dbm);

/**
 * @brief Blocks. The power on @p count frequencies, @p first_hz + i * @p step_hz,
 *        each read as fck_subghz_level() reads one.
 * @param dbm @p count levels, FCK_SUBGHZ_NO_DBM where the CC1101 does not
 *            tune or its leg cannot be had; written on true.
 * @param cap What @p dbm holds: at least @p count.
 * @param n   @p count, on true.
 * @return true; false for @p count 0, over FCK_SUBGHZ_POINTS_MAX or
 *         over @p cap, when not one of the frequencies can be read, and as
 *         fck_subghz_level(); false (an I/O failure) when none reached the receiver.
 *
 * fck_cancel() ends it: false, "cancelled by fck_cancel()" in fck_error().
 */
bool fck_subghz_spectrum(uint32_t first_hz, uint32_t step_hz, uint16_t count,
                                  fck_subghz_bandwidth_t bw, int16_t *dbm, size_t cap, size_t *n);

/** fck_subghz_listen_start()'s frequency for hopping. */
#define FCK_SUBGHZ_HOP 0U

/**
 * @brief Blocks. Start listening for remotes: on @p hz, or hopping with FCK_SUBGHZ_HOP. The
 *        SDK listens in the background, in slices of 200 ms whenever nothing else wants the
 *        CC1101, and keeps the latest remote it heard; fck_subghz_listen_read() reads it, and
 *        the event packet_received says when there is a new one.
 *
 * Hopping goes over 433.92, 315, 868.35, 310, 318 and 390 MHz, 40 ms on
 * each, each with its own noise. Where a code is read (fck_subghz_decode())
 * the listen stays, and hops again 5 s after the last code read there -
 * and at once when that frequency's antenna leg cannot
 * be had (the GNSS receiver holding the module's bus). A capture with no
 * code is told where it was heard, and the listen hops on: a device
 * sending no code does not hold it, however often it sends. The Sub-GHz
 * app's listening. A listening started again starts afresh; one at a time. A call that
 * wants the CC1101 (fck_subghz_capture(), a send...) waits for the slice that runs, 200 ms
 * at most, and the listening goes on after it.
 *
 * It receives, never transmits, and is one of the settings the SDK puts back when the module
 * comes back (see FCK_SETTING_LISTEN).
 *
 * @return true; false for a frequency the CC1101 does not tune, with no module or no CC1101
 *         that answered, or before fck_begin().
 */
bool fck_subghz_listen_start(uint32_t hz);

/** @brief What fck_subghz_listen_read() heard. */
typedef struct {
    bool     heard;
    uint32_t hz;          /**< Where: hopping, the hop it was heard on, or
                           *   the one beside it, on the same antenna and
                           *   within 10 MHz, where it sounded 6 dB
                           *   louder - where the listen stays while codes
                           *   are read there.                             */
    int16_t  dbm;         /**< Its loudest level.                          */
    int16_t  noise_dbm;   /**< The channel's noise before it.              */
    uint32_t sequence;    /**< The capture's count, above 0 and moving with each one: pass it
                           *   as @p after to read only a newer one. 0 when @p heard is false. */
} fck_subghz_heard_t;

/**
 * @brief Blocks only if @p ms is above 0. The latest remote the listening heard, as
 *        fck_subghz_capture() records one, copied.
 *
 * @param after 0: the latest, if there is one. Else only a capture whose
 *              fck_subghz_heard_t.sequence is above it.
 * @param ms    0: do not wait. Else wait up to @p ms for one (cut within 20 ms by fck_cancel(),
 *              and by the listening ending).
 * @param us    The runs; the first @p cap when there are more (a capture keeps 512).
 * @param n     Runs written, on true; 0 when nothing newer was heard in @p ms.
 * @param heard Written on true; heard false with @p n 0.
 * @return true, also when nothing newer was heard; false before fck_subghz_listen_start() or
 *         after fck_subghz_listen_stop() or the module leaving, for @p ms over
 *         FCK_SUBGHZ_MS_MAX, a NULL or a @p cap of 0, or cancelled.
 *
 * fck_cancel() ends it: false, "cancelled by fck_cancel()" in fck_error().
 */
bool fck_subghz_listen_read(uint32_t after, uint32_t ms, int32_t *us, size_t cap, size_t *n,
                            fck_subghz_heard_t *heard);

/**
 * @brief Returns at once. The frequency the listening is set to: what fck_subghz_listen_start()
 *        was given.
 * @return The frequency in Hz; FCK_SUBGHZ_HOP (0) while it hops, and when no listening is in
 *         force. Safe from any task and from an Active Object.
 */
uint32_t fck_subghz_listen_hz(void);

/**
 * @brief Returns at once. Stop listening: what was learned is forgotten. The CC1101 is
 *        already asleep after the slice that runs.
 * @return true, also when nothing listened; false before fck_begin().
 */
bool fck_subghz_listen_stop(void);

/** How many frequencies fck_subghz_frequencies() gives. */
#define FCK_SUBGHZ_FREQUENCIES 38U

/**
 * @brief Returns at once. The frequencies remotes use, in Hz, low to high, in the CC1101's
 *        three bands: the 38 fck_subghz_find() listens on, for an app's list
 *        to choose one from. Plain C, no radio.
 * @param hz  Written on true; @p cap at least FCK_SUBGHZ_FREQUENCIES.
 * @param n   FCK_SUBGHZ_FREQUENCIES, on true.
 * @return true; false for a null argument or too small a @p cap.
 */
bool fck_subghz_frequencies(uint32_t *hz, size_t cap, size_t *n);

/** @brief What fck_subghz_find() heard. */
typedef struct {
    bool     found;
    uint32_t hz;          /**< Where it peaked with the narrow filter: on the
                           *   remotes' own frequency (433.92 MHz, 315...)
                           *   when within 40 kHz of one.                  */
    int16_t  dbm;         /**< Its level there.                            */
    int16_t  noise_dbm;   /**< What that part of the band read before.     */
} fck_subghz_hit_t;

/**
 * @brief Blocks. Listen for up to @p ms over the 38 frequencies remotes use, and say
 *        where a remote pressed meanwhile sends.
 *
 * The remote has to be kept pressed while it listens: the first three sweeps
 * (about 0.2 s) only learn each frequency's noise, and a remote is found when
 * it rises 10 dB over that noise twice within 1.5 s, or 25 dB once. A carrier
 * that is always there is learned as noise, not found.
 *
 * @param hit Written on true; found is false when nothing rose.
 * @return true; false for @p ms over FCK_SUBGHZ_MS_MAX and as
 *         fck_subghz_level(); false (an I/O failure) when the chip would not listen.
 *
 * fck_cancel() ends it: false, "cancelled by fck_cancel()" in fck_error().
 */
bool fck_subghz_find(uint32_t ms, fck_subghz_hit_t *hit);

/**
 * @brief Blocks. Listen on @p hz for up to @p ms, and record what the first remote
 *        heard sends, as the CC1101 demodulates on-off keying: us[i] > 0 is
 *        the carrier on for that many microseconds, < 0 off. The runs of a
 *        .ksub RAW file.
 *
 * A capture starts when the level rises 10 dB over the channel's noise, and
 * ends 60 ms after it has fallen back, or 0.5 s after it started: one press,
 * several frames. A click of fewer than 16 runs is not kept, and it listens
 * on.
 *
 * @param us The runs; the first @p cap when there are more (4097 at most).
 * @param n  Runs written, on true; 0 when nothing was heard in @p ms.
 * @return true; false as fck_subghz_find(); false (no memory)
 *         without 16 KB of internal RAM for the edges, taken for the call;
 *         false (an I/O failure) when the chip would not listen; false when
 *         GDO0's interrupt could not be had.
 *
 * fck_cancel() ends it: false, "cancelled by fck_cancel()" in fck_error().
 */
bool fck_subghz_capture(uint32_t hz, uint32_t ms, int32_t *us, size_t cap, size_t *n);

/**
 * @brief The fixed codes the SDK reads, writes and sends: a remote that
 *        sends the same bits at every press.
 *
 * The bits of each: 24 for EV1527 and PT2262, 12 for HT12E. Their timings are
 * the ones their chips' datasheets draw (fck_drv_ook.c names each).
 *
 * KeeLoq and Somfy are not here, on purpose: they are rolling codes, read by
 * fck_subghz_decode_rolling() and never written or sent.
 */
typedef enum {
    FCK_SUBGHZ_EV1527 = 0,
    FCK_SUBGHZ_PT2262 = 1,
    FCK_SUBGHZ_HT12E  = 2,
} fck_subghz_protocol_t;

/** @brief One code a capture carries. */
typedef struct {
    fck_subghz_protocol_t proto;
    uint8_t            bits;      /**< 12 or 24, as @c proto has it.        */
    uint32_t           key;       /**< As sent, its first bit highest.     */
    uint16_t           te_us;     /**< The base pulse it was sent with.    */
    uint8_t            repeats;   /**< Frames of the capture that carried it. */
} fck_subghz_code_t;

/**
 * @brief Returns at once. The codes a capture carries, the one most of its frames carry
 *        first; frames of the same protocol, bits and key are one code.
 *        EV1527, PT2262 and HT12E are read, timed as their datasheets draw them.
 *
 * One whole frame is enough, and the sync after it for a PT2262 or EV1527. An
 * HT12E sends a pilot (36 clocks, a low) before each word: a word is read
 * when a low of 15 clocks or more precedes its start pulse, so a capture's
 * first word is read when something quiet precedes it, and a word at the very
 * first run of @p us is not read.
 *
 * A code is a PT2262's when each pair of its bits is 00, 11 or 01 (an open
 * pin) and one at least is 01; a PT2262 with every pin tied reads as EV1527.
 * Plain C, no radio and no module: also for a capture an app kept on its SD.
 * Rolling codes (KeeLoq, Somfy) are not read here: fck_subghz_decode_rolling().
 *
 * @param out Up to @p cap codes, written on true.
 * @param got Codes written; 0 when no frame decoded.
 * @return true; false for a bad argument.
 */
bool fck_subghz_decode(const int32_t *us, size_t n, fck_subghz_code_t *out,
                                size_t cap, size_t *got);

/** The most runs one frame of a code takes: 24 bits and the two a frame adds
 *  - the sync of a PT2262 or EV1527, the start pulse and the pilot of an
 *  HT12E. */
#define FCK_SUBGHZ_FRAME_RUNS  50U
/** The most frames fck_subghz_encode() writes. */
#define FCK_SUBGHZ_FRAMES_MAX  16U
/** The base pulse an EV1527 or PT2262 code is read and written with; an
 *  HT12E's band is narrower, 150 to 449 us (its oscillator's 1 to 7 kHz is
 *  one clock of 143 to 1000 us, and a clock is a third of a bit). */
#define FCK_SUBGHZ_TE_MIN_US   100U
#define FCK_SUBGHZ_TE_MAX_US   1200U

/**
 * @brief Returns at once. A code as its remote sends it, in fck_subghz_capture()'s runs:
 *        @p frames times its bits, the first highest - what
 *        fck_subghz_decode() reads back. Plain C, no radio: to draw a remote
 *        an app keeps, or to write it as a file. Nothing here transmits.
 *
 * PT2262 and EV1527: bit 0 is te high and 3 te low, bit 1 3 te high and te
 * low, and the sync after the bits te high and 31 te low. HT12E: a start
 * pulse of te, then each bit as a low and a high - a 1 is 2 te low and te
 * high, a 0 te low and 2 te high - and the pilot, a low of 36 te, at the end
 * of each frame so that the first starts with its pulse. A capture that
 * holds one frame of an HT12E has no pilot before it: decode it after at
 * least one more frame, or after a quiet of the pilot's length.
 *
 * @param code   Its proto, bits and key (within its bits), and te_us (the
 *               band of FCK_SUBGHZ_TE_MIN_US's note); repeats is not read.
 * @param frames 1 to FCK_SUBGHZ_FRAMES_MAX.
 * @param us     The runs; @p cap at least @p frames * FCK_SUBGHZ_FRAME_RUNS
 *               always does.
 * @param n      Runs written, on true.
 * @return true; false for a bad argument, a count of bits the
 *         protocol does not send, a key over its bits, a te outside the
 *         protocol's band, a value of proto that is none of the enum's, or a
 *         PT2262 code with a pair of bits 10, which no PT2262 sends.
 */
bool fck_subghz_encode(const fck_subghz_code_t *code, uint8_t frames, int32_t *us,
                                size_t cap, size_t *n);

/** @brief What fck_subghz_protocol_info() says of a fixed-code protocol. */
typedef struct {
    const char *name;           /**< "EV1527", "PT2262" or "HT12E": to show. */
    const char *file_name;      /**< A .ksub code file's protocol line:
                                 *   "ev1527", "pt2262" or "ht12e".           */
    uint8_t     bits[2];        /**< The counts of bits it sends, the fewer
                                 *   first; bits[1] is 0 when it sends one. */
    uint16_t    te_us;          /**< The te its remotes typically send, from
                                 *   te_min_us to te_max_us.                */
    uint16_t    te_min_us;      /**< The te read and written, from this... */
    uint16_t    te_max_us;      /**< ...to this, both included.            */
} fck_subghz_protocol_info_t;

/**
 * @brief Returns at once. What a code of @p proto takes, for an app that lets one be written
 *        by hand: a code with one of @c bits, a key within them and a te from
 *        @c te_min_us to @c te_max_us is one fck_subghz_encode() writes and
 *        fck_subghz_send_code() sends - but a PT2262's, whose key must also
 *        be trits (each pair of bits 00, 11 or 01, one 01 at least), and
 *        reads back as an EV1527 otherwise. @c te_us is the te to start
 *        from. Plain C, no radio and no module.
 *
 * The windows: 100 to 1200 us for EV1527 and PT2262; 150 to 449 for HT12E.
 *
 * @param info Written on true.
 * @return true; false for a null @p info or a value of @p proto
 *         that is none of the enum's.
 */
bool fck_subghz_protocol_info(fck_subghz_protocol_t proto, fck_subghz_protocol_info_t *info);

/** @brief The rolling codes fck_subghz_decode_rolling() reads. They are not
 *         in fck_subghz_protocol_t: nothing here writes or sends them. */
typedef enum {
    FCK_SUBGHZ_KEELOQ      = 0,   /**< HCS200, HCS300...: 64 to 66 bits.  */
    FCK_SUBGHZ_SOMFY_RTS   = 1,   /**< Somfy RTS: 56 bits.                */
} fck_subghz_rolling_protocol_t;

/** @brief One rolling code a capture carries. */
typedef struct {
    fck_subghz_rolling_protocol_t proto;
    uint8_t   bits;           /**< Bits read: KeeLoq 64 to 66, Somfy 56.      */
    uint64_t  data;           /**< The bits as they were received, the first
     *                             highest: KeeLoq's first 64, Somfy's 56.   */
    uint32_t  serial;         /**< KeeLoq's 28 bits, Somfy's 24.              */
    uint8_t   button;         /**< The button, 4 bits.                        */
    uint32_t  hop;            /**< KeeLoq's encrypted half, as it came - never
     *                             decrypted; 0 for Somfy.                    */
    uint16_t  counter;        /**< Somfy's counter; 0 for KeeLoq, whose
     *                             counter is inside the hop.                 */
    bool      counter_read;   /**< @c counter is the remote's: Somfy.         */
    bool      battery_low;    /**< KeeLoq's VLOW bit, sent in the 65th bit.   */
    uint16_t  te_us;          /**< The base pulse, averaged over the frames.  */
    uint8_t   repeats;        /**< Frames of the capture that carried it.     */
} fck_subghz_rolling_t;

/**
 * @brief Returns at once. The rolling codes a capture carries - KeeLoq and Somfy RTS - the
 *        one most of its frames carry first, to show a remote's serial and
 *        button. READ ONLY: nothing in the SDK writes a rolling code as runs,
 *        keeps it as a file or transmits it, and the hop of a KeeLoq
 *        is shown as it came, never decrypted. A rolling code changes at
 *        every press; sending one back does not open what the remote opens.
 *
 * KeeLoq: a preamble of pulses of te (100 to 660 us), a header low of about
 * 10 te, and 64 to 66 bits sent least significant first, 32 of hop, 28 of
 * serial, 4 of button, then the battery. Somfy RTS: hardware sync
 * pulses of 4 te, a software sync of 7.5 te and 56 Manchester bits of te (480
 * to 730 us); a frame whose checksum is wrong is not read. A frame starts at
 * the capture's first run or after a low of 3 ms.
 *
 * Plain C, no radio and no module, like fck_subghz_decode().
 *
 * @param out Up to @p cap codes, written on true.
 * @param got Codes written; 0 when no frame decoded.
 * @return true; false for a bad argument.
 */
bool fck_subghz_decode_rolling(const int32_t *us, size_t n, fck_subghz_rolling_t *out, size_t cap,
                                        size_t *got);

/* ---- Sending: cloning a captured remote, emulating a decoded one ------------ */

/** The most times a signal is repeated in one transmission. */
#define FCK_SUBGHZ_SEND_REPEATS_MAX  32U

/** The most one send call keeps the carrier on, in milliseconds, all its
 *  repeats: the positive runs of fck_subghz_send_raw()'s signal, or the air
 *  time of fck_subghz_packet_send()'s packets. Checked before the PA is keyed. */
#define FCK_SUBGHZ_SEND_MS_MAX       3000U

/** @brief Returns at once. Whether the module may transmit on @p hz: inside 433.05-434.79 or
 *         868.0-868.6 MHz, the two European SRD bands it sends in.
 *         Receiving is legal wherever fck_subghz_level() reads; sending only
 *         here. For an app to grey out what it may not send.
 * @return true inside either band; false anywhere else, 0 included. */
bool fck_subghz_may_transmit(uint32_t hz);

/**
 * @brief Returns at once. Whether fck_subghz_send_raw() would refuse @p us as a rolling code:
 *        a KeeLoq or Somfy it decodes, or anything shaped like one (the rule
 *        and its cost are in fck_subghz_send_raw()'s note). The same test, so
 *        an app can mark a remote it cannot copy as soon as it hears or opens
 *        it, not when Send is pressed. Plain C, no radio and no module.
 * @return false for a null @p us or an @p n of 0.
 */
bool fck_subghz_is_rolling(const int32_t *us, size_t n);

/**
 * @brief Returns at once. How long the module may still be on air in @p hz's band, in
 *        milliseconds, within the last hour's limit: 360 000 ms in
 *        433.05-434.79 MHz and 36 000 ms in 868.0-868.6 MHz, less what the
 *        sends of the last hour have used. Every send checks it again: the
 *        figure is for an app to show, not to reserve.
 *
 * The account is kept in RAM, in the SDK's own statics: it starts empty each time the app
 * starts (an app closed and opened again, or the P4 restarting, begins with a whole hour
 * to spend), and it survives fck_end() and fck_begin() within one run. The limit is the
 * module's, and the module does not remember it either: an app that keeps the hour across runs
 * keeps its own record. It reads a copy of the account and waits for nothing, so an Active Object
 * may ask it (fck_sys_subghz_air_time_left()).
 *
 * @param ms Written on true.
 * @return true; false for a @p hz outside both bands
 *         (fck_subghz_may_transmit()), a null @p ms, as fck_subghz_level()
 *         with no module, and when a send was writing the account at that
 *         moment ("the air time's account was being written: ask again" in
 *         fck_error()): it does not wait, so ask again.
 */
bool fck_subghz_air_time_left(uint32_t hz, uint32_t *ms);

/**
 * @brief Blocks. TRANSMITS @p n raw OOK runs (fck_subghz_capture()'s, signed
 *        microseconds) on @p hz, @p repeats times: cloning a remote just as
 *        it was heard. The carrier is keyed in asynchronous serial mode,
 *        each band at its power and through its antenna leg (fck_bsp.h):
 *        433 at the CC1101's +10 dBm setting
 *        through the middle leg, with its ladder filter, and 868 at a lower
 *        one through the bottom leg. One transmission at a time, a job of
 *        the owner's.
 *
 * Refused when the capture reads as a rolling code: a KeeLoq-like preamble
 * and header with 40 or more bits' runs after it, a Somfy-like sync with 30
 * or more after it, or more than 60 runs in a row, between pulses of 6 te or
 * more or a low of 3 ms, nearly all a short pulse or a long one, 1.55 to 3.75
 * times the short - what every rolling code of that length looks
 * like, read or not, so a capture that is damaged or cut is refused too. It does
 * not need to decode. THE COST: the fixed codes of more than 30 bits, a fixed
 * remote that sends its frames 6 te apart or closer, and a Manchester remote
 * of Somfy's timing are refused as well and cannot be cloned until they have a
 * decoder. Every fixed code this SDK reads (24 bits at most) is cloned.
 * @param us      The runs; none 0, each at most 1 s; the first
 *                4096 of them at most (what one capture holds).
 * @param n       1 or more.
 * @param repeats 1 to FCK_SUBGHZ_SEND_REPEATS_MAX.
 * @return true; false when the module may not transmit on @p hz
 *         (fck_subghz_may_transmit()), for a bad argument, a run of 0 or too
 *         long, a capture that reads as a rolling code (KeeLoq, Somfy and
 *         anything shaped like one, above) - refused, nothing transmitted -, the
 *         leg cannot be had, when the carrier would be on for more
 *         than FCK_SUBGHZ_SEND_MS_MAX (the positive runs times @p repeats),
 *         or when the signal's whole length times @p repeats is more than
 *         fck_subghz_air_time_left() says;
 *         false (no memory) without internal RAM;
 *         false/false (a timeout)/false (an I/O failure) as the chip answers.
 *
 * fck_cancel() ends it: false, "cancelled by fck_cancel()" in fck_error().
 */
bool fck_subghz_send_raw(uint32_t hz, const int32_t *us, size_t n, uint8_t repeats);

/**
 * @brief Blocks. TRANSMITS @p code (fck_subghz_decode()'s) on @p hz as @p frames
 *        frames: emulating a remote from the code read off it, of any of
 *        fck_subghz_protocol_t's fixed codes. The runs are
 *        fck_subghz_encode()'s, sent once. A rolling code (KeeLoq, Somfy) has
 *        no value there: nothing in the SDK sends one.
 * @param code   proto, bits, key and te_us as fck_subghz_encode() takes them.
 * @param frames 1 to FCK_SUBGHZ_FRAMES_MAX.
 * @return true; false as fck_subghz_send_raw(), and for a code
 *         fck_subghz_encode() refuses (a PT2262 pair 10, a count of bits the
 *         protocol does not send), and one whose frames keep the carrier on
 *         for more than FCK_SUBGHZ_SEND_MS_MAX; the rest as it.
 *
 * fck_cancel() ends it: false, "cancelled by fck_cancel()" in fck_error().
 */
bool fck_subghz_send_code(uint32_t hz, const fck_subghz_code_t *code, uint8_t frames);

/** What one packet carries at most, and the most one call receives. */
#define FCK_SUBGHZ_PACKET_MAX   61U
#define FCK_SUBGHZ_PACKETS_MAX  32U

/** @brief How a packet's bits are sent. */
typedef enum {
    FCK_SUBGHZ_2FSK = 0,
    FCK_SUBGHZ_GFSK = 1,
} fck_subghz_modulation_t;

/**
 * @brief A packet receiver: what the far end sends with. Each number is
 *        taken to the nearest the CC1101 has; the filter, to the narrowest
 *        not narrower than asked. What examples/subghz receives with:
 *        { FCK_SUBGHZ_GFSK, 38400, 20630, 101000, 0xD391, 8, true, true }.
 */
typedef struct {
    fck_subghz_modulation_t modulation;
    uint32_t                baud;           /**< 600 to 250 000.            */
    uint32_t                deviation_hz;   /**< 1 586 to 380 859.          */
    uint32_t                filter_hz;      /**< The channel filter, up to
                                             *   812 500; 58 035 at least.  */
    uint16_t                sync_word;      /**< Highest byte first; 30 of
                                             *   its 32 bits must match.    */
    uint8_t                 length;         /**< Fixed, 1 to
                                             *   FCK_SUBGHZ_PACKET_MAX.     */
    bool                    crc;            /**< CRC-16 over the payload.   */
    bool                    whitening;      /**< PN9 data whitening.        */
} fck_subghz_packet_settings_t;

/** @brief One packet received. */
typedef struct {
    uint8_t bytes[FCK_SUBGHZ_PACKET_MAX];
    uint8_t length;
    int16_t dbm;                  /**< At its sync word.                   */
    uint8_t link_quality;         /**< 0 to 127: lower is cleaner.         */
    bool    crc_ok;               /**< false too when crc is off.          */
    int32_t frequency_error_hz;   /**< Its carrier against @p hz.          */
} fck_subghz_packet_t;

/**
 * @brief Blocks. Receive packets on @p hz for up to @p ms, or until @p cap of them:
 *        2-FSK or GFSK, a sync word, a fixed length, as @p settings say.
 *        Only receives: the CC1101's transmitter is never started.
 *
 * A packet whose CRC fails is kept, with crc_ok false, so that an app can
 * count them; a sync word found in noise and cut short is not a packet.
 *
 * @param packets Written on true, the first @p got of them.
 * @param cap     1 to FCK_SUBGHZ_PACKETS_MAX.
 * @return true; false for settings the CC1101 cannot be set to, a
 *         bad argument, @p ms over FCK_SUBGHZ_MS_MAX and as
 *         fck_subghz_level(); false (no memory) without memory for @p cap
 *         packets; false (an I/O failure) when the chip would not receive.
 *
 * fck_cancel() ends it: false, "cancelled by fck_cancel()" in fck_error().
 */
bool fck_subghz_packet_receive(uint32_t hz, const fck_subghz_packet_settings_t *settings,
                                        uint32_t ms, fck_subghz_packet_t *packets, size_t cap,
                                        size_t *got);

/**
 * @brief Blocks. TRANSMITS @p bytes as one packet, @p repeats times, that
 *        fck_subghz_packet_receive() with the same @p settings receives:
 *        2-FSK or GFSK, 4 bytes of preamble, the sync word, @p settings'
 *        fixed length, a CRC-16 and PN9 whitening if @p settings say so.
 *        Only 2-FSK and GFSK, the module's modulations besides OOK;
 *        at the same power as OOK's on level on that band. The repeats are a
 *        few milliseconds apart. One transmission at a time, a job of the
 *        owner's. It is for an app's own packets, not for sending again what
 *        fck_subghz_packet_receive() heard from another device.
 *
 * The packet must stay inside the sub-band: by Carson's rule it occupies
 * twice @p settings' deviation plus its baud rate, both as the chip gets
 * them, and all of that centred on @p hz must lie inside 433.05-434.79 or
 * 868.0-868.6 MHz. The settings of examples/subghz do, 80 kHz wide.
 *
 * @param bytes   @p length of them.
 * @param length  @p settings' length: 1 to FCK_SUBGHZ_PACKET_MAX.
 * @param repeats 1 to FCK_SUBGHZ_SEND_REPEATS_MAX.
 * @return true; false when the module may not transmit on @p hz
 *         (fck_subghz_may_transmit()), when the packet's occupied bandwidth
 *         leaves the sub-band, for settings the CC1101 cannot be set to, a
 *         bad argument or a @p length that is not @p settings', the leg
 *         cannot be had, the packets' air time times @p repeats is more than
 *         FCK_SUBGHZ_SEND_MS_MAX, or the same is more than
 *         fck_subghz_air_time_left() says; false/false (a timeout)/
 *         false (an I/O failure) as the chip answers (a packet that did not end in
 *         the time it needs is a false (a timeout)).
 *
 * fck_cancel() ends it: false, "cancelled by fck_cancel()" in fck_error().
 */
bool fck_subghz_packet_send(uint32_t hz, const fck_subghz_packet_settings_t *settings,
                                     const uint8_t *bytes, uint8_t length, uint8_t repeats);

/** Bytes that always hold fck_subghz_to_file()'s text of @p n runs,
 *  its NUL included. */
#define FCK_SUBGHZ_FILE_RAW_MAX(n)  (136U + 13U * (n))
/** Bytes that always hold fck_subghz_code_to_file()'s text. */
#define FCK_SUBGHZ_FILE_CODE_MAX    192U

/**
 * @brief Returns at once. A capture as a Kode .ksub RAW file: the text, NUL-terminated,
 *        that the app keeps on the SD card with kode_fs. Plain C, no radio, no file.
 *
 * The format is Kode's own, in docs/ksub-format.md: `kode-subghz 1`, a kind
 * (raw), the frequency, the modulation (ook), the number of pulses and the
 * runs on `raw:` lines, 512 a line.
 *
 * @param hz     Where it was heard.
 * @param us     fck_subghz_capture()'s runs; none of them 0.
 * @param text   @p cap at least FCK_SUBGHZ_FILE_RAW_MAX(n) always does.
 * @param length Its length, the NUL not counted, on true.
 * @return true; false for a null argument, no runs, a run of 0, a
 *         frequency of 0 or too small a @p cap.
 */
bool fck_subghz_to_file(uint32_t hz, const int32_t *us, size_t n, char *text, size_t cap,
                                         size_t *length);

/**
 * @brief Returns at once. A code as a .ksub code file: its protocol's name in lowercase
 *        ("ev1527", "pt2262" or "ht12e"), its bits, the code in hexadecimal and its
 *        pulse in microseconds.
 * @param code   proto and bits as fck_subghz_encode() takes them, a key within
 *               its bits and a te_us in its protocol's band; repeats is not
 *               written.
 * @param text   @p cap FCK_SUBGHZ_FILE_CODE_MAX always does.
 * @param length Its length, the NUL not counted, on true.
 * @return true; false for a null argument, a proto or a count of bits
 *         that is not one of the enum's, a key over its bits, a te outside
 *         its band, a frequency of 0 or too small a @p cap.
 */
bool fck_subghz_code_to_file(uint32_t hz, const fck_subghz_code_t *code, char *text,
                                              size_t cap, size_t *length);

/** @brief What fck_subghz_from_file() read. */
typedef struct {
    uint32_t          hz;
    bool              is_code;   /**< A code file: @c code holds it. Else RAW. */
    fck_subghz_code_t code;      /**< proto as fck_subghz_decode() would say,
                                  *   repeats 0; te_us the file's pulse.       */
    bool              cut;       /**< RAW: more runs than @p cap; the first
                                  *   @p cap kept.                           */
} fck_subghz_file_t;

/**
 * @brief Returns at once. Read a .ksub file, RAW or a code file of one of
 *        fck_subghz_code_to_file()'s protocols, from the first
 *        @p length bytes of @p text: no NUL needed, as a file read from the
 *        SD card into a buffer has none. Plain C, no radio, no file.
 *
 * A code file must have a pulse, within its protocol's band
 * (fck_subghz_protocol_info()). A code file of any other protocol (KeeLoq
 * among them) is refused. The reader's rules - what it skips and what it
 * refuses - are in docs/ksub-format.md.
 *
 * @param file Written on true.
 * @param us   A RAW file's runs, its first @p cap; may be NULL with @p cap 0
 *             for a code file. Written on true alone.
 * @param n    The runs the file has, on true; 0 for a code file.
 * @return true; false for a null argument and for a file refused: not a
 *         .ksub file, broken or contradicting itself, a protocol that is none
 *         of the above, a count of bits it does not send, a code over its
 *         bits, a pulse that is missing or outside the protocol's band, no
 *         runs, a run of 0, or a number that does not parse.
 */
bool fck_subghz_from_file(const char *text, size_t length, fck_subghz_file_t *file,
                                           int32_t *us, size_t cap, size_t *n);

#ifdef __cplusplus
}
#endif

#endif /* FCK_H_ */
