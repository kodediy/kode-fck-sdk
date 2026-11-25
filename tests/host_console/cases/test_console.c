/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_console.c
 * @brief examples/console_actions over a recording fck_sys.h: its tables kept to
 *        kodeOS's rules, every function making the request its name says and answering the
 *        console with what the SDK said, every answer and every piece of news a line on the
 *        log, every variable a word the console sends, in every state.
 *
 * The example is built into this file, so a case carries a request as the board does: the row
 * makes it (console_fake_run()) and returns at once, and the module's answer is an event the
 * case delivers to the Active Object's one state (answer()). Nothing here waits: there is no
 * worker and no queue in the example any more.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "unity.h"

#include "fake_console.h"
#include "fake_fck.h"
#include "fake_kode.h"

#include "console_actions_ao.c"

/** @brief Functions on the console when the initial transition ran. */
static size_t s_functions_at_initial;

/** @brief What QACTIVE_START() runs, where QP/C takes the initial transition. */
static void initial(void)
{
    s_functions_at_initial = console_fake_functions();
    TEST_ASSERT_EQUAL_UINT8(Q_TRAN(&ConsoleActions_serving), ConsoleActions_initial(&l_ao, NULL));
}

/** @brief Nothing recorded, no module, every answer KODE_OK. */
static void fakes_reset(void)
{
    kode_fake_reset();
    fck_mock_reset();
    console_fake_reset();
    g_kode.initial = initial;
}

/** @brief The example brought up as its main() does. */
static void console_up(void)
{
    ConsoleActions_ctor();
    ConsoleActions_start();
}

static void console_reset(void)
{
    fakes_reset();
    console_up();
}

/** @brief The module's answer to a request, delivered to the AO's step. */
static void answer(QSignal sig, kode_status_t rc, fck_why_t why, uint32_t count)
{
    fck_sys_answer_t a;
    (void)memset(&a, 0, sizeof(a));
    a.super.sig = sig;
    a.rc        = rc;
    a.why       = why;
    a.count     = count;
    TEST_ASSERT_EQUAL_UINT8(Q_HANDLED(), ConsoleActions_serving(&l_ao, &a.super));
}

/** @brief A piece of news, delivered the same way. */
static void news(QSignal sig, int32_t value)
{
    fck_sys_news_t n;
    n.super.sig = sig;
    n.value     = value;
    TEST_ASSERT_EQUAL_UINT8(Q_HANDLED(), ConsoleActions_serving(&l_ao, &n.super));
}

/**
 * @brief `function name=@p name` with @p v: the row makes one request, @p call, FOR the AO, and
 *        the console is told KODE_OK.
 */
static void asks(const char *name, const kode_value_t *v, fck_call_t call)
{
    unsigned const before = g_fck.calls;
    TEST_ASSERT_EQUAL_MESSAGE(KODE_OK, console_fake_run(name, v), name);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(before + 1U, g_fck.calls, name);
    TEST_ASSERT_EQUAL_MESSAGE(call, g_fck.last.call, name);
    TEST_ASSERT_EQUAL_PTR_MESSAGE(AO_ConsoleActions, g_fck.last.me, name);
}

/** @brief Word @p e of enum row @p name carried out: @p call, on @p chip. */
static void asks_chip(const char *name, uint8_t e, fck_call_t call, fck_chip_t chip)
{
    kode_value_t v = { 0 };
    v.e = e;
    asks(name, &v, call);
    TEST_ASSERT_EQUAL_MESSAGE(chip, g_fck.last.chip, name);
}

/** @brief `function name=@p name` refused by the console before the row sees it. */
static void refused_early(const char *name, const kode_value_t *v)
{
    unsigned const before = g_fck.calls;
    TEST_ASSERT_EQUAL_MESSAGE(KODE_DENIED, console_fake_run(name, v), name);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(before, g_fck.calls, name);
}

/** @brief Read variable @p name and check its word is @p want. */
static void reads(const char *name, const char *want)
{
    char word[KODE_VALUE_STR_MAX + 1U];
    TEST_ASSERT_EQUAL_MESSAGE(KODE_OK, console_fake_read(name, word), name);
    TEST_ASSERT_EQUAL_STRING_MESSAGE(want, word, name);
}

