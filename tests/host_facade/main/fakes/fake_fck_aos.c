/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file fake_fck_aos.c
 * @brief fake_fck_aos.h: the owner's calls that fck.c, fck_wifi.c, fck_radio.c
 *        and fck_gnss.c make, written down and answered.
 */
#include "fake_fck_aos.h"

#include <string.h>

fake_fck_aos_t g_aos;

static void note(fake_call_t c)
{
    if (g_aos.n_calls < FAKE_CALLS_MAX) {
        g_aos.calls[g_aos.n_calls] = c;
    }
    g_aos.n_calls++;
}

static void copy(char *to, const char *from)
{
    (void)strncpy(to, (from != NULL) ? from : "", FAKE_TEXT_MAX - 1U);
    to[FAKE_TEXT_MAX - 1U] = '\0';
}

void fake_fck_aos_reset(void)
{
    (void)memset(&g_aos, 0, sizeof(g_aos));
    g_aos.present       = true;
    g_aos.st.present    = true;
    g_aos.st.generation = 1U;
    g_aos.command_rc    = KODE_OK;
    g_aos.hold_rc       = KODE_OK;
    g_aos.power_rc      = KODE_OK;
    g_aos.reset_rc      = KODE_OK;
    g_aos.gnss_power_rc = KODE_OK;
    g_aos.start_rc      = KODE_OK;
    g_aos.cancel_rc     = KODE_OK;
    g_aos.wait_rc       = KODE_OK;
    for (int i = 0; i < FCK_AOS_CHIPS; i++) {
        g_aos.firmware_rc[i] = KODE_OK;
    }
    g_aos.gnss_fix_rc = KODE_OK;
    g_aos.post_rc     = KODE_OK;
}

void fake_fck_aos_firmware(fck_aos_chip_t chip, const char *body)
{
    g_aos.firmware[chip].known = (body != NULL);
    g_aos.firmware[chip].seq++;
    (void)strncpy(g_aos.firmware[chip].body, (body != NULL) ? body : "", FCK_AOS_FIRMWARE_MAX - 1U);
}

bool fake_fck_aos_called(fake_call_t c)
{
    for (unsigned i = 0U; i < g_aos.n_calls && i < FAKE_CALLS_MAX; i++) {
        if (g_aos.calls[i] == c) {
            return true;
        }
    }
    return false;
}

/* ---- What the facade calls ---------------------------------------------------------- */

kode_status_t fck_aos_start(void)
{
    note(CALL_START);
    return g_aos.start_rc;
}

kode_status_t fck_aos_stop(void)
{
    note(CALL_STOP);
    return g_aos.stop_rc;
}

void fck_aos_error_set(const char *why)
{
    g_aos.why = (why != NULL) ? why : "";
}

const char *fck_aos_error(void)
{
    return (g_aos.why != NULL) ? g_aos.why : "";
}

kode_status_t fck_aos_cancel(void)
{
    note(CALL_CANCEL);
    g_aos.cancel_gen++;
    return g_aos.cancel_rc;
}

uint32_t fck_aos_cancel_mark(void)
{
    return g_aos.cancel_gen;
}

bool fck_aos_cancelled(uint32_t mark)
{
    return g_aos.cancel_gen != mark;
}

bool fck_aos_present(void)
{
    return g_aos.present;
}

kode_status_t fck_aos_wait_present(uint32_t ms)
{
    note(CALL_WAIT_PRESENT);
    g_aos.last_ms = ms;
    return g_aos.wait_rc;
}

void fck_aos_status(fck_aos_status_t *out)
{
    *out = g_aos.st;
}

kode_status_t fck_aos_firmware(fck_aos_chip_t chip, uint32_t ms, fck_aos_firmware_t *out)
{
    note(CALL_FIRMWARE);
    g_aos.last_ms   = ms;
    g_aos.last_chip = chip;
    if (g_aos.firmware_rc[chip] != KODE_OK) {
        return g_aos.firmware_rc[chip];
    }
    *out = g_aos.firmware[chip];
    return KODE_OK;
}

