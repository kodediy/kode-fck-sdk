/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_ao.c
 * @brief fck_ao.c: the owner's Active Object on kode-sdk's own QP/C,
 *        dispatched by hand.
 *
 * fck_ao.c is built into this file, so a case reaches its state machine as
 * QP/C does: Fck_ctor(), then QASM_INIT, then each event taken off the queue
 * and dispatched, its block handed back. The queue is a FreeRTOS one laid
 * out where the port reads it, so a post with a margin counts its room as on
 * the board; the pool is QF's own, with a count the cases know. What is not
 * here is the AO's task: no QACTIVE_START, so nothing dispatches but the
 * case. The worker is here as it ships: Fck_ctor() starts it, and a job a
 * case hands it runs on the host port's scheduler and answers in the owner's
 * queue. The AO's task, its level and the time it takes are the board's to
 * show.
 *
 * The other side of the seam, fck_aos.c's fck_aos_on_*(), is faked below:
 * each counts its calls and keeps what it was given.
 */
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* What fck_ao.c asks for its worker. The host port has one core, and asks
 * for core 0 or none; the board has two, and the worker is pinned to core 1
 * (fck_drv_tca6408a.h). So the task is made on the host's core, and the core
 * asked for is kept for a case to check. */
static struct {
    char        name[16];
    uint32_t    stack_bytes;
    UBaseType_t prio;
    BaseType_t  core;
    unsigned    created;
} g_worker;

static BaseType_t create_on_the_host_core(TaskFunction_t fn, const char * const name,
                                          const uint32_t stack_bytes,
                                          void * const arg, UBaseType_t prio,
                                          TaskHandle_t * const handle, const BaseType_t core)
{
    (void)strncpy(g_worker.name, name, sizeof(g_worker.name) - 1U);
    g_worker.stack_bytes = (uint32_t)stack_bytes;
    g_worker.prio        = prio;
    g_worker.core        = core;
    g_worker.created++;
    return xTaskCreatePinnedToCore(fn, name, stack_bytes, arg, prio, handle, 0);
}

/* task.h is in already, so its declaration stays as it is. */
#define xTaskCreatePinnedToCore create_on_the_host_core

#include "fck_ao.c"

#include <stdio.h>
#include <stdlib.h>

#include "freertos/semphr.h"
#include "unity.h"

#include "fake_kode.h"

/* QP/C's registry of started Active Objects: private to QP/C (qp_pkg.h), and the table a task is
 * told to be an Active Object's by (kqp_current_is_ao()). The cases put a stand-in in it. */
extern QActive *QActive_registry_[QF_MAX_ACTIVE + 1U];

/* fck_ao.h leaves a request opaque and fck_aos.c says what is in it. The
 * Active Object only carries its pointer, and so do these cases. */
struct fck_aos_request {
    int tag;
};

/* ---- fck_aos.c, faked at fck_ao.h ------------------------------------------ */

#define T_SEQS 4U   /* the jobs' numbers kept, in the order their answers came */

static struct {
    unsigned           requests;
    fck_aos_request_t *last;
    unsigned           ticks;
    unsigned           rests;
    bool               rest_off;      /* the last rest was for OFF                  */
    unsigned           wakes;
    unsigned           powers;
    bool               power_on;      /* the last EXP_POWER_SIG's switch ...        */
    uint8_t            power_rc;      /* ... and its rc                             */
    unsigned           homes;
    unsigned           dones;
    uint16_t           done_seq[T_SEQS];
    kode_status_t      done_status;
    uint32_t           done_value;
} g_on;

void fck_aos_on_request(fck_aos_request_t *request)
{
    g_on.requests++;
    g_on.last = request;
}

void fck_aos_on_tick(void)
{
    g_on.ticks++;
}

void fck_aos_on_rest(bool off)
{
    g_on.rests++;
    g_on.rest_off = off;
}

void fck_aos_on_wake(void)
{
    g_on.wakes++;
}

void fck_aos_on_exp_power(bool on, uint8_t rc)
{
    g_on.powers++;
    g_on.power_on = on;
    g_on.power_rc = rc;
}