TEST_CASE("ConsoleActions_start() subscribes, starts the SDK, then gives the console six functions and six variables, once", "[console]")
{
    console_reset();
    TEST_ASSERT_EQUAL_UINT(1U, g_fck.subscribes);
    TEST_ASSERT_EQUAL_PTR(AO_ConsoleActions, g_fck.subscriber);
    TEST_ASSERT_EQUAL_UINT(1U, g_fck.begins);
    TEST_ASSERT_EQUAL_UINT(0U, s_functions_at_initial);   /* the SDK first, then the rows */
    TEST_ASSERT_EQUAL_UINT(6U, console_fake_functions());
    TEST_ASSERT_EQUAL_UINT(6U, console_fake_variables());
    TEST_ASSERT_EQUAL_UINT(0U, g_kode.errors);

    /* Its own task and queue at the app's level; a queue of 32 for its four doors, its news and a scan's entries. */
    TEST_ASSERT_EQUAL_PTR(AO_ConsoleActions, g_kode.ao);
    TEST_ASSERT_EQUAL_STRING("console_actions", g_kode.task_name);
    TEST_ASSERT_EQUAL_UINT8(KODE_PRIO_USER_LOW, g_kode.prio);
    TEST_ASSERT_EQUAL_UINT(2U * KQP_AO_QUEUE_LEN, g_kode.queue_len);

    /* Nothing asked of the module until a person types it. */
    TEST_ASSERT_EQUAL_UINT(0U, g_fck.calls);

    g_kode.initial = NULL;
    ConsoleActions_start();   /* the names are taken: both tables refused whole */
    TEST_ASSERT_EQUAL_UINT(6U, console_fake_functions());
    TEST_ASSERT_EQUAL_UINT(6U, console_fake_variables());
    TEST_ASSERT_TRUE(kode_fake_logged("the console refused the functions"));
    TEST_ASSERT_TRUE(kode_fake_logged("the console refused the variables"));
}

TEST_CASE("a subscription or a start that fails is an error on the log, and the rows are still given", "[console]")
{
    fakes_reset();
    g_fck.subscribe_answer = KODE_DENIED;
    console_up();
    TEST_ASSERT_EQUAL_UINT(1U, g_kode.errors);
    TEST_ASSERT_TRUE(kode_fake_logged("no subscription; the module will not be heard of"));
    TEST_ASSERT_EQUAL_UINT(1U, g_fck.begins);
    TEST_ASSERT_EQUAL_UINT(6U, console_fake_functions());

    fakes_reset();
    g_fck.begin_answer = KODE_EXISTS;
    console_up();
    TEST_ASSERT_EQUAL_UINT(1U, g_kode.errors);
    TEST_ASSERT_TRUE(kode_fake_logged("the SDK did not start (status"));
    TEST_ASSERT_EQUAL_UINT(6U, console_fake_functions());
}

static kode_status_t app_get(char *buf, size_t cap)
{
    (void)buf;
    (void)cap;
    return KODE_OK;
}

static kode_status_t app_run(const kode_value_t *arg)
{
    (void)arg;
    return KODE_OK;
}

TEST_CASE("an app's row named like one of the example's is refused, whichever its kind", "[console]")
{
    console_reset();
    static const kode_variable_t SAME_VARIABLE[] = {
        { .name = "fck_present", .get = app_get, .desc = "the app's" },
    };
    static const kode_function_t FUNCTION_AS_VARIABLE[] = {
        { .name = "fck_c5", .run = app_run, .desc = "the app's" },
    };
    /* kodeOS refuses a variable named like a function, and the whole table with it. */
    static const kode_variable_t VARIABLE_AS_FUNCTION[] = {
        { .name = "fck_gnss", .get = app_get, .desc = "the app's" },
    };
    TEST_ASSERT_EQUAL(KODE_EXISTS, KODE_VARIABLES_ADD(SAME_VARIABLE));
    TEST_ASSERT_EQUAL(KODE_EXISTS, KODE_FUNCTIONS_ADD(FUNCTION_AS_VARIABLE));
    TEST_ASSERT_EQUAL(KODE_EXISTS, KODE_VARIABLES_ADD(VARIABLE_AS_FUNCTION));
    TEST_ASSERT_EQUAL_UINT(6U, console_fake_functions());
    TEST_ASSERT_EQUAL_UINT(6U, console_fake_variables());
}