kode_status_t fck_aos_command_if_same_firmware(fck_aos_chip_t chip, uint32_t seq, const char *line,
                                            const char *verb, fck_drv_link_message_fn on_reply,
                                            void *ctx, uint32_t ms, int32_t *rc)
{
    note(CALL_COMMAND);
    g_aos.commands++;
    if (g_aos.cancel_after != 0U && g_aos.commands == g_aos.cancel_after) {
        g_aos.cancel_gen++;     /* an app's fck_cancel(), landing while this one runs */
    }
    g_aos.cmd_chip = chip;
    g_aos.cmd_seq  = seq;
    g_aos.cmd_ms   = ms;
    copy(g_aos.cmd_line, line);
    copy(g_aos.cmd_verb, verb);
    if (g_aos.command_rc != KODE_OK) {
        return g_aos.command_rc;
    }
    if (on_reply != NULL) {
        for (unsigned i = 0U; i < FAKE_LINES_MAX && g_aos.replies[i].tag != NULL; i++) {
            const fck_drv_link_message_t m = { .tag = g_aos.replies[i].tag, .body = g_aos.replies[i].body };
            on_reply(ctx, &m);
        }
    }
    if (rc != NULL) {
        *rc = g_aos.command_out;
    }
    return KODE_OK;
}

kode_status_t fck_aos_hold(fck_aos_chip_t chip, uint32_t ms)
{
    note(CALL_HOLD);
    g_aos.last_chip = chip;
    g_aos.last_ms   = ms;
    return g_aos.hold_rc;
}

kode_status_t fck_aos_release(fck_aos_chip_t chip)
{
    note(CALL_RELEASE);
    g_aos.last_chip = chip;
    return KODE_OK;
}

kode_status_t fck_aos_power(fck_aos_chip_t chip, bool on)
{
    note(CALL_POWER);
    g_aos.last_chip = chip;
    g_aos.last_on   = on;
    return g_aos.power_rc;
}

kode_status_t fck_aos_reset(fck_aos_chip_t chip)
{
    note(CALL_RESET);
    g_aos.last_chip = chip;
    return g_aos.reset_rc;
}

kode_status_t fck_aos_gnss_power(bool run)
{
    note(CALL_GNSS_POWER);
    if (g_aos.gnss_powers < 4U) {
        g_aos.gnss_power_ons[g_aos.gnss_powers] = run;
    }
    g_aos.gnss_powers++;
    return g_aos.gnss_power_rc;
}

kode_status_t fck_aos_gnss_fix(uint32_t ms, fck_drv_nmea_fix_t *out, uint32_t *age_ms)
{
    note(CALL_GNSS_FIX);
    g_aos.fix_ms = ms;
    if (g_aos.gnss_fix_rc == KODE_OK) {
        *out = g_aos.fix;
        if (age_ms != NULL) {
            *age_ms = g_aos.fix_age_ms;
        }
    }
    return g_aos.gnss_fix_rc;
}

uint8_t fck_aos_header_position(uint8_t pos)
{
    (void)pos;
    return g_aos.pos_ret;
}

bool fck_aos_refuse_ao(const char *call_name)
{
    (void)call_name;
    return false;       /* the suite's task is no Active Object's */
}

void fck_aos_error_ok(void)
{
    g_aos.why = "";
}

void fck_aos_error_set_for(kode_status_t st, const char *why)
{
    (void)st;
    g_aos.why = (why != NULL) ? why : "";
}

kode_status_t fck_aos_command_post(fck_aos_chip_t chip, const char *line, const char *verb, uint32_t ms)
{
    (void)verb;
    (void)ms;
    g_aos.posts++;
    g_aos.last_chip = chip;
    copy(g_aos.post_line, line);
    return g_aos.post_rc;
}
