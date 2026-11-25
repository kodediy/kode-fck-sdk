/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_sys.c
 * @brief fck_sys.c: the extended layer on kode-sdk's own QP/C, over a core that does what a case
 *        scripts and an owner that keeps a per-task error and the cancel marks.
 *
 * fck_sys.c is built into this file, so a case reaches its statics (the doors, the subscribers).
 * The Active Objects are stand-ins: a priority and a FreeRTOS queue laid out where the QP/C port
 * reads it, so a post with a margin counts its room as on the board; a case reads what was posted
 * to them by hand and hands each block back to QF's pool. The pool is QF's own, of a count the
 * cases know, so one can be drained on purpose. The executors and the end task are FreeRTOS tasks of
 * the SDK, running as they ship on the host port.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fck_sys.c"

#include "freertos/semphr.h"
#include "unity.h"

#include "fake_aos.h"
#include "fake_core.h"
#include "fake_kode.h"

/**
 * @brief What QP/C calls instead of carrying on: the run stops, naming it. A case that reached an
 *        assert of QP/C through the API would end the whole binary here, and be seen.
 */
Q_NORETURN Q_onError(char const * const module, int_t const id)
{
    (void)printf("Q_onError %s:%d\n", module, (int)id);
    abort();
}

/* ---- QF, the pool and the Active Objects ------------------------------------------------------ */

#define MAXOF2(a, b) (((a) > (b)) ? (a) : (b))
#define T_BLOCK                                                                                    \
    ((MAXOF2(MAXOF2(sizeof(fck_sys_wifi_entry_t), sizeof(fck_sys_ble_entry_t)),                     \
             MAXOF2(sizeof(fck_sys_answer_t), sizeof(fck_sys_news_t))) + 7U) & ~(size_t)7U)
#define T_BLOCKS 40U

static uint64_t s_pool[(T_BLOCK * T_BLOCKS) / sizeof(uint64_t)];

#define AOS      12U
#define AO_QMAX  64U

typedef struct {
    QActive super;
    uint8_t sto[AO_QMAX * sizeof(QEvt *)];
} tao_t;

static tao_t s_ao[AOS];

static void ao_drain(QActive *a)
{
    if (a->eQueue == NULL) {
        return;
    }
    QEvt const *e = NULL;
    while (xQueueReceive(a->eQueue, &e, 0U) == pdTRUE) {
        QF_gc(e);
    }
}

/** @brief The @p i th stand-in, started: priority i + 1 and a queue of @p qlen. */
static QActive *ao_new(unsigned i, unsigned qlen)
{
    tao_t *const a = &s_ao[i];
    ao_drain(&a->super);
    (void)memset(a, 0, sizeof(*a));
    a->super.prio   = (uint8_t)(i + 1U);
    a->super.eQueue = xQueueCreateStatic(qlen, sizeof(QEvt *), a->sto, &a->super.osObject.queue);
    return &a->super;
}

/** @brief A stand-in that is not started: priority 0. */
static QActive *ao_unstarted(unsigned i)
{
    QActive *const a = ao_new(i, 4U);
    a->prio = 0U;
    return a;
}

typedef struct {
    QSignal sig;
    union {
        fck_sys_answer_t     answer;
        fck_sys_news_t       news;
        fck_sys_wifi_entry_t wifi;
        fck_sys_ble_entry_t  ble;
    } u;
} got_t;

/** @brief Take what is first in @p a's queue, waiting up to @p ms; its block goes back to the pool. */
static bool take(QActive *a, got_t *g, uint32_t ms)
{
    QEvt const *e = NULL;
    if (xQueueReceive(a->eQueue, &e, (ms == 0U) ? 0U : (pdMS_TO_TICKS(ms) > 0U ? pdMS_TO_TICKS(ms) : 1U)) != pdTRUE) {
        return false;
    }
    g->sig = e->sig;
    (void)memcpy(&g->u, e, (sizeof(g->u) < T_BLOCK) ? sizeof(g->u) : T_BLOCK);
    QF_gc(e);
    return true;
}

static unsigned waiting(QActive *a)
{
    return (unsigned)uxQueueMessagesWaiting(a->eQueue);
}