void fck_aos_on_home(void)
{
    g_on.homes++;
}

void fck_aos_on_work_done(uint16_t seq, kode_status_t status, uint32_t value)
{
    if (g_on.dones < T_SEQS) {
        g_on.done_seq[g_on.dones] = seq;
    }
    g_on.dones++;
    g_on.done_status = status;
    g_on.done_value  = value;
}

/* ---- QF, brought up once ------------------------------------------------------ */

/* One pool, of blocks wide enough for anything the owner is sent, and a count
 * the cases rely on: more than the queue takes past its margin, so the queue
 * is what refuses first unless a case drains the pool on purpose. */
#define T_BLOCK                                                                      \
    ((((sizeof(FckRequestEvt) > sizeof(FckWorkDoneEvt)) ? sizeof(FckRequestEvt)      \
                                                        : sizeof(FckWorkDoneEvt))    \
      + 7U)                                                                          \
     & ~(size_t)7U)
#define T_BLOCKS 32U

static QSubscrList       s_subscr[MAX_PUB_SIG];
static uint64_t          s_pool[(T_BLOCK * T_BLOCKS) / sizeof(uint64_t)];
static uint8_t           s_queue_sto[AO_QUEUE_LENGTH * sizeof(QEvt *)];
static QEvt const        s_home  = QEVT_INITIALIZER(HOME_SIG);
/* The system's signals, as static events (pool number 0): what kode-sdk publishes. */
static LowPowerRoundEvt const s_round_asleep = { .super = QEVT_INITIALIZER(LOWPOWER_ROUND_SIG),
                                                 .target = (uint8_t)KODE_LP_ASLEEP, .round = 1U };
static LowPowerRoundEvt const s_round_off    = { .super = QEVT_INITIALIZER(LOWPOWER_ROUND_SIG),
                                                 .target = (uint8_t)KODE_LP_OFF, .round = 2U };
static LowPowerRoundEvt const s_round_awake  = { .super = QEVT_INITIALIZER(LOWPOWER_ROUND_SIG),
                                                 .target = (uint8_t)KODE_LP_AWAKE, .round = 3U };
static LowPowerStateEvt const s_state_awake  = { .super = QEVT_INITIALIZER(LOWPOWER_STATE_SIG),
                                                 .state = (uint8_t)KODE_LP_AWAKE, .cause = 0U, .prev = (uint8_t)KODE_LP_ASLEEP };
static LowPowerStateEvt const s_state_asleep = { .super = QEVT_INITIALIZER(LOWPOWER_STATE_SIG),
                                                 .state = (uint8_t)KODE_LP_ASLEEP, .cause = 0U, .prev = (uint8_t)KODE_LP_AWAKE };
static LowPowerStateEvt const s_state_radio  = { .super = QEVT_INITIALIZER(LOWPOWER_STATE_SIG),
                                                 .state = (uint8_t)KODE_LP_DARK, .cause = 0U, .prev = (uint8_t)KODE_LP_ASLEEP };
static ExpPowerDoneEvt const s_power_on_ok   = { .super = QEVT_INITIALIZER(EXP_POWER_SIG), .on = true, .rc = (uint8_t)KODE_OK };
static ExpPowerDoneEvt const s_power_no_link = { .super = QEVT_INITIALIZER(EXP_POWER_SIG), .on = true, .rc = (uint8_t)KODE_DENIED };
static ExpPowerDoneEvt const s_power_off     = { .super = QEVT_INITIALIZER(EXP_POWER_SIG), .on = false, .rc = (uint8_t)KODE_OK };
static fck_aos_request_t s_request = { .tag = 7 };

/**
 * @brief QF, the pool, the queue and the owner, once per binary: QP/C has
 *        no teardown, and a priority is registered once.
 */
