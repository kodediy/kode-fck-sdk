/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file fck_sys.h
 * @brief The F*CK module (KDFCK001) for an Active Object of the app's own: the extended layer.
 *
 * Nothing here blocks. A call is one of three kinds, in three sections, as kode_sys.h's are:
 *   Commands    post to the module's owner and return whether it was taken.
 *   Requests    take the asking Active Object, post, and return at once; the answer
 *               arrives to that Active Object later as an event.
 *   Immediate   read a copy, a flag or a setting, or subscribe the calling Active Object,
 *               and return.
 * Whatever its kind, a call refuses a bad argument with KODE_DENIED and does nothing else.
 * A task of the app's own uses fck.h instead: the same module, a call that waits and answers a
 * bool. The two layers can be used side by side in one app, and the pure helpers of fck.h
 * (fck_subghz_decode(), _encode(), _decode_rolling(), _to_file() ..., and
 * fck_ble_manufacturer_data(), fck_ble_service_data(), fck_ble_services() and
 * fck_ble_appearance()) never wait, so they are allowed here too.
 *
 *     kode_init();
 *     fck_sys_begin();                           // takes J3, looks for the module
 *     fck_sys_subscribe(&AO_My.super);           // FCK_MODULE_FOUND_SIG ... FCK_SEND_CANCELLED_SIG
 *     ...
 *     fck_sys_wifi_scan(&AO_My.super, FCK_BAND_2G);   // FCK_WIFI_SCAN_ENTRY_SIG per network,
 *                                                      // then FCK_WIFI_SCAN_RSP_SIG
 *
 * Written against kode-sdk v0.1.5 (API v4).
 *
 * =========================================================================
 * Signals
 *
 * An Active Object of the app cannot publish (MAX_PUB_SIG), so every signal here is POSTED, to
 * one Active Object, and lives in a range of its own: FCK_SIG_FIRST = MAX_SIG +
 * CONFIG_FCK_SIG_OFFSET (1300 unless the project says otherwise). kode_sys.h's own
 * extension SDKs take 1000 (Mesh), 1100 (Robotics) and 1200 (Camera); this one takes 1300 to
 * 1389 for what an app sees and keeps 1390 to 1399 for its owner. Your own signals are
 * MAX_SIG to MAX_SIG + 999. An Active Object never subscribes to a _RSP_SIG or an _ENTRY_SIG: they
 * are answers, posted to the one that asked.
 *
 * =========================================================================
 * Requests
 *
 * A request takes the asking Active Object, @p me, posts to a task of this SDK that carries it
 * out through the owner, and returns at once. Its answer arrives to @p me later as an
 * fck_sys_answer_t on the signal each call names.
 *
 * Immediate returns:
 *   KODE_OK            taken; the answer will come.
 *   KODE_DENIED        @p me is NULL or not a started Active Object (a plain task asks through fck.h), an argument is bad,
 *                      fck_sys_begin() has not run, @p me's door of that kind is busy, or a
 *                      buffer lent is NULL. Nothing was posted.
 *   KODE_NO_RESOURCES  the SDK's queue of requests is full, or kode-sdk's event pool is dry. Nothing
 *                      was posted.
 * The owner's refusals (no module, the chip held in reset, a radio limit, a timeout...)
 * arrive in the answer, never as a return: the answer's @c rc is the code the call
 * would have returned in fck.h's terms (KODE_OK, KODE_DENIED, KODE_TIMEOUT, KODE_IO_ERROR, KODE_NO_RESOURCES
 * ...) and @c why the class it falls in (fck_why_t).
 *
 * Doors: each asker has one door per kind of request, and each request names its kind: COMMAND,
 * FLASH, SCAN, GNSS, RADIO, LISTEN and SEND below. A request holds its door until its answer; a
 * second request of the same kind before then is refused at once. The door is free again just
 * before the answer is posted, so @p me may ask again from the answer's handler. At most
 * FCK_SYS_ASKERS_MAX Active Objects hold a door at once.
 *
 * Answers: a request taken is answered exactly once, posted to @p me with no margin and never
 * published, with the final status in @c rc. If the event pool is dry the SDK waits for a
 * block for up to a second; if @p me's queue is full it asks again for as long: then it logs,
 * counts it in fck_status().answers_dropped and drops the answer - the door is open all the same.
 *
 * Lent buffers: a payload too big for an event stays in the caller's buffer, marked LENT on the
 * call. Keep it valid, and leave it alone, from the call until the answer arrives. The SDK never
 * writes it after the answer, and never at all for a request refused at once. Where a
 * buffer is lent, @c count says how much of it was written.
 *
 * Queue depth, the one rule for every asker: an answer that finds @p me's queue full is lost
 * after a second, and the call that waited for it never finishes. So @p me's queue holds
 * everything that can wait in it at once:
 *   - one slot for each request waiting for its answer: one per door open (seven kinds);
 *   - the entries of a scan, posted with a margin of FCK_SYS_ENTRY_MARGIN slots: a full queue
 *     loses entries (the answer's @c value counts them) and never the answer;
 *   - the news (below), posted with a margin of FCK_SYS_NEWS_MARGIN slots;
 *   - @p me's own events: its timers, the signals it subscribes to (a press and a release are
 *     two) and whatever else is posted to it.
 * For example, an AO with a 1 s timer and BUTTON_EVT_SIG that asks, on one tick, for a Wi-Fi
 * scan, a sub-GHz capture and a GNSS stop has three doors open: three slots for the answers,
 * two for the timer and a press and release, and the 64 entries a busy air could bring wait
 * behind them only as far as the margin allows. KQP_AO_QUEUE_LEN (16) is enough for that AO;
 * one that holds all seven doors open and lists is at ease with 32.
 *
 * =========================================================================
 * News
 *
 * fck_sys_subscribe() points @p me at the news: the same events as fck.h's named ones,
 * posted to every subscriber as an fck_sys_news_t (FCK_SUBSCRIBERS_MAX of them). They are posted
 * with a margin of FCK_SYS_NEWS_MARGIN slots: if @p me's queue does not have them they are lost
 * and counted in fck_status().news_dropped. Never an assert. Subscribers stay across
 * fck_sys_end() and fck_sys_begin().
 *
 *   | Signal                      | value                   | When                                  |
 *   |-----------------------------|-------------------------|---------------------------------------|
 *   | FCK_MODULE_FOUND_SIG        | 1 (held upright)        | An F*CK was found and set up.         |
 *   | FCK_MODULE_LOST_SIG         | 0                       | It left J3, lost power, or fck_sys_end()|
 *   |                             |                         | put it down.                          |
 *   | FCK_WRONG_MODULE_SIG        | an FCK_WRONG_MODULE_*   | Not an F*CK on J3.                    |
 *   | FCK_SETTING_LOST_SIG        | the FCK_SETTING_*       | What the app had set went with the    |
 *   |                             |                         | module.                               |
 *   | FCK_SETTING_RESTORED_SIG    | the FCK_SETTING_*       | The SDK set again what does not emit. |
 *   | FCK_PACKET_RECEIVED_SIG     | the capture's count     | The sub-GHz listening heard a remote; |
 *   |                             |                         | read it with fck_sys_subghz_listen_read().|
 *   | FCK_SEND_CANCELLED_SIG      | FCK_CANCEL_CALL or      | A sub-GHz send was cut short; its    |
 *   |                             | FCK_CANCEL_LEAVING      | request is answered as cancelled.     |
 *
 * =========================================================================
 * What the module does by itself, and what it never does
 *
 * Nothing transmits or records by itself: no call here emits unless an Active Object asks it
 * to, and the module coming back or the board waking restores only what listens (the GNSS
 * receiver, the sub-GHz listening). The radio limits (the hour's air time, the bands) and the
 * console's `fck_arm` stay as they are. fck_sys_command() is a GATEWAY: it hands a line to the
 * chip's firmware as written, and the SDK checks it against the radio limits and nothing else (see
 * fck_command() in fck.h).
 *
 * Sleep. With CONFIG_KODE_LOWPOWER the owner puts the module down when the board is going to
 * rest (a round with target ASLEEP or OFF: what is on the air is cut, a send in flight is
 * answered as cancelled, the GNSS receiver goes to reset, the CC1101 asleep, the chips held)
 * and looks for it again, giving back only what listens, when the board is awake.
 *
 * J3's supply. The owner asks kode-sdk for it with the radio's help and fck_status().j3_supply
 * says what came of it. With no radio in the image (CONFIG_KODEOS_RADIO=n) J3 is always
 * powered and fck_sys_end() cannot switch it off.
 */