static uint32_t now_ms(void)
{
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

static void sleep_ms(uint32_t ms)
{
    vTaskDelay(pdMS_TO_TICKS(ms) > 0U ? pdMS_TO_TICKS(ms) : 1U);
}

/* The blocks held back to drain the pool on purpose. */
static QEvt *s_hog[T_BLOCKS];
static unsigned s_hogged;

/** @brief Hold blocks until only @p leave are free. */
static void hog_leave(unsigned leave)
{
    while (s_hogged < T_BLOCKS - leave) {
        QEvt *const e = Q_NEW_X(QEvt, 0U, 7U);
        if (e == NULL) {
            break;
        }
        s_hog[s_hogged++] = e;
    }
}

static void hog_release(void)
{
    while (s_hogged > 0U) {
        QF_gc(s_hog[--s_hogged]);
    }
}

/** @brief Fill @p a's queue with @p n events of an app's own. */
static void fill(QActive *a, unsigned n)
{
    for (unsigned i = 0U; i < n; i++) {
        QEvt *const e = Q_NEW_X(QEvt, 0U, 7U);
        TEST_ASSERT_NOT_NULL(e);
        TEST_ASSERT_TRUE(QACTIVE_POST_X(a, e, 0U, (void *)0));
    }
}

/* Nobody is in flight: no executor inside the core, nothing queued, no door open, no end pending. */
static bool quiet(void)
{
    if (g_core.inside != 0U) {
        return false;
    }
    if ((s_queue != NULL) && (uxQueueMessagesWaiting(s_queue) != 0U)) {
        return false;
    }
    bool idle = true;
    taskENTER_CRITICAL(&s_mux);
    for (size_t i = 0U; i < FCK_SYS_ASKERS_MAX; i++) {
        if ((s_asker[i].me != NULL) || (s_asker[i].open != 0U)) {
            idle = false;
        }
    }
    if (s_end_pending) {
        idle = false;
    }
    taskEXIT_CRITICAL(&s_mux);
    return idle;
}

/** @brief Let the SDK's tasks finish what is in hand: up to @p ms. */
static bool wait_quiet(uint32_t ms)
{
    /* Quiet twice, 30 ms apart: an answer whose door was given back is posted a moment later. */
    for (uint32_t t = 0U; t <= ms; t += 10U) {
        if (quiet()) {
            sleep_ms(30U);
            if (quiet()) {
                return true;
            }
        }
        sleep_ms(10U);
    }
    return false;
}

/** @brief Wait for the answers lost to be counted (an answer retries for a second before it is). */
static bool wait_dropped(uint32_t want, uint32_t ms)
{
    for (uint32_t t = 0U; t <= ms; t += 10U) {
        if (g_aos_dropped[FCK_AOS_SINK_ANSWERS] >= want) {
            return true;
        }
        sleep_ms(10U);
    }
    return false;
}

static void ao_up(void)
{
    static bool up;
    if (up) {
        return;
    }
    up = true;
    QF_init();
    QF_poolInit(s_pool, sizeof(s_pool), T_BLOCK);
}

static void clear_everything(void)
{
    g_core.hold = false;
    (void)wait_quiet(2500U);
    for (size_t i = 0U; i < FCK_SUBSCRIBERS_MAX; i++) {
        taskENTER_CRITICAL(&s_mux);
        s_subscriber[i] = NULL;
        taskEXIT_CRITICAL(&s_mux);
    }
    for (unsigned i = 0U; i < AOS; i++) {
        if (s_ao[i].super.eQueue != NULL) {
            ao_drain(&s_ao[i].super);
        }
    }
    hog_release();
}

void setUp(void)
{
    ao_up();
    clear_everything();
    fake_core_reset();
    fake_aos_reset();
    g_log_warnings = 0U;
}

void tearDown(void)
{
    clear_everything();
    /* Every block is back: a leak would be seen as a pool that is short. */
    hog_leave(0U);
    unsigned const held = s_hogged;
    hog_release();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(T_BLOCKS, held, "an event block was not given back to the pool");
}

/* ---- The 24 requests, each asked well and badly ----------------------------------------------- */

static char                          s_reply[64];
static char                          s_text[300];
static fck_firmware_t                   s_firmware;
static fck_image_t                   s_image;
static fck_flash_report_t            s_flash_rep;
static fck_rescue_report_t           s_rescue;
static fck_wifi_channel_t            s_chan[4];
static int8_t                        s_dbm8[16];
static fck_radio_frame_t             s_frames[4];
static fck_radio_sniff_statistics_t  s_sniff;
static fck_radio_ping_statistics_t   s_ping;
static fck_radio_echo_statistics_t   s_echo;
static int16_t                       s_dbm16[16];
static fck_subghz_hit_t              s_hit;
static int32_t                       s_us[16] = { 400, -400, 400, -400, 400, -400, 400, -400 };
static int32_t                       s_us_out[16];
static fck_subghz_packet_settings_t  s_settings;
static fck_subghz_packet_t           s_packets[4];
static fck_subghz_code_t             s_code;
static uint8_t                       s_bytes[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };

#define HZ 433920000UL

static kode_status_t ask_command(QActive *me) { return fck_sys_command(me, FCK_C5, "info", "info", 100U, s_reply, sizeof(s_reply)); }
static kode_status_t bad_command(QActive *me) { return fck_sys_command(me, (fck_chip_t)7, "info", NULL, 0U, NULL, 0U); }
static kode_status_t ask_reset(QActive *me) { return fck_sys_reset(me, FCK_NRF); }
static kode_status_t bad_reset(QActive *me) { return fck_sys_reset(me, (fck_chip_t)9); }
static kode_status_t ask_power(QActive *me) { return fck_sys_power(me, FCK_C5, true); }
static kode_status_t bad_power(QActive *me) { return fck_sys_power(me, (fck_chip_t)9, true); }
static kode_status_t ask_firmware(QActive *me) { return fck_sys_firmware(me, FCK_C5, &s_firmware); }
static kode_status_t bad_firmware(QActive *me) { return fck_sys_firmware(me, FCK_C5, NULL); }
static kode_status_t ask_flash(QActive *me) { return fck_sys_flash(me, FCK_C5, &s_image, 1U, &s_flash_rep); }
static kode_status_t bad_flash(QActive *me) { return fck_sys_flash(me, FCK_C5, &s_image, 0U, &s_flash_rep); }
static kode_status_t ask_rescue(QActive *me) { return fck_sys_rescue_probe(me, FCK_C5, &s_rescue); }
static kode_status_t bad_rescue(QActive *me) { return fck_sys_rescue_probe(me, FCK_C5, NULL); }
static kode_status_t ask_wifi_scan(QActive *me) { return fck_sys_wifi_scan(me, FCK_BAND_2G); }
static kode_status_t ask_wifi_channels(QActive *me) { return fck_sys_wifi_channels(me, FCK_BAND_2G, 100U, s_chan, 4U); }
static kode_status_t bad_wifi_channels(QActive *me) { return fck_sys_wifi_channels(me, FCK_BAND_2G, 100U, s_chan, 0U); }
static kode_status_t ask_ble_scan(QActive *me) { return fck_sys_ble_scan(me, 3U); }
static kode_status_t ask_gnss_begin(QActive *me) { return fck_sys_gnss_begin(me); }
static kode_status_t ask_gnss_end(QActive *me) { return fck_sys_gnss_end(me); }
static kode_status_t ask_radio_spectrum(QActive *me) { return fck_sys_radio_spectrum(me, 2400U, 2480U, 100U, 1U, s_dbm8, 16U); }
static kode_status_t bad_radio_spectrum(QActive *me) { return fck_sys_radio_spectrum(me, 2400U, 2480U, 100U, 1U, NULL, 16U); }
static kode_status_t ask_radio_sniff(QActive *me) { return fck_sys_radio_sniff(me, 2440U, 100U, -90, s_frames, 4U, &s_sniff); }
static kode_status_t bad_radio_sniff(QActive *me) { return fck_sys_radio_sniff(me, 2440U, 100U, -90, NULL, 4U, &s_sniff); }
static kode_status_t ask_radio_ping(QActive *me) { return fck_sys_radio_ping(me, 3U, 2440U, 10U, 0, &s_ping); }
static kode_status_t bad_radio_ping(QActive *me) { return fck_sys_radio_ping(me, 3U, 2440U, 10U, 0, NULL); }
static kode_status_t ask_echo_start(QActive *me) { return fck_sys_radio_echo_start(me, 2440U, 0); }
static kode_status_t ask_echo_stop(QActive *me) { return fck_sys_radio_echo_stop(me, &s_echo); }
static kode_status_t ask_level(QActive *me) { return fck_sys_subghz_level(me, HZ, FCK_SUBGHZ_WIDE, &s_dbm16[0]); }
static kode_status_t bad_level(QActive *me) { return fck_sys_subghz_level(me, HZ, FCK_SUBGHZ_WIDE, NULL); }
static kode_status_t ask_spectrum(QActive *me) { return fck_sys_subghz_spectrum(me, 433000000UL, 100000UL, 4U, FCK_SUBGHZ_WIDE, s_dbm16, 16U); }
static kode_status_t bad_spectrum(QActive *me) { return fck_sys_subghz_spectrum(me, 433000000UL, 100000UL, 4U, FCK_SUBGHZ_WIDE, s_dbm16, 0U); }
static kode_status_t ask_find(QActive *me) { return fck_sys_subghz_find(me, 100U, &s_hit); }
static kode_status_t bad_find(QActive *me) { return fck_sys_subghz_find(me, 100U, NULL); }
static kode_status_t ask_capture(QActive *me) { return fck_sys_subghz_capture(me, HZ, 100U, s_us_out, 16U); }
static kode_status_t bad_capture(QActive *me) { return fck_sys_subghz_capture(me, HZ, 100U, NULL, 16U); }
static kode_status_t ask_packet_receive(QActive *me) { return fck_sys_subghz_packet_receive(me, HZ, &s_settings, 100U, s_packets, 4U); }
static kode_status_t bad_packet_receive(QActive *me) { return fck_sys_subghz_packet_receive(me, HZ, NULL, 100U, s_packets, 4U); }
static kode_status_t ask_send_raw(QActive *me) { return fck_sys_subghz_send_raw(me, HZ, s_us, 8U, 1U); }
static kode_status_t bad_send_raw(QActive *me) { return fck_sys_subghz_send_raw(me, HZ, s_us, 0U, 1U); }
static kode_status_t ask_send_code(QActive *me) { return fck_sys_subghz_send_code(me, HZ, &s_code, 1U); }
static kode_status_t bad_send_code(QActive *me) { return fck_sys_subghz_send_code(me, HZ, NULL, 1U); }
static kode_status_t ask_packet_send(QActive *me) { return fck_sys_subghz_packet_send(me, HZ, &s_settings, s_bytes, 8U, 1U); }
static kode_status_t bad_packet_send(QActive *me) { return fck_sys_subghz_packet_send(me, HZ, &s_settings, s_bytes, 0U, 1U); }

typedef kode_status_t (*ask_fn)(QActive *me);

typedef struct {
    const char *name;
    QSignal     sig;       /**< Its answer.                                       */
    door_t      door;
    const char *core;      /**< The core call that carries it out.                */
    ask_fn      ask;       /**< Well asked.                                       */
    ask_fn      bad;       /**< With a bad argument; NULL for a request with none. */
    const void *out;       /**< A buffer the core writes (first byte 0x5A) or NULL. */
    bool        lists;     /**< The answer's count is the core's out_n.            */
} row_t;

static const row_t ROW[] = {
    { "command",         FCK_COMMAND_RSP_SIG,               D_COMMAND, "command",              ask_command,        bad_command,        NULL,     false },
    { "reset",           FCK_RESET_RSP_SIG,                 D_COMMAND, "reset",                ask_reset,          bad_reset,          NULL,     false },
    { "power",           FCK_POWER_RSP_SIG,                 D_COMMAND, "power",                ask_power,          bad_power,          NULL,     false },
    { "firmware",           FCK_FIRMWARE_RSP_SIG,                 D_COMMAND, "firmware",                ask_firmware,          bad_firmware,          NULL,     false },
    { "flash",           FCK_FLASH_RSP_SIG,                 D_FLASH,   "flash",                ask_flash,          bad_flash,          &s_flash_rep, false },
    { "rescue_probe",    FCK_RESCUE_PROBE_RSP_SIG,          D_FLASH,   "rescue_probe",         ask_rescue,         bad_rescue,         &s_rescue, false },
    { "wifi_scan",       FCK_WIFI_SCAN_RSP_SIG,             D_SCAN,    "wifi_scan",            ask_wifi_scan,      NULL,               NULL,     true  },
    { "wifi_channels",   FCK_WIFI_CHANNELS_RSP_SIG,         D_SCAN,    "wifi_channels",        ask_wifi_channels,  bad_wifi_channels,  s_chan,   true  },
    { "ble_scan",        FCK_BLE_SCAN_RSP_SIG,              D_SCAN,    "ble_scan",             ask_ble_scan,       NULL,               NULL,     true  },
    { "gnss_begin",      FCK_GNSS_BEGIN_RSP_SIG,            D_GNSS,    "gnss_begin",           ask_gnss_begin,     NULL,               NULL,     false },
    { "gnss_end",        FCK_GNSS_END_RSP_SIG,              D_GNSS,    "gnss_end",             ask_gnss_end,       NULL,               NULL,     false },
    { "radio_spectrum",  FCK_RADIO_SPECTRUM_RSP_SIG,        D_RADIO,   "radio_spectrum",       ask_radio_spectrum, bad_radio_spectrum, s_dbm8,   true  },
    { "radio_sniff",     FCK_RADIO_SNIFF_RSP_SIG,           D_RADIO,   "radio_sniff",          ask_radio_sniff,    bad_radio_sniff,    s_frames, true  },
    { "radio_ping",      FCK_RADIO_PING_RSP_SIG,            D_RADIO,   "radio_ping",           ask_radio_ping,     bad_radio_ping,     &s_ping,  false },
    { "radio_echo_start",FCK_RADIO_ECHO_START_RSP_SIG,      D_RADIO,   "radio_echo_start",     ask_echo_start,     NULL,               NULL,     false },
    { "radio_echo_stop", FCK_RADIO_ECHO_STOP_RSP_SIG,       D_RADIO,   "radio_echo_stop",      ask_echo_stop,      NULL,               &s_echo,  false },
    { "subghz_level",    FCK_SUBGHZ_LEVEL_RSP_SIG,          D_LISTEN,  "subghz_level",         ask_level,          bad_level,          s_dbm16,  false },
    { "subghz_spectrum", FCK_SUBGHZ_SPECTRUM_RSP_SIG,       D_LISTEN,  "subghz_spectrum",      ask_spectrum,       bad_spectrum,       s_dbm16,  true  },
    { "subghz_find",     FCK_SUBGHZ_FIND_RSP_SIG,           D_LISTEN,  "subghz_find",          ask_find,           bad_find,           &s_hit,   false },
    { "subghz_capture",  FCK_SUBGHZ_CAPTURE_RSP_SIG,        D_LISTEN,  "subghz_capture",       ask_capture,        bad_capture,        s_us_out, true  },
    { "subghz_packet_receive", FCK_SUBGHZ_PACKET_RECEIVE_RSP_SIG, D_LISTEN, "subghz_packet_receive", ask_packet_receive, bad_packet_receive, s_packets, true },
    { "subghz_send_raw", FCK_SUBGHZ_SEND_RAW_RSP_SIG,       D_SEND,    "subghz_send_raw",      ask_send_raw,       bad_send_raw,       NULL,     false },
    { "subghz_send_code",FCK_SUBGHZ_SEND_CODE_RSP_SIG,      D_SEND,    "subghz_send_code",     ask_send_code,      bad_send_code,      NULL,     false },
    { "subghz_packet_send", FCK_SUBGHZ_PACKET_SEND_RSP_SIG, D_SEND,    "subghz_packet_send",   ask_packet_send,    bad_packet_send,    NULL,     false },
};
#define ROWS (sizeof(ROW) / sizeof(ROW[0]))

_Static_assert(ROWS == 24U, "the 24 requests of fck_sys.h");

static const char *k_door_name[] = { "COMMAND", "FLASH", "SCAN", "GNSS", "RADIO", "LISTEN", "SEND" };

static void label(char *to, size_t cap, const row_t *r, const char *what)
{
    (void)snprintf(to, cap, "fck_sys_%s(): %s", r->name, what);
}

static void clear_outputs(void)
{
    (void)memset(s_reply, 0, sizeof(s_reply));
    (void)memset(&s_firmware, 0, sizeof(s_firmware));
    (void)memset(&s_flash_rep, 0, sizeof(s_flash_rep));
    (void)memset(&s_rescue, 0, sizeof(s_rescue));
    (void)memset(s_chan, 0, sizeof(s_chan));
    (void)memset(s_dbm8, 0, sizeof(s_dbm8));
    (void)memset(s_frames, 0, sizeof(s_frames));
    (void)memset(&s_sniff, 0, sizeof(s_sniff));
    (void)memset(&s_ping, 0, sizeof(s_ping));
    (void)memset(&s_echo, 0, sizeof(s_echo));
    (void)memset(s_dbm16, 0, sizeof(s_dbm16));
    (void)memset(&s_hit, 0, sizeof(s_hit));
    (void)memset(s_us_out, 0, sizeof(s_us_out));
    (void)memset(s_packets, 0, sizeof(s_packets));
}

/** @brief The answer of a request taken, waiting up to a second. */
static void expect_answer(QActive *a, const row_t *r, got_t *g)
{
    char msg[96];
    label(msg, sizeof(msg), r, "its answer did not come");
    TEST_ASSERT_TRUE_MESSAGE(take(a, g, 1500U), msg);
    label(msg, sizeof(msg), r, "answered on a signal that is not its own");
    TEST_ASSERT_EQUAL_UINT_MESSAGE((unsigned)r->sig, (unsigned)g->sig, msg);
}

static void expect_nothing_more(QActive *a, const char *what)
{
    got_t g;
    sleep_ms(30U);
    TEST_ASSERT_FALSE_MESSAGE(take(a, &g, 0U), what);
}

/* ---- Signals ------------------------------------------------------------------------------------ */

TEST_CASE("the signals are those of the 1300 range: distinct, in order, and 90 at most", "[sys][signals]")
{
    TEST_ASSERT_EQUAL_UINT((unsigned)(MAX_SIG + 1300U), (unsigned)FCK_SIG_FIRST);
    TEST_ASSERT_EQUAL_UINT((unsigned)FCK_SIG_FIRST, (unsigned)FCK_MODULE_FOUND_SIG);
    TEST_ASSERT_TRUE((unsigned)FCK_SIG_END - (unsigned)FCK_SIG_FIRST <= 90U);
    QSignal prev = 0U;
    for (size_t i = 0U; i < ROWS; i++) {
        TEST_ASSERT_TRUE_MESSAGE(ROW[i].sig > prev, ROW[i].name);   /* each request's answer is a number of its own */
        prev = ROW[i].sig;
    }
    TEST_ASSERT_EQUAL_UINT((unsigned)FCK_SEND_CANCELLED_SIG + 1U, (unsigned)FCK_WIFI_SCAN_ENTRY_SIG);
    TEST_ASSERT_EQUAL_UINT((unsigned)FCK_BLE_SCAN_ENTRY_SIG + 1U, (unsigned)FCK_COMMAND_RSP_SIG);
}

/* ---- fck_why_text ---------------------------------------------------------------------------------- */

TEST_CASE("fck_why_text says something for every class, nothing for none, and is never NULL", "[sys][why]")
{
    TEST_ASSERT_EQUAL_STRING("", fck_why_text(FCK_WHY_NONE));
    for (int w = (int)FCK_WHY_NOT_BEGUN; w <= (int)FCK_WHY_AO_CALLER; w++) {
        TEST_ASSERT_NOT_NULL(fck_why_text((fck_why_t)w));
        TEST_ASSERT_TRUE_MESSAGE(strlen(fck_why_text((fck_why_t)w)) > 8U, "a sentence for each class");
        for (int v = w + 1; v <= (int)FCK_WHY_AO_CALLER; v++) {
            TEST_ASSERT_TRUE_MESSAGE(strcmp(fck_why_text((fck_why_t)w), fck_why_text((fck_why_t)v)) != 0, "each its own");
        }
    }
    TEST_ASSERT_EQUAL_STRING("", fck_why_text((fck_why_t)99));
    TEST_ASSERT_EQUAL_STRING("", fck_why_text((fck_why_t)-1));
}

/* ---- What is refused at once --------------------------------------------------------------------- */

TEST_CASE("every request with me NULL is refused with KODE_DENIED, and nothing is taken or asked", "[sys][refuse]")
{
    for (size_t i = 0U; i < ROWS; i++) {
        char msg[96];
        label(msg, sizeof(msg), &ROW[i], "NULL asker");
        TEST_ASSERT_EQUAL_INT_MESSAGE(KODE_DENIED, ROW[i].ask(NULL), msg);
        TEST_ASSERT_TRUE_MESSAGE(quiet(), msg);
        TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, g_core.calls, msg);
    }
}