static void ao_up(void)
{
    static bool up;
    if (up) {
        return;
    }
    up = true;
    QF_init();
    QActive_psInit(s_subscr, Q_DIM(s_subscr));
    QF_poolInit(s_pool, sizeof(s_pool), T_BLOCK);

    TEST_ASSERT_EQUAL(KODE_OK, Fck_ctor());
    /* What QACTIVE_START() does besides the task: the level, the registry
     * (QActive_subscribe() checks it), and the queue where the port reads
     * its room. */
    l_ao.super.prio  = AO_PRIORITY;
    l_ao.super.pthre = 0U;
    QActive_register_(&l_ao.super);
    l_ao.super.eQueue = xQueueCreateStatic(AO_QUEUE_LENGTH, sizeof(QEvt *), s_queue_sto,
                                           &l_ao.super.osObject.queue);
    QASM_INIT(&l_ao.super, (void *)0, 0U);
    s_ao_started = true;
}

/** @brief Dispatch what waits in the owner's queue, its blocks back to the pool. */
static unsigned settle(void)
{
    unsigned    n = 0U;
    QEvt const *e = NULL;
    while (xQueueReceive(l_ao.super.eQueue, &e, 0U) == pdTRUE) {
        QASM_DISPATCH(&l_ao.super, e, 0U);
        QF_gc(e);
        n++;
    }
    return n;
}

/**
 * @brief Dispatch until @p dones answers of the worker have reached the owner,
 *        waiting up to a second for each.
 * @return Whether they all came.
 */
static bool settle_answers(unsigned dones)
{
    while (g_on.dones < dones) {
        QEvt const *e = NULL;
        if (xQueueReceive(l_ao.super.eQueue, &e, pdMS_TO_TICKS(1000U)) != pdTRUE) {
            return false;
        }
        QASM_DISPATCH(&l_ao.super, e, 0U);
        QF_gc(e);
    }
    return true;
}

/** @brief @p steps of 10 ms of QF's ticks, each followed by what it posted. */
static void steps(unsigned steps)
{
    for (unsigned s = 0U; s < steps; s++) {
        for (uint32_t t = 0U; t < KQP_TICKS_PER_MS(10U); t++) {
            QTIMEEVT_TICK_X(0U, (void *)0);
        }
        (void)settle();
    }
}

/** @brief Requests posted until one is refused. @return How many went. */
static unsigned post_until_refused(kode_status_t *refused)
{
    unsigned n = 0U;
    for (;;) {
        kode_status_t const rc = fck_ao_post(&s_request);
        if (rc != KODE_OK) {
            *refused = rc;
            return n;
        }
        n++;
        TEST_ASSERT_TRUE(n <= T_BLOCKS);   /* never unbounded */
    }
}

void setUp(void)
{
    ao_up();
    (void)settle();
    (void)memset(&g_on, 0, sizeof(g_on));
}

void tearDown(void)
{
}

/* ---- The cases ------------------------------------------------------------------ */

TEST_CASE("built: a worker of its own on core 1 at the low level, one job waiting at most", "[ao]")
{
    TEST_ASSERT_EQUAL_UINT(1U, g_worker.created);
    TEST_ASSERT_EQUAL_STRING("fck_work", g_worker.name);
    TEST_ASSERT_EQUAL_UINT32(WORKER_STACK_BYTES, g_worker.stack_bytes);
    TEST_ASSERT_EQUAL_UINT((unsigned)KODE_PRIO_USER_LOW, (unsigned)g_worker.prio);
    TEST_ASSERT_EQUAL_INT(1, (int)g_worker.core);   /* on the board, fck_drv_tca6408a.h */
    TEST_ASSERT_NOT_NULL(l_worker);
    TEST_ASSERT_NOT_NULL(l_jobs);
    TEST_ASSERT_EQUAL_UINT(WORKER_DEPTH,
                           (unsigned)(uxQueueSpacesAvailable(l_jobs) + uxQueueMessagesWaiting(l_jobs)));
    TEST_ASSERT_EQUAL_UINT8(KODE_PRIO_USER_HIGH, AO_PRIORITY);   /* no CONFIG_FCK_AO_PRIORITY here */
}

TEST_CASE("the tick reaches the owner every 10 ms", "[ao]")
{
    steps(1U);
    TEST_ASSERT_EQUAL_UINT(1U, g_on.ticks);
    steps(9U);
    TEST_ASSERT_EQUAL_UINT(10U, g_on.ticks);
    TEST_ASSERT_EQUAL_UINT(0U, g_on.requests + g_on.rests + g_on.wakes + g_on.powers + g_on.homes + g_on.dones);
}