#ifndef FCK_SYS_H_
#define FCK_SYS_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "kode_sys.h"

#include "fck.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Signals -------------------------------------------------------------- */

#ifndef CONFIG_FCK_SIG_OFFSET
#define CONFIG_FCK_SIG_OFFSET 1300
#endif

/** @brief The first signal of this SDK: MAX_SIG + 1300 by default. */
#define FCK_SIG_FIRST ((QSignal)(MAX_SIG + CONFIG_FCK_SIG_OFFSET))

/** @brief The signals an app sees, in 90 numbers. The owner's own start 90 above FCK_SIG_FIRST. */
enum FckSysSignals {
    /* News, to the subscribers (fck_sys_subscribe()): an fck_sys_news_t. */
    FCK_MODULE_FOUND_SIG = FCK_SIG_FIRST,
    FCK_MODULE_LOST_SIG,
    FCK_WRONG_MODULE_SIG,
    FCK_SETTING_LOST_SIG,
    FCK_SETTING_RESTORED_SIG,
    FCK_PACKET_RECEIVED_SIG,
    FCK_SEND_CANCELLED_SIG,

    /* Entries of a scan, to the one that asked: before the answer. */
    FCK_WIFI_SCAN_ENTRY_SIG,    /**< An fck_sys_wifi_entry_t.   */
    FCK_BLE_SCAN_ENTRY_SIG,     /**< An fck_sys_ble_entry_t.    */