TEST_CASE("every request from an Active Object that is not started is refused with KODE_DENIED", "[sys][refuse]")
{
    QActive *const a = ao_unstarted(0U);
    for (size_t i = 0U; i < ROWS; i++) {
        char msg[96];
        label(msg, sizeof(msg), &ROW[i], "an Active Object not started");
        TEST_ASSERT_EQUAL_INT_MESSAGE(KODE_DENIED, ROW[i].ask(a), msg);
        TEST_ASSERT_TRUE_MESSAGE(quiet(), msg);
        TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, g_core.calls, msg);
    }
    a->prio = (uint8_t)(QF_MAX_ACTIVE + 1U);       /* out of range */
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, ask_wifi_scan(a));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, fck_sys_subscribe(a));
    a->prio = 0U;
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, fck_sys_subscribe(a));
    TEST_ASSERT_EQUAL_UINT(0U, g_core.calls);
}

TEST_CASE("every request is refused with KODE_DENIED before fck_sys_begin, and after the end", "[sys][refuse]")
{
    QActive *const a = ao_new(0U, 8U);
    g_aos_started = false;
    for (size_t i = 0U; i < ROWS; i++) {
        char msg[96];
        label(msg, sizeof(msg), &ROW[i], "before fck_sys_begin()");
        TEST_ASSERT_EQUAL_INT_MESSAGE(KODE_DENIED, ROW[i].ask(a), msg);
        TEST_ASSERT_TRUE_MESSAGE(quiet(), msg);
    }
    TEST_ASSERT_EQUAL_UINT(0U, g_core.calls);
    TEST_ASSERT_EQUAL_UINT(0U, waiting(a));
    g_aos_started = true;
    TEST_ASSERT_EQUAL_INT(KODE_OK, ask_wifi_scan(a));
    got_t g;
    TEST_ASSERT_TRUE(take(a, &g, 1500U));
}

TEST_CASE("a bad argument is refused with KODE_DENIED, nothing is asked, and the door stays free", "[sys][refuse]")
{
    unsigned checked = 0U;
    for (size_t i = 0U; i < ROWS; i++) {
        if (ROW[i].bad == NULL) {
            continue;
        }
        char msg[96];
        QActive *const a = ao_new(0U, 8U);
        fake_core_reset();
        label(msg, sizeof(msg), &ROW[i], "a bad argument");
        TEST_ASSERT_EQUAL_INT_MESSAGE(KODE_DENIED, ROW[i].bad(a), msg);
        TEST_ASSERT_TRUE_MESSAGE(quiet(), msg);
        TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, g_core.calls, msg);
        TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, waiting(a), msg);
        label(msg, sizeof(msg), &ROW[i], "the door must be free after a refusal");
        TEST_ASSERT_EQUAL_INT_MESSAGE(KODE_OK, ROW[i].ask(a), msg);
        got_t g;
        expect_answer(a, &ROW[i], &g);
        checked++;
    }
    TEST_ASSERT_EQUAL_UINT(18U, checked);      /* six requests have no argument that can be bad */
}

TEST_CASE("the bad arguments of fck_sys_command: a chip, a line, a verb, a reply buffer", "[sys][refuse]")
{
    QActive *const a = ao_new(0U, 8U);
    memset(s_text, 'x', 255);
    s_text[255] = '\0';
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, fck_sys_command(a, (fck_chip_t)2, "info", NULL, 0U, NULL, 0U));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, fck_sys_command(a, (fck_chip_t)-1, "info", NULL, 0U, NULL, 0U));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, fck_sys_command(a, FCK_C5, NULL, NULL, 0U, NULL, 0U));
    s_text[255] = 'x';
    s_text[256] = '\0';
    TEST_ASSERT_EQUAL_INT_MESSAGE(KODE_DENIED, fck_sys_command(a, FCK_C5, s_text, NULL, 0U, NULL, 0U), "256 bytes");
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, fck_sys_command(a, FCK_C5, "info", "0123456789012345678901234567890123456789", 0U, NULL, 0U));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, fck_sys_command(a, FCK_C5, "info", NULL, 0U, NULL, 8U));
    TEST_ASSERT_TRUE(quiet());
    TEST_ASSERT_EQUAL_UINT(0U, g_core.calls);
    s_text[255] = '\0';
    TEST_ASSERT_EQUAL_INT_MESSAGE(KODE_OK, fck_sys_command(a, FCK_NRF, s_text, NULL, 0U, NULL, 0U), "255 bytes are allowed");
    got_t g;
    TEST_ASSERT_TRUE(take(a, &g, 1500U));
}

TEST_CASE("the bounds of the others: a packet of 61 bytes is allowed, one of 62 is not", "[sys][refuse]")
{
    QActive *const a = ao_new(0U, 8U);
    uint8_t big[FCK_SUBGHZ_PACKET_MAX + 1U] = { 0 };
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, fck_sys_subghz_packet_send(a, HZ, &s_settings, big, FCK_SUBGHZ_PACKET_MAX + 1U, 1U));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, fck_sys_subghz_packet_send(a, HZ, NULL, big, 8U, 1U));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, fck_sys_subghz_packet_send(a, HZ, &s_settings, NULL, 8U, 1U));
    TEST_ASSERT_EQUAL_INT(KODE_OK, fck_sys_subghz_packet_send(a, HZ, &s_settings, big, FCK_SUBGHZ_PACKET_MAX, 1U));
    got_t g;
    TEST_ASSERT_TRUE(take(a, &g, 1500U));
    TEST_ASSERT_EQUAL_UINT(FCK_SUBGHZ_PACKET_MAX, g_core.length);
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, fck_sys_wifi_channels(a, FCK_BAND_2G, 100U, NULL, 4U));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, fck_sys_subghz_send_raw(a, HZ, NULL, 8U, 1U));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, fck_sys_subghz_packet_receive(a, HZ, &s_settings, 100U, NULL, 4U));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, fck_sys_subghz_packet_receive(a, HZ, &s_settings, 100U, s_packets, 0U));
    TEST_ASSERT_TRUE(quiet());
}

/* ---- Answers -------------------------------------------------------------------------------------- */

TEST_CASE("a request taken is answered exactly once, on its signal, with rc OK and its core call made once", "[sys][answer]")
{
    for (size_t i = 0U; i < ROWS; i++) {
        char msg[96];
        QActive *const a = ao_new(0U, 8U);
        fake_core_reset();
        clear_outputs();
        g_core.out_n  = 3U;
        g_core.wifi_n = 3U;
        g_core.ble_n  = 3U;
        label(msg, sizeof(msg), &ROW[i], "must be taken");
        TEST_ASSERT_EQUAL_INT_MESSAGE(KODE_OK, ROW[i].ask(a), msg);
        got_t g;
        if (ROW[i].sig == FCK_WIFI_SCAN_RSP_SIG || ROW[i].sig == FCK_BLE_SCAN_RSP_SIG) {
            for (unsigned k = 0U; k < 3U; k++) {
                TEST_ASSERT_TRUE_MESSAGE(take(a, &g, 1500U), "its entries");
            }
        }
        expect_answer(a, &ROW[i], &g);
        label(msg, sizeof(msg), &ROW[i], "rc");
        TEST_ASSERT_EQUAL_INT_MESSAGE(KODE_OK, g.u.answer.rc, msg);
        label(msg, sizeof(msg), &ROW[i], "why");
        TEST_ASSERT_EQUAL_INT_MESSAGE(FCK_WHY_NONE, g.u.answer.why, msg);
        label(msg, sizeof(msg), &ROW[i], "count");
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(ROW[i].lists ? 3U : 0U, g.u.answer.count, msg);
        label(msg, sizeof(msg), &ROW[i], "flags");
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(0U, g.u.answer.flags, msg);
        label(msg, sizeof(msg), &ROW[i], "its own core call, once");
        TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, fake_core_count(ROW[i].core), msg);
        TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, g_core.calls, msg);
        if (ROW[i].out != NULL) {
            label(msg, sizeof(msg), &ROW[i], "the buffer lent was not written");
            TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x5A, *(const uint8_t *)ROW[i].out, msg);
        }
        label(msg, sizeof(msg), &ROW[i], "a second answer");
        expect_nothing_more(a, msg);
        label(msg, sizeof(msg), &ROW[i], "the door is not free after the answer");
        TEST_ASSERT_TRUE_MESSAGE(wait_quiet(200U), msg);
    }
}

static const struct {
    kode_status_t st;
    const char   *name;
} k_fail[] = {
    { KODE_DENIED, "KODE_DENIED" }, { KODE_TIMEOUT, "KODE_TIMEOUT" }, { KODE_IO_ERROR, "KODE_IO_ERROR" },
    { KODE_NO_RESOURCES, "KODE_NO_RESOURCES" }, { KODE_ERROR, "KODE_ERROR" }, { KODE_EXISTS, "KODE_EXISTS" },
};

static void every_request_fails_with(kode_status_t st, const char *name)
{
    for (size_t i = 0U; i < ROWS; i++) {
        char msg[96];
        QActive *const a = ao_new(0U, 8U);
        fake_core_reset();
        clear_outputs();
        g_core.next  = st;
        g_core.why   = FCK_AOS_WHY_IO;
        g_core.wifi_n = 3U;
        g_core.ble_n  = 3U;
        (void)snprintf(msg, sizeof(msg), "fck_sys_%s() with %s", ROW[i].name, name);
        TEST_ASSERT_EQUAL_INT_MESSAGE(KODE_OK, ROW[i].ask(a), msg);
        got_t g;
        expect_answer(a, &ROW[i], &g);
        TEST_ASSERT_EQUAL_INT_MESSAGE(st, g.u.answer.rc, msg);
        TEST_ASSERT_EQUAL_INT_MESSAGE(FCK_WHY_IO, g.u.answer.why, msg);
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(0U, g.u.answer.count, msg);
        TEST_ASSERT_TRUE_MESSAGE(wait_quiet(200U), msg);
        expect_nothing_more(a, msg);
    }
}

TEST_CASE("every request is answered with rc KODE_DENIED when the core says it", "[sys][answer]")
{
    every_request_fails_with(KODE_DENIED, "KODE_DENIED");
}

TEST_CASE("every request is answered with rc KODE_TIMEOUT when the core says it", "[sys][answer]")
{
    every_request_fails_with(KODE_TIMEOUT, "KODE_TIMEOUT");
}

TEST_CASE("every request is answered with rc KODE_IO_ERROR when the core says it", "[sys][answer]")
{
    every_request_fails_with(KODE_IO_ERROR, "KODE_IO_ERROR");
}

TEST_CASE("every request is answered with rc KODE_NO_RESOURCES when the core says it", "[sys][answer]")
{
    every_request_fails_with(KODE_NO_RESOURCES, "KODE_NO_RESOURCES");
}

TEST_CASE("every request is answered with rc KODE_ERROR when the core says it", "[sys][answer]")
{
    every_request_fails_with(KODE_ERROR, "KODE_ERROR");
}

TEST_CASE("every request is answered with rc KODE_EXISTS when the core says it", "[sys][answer]")
{
    every_request_fails_with(KODE_EXISTS, "KODE_EXISTS");
}

TEST_CASE("each class of failure the core gives reaches the answer as its fck_why_t", "[sys][answer]")
{
    QActive *const a = ao_new(0U, 8U);
    for (int w = (int)FCK_AOS_WHY_NOT_BEGUN; w <= (int)FCK_AOS_WHY_AO_CALLER; w++) {
        g_core.next = KODE_DENIED;
        g_core.why  = (fck_aos_why_t)w;
        TEST_ASSERT_EQUAL_INT(KODE_OK, ask_reset(a));
        got_t g;
        TEST_ASSERT_TRUE(take(a, &g, 1500U));
        TEST_ASSERT_EQUAL_INT_MESSAGE(w, (int)g.u.answer.why, fck_why_text((fck_why_t)w));
        TEST_ASSERT_TRUE(wait_quiet(200U));
    }
}