/* Fails with a periodic tick. kode-sdk's QF tick is an esp_timer, and one
 * held back fires every missed tick back to back: a periodic time event then
 * posts one per 10 ms into a queue no one drains meanwhile, and the 17th is
 * qf_port:130.
 * One shot, armed again as it is served, is one in the queue at most,
 * however long the hold. */
TEST_CASE("the tick held back 300 ms leaves one tick in the queue, not thirty", "[ao]")
{
    for (uint32_t t = 0U; t < KQP_TICKS_PER_MS(300U); t++) {
        QTIMEEVT_TICK_X(0U, (void *)0);
    }
    TEST_ASSERT_EQUAL_UINT(AO_QUEUE_LENGTH - 1U, uxQueueSpacesAvailable(l_ao.super.eQueue));
    TEST_ASSERT_EQUAL_UINT(1U, settle());
    TEST_ASSERT_EQUAL_UINT(1U, g_on.ticks);
    steps(3U);
    TEST_ASSERT_EQUAL_UINT(4U, g_on.ticks);   /* and every 10 ms again */
}

TEST_CASE("a request reaches the owner as its pointer, and its block goes back to the pool", "[ao]")
{
    TEST_ASSERT_EQUAL(KODE_OK, fck_ao_post(&s_request));
    TEST_ASSERT_EQUAL_UINT(1U, settle());
    TEST_ASSERT_EQUAL_UINT(1U, g_on.requests);
    TEST_ASSERT_EQUAL_PTR(&s_request, g_on.last);

    kode_status_t rc    = KODE_OK;
    unsigned const went = post_until_refused(&rc);
    (void)settle();
    TEST_ASSERT_EQUAL_UINT(went, post_until_refused(&rc));   /* as many again: none kept */
    (void)settle();
}

TEST_CASE("a queue nearly full refuses a request; the tick, a rest, the supply's answer and home still find room", "[ao]")
{
    kode_status_t rc    = KODE_OK;
    unsigned const went = post_until_refused(&rc);
    TEST_ASSERT_EQUAL(KODE_NO_RESOURCES, rc);
    TEST_ASSERT_EQUAL_UINT(AO_QUEUE_LENGTH - AO_QUEUE_MARGIN, went);

    for (uint32_t t = 0U; t < KQP_TICKS_PER_MS(10U); t++) {
        QTIMEEVT_TICK_X(0U, (void *)0);   /* no margin: a full queue would stop the run */
    }
    QACTIVE_PUBLISH(&s_round_asleep.super, (void *)0);
    QACTIVE_PUBLISH(&s_power_on_ok.super, (void *)0);
    QACTIVE_PUBLISH(&s_home, (void *)0);
    (void)settle();
    TEST_ASSERT_EQUAL_UINT(went, g_on.requests);
    TEST_ASSERT_EQUAL_UINT(1U, g_on.ticks);
    TEST_ASSERT_EQUAL_UINT(1U, g_on.rests);
    TEST_ASSERT_EQUAL_UINT(1U, g_on.powers);
    TEST_ASSERT_EQUAL_UINT(1U, g_on.homes);
}

TEST_CASE("a pool running low refuses a request before QP/C would stop the board", "[ao]")
{
    QEvt    *held[T_BLOCKS];
    unsigned n = 0U;
    /* Leave the pool one block past the owner's margin. */
    while (n < (T_BLOCKS - AO_POOL_MARGIN - 1U)) {
        held[n] = Q_NEW_X(QEvt, 0U, FCK_TICK_SIG);
        TEST_ASSERT_NOT_NULL(held[n]);
        n++;
    }
    TEST_ASSERT_EQUAL(KODE_OK, fck_ao_post(&s_request));           /* the last above the margin */
    TEST_ASSERT_EQUAL(KODE_NO_RESOURCES, fck_ao_post(&s_request)); /* the margin is the system's */
    TEST_ASSERT_TRUE(uxQueueSpacesAvailable(l_ao.super.eQueue) > AO_QUEUE_MARGIN); /* not the queue */
    for (unsigned i = 0U; i < n; i++) {
        QF_gc(held[i]);
    }
    TEST_ASSERT_EQUAL(KODE_OK, fck_ao_post(&s_request));
    (void)settle();
    TEST_ASSERT_EQUAL_UINT(2U, g_on.requests);
}