    /* Answers, to the one that asked: an fck_sys_answer_t each. */
    FCK_COMMAND_RSP_SIG,
    FCK_RESET_RSP_SIG,
    FCK_POWER_RSP_SIG,
    FCK_FIRMWARE_RSP_SIG,
    FCK_FLASH_RSP_SIG,
    FCK_RESCUE_PROBE_RSP_SIG,
    FCK_WIFI_SCAN_RSP_SIG,
    FCK_WIFI_CHANNELS_RSP_SIG,
    FCK_BLE_SCAN_RSP_SIG,
    FCK_GNSS_BEGIN_RSP_SIG,
    FCK_GNSS_END_RSP_SIG,
    FCK_RADIO_SPECTRUM_RSP_SIG,
    FCK_RADIO_SNIFF_RSP_SIG,
    FCK_RADIO_PING_RSP_SIG,
    FCK_RADIO_ECHO_START_RSP_SIG,
    FCK_RADIO_ECHO_STOP_RSP_SIG,
    FCK_SUBGHZ_LEVEL_RSP_SIG,
    FCK_SUBGHZ_SPECTRUM_RSP_SIG,
    FCK_SUBGHZ_FIND_RSP_SIG,
    FCK_SUBGHZ_CAPTURE_RSP_SIG,
    FCK_SUBGHZ_PACKET_RECEIVE_RSP_SIG,
    FCK_SUBGHZ_SEND_RAW_RSP_SIG,
    FCK_SUBGHZ_SEND_CODE_RSP_SIG,
    FCK_SUBGHZ_PACKET_SEND_RSP_SIG,

    FCK_SIG_END     /**< Not a signal: one past the last. */
};

_Static_assert((unsigned)FCK_SIG_END - (unsigned)FCK_SIG_FIRST <= 90U,
               "the app's signals fit in the 90 below the owner's");
_Static_assert((unsigned)FCK_SIG_FIRST + 100U < 0xFFFFU, "the signals fit in a QSignal (16 bits)");
_Static_assert(CONFIG_FCK_SIG_OFFSET >= 1000, "CONFIG_FCK_SIG_OFFSET must leave the app its own 1000 above MAX_SIG");

/* ---- Limits --------------------------------------------------------------- */

/** @brief Active Objects that may be subscribed to the news at once. */
#define FCK_SUBSCRIBERS_MAX  4U
/** @brief Active Objects that may hold a door at once. */
#define FCK_SYS_ASKERS_MAX   8U
/** @brief Slots of the asker's queue the entries of a scan leave free. */
#define FCK_SYS_ENTRY_MARGIN 3U
/** @brief Slots of a subscriber's queue the news leave free. */
#define FCK_SYS_NEWS_MARGIN  2U
/** @brief Requests the SDK's task can hold waiting for a turn. */
#define FCK_SYS_QUEUE        8U

/* ---- Why ------------------------------------------------------------------- */

/**
 * @brief The class a failure falls in: the table of causes in fck.h, as a number an Active Object
 *        can switch on. fck_why_text() gives the sentence.
 *
 * The numbers are those of the owner's classes and stay as they are.
 */