TEST_CASE("a core that fails without a class is answered FCK_WHY_REFUSED, never FCK_WHY_NONE", "[sys][answer]")
{
    QActive *const a = ao_new(0U, 8U);
    g_core.next = KODE_ERROR;
    g_core.why  = FCK_AOS_WHY_NONE;
    TEST_ASSERT_EQUAL_INT(KODE_OK, ask_gnss_end(a));
    got_t g;
    TEST_ASSERT_TRUE(take(a, &g, 1500U));
    TEST_ASSERT_EQUAL_INT(KODE_ERROR, g.u.answer.rc);
    TEST_ASSERT_EQUAL_INT(FCK_WHY_REFUSED, g.u.answer.why);
}

/* ---- A command's reply and the copies ------------------------------------------------------------ */

TEST_CASE("a command's reply is its tagged lines in the buffer lent, with their byte count and the #DONE rc", "[sys][command]")
{
    QActive *const a = ao_new(0U, 8U);
    g_core.n_lines    = 2U;
    g_core.line[0].tag  = "AP";
    g_core.line[0].body = "one";
    g_core.line[1].tag  = "C5INFO";
    g_core.line[1].body = "x y";
    g_core.cmd_rc     = 2;
    TEST_ASSERT_EQUAL_INT(KODE_OK, ask_command(a));
    got_t g;
    TEST_ASSERT_TRUE(take(a, &g, 1500U));
    TEST_ASSERT_EQUAL_STRING("AP one\nC5INFO x y\n", s_reply);
    TEST_ASSERT_EQUAL_UINT32(strlen("AP one\nC5INFO x y\n"), g.u.answer.count);
    TEST_ASSERT_EQUAL_INT32(2, g.u.answer.value);
    TEST_ASSERT_EQUAL_UINT32(0U, g.u.answer.flags);
}

TEST_CASE("a reply longer than the buffer is cut, NUL-terminated, and says so", "[sys][command]")
{
    QActive *const a = ao_new(0U, 8U);
    g_core.n_lines      = 2U;
    g_core.line[0].tag  = "AP";
    g_core.line[0].body = "one";
    g_core.line[1].tag  = "C5INFO";
    g_core.line[1].body = "x y";
    char small[8];
    memset(small, 'z', sizeof(small));
    TEST_ASSERT_EQUAL_INT(KODE_OK, fck_sys_command(a, FCK_C5, "info", NULL, 0U, small, sizeof(small)));
    got_t g;
    TEST_ASSERT_TRUE(take(a, &g, 1500U));
    TEST_ASSERT_EQUAL_UINT32(7U, g.u.answer.count);
    TEST_ASSERT_EQUAL_UINT32(FCK_SYS_ANSWER_TRUNCATED, g.u.answer.flags);
    TEST_ASSERT_EQUAL_CHAR('\0', small[7]);
    TEST_ASSERT_EQUAL_STRING("AP one\n", small);
}

TEST_CASE("a command with no reply buffer drops the lines and still counts nothing", "[sys][command]")
{
    QActive *const a = ao_new(0U, 8U);
    g_core.n_lines      = 1U;
    g_core.line[0].tag  = "AP";
    g_core.line[0].body = "one";
    TEST_ASSERT_EQUAL_INT(KODE_OK, fck_sys_command(a, FCK_NRF, "info", NULL, 0U, NULL, 0U));
    got_t g;
    TEST_ASSERT_TRUE(take(a, &g, 1500U));
    TEST_ASSERT_EQUAL_UINT32(0U, g.u.answer.count);
    const fake_call_t *c = fake_core_call("command", 0U);
    TEST_ASSERT_NOT_NULL(c);
    TEST_ASSERT_EQUAL_UINT64(0U, c->arg[3]);      /* no line callback */
}

TEST_CASE("the line, the verb and the time reach the core as given, and the line is copied", "[sys][command]")
{
    QActive *const a = ao_new(0U, 8U);
    g_core.hold = true;
    char line[16] = "info";
    char verb[16] = "info";
    TEST_ASSERT_EQUAL_INT(KODE_OK, fck_sys_command(a, FCK_NRF, line, verb, 750U, s_reply, sizeof(s_reply)));
    (void)strcpy(line, "zzzz");                 /* the caller reuses its buffers at once */
    (void)strcpy(verb, "zzzz");
    g_core.hold = false;
    got_t g;
    TEST_ASSERT_TRUE(take(a, &g, 1500U));
    TEST_ASSERT_EQUAL_STRING("info", g_core.cmd_line);
    TEST_ASSERT_TRUE(g_core.cmd_has_verb);
    TEST_ASSERT_EQUAL_STRING("info", g_core.cmd_verb);
    TEST_ASSERT_EQUAL_UINT32(750U, g_core.cmd_ms);
    const fake_call_t *c = fake_core_call("command", 0U);
    TEST_ASSERT_EQUAL_UINT64((uint64_t)FCK_NRF, c->arg[0]);
    TEST_ASSERT_TRUE(c->arg[1] != (uint64_t)(uintptr_t)line);

    TEST_ASSERT_EQUAL_INT(KODE_OK, fck_sys_command(a, FCK_C5, "go", NULL, 0U, NULL, 0U));
    TEST_ASSERT_TRUE(take(a, &g, 1500U));
    TEST_ASSERT_FALSE(g_core.cmd_has_verb);
}

TEST_CASE("a code, the packet's settings and its bytes are copied: the caller may change them at once", "[sys][copy]")
{
    QActive *const a = ao_new(0U, 8U);
    got_t g;
    g_core.hold = true;
    fck_subghz_code_t code;
    (void)memset(&code, 0x11, sizeof(code));
    TEST_ASSERT_EQUAL_INT(KODE_OK, fck_sys_subghz_send_code(a, HZ, &code, 2U));
    (void)memset(&code, 0x22, sizeof(code));
    g_core.hold = false;
    TEST_ASSERT_TRUE(take(a, &g, 1500U));
    uint8_t want[sizeof(fck_subghz_code_t) + sizeof(fck_subghz_packet_settings_t)];
    (void)memset(want, 0x11, sizeof(want));
    TEST_ASSERT_EQUAL_MEMORY(want, &g_core.code, sizeof(code));
    TEST_ASSERT_EQUAL_UINT64(2U, fake_core_call("subghz_send_code", 0U)->arg[2]);

    fck_subghz_packet_settings_t set;
    uint8_t bytes[4] = { 9, 8, 7, 6 };
    (void)memset(&set, 0x33, sizeof(set));
    g_core.hold = true;
    TEST_ASSERT_EQUAL_INT(KODE_OK, fck_sys_subghz_packet_send(a, HZ, &set, bytes, 4U, 3U));
    (void)memset(&set, 0x44, sizeof(set));
    bytes[0] = 0xEE;
    g_core.hold = false;
    TEST_ASSERT_TRUE(take(a, &g, 1500U));
    (void)memset(want, 0x33, sizeof(want));
    TEST_ASSERT_EQUAL_MEMORY(want, &g_core.settings, sizeof(set));
    TEST_ASSERT_EQUAL_UINT8(9, g_core.bytes[0]);
    TEST_ASSERT_EQUAL_UINT8(6, g_core.bytes[3]);
    TEST_ASSERT_EQUAL_UINT8(4, g_core.length);

    (void)memset(&set, 0x55, sizeof(set));
    g_core.hold = true;
    TEST_ASSERT_EQUAL_INT(KODE_OK, fck_sys_subghz_packet_receive(a, HZ, &set, 100U, s_packets, 4U));
    (void)memset(&set, 0x66, sizeof(set));
    g_core.hold = false;
    TEST_ASSERT_TRUE(take(a, &g, 1500U));
    (void)memset(want, 0x55, sizeof(want));
    TEST_ASSERT_EQUAL_MEMORY(want, &g_core.settings, sizeof(set));
}

TEST_CASE("the buffers lent are passed on as they are: the core writes the caller's own memory", "[sys][lent]")
{
    QActive *const a = ao_new(0U, 8U);
    got_t g;
    TEST_ASSERT_EQUAL_INT(KODE_OK, ask_send_raw(a));
    TEST_ASSERT_TRUE(take(a, &g, 1500U));
    const fake_call_t *c = fake_core_call("subghz_send_raw", 0U);
    TEST_ASSERT_EQUAL_UINT64((uint64_t)(uintptr_t)s_us, c->arg[1]);
    TEST_ASSERT_EQUAL_UINT64(8U, c->arg[2]);
    TEST_ASSERT_EQUAL_UINT64(HZ, c->arg[0]);
    TEST_ASSERT_EQUAL_UINT64(1U, c->arg[3]);

    TEST_ASSERT_EQUAL_INT(KODE_OK, ask_firmware(a));
    TEST_ASSERT_TRUE(take(a, &g, 1500U));
    TEST_ASSERT_TRUE(s_firmware.known);
    TEST_ASSERT_EQUAL_UINT64((uint64_t)(uintptr_t)&s_firmware, fake_core_call("firmware", 0U)->arg[1]);
}

TEST_CASE("nothing is written to a buffer lent for a request refused at once", "[sys][lent]")
{
    QActive *const a = ao_new(0U, 8U);
    clear_outputs();
    g_aos_started = false;
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, ask_firmware(a));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, ask_capture(a));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, ask_level(NULL));
    sleep_ms(80U);
    TEST_ASSERT_FALSE(s_firmware.known);
    TEST_ASSERT_EQUAL_INT32(0, s_us_out[0]);
    TEST_ASSERT_EQUAL_INT16(0, s_dbm16[0]);
}

/* ---- Doors -------------------------------------------------------------------------------------- */

static int door_bit(door_t d)
{
    return 1 << (int)d;
}

TEST_CASE("a second request of the same door is refused until the answer; another door is not", "[sys][doors]")
{
    for (size_t first = 0U; first < ROWS; first++) {
        char msg[96];
        QActive *const a = ao_new(0U, 16U);
        fake_core_reset();
        g_core.hold = true;
        label(msg, sizeof(msg), &ROW[first], "the first request");
        TEST_ASSERT_EQUAL_INT_MESSAGE(KODE_OK, ROW[first].ask(a), msg);
        int taken = door_bit(ROW[first].door);
        unsigned accepted = 1U;
        for (size_t second = 0U; second < ROWS; second++) {
            bool const busy = (taken & door_bit(ROW[second].door)) != 0;
            (void)snprintf(msg, sizeof(msg), "fck_sys_%s() after fck_sys_%s() (door %s)", ROW[second].name,
                           ROW[first].name, k_door_name[ROW[second].door]);
            kode_status_t const rc = ROW[second].ask(a);
            TEST_ASSERT_EQUAL_INT_MESSAGE(busy ? KODE_DENIED : KODE_OK, rc, msg);
            if (rc == KODE_OK) {
                taken |= door_bit(ROW[second].door);
                accepted++;
            }
        }
        g_core.hold = false;
        g_core.out_n = 0U;
        got_t g;
        unsigned answers = 0U;
        while (take(a, &g, (answers < accepted) ? 1500U : 80U)) {
            if (g.sig >= FCK_COMMAND_RSP_SIG) {
                answers++;
            }
        }
        label(msg, sizeof(msg), &ROW[first], "one answer for each request taken");
        TEST_ASSERT_EQUAL_UINT_MESSAGE(accepted, answers, msg);
        TEST_ASSERT_TRUE(wait_quiet(500U));
    }
}

TEST_CASE("the door is free when the answer is read: the handler may ask again at once", "[sys][doors]")
{
    for (size_t i = 0U; i < ROWS; i++) {
        char msg[96];
        QActive *const a = ao_new(0U, 8U);
        fake_core_reset();
        g_core.wifi_n = 0U;
        g_core.ble_n  = 0U;
        label(msg, sizeof(msg), &ROW[i], "first");
        TEST_ASSERT_EQUAL_INT_MESSAGE(KODE_OK, ROW[i].ask(a), msg);
        got_t g;
        expect_answer(a, &ROW[i], &g);
        label(msg, sizeof(msg), &ROW[i], "asked again from the answer");
        TEST_ASSERT_EQUAL_INT_MESSAGE(KODE_OK, ROW[i].ask(a), msg);
        expect_answer(a, &ROW[i], &g);
    }
}