TEST_CASE("a round for ASLEEP, published, reaches the owner as a rest; one for OFF as a rest that is for OFF", "[ao]")
{
    QACTIVE_PUBLISH(&s_round_asleep.super, (void *)0);
    TEST_ASSERT_EQUAL_UINT(1U, settle());
    TEST_ASSERT_EQUAL_UINT(1U, g_on.rests);
    TEST_ASSERT_FALSE(g_on.rest_off);
    QACTIVE_PUBLISH(&s_round_off.super, (void *)0);
    TEST_ASSERT_EQUAL_UINT(1U, settle());
    TEST_ASSERT_EQUAL_UINT(2U, g_on.rests);
    TEST_ASSERT_TRUE(g_on.rest_off);
}

TEST_CASE("a round for AWAKE is not a rest", "[ao]")
{
    QACTIVE_PUBLISH(&s_round_awake.super, (void *)0);
    TEST_ASSERT_EQUAL_UINT(1U, settle());
    TEST_ASSERT_EQUAL_UINT(0U, g_on.rests);
    TEST_ASSERT_EQUAL_UINT(0U, g_on.wakes);
}

TEST_CASE("the board AWAKE again reaches the owner as a wake; ASLEEP and RADIO are not one", "[ao]")
{
    QACTIVE_PUBLISH(&s_state_asleep.super, (void *)0);
    QACTIVE_PUBLISH(&s_state_radio.super, (void *)0);
    TEST_ASSERT_EQUAL_UINT(2U, settle());
    TEST_ASSERT_EQUAL_UINT(0U, g_on.wakes);
    QACTIVE_PUBLISH(&s_state_awake.super, (void *)0);
    TEST_ASSERT_EQUAL_UINT(1U, settle());
    TEST_ASSERT_EQUAL_UINT(1U, g_on.wakes);
    QACTIVE_PUBLISH(&s_state_awake.super, (void *)0);   /* it repeats: the restore is idempotent */
    TEST_ASSERT_EQUAL_UINT(1U, settle());
    TEST_ASSERT_EQUAL_UINT(2U, g_on.wakes);
    TEST_ASSERT_EQUAL_UINT(0U, g_on.rests);
}

/* Fails if the owner stops subscribing to EXP_POWER_SIG: the supply's answer would never arrive
 * and the status would say "asked" for ever. */
TEST_CASE("EXP_POWER_SIG, published, reaches the owner with the switch and the rc it carries", "[ao]")
{
    QACTIVE_PUBLISH(&s_power_on_ok.super, (void *)0);
    TEST_ASSERT_EQUAL_UINT(1U, settle());
    TEST_ASSERT_EQUAL_UINT(1U, g_on.powers);
    TEST_ASSERT_TRUE(g_on.power_on);
    TEST_ASSERT_EQUAL_UINT8(KODE_OK, g_on.power_rc);
    QACTIVE_PUBLISH(&s_power_no_link.super, (void *)0);
    TEST_ASSERT_EQUAL_UINT(1U, settle());
    TEST_ASSERT_TRUE(g_on.power_on);
    TEST_ASSERT_EQUAL_UINT8(KODE_DENIED, g_on.power_rc);
    QACTIVE_PUBLISH(&s_power_off.super, (void *)0);
    TEST_ASSERT_EQUAL_UINT(1U, settle());
    TEST_ASSERT_FALSE(g_on.power_on);
    TEST_ASSERT_EQUAL_UINT(3U, g_on.powers);
}

/* Fails if the owner stops subscribing to HOME_SIG: a held bottom button
 * would reset the board with the module's radios still up. */
