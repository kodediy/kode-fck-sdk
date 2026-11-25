/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_cc1101_capture.c
 * @brief fck_drv_cc1101_record()'s capture: the edges GDO0's interrupt
 *        stamps, and the room they are turned into runs in.
 *
 * The fake raises no interrupt of its own. A case calls the handler the
 * driver added (idf_fake_gpio_edge()) from the hooks the fakes give - as RSSI
 * is read, and as the interrupt is disabled - so that an edge arrives at a
 * known step of the driver.
 */
#include <stdint.h>

#include "esp_timer.h"
#include "unity.h"

#include "fck_bsp.h"
#include "fck_drv_cc1101.h"

#include "fake_cc1101.h"
#include "fake_idf.h"

#define HZ_433      433920000U
#define RAW_LOUD    0x70U     /* 24 dB over the fake's quiet channel */
#define EDGE_GAP_US 80        /* longer than a glitch: every run is kept */

/* The driver's own steps, counted in RSSI readings: 32 for the noise on
 * arrival, then one a step while it waits for the power to rise, then one a
 * step of the capture. */
#define READ_RISE     34U     /* the carrier comes: the capture starts          */
#define READ_FILL     35U     /* the capture's first step                       */
#define READ_CUT      36U     /* its second                                     */

static int      s_gdo0;
static int      s_level;
static int64_t  s_cut_at;
static unsigned s_edges;

static void edge(void)
{
    s_level = (s_level == 0) ? 1 : 0;
    if (idf_fake_gpio_edge(s_gdo0, s_level)) {
        s_edges++;
    }
}

/* A signal that fills the buffer but for one edge, and then the cut. */
static void fill_then_cut(unsigned n)
{
    if (n == READ_RISE) {
        g_cc.peak_hz  = HZ_433;
        g_cc.peak_raw = RAW_LOUD;
    } else if (n == READ_FILL) {
        for (unsigned i = 0U; i < FCK_DRV_CC1101_EDGES - 1U; i++) {
            g_idf.skipped_us += EDGE_GAP_US;
            edge();
        }
    } else if (n == READ_CUT) {
        s_cut_at = esp_timer_get_time();
        fck_drv_cc1101_abort();
    } else {
        /* The noise on arrival, and the wait for the power to rise. */
    }
}

/* The last edge the buffer has room for, between the capture's last look at
 * the buffer and its interrupt going off: 100 us after the cut, whatever the
 * host took to get here. */
static void last_edge(int gpio)
{
    if (gpio != s_gdo0 || s_cut_at == 0) {
        return;
    }
    g_idf.skipped_us -= esp_timer_get_time() - (s_cut_at + 100);
    edge();
}

TEST_CASE("a capture cut with its buffer full gives no more runs than the buffer holds", "[cc1101][capture]")
{
    idf_fake_reset();
    fake_cc1101_reset(idf_fake_gpio(FCK_BSP_POSITION_CC_CS), idf_fake_gpio(FCK_BSP_POSITION_CC_MISO));
    fck_drv_cc1101_abort_clear();
    s_gdo0                = idf_fake_gpio(FCK_BSP_POSITION_CC_GDO0);
    s_level               = 0;
    s_cut_at              = 0;
    s_edges               = 0U;
    g_cc.on_rssi          = fill_then_cut;
    g_idf.intr_on_disable = last_edge;

    /* Room for more than the buffer holds, so that a run too many is seen
     * here and not only by the sanitizers. */
    static int32_t runs[FCK_DRV_CC1101_EDGES + 8U];
    size_t         n = 0U;
    TEST_ASSERT_EQUAL(KODE_OK, fck_drv_cc1101_record(idf_fake_hw(), NULL, HZ_433, 2000U, runs,
                                                    sizeof(runs) / sizeof(runs[0]), &n));
    fck_drv_cc1101_abort_clear();

    TEST_ASSERT_EQUAL_UINT_MESSAGE(FCK_DRV_CC1101_EDGES, s_edges, "every edge reached the driver's handler");
    TEST_ASSERT_TRUE_MESSAGE(n > 0U, "the capture was thrown away");
    TEST_ASSERT_TRUE_MESSAGE(n <= FCK_DRV_CC1101_EDGES, "a run was written past the buffer of edges");
    TEST_ASSERT_TRUE(g_cc.powered_down);
}