typedef enum {
    FCK_WHY_NONE = 0,       /**< It went well.                                                       */
    FCK_WHY_NOT_BEGUN,      /**< fck_sys_begin() / fck_begin() has not run, or the end has.          */
    FCK_WHY_LEAVING,        /**< The way home, a rest or a restart has begun.                        */
    FCK_WHY_NO_MODULE,      /**< Not on J3, powered off, or just pulled out.                         */
    FCK_WHY_CHIP_HELD,      /**< The chip is held in reset or kept by a flash.                       */
    FCK_WHY_CHIP_NOT_READY, /**< It has not said what it runs; the GNSS holds the bus.               */
    FCK_WHY_BAD_ARGUMENT,   /**< NULL, 0 or a value out of the range the call states.                */
    FCK_WHY_BEYOND_LIMITS,  /**< A band, power, carrier length or hour's air time the module does not allow; a rolling code; a line the gate refuses. */
    FCK_WHY_FIRMWARE_TOO_OLD, /**< The firmware lacks the command.                                      */
    FCK_WHY_WRONG_ORDER,    /**< A read before its start, a stop with nothing running.               */
    FCK_WHY_CANCELLED,      /**< fck_sys_cancel(), or a send cut by the way home, a rest or the end. */
    FCK_WHY_BUSY,           /**< A chip another task keeps, or no memory or room for the call.       */
    FCK_WHY_TIMEOUT,        /**< A wait that ran out.                                                */
    FCK_WHY_IO,             /**< The module or a bus failed under the call, or the firmware said it did not do it. */
    FCK_WHY_REFUSED,        /**< Refused for a reason none of the above names; the log says which.   */
    FCK_WHY_AO_CALLER       /**< An fck.h call that blocks was made from an Active Object.           */
} fck_why_t;

/**
 * @brief Returns at once. A sentence for a person that says what @p why means.
 * @return A literal, valid for the life of the program; "" for FCK_WHY_NONE or a value that is
 *         none of the enum's. Never NULL.
 */
const char *fck_why_text(fck_why_t why);

/* ---- Answers and news ------------------------------------------------------- */

/**
 * @brief The answer to a request, on its _RSP_SIG.
 *
 * @c rc is KODE_OK when the call did what it was asked, and otherwise the code fck.h's @return
 * would have given (KODE_DENIED, KODE_TIMEOUT, KODE_IO_ERROR, KODE_NO_RESOURCES, KODE_ERROR,
 * KODE_EXISTS); @c why the class of the failure, FCK_WHY_NONE on KODE_OK. @c count is what the
 * call wrote into a buffer lent to it (items, or bytes for a command's reply); @c value is the
 * call's own number, said where each request says it.
 */
typedef struct {
    QEvt          super;
    kode_status_t rc;
    fck_why_t     why;
    uint32_t      count;
    int32_t       value;
    uint32_t      flags;    /**< FCK_SYS_ANSWER_*. */
} fck_sys_answer_t;

/** @brief fck_sys_answer_t.flags: the reply of a command was longer than the buffer lent. */
#define FCK_SYS_ANSWER_TRUNCATED  1UL

/** @brief The news: FCK_MODULE_FOUND_SIG ... FCK_SEND_CANCELLED_SIG. @c value as the table above says. */
typedef struct {
    QEvt    super;
    int32_t value;
} fck_sys_news_t;

/** @brief One network (FCK_WIFI_SCAN_ENTRY_SIG): @c index counts from 0 in the order the scan gave them. */
typedef struct {
    QEvt               super;
    uint16_t           index;
    fck_wifi_network_t network;
} fck_sys_wifi_entry_t;

/**
 * @brief One device (FCK_BLE_SCAN_ENTRY_SIG): @c index counts from 0 in the order the scan gave them.
 *
 * @c device is whole, what it advertised included: fck_ble_manufacturer_data(),
 * fck_ble_service_data(), fck_ble_services() and fck_ble_appearance() read it from the step that
 * takes the event. The event is gone when that step returns, so an Active Object that wants a
 * device later copies it.
 */
typedef struct {
    QEvt             super;
    uint16_t         index;
    fck_ble_device_t device;
} fck_sys_ble_entry_t;

/* =========================================================================
 * Start and end
 * ========================================================================= */

/**
 * @brief Immediate. Take J3 for the module, start its owner and have it look for the module. After
 *        kode_init(). Idempotent. Returns as soon as the owner runs: not when the module is
 *        found (FCK_MODULE_FOUND_SIG says that).
 *
 * J3's supply is asked on, and the answer is in fck_status().j3_supply.
 * @return KODE_OK; KODE_DENIED when kode_init() has not run, something else in the image
 *         borrowed some of J3's positions, another task is starting it, or fck_sys_end() is still
 *         under way (ask again); KODE_EXISTS when the
 *         console took J3 by hand (`exp`); KODE_NO_RESOURCES when the owner's task would not start,
 *         kode-sdk has no controller left, or there is no memory for this SDK's task.
 *         A start that fails leaves everything as it was.
 */
