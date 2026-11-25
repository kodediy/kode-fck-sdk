/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_8_events.c
 * @brief What the owner tells the app as it happens (sim_on_event()), and
 *        where the module's lines are (fck_header_position()).
 *
 * The callback runs on the owner's task; a case reads what it wrote down
 * after letting the model's clock run.
 */
#include <string.h>

#include "unity.h"

#include "fck.h"
#include "fck_core.h"
#include "fck_bsp.h"
#include "fck_aos.h"

#include "fake_world.h"

#define EVENTS_MAX 8U

static struct {
    unsigned    n;
    fck_aos_event_t ev[EVENTS_MAX];
    uint32_t    arg[EVENTS_MAX];
    void       *ctx;
} g;

static void on_ev(fck_aos_event_t ev, uint32_t arg, void *ctx)
{
    if (g.n < EVENTS_MAX) {
        g.ev[g.n]  = ev;
        g.arg[g.n] = arg;
    }
    g.n++;
    g.ctx = ctx;
}

static void listen(void *ctx)
{
    (void)memset(&g, 0, sizeof(g));
    sim_on_event(on_ev, ctx);
}

static void deafen(void)
{
    sim_on_event(NULL, NULL);
}

TEST_CASE("an F*CK found is told once as PRESENT, with the app's context", "[events]")
{
    sim_begin_empty();
    int marker = 0;
    listen(&marker);
    sim_plug(SIM_DEV_EXPANDER, false);
    TEST_ASSERT_TRUE(sim_wait_present(2000U));
    sim_sleep(200U);
    deafen();
    TEST_ASSERT_EQUAL_UINT(1U, g.n);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)FCK_AOS_EVENT_PRESENT, (uint32_t)g.ev[0]);
    TEST_ASSERT_EQUAL_UINT32(0U, g.arg[0]);
    TEST_ASSERT_EQUAL_PTR(&marker, g.ctx);
}

TEST_CASE("a module that leaves is told as GONE", "[events]")
{
    sim_begin_fck(NULL, NULL);
    listen(NULL);
    sim_unplug();
    TEST_ASSERT_TRUE(sim_wait_absent(5000U));
    deafen();
    TEST_ASSERT_EQUAL_UINT(1U, g.n);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)FCK_AOS_EVENT_GONE, (uint32_t)g.ev[0]);
}

TEST_CASE("a module that lost power is GONE, then PRESENT again", "[events]")
{
    sim_begin_fck(NULL, NULL);
    fck_status_t info;
    fck_core_status(&info);
    listen(NULL);
    sim_blip();
    TEST_ASSERT_TRUE(sim_wait_generation(info.generation + 1U, 4000U));
    sim_sleep(200U);
    deafen();
    TEST_ASSERT_EQUAL_UINT(2U, g.n);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)FCK_AOS_EVENT_GONE, (uint32_t)g.ev[0]);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)FCK_AOS_EVENT_PRESENT, (uint32_t)g.ev[1]);
}

TEST_CASE("a chip at the expander's address that is not one is WRONG_MODULE, once", "[events]")
{
    sim_begin_empty();
    listen(NULL);
    sim_plug(SIM_DEV_PCF8574, false);
    sim_sleep(5000U);
    deafen();
    TEST_ASSERT_EQUAL_UINT(1U, g.n);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)FCK_AOS_EVENT_WRONG_MODULE, (uint32_t)g.ev[0]);
    TEST_ASSERT_EQUAL_UINT32(FCK_WRONG_MODULE_OTHER_CHIP, g.arg[0]);
}

TEST_CASE("a module that holds the pair and answers nothing is WRONG_MODULE, once", "[events]")
{
    sim_begin_empty();
    listen(NULL);
    sim_plug(SIM_DEV_NONE, false);
    sim_sleep(5000U);
    deafen();
    TEST_ASSERT_EQUAL_UINT(1U, g.n);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)FCK_AOS_EVENT_WRONG_MODULE, (uint32_t)g.ev[0]);
    TEST_ASSERT_EQUAL_UINT32(FCK_WRONG_MODULE_HOLDS_PAIR, g.arg[0]);
}

TEST_CASE("with no callback nothing is relayed, and one set late hears what comes after", "[events]")
{
    sim_begin_empty();
    listen(NULL);
    deafen();
    sim_plug(SIM_DEV_EXPANDER, false);
    TEST_ASSERT_TRUE(sim_wait_present(2000U));
    sim_sleep(200U);
    TEST_ASSERT_EQUAL_UINT(0U, g.n);

    listen(NULL);
    sim_unplug();
    TEST_ASSERT_TRUE(sim_wait_absent(5000U));
    deafen();
    TEST_ASSERT_EQUAL_UINT(1U, g.n);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)FCK_AOS_EVENT_GONE, (uint32_t)g.ev[0]);
}

TEST_CASE("header_position is a position itself while the module is there, else 0", "[events]")
{
    sim_begin_empty();
    TEST_ASSERT_EQUAL_UINT8(0U, fck_header_position(FCK_BSP_POSITION_C5_EN));
    sim_plug(SIM_DEV_EXPANDER, false);
    TEST_ASSERT_TRUE(sim_wait_present(2000U));
    TEST_ASSERT_EQUAL_UINT8(FCK_BSP_POSITION_C5_EN, fck_header_position(FCK_BSP_POSITION_C5_EN));
    TEST_ASSERT_EQUAL_UINT8(FCK_BSP_POSITION_I2C_SDA, fck_header_position(FCK_BSP_POSITION_I2C_SDA));
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(0U, fck_header_position(0U), "there is no position 0");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(0U, fck_header_position(FCK_BSP_POSITIONS + 1U), "J3 has 14");
    sim_unplug();
    TEST_ASSERT_TRUE(sim_wait_absent(5000U));
    TEST_ASSERT_EQUAL_UINT8(0U, fck_header_position(FCK_BSP_POSITION_C5_EN));
}
