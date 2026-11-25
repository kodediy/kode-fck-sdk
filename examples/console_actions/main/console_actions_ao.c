/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file console_actions_ao.c
 * @brief The F*CK module on kodeOS's console: six functions and six
 *        variables, and an Active Object, the extended layer (fck_sys.h), that hears what came of them.
 *
 * KODE_FUNCTIONS_ADD() and KODE_VARIABLES_ADD() give the console rows of the
 * app's own (kode_functions.h, kode_variables.h). These are the module's, for
 * the serial console, BLE and kode-cli: an app that wants them copies this file's
 * tables, or the rows it needs.
 *
 * Try it, on the console (the USB-C cable). The name goes in quotes; an INT
 * or ENUM argument bare:
 *
 *     function name="fck_firmware" arg=c5         what the C5 runs, on the log
 *     function name="fck_power" arg=nrf_off    the nRF held in reset
 *     function name="fck_reset" arg=nrf
 *     function name="fck_gnss" arg=on          the receiver out of reset
 *     function name="fck_ble_scan" arg=5       BLE heard for 5 s, passively
 *     function name="fck_rescue" arg=nrf       what the nRF carries; resets it
 *     function name="fck_rescue" arg=c5        what the C5 carries; resets it
 *     variable name="fck_present" watch=500    follow the module
 *
 * Functions (one argument each; none blocks - each is a request of fck_sys.h made in the
 * row, so `>ok` says the SDK took it, and what came of it is on the log, from this AO):
 *
 *     fck_firmware       read what a chip runs: c5 or nrf
 *     fck_power       a chip let run or held in reset: c5_on c5_off nrf_on nrf_off
 *     fck_reset       a chip restarted: c5 or nrf
 *     fck_gnss        the GNSS receiver: on or off
 *     fck_ble_scan    BLE devices counted, passively, 1..30 s
 *     fck_rescue      what a chip carries - its boot code, what starts: c5 or nrf
 *
 * Variables (read-only):
 *
 *     fck_present     on, off
 *     fck_generation  modules set up since the app started
 *     fck_c5          on, reset, none
 *     fck_nrf         on, reset, none
 *     fck_cc1101      asleep, missing, none
 *     fck_gnss_state  running, reset, none
 *
 * Six variables of the firmware's 32 and six functions of its 64, in one
 * table each of the four each kind allows.
 *
 * NOTHING HERE TRANSMITS. fck_sys_wifi_scan() (probe requests), fck_sys_radio_ping()
 * and fck_sys_radio_echo_start() are not on the console on purpose.
 *
 * ---- Who owns what -------------------------------------------------------
 *
 * A function's `run` is called on AO_Console's task, in the middle of its step, and must not
 * block. No call of fck_sys.h does: a request takes an Active Object, posts, and returns at once
 * with whether it was taken, so `run` makes the request itself and answers the console with that
 * very status - a refusal (the module not there, the door busy, a bad argument) is `>` something
 * other than ok at the console, not a line lost on the log. The request is made FOR this AO, so
 * its answer arrives here as an event (FCK_FIRMWARE_RSP_SIG and the like) and the AO says on the log
 * what came of it. The six use four doors: fck_firmware, fck_power and fck_reset share COMMAND,
 * and fck_gnss (GNSS), fck_ble_scan (SCAN) and fck_rescue (FLASH) have one each, so four can run
 * at once; a request typed while its door waits for an answer is refused by the SDK and says so
 * at the console.
 *
 * The buffers a request lends (the firmware's greeting, the rescue report) live in this AO and are
 * read once the answer is there. The module coming or going is news (FCK_MODULE_FOUND_SIG,
 * FCK_MODULE_LOST_SIG): the AO subscribes and logs it. A getter reads fck_sys_is_present() or
 * fck_sys_status_read(), immediate reads of a copy the owner keeps for exactly this.
 *
 * The queue holds 32, the size fck_sys.h gives an AO that may hold all its doors open (see
 * "Queue depth" there): the doors, the news and a BLE scan's entries wait in it at once.
 */
#include "console_actions_ao.h"

#include <stdio.h>

#include "kode.h"
#include "kode_log.h"
#include "fck_sys.h"

#define TAG "console_actions"

/** @brief The AO's own state, private to this file. */
typedef struct {
    QActive             super;
    fck_firmware_t         firmware;      /**< LENT to fck_sys_firmware().          */
    fck_rescue_report_t rescue;     /**< LENT to fck_sys_rescue_probe().   */
    bool                rescue_c5;  /**< The chip the rescue probe looks at. */
    uint8_t             scan_s;     /**< The seconds of the BLE scan asked. */
} ConsoleActions;

static QEvtPtr     l_queue[2U * KQP_AO_QUEUE_LEN];
static StackType_t l_stack[KQP_AO_STACK_BYTES / sizeof(StackType_t)];

static ConsoleActions l_ao;
QActive * const AO_ConsoleActions = &l_ao.super;

static QState ConsoleActions_initial(ConsoleActions * const me, void const * const par);
static QState ConsoleActions_serving(ConsoleActions * const me, QEvt const * const e);

/* ---- Functions ---------------------------------------------------------- */

/**
 * @brief The chip a console enum index names: 0 is the C5, 1 the nRF.
 * @param index The enum's index.
 * @return The chip.
 */
static fck_chip_t chip_of(int32_t const index)
{
    return (0 == index) ? FCK_C5 : FCK_NRF;
}

/*
 * Each row makes its request FOR the AO and answers the console with what the SDK said: KODE_OK,
 * taken; KODE_DENIED, a bad argument, the SDK not begun or the door busy; KODE_NO_RESOURCES, no
 * room. The SDK's own log line says which. What the module made of it arrives to the AO.
 */
static kode_status_t do_firmware(const kode_value_t *arg)
{
    return (NULL == arg) ? KODE_DENIED
                         : fck_sys_firmware(&l_ao.super, chip_of((int32_t)arg->e), &l_ao.firmware);
}

static kode_status_t do_power(const kode_value_t *arg)
{
    /* The enum's index: c5_on 0, c5_off 1, nrf_on 2, nrf_off 3. */
    return (NULL == arg) ? KODE_DENIED
                         : fck_sys_power(&l_ao.super, ((int32_t)arg->e < 2) ? FCK_C5 : FCK_NRF,
                                         (0 == ((int32_t)arg->e % 2)));
}

static kode_status_t do_reset(const kode_value_t *arg)
{
    return (NULL == arg) ? KODE_DENIED : fck_sys_reset(&l_ao.super, chip_of((int32_t)arg->e));
}

static kode_status_t do_gnss(const kode_value_t *arg)
{
    if ( NULL == arg )
    {
        return KODE_DENIED;
    }
    return (0 == (int32_t)arg->e) ? fck_sys_gnss_begin(&l_ao.super) : fck_sys_gnss_end(&l_ao.super);
}

static kode_status_t do_ble_scan(const kode_value_t *arg)
{
    if ( NULL == arg )
    {
        return KODE_DENIED;
    }
    kode_status_t const rc = fck_sys_ble_scan(&l_ao.super, (uint8_t)arg->i);
    if ( KODE_OK == rc )
    {
        l_ao.scan_s = (uint8_t)arg->i;
    }
    return rc;
}

static kode_status_t do_rescue(const kode_value_t *arg)
{
    if ( NULL == arg )
    {
        return KODE_DENIED;
    }
    kode_status_t const rc = fck_sys_rescue_probe(&l_ao.super, chip_of((int32_t)arg->e), &l_ao.rescue);
    if ( KODE_OK == rc )
    {
        l_ao.rescue_c5 = (0 == (int32_t)arg->e);
    }
    return rc;
}

static const char * const CHIPS[]  = { "c5", "nrf" };
static const char * const POWERS[] = { "c5_on", "c5_off", "nrf_on", "nrf_off" };
static const char * const ONOFF[]  = { "on", "off" };

static const kode_function_t FUNCTIONS[] = {
    { .name = "fck_firmware", .arg = KODE_TYPE_ENUM, .enums = CHIPS, .n_enums = 2U, .run = do_firmware,
      .desc = "What a chip runs, on the log: c5 or nrf" },
    { .name = "fck_power", .arg = KODE_TYPE_ENUM, .enums = POWERS, .n_enums = 4U, .run = do_power,
      .desc = "A chip let run or held in reset" },
    { .name = "fck_reset", .arg = KODE_TYPE_ENUM, .enums = CHIPS, .n_enums = 2U, .run = do_reset,
      .desc = "Restart a chip: c5 or nrf" },
    { .name = "fck_gnss", .arg = KODE_TYPE_ENUM, .enums = ONOFF, .n_enums = 2U, .run = do_gnss,
      .desc = "The GNSS receiver: on or off" },
    { .name = "fck_ble_scan", .arg = KODE_TYPE_INT, .min = 1, .max = 30, .run = do_ble_scan,
      .desc = "BLE devices counted, passively, s" },
    { .name = "fck_rescue", .arg = KODE_TYPE_ENUM, .enums = CHIPS, .n_enums = 2U, .run = do_rescue,
      .desc = "What a chip carries; resets it: c5 or nrf" },
};

/* ---- Variables ---------------------------------------------------------- */

static kode_status_t get_present(char *buf, size_t cap)
{
    (void)snprintf(buf, cap, "%s", fck_sys_is_present() ? "on" : "off");
    return KODE_OK;
}

static kode_status_t get_generation(char *buf, size_t cap)
{
    fck_status_t st;
    (void)fck_sys_status_read(&st);
    (void)snprintf(buf, cap, "%lu", (unsigned long)st.generation);
    return KODE_OK;
}

/**
 * @brief A chip's word: on, reset, or none without a module.
 */
static kode_status_t chip_word(char *buf, size_t cap, bool const nrf)
{
    fck_status_t st;
    (void)fck_sys_status_read(&st);
    bool const on = nrf ? st.nrf_on : st.c5_on;
    (void)snprintf(buf, cap, "%s", !st.present ? "none" : (on ? "on" : "reset"));
    return KODE_OK;
}

static kode_status_t get_c5(char *buf, size_t cap)
{
    return chip_word(buf, cap, false);
}

static kode_status_t get_nrf(char *buf, size_t cap)
{
    return chip_word(buf, cap, true);
}

static kode_status_t get_cc1101(char *buf, size_t cap)
{
    fck_status_t st;
    (void)fck_sys_status_read(&st);
    (void)snprintf(buf, cap, "%s", !st.present ? "none" : (st.cc1101_ok ? "asleep" : "missing"));
    return KODE_OK;
}

static kode_status_t get_gnss(char *buf, size_t cap)
{
    fck_status_t st;
    (void)fck_sys_status_read(&st);
    (void)snprintf(buf, cap, "%s", !st.present ? "none" : (st.gnss_running ? "running" : "reset"));
    return KODE_OK;
}

static const kode_variable_t VARIABLES[] = {
    { .name = "fck_present", .get = get_present, .desc = "F*CK on J3: on, off" },
    { .name = "fck_generation", .get = get_generation, .desc = "Modules set up since start" },
    { .name = "fck_c5", .get = get_c5, .desc = "The C5: on, reset, none" },
    { .name = "fck_nrf", .get = get_nrf, .desc = "The nRF: on, reset, none" },
    { .name = "fck_cc1101", .get = get_cc1101, .desc = "The CC1101: asleep, missing, none" },
    { .name = "fck_gnss_state", .get = get_gnss, .desc = "The receiver: running, reset, none" },
};

/* ---- What came of each request, on the log ---------------------------------- */

/**
 * @brief A chip's version as " 0.1.0", or "" for one that printed none: a 0 is a number there.
 *
 * @param a   What the chip said.
 * @param out Where the text goes.
 * @param cap Its room.
 */
static void version_text(fck_firmware_t const * const a, char * const out, size_t const cap)
{
    out[0] = '\0';
    if ( a->has_version )
    {
        (void)snprintf(out, cap, " %u.%u.%u", (unsigned)a->ver_major, (unsigned)a->ver_minor,
                       (unsigned)a->ver_patch);
    }
}

/**
 * @brief What fck_sys_rescue_probe() found, on the log: the state, the boot code
 *        (the C5's ROM loader, or MCUboot and slot 0), and what the image
 *        said.
 * @param name The function's name.
 * @param c5   The report is the C5's.
 * @param r    The report.
 */
static void show_rescue(char const * const name, bool const c5,
                        fck_rescue_report_t const * const r)
{
    static const char * const STATE[] = { "the firmware runs", "another firmware runs",
                                          "the image says nothing", "only the boot code runs",
                                          "nothing answers" };
    char const * const state = ((unsigned)r->state < Q_DIM(STATE)) ? STATE[r->state] : "?";

    if ( c5 )
    {
        KLOG_I(TAG, "%s: %s; ROM loader %s%s; its ROM started %lu time(s); %lu ms", name, state,
               r->bootloader ? "answered" : "silent", r->locked ? ", LOCKED" : "",
               (unsigned long)r->restarts, (unsigned long)r->ms);
        KLOG_I(TAG, "%s: MAC %02x:%02x:%02x:%02x:%02x:%02x, %lu KB of flash", name, r->mac[0],
               r->mac[1], r->mac[2], r->mac[3], r->mac[4], r->mac[5],
               (unsigned long)(r->flash_size / 1024U));
    }
    else if ( r->bootloader )
    {
        KLOG_I(TAG, "%s: %s; MCUboot answered at %lu ms; slot 0 %s %s %02x%02x%02x%02x; %lu ms", name,
               state, (unsigned long)r->bootloader_ms, r->image ? "holds" : "holds nothing",
               r->version, r->hash[0], r->hash[1], r->hash[2], r->hash[3],
               (unsigned long)r->ms);
    }
    else
    {
        KLOG_I(TAG, "%s: %s; no MCUboot window; %lu ms", name, state, (unsigned long)r->ms);
    }
    if ( '\0' != r->line[0] )
    {
        KLOG_I(TAG, "%s: its first line: %s", name, r->line);
    }
    if ( r->firmware.known )
    {
        char ver[16];
        version_text(&r->firmware, ver, sizeof(ver));
        KLOG_I(TAG, "%s: it says %s%s", name, r->firmware.fw, ver);
    }
}

/**
 * @brief The answer of a request that did not go well, as a sentence.
 * @param name The function's name.
 * @param a    The answer.
 */
static void refused(char const * const name, fck_sys_answer_t const * const a)
{
    KLOG_W(TAG, "%s: refused (status %d): %s", name, (int)a->rc, fck_why_text(a->why));
}

/**
 * @brief What an answer came to, on the log.
 * @param me  The AO.
 * @param a   The answer.
 * @param sig Which request it answers.
 */
static void show(ConsoleActions const * const me, fck_sys_answer_t const * const a, QSignal const sig)
{
    if ( KODE_OK != a->rc )
    {
        refused((FCK_FIRMWARE_RSP_SIG == sig)           ? "fck_firmware"
                : (FCK_POWER_RSP_SIG == sig)         ? "fck_power"
                : (FCK_RESET_RSP_SIG == sig)         ? "fck_reset"
                : (FCK_BLE_SCAN_RSP_SIG == sig)      ? "fck_ble_scan"
                : (FCK_RESCUE_PROBE_RSP_SIG == sig)  ? "fck_rescue"
                                                     : "fck_gnss",
                a);
        return;
    }
    switch ( sig )
    {
        case FCK_FIRMWARE_RSP_SIG:
            if ( !me->firmware.known )
            {
                KLOG_I(TAG, "fck_firmware: the chip said nothing");
            }
            else
            {
                char ver[16];
                version_text(&me->firmware, ver, sizeof(ver));
                KLOG_I(TAG, "fck_firmware: %s%s, %s%s", me->firmware.fw, ver,
                       me->firmware.official ? "the module's firmware" : "a firmware of its own",
                       me->firmware.official ? "" : (me->firmware.compatible ? ", speaks the SDK's words" : ", not the SDK's words"));
            }
            break;
        case FCK_BLE_SCAN_RSP_SIG:
            KLOG_I(TAG, "fck_ble_scan: %lu device(s) advertised in %u s", (unsigned long)a->count,
                   (unsigned)me->scan_s);
            break;
        case FCK_RESCUE_PROBE_RSP_SIG:
            show_rescue("fck_rescue", me->rescue_c5, &me->rescue);
            break;
        default:
            KLOG_I(TAG, "%s: done",
                   (FCK_POWER_RSP_SIG == sig) ? "fck_power"
                   : (FCK_RESET_RSP_SIG == sig) ? "fck_reset" : "fck_gnss");
            break;
    }
}

/* ---- The Active Object -------------------------------------------------- */

void ConsoleActions_ctor(void)
{
    ConsoleActions * const me = &l_ao;
    QActive_ctor(&me->super, Q_STATE_CAST(&ConsoleActions_initial));
    me->rescue_c5 = false;
    me->scan_s    = 0U;
}

void ConsoleActions_start(void)
{
    QActive_setAttr(AO_ConsoleActions, TASK_NAME_ATTR, "console_actions");
    QACTIVE_START(AO_ConsoleActions, KODE_PRIO_USER_LOW,
                  l_queue, Q_DIM(l_queue),
                  l_stack, sizeof(l_stack),
                  (void *)0);

    /* A table with one bad row is refused whole, with a log line naming the
     * row; a name the app already uses answers KODE_EXISTS. */
    if ( KODE_FUNCTIONS_ADD(FUNCTIONS) != KODE_OK )
    {
        KLOG_E(TAG, "the console refused the functions");
    }
    if ( KODE_VARIABLES_ADD(VARIABLES) != KODE_OK )
    {
        KLOG_E(TAG, "the console refused the variables");
    }
}

/**
 * @brief The initial transition: subscribe to the news, then start the SDK.
 *
 * Runs in the thread that called kode_init(). The subscription comes first, so the module
 * being found is never missed.
 *
 * @param me  The AO.
 * @param par Unused.
 * @return The transition to ConsoleActions_serving.
 */
static QState ConsoleActions_initial(ConsoleActions * const me, void const * const par)
{
    (void)par;

    if ( KODE_OK != fck_sys_subscribe(&me->super) )
    {
        KLOG_E(TAG, "no subscription; the module will not be heard of");
    }
    kode_status_t const rc = fck_sys_begin();
    if ( KODE_OK != rc )
    {
        KLOG_E(TAG, "the SDK did not start (status %d)", (int)rc);
    }

    KLOG_I(TAG, "up; the module's rows are on the console");
    return Q_TRAN(&ConsoleActions_serving);
}

/**
 * @brief The one state: the answers to the log, and the module watched through the news.
 * @param me The AO.
 * @param e  The event.
 * @return Q_HANDLED() for its own events, Q_SUPER() otherwise.
 */
static QState ConsoleActions_serving(ConsoleActions * const me, QEvt const * const e)
{
    QState status = Q_HANDLED();

    switch ( e->sig )
    {
        case FCK_MODULE_FOUND_SIG: {
            KLOG_I(TAG, "the module is on J3");
            break;
        }
        case FCK_MODULE_LOST_SIG: {
            KLOG_W(TAG, "the module left J3");
            break;
        }
        case FCK_WRONG_MODULE_SIG: {
            KLOG_W(TAG, "that is not an F*CK");
            break;
        }
        case FCK_FIRMWARE_RSP_SIG:
        case FCK_POWER_RSP_SIG:
        case FCK_RESET_RSP_SIG:
        case FCK_GNSS_BEGIN_RSP_SIG:
        case FCK_GNSS_END_RSP_SIG:
        case FCK_BLE_SCAN_RSP_SIG:
        case FCK_RESCUE_PROBE_RSP_SIG: {
            show(me, Q_EVT_CAST(fck_sys_answer_t), e->sig);
            break;
        }
        default: {
            status = Q_SUPER(&QHsm_top);
            break;
        }
    }

    return status;
}