kode_status_t fck_sys_begin(void);

/**
 * @brief Command. Put the module down and let go of it, on a task of this SDK: what is on the
 *        air is cut (and its request answered as cancelled), the GNSS receiver goes to reset, the
 *        CC1101 to sleep, both chips are held, J3's supply is asked off and the loan is given
 *        back. Returns at once; the end is FCK_MODULE_LOST_SIG (when the module was there) and
 *        fck_status().j3_supply. fck_sys_begin() starts it again.
 * @return KODE_OK, also when nothing was started or the end is under way; KODE_NO_RESOURCES when
 *         there is no memory for this SDK's task.
 */
kode_status_t fck_sys_end(void);

/* =========================================================================
 * Commands
 * ========================================================================= */

/**
 * @brief Command. Cancel what the app is waiting for: the same as fck_cancel() in fck.h. The
 *        requests that began before it are answered at once, as cancelled (rc KODE_DENIED, why
 *        FCK_WHY_CANCELLED); one begun after it is not touched. A flash is not cancellable.
 * @return KODE_OK; KODE_DENIED before fck_sys_begin().
 */
kode_status_t fck_sys_cancel(void);

/**
 * @brief Command. Start listening for remotes on @p hz, or hopping with FCK_SUBGHZ_HOP: the owner
 *        listens in the background and keeps the latest remote it heard; FCK_PACKET_RECEIVED_SIG
 *        says when there is one, and fck_sys_subghz_listen_read() reads it. It receives and
 *        never transmits. A listening started again starts afresh.
 * @return KODE_OK; KODE_DENIED for a frequency the CC1101 does not tune, before fck_sys_begin(), or
 *         with no module or a CC1101 that did not answer. Nothing else is done.
 */
kode_status_t fck_sys_subghz_listen_start(uint32_t hz);

/**
 * @brief Command. Stop listening: what was learned is forgotten.
 * @return KODE_OK, also when nothing listened; KODE_DENIED before fck_sys_begin().
 */
kode_status_t fck_sys_subghz_listen_stop(void);

/**
 * @brief Command. Tune the nRF's sniffer, as fck_radio_sniff_tune() does: two lines to its firmware,
 *        posted one after the other. The second is sent when the first has been answered. No
 *        answer comes for either: a line the firmware refuses, or one that is not sent (the nRF
 *        off, the app leaving, the module gone), leaves a warning in the log. Both are
 *        idempotent, so on KODE_NO_RESOURCES (the second did not fit) ask again.
 * @return KODE_OK both lines were taken; KODE_DENIED for a value the firmware does not take, with no
 *         module, or an nRF whose firmware is not compatible; KODE_NO_RESOURCES.
 */
kode_status_t fck_sys_radio_sniff_tune(const fck_radio_sniff_config_t *cfg);

/* =========================================================================
 * Requests
 *
 * Each names its door and its answer. The buffers marked LENT are the caller's.
 * ========================================================================= */

/**
 * @brief Request, door COMMAND. Send one command to a chip's firmware and wait for its #DONE: the free
 *        GATEWAY of fck_command() in fck.h, with its limits. It TRANSMITS when the line does: the
 *        C5's `scan`, the nRF's `rf ping` and `rf echo on`, and whatever another firmware's lines do.
 * @param me    The asking AO.
 * @param chip  Which chip.
 * @param line  The command (copied), under 256 bytes; for the nRF's firmware, at most 127.
 * @param verb  The word its #DONE carries (copied); NULL as fck_command() says.
 * @param ms    How long the firmware is given; 0 for what the command takes.
 * @param reply LENT. The firmware's tagged lines, "TAG body\n" each, NUL-terminated; may be NULL
 *              with @p reply_cap 0 to drop them.
 * @return Answered on FCK_COMMAND_RSP_SIG: @c rc KODE_OK when the #DONE came, @c value its rc (0 done,
 *         1 failed, 2 not understood, -1 none), @c count the bytes of @p reply written (the NUL
 *         not counted), FCK_SYS_ANSWER_TRUNCATED when it was cut.
 */
kode_status_t fck_sys_command(QActive *me, fck_chip_t chip, const char *line, const char *verb,
                              uint32_t ms, char *reply, size_t reply_cap);

/** @brief Request, door COMMAND. Reset a chip: see fck_reset(). Answered on FCK_RESET_RSP_SIG. */
kode_status_t fck_sys_reset(QActive *me, fck_chip_t chip);

