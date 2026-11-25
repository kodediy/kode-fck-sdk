/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file fck_events.c
 * @brief The standard layer's events: listeners and fck_event_wait(), on a task of the SDK's own.
 *
 * The owner raises an event on its own task (or on the worker's, for a send that was cut) and
 * hands it to fck_events_sink() without blocking: a queue of FCK_EVENT_QUEUE. A full queue
 * loses the event, which the owner counts (fck_status().events_dropped). The task `fck_events`
 * takes them one at a time, in order, wakes the tasks waiting in fck_event_wait() and calls the
 * listeners. A listener is never called on the owner's task, and the owner never waits for one.
 *
 * Nothing is queued while nobody listens and nobody waits for that event.
 */
#include "fck.h"

#include <stdatomic.h>
#include <stddef.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "kode_log.h"

#include "fck_aos.h"
#include "fck_events.h"
#include "fck_private.h"

#define TAG "fck_events"

/** The task's stack and priority. The listeners run the app's code, which may take as long as it
 * likes, so the task sits where it can only ever delay itself: 1, the level kode-sdk gives
 * its own standard layer's listener task (KODE_PRIO_STANDARD_EVENTS in kqp_config.h, which this
 * component, with no QP/C in it, does not include), below every owner AO. */
#define EVENT_STACK_BYTES   3584U
#define EVENT_PRIORITY      (tskIDLE_PRIORITY + 1U)
/** The queue is read in steps this long, so that the task never waits without a bound. */
#define EVENT_POLL_MS       1000U
/** A listener longer than this is logged, one in LOG_EVERY. */
#define LISTENER_SLOW_MS    10U
#define LOG_EVERY           100U
/** The most tasks that may wait for events at once. */
#define WAITERS_MAX         4U
/** How long fck_event_remove_listener() waits for the listener that is running. */
#define REMOVE_WAIT_MS      2000U
#define REMOVE_POLL_MS      5U
/** fck_event_wait() looks for fck_cancel() this often. */
#define CANCEL_STEP_MS      20U

/* The names, and what the owner calls them. */
static const struct {
    const char     *name;
    fck_aos_event_t ev;
} NAMES[] = {
    { "module_found",     FCK_AOS_EVENT_PRESENT },
    { "module_lost",      FCK_AOS_EVENT_GONE },
    { "wrong_module",     FCK_AOS_EVENT_WRONG_MODULE },
    { "setting_lost",     FCK_AOS_EVENT_SETTING_LOST },
    { "setting_restored", FCK_AOS_EVENT_SETTING_RESTORED },
    { "packet_received",  FCK_AOS_EVENT_PACKET_RECEIVED },
    { "send_cancelled",   FCK_AOS_EVENT_SEND_CANCELLED },
};
#define NAME_COUNT (sizeof(NAMES) / sizeof(NAMES[0]))

_Static_assert(NAME_COUNT <= 32U, "the interest of the names is a 32-bit mask");

typedef struct {
    uint8_t  name;      /* an index into NAMES */
    uint32_t arg;
    uint32_t seq;       /* the order events were handed over in */
} item_t;

typedef struct {
    bool           used;
    uint8_t        name;
    fck_listener_t fn;
} listener_t;

typedef struct {
    bool              used;
    bool              got;
    uint8_t           name;
    uint32_t          after;    /* only an event with a count above this counts */
    fck_event_t       event;
} waiter_t;

static portMUX_TYPE   s_mux = portMUX_INITIALIZER_UNLOCKED;
static QueueHandle_t  s_queue;
static TaskHandle_t   s_task;
static bool           s_started;
static atomic_uint    s_seq;
static uint32_t       s_interest;                  /* bit n: someone listens or waits for NAMES[n] */
static listener_t     s_listener[FCK_LISTENERS_MAX];
static waiter_t       s_waiter[WAITERS_MAX];
/* One semaphore for each slot, made once and never deleted: the events task may give one a moment
 * after its waiter gave up, and a deleted one would be a use after free. A stale give only wakes the
 * next waiter of the slot early, which looks at its flag and goes on waiting. */
static StaticSemaphore_t s_sem_buf[WAITERS_MAX];
static SemaphoreHandle_t s_sem[WAITERS_MAX];
static fck_listener_t s_running;                   /* the listener being called now */
static uint32_t       s_slow;

static int name_index(const char *event)
{
    if (event == NULL || strnlen(event, FCK_EVENT_NAME_MAX) >= FCK_EVENT_NAME_MAX) {
        return -1;
    }
    for (size_t i = 0U; i < NAME_COUNT; i++) {
        if (strcmp(event, NAMES[i].name) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static int index_of(fck_aos_event_t ev)
{
    for (size_t i = 0U; i < NAME_COUNT; i++) {
        if (NAMES[i].ev == ev) {
            return (int)i;
        }
    }
    return -1;
}

/* Under s_mux. */
static void interest_update(void)
{
    uint32_t mask = 0U;
    for (size_t i = 0U; i < FCK_LISTENERS_MAX; i++) {
        if (s_listener[i].used) {
            mask |= 1UL << s_listener[i].name;
        }
    }
    for (size_t i = 0U; i < WAITERS_MAX; i++) {
        if (s_waiter[i].used) {
            mask |= 1UL << s_waiter[i].name;
        }
    }
    s_interest = mask;
}

/* What an event's value and text are: the table in fck.h. */
static fck_event_t event_of(const item_t *it)
{
    fck_event_t e = { .name = NAMES[it->name].name, .value = (int32_t)it->arg, .text = "" };
    switch (NAMES[it->name].ev) {
    case FCK_AOS_EVENT_PRESENT:
        e.value = 1;
        break;
    case FCK_AOS_EVENT_GONE:
        e.value = 0;
        break;
    case FCK_AOS_EVENT_SEND_CANCELLED:
        e.text = (it->arg == FCK_AOS_CANCEL_CALL) ? "cancel" : "leaving";
        e.value = (it->arg == FCK_AOS_CANCEL_CALL) ? FCK_CANCEL_CALL : FCK_CANCEL_LEAVING;
        break;
    default:
        break;
    }
    return e;
}

/* ---- The owner's side: no waiting ------------------------------------------------------- */

static bool events_sink(fck_aos_event_t ev, uint32_t arg, void *ctx)
{
    (void)ctx;
    int const idx = index_of(ev);
    if (idx < 0) {
        return true;
    }
    taskENTER_CRITICAL(&s_mux);
    bool const interested = ((s_interest >> (unsigned)idx) & 1U) != 0U;
    taskEXIT_CRITICAL(&s_mux);
    if (!interested || s_queue == NULL) {
        return true;        /* nobody to tell: not a loss */
    }
    item_t const it = { .name = (uint8_t)idx, .arg = arg,
                        .seq = atomic_fetch_add(&s_seq, 1U) + 1U };
    return xQueueSend(s_queue, &it, 0) == pdTRUE;
}

/* ---- The listeners' task ------------------------------------------------------------------ */

static void deliver(const item_t *it)
{
    fck_event_t const event = event_of(it);

    /* The tasks waiting for it. Their semaphores are given outside the critical section. */
    SemaphoreHandle_t wake[WAITERS_MAX];
    size_t            n_wake = 0U;
    taskENTER_CRITICAL(&s_mux);
    for (size_t i = 0U; i < WAITERS_MAX; i++) {
        waiter_t *const w = &s_waiter[i];
        if (w->used && !w->got && (w->name == it->name) && (it->seq > w->after)) {
            w->event = event;
            w->got   = true;
            wake[n_wake++] = s_sem[i];
        }
    }
    taskEXIT_CRITICAL(&s_mux);
    for (size_t i = 0U; i < n_wake; i++) {
        (void)xSemaphoreGive(wake[i]);
    }

    /* The listeners, one at a time, each looked up again so that one removed meanwhile is not called. */
    for (size_t i = 0U; i < FCK_LISTENERS_MAX; i++) {
        fck_listener_t fn = NULL;
        taskENTER_CRITICAL(&s_mux);
        if (s_listener[i].used && (s_listener[i].name == it->name)) {
            fn        = s_listener[i].fn;
            s_running = fn;
        }
        taskEXIT_CRITICAL(&s_mux);
        if (fn == NULL) {
            continue;
        }
        TickType_t const t0 = xTaskGetTickCount();
        fn(&event);
        uint32_t const ms = (uint32_t)((xTaskGetTickCount() - t0) * portTICK_PERIOD_MS);
        taskENTER_CRITICAL(&s_mux);
        s_running = NULL;
        taskEXIT_CRITICAL(&s_mux);
        if ((ms > LISTENER_SLOW_MS) && ((s_slow++ % LOG_EVERY) == 0U)) {
            KLOG_W(TAG, "a listener of %s took %u ms: it holds up the ones after it", event.name, (unsigned)ms);
        }
    }
}

/* The queue is the task's argument: it may run before fck_events_start() has stored it. */
static void events_task(void *arg)
{
    QueueHandle_t const queue = (QueueHandle_t)arg;
    for (;;) {
        item_t it;
        if (xQueueReceive(queue, &it, pdMS_TO_TICKS(EVENT_POLL_MS)) == pdTRUE) {
            deliver(&it);
        }
    }
}

bool fck_events_start(void)
{
    taskENTER_CRITICAL(&s_mux);
    bool const first = (s_queue == NULL) && !s_started;
    if (first) {
        s_started = true;       /* one task makes it; another meanwhile finds it "started" and queue NULL */
    }
    taskEXIT_CRITICAL(&s_mux);
    if (first) {
        for (size_t i = 0U; i < WAITERS_MAX; i++) {
            s_sem[i] = xSemaphoreCreateBinaryStatic(&s_sem_buf[i]);
        }
        QueueHandle_t const q = xQueueCreate(FCK_EVENT_QUEUE, sizeof(item_t));
        TaskHandle_t        t = NULL;
        if ((q != NULL)
            && (xTaskCreate(events_task, "fck_events", EVENT_STACK_BYTES, (void *)q, EVENT_PRIORITY, &t) == pdPASS)) {
            s_task  = t;
            s_queue = q;        /* last: the sink queues nothing before it */
            fck_aos_sink_set(FCK_AOS_SINK_EVENTS, events_sink, NULL);
        } else {
            if (q != NULL) {
                vQueueDelete(q);
            }
            taskENTER_CRITICAL(&s_mux);
            s_started = false;  /* the next try may have the memory */
            taskEXIT_CRITICAL(&s_mux);
            KLOG_E(TAG, "no memory for the listeners' task and queue");
        }
    }
    return s_queue != NULL;
}

bool fck_events_in_listener(void)
{
    return (s_task != NULL) && (xTaskGetCurrentTaskHandle() == s_task);
}

/* ---- The app's side ----------------------------------------------------------------------- */

bool fck_event_add_listener(const char *event, fck_listener_t fn)
{
    if (fck_aos_refuse_ao("fck_event_add_listener()")) {
        return false;
    }
    int const idx = name_index(event);
    if (idx < 0 || fn == NULL) {
        (void)fck_fail(KODE_DENIED, "event is not one of the names in fck.h, or fn is NULL");
        return false;
    }
    if (!fck_events_start()) {
        (void)fck_fail(KODE_NO_RESOURCES, "no memory for the listeners' task and queue");
        return false;
    }
    kode_status_t st = KODE_OK;
    taskENTER_CRITICAL(&s_mux);
    int free_slot = -1;
    for (size_t i = 0U; i < FCK_LISTENERS_MAX; i++) {
        if (s_listener[i].used) {
            if (s_listener[i].name == (uint8_t)idx && s_listener[i].fn == fn) {
                st = KODE_EXISTS;
            }
        } else if (free_slot < 0) {
            free_slot = (int)i;
        }
    }
    if (st == KODE_OK && free_slot < 0) {
        st = KODE_NO_RESOURCES;
    }
    if (st == KODE_OK) {
        s_listener[free_slot] = (listener_t){ .used = true, .name = (uint8_t)idx, .fn = fn };
        interest_update();
    }
    taskEXIT_CRITICAL(&s_mux);
    if (st == KODE_EXISTS) {
        (void)fck_fail(st, "fn is already listening to that event");
    } else if (st != KODE_OK) {
        (void)fck_fail(st, "FCK_LISTENERS_MAX listeners are taken");
    } else {
        fck_aos_error_ok();
    }
    return st == KODE_OK;
}

bool fck_event_remove_listener(const char *event, fck_listener_t fn)
{
    if (fck_aos_refuse_ao("fck_event_remove_listener()")) {
        return false;
    }
    int const idx = name_index(event);
    if (idx < 0 || fn == NULL) {
        (void)fck_fail(KODE_DENIED, "event is not one of the names in fck.h, or fn is NULL");
        return false;
    }
    bool found = false;
    taskENTER_CRITICAL(&s_mux);
    for (size_t i = 0U; i < FCK_LISTENERS_MAX; i++) {
        if (s_listener[i].used && s_listener[i].name == (uint8_t)idx && s_listener[i].fn == fn) {
            s_listener[i].used = false;
            found = true;
        }
    }
    interest_update();
    taskEXIT_CRITICAL(&s_mux);
    if (!found) {
        (void)fck_fail(KODE_DENIED, "fn was not listening to that event");
        return false;
    }
    /* When it returns true, fn is not running: a listener removing itself cannot wait for itself. */
    bool running = false;
    if (!fck_events_in_listener()) {
        TickType_t const t0 = xTaskGetTickCount();    /* the tick may be longer than the poll: count ticks, not polls */
        for (;;) {
            taskENTER_CRITICAL(&s_mux);
            running = (s_running == fn);
            taskEXIT_CRITICAL(&s_mux);
            if (!running || ((uint32_t)((xTaskGetTickCount() - t0) * portTICK_PERIOD_MS) >= REMOVE_WAIT_MS)) {
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(REMOVE_POLL_MS) > 0U ? pdMS_TO_TICKS(REMOVE_POLL_MS) : 1U);
        }
    }
    if (running) {
        /* It is removed and is not called again, but it has not returned: what it uses must stay. */
        (void)fck_fail(KODE_TIMEOUT, "the listener is still running");
        return false;
    }
    fck_aos_error_ok();
    return true;
}

bool fck_event_wait(const char *event, uint32_t ms, fck_event_t *out)
{
    if (fck_aos_refuse_ao("fck_event_wait()")) {
        return false;
    }
    if (fck_events_in_listener()) {
        (void)fck_fail(KODE_DENIED, "fck_event_wait() was called from a listener: it would wait for itself");
        return false;
    }
    int const idx = name_index(event);
    if (idx < 0 || ms == 0U || ms > FCK_AOS_CALL_MS_MAX) {
        (void)fck_fail(KODE_DENIED, "event is not one of the names in fck.h, or ms is 0 or above FCK_AOS_CALL_MS_MAX");
        return false;
    }
    fck_aos_status_t s;
    fck_aos_status(&s);
    if (!s.started) {
        (void)fck_fail(KODE_DENIED, "fck_begin() has not run, or fck_end() was called");
        return false;
    }
    if (!fck_events_start()) {
        (void)fck_fail(KODE_NO_RESOURCES, "no memory for the listeners' task and queue");
        return false;
    }
    uint32_t const mark = fck_aos_cancel_mark();
    int            slot = -1;
    taskENTER_CRITICAL(&s_mux);
    for (size_t i = 0U; slot < 0 && i < WAITERS_MAX; i++) {
        if (!s_waiter[i].used) {
            slot = (int)i;
        }
    }
    if (slot >= 0) {
        s_waiter[slot] = (waiter_t){ .used = true, .name = (uint8_t)idx, .after = atomic_load(&s_seq) };
        interest_update();
    }
    taskEXIT_CRITICAL(&s_mux);
    if (slot < 0) {
        (void)fck_fail(KODE_NO_RESOURCES, "too many tasks are waiting for events at once");
        return false;
    }
    SemaphoreHandle_t const sem = s_sem[slot];
    (void)xSemaphoreTake(sem, 0);       /* a stale give of the slot's last waiter */

    /* The time is measured from the start, not counted in steps: a give that is not for this
     * wait (a late one for the slot's last waiter) wakes it early, and the rest is still waited. */
    bool             cut   = false;
    TickType_t const start = xTaskGetTickCount();
    TickType_t       total = pdMS_TO_TICKS(ms);
    if ((ms > 0U) && (total == 0U)) {
        total = 1U;
    }
    TickType_t step_max = pdMS_TO_TICKS(CANCEL_STEP_MS);
    if (step_max == 0U) {
        step_max = 1U;
    }
    for (;;) {
        TickType_t const spent = xTaskGetTickCount() - start;
        if (spent >= total) {
            break;
        }
        TickType_t const rest = total - spent;
        (void)xSemaphoreTake(sem, (rest < step_max) ? rest : step_max);
        taskENTER_CRITICAL(&s_mux);
        bool const arrived = s_waiter[slot].got;
        taskEXIT_CRITICAL(&s_mux);
        if (arrived) {
            break;
        }
        if (fck_aos_cancelled(mark)) {
            cut = true;
            break;
        }
    }

    taskENTER_CRITICAL(&s_mux);
    bool const        got = s_waiter[slot].got;
    fck_event_t const ev  = s_waiter[slot].event;
    s_waiter[slot].used   = false;
    interest_update();
    taskEXIT_CRITICAL(&s_mux);
    if (got) {
        if (out != NULL) {
            *out = ev;
        }
        fck_aos_error_ok();
        return true;
    }
    if (cut) {
        (void)fck_fail(KODE_DENIED, FCK_WHY_CANCELLED);
    } else {
        (void)fck_fail(KODE_TIMEOUT, "the event did not happen in the time given");
    }
    return false;
}
