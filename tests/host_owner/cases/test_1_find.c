/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_1_find.c
 * @brief Finding the module: what the owner does to a header before it knows
 *        what is on it (fck_aos.h, "Finding a module").
 *
 * The rule under test: until a chip at 0x21 has read like the F*CK's expander
 * and kept the power-loss mark, nothing is written to the module and no line
 * of it is driven. A Mesh module, a camera or a board of the user's own may
 * be on J3, and it is theirs. Every case looks at the wire log, which lists
 * what reached a chip.
 *
 * The cases share one owner and run in the order of the file.
 */
#include <string.h>

#include "unity.h"

#include "fck.h"
#include "fck_core.h"
#include "fck_bsp.h"
#include "fck_aos.h"

#include "fake_world.h"

#define PULLUPS ((uint8_t)FCK_BSP_EXPANDER_PULLUPS_MASK)
#define P2      ((uint8_t)(1U << FCK_BSP_EXPANDER_GNSS_RST))
#define P6      ((uint8_t)(1U << FCK_BSP_EXPANDER_NRF_RST))

static uint32_t generation(void)
{
    fck_status_t i;
    fck_core_status(&i);
    return i.generation;
}

/* Nothing in the log or the wire may say a module was written, or driven. */
static void assert_untouched(const char *why)
{
    TEST_ASSERT_EQUAL_MESSAGE(0, sim_wire_count(SIM_W_WRITE), why);
    TEST_ASSERT_EQUAL_MESSAGE(0, sim_wire_count(SIM_W_BLIND), why);
    TEST_ASSERT_EQUAL_MESSAGE(-1, sim_wire_first_write(), why);
    TEST_ASSERT_EQUAL_MESSAGE(0, sim_c5_resets(), why);
    TEST_ASSERT_FALSE_MESSAGE(fck_aos_present(), why);
}

TEST_CASE("an empty header is looked at and nothing more", "[find]")
{
    sim_begin_empty();
    sim_sleep(1000U);
    TEST_ASSERT_EQUAL_UINT(0, sim_wire_n());
    assert_untouched("an empty header");
}

TEST_CASE("a module that NAKs 0x21 is probed and never touched", "[find]")
{
    sim_begin_empty();
    sim_plug(SIM_DEV_NONE, false);
    sim_sleep(1500U);
    TEST_ASSERT_EQUAL_UINT(0, sim_wire_count(SIM_W_READ));
    TEST_ASSERT_TRUE(sim_wire_count(SIM_W_PROBE) >= 2U);
    TEST_ASSERT_TRUE_MESSAGE(sim_log_has("nothing answers at 0x21"), "it is called what it is");
    assert_untouched("a clean NAK");
}

TEST_CASE("a held bus that answers nothing is not driven when nobody was identified", "[find]")
{
    sim_begin_empty();
    sim_plug(SIM_DEV_STUCK, true);
    sim_sleep(3600U);       /* the first ask, and the second, 3 s later */
    TEST_ASSERT_EQUAL_UINT(0, sim_wire_count(SIM_W_READ));
    TEST_ASSERT_TRUE(sim_wire_count(SIM_W_PROBE) >= 1U);
    TEST_ASSERT_TRUE_MESSAGE(sim_log_has("nothing is written or driven"), "it says why it waits");
    TEST_ASSERT_EQUAL_MESSAGE(1U, sim_log_count("nothing is written or driven"),
                              "and says it once");
    assert_untouched("a bus nobody acknowledges");
}

TEST_CASE("a chip that takes the register byte for its port is read once and left", "[find]")
{
    sim_begin_empty();
    sim_plug(SIM_DEV_PCF8574, false);
    sim_sleep(1500U);
    /* The only thing it was sent is INPUT's number: a byte for its port. */
    TEST_ASSERT_TRUE(sim_wire_count(SIM_W_READ) >= 2U);
    for (size_t i = 0U; i < sim_wire_n(); i++) {
        const sim_wire_t *const w = sim_wire_at(i);
        if (w->kind == SIM_W_READ) {
            TEST_ASSERT_EQUAL_HEX8(FCK_BSP_EXPANDER_REGISTER_INPUT, w->reg);
        }
    }
    TEST_ASSERT_EQUAL_HEX8(0x00, sim_pcf_port());
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(sim_wire_count(SIM_W_READ), sim_pcf_writes(),
                                     "one byte per read, and no other");
    TEST_ASSERT_TRUE_MESSAGE(sim_log_has("is not the F*CK's expander"), "it is ruled out");
    assert_untouched("a PCF8574");
}

TEST_CASE("a chip whose pins read low at 0x21 is not written at all", "[find]")
{
    sim_begin_empty();
    sim_plug_t const p = { .dev = SIM_DEV_LOOKALIKE, .pins = 0x00 };
    sim_plug_ex(&p);
    sim_sleep(1500U);
    TEST_ASSERT_TRUE(sim_wire_count(SIM_W_READ) >= 2U);
    TEST_ASSERT_EQUAL_UINT(0, sim_wire_count(SIM_W_WRITE));
    assert_untouched("pins that read low");
}