/** @brief Request, door COMMAND. Hold a chip in reset or let it run: see fck_power(). Answered on FCK_POWER_RSP_SIG. */
kode_status_t fck_sys_power(QActive *me, fck_chip_t chip, bool on);

/**
 * @brief Request, door COMMAND. What a chip runs: see fck_firmware().
 * @param out LENT. Filled on rc KODE_OK, with known set.
 * @return Answered on FCK_FIRMWARE_RSP_SIG.
 */
kode_status_t fck_sys_firmware(QActive *me, fck_chip_t chip, fck_firmware_t *out);

/**
 * @brief Request, door FLASH. Write a chip's firmware: see fck_flash(). It takes minutes; it is not
 *        cancellable.
 * @param img LENT. The images, @p n of them, and the data they point at.
 * @param rep LENT. The report; may be NULL.
 * @return Answered on FCK_FLASH_RSP_SIG.
 */
kode_status_t fck_sys_flash(QActive *me, fck_chip_t chip, const fck_image_t *img, size_t n,
                            fck_flash_report_t *rep);

/**
 * @brief Request, door FLASH. Look at what a chip's lines say without flashing it: see fck_rescue_probe().
 * @param out LENT. Filled on rc KODE_OK.
 * @return Answered on FCK_RESCUE_PROBE_RSP_SIG.
 */
kode_status_t fck_sys_rescue_probe(QActive *me, fck_chip_t chip, fck_rescue_report_t *out);

/**
 * @brief Request, door SCAN. Scan Wi-Fi with the C5: it TRANSMITS, see fck_wifi_scan(). One FCK_WIFI_SCAN_ENTRY_SIG
 *        per network (fck_sys_wifi_entry_t), then FCK_WIFI_SCAN_RSP_SIG.
 * @return The answer's @c count is the networks found, @c value the entries a full queue lost
 *         (the entries leave FCK_SYS_ENTRY_MARGIN slots free).
 */
kode_status_t fck_sys_wifi_scan(QActive *me, fck_band_t band);

/**
 * @brief Request, door SCAN. Survey the Wi-Fi channels: see fck_wifi_channels().
 * @param out LENT. @p cap channels at most.
 * @return Answered on FCK_WIFI_CHANNELS_RSP_SIG, @c count channels written.
 */
kode_status_t fck_sys_wifi_channels(QActive *me, fck_band_t band, uint16_t dwell_ms,
                                    fck_wifi_channel_t *out, size_t cap);

/**
 * @brief Request, door SCAN. Scan Bluetooth LE with the C5: see fck_ble_scan(). One
 *        FCK_BLE_SCAN_ENTRY_SIG per device (fck_sys_ble_entry_t), then FCK_BLE_SCAN_RSP_SIG, with
 *        @c count the devices found and @c value the entries lost.
 */
kode_status_t fck_sys_ble_scan(QActive *me, uint8_t duration_s);

/** @brief Request, door GNSS. Turn the GNSS receiver on and the C5's relay: see fck_gnss_begin(). FCK_GNSS_BEGIN_RSP_SIG. */
kode_status_t fck_sys_gnss_begin(QActive *me);

/** @brief Request, door GNSS. Turn it off: see fck_gnss_end(). FCK_GNSS_END_RSP_SIG. */
kode_status_t fck_sys_gnss_end(QActive *me);

/**
 * @brief Request, door RADIO. The nRF's 2.4 GHz spectrum: see fck_radio_spectrum().
 * @param dbm LENT. @p cap levels at most.
 * @return Answered on FCK_RADIO_SPECTRUM_RSP_SIG, @c count levels written.
 */
kode_status_t fck_sys_radio_spectrum(QActive *me, uint16_t first_mhz, uint16_t last_mhz,
                                     uint32_t dwell_us, uint16_t sweeps, int8_t *dbm, size_t cap);

/**
 * @brief Request, door RADIO. Sniff frames on one channel: see fck_radio_sniff().
 * @param out   LENT. @p cap frames at most.
 * @param stats LENT; may be NULL.
 * @return Answered on FCK_RADIO_SNIFF_RSP_SIG, @c count frames written.
 */
kode_status_t fck_sys_radio_sniff(QActive *me, uint16_t mhz, uint32_t ms, int8_t rssi_min,
                                  fck_radio_frame_t *out, size_t cap,
                                  fck_radio_sniff_statistics_t *stats);

