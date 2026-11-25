/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file fake_fck_ao.c
 * @brief AO_Fck as the suite plays it: fck_ao.h's calls, on two FreeRTOS
 *        tasks instead of QP/C.
 *
 * The signals the real AO subscribes to - HOME_SIG, EXP_POWER_SIG, LOWPOWER_ROUND_SIG and
 * LOWPOWER_STATE_SIG - are played by the sim_ao_*() calls below, in order with the rest.
 *
 * fck_aos.c is the code under test, and it sees only fck_ao.h. Here one task
 * takes the owner's events in order - a request, the tick every
 * FCK_AO_TICK_MS, a job's end - and calls fck_aos_on_*() for each, one at a
 * time, as QP/C runs an Active Object's steps; another runs the jobs, one at
 * a time, and posts their ends back. What QP/C itself does (pools, margins,
 * priorities) is kode-sdk's and is not under test here.
 */
#include <stdbool.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "fck_ao.h"

#define EVENTS_DEPTH  16
#define AO_STACK      8192
#define WORKER_STACK  8192
#define AO_PRIO       4
#define WORKER_PRIO   3

typedef enum {
    EVENT_REQUEST,
    EVENT_WORK_DONE,
    EVENT_HOME,       /* HOME_SIG: the bottom button was held */
    EVENT_REST,       /* LOWPOWER_ROUND_SIG, target ASLEEP or OFF */
    EVENT_WAKE,       /* LOWPOWER_STATE_SIG, AWAKE */
    EVENT_EXP_POWER,  /* EXP_POWER_SIG */
} event_kind_t;

typedef struct {
    event_kind_t       kind;
    fck_aos_request_t *request;
    uint16_t           seq;
    kode_status_t      status;
    uint32_t           value;
    bool               flag;
    uint8_t            rc;
} event_t;

typedef struct {
    fck_ao_job_t fn;
    void        *arg;
    uint16_t     seq;
} job_t;

static QueueHandle_t s_events;
static QueueHandle_t s_jobs;
static TaskHandle_t  s_ao;
static TaskHandle_t  s_worker;
static uint16_t      s_seq;
/* The task of some other Active Object (an app's), as a case marks it. */
static TaskHandle_t  s_app_ao;
/* The owner's task does not take events while this is set: a stuck owner. */
static volatile bool s_stall;
/* The owner's queue takes no request while this is set: a full queue. */
static volatile bool s_full;

static void ao_task(void *unused)
{
    (void)unused;
    TickType_t const tick = pdMS_TO_TICKS(FCK_AO_TICK_MS);
    TickType_t       next = xTaskGetTickCount() + tick;
    for (;;) {
        if (s_stall) {
            vTaskDelay(1);
            continue;
        }
        TickType_t const now  = xTaskGetTickCount();
        TickType_t const wait = ((int32_t)(next - now) > 0) ? (next - now) : 0U;
        event_t          e;
        if (xQueueReceive(s_events, &e, wait) == pdTRUE) {
            while (s_stall) {
                vTaskDelay(1);     /* stuck in the step before this one */
            }
            switch (e.kind) {
            case EVENT_REQUEST:
                fck_aos_on_request(e.request);
                break;
            case EVENT_HOME:
                fck_aos_on_home();
                break;
            case EVENT_REST:
                fck_aos_on_rest(e.flag);
                break;
            case EVENT_WAKE:
                fck_aos_on_wake();
                break;
            case EVENT_EXP_POWER:
                fck_aos_on_exp_power(e.flag, e.rc);
                break;
            default:
                fck_aos_on_work_done(e.seq, e.status, e.value);
                break;
            }
        }
        if ((int32_t)(xTaskGetTickCount() - next) >= 0) {
            next += tick;
            fck_aos_on_tick();
        }
    }
}