TEST_CASE("each console function makes the request its name says, for the AO, and returns at once", "[console]")
{
    console_reset();
    g_fck.st.present = true;

    asks_chip("fck_firmware", 0U, FCK_CALL_FIRMWARE, FCK_C5);
    TEST_ASSERT_EQUAL_PTR(&l_ao.firmware, g_fck.last.lent);
    asks_chip("fck_firmware", 1U, FCK_CALL_FIRMWARE, FCK_NRF);

    /* c5_on c5_off nrf_on nrf_off */
    asks_chip("fck_power", 0U, FCK_CALL_POWER, FCK_C5);
    TEST_ASSERT_TRUE(g_fck.last.on);
    asks_chip("fck_power", 1U, FCK_CALL_POWER, FCK_C5);
    TEST_ASSERT_FALSE(g_fck.last.on);
    asks_chip("fck_power", 2U, FCK_CALL_POWER, FCK_NRF);
    TEST_ASSERT_TRUE(g_fck.last.on);
    asks_chip("fck_power", 3U, FCK_CALL_POWER, FCK_NRF);
    TEST_ASSERT_FALSE(g_fck.last.on);

    asks_chip("fck_reset", 0U, FCK_CALL_RESET, FCK_C5);
    asks_chip("fck_reset", 1U, FCK_CALL_RESET, FCK_NRF);

    kode_value_t v = { 0 };
    v.e = 0U;
    asks("fck_gnss", &v, FCK_CALL_GNSS_START);
    v.e = 1U;
    asks("fck_gnss", &v, FCK_CALL_GNSS_STOP);

    v.i = 5;
    asks("fck_ble_scan", &v, FCK_CALL_BLE_SCAN);
    TEST_ASSERT_EQUAL_UINT8(5U, g_fck.last.secs);
    TEST_ASSERT_EQUAL_UINT8(5U, l_ao.scan_s);

    asks_chip("fck_rescue", 0U, FCK_CALL_RESCUE_PROBE, FCK_C5);
    TEST_ASSERT_TRUE(l_ao.rescue_c5);
    TEST_ASSERT_EQUAL_PTR(&l_ao.rescue, g_fck.last.lent);
    asks_chip("fck_rescue", 1U, FCK_CALL_RESCUE_PROBE, FCK_NRF);
    TEST_ASSERT_FALSE(l_ao.rescue_c5);

    TEST_ASSERT_EQUAL_UINT(0U, g_fck.bad_args);
    TEST_ASSERT_EQUAL_UINT(0U, g_kode.warnings);
    TEST_ASSERT_EQUAL_UINT(0U, g_kode.errors);
}

TEST_CASE("a request the SDK does not take is the console's answer, and nothing is remembered of it", "[console]")
{
    static const kode_status_t REFUSALS[] = { KODE_DENIED, KODE_NO_RESOURCES };
    console_reset();
    g_fck.st.present = true;
    l_ao.scan_s = 7U;
    for (size_t k = 0U; k < Q_DIM(REFUSALS); k++) {
        g_fck.answer = REFUSALS[k];
        kode_value_t v = { 0 };
        v.i = 9;
        TEST_ASSERT_EQUAL(REFUSALS[k], console_fake_run("fck_ble_scan", &v));
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(7U, l_ao.scan_s, "the seconds of a scan not taken are not kept");
        v.e = 0U;
        TEST_ASSERT_EQUAL(REFUSALS[k], console_fake_run("fck_firmware", &v));
        TEST_ASSERT_EQUAL(REFUSALS[k], console_fake_run("fck_power", &v));
        TEST_ASSERT_EQUAL(REFUSALS[k], console_fake_run("fck_reset", &v));
        TEST_ASSERT_EQUAL(REFUSALS[k], console_fake_run("fck_gnss", &v));
        l_ao.rescue_c5 = true;
        TEST_ASSERT_EQUAL(REFUSALS[k], console_fake_run("fck_rescue", &v));
        v.e = 1U;
        TEST_ASSERT_EQUAL(REFUSALS[k], console_fake_run("fck_rescue", &v));
        TEST_ASSERT_TRUE_MESSAGE(l_ao.rescue_c5, "the chip of a probe not taken is not kept");
    }
    TEST_ASSERT_EQUAL_UINT(0U, g_kode.warnings);   /* the console says it; the AO has nothing to log */
}