TEST_CASE("the doors are an Active Object's own: another asks the same kind meanwhile", "[sys][doors]")
{
    QActive *const a = ao_new(0U, 8U);
    QActive *const b = ao_new(1U, 8U);
    g_core.hold = true;
    TEST_ASSERT_EQUAL_INT(KODE_OK, ask_wifi_scan(a));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, ask_ble_scan(a));
    TEST_ASSERT_EQUAL_INT(KODE_OK, ask_ble_scan(b));
    g_core.hold = false;
    got_t g;
    TEST_ASSERT_TRUE(take(a, &g, 1500U));
    TEST_ASSERT_EQUAL_UINT((unsigned)FCK_WIFI_SCAN_RSP_SIG, (unsigned)g.sig);
    TEST_ASSERT_TRUE(take(b, &g, 1500U));
    TEST_ASSERT_EQUAL_UINT((unsigned)FCK_BLE_SCAN_RSP_SIG, (unsigned)g.sig);
}

TEST_CASE("past FCK_SYS_ASKERS_MAX Active Objects holding a door, another is refused with KODE_NO_RESOURCES", "[sys][doors]")
{
    QActive *as[FCK_SYS_ASKERS_MAX + 1U];
    for (unsigned i = 0U; i <= FCK_SYS_ASKERS_MAX; i++) {
        as[i] = ao_new(i, 8U);
    }
    g_core.hold = true;
    for (unsigned i = 0U; i < FCK_SYS_ASKERS_MAX; i++) {
        TEST_ASSERT_EQUAL_INT_MESSAGE(KODE_OK, ask_gnss_end(as[i]), "up to FCK_SYS_ASKERS_MAX");
    }
    TEST_ASSERT_EQUAL_INT(KODE_NO_RESOURCES, ask_gnss_end(as[FCK_SYS_ASKERS_MAX]));
    TEST_ASSERT_EQUAL_INT(KODE_NO_RESOURCES, ask_level(as[FCK_SYS_ASKERS_MAX]));
    TEST_ASSERT_EQUAL_INT_MESSAGE(KODE_DENIED, ask_gnss_begin(as[3]), "one that holds a door has its place");
    TEST_ASSERT_EQUAL_INT_MESSAGE(KODE_OK, ask_level(as[3]), "and may open another door");
    g_core.hold = false;
    got_t g;
    for (unsigned i = 0U; i < FCK_SYS_ASKERS_MAX; i++) {
        TEST_ASSERT_TRUE(take(as[i], &g, 2000U));
    }
    TEST_ASSERT_TRUE(take(as[3], &g, 2000U));
    TEST_ASSERT_TRUE(wait_quiet(500U));
    TEST_ASSERT_EQUAL_INT_MESSAGE(KODE_OK, ask_gnss_end(as[FCK_SYS_ASKERS_MAX]), "a place is free again");
    TEST_ASSERT_TRUE(take(as[FCK_SYS_ASKERS_MAX], &g, 2000U));
}

TEST_CASE("the queue of requests full: KODE_NO_RESOURCES at once, the door given back, nothing lost", "[sys][queue]")
{
    QActive *const a = ao_new(0U, 16U);
    QActive *const b = ao_new(1U, 16U);
    QActive *const c = ao_new(2U, 16U);
    g_core.hold = true;
    /* Two executors hold one request each, the queue holds FCK_SYS_QUEUE more. */
    ask_fn const kinds[] = { ask_command, ask_flash, ask_wifi_scan, ask_gnss_begin, ask_radio_spectrum, ask_level, ask_send_raw };
    TEST_ASSERT_EQUAL_INT(KODE_OK, kinds[0](a));
    TEST_ASSERT_EQUAL_INT(KODE_OK, kinds[1](a));
    for (unsigned t = 0U; (t < 100U) && (g_core.inside < 2U); t++) {
        sleep_ms(10U);
    }
    TEST_ASSERT_EQUAL_UINT_MESSAGE(2U, g_core.inside, "both executors are busy");
    unsigned queued = 0U;
    for (unsigned k = 2U; k < 7U; k++) {
        TEST_ASSERT_EQUAL_INT(KODE_OK, kinds[k](a));
        queued++;
    }
    for (unsigned k = 0U; k < 3U; k++) {
        TEST_ASSERT_EQUAL_INT(KODE_OK, kinds[k](b));
        queued++;
    }
    TEST_ASSERT_EQUAL_UINT(FCK_SYS_QUEUE, queued);
    uint32_t const t0 = now_ms();
    TEST_ASSERT_EQUAL_INT(KODE_NO_RESOURCES, ask_command(c));
    TEST_ASSERT_EQUAL_INT(KODE_NO_RESOURCES, ask_gnss_end(c));
    TEST_ASSERT_TRUE_MESSAGE(now_ms() - t0 < 200U, "refused at once, not after a wait");
    g_core.hold = false;
    got_t g;
    unsigned answers = 0U;
    while (take(a, &g, 2000U)) {
        answers++;
        if (answers == 7U) {
            break;
        }
    }
    TEST_ASSERT_EQUAL_UINT(7U, answers);
    answers = 0U;
    while (take(b, &g, 2000U)) {
        answers++;
        if (answers == 3U) {
            break;
        }
    }
    TEST_ASSERT_EQUAL_UINT(3U, answers);
    TEST_ASSERT_TRUE(wait_quiet(500U));
    TEST_ASSERT_EQUAL_INT_MESSAGE(KODE_OK, ask_command(c), "the door the refusal took is free");
    TEST_ASSERT_TRUE(take(c, &g, 1500U));
}

TEST_CASE("a long request does not hold a short one up: the second executor serves it", "[sys][queue]")
{
    QActive *const a = ao_new(0U, 8U);
    QActive *const b = ao_new(1U, 8U);
    g_core.hold      = true;
    g_core.hold_name = "subghz_capture";
    TEST_ASSERT_EQUAL_INT(KODE_OK, ask_capture(a));
    sleep_ms(50U);
    TEST_ASSERT_EQUAL_INT(KODE_OK, ask_reset(b));
    got_t g;
    TEST_ASSERT_TRUE_MESSAGE(take(b, &g, 1500U), "the short one was answered while the long one waits");
    TEST_ASSERT_EQUAL_UINT((unsigned)FCK_RESET_RSP_SIG, (unsigned)g.sig);
    TEST_ASSERT_FALSE(take(a, &g, 0U));
    g_core.hold = false;
    TEST_ASSERT_TRUE(take(a, &g, 1500U));
    TEST_ASSERT_EQUAL_UINT((unsigned)FCK_SUBGHZ_CAPTURE_RSP_SIG, (unsigned)g.sig);
}

TEST_CASE("two long requests take both executors and a third waits for one of them", "[sys][queue]")
{
    QActive *const a = ao_new(0U, 8U);
    QActive *const b = ao_new(1U, 8U);
    QActive *const c = ao_new(2U, 8U);
    g_core.hold      = true;
    g_core.hold_name = "subghz_capture";
    TEST_ASSERT_EQUAL_INT(KODE_OK, ask_capture(a));
    TEST_ASSERT_EQUAL_INT(KODE_OK, ask_capture(b));
    for (unsigned t = 0U; (t < 100U) && (g_core.inside < 2U); t++) {
        sleep_ms(10U);
    }
    TEST_ASSERT_EQUAL_UINT(2U, g_core.inside);
    TEST_ASSERT_EQUAL_INT(KODE_OK, ask_reset(c));       /* not held, but no executor is free */
    got_t g;
    TEST_ASSERT_FALSE_MESSAGE(take(c, &g, 200U), "it waits its turn");
    g_core.hold = false;
    TEST_ASSERT_TRUE(take(c, &g, 2000U));
    TEST_ASSERT_TRUE(take(a, &g, 2000U));
    TEST_ASSERT_TRUE(take(b, &g, 2000U));
}

/* ---- Cancel ------------------------------------------------------------------------------------- */

TEST_CASE("fck_sys_cancel answers what runs and what is queued as cancelled, and touches what comes after", "[sys][cancel]")
{
    QActive *const a = ao_new(0U, 8U);
    QActive *const b = ao_new(1U, 8U);
    QActive *const c = ao_new(2U, 8U);
    g_core.hold = true;
    TEST_ASSERT_EQUAL_INT(KODE_OK, ask_capture(a));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, ask_find(a));      /* the same door as the capture */
    TEST_ASSERT_EQUAL_INT(KODE_OK, ask_wifi_scan(b));
    for (unsigned t = 0U; (t < 100U) && (g_core.inside < 2U); t++) {
        sleep_ms(10U);
    }
    TEST_ASSERT_EQUAL_INT(KODE_OK, ask_gnss_end(c));      /* queued behind the two that run */
    TEST_ASSERT_EQUAL_INT(KODE_OK, fck_sys_cancel());
    got_t g;
    TEST_ASSERT_TRUE(take(a, &g, 2000U));
    TEST_ASSERT_EQUAL_UINT((unsigned)FCK_SUBGHZ_CAPTURE_RSP_SIG, (unsigned)g.sig);
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, g.u.answer.rc);
    TEST_ASSERT_EQUAL_INT(FCK_WHY_CANCELLED, g.u.answer.why);
    TEST_ASSERT_TRUE(take(b, &g, 2000U));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, g.u.answer.rc);
    TEST_ASSERT_EQUAL_INT(FCK_WHY_CANCELLED, g.u.answer.why);
    TEST_ASSERT_TRUE(take(c, &g, 2000U));
    TEST_ASSERT_EQUAL_UINT((unsigned)FCK_GNSS_END_RSP_SIG, (unsigned)g.sig);
    TEST_ASSERT_EQUAL_INT_MESSAGE(KODE_DENIED, g.u.answer.rc, "queued before the cancel: answered cancelled");
    TEST_ASSERT_EQUAL_INT(FCK_WHY_CANCELLED, g.u.answer.why);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, fake_core_count("gnss_end"), "and never started");
    TEST_ASSERT_TRUE(wait_quiet(500U));

    g_core.hold = false;
    TEST_ASSERT_EQUAL_INT(KODE_OK, ask_gnss_end(c));
    TEST_ASSERT_TRUE(take(c, &g, 2000U));
    TEST_ASSERT_EQUAL_INT_MESSAGE(KODE_OK, g.u.answer.rc, "a request begun after the cancel is not touched");
    TEST_ASSERT_EQUAL_UINT(1U, fake_core_count("gnss_end"));
}

TEST_CASE("fck_sys_cancel returns the core's own status", "[sys][cancel]")
{
    TEST_ASSERT_EQUAL_INT(KODE_OK, fck_sys_cancel());
    g_core.next = KODE_DENIED;
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, fck_sys_cancel());
    TEST_ASSERT_EQUAL_UINT(2U, fake_core_count("cancel"));
}

/* ---- Scans ---------------------------------------------------------------------------------------- */

TEST_CASE("a Wi-Fi scan posts one entry per network, in order, then the answer", "[sys][scan]")
{
    QActive *const a = ao_new(0U, 16U);
    g_core.wifi_n = 5U;
    TEST_ASSERT_EQUAL_INT(KODE_OK, fck_sys_wifi_scan(a, FCK_BAND_5G));
    got_t g;
    for (unsigned i = 0U; i < 5U; i++) {
        TEST_ASSERT_TRUE(take(a, &g, 1500U));
        TEST_ASSERT_EQUAL_UINT((unsigned)FCK_WIFI_SCAN_ENTRY_SIG, (unsigned)g.sig);
        TEST_ASSERT_EQUAL_UINT(i, g.u.wifi.index);
        char want[8];
        (void)snprintf(want, sizeof(want), "net%u", i);
        TEST_ASSERT_EQUAL_STRING(want, g.u.wifi.network.ssid);
    }
    TEST_ASSERT_TRUE(take(a, &g, 1500U));
    TEST_ASSERT_EQUAL_UINT((unsigned)FCK_WIFI_SCAN_RSP_SIG, (unsigned)g.sig);
    TEST_ASSERT_EQUAL_UINT32(5U, g.u.answer.count);
    TEST_ASSERT_EQUAL_INT32(0, g.u.answer.value);
    TEST_ASSERT_EQUAL_UINT64((uint64_t)FCK_BAND_5G, fake_core_call("wifi_scan", 0U)->arg[0]);
    expect_nothing_more(a, "after the answer");
}

