/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_uart.c
 * @brief fck_drv_uart: a UART to one of the module's chips, against a fake
 *        ESP-IDF UART that keeps what arrives and what is written.
 */
#include <string.h>

#include "unity.h"

#include "fck_drv_uart.h"

#include "fake_idf.h"

TEST_CASE("open installs the driver, sets the pins the module names and the rate, and pulls RX up", "[uart]")
{
    idf_fake_reset();
    TEST_ASSERT_EQUAL(KODE_OK, fck_drv_uart_open(1, 21, 22, 115200U));
    TEST_ASSERT_TRUE(g_idf.uart[1].installed);
    TEST_ASSERT_EQUAL_INT(4096, g_idf.uart[1].rx_size);
    TEST_ASSERT_EQUAL_INT(21, g_idf.uart[1].tx_pin);
    TEST_ASSERT_EQUAL_INT(22, g_idf.uart[1].rx_pin);
    TEST_ASSERT_EQUAL_UINT32(115200U, g_idf.uart[1].baud);
    TEST_ASSERT_TRUE_MESSAGE(g_idf.pin[22].pull_up, "an absent chip's RX does not read as a stream of zeros");
    TEST_ASSERT_FALSE(g_idf.pin[22].pull_down);
}

TEST_CASE("open refuses a port that does not exist and one that is open", "[uart]")
{
    idf_fake_reset();
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_drv_uart_open(-1, 1, 2, 9600U));
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_drv_uart_open(UART_NUM_MAX, 1, 2, 9600U));
    TEST_ASSERT_EQUAL(KODE_OK, fck_drv_uart_open(1, 21, 22, 9600U));
    TEST_ASSERT_EQUAL(KODE_EXISTS, fck_drv_uart_open(1, 21, 22, 9600U));
}

TEST_CASE("a driver that will not install, or will not be set up, leaves nothing installed", "[uart]")
{
    idf_fake_reset();
    g_idf.uart_install_rc = ESP_FAIL;
    TEST_ASSERT_EQUAL(KODE_ERROR, fck_drv_uart_open(1, 21, 22, 9600U));
    TEST_ASSERT_FALSE(g_idf.uart[1].installed);

    idf_fake_reset();
    g_idf.uart_config_rc = ESP_FAIL;
    TEST_ASSERT_EQUAL(KODE_ERROR, fck_drv_uart_open(1, 21, 22, 9600U));
    TEST_ASSERT_FALSE_MESSAGE(g_idf.uart[1].installed, "the half-made driver is deleted");
    TEST_ASSERT_EQUAL_UINT(1U, g_idf.uart_deletes);

    idf_fake_reset();
    g_idf.uart_pin_rc = ESP_FAIL;
    TEST_ASSERT_EQUAL(KODE_ERROR, fck_drv_uart_open(1, 21, 22, 9600U));
    TEST_ASSERT_FALSE(g_idf.uart[1].installed);
}

TEST_CASE("close removes an open driver and ignores a port that is not", "[uart]")
{
    idf_fake_reset();
    TEST_ASSERT_EQUAL(KODE_OK, fck_drv_uart_open(2, 21, 22, 9600U));
    fck_drv_uart_close(2);
    TEST_ASSERT_FALSE(g_idf.uart[2].installed);
    TEST_ASSERT_EQUAL_UINT(1U, g_idf.uart_deletes);
    fck_drv_uart_close(2);
    fck_drv_uart_close(-1);
    fck_drv_uart_close(99);
    TEST_ASSERT_EQUAL_UINT(1U, g_idf.uart_deletes);
}

TEST_CASE("baud moves an open port and says when ESP-IDF would not", "[uart]")
{
    idf_fake_reset();
    TEST_ASSERT_EQUAL(KODE_OK, fck_drv_uart_open(1, 21, 22, 115200U));
    TEST_ASSERT_EQUAL(KODE_OK, fck_drv_uart_baud(1, 460800U));
    TEST_ASSERT_EQUAL_UINT32(460800U, g_idf.uart[1].baud);
    g_idf.uart_baud_rc = ESP_FAIL;
    TEST_ASSERT_EQUAL(KODE_ERROR, fck_drv_uart_baud(1, 9600U));
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_drv_uart_baud(7, 9600U));
}

TEST_CASE("read gives what has arrived, in order, up to the caller's room", "[uart]")
{
    idf_fake_reset();
    TEST_ASSERT_EQUAL(KODE_OK, fck_drv_uart_open(1, 21, 22, 115200U));
    idf_fake_uart_feed(1, "#C5INFO fw=x\n", 13U);
    uint8_t buf[8];
    TEST_ASSERT_EQUAL_UINT(8U, fck_drv_uart_read(1, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_MEMORY("#C5INFO ", buf, 8U);
    TEST_ASSERT_EQUAL_UINT(5U, fck_drv_uart_read(1, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_MEMORY("fw=x\n", buf, 5U);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, fck_drv_uart_read(1, buf, sizeof(buf)), "nothing more, nothing waited for");
}

TEST_CASE("read with nothing to read, a closed port, a bad port or no room is zero", "[uart]")
{
    idf_fake_reset();
    uint8_t buf[4];
    TEST_ASSERT_EQUAL_UINT(0U, fck_drv_uart_read_wait(1, buf, sizeof(buf), 5U));
    TEST_ASSERT_EQUAL_UINT(0U, fck_drv_uart_read_wait(9, buf, sizeof(buf), 5U));
    TEST_ASSERT_EQUAL(KODE_OK, fck_drv_uart_open(1, 21, 22, 115200U));
    idf_fake_uart_feed(1, "ab", 2U);
    TEST_ASSERT_EQUAL_UINT(0U, fck_drv_uart_read_wait(1, buf, 0U, 5U));
    TEST_ASSERT_EQUAL_UINT(1U, fck_drv_uart_read_wait(1, buf, 1U, 5U));
}

TEST_CASE("write sends every byte or says the line is short", "[uart]")
{
    idf_fake_reset();
    TEST_ASSERT_EQUAL(KODE_DENIED, fck_drv_uart_write(1, "info\n", 5U));
    TEST_ASSERT_EQUAL(KODE_OK, fck_drv_uart_open(1, 21, 22, 115200U));
    TEST_ASSERT_EQUAL(KODE_OK, fck_drv_uart_write(1, "info\n", 5U));
    TEST_ASSERT_EQUAL_UINT(5U, g_idf.uart[1].tx_n);
    TEST_ASSERT_EQUAL_MEMORY("info\n", g_idf.uart[1].tx, 5U);
    g_idf.uart_max_write = 3;
    TEST_ASSERT_EQUAL(KODE_IO_ERROR, fck_drv_uart_write(1, "reset\n", 6U));
}

TEST_CASE("flush drops what arrived and was not read", "[uart]")
{
    idf_fake_reset();
    TEST_ASSERT_EQUAL(KODE_OK, fck_drv_uart_open(1, 21, 22, 115200U));
    idf_fake_uart_feed(1, "stale", 5U);
    fck_drv_uart_flush(1);
    uint8_t buf[8];
    TEST_ASSERT_EQUAL_UINT(0U, fck_drv_uart_read(1, buf, sizeof(buf)));
    fck_drv_uart_flush(9);
    TEST_ASSERT_EQUAL_UINT(1U, g_idf.uart[1].flushes);
}