/**
 * @brief Request, door RADIO. Ping another nRF: it TRANSMITS, as fck_radio_ping() does; ask it only
 *        on a person's press.
 * @param stats LENT. Filled on rc KODE_OK.
 * @return Answered on FCK_RADIO_PING_RSP_SIG.
 */
kode_status_t fck_sys_radio_ping(QActive *me, uint16_t count, uint16_t mhz, uint16_t gap_ms,
                                 int8_t dbm, fck_radio_ping_statistics_t *stats);

/** @brief Request, door RADIO. Answer pings: it TRANSMITS; see fck_radio_echo_start(). FCK_RADIO_ECHO_START_RSP_SIG. */
kode_status_t fck_sys_radio_echo_start(QActive *me, uint16_t mhz, int8_t dbm);

/** @brief Request, door RADIO. Stop answering pings: see fck_radio_echo_stop(). @p stats LENT, may be NULL. FCK_RADIO_ECHO_STOP_RSP_SIG. */
kode_status_t fck_sys_radio_echo_stop(QActive *me, fck_radio_echo_statistics_t *stats);

/**
 * @brief Request, door LISTEN. The power on one frequency: see fck_subghz_level().
 * @param dbm LENT. Written on rc KODE_OK.
 * @return Answered on FCK_SUBGHZ_LEVEL_RSP_SIG.
 */
kode_status_t fck_sys_subghz_level(QActive *me, uint32_t hz, fck_subghz_bandwidth_t bw, int16_t *dbm);

/**
 * @brief Request, door LISTEN. The power on @p count frequencies: see fck_subghz_spectrum().
 * @param dbm LENT. @p cap levels at least @p count.
 * @return Answered on FCK_SUBGHZ_SPECTRUM_RSP_SIG, @c count levels written.
 */
kode_status_t fck_sys_subghz_spectrum(QActive *me, uint32_t first_hz, uint32_t step_hz, uint16_t count,
                                      fck_subghz_bandwidth_t bw, int16_t *dbm, size_t cap);

/**
 * @brief Request, door LISTEN. Where does a remote send: see fck_subghz_find().
 * @param hit LENT. Written on rc KODE_OK.
 * @return Answered on FCK_SUBGHZ_FIND_RSP_SIG.
 */
kode_status_t fck_sys_subghz_find(QActive *me, uint32_t ms, fck_subghz_hit_t *hit);

/**
 * @brief Request, door LISTEN. Record the first remote heard on @p hz: see fck_subghz_capture().
 * @param us LENT. @p cap runs at most.
 * @return Answered on FCK_SUBGHZ_CAPTURE_RSP_SIG, @c count runs written (0 when nothing was heard).
 */
kode_status_t fck_sys_subghz_capture(QActive *me, uint32_t hz, uint32_t ms, int32_t *us, size_t cap);

/**
 * @brief Request, door LISTEN. Receive packets with a modem of your own: see fck_subghz_packet_receive().
 * @param settings The modem (copied).
 * @param packets  LENT. @p cap packets at most.
 * @return Answered on FCK_SUBGHZ_PACKET_RECEIVE_RSP_SIG, @c count packets received.
 */
kode_status_t fck_sys_subghz_packet_receive(QActive *me, uint32_t hz,
                                            const fck_subghz_packet_settings_t *settings, uint32_t ms,
                                            fck_subghz_packet_t *packets, size_t cap);

/**
 * @brief Request, door SEND. TRANSMITS raw OOK runs: see fck_subghz_send_raw(), its limits and the
 *        hour's air time. Ask it only on a person's press.
 * @param us LENT. The @p n runs, valid until the answer.
 * @return Answered on FCK_SUBGHZ_SEND_RAW_RSP_SIG: rc KODE_OK when it was sent; a send cut by
 *         fck_sys_cancel(), the way home, a rest or fck_sys_end() is answered rc KODE_DENIED (or KODE_IO_ERROR
 *         when cut by leaving) with why FCK_WHY_CANCELLED, and FCK_SEND_CANCELLED_SIG is posted.
 */
kode_status_t fck_sys_subghz_send_raw(QActive *me, uint32_t hz, const int32_t *us, size_t n, uint8_t repeats);

/** @brief Request, door SEND. TRANSMITS a code: see fck_subghz_send_code(). @p code is copied. FCK_SUBGHZ_SEND_CODE_RSP_SIG. */
kode_status_t fck_sys_subghz_send_code(QActive *me, uint32_t hz, const fck_subghz_code_t *code, uint8_t frames);