TEST_CASE("a BLE scan posts one entry per device, in order, then the answer", "[sys][scan]")
{
    QActive *const a = ao_new(0U, 16U);
    g_core.ble_n = 4U;
    TEST_ASSERT_EQUAL_INT(KODE_OK, fck_sys_ble_scan(a, 7U));
    got_t g;
    for (unsigned i = 0U; i < 4U; i++) {
        TEST_ASSERT_TRUE(take(a, &g, 1500U));
        TEST_ASSERT_EQUAL_UINT((unsigned)FCK_BLE_SCAN_ENTRY_SIG, (unsigned)g.sig);
        TEST_ASSERT_EQUAL_UINT(i, g.u.ble.index);
        TEST_ASSERT_EQUAL_UINT8(i, g.u.ble.device.addr[5]);
    }
    TEST_ASSERT_TRUE(take(a, &g, 1500U));
    TEST_ASSERT_EQUAL_UINT((unsigned)FCK_BLE_SCAN_RSP_SIG, (unsigned)g.sig);
    TEST_ASSERT_EQUAL_UINT32(4U, g.u.answer.count);
    TEST_ASSERT_EQUAL_UINT64(7U, fake_core_call("ble_scan", 0U)->arg[0]);
}

TEST_CASE("a BLE entry carries what its device advertised, whole", "[sys][scan]")
{
    QActive *const a = ao_new(0U, 16U);
    g_core.ble_n = 3U;
    TEST_ASSERT_EQUAL_INT(KODE_OK, fck_sys_ble_scan(a, 5U));
    got_t g;
    for (unsigned i = 0U; i < 3U; i++) {
        TEST_ASSERT_TRUE(take(a, &g, 1500U));
        TEST_ASSERT_EQUAL_UINT((unsigned)FCK_BLE_SCAN_ENTRY_SIG, (unsigned)g.sig);
        const fck_ble_device_t *const d = &g.u.ble.device;
        TEST_ASSERT_EQUAL_INT(FCK_BLE_ADVERTISING_NOT_CONNECTABLE, d->advertising);
        TEST_ASSERT_EQUAL_UINT32(100U * i, d->first_ms);
        TEST_ASSERT_EQUAL_UINT32((100U * i) + 50U, d->last_ms);
        TEST_ASSERT_EQUAL_UINT8(FCK_BLE_DATA_MAX, d->data_len);
        TEST_ASSERT_EQUAL_HEX8(FCK_BLE_DATA_MAX - 1U, d->data[0]);
        TEST_ASSERT_EQUAL_HEX8(0xFFU, d->data[1]);
        for (size_t b = 2U; b < FCK_BLE_DATA_MAX; b++) {
            TEST_ASSERT_EQUAL_HEX8((uint8_t)((i * 16U) + b), d->data[b]);
        }
    }
    TEST_ASSERT_TRUE(take(a, &g, 1500U));
    TEST_ASSERT_EQUAL_UINT((unsigned)FCK_BLE_SCAN_RSP_SIG, (unsigned)g.sig);
    TEST_ASSERT_EQUAL_UINT32(3U, g.u.answer.count);
}

TEST_CASE("a scan that finds nothing posts only the answer, count 0", "[sys][scan]")
{
    QActive *const a = ao_new(0U, 8U);
    TEST_ASSERT_EQUAL_INT(KODE_OK, ask_wifi_scan(a));
    got_t g;
    TEST_ASSERT_TRUE(take(a, &g, 1500U));
    TEST_ASSERT_EQUAL_UINT((unsigned)FCK_WIFI_SCAN_RSP_SIG, (unsigned)g.sig);
    TEST_ASSERT_EQUAL_UINT32(0U, g.u.answer.count);
    TEST_ASSERT_EQUAL_INT32(0, g.u.answer.value);
}

TEST_CASE("a scan that fails posts no entry, only the answer", "[sys][scan]")
{
    QActive *const a = ao_new(0U, 8U);
    g_core.wifi_n = 5U;
    g_core.next   = KODE_TIMEOUT;
    g_core.why    = FCK_AOS_WHY_TIMEOUT;
    TEST_ASSERT_EQUAL_INT(KODE_OK, ask_wifi_scan(a));
    got_t g;
    TEST_ASSERT_TRUE(take(a, &g, 1500U));
    TEST_ASSERT_EQUAL_UINT((unsigned)FCK_WIFI_SCAN_RSP_SIG, (unsigned)g.sig);
    TEST_ASSERT_EQUAL_INT(KODE_TIMEOUT, g.u.answer.rc);
    TEST_ASSERT_EQUAL_INT(FCK_WHY_TIMEOUT, g.u.answer.why);
    expect_nothing_more(a, "an entry after a failed scan");
}

TEST_CASE("a full queue loses entries and never the answer: value counts the lost ones", "[sys][scan]")
{
    unsigned const qlen = 8U;
    QActive *const a = ao_new(0U, qlen);
    g_core.wifi_n = 20U;
    TEST_ASSERT_EQUAL_INT(KODE_OK, ask_wifi_scan(a));
    TEST_ASSERT_TRUE(wait_quiet(1500U));
    TEST_ASSERT_EQUAL_UINT_MESSAGE(qlen - FCK_SYS_ENTRY_MARGIN + 1U, waiting(a),
                                   "the entries leave FCK_SYS_ENTRY_MARGIN slots free, then the answer takes one");
    got_t g;
    unsigned got_entries = 0U;
    bool answered = false;
    while (take(a, &g, 100U)) {
        if (g.sig == FCK_WIFI_SCAN_ENTRY_SIG) {
            TEST_ASSERT_EQUAL_UINT(got_entries, g.u.wifi.index);
            got_entries++;
        } else if (g.sig == FCK_WIFI_SCAN_RSP_SIG) {
            answered = true;
            TEST_ASSERT_EQUAL_UINT32(20U, g.u.answer.count);
            TEST_ASSERT_EQUAL_INT32((int32_t)(20U - got_entries), g.u.answer.value);
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(answered, "the answer is never lost to the entries");
    TEST_ASSERT_EQUAL_UINT(qlen - FCK_SYS_ENTRY_MARGIN, got_entries);
}

TEST_CASE("a BLE scan with a full queue loses entries and keeps the answer", "[sys][scan]")
{
    unsigned const qlen = 6U;
    QActive *const a = ao_new(0U, qlen);
    g_core.ble_n = 30U;
    TEST_ASSERT_EQUAL_INT(KODE_OK, ask_ble_scan(a));
    TEST_ASSERT_TRUE(wait_quiet(1500U));
    got_t g;
    unsigned entries = 0U;
    bool answered    = false;
    while (take(a, &g, 100U)) {
        if (g.sig == FCK_BLE_SCAN_ENTRY_SIG) {
            entries++;
        } else {
            answered = true;
            TEST_ASSERT_EQUAL_UINT32(30U, g.u.answer.count);
            TEST_ASSERT_EQUAL_INT32((int32_t)(30U - (qlen - FCK_SYS_ENTRY_MARGIN)), g.u.answer.value);
        }
    }
    TEST_ASSERT_TRUE(answered);
    TEST_ASSERT_EQUAL_UINT(qlen - FCK_SYS_ENTRY_MARGIN, entries);
}

TEST_CASE("a pool nearly dry loses entries too, and the answer still finds a block", "[sys][scan]")
{
    QActive *const a = ao_new(0U, 32U);
    g_core.wifi_n = 10U;
    hog_leave(3U);                       /* 3 free: an entry needs more than POOL_MARGIN (2) free */
    TEST_ASSERT_EQUAL_INT(KODE_OK, ask_wifi_scan(a));
    got_t g;
    unsigned entries = 0U;
    bool answered    = false;
    for (unsigned k = 0U; k < 12U; k++) {
        if (!take(a, &g, 800U)) {
            break;
        }
        if (g.sig == FCK_WIFI_SCAN_ENTRY_SIG) {
            entries++;
        } else {
            answered = true;
            TEST_ASSERT_EQUAL_INT32((int32_t)(10U - entries), g.u.answer.value);
            break;
        }
    }
    TEST_ASSERT_TRUE(answered);
    TEST_ASSERT_EQUAL_UINT(1U, entries);
}

/* ---- The answer's own post: a queue full, a pool dry ---------------------------------------------- */

TEST_CASE("an answer that finds the queue full waits and arrives when a slot is free", "[sys][answer][full]")
{
    QActive *const a = ao_new(0U, 2U);
    fill(a, 2U);
    TEST_ASSERT_EQUAL_INT(KODE_OK, ask_reset(a));
    sleep_ms(250U);
    TEST_ASSERT_EQUAL_UINT(2U, waiting(a));
    got_t g;
    TEST_ASSERT_TRUE(take(a, &g, 0U));        /* the app reads one of its own events */
    TEST_ASSERT_EQUAL_UINT(7U, (unsigned)g.sig);
    TEST_ASSERT_TRUE(take(a, &g, 0U));
    TEST_ASSERT_TRUE_MESSAGE(take(a, &g, 1500U), "the answer came once there was room");
    TEST_ASSERT_EQUAL_UINT((unsigned)FCK_RESET_RSP_SIG, (unsigned)g.sig);
    TEST_ASSERT_EQUAL_INT(KODE_OK, g.u.answer.rc);
    TEST_ASSERT_EQUAL_UINT32(0U, g_aos_dropped[FCK_AOS_SINK_ANSWERS]);
}

TEST_CASE("an answer that finds no room for a second is lost, logged, counted, and the door is open", "[sys][answer][full]")
{
    QActive *const a = ao_new(0U, 2U);
    fill(a, 2U);
    TEST_ASSERT_EQUAL_INT(KODE_OK, ask_reset(a));
    uint32_t const t0 = now_ms();
    TEST_ASSERT_TRUE(wait_dropped(1U, 3000U));
    uint32_t const took = now_ms() - t0;
    TEST_ASSERT_TRUE_MESSAGE(took >= 900U && took < 2500U, "about a second, not for ever");
    TEST_ASSERT_EQUAL_UINT32(1U, g_aos_dropped[FCK_AOS_SINK_ANSWERS]);
    TEST_ASSERT_TRUE_MESSAGE(g_log_warnings >= 1U, "logged");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(2U, waiting(a), "nothing but its own events");
    TEST_ASSERT_EQUAL_INT_MESSAGE(KODE_OK, ask_reset(a), "the door was open all the same");
    ao_drain(a);
    got_t g;
    TEST_ASSERT_TRUE(take(a, &g, 1500U));
}

TEST_CASE("an answer that finds the pool dry waits for a block and arrives when one is back", "[sys][answer][full]")
{
    QActive *const a = ao_new(0U, 8U);
    hog_leave(0U);
    TEST_ASSERT_EQUAL_INT(KODE_OK, ask_power(a));
    sleep_ms(300U);
    TEST_ASSERT_EQUAL_UINT(0U, waiting(a));
    hog_release();
    got_t g;
    TEST_ASSERT_TRUE(take(a, &g, 1500U));
    TEST_ASSERT_EQUAL_UINT((unsigned)FCK_POWER_RSP_SIG, (unsigned)g.sig);
    TEST_ASSERT_EQUAL_UINT32(0U, g_aos_dropped[FCK_AOS_SINK_ANSWERS]);
}

TEST_CASE("an answer that finds the pool dry for a second is lost and counted", "[sys][answer][full]")
{
    QActive *const a = ao_new(0U, 8U);
    hog_leave(0U);
    TEST_ASSERT_EQUAL_INT(KODE_OK, ask_power(a));
    TEST_ASSERT_TRUE(wait_dropped(1U, 3000U));
    TEST_ASSERT_EQUAL_UINT32(1U, g_aos_dropped[FCK_AOS_SINK_ANSWERS]);
    TEST_ASSERT_EQUAL_UINT(0U, waiting(a));
    hog_release();
    TEST_ASSERT_EQUAL_INT(KODE_OK, ask_power(a));
    got_t g;
    TEST_ASSERT_TRUE(take(a, &g, 1500U));
}

/* ---- Start and end ------------------------------------------------------------------------------- */

TEST_CASE("fck_sys_begin asks the core to begin, sets the news sink and returns the core's status", "[sys][begin]")
{
    s_sink_set = false;
    fck_aos_sink_set(FCK_AOS_SINK_NEWS, NULL, NULL);
    TEST_ASSERT_FALSE(fake_aos_news_sink_is_set());
    TEST_ASSERT_EQUAL_INT(KODE_OK, fck_sys_begin());
    TEST_ASSERT_EQUAL_UINT(1U, fake_core_count("begin"));
    TEST_ASSERT_TRUE(fake_aos_news_sink_is_set());
    TEST_ASSERT_EQUAL_INT_MESSAGE(KODE_OK, fck_sys_begin(), "idempotent");
    TEST_ASSERT_EQUAL_UINT(2U, fake_core_count("begin"));
    static const kode_status_t k_st[] = { KODE_DENIED, KODE_EXISTS, KODE_NO_RESOURCES, KODE_ERROR, KODE_IO_ERROR, KODE_TIMEOUT };
    for (size_t i = 0U; i < sizeof(k_st) / sizeof(k_st[0]); i++) {
        g_core.next = k_st[i];
        TEST_ASSERT_EQUAL_INT(k_st[i], fck_sys_begin());
    }
}

TEST_CASE("fck_sys_end before the begin returns OK and does nothing", "[sys][end]")
{
    g_aos_started = false;
    TEST_ASSERT_EQUAL_INT(KODE_OK, fck_sys_end());
    sleep_ms(100U);
    TEST_ASSERT_EQUAL_UINT(0U, g_core.calls);
}

TEST_CASE("fck_sys_end returns at once, cuts what is on the air, ends on its own task and tells the module was lost", "[sys][end]")
{
    QActive *const a = ao_new(0U, 8U);
    QActive *const s = ao_new(1U, 8U);
    TEST_ASSERT_EQUAL_INT(KODE_OK, fck_sys_subscribe(s));
    g_core.hold      = true;
    g_core.hold_name = "subghz_send_raw";
    TEST_ASSERT_EQUAL_INT(KODE_OK, ask_send_raw(a));
    for (unsigned t = 0U; (t < 100U) && (g_core.inside < 1U); t++) {
        sleep_ms(10U);
    }
    uint32_t const t0 = now_ms();
    TEST_ASSERT_EQUAL_INT(KODE_OK, fck_sys_end());
    TEST_ASSERT_TRUE_MESSAGE(now_ms() - t0 < 100U, "at once");
    got_t g;
    TEST_ASSERT_TRUE(take(a, &g, 2000U));
    TEST_ASSERT_EQUAL_UINT((unsigned)FCK_SUBGHZ_SEND_RAW_RSP_SIG, (unsigned)g.sig);
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, g.u.answer.rc);
    TEST_ASSERT_EQUAL_INT(FCK_WHY_CANCELLED, g.u.answer.why);
    TEST_ASSERT_TRUE(take(s, &g, 2000U));
    TEST_ASSERT_EQUAL_UINT((unsigned)FCK_MODULE_LOST_SIG, (unsigned)g.sig);
    TEST_ASSERT_EQUAL_INT32(0, g.u.news.value);
    TEST_ASSERT_EQUAL_UINT(1U, fake_core_count("end"));
    TEST_ASSERT_TRUE(wait_quiet(1000U));
}

TEST_CASE("fck_sys_end of a module that was not there tells nothing", "[sys][end]")
{
    QActive *const s = ao_new(0U, 8U);
    TEST_ASSERT_EQUAL_INT(KODE_OK, fck_sys_subscribe(s));
    g_aos_present = false;
    TEST_ASSERT_EQUAL_INT(KODE_OK, fck_sys_end());
    TEST_ASSERT_TRUE(wait_quiet(1500U));
    TEST_ASSERT_EQUAL_UINT(1U, fake_core_count("end"));
    got_t g;
    TEST_ASSERT_FALSE(take(s, &g, 100U));
}

TEST_CASE("a second fck_sys_end while the first is under way is OK and ends once", "[sys][end]")
{
    g_core.hold      = true;
    g_core.hold_name = "end";
    TEST_ASSERT_EQUAL_INT(KODE_OK, fck_sys_end());
    for (unsigned t = 0U; (t < 100U) && (g_core.inside < 1U); t++) {
        sleep_ms(10U);
    }
    TEST_ASSERT_EQUAL_INT(KODE_OK, fck_sys_end());
    TEST_ASSERT_EQUAL_INT(KODE_OK, fck_sys_end());
    g_core.hold = false;
    TEST_ASSERT_TRUE(wait_quiet(1500U));
    TEST_ASSERT_EQUAL_UINT(1U, fake_core_count("end"));
    TEST_ASSERT_EQUAL_INT(KODE_OK, fck_sys_end());      /* and a later one ends again */
    TEST_ASSERT_TRUE(wait_quiet(1500U));
    TEST_ASSERT_EQUAL_UINT(2U, fake_core_count("end"));
}

TEST_CASE("fck_sys_begin while the end is still under way is refused with KODE_DENIED, and works after", "[sys][end]")
{
    g_core.hold      = true;
    g_core.hold_name = "end";
    TEST_ASSERT_EQUAL_INT(KODE_OK, fck_sys_end());
    sleep_ms(50U);
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, fck_sys_begin());
    TEST_ASSERT_EQUAL_UINT(0U, fake_core_count("begin"));
    g_core.hold = false;
    TEST_ASSERT_TRUE(wait_quiet(1500U));
    TEST_ASSERT_EQUAL_INT(KODE_OK, fck_sys_begin());
    TEST_ASSERT_EQUAL_UINT(1U, fake_core_count("begin"));
}