TEST_CASE("HOME_SIG, published, reaches the owner", "[ao]")
{
    QACTIVE_PUBLISH(&s_home, (void *)0);
    TEST_ASSERT_EQUAL_UINT(1U, settle());
    TEST_ASSERT_EQUAL_UINT(1U, g_on.homes);
    TEST_ASSERT_EQUAL_UINT(0U, g_on.rests);
}

/* Where a job ran, as it saw it. */
static struct {
    bool on_worker;
    bool on_owner;
} g_job;

/** @brief A job as the owner gives one: what it read, what it answers. */
static kode_status_t job(void *arg, uint32_t *value)
{
    uint32_t const * const in = (uint32_t const *)arg;
    g_job.on_worker = fck_ao_is_worker_task();
    g_job.on_owner  = fck_ao_is_own_task();
    *value = *in * 2U;
    return KODE_IO_ERROR;
}

TEST_CASE("a job runs on the worker, and its answer reaches the owner with its number", "[ao]")
{
    static uint32_t in    = 21U;
    uint16_t        seq_a = 0U;
    uint16_t        seq_b = 0U;
    (void)memset(&g_job, 0, sizeof(g_job));
    TEST_ASSERT_EQUAL(KODE_OK, fck_ao_submit_work(job, &in, &seq_a));
    TEST_ASSERT_TRUE(settle_answers(1U));
    TEST_ASSERT_EQUAL_UINT16(seq_a, g_on.done_seq[0]);
    TEST_ASSERT_EQUAL(KODE_IO_ERROR, g_on.done_status);
    TEST_ASSERT_EQUAL_UINT32(42U, g_on.done_value);
    TEST_ASSERT_TRUE(g_job.on_worker);
    TEST_ASSERT_FALSE(g_job.on_owner);

    TEST_ASSERT_EQUAL(KODE_OK, fck_ao_submit_work(job, &in, &seq_b));
    TEST_ASSERT_TRUE(seq_b != seq_a);   /* each job its own number */
    TEST_ASSERT_TRUE(settle_answers(2U));
    TEST_ASSERT_EQUAL_UINT16(seq_b, g_on.done_seq[1]);
}

static SemaphoreHandle_t s_started;
static SemaphoreHandle_t s_release;

/** @brief A job that holds the worker until the case lets it go. */
static kode_status_t held_job(void *arg, uint32_t *value)
{
    (void)arg;
    (void)xSemaphoreGive(s_started);
    (void)xSemaphoreTake(s_release, portMAX_DELAY);
    *value = 1U;
    return KODE_OK;
}

TEST_CASE("one job runs and one waits; a third is refused, and both answers come in turn", "[ao]")
{
    static uint32_t in    = 1U;
    uint16_t        seq_a = 0U;
    uint16_t        seq_b = 0U;
    uint16_t        seq_c = 0U;
    s_started = xSemaphoreCreateBinary();
    s_release = xSemaphoreCreateBinary();
    TEST_ASSERT_NOT_NULL(s_started);
    TEST_ASSERT_NOT_NULL(s_release);

    TEST_ASSERT_EQUAL(KODE_OK, fck_ao_submit_work(held_job, NULL, &seq_a));
    TEST_ASSERT_EQUAL(pdTRUE, xSemaphoreTake(s_started, pdMS_TO_TICKS(1000U)));   /* A runs */
    TEST_ASSERT_EQUAL(KODE_OK, fck_ao_submit_work(job, &in, &seq_b));             /* B waits */
    TEST_ASSERT_EQUAL(KODE_NO_RESOURCES, fck_ao_submit_work(job, &in, &seq_c));   /* no room */
    TEST_ASSERT_EQUAL_UINT16(0U, seq_c);   /* a refused job is given no number */
    TEST_ASSERT_EQUAL_UINT(0U, settle());

    (void)xSemaphoreGive(s_release);
    TEST_ASSERT_TRUE(settle_answers(2U));
    TEST_ASSERT_EQUAL_UINT16(seq_a, g_on.done_seq[0]);
    TEST_ASSERT_EQUAL_UINT16(seq_b, g_on.done_seq[1]);
    vSemaphoreDelete(s_started);
    vSemaphoreDelete(s_release);
}