/** @brief Row @p name takes an int from @p lo to @p hi, as function_list will say. */
static void range_is(const char *name, int32_t lo, int32_t hi)
{
    const kode_function_t *const r = console_fake_function(name);
    TEST_ASSERT_NOT_NULL_MESSAGE(r, name);
    TEST_ASSERT_EQUAL_MESSAGE(KODE_TYPE_INT, r->arg, name);
    TEST_ASSERT_EQUAL_INT32_MESSAGE(lo, r->min, name);
    TEST_ASSERT_EQUAL_INT32_MESSAGE(hi, r->max, name);
}

/** @brief Row @p name takes one of @p n words; one past them, or none, is refused. */
static void words_are(const char *name, uint8_t n)
{
    const kode_function_t *const r = console_fake_function(name);
    TEST_ASSERT_NOT_NULL_MESSAGE(r, name);
    TEST_ASSERT_EQUAL_MESSAGE(KODE_TYPE_ENUM, r->arg, name);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(n, r->n_enums, name);
    kode_value_t v = { 0 };
    v.e = n;
    refused_early(name, &v);
    refused_early(name, NULL);   /* the line carried no arg=: the row refuses it */
}

TEST_CASE("the rows ask only what fck_sys.h takes: the ends pass, past them is refused", "[console]")
{
    console_reset();
    g_fck.st.present = true;

    /* fck_sys.h scans for 1 to 60 s; the row offers 1 to 30. */
    range_is("fck_ble_scan", 1, 30);
    kode_value_t v = { 0 };
    v.i = 1;
    asks("fck_ble_scan", &v, FCK_CALL_BLE_SCAN);
    TEST_ASSERT_EQUAL_UINT8(1U, g_fck.last.secs);
    v.i = 30;
    asks("fck_ble_scan", &v, FCK_CALL_BLE_SCAN);
    TEST_ASSERT_EQUAL_UINT8(30U, g_fck.last.secs);
    v.i = 0;
    refused_early("fck_ble_scan", &v);
    v.i = 31;
    refused_early("fck_ble_scan", &v);
    refused_early("fck_ble_scan", NULL);

    words_are("fck_firmware", 2U);
    words_are("fck_power", 4U);
    words_are("fck_reset", 2U);
    words_are("fck_gnss", 2U);
    words_are("fck_rescue", 2U);

    TEST_ASSERT_EQUAL_UINT(0U, g_fck.bad_args);
    TEST_ASSERT_EQUAL_UINT(0U, g_kode.warnings);
}

TEST_CASE("each console variable is one word the console sends, in every state", "[console]")
{
    console_reset();
    fck_status_t *const st = &g_fck.st;

    /* No module: "none" for every chip, whatever the flags were left at. */
    st->c5_on        = true;
    st->nrf_on       = true;
    st->cc1101_ok    = true;
    st->gnss_running = true;
    reads("fck_present", "off");
    reads("fck_generation", "0");
    reads("fck_c5", "none");
    reads("fck_nrf", "none");
    reads("fck_cc1101", "none");
    reads("fck_gnss_state", "none");

    st->present = true;
    reads("fck_present", "on");
    reads("fck_c5", "on");
    reads("fck_nrf", "on");
    reads("fck_cc1101", "asleep");
    reads("fck_gnss_state", "running");

    st->c5_on        = false;
    st->nrf_on       = false;
    st->cc1101_ok    = false;
    st->gnss_running = false;
    reads("fck_c5", "reset");
    reads("fck_nrf", "reset");
    reads("fck_cc1101", "missing");
    reads("fck_gnss_state", "reset");

    st->generation = UINT32_MAX;
    reads("fck_generation", "4294967295");
}

