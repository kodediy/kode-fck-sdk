/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file fck_aos.h
 * @brief The module's owner: one Active Object, AO_Fck, that holds the F*CK
 *        and does everything that touches it.
 *
 * Not an app's header: the standard layer (fck.h), the extended one (fck_sys.h)
 * and the chips' clients call this. An app calls fck_*() from a task, fck_sys_*()
 * from an Active Object. fck_aos_command() and fck_aos_command_post() hand a line
 * to a chip without the gates fck_command() keeps, so this header compiles only
 * under FCK_AOS_ALLOWED, which `fck`, `fck_sys` and `fck_aos` define for their own
 * sources and tools/check_layers.sh (rule 15) keeps to those three and the tests.
 *
 * One owner, the way kode-sdk's own devices have one: an Active Object on
 * kode-sdk's QP/C (fck_ao.c says at which level, and why), and a worker of
 * its own, pinned to core 1 (fck_drv_tca6408a.h says why), for what waits.
 * It borrows J3 once, with kode_sys_exp_lend() under FCK_BSP_LOAN_TAG, takes its
 * controllers under the same tag with kode_sys_exp_ctrl(), switches the module's
 * supply on with kode_sys_exp_power(true) (an answer comes as EXP_POWER_SIG, and the
 * owner does not wait for it: with the header empty it keeps looking for the module), and keeps them. With the header empty
 * it looks for a module every 300 ms, on the worker; with an F*CK on it, it
 * reads both chips' UARTs every FCK_AO_TICK_MS, carries requests out one at
 * a time, and asks the expander once a second whether the module is still
 * there and never lost power.
 *
 * What it takes from the image, so that it lives beside kode-sdk and kodeOS:
 * J3's 14 positions; from kode-sdk's books, I2C1, SPI3 (open only while the
 * CC1101 is) and the two lowest free UARTs; a QP/C priority, its queue and
 * an event per request; one of ESP-IDF's five restart hooks; ESP-IDF's GPIO
 * ISR service, with one handler on the CC1101's GDO0 while a capture stamps
 * its edges. Nothing before kode_init(): its J3 setup hands every loan back.
 *
 * Finding a module, in this order, because each step protects the next. Until
 * step 3 has said it is an F*CK's expander, nothing is written to the module
 * and no line of it is driven, but for the I2C pair by the bus itself, an
 * address and a stop at a time: a module of another kind may be on the header,
 * and a chip that is not the expander may answer at 0x21.
 *  1. The I2C pair is held high (fck_drv_pins_pair_held()): something is there.
 *     Both lines are only read, against their own pull-downs.
 *  2. One probe at 0x21: an address and a stop, no register written. A clean NAK
 *     is not an F*CK, and nothing on it is ever driven.
 *  3. The chip that answered is read once, at its INPUT register: the one
 *     transaction a register-less chip (a PCF8574, which takes the register
 *     byte for its port) turns into a write. Five of the expander's pins are
 *     held high by 10 k resistors on the module (FCK_BSP_EXPANDER_PULLUPS_MASK; P7
 *     is too, but its polarity bit may be set), so a chip that reads none of
 *     them high is not the expander: it echoes that byte, or its pins are held
 *     low. A chip that keeps registers the same way is not told apart by
 *     anything on the bus, and passes.
 *  4. The first thing written to it is the power-loss mark, in POLARITY (P7's
 *     bit), read back. A chip that does not keep it is no expander, and it is
 *     left alone.
 *  5. The GNSS receiver goes to reset, before its standard firmware takes SDA,
 *     about 420 ms after it powers up. When it already has, it goes to reset
 *     by hand (fck_drv_tca6408a_blind_gnss_reset()), and the bus is asked for up to a
 *     second, since it takes up to 500 ms to come back.
 *  6. SW0 is driven to its rest (fck_bsp.h: high, the level of 433's leg and
 *     of 868 and 915's) on P3 and read back, so the antenna switches never
 *     float.
 *  7. The CC1101 is reset - it has no reset line, and a firmware that died
 *     may have left a carrier on - and powered down (SPWD).
 *  8. The two UARTs open. The C5 and the nRF keep running.
 *
 * A module whose receiver already holds SDA when it is first looked at gets
 * no further than step 2: a Mesh module, whose receiver does the same, holds
 * the same pair, and the two cannot be told apart through a dead bus, so nothing
 * is driven for either. The F*CK is found when it is plugged in (the first
 * look comes within 300 ms of the contact, well inside the 420 ms), or after
 * its receiver is put in reset some other way. One that was identified, and
 * whose pair has been held since, is the same F*CK: its bus is rescued by hand
 * if the receiver takes it again.
 *
 * What costs energy and nobody asked for is off: the GNSS receiver stays in
 * reset until fck_aos_gnss_power(true), the CC1101 sleeps until its driver opens
 * it. Whether the C5 and the nRF should sleep too is for the consumption
 * study to say; fck_aos_power() is how they are put down.
 *
 * Going home. kodeOS knows nothing about the module, its supply may stay on,
 * and a restart leaves the expander's outputs, the CC1101 and both chips as
 * they were (fck_drv_system.h). So when the app restarts on purpose - kodeOS's ways
 * home, kode_exit_to_os(), esp_restart() - the owner, before the P4 goes,
 * puts the GNSS receiver back in reset and the CC1101 down, takes the nRF
 * through a reset (or, with the expander out of reach, tells it `rf off`),
 * and holds the C5's EN low until kodeOS's kode_init() lets it go. Both
 * chips come back as from power-on, with their radios off, and from then on
 * nothing asked of the owner is done. An app started from a mailbox (kodeOS's
 * ALWAYS and ARMED) restarts into itself rather than kodeOS, and finds the
 * module again.
 *
 * Resting. With CONFIG_KODE_LOWPOWER the board can rest (ASLEEP, OFF) and wake: on
 * the round for it the owner puts the module down as for a way home - what is on the
 * air is cut, the GNSS receiver in reset, the CC1101 asleep, the nRF reset, the C5 held
 * - and tells the app it left; on AWAKE (which repeats: a round may be abandoned) it
 * looks for the module again, sets it up and gives back what the app had set that does
 * not emit (the receiver, the listening). Nothing is armed on ASLEEP, and nothing
 * emits, moves or records by itself when the board wakes. J3's supply stays on in a
 * rest (R72 holds it): the chips are what is put in their lowest consumption.
 *
 * What runs no hook leaves the module as it was, transmitting included: a
 * panic, a watchdog, a brownout, and the resets from outside the CPU - the
 * host opening the USB serial port, the bottom and left buttons together.
 * The next app that opens the module sets it up again.
 *
 * Every call here but fck_aos_present(), fck_aos_status(), the listening's
 * settings and fck_aos_news() BLOCKS its caller until the owner has answered:
 * NEVER from an Active Object (kqp_current_is_ao()), the owner's own included,
 * nor from a job of fck_aos_run_job(). Neither layer's Active Object code calls
 * them: fck_sys.h's requests are carried out by a task of fck_sys's own. A call may
 * also return KODE_NO_RESOURCES when kode-sdk's event pools or the owner's queue
 * are full.
 */
#ifndef FCK_AOS_H_
#define FCK_AOS_H_

#ifndef FCK_AOS_ALLOWED
#error "fck_aos.h is the SDK's own: an app includes fck.h (tasks) or fck_sys.h (Active Objects)"
#endif

#include <stdbool.h>
#include <stdint.h>

#include "kode_defs.h"

#include "fck_drv_cc1101.h"
#include "fck_drv_pins.h"
#include "fck_drv_link.h"
#include "fck_drv_nmea.h"
#include "fck_drv_tca6408a.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief The module's two chips that run firmware. */
typedef enum {
    FCK_AOS_C5 = 0,
    FCK_AOS_NRF,
    FCK_AOS_CHIPS
} fck_aos_chip_t;

/** @brief What the owner knows about the module, for fck_status() and logs. */
typedef struct {
    bool     present;
    uint32_t generation;    /**< Modules set up since start: a new number is a
                             *   module plugged in, or one that lost power.  */
    bool     cc1101_ok;     /**< A CC1101 answered and was powered down.     */
    uint8_t  cc1101_part;
    uint8_t  cc1101_version;
    bool     gnss_running;  /**< Out of reset (fck_aos_gnss_power(true)).         */
    bool     gnss_holds_bus;/**< Running, and its firmware took SDA: the
                             *   expander cannot be reached until it stops. */
    bool     chip_on[FCK_AOS_CHIPS];
    uint8_t  j3_supply;     /**< FCK_AOS_J3_*: what J3's +3V3_EXT switch came to.  */
    bool     listening;     /**< A sub-GHz listening is in force and the module is there. */
    bool     started;       /**< fck_aos_start() has run and fck_aos_stop() has not.      */
    uint32_t events_dropped;/**< Events the standard layer's queue had no room for.   */
    uint32_t news_dropped;  /**< News the extended layer could not post to a subscriber. */
    uint32_t answers_dropped; /**< Answers the extended layer could not post to the asker. */
} fck_aos_status_t;

/** @brief fck_aos_status_t.j3_supply: J3's supply, as the owner asked and EXP_POWER_SIG answered. */
#define FCK_AOS_J3_UNKNOWN  0U  /**< Not asked yet, or the owner is idle.                          */
#define FCK_AOS_J3_ASKED    1U  /**< kode_sys_exp_power(true) was posted; the rail follows later.  */
#define FCK_AOS_J3_ON       2U  /**< EXP_POWER_SIG said KODE_OK for on.                            */
#define FCK_AOS_J3_NO_LINK  3U  /**< The Dot's C5 link did not come up: the wish stays, and the next link carries it out. */
#define FCK_AOS_J3_FAILED   4U  /**< The C5 refused (KODE_IO_ERROR): the wish was dropped.         */
#define FCK_AOS_J3_ALWAYS   5U  /**< No radio in the image: J3 is always powered and cannot be switched. */
#define FCK_AOS_J3_OFF      6U  /**< Switched off by fck_aos_stop().                               */

/** The longest any wait of a call is, in milliseconds (ten minutes): a longer one is cut to it. */
#define FCK_AOS_CALL_MS_MAX (10U * 60U * 1000U)

/** The longest banner kept, its terminator included. Both firmwares keep
 *  theirs within it: fck-nrf's build fails if its #NRFINFO could pass it, and
 *  fck-c5 leaves the last keys of its #C5INFO out rather than pass it. */
#define FCK_AOS_FIRMWARE_MAX 192U

/** @brief What a chip said it runs: the banner it prints when it starts,
 *         and answers `info` with. */
typedef struct {
    bool     known;   /**< It has said, since it last started.             */
    uint32_t seq;     /**< Changes each time it says, and each time it is
                       *   reset, powered, handed to a job (fck_aos_run_job())
                       *   or set up again: what
                       *   fck_aos_command_if_same_firmware() checks.          */
    char     body[FCK_AOS_FIRMWARE_MAX]; /**< The #C5INFO or #NRFINFO after its
                       *   tag, for fck_drv_c5_parse_info() or
                       *   fck_drv_nrf_parse_info(); "" for one too long to
                       *   keep whole.                                      */
} fck_aos_firmware_t;

/** @brief What a job sees: the module's lines, bus and UARTs, all its own
 *         for as long as it runs. */
typedef struct {
    const fck_drv_pins_t *hw;
    fck_drv_tca6408a_t      *tca;      /**< NULL while the GNSS holds the bus.        */
    int             uart[FCK_AOS_CHIPS];  /**< The ports the owner opened for
                                           *   each chip, -1 for none.      */
    bool           *chip_on;  /**< [FCK_AOS_CHIPS] Each chip as the job found
                               *   it: out of reset, or held in it. A job
                               *   that leaves one otherwise writes it here,
                               *   and the owner takes it from there: a C5
                               *   written through its ROM is left running,
                               *   whether it was on before or not.        */
} fck_aos_job_context_t;

/** @brief Work done on the owner's worker, with the module to itself. */
typedef kode_status_t (*fck_aos_job_fn)(void *arg, const fck_aos_job_context_t *job);

/** @brief What happened, for the sinks below (fck.h's events, fck_sys.h's news). */
typedef enum {
    FCK_AOS_EVENT_PRESENT      = 1,   /**< The module was found and set up; arg 0.    */
    FCK_AOS_EVENT_GONE         = 2,   /**< It left J3, or lost power; arg 0.          */
    FCK_AOS_EVENT_WRONG_MODULE = 3,   /**< Not an F*CK on J3; arg a FCK_AOS_SEEN_*.   */
    FCK_AOS_EVENT_SETTING_LOST     = 4,   /**< What the app had set went with the module; arg FCK_AOS_SETTING_*. */
    FCK_AOS_EVENT_SETTING_RESTORED = 5,   /**< Some of it was set again; arg FCK_AOS_SETTING_*.                 */
    FCK_AOS_EVENT_PACKET_RECEIVED  = 6,   /**< The sub-GHz listening heard a remote; arg its count (fck_aos_listen_seq()); read it with fck_aos_listen_last(). */
    FCK_AOS_EVENT_SEND_CANCELLED   = 7,   /**< A send was cut; arg FCK_AOS_CANCEL_*.                           */
} fck_aos_event_t;

/** @brief FCK_AOS_EVENT_SEND_CANCELLED's arg: what cut it. */
#define FCK_AOS_CANCEL_CALL     1UL  /**< fck_cancel().                                             */
#define FCK_AOS_CANCEL_LEAVING  2UL  /**< The way home, a rest, a restart or fck_end().              */

/** @brief The settings of an app that the module forgets when it leaves or loses power. */
#define FCK_AOS_SETTING_GNSS      1UL  /**< The GNSS receiver was running.                         */
#define FCK_AOS_SETTING_LISTEN    2UL  /**< A sub-GHz listening was in force (fck_aos_setting_listen()). */
#define FCK_AOS_SETTING_C5_POWER  4UL  /**< The app had switched the C5 off (it is on again, as after any setup). */
#define FCK_AOS_SETTING_NRF_POWER 8UL  /**< The same for the nRF.                            */

/** @brief FCK_AOS_EVENT_WRONG_MODULE's arg: what the owner made of it. */
#define FCK_AOS_SEEN_HOLDS_PAIR  1UL  /**< Something holds the I2C pair, nothing answers at the expander's address. */
#define FCK_AOS_SEEN_OTHER_CHIP  2UL  /**< A chip answers at the expander's address but is not the expander.        */

/** @brief Who is told what happens: the standard layer's events and the extended layer's news. */
typedef enum {
    FCK_AOS_SINK_EVENTS = 0,   /**< fck.h: the listeners' queue.                         */
    FCK_AOS_SINK_NEWS   = 1,   /**< fck_sys.h: the subscribers' Active Objects.           */
    FCK_AOS_SINK_ANSWERS = 2,  /**< fck_sys.h: the answers, to the asking Active Objects. No sink
                                *   function: only the count, fck_aos_dropped_add().        */
    FCK_AOS_SINKS
} fck_aos_sink_t;

/**
 * @brief A sink: told on the task that raised the event (the owner's, or the call's that
 *        cut a send), so short and never blocking.
 * @return true when it was taken; false when it had no room - the owner counts the drop
 *         (fck_aos_status_t.events_dropped, .news_dropped, .answers_dropped).
 */
typedef bool (*fck_aos_sink_fn)(fck_aos_event_t ev, uint32_t arg, void *ctx);

/**
 * @brief Set who is told for @p which; NULL for nobody. Set once by each layer's begin and
 *        kept: the owner calls it and no layer sets it again while one may be running.
 */
void fck_aos_sink_set(fck_aos_sink_t which, fck_aos_sink_fn fn, void *ctx);

/**
 * @brief Say that @p ev happened, to every sink, from any task. Never blocks.
 */
void fck_aos_news(fck_aos_event_t ev, uint32_t arg);

/** @brief Count @p n more losses for @p which (an answer the extended layer could not post). Never blocks. */
void fck_aos_dropped_add(fck_aos_sink_t which, uint32_t n);

/** @brief Why a call failed, in the classes fck.h's table of causes has (fck_sys.h's fck_why_t). */
typedef enum {
    FCK_AOS_WHY_NONE = 0,
    FCK_AOS_WHY_NOT_BEGUN,
    FCK_AOS_WHY_LEAVING,
    FCK_AOS_WHY_NO_MODULE,
    FCK_AOS_WHY_CHIP_HELD,
    FCK_AOS_WHY_CHIP_NOT_READY,
    FCK_AOS_WHY_BAD_ARGUMENT,
    FCK_AOS_WHY_BEYOND_LIMITS,
    FCK_AOS_WHY_FIRMWARE_TOO_OLD,
    FCK_AOS_WHY_WRONG_ORDER,
    FCK_AOS_WHY_CANCELLED,
    FCK_AOS_WHY_BUSY,
    FCK_AOS_WHY_TIMEOUT,
    FCK_AOS_WHY_IO,
    FCK_AOS_WHY_REFUSED,
    FCK_AOS_WHY_AO_CALLER,
} fck_aos_why_t;

/** @brief The room a composed reason takes per task, its terminator included. */
#define FCK_AOS_ERROR_TEXT 128U

/**
 * @brief Whether the calling task is an Active Object's: then say so for fck_error() ("<call_name> was
 *        called from an Active Object (<name>); fck.h calls block - call fck_sys_*() instead") and
 *        return true. The standard layer's gate. Never blocks.
 */
bool fck_aos_refuse_ao(const char *call_name);

/** @brief The class of the calling task's last failure; FCK_AOS_WHY_NONE when none. Never blocks. */
fck_aos_why_t fck_aos_error_why(void);

/** @brief As fck_aos_error_set(), for a call that ended with @p st: its class comes from @p st and the text. */
void fck_aos_error_set_for(kode_status_t st, const char *why);

/**
 * @brief The sub-GHz listening's latest capture (fck_aos_setting_listen() turned it on): the
 *        owner listens in slices on its worker while it is in force and nothing else uses the
 *        CC1101, and keeps the latest remote it heard.
 *
 * @param after  0: the latest, if there is one. Else: only a capture whose count is above it.
 * @param ms     0: do not wait. Else wait up to @p ms for a capture above @p after (cut within
 *               20 ms by fck_aos_cancel(), and by the listening ending).
 * @param us     Out: the runs, the first @p cap; may be NULL with @p cap 0.
 * @param n      Out: runs written (a capture holds FCK_AOS_LISTEN_RUNS at most).
 * @param heard  Out: where and how loud.
 * @param seq    Out: the capture's count, above 0 and moving with each one.
 * @return KODE_OK a capture was copied; KODE_TIMEOUT none above @p after (in @p ms);
 *         KODE_DENIED when no listening is in force or the owner is not started, or cancelled.
 *         Waits only when @p ms is above 0: then NEVER from an Active Object.
 */
kode_status_t fck_aos_listen_last(uint32_t after, uint32_t ms, int32_t *us, size_t cap, size_t *n,
                                  fck_drv_cc1101_heard_t *heard, uint32_t *seq);

/** @brief The most runs one capture of the listening keeps. */
#define FCK_AOS_LISTEN_RUNS 512U

/** @brief The count of the latest capture the listening kept, 0 for none. Never blocks. */
uint32_t fck_aos_listen_seq(void);

/** @brief The posted lines that wait for one chip at most, besides the one it has. */
#define FCK_AOS_POSTED_MAX 4U

/**
 * @brief Post a line to a chip's firmware and do not wait for its answer. For a command with nothing
 *        to answer (fck_sys_radio_sniff_tune()).
 *
 * The owner writes it when the chip is free: a line that finds a command in flight on its chip
 * waits its turn, up to FCK_AOS_POSTED_MAX of them, and they are sent in the order they came. Nobody
 * is told how it went. A line that is not sent (the chip is off, the app is leaving, the module
 * left, or more lines wait than the owner keeps), or that the firmware answers with a failure,
 * leaves a warning in the log.
 *
 * @return KODE_OK posted; KODE_DENIED before fck_aos_start(), with a bad chip or line, or no module;
 *         KODE_NO_RESOURCES when the owner's queue or pool had no room (nothing was posted).
 *         Never blocks; safe from an Active Object.
 */
kode_status_t fck_aos_command_post(fck_aos_chip_t chip, const char *line, const char *verb, uint32_t ms);

/**
 * @brief Borrow J3 and its controllers, start AO_Fck and have it ask for the module's supply
 *        (kode_sys_exp_power(true), answered by EXP_POWER_SIG, which it subscribed to first).
 *        Idempotent. After kode_init(). Returns once the owner runs: not when the module is
 *        found or the supply is on (fck_aos_status_t.j3_supply says where that is).
 *
 *        From an Active Object it never waits: a start under way in another task answers
 *        KODE_DENIED at once.
 * @return KODE_OK; KODE_EXISTS when the console took J3 by hand (an
 *         `exp` typed before this call); KODE_DENIED when another loan in
 *         the image holds some of its positions; KODE_NO_RESOURCES when
 *         kode-sdk has no I2C, SPI or UART left for J3, or the Active Object
 *         or its worker could not start; KODE_DENIED also when kode_init() has not run.
 */
kode_status_t fck_aos_start(void);

/**
 * @brief Put the module down and let go of it: whatever is on the air is cut,
 *        the GNSS receiver goes to reset, the CC1101 to sleep, both chips are
 *        held, the UARTs and the bus close, J3's supply is switched off and
 *        the loan is given back. The owner idles until fck_aos_start().
 *
 * Blocks until the owner has done it. What was asked of the module and not
 * finished is answered KODE_IO_ERROR (a command) or KODE_DENIED. The Active
 * Object and its worker stay: QP/C has no way to give them back.
 *
 * @return KODE_OK, also when it was not started; KODE_DENIED from the owner's
 *         own task or its worker, or while a start is in progress;
 *         KODE_NO_RESOURCES when the owner's queue or pool had no room for
 *         the request (nothing was done; try again).
 */
kode_status_t fck_aos_stop(void);

/**
 * @brief Cancel what is waiting or running: fck_cancel().
 *
 * Every blocking call that began before it ends at once or within a step:
 * a wait for the owner, for a chip's lock, for the module or for a fix is
 * cut within 20 ms (KODE_DENIED); a command in flight is let go of (the chip
 * finishes it, and nothing new is sent to it until it has); a job on the
 * worker (a capture, a listening, a spectrum, a send) is told to stop at its
 * next step, a few milliseconds, and ends with the status it has. A call
 * begun after this one is not touched.
 *
 * @return KODE_OK; KODE_DENIED before fck_aos_start() or after fck_aos_stop().
 */
kode_status_t fck_aos_cancel(void);

/** @brief How many tasks' last reasons fck_error() keeps (the owner's and the worker's count). */
#define FCK_AOS_ERROR_TASKS 8U

/**
 * @brief Record why the calling task's call failed: what fck_error() answers it.
 *
 * FCK_AOS_ERROR_TASKS tasks are kept, the oldest forgotten for one more. @p why must be a string literal
 * (or live as long as the program): it is kept, not copied. Never blocks.
 */
void fck_aos_error_set(const char *why);

/** @brief A call went well: the calling task's last reason is "" again, and its class none. Never blocks. */
void fck_aos_error_ok(void);

/** @brief The last reason recorded for the calling task, "" if none. Never blocks. */
const char *fck_aos_error(void);

/**
 * @brief Say that the sub-GHz listening is in force (@p on, tuned to @p hz) or over.
 *
 * The owner keeps it for the facade: when the module leaves it is put out of force and told
 * (FCK_AOS_EVENT_SETTING_LOST), and when the module is back it is put in force again and told
 * (FCK_AOS_EVENT_SETTING_RESTORED) - it emits nothing. Turning it off also forgets what was
 * to be restored, with the module gone. Never blocks.
 */
void fck_aos_setting_listen(bool on, uint32_t hz);

/**
 * @brief Whether the listening is in force now, on which frequency, and the count that moves
 *        each time it is set (again): a reader that tuned on an older count tunes again.
 * @return false when none, or the module is gone and it has not been restored.
 */
bool fck_aos_setting_listening(uint32_t *hz, uint32_t *generation);

/** @brief The count fck_aos_cancel() moves on; a call takes it when it begins. */
uint32_t fck_aos_cancel_mark(void);

/** @brief Whether fck_aos_cancel() was called since @p mark was taken. */
bool fck_aos_cancelled(uint32_t mark);

/** @brief Whether an F*CK is plugged in and set up. Never blocks. */
bool fck_aos_present(void);

/**
 * @brief Wait for an F*CK to be found.
 * @return KODE_OK once one is; KODE_TIMEOUT after @p ms; KODE_DENIED when the
 *         owner was never started.
 */
kode_status_t fck_aos_wait_present(uint32_t ms);

/** @brief What the owner knows, copied. Never blocks. */
void fck_aos_status(fck_aos_status_t *out);

/**
 * @brief Send one command to a chip's firmware and wait for its #DONE.
 *
 * @param chip     Which chip.
 * @param line     The command as written: 1 to 255 printable ASCII
 *                 characters, and nothing else but a final '\n'. A line that
 *                 does not end in '\n' gets "\r\n"; one that does (a
 *                 builder's) goes as it is.
 * @param verb     The word its #DONE carries (fck_drv_link_begin()).
 * @param on_reply The command's machine lines, in the owner's step; may be
 *                 NULL.
 * @param ctx      Handed back to @p on_reply.
 * @param ms       How long the firmware is given. The caller waits a little
 *                 longer than this for its place, if another caller holds
 *                 the chip.
 * @param rc       The #DONE's rc, -1 if it carried none; may be NULL.
 * @return KODE_OK when the #DONE came (look at @p rc), or for `info` its
 *         banner alone (@p rc -1: the chip was starting and printed it by
 *         itself); KODE_TIMEOUT when it did not in time, or the chip stayed
 *         busy; KODE_DENIED with no module, the chip off, or a bad argument
 *         - a line with a control character in it among them;
 *         KODE_IO_ERROR when the module left or lost power on the way, or
 *         the chip restarted: its banner came in the middle of the command.
 */
kode_status_t fck_aos_command(fck_aos_chip_t chip, const char *line, const char *verb,
                              fck_drv_link_message_fn on_reply, void *ctx, uint32_t ms,
                              int32_t *rc);

/**
 * @brief fck_aos_command(), only while the chip still runs what
 *        fck_aos_firmware() said with @p seq.
 *
 * Checked in the owner's step, right before the line is written. A chip
 * reset, powered, written by a job or restarted since then is not sent it,
 * so a line judged by what a chip runs reaches only that.
 *
 * @return As fck_aos_command(); KODE_DENIED too when the chip changed.
 */
kode_status_t fck_aos_command_if_same_firmware(fck_aos_chip_t chip, uint32_t seq, const char *line,
                                 const char *verb, fck_drv_link_message_fn on_reply, void *ctx,
                                 uint32_t ms, int32_t *rc);

/**
 * @brief What a chip says it runs; asked for, and waited for, if it has not
 *        said since it last started.
 *
 * The owner keeps each chip's banner: the one it prints when it starts, and
 * its answer to `info`. It forgets it when the chip is reset or powered,
 * when a job has the module (fck_aos_run_job()), and when the module is set up
 * again. Asked for one it does not have, it sends `info` and waits for the
 * banner. A chip still starting - the C5 takes up to 5 s, the nRF about
 * 6.3 s (FCK_BSP_NRF_BOOT_TO_FIRMWARE_MS) - never hears that `info`, and its
 * own banner, when its firmware is up, is the answer.
 *
 * @param chip Which chip.
 * @param ms   How long to wait; 0 copies what there is, at once, and asks
 *             nothing. A look that finds nothing notes no reason: fck_aos_error()
 *             stays as the call around it left it.
 * @param out  What it said; out->known false when it has not.
 * @return KODE_OK when it is known; KODE_TIMEOUT when it did not say in
 *         time, or answered `info` without a banner; KODE_DENIED with no
 *         module, the chip held in reset, a bad argument, or before
 *         fck_aos_start(); KODE_IO_ERROR when the module left on the way.
 */
kode_status_t fck_aos_firmware(fck_aos_chip_t chip, uint32_t ms, fck_aos_firmware_t *out);

/**
 * @brief Keep a chip for the calling task, across several calls, so that no
 *        other task's command comes between them: a power level and the
 *        ping sent at it.
 *
 * Every call here for that chip goes on working in the meantime, from the
 * holding task; from any other it waits, as it waits for any command in
 * flight. Holds nest: each is ended by its own fck_aos_release(). Hold one
 * chip at a time; fck_aos_run_job(), which needs both, may otherwise wait out
 * its time.
 *
 * @return KODE_OK; KODE_TIMEOUT when another task kept it for @p ms;
 *         KODE_DENIED for a bad chip, before fck_aos_start(), or from the
 *         owner's step or its worker.
 */
kode_status_t fck_aos_hold(fck_aos_chip_t chip, uint32_t ms);

/**
 * @brief End one fck_aos_hold().
 * @return KODE_OK; KODE_DENIED for a chip the calling task does not hold.
 */
kode_status_t fck_aos_release(fck_aos_chip_t chip);

/**
 * @brief Hear what a chip says outside any command: its boot banner, and the
 *        tags its link routes as asynchronous (#GNSSU and #PPS on the C5).
 *        A banner that ends a command, the chip restarting, is heard here
 *        too.
 *
 * The change is made by the owner, between two callbacks: once this returns,
 * the function it replaces is never called again, and its @p ctx may go.
 *
 * @param fn  Called in the owner's step; NULL stops listening.
 * @return KODE_OK; KODE_DENIED for a bad chip.
 */
kode_status_t fck_aos_listen(fck_aos_chip_t chip, fck_drv_link_message_fn fn, void *ctx);

/**
 * @brief Hold a chip in reset, or let it run.
 *
 * The C5 through its EN, pulled low (R502 then draws 0.33 mA); the nRF
 * through the expander's P6. A chip held in reset answers no command.
 *
 * @return KODE_OK; KODE_DENIED with no module, or for the nRF while the
 *         expander cannot be reached: the GNSS receiver holds the bus, or
 *         fck_aos_gnss_power(true) is still finding out whether it will (about
 *         620 ms); KODE_IO_ERROR when the expander did not take it.
 */
kode_status_t fck_aos_power(fck_aos_chip_t chip, bool on);

/**
 * @brief Reset a chip and let it boot its own firmware again.
 *
 * The C5 talks again after its boot; the nRF after about 6.3 s
 * (FCK_BSP_NRF_BOOT_TO_FIRMWARE_MS), the first 3 s of which MCUboot listens for
 * a new image.
 *
 * @return As fck_aos_power().
 */
kode_status_t fck_aos_reset(fck_aos_chip_t chip);

/**
 * @brief Let the GNSS receiver run, or put it back in reset.
 *
 * Running, its standard firmware may take the I2C bus away from the
 * expander. The owner then closes the bus, keeps every pin where it was (SW0
 * included), refuses what needs the expander, and watches for the module on
 * SCL alone: a module unplugged is seen, one that lost power for an instant
 * is not, until the receiver is back in reset. Putting it back in reset gets
 * the bus back, by hand if it has to (fck_drv_tca6408a_blind_gnss_reset()).
 *
 * @return KODE_OK; KODE_DENIED with no module; KODE_IO_ERROR when the
 *         expander did not take it. Going to reset, the receiver is counted
 *         as in reset even then: P2 was written low by hand.
 */
kode_status_t fck_aos_gnss_power(bool run);

/**
 * @brief The GNSS receiver's latest fix, or the next one.
 *
 * While the receiver runs (fck_aos_gnss_power(true)), the owner reads every #GNSSU
 * the C5 relays (fck_drv_c5_command_gnss()) - before it hands the line to whoever
 * listens with fck_aos_listen(), who goes on getting it - and keeps the fix
 * the receiver's latest GGA and RMC make. A fix is one of each, read since
 * the previous fix. It is forgotten when the receiver is stopped or started,
 * when a module is set up or lost, and when the C5 is reset, powered or
 * written (its relay goes off with it). A position is private: nothing the
 * owner does with a sentence or a fix reaches a log, in any level.
 *
 * @param ms  0: copy the latest fix, if there is one, and wait for nothing.
 *            Else: wait up to @p ms for a fix completed after this call; its
 *            GGA may have come just before it, and a receiver at 1 Hz takes
 *            up to a second.
 * @param out Out: written on KODE_OK only.
 * @param age_ms Out, on KODE_OK only: how long ago the owner read the sentence
 *            that completed the fix, by the Kode's own clock. May be NULL.
 * @return KODE_OK; KODE_TIMEOUT when the receiver runs and no fix came, in
 *         time or at all so far (@p ms 0); KODE_DENIED with no module, the
 *         receiver not running, or before fck_aos_start(); KODE_IO_ERROR when
 *         the module left, or lost power, while waiting.
 */
kode_status_t fck_aos_gnss_fix(uint32_t ms, fck_drv_nmea_fix_t *out, uint32_t *age_ms);

/**
 * @brief Run @p fn on the owner's worker, with the module to itself.
 *
 * For what is longer than a command: writing a chip's flash. The owner stops
 * reading the UARTs for the length of it, so @p fn may use them as it likes,
 * and afterwards drops whatever both links held and takes each chip as
 * job->chip_on says. No command is in flight while it runs: the call waits
 * for both chips to be free first.
 *
 * A way home does not wait for a job: after 250 ms the C5 is held in reset
 * with BOOT let go, whatever the job is doing to it (fck_drv_pins_c5_park()),
 * and the P4 restarts with the rest of the module as the job left it. The C5
 * boots its flash when kodeOS lets EN go; one cut off half written does not
 * start its firmware, and is written again.
 *
 * @param ms How long the caller waits for both chips to be free; @p fn
 *           itself is not cut short, so it must keep to its own time.
 * @return What @p fn returned; KODE_DENIED with no module; KODE_TIMEOUT when
 *         the chips stayed busy.
 */
kode_status_t fck_aos_run_job(fck_aos_job_fn fn, void *arg, uint32_t ms);

/**
 * @brief As fck_aos_run_job(), for a job that touches neither chip: the
 *        CC1101's.
 *
 * The module is the job's as much - no command is in flight while it runs,
 * and what comes for a chip waits for its end - but the owner goes on
 * reading both UARTs meanwhile and drops nothing afterwards: what each chip
 * said it runs stands, so a line judged by it before the job is still sent,
 * and the sentences the C5 relays keep making GNSS fixes. The job gets no
 * UART (job->uart is -1 for both) and must leave job->chip_on as it is.
 * The CC1101's calls once ran through fck_aos_run_job(): with all
 * five parts in use at once, half the GNSS reads ran out and most C5
 * commands were refused after one of them.
 *
 * @return As fck_aos_run_job().
 */
kode_status_t fck_aos_run_job_without_chips(fck_aos_job_fn fn, void *arg, uint32_t ms);

/**
 * @brief The J3 position of the module's line at upright position @p pos; 0
 *        while it is not there, or when @p pos is not one of the header's.
 *        (The module is held upright only: a position is itself.)
 */
uint8_t fck_aos_header_position(uint8_t pos);


#ifdef __cplusplus
}
#endif

#endif /* FCK_AOS_H_ */