TEST_CASE("a chip that reads like the expander but keeps no POLARITY gets one write", "[find]")
{
    sim_begin_empty();
    sim_plug(SIM_DEV_LOOKALIKE, false);
    sim_sleep(1500U);
    /* One look reads INPUT, writes the mark and reads it back; the next does
     * the same. Nothing else is ever written. */
    size_t writes = 0U;
    for (size_t i = 0U; i < sim_wire_n(); i++) {
        const sim_wire_t *const w = sim_wire_at(i);
        if (w->kind == SIM_W_WRITE) {
            writes++;
            TEST_ASSERT_EQUAL_HEX8(FCK_BSP_EXPANDER_REGISTER_POLARITY, w->reg);
            TEST_ASSERT_EQUAL_HEX8(FCK_BSP_EXPANDER_CANARY_BIT, w->val);
        }
    }
    TEST_ASSERT_TRUE(writes >= 1U && writes <= 3U);
    TEST_ASSERT_EQUAL_UINT32(sim_wire_count(SIM_W_WRITE), writes);
    TEST_ASSERT_EQUAL_UINT(0, sim_wire_count(SIM_W_BLIND));
    TEST_ASSERT_EQUAL_UINT(0, sim_c5_resets());
    TEST_ASSERT_FALSE(fck_aos_present());
    TEST_ASSERT_TRUE(sim_log_has("is not the F*CK's expander"));
}

TEST_CASE("the F*CK is read, then marked, and only then set up", "[find]")
{
    sim_begin_empty();
    uint32_t const gen = generation();
    sim_plug(SIM_DEV_EXPANDER, false);
    TEST_ASSERT_TRUE_MESSAGE(sim_wait_present(2000U), "found");

    /* The order on the wire: an address, INPUT, and the first write is the
     * power-loss mark in POLARITY. */
    TEST_ASSERT_EQUAL(SIM_W_PROBE, sim_wire_at(0)->kind);
    TEST_ASSERT_EQUAL(SIM_W_READ, sim_wire_at(1)->kind);
    TEST_ASSERT_EQUAL_HEX8(FCK_BSP_EXPANDER_REGISTER_INPUT, sim_wire_at(1)->reg);
    TEST_ASSERT_TRUE_MESSAGE((sim_wire_at(1)->val & PULLUPS) != 0U, "the pull-ups read high");
    TEST_ASSERT_EQUAL_INT(2, sim_wire_first_write());
    TEST_ASSERT_EQUAL_HEX8(FCK_BSP_EXPANDER_REGISTER_POLARITY, sim_wire_at(2)->reg);
    TEST_ASSERT_EQUAL_HEX8(FCK_BSP_EXPANDER_CANARY_BIT, sim_wire_at(2)->val);
    TEST_ASSERT_EQUAL_UINT(0, sim_wire_count(SIM_W_BLIND));

    /* The module as fck_aos.h says it is left. */
    TEST_ASSERT_EQUAL_UINT32(gen + 1U, generation());
    TEST_ASSERT_FALSE_MESSAGE(sim_gnss_running(), "the receiver is in reset");
    TEST_ASSERT_EQUAL_HEX8(0x00, (uint8_t)(sim_exp_cfg() & P2));
    TEST_ASSERT_EQUAL_HEX8(0x00, (uint8_t)(sim_exp_out() & P2));
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(FCK_BSP_EXPANDER_SW0_MASK, (uint8_t)(sim_exp_out() & FCK_BSP_EXPANDER_SW0_MASK),
                                   "SW0 rests high: 433's leg, and 868 and 915's");
    TEST_ASSERT_EQUAL_HEX8(0x00, (uint8_t)(sim_exp_cfg() & FCK_BSP_EXPANDER_SW0_MASK));
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(1U << FCK_BSP_EXPANDER_HOST_WP_NRF,
                                   (uint8_t)(sim_exp_cfg() & (1U << FCK_BSP_EXPANDER_HOST_WP_NRF)),
                                   "the nRF's wake line (P4) is left an input");
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(P6, (uint8_t)(sim_exp_cfg() & P6), "the nRF's reset is let go");
    TEST_ASSERT_EQUAL_HEX8(FCK_BSP_EXPANDER_CANARY_BIT, sim_exp_pol());
    fck_status_t info;
    fck_core_status(&info);
    TEST_ASSERT_TRUE(info.present && info.c5_on && info.nrf_on && info.cc1101_ok);
    TEST_ASSERT_FALSE(info.gnss_running);
    TEST_ASSERT_TRUE(sim_log_has("F*CK found"));
}