TEST_CASE("the subscribers stay across fck_sys_end and fck_sys_begin", "[sys][end]")
{
    QActive *const s = ao_new(0U, 8U);
    TEST_ASSERT_EQUAL_INT(KODE_OK, fck_sys_subscribe(s));
    TEST_ASSERT_EQUAL_INT(KODE_OK, fck_sys_end());
    TEST_ASSERT_TRUE(wait_quiet(1500U));
    ao_drain(s);
    TEST_ASSERT_EQUAL_INT(KODE_OK, fck_sys_begin());
    TEST_ASSERT_TRUE(fake_aos_news(FCK_AOS_EVENT_PRESENT, 0U));
    got_t g;
    TEST_ASSERT_TRUE(take(s, &g, 500U));
    TEST_ASSERT_EQUAL_UINT((unsigned)FCK_MODULE_FOUND_SIG, (unsigned)g.sig);
}

/* ---- The news ------------------------------------------------------------------------------------- */

TEST_CASE("fck_sys_subscribe: NULL is refused, a second time is OK, past FCK_SUBSCRIBERS_MAX is NO_RESOURCES", "[sys][subscribe]")
{
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, fck_sys_subscribe(NULL));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, fck_sys_unsubscribe(NULL));
    QActive *as[FCK_SUBSCRIBERS_MAX + 1U];
    for (unsigned i = 0U; i <= FCK_SUBSCRIBERS_MAX; i++) {
        as[i] = ao_new(i, 8U);
    }
    for (unsigned i = 0U; i < FCK_SUBSCRIBERS_MAX; i++) {
        TEST_ASSERT_EQUAL_INT(KODE_OK, fck_sys_subscribe(as[i]));
    }
    TEST_ASSERT_EQUAL_INT(KODE_OK, fck_sys_subscribe(as[0]));
    TEST_ASSERT_EQUAL_INT(KODE_OK, fck_sys_subscribe(as[FCK_SUBSCRIBERS_MAX - 1U]));
    TEST_ASSERT_EQUAL_INT(KODE_NO_RESOURCES, fck_sys_subscribe(as[FCK_SUBSCRIBERS_MAX]));
    TEST_ASSERT_EQUAL_INT(KODE_OK, fck_sys_unsubscribe(as[1]));
    TEST_ASSERT_EQUAL_INT(KODE_OK, fck_sys_subscribe(as[FCK_SUBSCRIBERS_MAX]));
    TEST_ASSERT_EQUAL_INT_MESSAGE(KODE_OK, fck_sys_unsubscribe(as[1]), "also when it was not subscribed");
}

TEST_CASE("fck_sys_subscribe sets the news sink even before fck_sys_begin", "[sys][subscribe]")
{
    QActive *const a = ao_new(0U, 8U);
    s_sink_set = false;
    fck_aos_sink_set(FCK_AOS_SINK_NEWS, NULL, NULL);
    g_aos_started = false;
    TEST_ASSERT_EQUAL_INT(KODE_OK, fck_sys_subscribe(a));
    TEST_ASSERT_TRUE(fake_aos_news_sink_is_set());
}

static const struct {
    fck_aos_event_t ev;
    uint32_t        arg;
    QSignal         sig;
    int32_t         value;
    const char     *name;
} k_news[] = {
    { FCK_AOS_EVENT_PRESENT,          0U,                        FCK_MODULE_FOUND_SIG,     1, "module found" },
    { FCK_AOS_EVENT_PRESENT,          5U,                        FCK_MODULE_FOUND_SIG,     1, "module found, an arg that is ignored" },
    { FCK_AOS_EVENT_GONE,             0U,                        FCK_MODULE_LOST_SIG,      0, "module lost" },
    { FCK_AOS_EVENT_GONE,             9U,                        FCK_MODULE_LOST_SIG,      0, "module lost, an arg that is ignored" },
    { FCK_AOS_EVENT_WRONG_MODULE,     FCK_AOS_SEEN_HOLDS_PAIR,   FCK_WRONG_MODULE_SIG,     (int32_t)FCK_WRONG_MODULE_HOLDS_PAIR, "wrong module, a pair held" },
    { FCK_AOS_EVENT_WRONG_MODULE,     FCK_AOS_SEEN_OTHER_CHIP,   FCK_WRONG_MODULE_SIG,     (int32_t)FCK_WRONG_MODULE_OTHER_CHIP, "wrong module, another chip" },
    { FCK_AOS_EVENT_SETTING_LOST,     FCK_AOS_SETTING_GNSS | FCK_AOS_SETTING_LISTEN, FCK_SETTING_LOST_SIG, (int32_t)(FCK_SETTING_GNSS | FCK_SETTING_LISTEN), "setting lost" },
    { FCK_AOS_EVENT_SETTING_RESTORED, FCK_AOS_SETTING_LISTEN,    FCK_SETTING_RESTORED_SIG, (int32_t)FCK_SETTING_LISTEN, "setting restored" },
    { FCK_AOS_EVENT_PACKET_RECEIVED,  12U,                       FCK_PACKET_RECEIVED_SIG,  12, "packet received" },
    { FCK_AOS_EVENT_SEND_CANCELLED,   FCK_AOS_CANCEL_CALL,       FCK_SEND_CANCELLED_SIG,   FCK_CANCEL_CALL, "send cancelled by a call" },
    { FCK_AOS_EVENT_SEND_CANCELLED,   FCK_AOS_CANCEL_LEAVING,    FCK_SEND_CANCELLED_SIG,   FCK_CANCEL_LEAVING, "send cancelled by leaving" },
};

TEST_CASE("each event of the owner is posted to the subscriber as its signal and its value", "[sys][news]")
{
    QActive *const a = ao_new(0U, 8U);
    TEST_ASSERT_EQUAL_INT(KODE_OK, fck_sys_subscribe(a));
    for (size_t i = 0U; i < sizeof(k_news) / sizeof(k_news[0]); i++) {
        TEST_ASSERT_TRUE_MESSAGE(fake_aos_news(k_news[i].ev, k_news[i].arg), k_news[i].name);
        got_t g;
        TEST_ASSERT_TRUE_MESSAGE(take(a, &g, 100U), k_news[i].name);
        TEST_ASSERT_EQUAL_UINT_MESSAGE((unsigned)k_news[i].sig, (unsigned)g.sig, k_news[i].name);
        TEST_ASSERT_EQUAL_INT32_MESSAGE(k_news[i].value, g.u.news.value, k_news[i].name);
        TEST_ASSERT_FALSE_MESSAGE(take(a, &g, 0U), k_news[i].name);
    }
}

TEST_CASE("an event that is none of the owner's is taken and posted to nobody", "[sys][news]")
{
    QActive *const a = ao_new(0U, 8U);
    TEST_ASSERT_EQUAL_INT(KODE_OK, fck_sys_subscribe(a));
    TEST_ASSERT_TRUE(fake_aos_news((fck_aos_event_t)99, 0U));
    TEST_ASSERT_EQUAL_UINT(0U, waiting(a));
}

TEST_CASE("the news go to every subscriber, and not to one that unsubscribed", "[sys][news]")
{
    QActive *const a = ao_new(0U, 8U);
    QActive *const b = ao_new(1U, 8U);
    QActive *const c = ao_new(2U, 8U);
    TEST_ASSERT_EQUAL_INT(KODE_OK, fck_sys_subscribe(a));
    TEST_ASSERT_EQUAL_INT(KODE_OK, fck_sys_subscribe(b));
    TEST_ASSERT_EQUAL_INT(KODE_OK, fck_sys_subscribe(c));
    TEST_ASSERT_EQUAL_INT(KODE_OK, fck_sys_unsubscribe(b));
    TEST_ASSERT_TRUE(fake_aos_news(FCK_AOS_EVENT_PRESENT, 0U));
    TEST_ASSERT_EQUAL_UINT(1U, waiting(a));
    TEST_ASSERT_EQUAL_UINT(0U, waiting(b));
    TEST_ASSERT_EQUAL_UINT(1U, waiting(c));
}