TEST_CASE("the module's news is a line on the log, and nothing else is asked", "[console]")
{
    console_reset();
    unsigned const lines = g_kode.infos + g_kode.warnings;

    news(FCK_MODULE_FOUND_SIG, 1);
    TEST_ASSERT_EQUAL_STRING("the module is on J3", kode_fake_last_line());
    news(FCK_MODULE_LOST_SIG, 0);
    TEST_ASSERT_EQUAL_STRING("the module left J3", kode_fake_last_line());
    news(FCK_WRONG_MODULE_SIG, FCK_WRONG_MODULE_HOLDS_PAIR);
    TEST_ASSERT_EQUAL_STRING("that is not an F*CK", kode_fake_last_line());
    TEST_ASSERT_EQUAL_UINT(lines + 3U, g_kode.infos + g_kode.warnings);
    TEST_ASSERT_EQUAL_UINT(0U, g_fck.calls);   /* nothing is asked of a module that has just come back */

    /* What is not its own goes up to the top state, and nothing is said. */
    fck_sys_news_t other = { .super = { .sig = (QSignal)(MAX_SIG + 5) }, .value = 0 };
    TEST_ASSERT_EQUAL_UINT8(Q_SUPER(&QHsm_top), ConsoleActions_serving(&l_ao, &other.super));
    other.super.sig = FCK_PACKET_RECEIVED_SIG;   /* news it did not ask for in its handler */
    TEST_ASSERT_EQUAL_UINT8(Q_SUPER(&QHsm_top), ConsoleActions_serving(&l_ao, &other.super));
    TEST_ASSERT_EQUAL_UINT(lines + 3U, g_kode.infos + g_kode.warnings);
}

TEST_CASE("each answer is a line: what the chip said, or why it was refused", "[console]")
{
    console_reset();

    /* fck_firmware: nothing, then a greeting. */
    answer(FCK_FIRMWARE_RSP_SIG, KODE_OK, FCK_WHY_NONE, 0U);
    TEST_ASSERT_EQUAL_STRING("fck_firmware: the chip said nothing", kode_fake_last_line());
    l_ao.firmware.known     = true;
    l_ao.firmware.official     = true;
    l_ao.firmware.compatible = true;
    (void)strcpy(l_ao.firmware.fw, "nrf_fck_agent");
    l_ao.firmware.has_version = true;
    l_ao.firmware.ver_major = 2U;
    l_ao.firmware.ver_minor = 0U;
    l_ao.firmware.ver_patch = 1U;
    answer(FCK_FIRMWARE_RSP_SIG, KODE_OK, FCK_WHY_NONE, 0U);
    TEST_ASSERT_EQUAL_STRING("fck_firmware: nrf_fck_agent 2.0.1, the module's firmware",
                             kode_fake_last_line());
    l_ao.firmware.official     = false;
    l_ao.firmware.compatible = false;
    answer(FCK_FIRMWARE_RSP_SIG, KODE_OK, FCK_WHY_NONE, 0U);
    TEST_ASSERT_EQUAL_STRING("fck_firmware: nrf_fck_agent 2.0.1, a firmware of its own, not the SDK's words",
                             kode_fake_last_line());
    l_ao.firmware.compatible = true;
    answer(FCK_FIRMWARE_RSP_SIG, KODE_OK, FCK_WHY_NONE, 0U);
    TEST_ASSERT_EQUAL_STRING("fck_firmware: nrf_fck_agent 2.0.1, a firmware of its own, speaks the SDK's words",
                             kode_fake_last_line());

    answer(FCK_POWER_RSP_SIG, KODE_OK, FCK_WHY_NONE, 0U);
    TEST_ASSERT_EQUAL_STRING("fck_power: done", kode_fake_last_line());
    answer(FCK_RESET_RSP_SIG, KODE_OK, FCK_WHY_NONE, 0U);
    TEST_ASSERT_EQUAL_STRING("fck_reset: done", kode_fake_last_line());
    answer(FCK_GNSS_BEGIN_RSP_SIG, KODE_OK, FCK_WHY_NONE, 0U);
    TEST_ASSERT_EQUAL_STRING("fck_gnss: done", kode_fake_last_line());
    answer(FCK_GNSS_END_RSP_SIG, KODE_OK, FCK_WHY_NONE, 0U);
    TEST_ASSERT_EQUAL_STRING("fck_gnss: done", kode_fake_last_line());

    l_ao.scan_s = 5U;
    answer(FCK_BLE_SCAN_RSP_SIG, KODE_OK, FCK_WHY_NONE, 3U);
    TEST_ASSERT_EQUAL_STRING("fck_ble_scan: 3 device(s) advertised in 5 s", kode_fake_last_line());
    TEST_ASSERT_EQUAL_UINT(0U, g_kode.warnings);

    /* A BLE scan's entries are not this AO's business: the count is in the answer. */
    fck_sys_ble_entry_t entry;
    (void)memset(&entry, 0, sizeof(entry));
    entry.super.sig = FCK_BLE_SCAN_ENTRY_SIG;
    unsigned const lines = g_kode.infos + g_kode.warnings;
    TEST_ASSERT_EQUAL_UINT8(Q_SUPER(&QHsm_top), ConsoleActions_serving(&l_ao, &entry.super));
    TEST_ASSERT_EQUAL_UINT(lines, g_kode.infos + g_kode.warnings);
}