/**
 * @brief Request, door SEND. TRANSMITS a packet: see fck_subghz_packet_send(). @p settings and @p bytes
 *        are copied. Answered on FCK_SUBGHZ_PACKET_SEND_RSP_SIG: rc KODE_OK says it was sent.
 */
kode_status_t fck_sys_subghz_packet_send(QActive *me, uint32_t hz, const fck_subghz_packet_settings_t *settings,
                                         const uint8_t *bytes, uint8_t length, uint8_t repeats);

/* =========================================================================
 * Immediate
 * ========================================================================= */

/** @brief Immediate. Whether an F*CK is plugged in and set up. Safe from anywhere. */
bool fck_sys_is_present(void);

/**
 * @brief Immediate. What is known about the module, copied.
 * @return KODE_OK; KODE_DENIED when @p out is NULL.
 */
kode_status_t fck_sys_status_read(fck_status_t *out);

/** @brief Immediate. The J3 position of one of the module's lines, 0 if there is none: see fck_header_position(). */
uint8_t fck_sys_header_position(uint8_t pos);

/**
 * @brief Immediate. The GNSS receiver's latest fix, copied: it waits for none.
 *
 * The fix's @c age_ms says how long ago it was read: the receiver's sentences go on being read
 * while a scan of the C5's is in flight, so the fix read on a scan's response is the scan's place.
 *
 * @param out Written on KODE_OK only.
 * @return KODE_OK; KODE_DENIED when the receiver is off (fck_sys_gnss_begin()), nothing has come yet,
 *         or there is no module.
 */
kode_status_t fck_sys_gnss_read(fck_gnss_fix_t *out);

/**
 * @brief Immediate. The latest remote the listening kept, copied (see fck_subghz_listen_read()).
 * @param after 0: the latest. Else only a capture newer than that sequence.
 * @param us    The runs; the first @p cap (a capture keeps 512).
 * @param n     Runs written; 0 when nothing newer.
 * @param heard Written on KODE_OK; heard false when nothing newer.
 * @return KODE_OK, also when nothing newer was heard; KODE_DENIED for a NULL argument, a @p cap of 0,
 *         no listening in force (fck_sys_subghz_listen_start()) or the module gone.
 */
kode_status_t fck_sys_subghz_listen_read(uint32_t after, int32_t *us, size_t cap, size_t *n,
                                         fck_subghz_heard_t *heard);

/** @brief Immediate. The frequency the listening is set to; FCK_SUBGHZ_HOP (0) while it hops or when none. */
uint32_t fck_sys_subghz_listen_hz(void);

/** @brief Immediate. The frequencies of the remotes the SDK knows: see fck_subghz_frequencies(). */
kode_status_t fck_sys_subghz_frequencies(uint32_t *hz, size_t cap, size_t *n);

/** @brief Immediate. Whether the module may transmit on @p hz: see fck_subghz_may_transmit(). */
bool fck_sys_subghz_may_transmit(uint32_t hz);

/** @brief Immediate. Whether a send would refuse @p us as a rolling code: see fck_subghz_is_rolling(). */
bool fck_sys_subghz_is_rolling(const int32_t *us, size_t n);

/**
 * @brief Immediate. How long the module may still be on air in @p hz's band within the last hour:
 *        see fck_subghz_air_time_left(). It reads a copy of the account.
 * @return KODE_OK; KODE_DENIED for a @p hz outside both bands, a NULL @p ms, before fck_sys_begin() or
 *         with no module; KODE_NO_RESOURCES if the account was being written (ask again).
 */
kode_status_t fck_sys_subghz_air_time_left(uint32_t hz, uint32_t *ms);

/** @brief Immediate. What a protocol of the SDK is called and sends: see fck_subghz_protocol_info(). KODE_DENIED for a bad argument. */
kode_status_t fck_sys_subghz_protocol_info(fck_subghz_protocol_t proto, fck_subghz_protocol_info_t *info);

/**
 * @brief Immediate. Point @p me at the news (FCK_MODULE_FOUND_SIG ...). Idempotent.
 * @return KODE_OK; KODE_DENIED when @p me is NULL or not a started Active Object; KODE_NO_RESOURCES when
 *         FCK_SUBSCRIBERS_MAX are taken.
 */
kode_status_t fck_sys_subscribe(QActive *me);

/**
 * @brief Immediate. Stop the news to @p me.
 * @return KODE_OK, also when it was not subscribed; KODE_DENIED when @p me is NULL.
 */
kode_status_t fck_sys_unsubscribe(QActive *me);

#ifdef __cplusplus
}
#endif

#endif /* FCK_SYS_H_ */
