/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file runner.c
 * @brief Entry point of the simulation suite: run every registered case, then go.
 *
 * The exit() is not tidiness. The host port starts a scheduler and never
 * stops it, so a runner that simply returns from app_main() hangs forever -
 * under CI, a job that times out with no result.
 */
#include <stdlib.h>

#include "unity.h"

#include "fake_world.h"

/** @brief Runs before every case. IDF's default is weak; this replaces it. */
void setUp(void)
{
}

/** @brief Runs after every case. */
void tearDown(void)
{
}

/**
 * Every case, or - for the one working on a change - only those whose tags
 * contain FCK_SIM_TAG, or none of FCK_SIM_SKIP_TAG ("[slow]" leaves out the ones
 * that wait seconds for a chip that never answers). tools/host_tests.sh sets
 * neither.
 */
void app_main(void)
{
    sim_init();
    const char *const only = getenv("FCK_SIM_TAG");
    const char *const skip = getenv("FCK_SIM_SKIP_TAG");
    UNITY_BEGIN();
    if (only != NULL) {
        unity_run_tests_by_tag(only, false);
    } else if (skip != NULL) {
        unity_run_tests_by_tag(skip, true);
    } else {
        unity_run_all_tests();
    }
    exit(UNITY_END());
}