TEST_CASE("an answer that failed is a warning with its status and its sentence, one per request", "[console]")
{
    static const struct { QSignal sig; const char *name; } WHAT[] = {
        { FCK_FIRMWARE_RSP_SIG, "fck_firmware" },       { FCK_POWER_RSP_SIG, "fck_power" },
        { FCK_RESET_RSP_SIG, "fck_reset" },       { FCK_GNSS_BEGIN_RSP_SIG, "fck_gnss" },
        { FCK_GNSS_END_RSP_SIG, "fck_gnss" },     { FCK_BLE_SCAN_RSP_SIG, "fck_ble_scan" },
        { FCK_RESCUE_PROBE_RSP_SIG, "fck_rescue" },
    };
    char want[KODE_FAKE_LINE];

    console_reset();
    unsigned warnings = 0U;
    for (size_t k = 0U; k < Q_DIM(WHAT); k++) {
        answer(WHAT[k].sig, KODE_DENIED, FCK_WHY_NO_MODULE, 0U);
        warnings++;
        (void)snprintf(want, sizeof(want), "%s: refused (status %d): no module on J3", WHAT[k].name,
                       (int)KODE_DENIED);
        TEST_ASSERT_EQUAL_STRING(want, kode_fake_last_line());
        TEST_ASSERT_EQUAL_UINT(warnings, g_kode.warnings);
        answer(WHAT[k].sig, KODE_TIMEOUT, FCK_WHY_TIMEOUT, 0U);
        warnings++;
        (void)snprintf(want, sizeof(want), "%s: refused (status %d): another reason", WHAT[k].name,
                       (int)KODE_TIMEOUT);
        TEST_ASSERT_EQUAL_STRING(want, kode_fake_last_line());
    }
}

