/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file fck_ao.h
 * @brief The owner's Active Object, and what it calls in fck_aos.c. Private
 *        to this component.
 *
 * Two files, one owner. fck_ao.c is the Active Object: its queue, its time
 * event, its worker, the signals it takes. fck_aos.c is what those signals
 * do: the module's state and its rules. The seam between them is this
 * header, and it is where tests/host_owner cuts: it runs fck_aos.c with the calls
 * below played by a task of its own (tests/host_owner/main/sim_ao.c).
 * tests/host_ao cuts it from the other side: fck_ao.c on QP/C, with
 * fck_aos.c's calls faked.
 *
 * fck_aos.c sees no QP/C type: a request travels as a pointer, a job is a
 * plain function, the same shape as kode_worker.h's KqpJobFn.
 */
#ifndef FCK_AO_H_
#define FCK_AO_H_

#include <stdbool.h>
#include <stdint.h>

#include "kode_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief How often the owner reads the chips' UARTs and moves what is in
 *         flight on, ms. Each UART keeps 4 KB, about 350 ms at 115200 baud. */
#define FCK_AO_TICK_MS 10U

/** @brief A request: the caller's, on its stack, until fck_aos.c answers it. */
typedef struct fck_aos_request fck_aos_request_t;

/** @brief Work that waits, run on the owner's worker. */
typedef kode_status_t (*fck_ao_job_t)(void *arg, uint32_t *value);

/* ---- fck_ao.c --------------------------------------------------------------- */

/**
 * @brief Construct the Active Object, bring its worker up, start it. Call
 *        after kode_init(), once: fck_aos_start() is the only caller.
 * @return KODE_OK; KODE_NO_RESOURCES when a request fits no event pool or
 *         the worker's task could not be had; KODE_EXISTS when another
 *         Active Object already holds the owner's priority level (which QP/C
 *         would stop the board for); KODE_DENIED when the level is not one
 *         QP/C has.
 */
kode_status_t fck_ao_start(void);

/**
 * @brief Hand @p request to the owner. Its caller waits until fck_aos.c
 *        answers it, so the owner may keep the pointer until then.
 *
 * Never asserts: an event is taken only while the pools keep some blocks
 * for the rest of the system, and posted only while the queue keeps room
 * for what arrives without asking - the tick, EXP_POWER_SIG, the rest signals,
 * HOME_SIG, the worker.
 * @return KODE_OK; KODE_NO_RESOURCES when there was no block or no room;
 *         KODE_DENIED before fck_ao_start().
 */
kode_status_t fck_ao_post(fck_aos_request_t *request);

/** @brief Whether the caller runs on the owner's own task. */
bool fck_ao_is_own_task(void);

/**
 * @brief Whether the caller runs on the task of any Active Object of the
 *        image - the owner's, kodeOS's or an app's own (kqp_current_is_ao()).
 *        A call that blocks stops an Active Object's step, and so its queue:
 *        it is refused there.
 * @param name Receives the Active Object's task name when true; may be NULL.
 */
bool fck_ao_is_any_ao_task(const char **name);

/** @brief Whether kode_init() has run: AO_Boot, which it always starts, holds its level. */
bool fck_ao_kode_is_up(void);

/** @brief Whether the caller runs on the owner's worker, in a job. */
bool fck_ao_is_worker_task(void);

/**
 * @brief Raise the owner's task to the caller's priority, if that is higher.
 *        For a way home that waits for the owner to park the module.
 */
void fck_ao_lift_to_caller(void);

/**
 * @brief Run @p fn on the owner's worker; fck_aos_on_work_done() follows.
 *
 * From the owner's task only. @p arg must stay valid until the done event:
 * the job may still be reading it.
 * @param seq Receives the job's number, for fck_aos_on_work_done().
 * @return KODE_OK; KODE_NO_RESOURCES when a job is already waiting.
 */
kode_status_t fck_ao_submit_work(fck_ao_job_t fn, void *arg, uint16_t *seq);

/* ---- fck_aos.c: what the Active Object's signals do ------------------------- */

/** @brief A request off the queue: served now, after the job in the way, or
 *         when its command ends. */
void fck_aos_on_request(fck_aos_request_t *request);

/** @brief The time event, every FCK_AO_TICK_MS. */
void fck_aos_on_tick(void);

/** @brief A switch of J3's supply is over (EXP_POWER_SIG): @p on was asked, @p rc is a
 *         kode_status_t - KODE_OK, KODE_IO_ERROR (the C5 refused) or KODE_DENIED (no link). */
void fck_aos_on_exp_power(bool on, uint8_t rc);

/** @brief The board is going to rest (a LOWPOWER_ROUND_SIG for ASLEEP, or for OFF when @p off):
 *         the module is put down, as for a way home, but it can be set up again. */
void fck_aos_on_rest(bool off);

/** @brief The board is awake (LOWPOWER_STATE_SIG with AWAKE, which repeats): if the module was
 *         put down for the rest, it is looked for again and what the app had set is given back.
 *         Idempotent, and nothing it does emits. */
void fck_aos_on_wake(void);

/**
 * @brief The bottom button was held: the board goes back to kodeOS
 *        KODE_HOME_GRACE_MS from now. The module parked first.
 */
void fck_aos_on_home(void);

/** @brief Job @p seq returned @p status and @p value on the worker. */
void fck_aos_on_work_done(uint16_t seq, kode_status_t status, uint32_t value);

#ifdef __cplusplus
}
#endif

#endif /* FCK_AO_H_ */