TEST_CASE("the case's task is neither the owner's nor its worker's; before the start nothing is posted", "[ao]")
{
    TEST_ASSERT_FALSE(fck_ao_is_own_task());
    TEST_ASSERT_FALSE(fck_ao_is_worker_task());
    s_ao_started = false;
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_ao_post(&s_request));
    s_ao_started = true;
    TEST_ASSERT_EQUAL_UINT(0U, settle());
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_ao_post(NULL));
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_ao_submit_work(NULL, NULL, NULL));
}

/* A task that is some other Active Object's: QP/C's table says which tasks
 * the image's Active Objects run on. */
static QActive s_app_ao;

TEST_CASE("any Active Object's task is told from a plain task, the owner's included, and named", "[ao]")
{
    ao_up();
    const char *name = "untouched";
    /* The case's task is registered by nobody: the name is left alone. */
    TEST_ASSERT_FALSE(fck_ao_is_any_ao_task(&name));
    TEST_ASSERT_EQUAL_STRING("untouched", name);
    TEST_ASSERT_FALSE(fck_ao_is_any_ao_task(NULL));
    /* An app's Active Object, at another level, on this task. */
    s_app_ao.prio   = (uint8_t)(AO_PRIORITY + 1U);
    s_app_ao.thread = xTaskGetCurrentTaskHandle();
    QActive_registry_[s_app_ao.prio] = &s_app_ao;
    TEST_ASSERT_TRUE(fck_ao_is_any_ao_task(&name));
    TEST_ASSERT_NOT_NULL(name);
    TEST_ASSERT_TRUE(fck_ao_is_any_ao_task(NULL));
    TEST_ASSERT_FALSE(fck_ao_is_own_task());      /* not the owner's: the app's */
    QActive_registry_[s_app_ao.prio] = (QActive *)0;
    TEST_ASSERT_FALSE(fck_ao_is_any_ao_task(NULL));
    /* The owner's own task is one: the registered Active Object runs on the worker's. */
    TaskHandle_t const was = l_ao.super.thread;
    l_ao.super.thread      = xTaskGetCurrentTaskHandle();
    TEST_ASSERT_TRUE(fck_ao_is_any_ao_task(NULL));
    l_ao.super.thread = was;
    TEST_ASSERT_FALSE(fck_ao_is_any_ao_task(NULL));
}

/* Whether kodeOS is up is whether AO_Boot holds its level: before kode_init() nothing does. */
TEST_CASE("kode_init() is known to have run by AO_Boot holding its level", "[ao]")
{
    ao_up();
    TEST_ASSERT_FALSE(fck_ao_kode_is_up());
    static QActive boot;
    boot.prio   = (uint8_t)KODE_PRIO_BOOT;
    boot.thread = xTaskGetCurrentTaskHandle();
    QActive_registry_[boot.prio] = &boot;
    TEST_ASSERT_TRUE(fck_ao_kode_is_up());
    QActive_registry_[boot.prio] = (QActive *)0;
    TEST_ASSERT_FALSE(fck_ao_kode_is_up());
}

TEST_CASE("a level another Active Object holds is refused with KODE_EXISTS, before anything is made", "[ao]")
{
    ao_up();
    unsigned const made = g_worker.created;
    s_ao_started = false;       /* as before the start: the level is the owner's own, taken */
    unsigned const warnings = g_log_warnings;
    TEST_ASSERT_EQUAL(KODE_EXISTS, fck_ao_start());
    TEST_ASSERT_EQUAL_UINT(made, g_worker.created);
    s_ao_started = true;
    TEST_ASSERT_TRUE(g_log_warnings >= warnings);
    /* And started, it answers OK without looking again. */
    TEST_ASSERT_EQUAL(KODE_OK, fck_ao_start());
}

/**
 * @brief What QP/C calls instead of carrying on: the run stops, naming it.
 * @param module The QP/C file that raised it.
 * @param id     Its assertion id.
 */
Q_NORETURN Q_onError(char const * const module, int_t const id)
{
    (void)printf("Q_onError %s:%d\n", module, (int)id);
    abort();
}