TEST_CASE("fck_rescue's report reads in fck_sys.h's states, in their order", "[console]")
{
    static const fck_rescue_state_t STATE[] = { FCK_RESCUE_OFFICIAL, FCK_RESCUE_FIRMWARE,
                                                FCK_RESCUE_QUIET, FCK_RESCUE_BOOTLOADER,
                                                FCK_RESCUE_NOTHING };
    static const char *const WORD[] = { "the firmware runs", "another firmware runs",
                                        "the image says nothing", "only the boot code runs",
                                        "nothing answers" };
    char want[KODE_FAKE_LINE];

    console_reset();
    g_fck.st.present = true;
    for (size_t k = 0U; k < Q_DIM(STATE); k++) {
        l_ao.rescue.state = STATE[k];
        asks_chip("fck_rescue", 1U, FCK_CALL_RESCUE_PROBE, FCK_NRF);
        answer(FCK_RESCUE_PROBE_RSP_SIG, KODE_OK, FCK_WHY_NONE, 0U);
        (void)snprintf(want, sizeof(want), "fck_rescue: %s; no MCUboot window; 0 ms", WORD[k]);
        TEST_ASSERT_EQUAL_STRING(want, kode_fake_last_line());
    }
    l_ao.rescue.state = (fck_rescue_state_t)9;   /* a state from a newer fck.h */
    answer(FCK_RESCUE_PROBE_RSP_SIG, KODE_OK, FCK_WHY_NONE, 0U);
    TEST_ASSERT_EQUAL_STRING("fck_rescue: ?; no MCUboot window; 0 ms", kode_fake_last_line());

    /* The nRF behind MCUboot, slot 0 holding the firmware. */
    static const uint8_t HASH[4] = { 0xa1U, 0xb2U, 0xc3U, 0xd4U };
    l_ao.rescue.state         = FCK_RESCUE_OFFICIAL;
    l_ao.rescue.bootloader    = true;
    l_ao.rescue.bootloader_ms = 120U;
    l_ao.rescue.image         = true;
    (void)strcpy(l_ao.rescue.version, "2.0.1");
    (void)memcpy(l_ao.rescue.hash, HASH, sizeof(HASH));
    l_ao.rescue.ms = 900U;
    (void)strcpy(l_ao.rescue.line, "nrf_fck_agent 2.0.1");
    l_ao.rescue.firmware.known = true;
    (void)strcpy(l_ao.rescue.firmware.fw, "nrf_fck_agent");
    l_ao.rescue.firmware.has_version = true;
    l_ao.rescue.firmware.ver_major = 2U;
    l_ao.rescue.firmware.ver_minor = 0U;
    l_ao.rescue.firmware.ver_patch = 1U;
    answer(FCK_RESCUE_PROBE_RSP_SIG, KODE_OK, FCK_WHY_NONE, 0U);
    TEST_ASSERT_TRUE(kode_fake_logged(
        "fck_rescue: the firmware runs; MCUboot answered at 120 ms; slot 0 holds 2.0.1 a1b2c3d4; 900 ms"));
    TEST_ASSERT_TRUE(kode_fake_logged("fck_rescue: its first line: nrf_fck_agent 2.0.1"));
    TEST_ASSERT_EQUAL_STRING("fck_rescue: it says nrf_fck_agent 2.0.1", kode_fake_last_line());

    /* The C5: only its ROM loader, locked, and its ROM started three times. */
    static const uint8_t MAC[6] = { 0x24U, 0x58U, 0x7cU, 0x01U, 0x02U, 0x03U };
    (void)memset(&l_ao.rescue, 0, sizeof(l_ao.rescue));
    l_ao.rescue.state      = FCK_RESCUE_BOOTLOADER;
    l_ao.rescue.bootloader = true;
    l_ao.rescue.locked     = true;
    l_ao.rescue.restarts   = 3U;
    l_ao.rescue.ms         = 2500U;
    (void)memcpy(l_ao.rescue.mac, MAC, sizeof(MAC));
    l_ao.rescue.flash_size = 4U * 1024U * 1024U;
    asks_chip("fck_rescue", 0U, FCK_CALL_RESCUE_PROBE, FCK_C5);
    answer(FCK_RESCUE_PROBE_RSP_SIG, KODE_OK, FCK_WHY_NONE, 0U);
    TEST_ASSERT_TRUE(kode_fake_logged("fck_rescue: only the boot code runs; ROM loader answered, "
                                      "LOCKED; its ROM started 3 time(s); 2500 ms"));
    TEST_ASSERT_EQUAL_STRING("fck_rescue: MAC 24:58:7c:01:02:03, 4096 KB of flash",
                             kode_fake_last_line());
    TEST_ASSERT_EQUAL_UINT(0U, g_kode.warnings);
}