TEST_CASE("an F*CK plugged in while its receiver boots is found before it takes SDA", "[find]")
{
    sim_begin_empty();
    sim_plug(SIM_DEV_EXPANDER, true);   /* standard firmware: SDA at 420 ms */
    TEST_ASSERT_TRUE_MESSAGE(sim_wait_present(2000U), "found");
    sim_sleep(800U);
    TEST_ASSERT_FALSE_MESSAGE(sim_bus_dead(), "its receiver was in reset in time");
    TEST_ASSERT_EQUAL_UINT(0, sim_wire_count(SIM_W_BLIND));
    TEST_ASSERT_TRUE(fck_aos_present());
}

TEST_CASE("an F*CK whose receiver already holds SDA is not found, and not driven", "[find]")
{
    sim_begin_empty();
    /* The app restarted without parking, the receiver had been left running. */
    sim_plug_t const p = { .dev = SIM_DEV_EXPANDER, .gnss_standard_fw = true,
                           .gnss_age_ms = 1000U, .pins = 0xF7 };
    sim_plug_ex(&p);
    TEST_ASSERT_TRUE(sim_bus_dead());
    sim_sleep(3600U);
    TEST_ASSERT_TRUE(sim_bus_dead());
    TEST_ASSERT_EQUAL_UINT(0, sim_wire_count(SIM_W_READ));
    assert_untouched("a dead bus nobody was identified on");
    TEST_ASSERT_TRUE(sim_log_has("is not found until it is unplugged"));
}

TEST_CASE("it is found again once it is unplugged and plugged in", "[find]")
{
    sim_begin_empty();
    sim_plug_t const old = { .dev = SIM_DEV_EXPANDER, .gnss_standard_fw = true,
                             .gnss_age_ms = 1000U, .pins = 0xF7 };
    sim_plug_ex(&old);
    sim_sleep(700U);
    sim_unplug();
    sim_sleep(700U);
    sim_plug(SIM_DEV_EXPANDER, true);
    TEST_ASSERT_TRUE_MESSAGE(sim_wait_present(2000U), "found when plugged in");
    TEST_ASSERT_FALSE(sim_gnss_running());
    TEST_ASSERT_EQUAL_UINT(0, sim_wire_count(SIM_W_BLIND));
}

TEST_CASE("an F*CK that was identified is rescued when its receiver takes the bus", "[find]")
{
    sim_begin_fck(NULL, NULL);
    uint32_t const gen = generation();
    /* The receiver is let out of reset by something else, and takes SDA; the
     * pair stays held: it is the same module. */
    sim_gnss_take_bus();
    TEST_ASSERT_TRUE(sim_bus_dead());
    /* It finds the bus gone, and the module again, in one go. */
    TEST_ASSERT_TRUE_MESSAGE(sim_wait_generation(gen + 1U, 6000U), "the module is set up again");
    TEST_ASSERT_TRUE(fck_aos_present());
    TEST_ASSERT_TRUE(sim_wire_count(SIM_W_BLIND) >= 2U);
    TEST_ASSERT_FALSE_MESSAGE(sim_bus_dead(), "its receiver is back in reset");
    TEST_ASSERT_FALSE(sim_gnss_running());
    TEST_ASSERT_EQUAL_UINT32(gen + 1U, generation());
}

TEST_CASE("a module that left is forgotten: its successor on a dead bus is not driven", "[find]")
{
    sim_begin_fck(NULL, NULL);
    sim_unplug();
    TEST_ASSERT_TRUE(sim_wait_absent(5000U));
    sim_sleep(800U);        /* looks at an empty header forget it */
    sim_wire_clear();
    sim_plug(SIM_DEV_STUCK, true);
    sim_sleep(1500U);
    assert_untouched("another module on the header");
}

TEST_CASE("an F*CK that lost power for a moment is set up again", "[find]")
{
    sim_begin_fck(NULL, NULL);
    uint32_t const gen = generation();
    sim_blip();
    TEST_ASSERT_TRUE_MESSAGE(sim_wait_generation(gen + 1U, 4000U), "the module is set up again");
    TEST_ASSERT_TRUE(fck_aos_present());
    TEST_ASSERT_TRUE(sim_log_has("lost power for a moment"));
    TEST_ASSERT_EQUAL_UINT32(gen + 1U, generation());
    TEST_ASSERT_FALSE(sim_gnss_running());
    TEST_ASSERT_EQUAL_HEX8(FCK_BSP_EXPANDER_CANARY_BIT, sim_exp_pol());
    TEST_ASSERT_EQUAL_UINT(0, sim_wire_count(SIM_W_BLIND));
}

TEST_CASE("with no I2C controller free nothing is attempted, and the owner says so", "[find]")
{
    sim_begin_empty();
    sim_no_i2c_controller(true);
    sim_plug(SIM_DEV_EXPANDER, false);
    sim_sleep(1000U);
    TEST_ASSERT_EQUAL_UINT(0, sim_wire_n());
    TEST_ASSERT_FALSE(fck_aos_present());
    TEST_ASSERT_TRUE(sim_log_has("no I2C controller is free"));
    sim_no_i2c_controller(false);
}