TEST_CASE("with no subscriber the news are taken and nothing is allocated", "[sys][news]")
{
    TEST_ASSERT_TRUE(fake_aos_news(FCK_AOS_EVENT_PRESENT, 0U));
    hog_leave(0U);
    TEST_ASSERT_TRUE(fake_aos_news(FCK_AOS_EVENT_GONE, 0U));
}

TEST_CASE("a subscriber whose queue is nearly full loses the news and the owner is told: the others still get them", "[sys][news]")
{
    unsigned const qlen = 5U;
    QActive *const slow = ao_new(0U, qlen);
    QActive *const fast = ao_new(1U, 16U);
    TEST_ASSERT_EQUAL_INT(KODE_OK, fck_sys_subscribe(slow));
    TEST_ASSERT_EQUAL_INT(KODE_OK, fck_sys_subscribe(fast));
    unsigned taken = 0U;
    for (unsigned i = 0U; i < 8U; i++) {
        if (fake_aos_news(FCK_AOS_EVENT_PACKET_RECEIVED, i + 1U)) {
            taken++;
        }
    }
    TEST_ASSERT_EQUAL_UINT_MESSAGE(qlen - FCK_SYS_NEWS_MARGIN, taken, "the news leave FCK_SYS_NEWS_MARGIN slots free");
    TEST_ASSERT_EQUAL_UINT(qlen - FCK_SYS_NEWS_MARGIN, waiting(slow));
    TEST_ASSERT_EQUAL_UINT(8U, waiting(fast));
}

TEST_CASE("the news with the pool nearly dry are lost, the owner is told, and the pool is left to the rest", "[sys][news]")
{
    QActive *const a = ao_new(0U, 8U);
    TEST_ASSERT_EQUAL_INT(KODE_OK, fck_sys_subscribe(a));
    hog_leave(POOL_MARGIN);
    TEST_ASSERT_FALSE(fake_aos_news(FCK_AOS_EVENT_PRESENT, 0U));
    TEST_ASSERT_EQUAL_UINT(0U, waiting(a));
    hog_release();
    hog_leave(POOL_MARGIN + 1U);
    TEST_ASSERT_TRUE(fake_aos_news(FCK_AOS_EVENT_PRESENT, 0U));
    TEST_ASSERT_EQUAL_UINT(1U, waiting(a));
}

TEST_CASE("a news and an answer can be in the same queue, each as its own signal", "[sys][news]")
{
    QActive *const a = ao_new(0U, 8U);
    TEST_ASSERT_EQUAL_INT(KODE_OK, fck_sys_subscribe(a));
    g_core.hold = true;
    TEST_ASSERT_EQUAL_INT(KODE_OK, ask_gnss_begin(a));
    TEST_ASSERT_TRUE(fake_aos_news(FCK_AOS_EVENT_GONE, 0U));
    g_core.hold = false;
    got_t g;
    TEST_ASSERT_TRUE(take(a, &g, 1500U));
    TEST_ASSERT_EQUAL_UINT((unsigned)FCK_MODULE_LOST_SIG, (unsigned)g.sig);
    TEST_ASSERT_TRUE(take(a, &g, 1500U));
    TEST_ASSERT_EQUAL_UINT((unsigned)FCK_GNSS_BEGIN_RSP_SIG, (unsigned)g.sig);
}

/* ---- Commands and immediates ---------------------------------------------------------------------- */

TEST_CASE("the commands pass their arguments to the core and return its status", "[sys][commands]")
{
    static const kode_status_t k_all[] = { KODE_OK, KODE_DENIED, KODE_NO_RESOURCES, KODE_ERROR };
    fck_radio_sniff_config_t cfg;
    (void)memset(&cfg, 0, sizeof(cfg));
    for (size_t i = 0U; i < sizeof(k_all) / sizeof(k_all[0]); i++) {
        fake_core_reset();
        g_core.next = k_all[i];
        TEST_ASSERT_EQUAL_INT(k_all[i], fck_sys_subghz_listen_start(433920000UL));
        TEST_ASSERT_EQUAL_UINT64(433920000UL, fake_core_call("subghz_listen_start", 0U)->arg[0]);
        TEST_ASSERT_EQUAL_INT(k_all[i], fck_sys_subghz_listen_stop());
        TEST_ASSERT_EQUAL_INT(k_all[i], fck_sys_radio_sniff_tune(&cfg));
        TEST_ASSERT_EQUAL_UINT64((uint64_t)(uintptr_t)&cfg, fake_core_call("radio_sniff_tune_post", 0U)->arg[0]);
        TEST_ASSERT_EQUAL_INT(k_all[i], fck_sys_cancel());
        TEST_ASSERT_EQUAL_UINT(4U, g_core.calls);
    }
}

TEST_CASE("the immediates pass their arguments to the core and return its status", "[sys][immediate]")
{
    static const kode_status_t k_all[] = { KODE_OK, KODE_DENIED, KODE_NO_RESOURCES };
    for (size_t i = 0U; i < sizeof(k_all) / sizeof(k_all[0]); i++) {
        fake_core_reset();
        g_core.next = k_all[i];
        fck_status_t        st;
        fck_gnss_fix_t      fix;
        int32_t             us[4];
        size_t              n = 0U;
        fck_subghz_heard_t  heard;
        uint32_t            hz[4];
        uint32_t            ms = 0U;
        fck_subghz_protocol_info_t info;
        TEST_ASSERT_EQUAL_INT(k_all[i], fck_sys_status_read(&st));
        TEST_ASSERT_EQUAL_INT(k_all[i], fck_sys_gnss_read(&fix));
        TEST_ASSERT_EQUAL_UINT64_MESSAGE(0U, fake_core_call("gnss_read", 0U)->arg[1], "it waits for no fix: ms 0");
        TEST_ASSERT_EQUAL_INT(k_all[i], fck_sys_subghz_listen_read(7U, us, 4U, &n, &heard));
        const fake_call_t *c = fake_core_call("subghz_listen_read", 0U);
        TEST_ASSERT_EQUAL_UINT64(7U, c->arg[0]);
        TEST_ASSERT_EQUAL_UINT64_MESSAGE(0U, c->arg[1], "it waits for no capture: ms 0");
        TEST_ASSERT_EQUAL_UINT64(4U, c->arg[3]);
        TEST_ASSERT_EQUAL_INT(k_all[i], fck_sys_subghz_frequencies(hz, 4U, &n));
        TEST_ASSERT_EQUAL_INT(k_all[i], fck_sys_subghz_air_time_left(HZ, &ms));
        TEST_ASSERT_EQUAL_UINT64(HZ, fake_core_call("subghz_air_time_left", 0U)->arg[0]);
        TEST_ASSERT_EQUAL_INT(k_all[i], fck_sys_subghz_protocol_info((fck_subghz_protocol_t)0, &info));
        TEST_ASSERT_EQUAL_UINT(6U, g_core.calls);
    }
}

TEST_CASE("the flags and the plain answers of the immediates come from the module's state", "[sys][immediate]")
{
    g_core.present = true;
    TEST_ASSERT_TRUE(fck_sys_is_present());
    g_core.present = false;
    TEST_ASSERT_FALSE(fck_sys_is_present());
    g_core.hz = 868300000UL;
    TEST_ASSERT_EQUAL_UINT32(868300000UL, fck_sys_subghz_listen_hz());
    g_core.hz = 0U;
    TEST_ASSERT_EQUAL_UINT32(FCK_SUBGHZ_HOP, fck_sys_subghz_listen_hz());
    g_core.may = true;
    TEST_ASSERT_TRUE(fck_sys_subghz_may_transmit(HZ));
    g_core.may = false;
    TEST_ASSERT_FALSE(fck_sys_subghz_may_transmit(HZ));
    g_core.rolling = true;
    TEST_ASSERT_TRUE(fck_sys_subghz_is_rolling(s_us, 8U));
    g_core.rolling = false;
    TEST_ASSERT_FALSE(fck_sys_subghz_is_rolling(s_us, 8U));
    TEST_ASSERT_EQUAL_UINT8(5U, fck_sys_header_position(4U));
    TEST_ASSERT_EQUAL_UINT(0U, g_core.calls);       /* none of them asks the core to do anything */
}

TEST_CASE("none of the immediates, commands or the news waits: each returns within a few ticks with the executors busy", "[sys][immediate]")
{
    QActive *const a = ao_new(0U, 8U);
    g_core.hold = true;
    TEST_ASSERT_EQUAL_INT(KODE_OK, ask_capture(a));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, ask_find(a));
    uint32_t const t0 = now_ms();
    fck_status_t st;
    uint32_t     ms;
    (void)fck_sys_status_read(&st);
    (void)fck_sys_is_present();
    (void)fck_sys_subghz_air_time_left(HZ, &ms);
    (void)fck_sys_subghz_listen_stop();
    (void)fck_sys_subscribe(a);
    (void)fck_sys_unsubscribe(a);
    (void)fake_aos_news(FCK_AOS_EVENT_GONE, 0U);
    TEST_ASSERT_TRUE(now_ms() - t0 < 100U);
    g_core.hold = false;
    got_t g;
    TEST_ASSERT_TRUE(take(a, &g, 2000U));
}

/* ---- Any call, in any state ------------------------------------------------------------------------ */

TEST_CASE("any call in any state: it returns one of three codes, an answer comes if and only if it was taken, and every door is free at the end", "[sys][any]")
{
    static const kode_status_t k_core[] = { KODE_OK, KODE_DENIED, KODE_TIMEOUT };
    unsigned runs = 0U;
    for (unsigned begun = 0U; begun < 2U; begun++) {
        for (unsigned who = 0U; who < 3U; who++) {          /* a started AO, NULL, an AO not started */
            for (size_t s = 0U; s < sizeof(k_core) / sizeof(k_core[0]); s++) {
                for (unsigned busy = 0U; busy < 2U; busy++) {
                    for (size_t i = 0U; i < ROWS; i++) {
                        char msg[96];
                        QActive *const ok = ao_new(0U, 16U);
                        QActive *const unstarted = ao_unstarted(1U);
                        QActive *const me = (who == 0U) ? ok : ((who == 1U) ? NULL : unstarted);
                        fake_core_reset();
                        g_aos_started = (begun != 0U);
                        g_core.next   = k_core[s];
                        g_core.wifi_n = 2U;
                        g_core.ble_n  = 2U;
                        g_core.hold   = (busy != 0U);
                        bool first_taken = false;
                        if (busy != 0U) {
                            first_taken = (ROW[i].ask(ok) == KODE_OK);       /* its door is now busy */
                        }
                        (void)snprintf(msg, sizeof(msg), "fck_sys_%s() begun %u who %u core %d busy %u", ROW[i].name, begun, who,
                                       (int)k_core[s], busy);
                        kode_status_t const rc = ROW[i].ask(me);
                        TEST_ASSERT_TRUE_MESSAGE(rc == KODE_OK || rc == KODE_DENIED || rc == KODE_NO_RESOURCES, msg);
                        bool const expect_taken = (begun != 0U) && (who == 0U) && !first_taken;
                        TEST_ASSERT_EQUAL_INT_MESSAGE(expect_taken ? KODE_OK : KODE_DENIED, rc, msg);
                        g_core.hold = false;
                        unsigned answers = 0U;
                        got_t g;
                        unsigned const want = (first_taken ? 1U : 0U) + ((rc == KODE_OK) ? 1U : 0U);
                        for (unsigned t = 0U; t < 200U && answers < want; t++) {
                            if (take(ok, &g, 20U) && g.sig >= FCK_COMMAND_RSP_SIG) {
                                answers++;
                            }
                        }
                        TEST_ASSERT_EQUAL_UINT_MESSAGE(want, answers, msg);
                        ao_drain(ok);
                        TEST_ASSERT_TRUE_MESSAGE(wait_quiet(1500U), msg);
                        if (who != 0U) {
                            TEST_ASSERT_TRUE_MESSAGE(quiet(), msg);
                        }
                        runs++;
                    }
                }
            }
        }
    }
    TEST_ASSERT_EQUAL_UINT(2U * 3U * 3U * 2U * ROWS, runs);
}