static void worker_task(void *unused)
{
    (void)unused;
    for (;;) {
        job_t job;
        if (xQueueReceive(s_jobs, &job, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        event_t e = { .kind = EVENT_WORK_DONE, .seq = job.seq };
        e.status  = job.fn(job.arg, &e.value);
        (void)xQueueSend(s_events, &e, portMAX_DELAY);
    }
}

kode_status_t fck_ao_start(void)
{
    if (s_ao != NULL) {
        return KODE_OK;
    }
    s_events = xQueueCreate(EVENTS_DEPTH, sizeof(event_t));
    s_jobs   = xQueueCreate(1, sizeof(job_t));
    if (s_events == NULL || s_jobs == NULL
        || xTaskCreate(worker_task, "fck_work", WORKER_STACK, NULL, WORKER_PRIO, &s_worker)
               != pdPASS
        || xTaskCreate(ao_task, "fck", AO_STACK, NULL, AO_PRIO, &s_ao) != pdPASS) {
        return KODE_NO_RESOURCES;
    }
    return KODE_OK;
}

kode_status_t fck_ao_post(fck_aos_request_t *request)
{
    if (s_ao == NULL || request == NULL) {
        return KODE_DENIED;
    }
    if (s_full) {
        return KODE_NO_RESOURCES;
    }
    event_t const e = { .kind = EVENT_REQUEST, .request = request };
    return (xQueueSend(s_events, &e, portMAX_DELAY) == pdTRUE) ? KODE_OK : KODE_NO_RESOURCES;
}

bool fck_ao_is_own_task(void)
{
    return s_ao != NULL && xTaskGetCurrentTaskHandle() == s_ao;
}

bool fck_ao_is_any_ao_task(const char **name)
{
    TaskHandle_t const me = xTaskGetCurrentTaskHandle();
    if (s_ao != NULL && me == s_ao) {
        if (name != NULL) {
            *name = "AO_Fck";
        }
        return true;
    }
    if (s_app_ao != NULL && me == s_app_ao) {
        if (name != NULL) {
            *name = "AO_App";
        }
        return true;
    }
    return false;
}

static volatile bool s_kode_up = true;

void sim_ao_kode_up(bool up)
{
    s_kode_up = up;
}

bool fck_ao_kode_is_up(void)
{
    return s_kode_up;
}

bool fck_ao_is_worker_task(void)
{
    return s_worker != NULL && xTaskGetCurrentTaskHandle() == s_worker;
}

void fck_ao_lift_to_caller(void)
{
    if (s_ao != NULL && uxTaskPriorityGet(NULL) > uxTaskPriorityGet(s_ao)) {
        vTaskPrioritySet(s_ao, uxTaskPriorityGet(NULL));
    }
}

kode_status_t fck_ao_submit_work(fck_ao_job_t fn, void *arg, uint16_t *seq)
{
    if (fn == NULL || s_jobs == NULL) {
        return KODE_DENIED;
    }
    s_seq++;
    job_t const job = { .fn = fn, .arg = arg, .seq = s_seq };
    if (xQueueSend(s_jobs, &job, 0) != pdTRUE) {
        return KODE_NO_RESOURCES;
    }
    if (seq != NULL) {
        *seq = job.seq;
    }
    return KODE_OK;
}

/* ---- The system's two signals, as the owner's task takes them ------------------------- */

void sim_ao_home(void)
{
    event_t const e = { .kind = EVENT_HOME };
    (void)xQueueSend(s_events, &e, portMAX_DELAY);
}

void sim_ao_rest(bool off)
{
    event_t const e = { .kind = EVENT_REST, .flag = off };
    (void)xQueueSend(s_events, &e, portMAX_DELAY);
}

void sim_ao_sleep(void)
{
    sim_ao_rest(false);
}

void sim_ao_wake(void)
{
    event_t const e = { .kind = EVENT_WAKE };
    (void)xQueueSend(s_events, &e, portMAX_DELAY);
}

void sim_ao_exp_power(bool on, uint8_t rc)
{
    event_t const e = { .kind = EVENT_EXP_POWER, .flag = on, .rc = rc };
    if (s_events != NULL) {
        (void)xQueueSend(s_events, &e, portMAX_DELAY);
    }
}

/* ---- What a case plays: an app's Active Object, a stuck owner ------------------------- */

void sim_ao_mark_task(TaskHandle_t task)
{
    s_app_ao = task;
}

void sim_ao_stall(bool stalled)
{
    s_stall = stalled;
}

void sim_ao_full(bool full)
{
    s_full = full;
}
