/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file fck_plan.c
 * @brief The owner's rules, in plain C. See fck_plan.h.
 */
#include "fck_plan.h"

#include "fck_bsp.h"

fck_plan_ident_t fck_plan_identify(uint8_t input)
{
    /* cppcheck-suppress misra-c2012-10.8 ; the BSP's mask is a cast of a composite expression, and is the one place that says which pins are the module's */
    uint32_t const pullups = FCK_BSP_EXPANDER_PULLUPS_MASK;
    return (((uint32_t)input & pullups) != 0U) ? FCK_PLAN_ID_FCK : FCK_PLAN_ID_OTHER;
}

fck_plan_ident_t fck_plan_canary_kept(uint8_t mark, uint8_t read_back)
{
    return (read_back == mark) ? FCK_PLAN_ID_FCK : FCK_PLAN_ID_OTHER;
}

bool fck_plan_power_lost(uint8_t polarity)
{
    return (polarity & (uint8_t)FCK_BSP_EXPANDER_CANARY_BIT) == 0U;
}

bool fck_plan_look_waits(uint32_t agreed, bool troubled, uint32_t reask)
{
    return (fck_plan_ruled_out(agreed) || troubled) && (reask > 0U);
}

bool fck_plan_ruled_out(uint32_t agreed)
{
    return agreed >= FCK_PLAN_RULEOUT_LOOKS;
}

bool fck_plan_gone(uint32_t misses)
{
    return misses >= FCK_PLAN_GONE_MISSES;
}

bool fck_plan_command_line_ok(const char *line)
{
    if (line == NULL) {
        return false;
    }
    size_t n = 0U;
    for (; line[n] != '\0'; n++) {
        uint32_t const c = (uint32_t)(unsigned char)line[n];
        if ((c == 0x0AU) && (line[n + 1U] == '\0')) {
            break;
        }
        if ((c < 0x20U) || (c > 0x7EU) || (n >= FCK_PLAN_COMMAND_LINE_MAX)) {
            return false;
        }
    }
    return n > 0U;
}

uint32_t fck_plan_fix_age_ms(uint32_t now, uint32_t at, uint32_t tick_hz)
{
    uint32_t age = UINT32_MAX;

    if (tick_hz != 0U) {
        /* Unsigned, so the counter's wrap between the two is counted through. */
        uint32_t const ticks = now - at;
        uint64_t const ms    = ((uint64_t)ticks * 1000U) / tick_hz;
        if (ms < (uint64_t)UINT32_MAX) {
            age = (uint32_t)ms;
        }
    }
    return age;
}

kode_status_t fck_plan_fix_verdict(bool first, bool latest, bool present, bool same_module,
                                   bool running, bool have, bool newer)
{
    if (!present) {
        return first ? KODE_DENIED : KODE_IO_ERROR;
    }
    if (!same_module) {
        return KODE_IO_ERROR;
    }
    if (!running) {
        return KODE_DENIED;
    }
    if (have && (latest || newer)) {
        return KODE_OK;
    }
    return KODE_TIMEOUT;
}
