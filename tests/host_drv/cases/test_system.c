/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_system.c
 * @brief fck_drv_system: the restart hook, against a fake one.
 */
#include "unity.h"

#include "fck_drv_system.h"

#include "fake_idf.h"

static void hook(void)
{
}

TEST_CASE("the restart hook is registered with ESP-IDF as it is given", "[system]")
{
    idf_fake_reset();
    TEST_ASSERT_EQUAL(KODE_OK, fck_drv_system_on_restart(hook));
    TEST_ASSERT_EQUAL_UINT(1U, g_idf.shutdowns);
    TEST_ASSERT_TRUE(g_idf.shutdown_fn == hook);
}

TEST_CASE("a hook registered twice is still a hook registered", "[system]")
{
    idf_fake_reset();
    g_idf.shutdown_rc = ESP_ERR_INVALID_STATE;
    TEST_ASSERT_EQUAL(KODE_OK, fck_drv_system_on_restart(hook));
}

TEST_CASE("a hook ESP-IDF has no room for is a shortage of resources", "[system]")
{
    idf_fake_reset();
    g_idf.shutdown_rc = ESP_ERR_NO_MEM;
    TEST_ASSERT_EQUAL(KODE_NO_RESOURCES, fck_drv_system_on_restart(hook));
}
