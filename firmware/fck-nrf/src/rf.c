/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * fck-nrf - 2.4 GHz commands on the nRF54L15 RADIO, driven directly (no BT/MPSL/802.15.4).
 *
 * The module's radio limits are kept by this firmware itself and cannot be lifted
 * from outside: it transmits only inside its transmit band, never above its highest power, and
 * never an unmodulated carrier. Every one of those numbers is in fck_nrf_limits.h and nowhere
 * else; this file reads them from there. A channel, `ch`, is always MHz - 2400.
 *
 *   rf spec [ch_first=0] [ch_last=83] [dwell_us=200] [sweeps=1]
 *       per sweep: #SPEC24 f0=<MHz> step=1 n=<N> sweep=<i> v=<dBm,dBm,...>
 *       each value = PEAK (max) RSSI seen on that 1 MHz channel during dwell_us
 *   rf off                     radio idle: #RFOFF ok=1
 *   rf power <dbm>             TX power for ping/echo, snapped DOWN to a supported step and
 *                              never above the limit: #POWER24 req= dbm=
 *   rf echo <on|off> [ch=40]   (ch inside the transmit limits)
 *                              responder: answers every ping with the RSSI it measured
 *                              #ECHO24 on=1 ch= mhz= dbm= node= idle_s= / #ECHOSTAT24 ... /
 *                              #ECHO24 on=0. It stops by itself, radio off, when no ping has
 *                              reached it for idle_s seconds; #ECHOSTAT24 then says on=0 idle=1
 *   rf ping <n> [ch=40] [gap_ms=10]   (ch inside the transmit limits)
 *                              initiator: #PINGSTART24 ..., per packet
 *                              #PING24 seq= ok=1 rssi_fwd= rssi_rev=   (or ok=0 why=...)
 *                              then #PINGDONE24 ch= sent= ok= fwd_mean= rev_mean= ...
 *   rf sniff <ch> [ms=20000] [rssi_min=-60]   (ch inside the receive limits)
 *                              promiscuous capture of nRF24-style GFSK bursts (no address
 *                              known): #SNIFF24 i= rssi= len= data= per new distinct frame,
 *                              then #SNIFFTOP24 i= cnt= rssi= data= (most repeated first)
 *                              and #SNIFFDONE24 ch= rx= gated= uniq= ms=
 *   rf scfg [rate 1|2] [crc 0|2] [len]   sniffer PHY knobs: #SCFG24 ...
 *   rf saddr [balen] [base0 hex] [prefix0 hex]   sniffer match address: #SADDR24 ...
 *   every command ends with #DONE rf_<sub> rc=<0 ok|1 failed|2 not understood>; `rf` alone or
 *   with a sub-command that is not one of these ends with #DONE rf rc=2
 *
 * nRF54L15 specifics (from nrf/samples/peripheral/radio_test and nrfxlib nrf_802154_trx.c):
 *  - The RADIO needs HFXO: requested through the nrf clock-control onoff manager, then
 *    TASKS_PLLSTART (MLTPAN-20), as an earlier radio_test's clock_init() did. The radio_test of
 *    nRF Connect SDK v3.4.2 answers that anomaly with constant latency (NRF_POWER_TASK_CONSTLAT)
 *    instead, which this firmware does not ask for. The crystal is held only while a command
 *    uses the radio or the echo responder listens, and given back after.
 *  - There is NO RSSIEND event on nRF54L: RSSISTART takes one sample and RSSISAMPLE is read
 *    back (the 802.15.4 driver treats the sample as available immediately); the value needs
 *    ~15 us of RX (RSSI_SETTLE_TIME_US) after ramp-up to be valid. dBm = -RSSISAMPLE, raw
 *    (Nordic's small temperature/linearity correction for nRF53/54 is not applied).
 *  - TX packets are ended with PHYEND->DISABLE (END only means "EasyDMA done" on nRF54L; radio_test
 *    uses PHYEND for its END->DISABLE short on nRF54L too).
 *
 * Link packets (ping/echo): Nrf_1Mbit, 8-bit preamble, 5-byte address (BASE0 "KODE" + PREFIX0),
 * static 8-byte payload, no whitening, CRC-16 (0x11021, init 0xFFFF, address included).
 *   payload[0] type (0xA5 ping / 0x5A echo), [1..2] seq LE, [3] node id (FICR DEVICEID low byte),
 *   [4] RSSI the echoing side measured on the ping (-dBm, 0 in a ping), [5] sender TX dBm (int8),
 *   [6..7] 'K','F' magic.
 * Semantics mirror the CC1101 link test: the echo carries the
 * peer's measurement of our ping (forward), and we measure its echo ourselves (reverse).
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/irq.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/util.h>
#include <zephyr/drivers/clock_control.h>
#include <zephyr/drivers/clock_control/nrf_clock_control.h>

#include <hal/nrf_radio.h>
#include <hal/nrf_clock.h>

#include "fck_nrf.h"
#include "fck_nrf_limits.h"

/* ===== Constants ============================================================ */

#define RADIO_BASE_MHZ     2400  /* RADIO.FREQUENCY counts MHz from here: ch = MHz - 2400 */
#define RADIO_CH_MAX       100   /* the highest FREQUENCY the RADIO takes */
#define CH_LINK_DEFAULT    40    /* ping / echo when no channel is given */
#define CH_SPEC_LAST       83    /* where `rf spec` stops when it is not told */

/* The limits as channels. The numbers themselves are in fck_nrf_limits.h. */
#define CH_TX_MIN ((int)fck_nrf_limits.tx_mhz_min - RADIO_BASE_MHZ)
#define CH_TX_MAX ((int)fck_nrf_limits.tx_mhz_max - RADIO_BASE_MHZ)
#define CH_RX_MIN ((int)fck_nrf_limits.rx_mhz_min - RADIO_BASE_MHZ)
#define CH_RX_MAX ((int)fck_nrf_limits.rx_mhz_max - RADIO_BASE_MHZ)

#define RSSI_SETTLE_US     15    /* nrf_802154_trx.c RSSI_SETTLE_TIME_US */
#define RSSI_STEP_US       4     /* spacing of RSSISTART samples inside the dwell */
#define RAMP_TIMEOUT_US    500   /* READY must come well before this (fast ramp-up ~40 us) */
#define DISABLE_TIMEOUT_US 500

#define PKT_LEN            8
#define PKT_PING           0xA5
#define PKT_ECHO           0x5A
#define PKT_MAGIC0         'K'
#define PKT_MAGIC1         'F'
#define LINK_BASE0         0x4B4F4445UL   /* "KODE" */
#define LINK_PREFIX0       0xA7UL

#define ECHO_DELAY_US      300   /* responder waits before replying: the initiator's RX ramp-up
				  * (thread wake-up + ~40 us) must finish before our preamble */
#define ECHO_RX_SLICE_MS   100   /* responder re-checks its on/off flag at least this often */
#define ECHO_IDLE_S        60    /* responder stops by itself after this long without a ping */
#define PING_RX_TIMEOUT_MS 20
#define ALIVE_SLICE_MS     200   /* a command that waits says it is alive at least this often */

/* How long one link packet is on the air, and when its transmitter must be off again. On the air:
 * preamble 1 + address 5 (BASE0 + PREFIX0) + payload + CRC 2 bytes at 1 Mbit/s = 128 us. Before
 * it, the TX ramp-up (fast, about 40 us). After that the RADIO has disabled itself
 * (PHYEND->DISABLE); one that has not, fck_nrf_limits.tx_overrun_us later, is switched off. */
#define LINK_AIR_US        ((1 + 5 + PKT_LEN + 2) * 8)
#define TX_RAMP_US         40
#define TX_DEADLINE_US     (TX_RAMP_US + LINK_AIR_US + fck_nrf_limits.tx_overrun_us)

/* ===== State ================================================================ */

static K_MUTEX_DEFINE(s_lock);        /* owner of the RADIO (shell thread or echo thread) */
static K_SEM_DEFINE(s_irq_sem, 0, 1); /* RADIO_0 IRQ -> waiting thread */

static struct onoff_client s_clk_cli;
static bool s_clk_held;               /* the crystal is requested and running (under s_lock) */

static int s_link_ch;                 /* channel link_config() last tuned to */
static int8_t s_dbm;                  /* applied TX power, dBm */
static uint16_t s_pwr_code;           /* TXPOWER register code for s_dbm */

static volatile bool s_echo_on;
static bool s_echo_idle;              /* the responder stopped by itself, for want of pings */
static int64_t s_echo_heard_ms;       /* uptime when it was started or last answered a ping */
static int s_echo_ch;
static struct {
	uint32_t rx, crc_bad, replied, foreign;
} s_echo_stat;

static uint32_t s_tx_slow;            /* TX that had to be switched off (see link_tx) */

static uint8_t s_txbuf[PKT_LEN] __aligned(4);
static uint8_t s_rxbuf[PKT_LEN] __aligned(4);
static char s_line[1024];             /* one #SPEC24 line (101 x "-127," max = ~620 chars) */

/* ===== Promiscuous sniffer (rf sniff) ======================================== *
 * Capture a proprietary nRF24-style GFSK burst without knowing its address: lock the
 * RADIO's "address" onto the preamble run (default 3 x 0xAA, CRC off) so END fires on
 * anything, and dump STATLEN raw bytes. Real button frames repeat many times and come
 * in strong (a remote close by is around -24..-33 dBm); noise locks are one-off and
 * weak, so an RSSI gate plus a repeat-count table separate them. Everything is runtime
 * settable (rf scfg / rf saddr), so it is tuned over the air without reflashing. */
#define SNIFF_MAX   40        /* bytes captured per frame (STATLEN cap) */
#define SNIFF_TBL   48        /* distinct payloads remembered per run */

static uint8_t s_snbuf[SNIFF_MAX] __aligned(4);
static int      s_sn_rate   = 1;             /* 1 or 2 Mbit GFSK */
static int      s_sn_crc    = 0;             /* 0 = off, 2 = CRC-16 (0x11021) */
static int      s_sn_len    = 32;            /* STATLEN bytes dumped per frame */
static int      s_sn_balen  = 2;             /* BASE bytes (2..4); match addr = balen+1 */
static uint32_t s_sn_base   = 0xAAAAAAAAUL;  /* BASE0 register (preamble run) */
static uint8_t  s_sn_prefix = 0xAA;          /* PREFIX0 register */

static uint8_t  s_sn_tbl[SNIFF_TBL][SNIFF_MAX];
static uint16_t s_sn_cnt[SNIFF_TBL];
static int8_t   s_sn_best[SNIFF_TBL];
static int      s_sn_ntbl;

/* ===== TX power table (nRF54L15 RADIO.TXPOWER) ================================ */

static const struct {
	int8_t dbm;
	uint16_t code;
} k_pwr[] = {
	/* Every step the nRF54L15 has, highest first. How high the firmware goes is not decided
	 * here: pwr_snap() stops at fck_nrf_limits.tx_dbm_max. */
	{8, RADIO_TXPOWER_TXPOWER_Pos8dBm},   {7, RADIO_TXPOWER_TXPOWER_Pos7dBm},
	{6, RADIO_TXPOWER_TXPOWER_Pos6dBm},   {5, RADIO_TXPOWER_TXPOWER_Pos5dBm},
	{4, RADIO_TXPOWER_TXPOWER_Pos4dBm},   {3, RADIO_TXPOWER_TXPOWER_Pos3dBm},
	{2, RADIO_TXPOWER_TXPOWER_Pos2dBm},   {1, RADIO_TXPOWER_TXPOWER_Pos1dBm},
	{0, RADIO_TXPOWER_TXPOWER_0dBm},      {-1, RADIO_TXPOWER_TXPOWER_Neg1dBm},
	{-2, RADIO_TXPOWER_TXPOWER_Neg2dBm},  {-3, RADIO_TXPOWER_TXPOWER_Neg3dBm},
	{-4, RADIO_TXPOWER_TXPOWER_Neg4dBm},  {-5, RADIO_TXPOWER_TXPOWER_Neg5dBm},
	{-6, RADIO_TXPOWER_TXPOWER_Neg6dBm},  {-7, RADIO_TXPOWER_TXPOWER_Neg7dBm},
	{-8, RADIO_TXPOWER_TXPOWER_Neg8dBm},  {-9, RADIO_TXPOWER_TXPOWER_Neg9dBm},
	{-10, RADIO_TXPOWER_TXPOWER_Neg10dBm}, {-12, RADIO_TXPOWER_TXPOWER_Neg12dBm},
	{-14, RADIO_TXPOWER_TXPOWER_Neg14dBm}, {-16, RADIO_TXPOWER_TXPOWER_Neg16dBm},
	{-18, RADIO_TXPOWER_TXPOWER_Neg18dBm}, {-20, RADIO_TXPOWER_TXPOWER_Neg20dBm},
	/* -22 dBm shares code 0x002 with -20 in the MDK, so it is not a distinct step */
	{-28, RADIO_TXPOWER_TXPOWER_Neg28dBm}, {-40, RADIO_TXPOWER_TXPOWER_Neg40dBm},
	{-46, RADIO_TXPOWER_TXPOWER_Neg46dBm},
};

#define PWR_DBM_MIN (k_pwr[ARRAY_SIZE(k_pwr) - 1].dbm)

/* Largest supported step <= requested (never transmit more than asked) and never above the
 * limit; clamps at both ends. */
static int8_t pwr_snap(int req, uint16_t *code)
{
	req = MIN(req, (int)fck_nrf_limits.tx_dbm_max);
	for (size_t i = 0; i < ARRAY_SIZE(k_pwr); i++) {
		if (k_pwr[i].dbm <= req) {
			*code = k_pwr[i].code;
			return k_pwr[i].dbm;
		}
	}
	*code = k_pwr[ARRAY_SIZE(k_pwr) - 1].code;
	return k_pwr[ARRAY_SIZE(k_pwr) - 1].dbm;
}

/* ===== Helpers ================================================================ */

static bool parse_int(const char *s, long lo, long hi, int *out)
{
	char *end;
	long v;

	if (s == NULL || *s == '\0') {
		return false;
	}
	v = strtol(s, &end, 0);
	if (*end != '\0' || v < lo || v > hi) {
		return false;
	}
	*out = (int)v;
	return true;
}

/* n bytes -> lowercase hex into b (needs 2*n+1). */
static void to_hex(const uint8_t *src, int n, char *b)
{
	static const char h[] = "0123456789abcdef";

	for (int i = 0; i < n; i++) {
		b[2 * i] = h[src[i] >> 4];
		b[2 * i + 1] = h[src[i] & 0xF];
	}
	b[2 * n] = '\0';
}

/* Sleep `ms` in slices, saying between them that the command is alive (see fck_alive()). */
static void sleep_alive(int ms)
{
	while (ms > 0) {
		int slice = MIN(ms, ALIVE_SLICE_MS);

		k_msleep(slice);
		ms -= slice;
		fck_alive();
	}
}

/* sum/cnt with one decimal, no float printf needed: "-45.3" */
static void fmt_mean(char *b, size_t n, int32_t sum, int cnt)
{
	if (cnt <= 0) {
		snprintf(b, n, "0.0");
		return;
	}
	int32_t t = (sum * 10 + (sum < 0 ? -cnt / 2 : cnt / 2)) / cnt;
	int32_t a = t < 0 ? -t : t;

	snprintf(b, n, "%s%d.%d", t < 0 ? "-" : "", (int)(a / 10), (int)(a % 10));
}

/* ===== Clock =================================================================== */

/* The RADIO needs the crystal (HFXO) and nothing else in this firmware does. It is held while
 * a command uses the radio or the echo responder listens, and given back after, so that an idle
 * firmware does not keep it running. Both functions are called with s_lock held: whoever owns
 * the RADIO owns the crystal with it.
 *
 * hfxo_hold() never blocks more than ~100 ms, so a dead crystal cannot hang the shell; a request
 * that has not come through by then is withdrawn. PLLSTART is for MLTPAN-20, as radio_test did.
 * Nothing here undoes it: the clock driver stops the PLL when it stops the crystal (nrfx's
 * clock_stop(): XOSTOP, then PLLSTOP on this chip). */
static int hfxo_hold(void)
{
	struct onoff_manager *mgr = z_nrf_clock_control_get_onoff(CLOCK_CONTROL_NRF_SUBSYS_HF);
	int res = 0;
	int err;

	if (s_clk_held) {
		return 0;
	}
	if (mgr == NULL) {
		return -ENODEV;
	}
	sys_notify_init_spinwait(&s_clk_cli.notify);
	err = onoff_request(mgr, &s_clk_cli);
	if (err < 0) {
		return err;
	}
	for (int i = 0; i < 1000; i++) {
		err = sys_notify_fetch_result(&s_clk_cli.notify, &res);
		if (err == 0) {
			break;
		}
		k_busy_wait(100);
	}
	if (err != 0) {
		(void)onoff_cancel_or_release(mgr, &s_clk_cli);
		return -ETIMEDOUT;
	}
	if (res != 0) {
		return res;
	}
#if defined(NRF54L15_XXAA)
	nrf_clock_task_trigger(NRF_CLOCK, NRF_CLOCK_TASK_PLLSTART); /* MLTPAN-20 */
#endif
	s_clk_held = true;
	return 0;
}

/* Give the crystal back. The RADIO must be DISABLED. */
static void hfxo_drop(void)
{
	if (!s_clk_held) {
		return;
	}
	(void)onoff_release(z_nrf_clock_control_get_onoff(CLOCK_CONTROL_NRF_SUBSYS_HF));
	s_clk_held = false;
}

/* ===== RADIO primitives ======================================================== */

static void radio_isr(const void *arg)
{
	ARG_UNUSED(arg);
	nrf_radio_int_disable(NRF_RADIO, 0xFFFFFFFFUL);
	k_sem_give(&s_irq_sem);
}

static bool wait_event_us(nrf_radio_event_t ev, uint32_t us)
{
	for (uint32_t i = 0; i < us; i++) {
		if (nrf_radio_event_check(NRF_RADIO, ev)) {
			return true;
		}
		k_busy_wait(1);
	}
	return nrf_radio_event_check(NRF_RADIO, ev);
}

static void radio_disable(void)
{
	nrf_radio_shorts_set(NRF_RADIO, 0);
	nrf_radio_int_disable(NRF_RADIO, 0xFFFFFFFFUL);
	if (nrf_radio_state_get(NRF_RADIO) != NRF_RADIO_STATE_DISABLED) {
		nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_DISABLED);
		nrf_radio_task_trigger(NRF_RADIO, NRF_RADIO_TASK_DISABLE);
		(void)wait_event_us(NRF_RADIO_EVENT_DISABLED, DISABLE_TIMEOUT_US);
	}
	nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_DISABLED);
}

static void events_clear(void)
{
	nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_READY);
	nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_ADDRESS);
	nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_END);
	nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_PHYEND);
	nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_CRCOK);
	nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_CRCERROR);
	nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_DISABLED);
}

/* Link PHY + packet format on channel `ch`; leaves the radio DISABLED. */
static void link_config(int ch)
{
	nrf_radio_packet_conf_t pc;

	radio_disable();
	nrf_radio_mode_set(NRF_RADIO, NRF_RADIO_MODE_NRF_1MBIT);
	nrf_radio_fast_ramp_up_enable_set(NRF_RADIO, true);
	nrf_radio_txpower_set(NRF_RADIO, (nrf_radio_txpower_t)s_pwr_code);
	nrf_radio_frequency_set(NRF_RADIO, (uint16_t)(RADIO_BASE_MHZ + ch));
	s_link_ch = ch;

	memset(&pc, 0, sizeof(pc));
	pc.lflen = 0;
	pc.s0len = 0;
	pc.s1len = 0;
	pc.plen = NRF_RADIO_PREAMBLE_LENGTH_8BIT;
	pc.maxlen = PKT_LEN;
	pc.statlen = PKT_LEN;
	pc.balen = 4;
	pc.big_endian = false;
	pc.whiteen = false;
	nrf_radio_packet_configure(NRF_RADIO, &pc);

	nrf_radio_base0_set(NRF_RADIO, LINK_BASE0);
	nrf_radio_prefix0_set(NRF_RADIO, LINK_PREFIX0);
	nrf_radio_txaddress_set(NRF_RADIO, 0);
	nrf_radio_rxaddresses_set(NRF_RADIO, 1);
	nrf_radio_crc_configure(NRF_RADIO, RADIO_CRCCNF_LEN_Two, NRF_RADIO_CRC_ADDR_INCLUDE, 0x11021UL);
	nrf_radio_crcinit_set(NRF_RADIO, 0xFFFFUL);
}

/* Send s_txbuf (radio must be DISABLED and link-configured). Ends DISABLED.
 * This is the one place the transmitter is switched on, so it is also where the limits are
 * looked at last: a channel or a power outside them is not sent, whoever asked. */
static int link_tx(void)
{
	if (s_link_ch < CH_TX_MIN || s_link_ch > CH_TX_MAX || s_dbm > fck_nrf_limits.tx_dbm_max) {
		return -EPERM;
	}
	nrf_radio_packetptr_set(NRF_RADIO, s_txbuf);
	events_clear();
	nrf_radio_shorts_set(NRF_RADIO,
			     NRF_RADIO_SHORT_READY_START_MASK | NRF_RADIO_SHORT_PHYEND_DISABLE_MASK);
	k_sem_reset(&s_irq_sem);
	nrf_radio_int_enable(NRF_RADIO, NRF_RADIO_INT_DISABLED_MASK);
	nrf_radio_task_trigger(NRF_RADIO, NRF_RADIO_TASK_TXEN);
	if (k_sem_take(&s_irq_sem, K_USEC(TX_DEADLINE_US)) != 0) {
		/* No PHYEND->DISABLE by the time the packet has long left the air. A transmitter
		 * that has sent its packet and is not disabled idles on an unmodulated carrier, so
		 * it is switched off now, not waited for. If END (EasyDMA done) happened the packet
		 * did go out: count it as sent but slow, so a PHYEND quirk shows up as txslow=
		 * instead of killing the link test. */
		bool sent = nrf_radio_event_check(NRF_RADIO, NRF_RADIO_EVENT_END);

		radio_disable();
		if (sent) {
			s_tx_slow++;
			return 0;
		}
		return -ETIMEDOUT;
	}
	nrf_radio_shorts_set(NRF_RADIO, 0);
	return 0;
}

/* Receive one packet into s_rxbuf within `to`. RSSI is sampled at ADDRESS (hardware short).
 * Returns 0 with *crc_ok / *rssi_dbm filled, or -ETIMEDOUT. Ends DISABLED. */
static int link_rx(k_timeout_t to, bool *crc_ok, int *rssi_dbm)
{
	nrf_radio_packetptr_set(NRF_RADIO, s_rxbuf);
	events_clear();
	nrf_radio_shorts_set(NRF_RADIO,
			     NRF_RADIO_SHORT_READY_START_MASK | NRF_RADIO_SHORT_ADDRESS_RSSISTART_MASK);
	k_sem_reset(&s_irq_sem);
	nrf_radio_int_enable(NRF_RADIO, NRF_RADIO_INT_END_MASK);
	nrf_radio_task_trigger(NRF_RADIO, NRF_RADIO_TASK_RXEN);
	if (k_sem_take(&s_irq_sem, to) != 0) {
		radio_disable();
		return -ETIMEDOUT;
	}
	/* END = payload in RAM; the CRC verdict comes with it or right after. */
	(void)wait_event_us(NRF_RADIO_EVENT_CRCOK, 50);
	*crc_ok = nrf_radio_event_check(NRF_RADIO, NRF_RADIO_EVENT_CRCOK);
	*rssi_dbm = -(int)nrf_radio_rssi_sample_get(NRF_RADIO);
	radio_disable();
	return 0;
}

/* Peak RSSI on one channel over dwell_us. Radio mode already set. Ends in RX (caller disables). */
static int spec_one(int ch, uint32_t dwell_us, bool *ok)
{
	uint8_t best = 127;
	uint32_t n = dwell_us / RSSI_STEP_US;

	radio_disable();
	nrf_radio_frequency_set(NRF_RADIO, (uint16_t)(RADIO_BASE_MHZ + ch));
	nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_READY);
	nrf_radio_shorts_set(NRF_RADIO, NRF_RADIO_SHORT_READY_START_MASK);
	nrf_radio_task_trigger(NRF_RADIO, NRF_RADIO_TASK_RXEN);
	(void)wait_event_us(NRF_RADIO_EVENT_READY, RAMP_TIMEOUT_US);

	nrf_radio_state_t st = nrf_radio_state_get(NRF_RADIO);

	*ok = (st == NRF_RADIO_STATE_RX || st == NRF_RADIO_STATE_RXIDLE);
	if (!*ok) {
		return -127;
	}
	k_busy_wait(RSSI_SETTLE_US);
	if (n == 0) {
		n = 1;
	}
	for (uint32_t i = 0; i < n; i++) {
		nrf_radio_task_trigger(NRF_RADIO, NRF_RADIO_TASK_RSSISTART);
		k_busy_wait(RSSI_STEP_US);
		uint8_t s = nrf_radio_rssi_sample_get(NRF_RADIO);

		if (s < best) {
			best = s;
		}
	}
	return -(int)best;
}

/* Take the RADIO: stops the echo responder (like `ping` does on the CC1101). The crystal the
 * responder held, if it was listening, stays held and is now the caller's. */
static void radio_take(void)
{
	s_echo_on = false;
	k_mutex_lock(&s_lock, K_FOREVER);
}

static void radio_give(void)
{
	k_mutex_unlock(&s_lock);
}

/* The end of every use of the RADIO but the responder's listening: radio off, crystal given
 * back, RADIO free. */
static void radio_release(void)
{
	radio_disable();
	hfxo_drop();
	radio_give();
}

/* ===== Echo responder thread ================================================= */

static void echo_thread(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	for (;;) {
		if (!s_echo_on) {
			k_sleep(K_MSEC(20));
			continue;
		}
		k_mutex_lock(&s_lock, K_FOREVER);
		if (!s_echo_on) {
			k_mutex_unlock(&s_lock);
			continue;
		}
		bool crc_ok = false;
		int rssi = 0;

		if (link_rx(K_MSEC(ECHO_RX_SLICE_MS), &crc_ok, &rssi) == 0) {
			s_echo_stat.rx++;
			if (!crc_ok) {
				s_echo_stat.crc_bad++;
			} else if (s_rxbuf[0] == PKT_PING && s_rxbuf[6] == PKT_MAGIC0 &&
				   s_rxbuf[7] == PKT_MAGIC1) {
				s_txbuf[0] = PKT_ECHO;
				s_txbuf[1] = s_rxbuf[1];
				s_txbuf[2] = s_rxbuf[2];
				s_txbuf[3] = fck_node_id();
				s_txbuf[4] = (uint8_t)CLAMP(-rssi, 0, 127);
				s_txbuf[5] = (uint8_t)s_dbm;
				s_txbuf[6] = PKT_MAGIC0;
				s_txbuf[7] = PKT_MAGIC1;
				k_busy_wait(ECHO_DELAY_US);
				if (link_tx() == 0) {
					s_echo_stat.replied++;
				}
				s_echo_heard_ms = k_uptime_get();
			} else {
				s_echo_stat.foreign++;
			}
		}
		/* Nobody has pinged it for a while: it stops by itself. link_rx() and link_tx()
		 * left the RADIO disabled, so giving the crystal back is all there is to do.
		 * (Stopped by a command instead, s_echo_on is already false and the crystal goes
		 * to that command.) */
		if (s_echo_on && k_uptime_get() - s_echo_heard_ms >= ECHO_IDLE_S * MSEC_PER_SEC) {
			s_echo_on = false;
			s_echo_idle = true;
			hfxo_drop();
		}
		k_mutex_unlock(&s_lock);
	}
}

K_THREAD_DEFINE(rf_echo_tid, 1536, echo_thread, NULL, NULL, NULL, 5, 0, 0);

/* ===== Shell commands =========================================================== */

/* One usage line per sub-command: its own bad arguments and rf_command() print the same. */
static void usage_spec(const struct shell *sh)
{
	shell_print(sh, "usage: rf spec [ch_first %d-%d] [ch_last %d-%d] [dwell_us 10-100000] "
			"[sweeps 1-1000]   (ch = MHz - %d)",
		    CH_RX_MIN, CH_RX_MAX, CH_RX_MIN, CH_RX_MAX, RADIO_BASE_MHZ);
}

static void usage_off(const struct shell *sh)
{
	shell_print(sh, "usage: rf off");
}

static void usage_power(const struct shell *sh)
{
	shell_print(sh, "usage: rf power <dbm>   (%d..%+d, snapped down to a step)", PWR_DBM_MIN,
		    fck_nrf_limits.tx_dbm_max);
}

static void usage_echo(const struct shell *sh)
{
	shell_print(sh, "usage: rf echo <on|off> [ch %d-%d]   (no args: statistics)", CH_TX_MIN,
		    CH_TX_MAX);
}

static void usage_ping(const struct shell *sh)
{
	shell_print(sh, "usage: rf ping <n 1-1000> [ch %d-%d] [gap_ms 0-1000]", CH_TX_MIN,
		    CH_TX_MAX);
}

static void usage_sniff(const struct shell *sh)
{
	shell_print(sh, "usage: rf sniff <ch %d-%d> [ms 100-600000] [rssi_min -127..0]   "
			"(ch = MHz - %d; promiscuous, tune with rf scfg / rf saddr)",
		    CH_RX_MIN, CH_RX_MAX, RADIO_BASE_MHZ);
}

static void usage_scfg(const struct shell *sh)
{
	shell_print(sh, "usage: rf scfg [rate 1|2] [crc 0|2] [len 1-%d]", SNIFF_MAX);
}

static void usage_saddr(const struct shell *sh)
{
	shell_print(sh, "usage: rf saddr [balen 2-4] [base0 hex32] [prefix0 hex8]");
}

/* The start of every command that uses the RADIO: take it (the echo responder stops) and hold
 * the crystal. When the crystal does not come it prints #ERR + #DONE, gives the RADIO back and
 * returns false. A command that got true ends with radio_release(), or, `rf echo on`, leaves
 * the crystal to the responder with radio_give(). */
static bool radio_claim(const struct shell *sh, const char *cmd)
{
	int err;

	radio_take();
	err = hfxo_hold();
	if (err) {
		radio_give();
		shell_print(sh, "#ERR cmd=%s why=hfxo err=%d", cmd, err);
		(void)fck_done(sh, cmd, RC_FAIL);
		return false;
	}
	return true;
}

static int cmd_rf_spec(const struct shell *sh, size_t argc, char **argv)
{
	int ch0 = CH_RX_MIN, ch1 = CLAMP(CH_SPEC_LAST, CH_RX_MIN, CH_RX_MAX);
	int dwell = 200, sweeps = 1;

	if ((argc > 1 && !parse_int(argv[1], CH_RX_MIN, CH_RX_MAX, &ch0)) ||
	    (argc > 2 && !parse_int(argv[2], CH_RX_MIN, CH_RX_MAX, &ch1)) ||
	    (argc > 3 && !parse_int(argv[3], 10, 100000, &dwell)) ||
	    (argc > 4 && !parse_int(argv[4], 1, 1000, &sweeps)) || ch1 < ch0) {
		usage_spec(sh);
		return fck_done(sh, "rf_spec", RC_USAGE);
	}
	if (!radio_claim(sh, "rf_spec")) {
		return 0;
	}

	int n = ch1 - ch0 + 1;
	int not_rx = 0;

	radio_disable();
	nrf_radio_mode_set(NRF_RADIO, NRF_RADIO_MODE_BLE_1MBIT);
	nrf_radio_fast_ramp_up_enable_set(NRF_RADIO, true);
	for (int s = 0; s < sweeps; s++) {
		size_t k = snprintf(s_line, sizeof(s_line), "#SPEC24 f0=%d step=1 n=%d sweep=%d v=",
				    RADIO_BASE_MHZ + ch0, n, s);

		for (int ch = ch0; ch <= ch1; ch++) {
			bool ok;
			int dbm = spec_one(ch, (uint32_t)dwell, &ok);

			fck_alive(); /* a store, between two points: nothing of a point is timed here */
			if (!ok) {
				not_rx++;
			}
			if (k < sizeof(s_line)) {
				k += snprintf(s_line + k, sizeof(s_line) - k, "%s%d",
					      ch == ch0 ? "" : ",", dbm);
			}
		}
		radio_disable();
		shell_print(sh, "%s", s_line);
	}
	radio_release();
	if (not_rx) {
		shell_print(sh, "#ERR cmd=rf_spec why=not_in_rx points=%d", not_rx);
	}
	return fck_done(sh, "rf_spec", not_rx ? RC_FAIL : RC_OK);
}

static int cmd_rf_off(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	radio_take();
	radio_release();
	shell_print(sh, "#RFOFF ok=1");
	return fck_done(sh, "rf_off", RC_OK);
}

static int cmd_rf_power(const struct shell *sh, size_t argc, char **argv)
{
	int req;

	if (argc < 2 || !parse_int(argv[1], -100, 100, &req)) {
		usage_power(sh);
		return fck_done(sh, "rf_power", RC_USAGE);
	}
	uint16_t code;
	int8_t applied = pwr_snap(req, &code);

	k_mutex_lock(&s_lock, K_FOREVER);
	s_dbm = applied;
	s_pwr_code = code;
	nrf_radio_txpower_set(NRF_RADIO, (nrf_radio_txpower_t)s_pwr_code);
	k_mutex_unlock(&s_lock);
	shell_print(sh, "#POWER24 req=%d dbm=%d", req, applied);
	return fck_done(sh, "rf_power", RC_OK);
}

static void echo_stat_print(const struct shell *sh)
{
	/* idle=1: it is off because it stopped by itself, no ping having reached it in time. */
	shell_print(sh,
		    "#ECHOSTAT24 on=%d ch=%d rx=%u crcbad=%u replied=%u foreign=%u txslow=%u idle=%d",
		    s_echo_on, s_echo_ch, s_echo_stat.rx, s_echo_stat.crc_bad, s_echo_stat.replied,
		    s_echo_stat.foreign, s_tx_slow, !s_echo_on && s_echo_idle);
}

static int cmd_rf_echo(const struct shell *sh, size_t argc, char **argv)
{
	int ch = CH_LINK_DEFAULT;

	if (argc < 2) {
		echo_stat_print(sh);
		return fck_done(sh, "rf_echo", RC_OK);
	}
	if (!strcmp(argv[1], "off")) {
		radio_take();
		radio_release();
		echo_stat_print(sh);
		shell_print(sh, "#ECHO24 on=0");
		return fck_done(sh, "rf_echo", RC_OK);
	}
	if (strcmp(argv[1], "on") || (argc > 2 && !parse_int(argv[2], CH_TX_MIN, CH_TX_MAX, &ch))) {
		usage_echo(sh);
		return fck_done(sh, "rf_echo", RC_USAGE);
	}
	if (!radio_claim(sh, "rf_echo")) {
		return 0;
	}
	link_config(ch);
	memset(&s_echo_stat, 0, sizeof(s_echo_stat));
	s_echo_ch = ch;
	s_echo_idle = false;
	s_echo_heard_ms = k_uptime_get();
	s_echo_on = true;
	radio_give(); /* the crystal stays held: it is the responder's until it stops */
	shell_print(sh, "#ECHO24 on=1 ch=%d mhz=%d dbm=%d node=%02X idle_s=%d", ch,
		    RADIO_BASE_MHZ + ch, s_dbm, fck_node_id(), ECHO_IDLE_S);
	return fck_done(sh, "rf_echo", RC_OK);
}

static int cmd_rf_ping(const struct shell *sh, size_t argc, char **argv)
{
	int n, ch = CH_LINK_DEFAULT, gap = 10;

	if (argc < 2 || !parse_int(argv[1], 1, 1000, &n) ||
	    (argc > 2 && !parse_int(argv[2], CH_TX_MIN, CH_TX_MAX, &ch)) ||
	    (argc > 3 && !parse_int(argv[3], 0, 1000, &gap))) {
		usage_ping(sh);
		return fck_done(sh, "rf_ping", RC_USAGE);
	}
	if (!radio_claim(sh, "rf_ping")) {
		return 0;
	}

	int ok = 0, timeouts = 0, crcbad = 0, mismatch = 0, txfail = 0;
	int32_t fwd_sum = 0, rev_sum = 0;
	int fwd_min = 0, fwd_max = 0, rev_min = 0, rev_max = 0;
	uint8_t peer = 0;
	int peer_dbm = 0;
	uint8_t node = fck_node_id();
	uint32_t slow0 = s_tx_slow;

	link_config(ch);
	shell_print(sh, "#PINGSTART24 n=%d ch=%d mhz=%d dbm=%d node=%02X timeout_ms=%d", n, ch,
		    RADIO_BASE_MHZ + ch, s_dbm, node, PING_RX_TIMEOUT_MS);
	for (int i = 1; i <= n; i++) {
		fck_alive();
		s_txbuf[0] = PKT_PING;
		s_txbuf[1] = (uint8_t)(i & 0xFF);
		s_txbuf[2] = (uint8_t)(i >> 8);
		s_txbuf[3] = node;
		s_txbuf[4] = 0;
		s_txbuf[5] = (uint8_t)s_dbm;
		s_txbuf[6] = PKT_MAGIC0;
		s_txbuf[7] = PKT_MAGIC1;
		if (link_tx() != 0) {
			txfail++;
			shell_print(sh, "#PING24 seq=%d ok=0 why=txfail", i);
			continue;
		}
		bool crc_ok = false;
		int rssi = 0;

		if (link_rx(K_MSEC(PING_RX_TIMEOUT_MS), &crc_ok, &rssi) != 0) {
			timeouts++;
			shell_print(sh, "#PING24 seq=%d ok=0 why=timeout", i);
		} else if (!crc_ok) {
			crcbad++;
			shell_print(sh, "#PING24 seq=%d ok=0 why=crc rssi_rev=%d", i, rssi);
		} else if (s_rxbuf[0] != PKT_ECHO || s_rxbuf[1] != (uint8_t)(i & 0xFF) ||
			   s_rxbuf[2] != (uint8_t)(i >> 8) || s_rxbuf[6] != PKT_MAGIC0 ||
			   s_rxbuf[7] != PKT_MAGIC1) {
			mismatch++;
			shell_print(sh, "#PING24 seq=%d ok=0 why=mismatch type=0x%02X seq_rx=%u", i,
				    s_rxbuf[0], (unsigned int)(s_rxbuf[1] | (s_rxbuf[2] << 8)));
		} else {
			int fwd = -(int)s_rxbuf[4];

			if (ok == 0 || fwd < fwd_min) {
				fwd_min = fwd;
			}
			if (ok == 0 || fwd > fwd_max) {
				fwd_max = fwd;
			}
			if (ok == 0 || rssi < rev_min) {
				rev_min = rssi;
			}
			if (ok == 0 || rssi > rev_max) {
				rev_max = rssi;
			}
			ok++;
			fwd_sum += fwd;
			rev_sum += rssi;
			peer = s_rxbuf[3];
			peer_dbm = (int8_t)s_rxbuf[5];
			shell_print(sh, "#PING24 seq=%d ok=1 rssi_fwd=%d rssi_rev=%d", i, fwd, rssi);
		}
		sleep_alive(gap);
	}
	radio_release();

	char fm[16], rm[16];

	fmt_mean(fm, sizeof(fm), fwd_sum, ok);
	fmt_mean(rm, sizeof(rm), rev_sum, ok);
	shell_print(sh,
		    "#PINGDONE24 ch=%d sent=%d ok=%d fwd_mean=%s rev_mean=%s mhz=%d dbm=%d "
		    "timeout=%d crcbad=%d mismatch=%d txfail=%d fwd_min=%d fwd_max=%d rev_min=%d "
		    "rev_max=%d peer=%02X peer_dbm=%d txslow=%u",
		    ch, n, ok, fm, rm, RADIO_BASE_MHZ + ch, s_dbm, timeouts, crcbad, mismatch, txfail,
		    fwd_min, fwd_max, rev_min, rev_max, peer, peer_dbm, s_tx_slow - slow0);
	return fck_done(sh, "rf_ping", ok ? RC_OK : RC_FAIL);
}

/* Promiscuous PHY + packet format on channel `ch`; leaves the radio DISABLED. Mirrors
 * link_config but uses the rf scfg / rf saddr knobs: variable rate, CRC on/off, STATLEN,
 * and a free match address so we can sit on the preamble run. */
static void sniff_config(int ch)
{
	nrf_radio_packet_conf_t pc;

	radio_disable();
	nrf_radio_mode_set(NRF_RADIO,
			   s_sn_rate == 2 ? NRF_RADIO_MODE_NRF_2MBIT : NRF_RADIO_MODE_NRF_1MBIT);
	nrf_radio_fast_ramp_up_enable_set(NRF_RADIO, true);
	nrf_radio_txpower_set(NRF_RADIO, (nrf_radio_txpower_t)s_pwr_code);
	nrf_radio_frequency_set(NRF_RADIO, (uint16_t)(RADIO_BASE_MHZ + ch));

	memset(&pc, 0, sizeof(pc));
	pc.lflen = 0;
	pc.s0len = 0;
	pc.s1len = 0;
	pc.plen = NRF_RADIO_PREAMBLE_LENGTH_8BIT;
	pc.maxlen = (uint8_t)s_sn_len;
	pc.statlen = (uint8_t)s_sn_len;   /* no length field: every frame is STATLEN bytes */
	pc.balen = (uint8_t)s_sn_balen;
	pc.big_endian = false;
	pc.whiteen = false;
	nrf_radio_packet_configure(NRF_RADIO, &pc);

	nrf_radio_base0_set(NRF_RADIO, s_sn_base);
	nrf_radio_prefix0_set(NRF_RADIO, s_sn_prefix);
	nrf_radio_rxaddresses_set(NRF_RADIO, 1);
	if (s_sn_crc) {
		nrf_radio_crc_configure(NRF_RADIO, RADIO_CRCCNF_LEN_Two,
					NRF_RADIO_CRC_ADDR_INCLUDE, 0x11021UL);
		nrf_radio_crcinit_set(NRF_RADIO, 0xFFFFUL);
	} else {
		nrf_radio_crc_configure(NRF_RADIO, RADIO_CRCCNF_LEN_Disabled,
					NRF_RADIO_CRC_ADDR_INCLUDE, 0);
	}
}

/* One raw frame into s_snbuf within `to`. RSSI sampled at ADDRESS (hardware short). No CRC
 * wait (CRC may be off). Returns 0 with *rssi filled, or -ETIMEDOUT. Ends DISABLED. */
static int sniff_rx(k_timeout_t to, int *rssi_dbm)
{
	nrf_radio_packetptr_set(NRF_RADIO, s_snbuf);
	events_clear();
	nrf_radio_shorts_set(NRF_RADIO,
			     NRF_RADIO_SHORT_READY_START_MASK | NRF_RADIO_SHORT_ADDRESS_RSSISTART_MASK);
	k_sem_reset(&s_irq_sem);
	nrf_radio_int_enable(NRF_RADIO, NRF_RADIO_INT_END_MASK);
	nrf_radio_task_trigger(NRF_RADIO, NRF_RADIO_TASK_RXEN);
	if (k_sem_take(&s_irq_sem, to) != 0) {
		radio_disable();
		return -ETIMEDOUT;
	}
	*rssi_dbm = -(int)nrf_radio_rssi_sample_get(NRF_RADIO);
	radio_disable();
	return 0;
}

/* rf sniff <ch> [ms=20000] [rssi_min_dbm=-60]: dump proprietary 2.4 GHz bursts.
 * Prints #SNIFF24 once per NEW distinct payload (live, as a button is pressed) and a
 * #SNIFFTOP24 table at the end, most-repeated first. */
static int cmd_rf_sniff(const struct shell *sh, size_t argc, char **argv)
{
	int ch, ms = 20000, rssi_min = -60;

	if (argc < 2 || !parse_int(argv[1], CH_RX_MIN, CH_RX_MAX, &ch) ||
	    (argc > 2 && !parse_int(argv[2], 100, 600000, &ms)) ||
	    (argc > 3 && !parse_int(argv[3], -127, 0, &rssi_min))) {
		usage_sniff(sh);
		return fck_done(sh, "rf_sniff", RC_USAGE);
	}
	if (!radio_claim(sh, "rf_sniff")) {
		return 0;
	}

	uint32_t rx = 0, gated = 0;
	char hex[2 * SNIFF_MAX + 1];
	int64_t deadline = k_uptime_get() + ms;

	s_sn_ntbl = 0;
	sniff_config(ch);
	shell_print(sh, "#SNIFFSTART24 ch=%d mhz=%d rate=%dM crc=%d len=%d balen=%d "
			"base=0x%08x prefix=0x%02x rssi_min=%d ms=%d",
		    ch, RADIO_BASE_MHZ + ch, s_sn_rate, s_sn_crc, s_sn_len, s_sn_balen,
		    (unsigned)s_sn_base, s_sn_prefix, rssi_min, ms);

	while (k_uptime_get() < deadline) {
		int rssi = 0;

		fck_alive();
		if (sniff_rx(K_MSEC(100), &rssi) != 0) {
			continue;
		}
		rx++;
		if (rssi < rssi_min) {
			gated++;
			continue;
		}
		int hit = -1;

		for (int i = 0; i < s_sn_ntbl; i++) {
			if (!memcmp(s_sn_tbl[i], s_snbuf, s_sn_len)) {
				hit = i;
				break;
			}
		}
		if (hit >= 0) {
			if (s_sn_cnt[hit] < 0xFFFF) {
				s_sn_cnt[hit]++;
			}
			if (rssi > s_sn_best[hit]) {
				s_sn_best[hit] = (int8_t)rssi;
			}
			continue;
		}
		if (s_sn_ntbl < SNIFF_TBL) {
			int i = s_sn_ntbl++;

			memcpy(s_sn_tbl[i], s_snbuf, s_sn_len);
			s_sn_cnt[i] = 1;
			s_sn_best[i] = (int8_t)rssi;
			to_hex(s_snbuf, s_sn_len, hex);
			shell_print(sh, "#SNIFF24 i=%d rssi=%d len=%d data=%s", i, rssi, s_sn_len, hex);
		}
	}
	radio_release();

	/* summary: selection-sort the table indices by count, strongest repeats first */
	for (int a = 0; a < s_sn_ntbl; a++) {
		int top = a;

		for (int b = a + 1; b < s_sn_ntbl; b++) {
			if (s_sn_cnt[b] > s_sn_cnt[top]) {
				top = b;
			}
		}
		if (top != a) {
			uint8_t tb[SNIFF_MAX];

			memcpy(tb, s_sn_tbl[a], s_sn_len);
			memcpy(s_sn_tbl[a], s_sn_tbl[top], s_sn_len);
			memcpy(s_sn_tbl[top], tb, s_sn_len);
			uint16_t c = s_sn_cnt[a];

			s_sn_cnt[a] = s_sn_cnt[top];
			s_sn_cnt[top] = c;
			int8_t r = s_sn_best[a];

			s_sn_best[a] = s_sn_best[top];
			s_sn_best[top] = r;
		}
	}
	for (int i = 0; i < s_sn_ntbl; i++) {
		fck_alive();
		to_hex(s_sn_tbl[i], s_sn_len, hex);
		shell_print(sh, "#SNIFFTOP24 i=%d cnt=%u rssi=%d data=%s", i, s_sn_cnt[i],
			    s_sn_best[i], hex);
	}
	shell_print(sh, "#SNIFFDONE24 ch=%d rx=%u gated=%u uniq=%d ms=%d", ch, rx, gated,
		    s_sn_ntbl, ms);
	return fck_done(sh, "rf_sniff", RC_OK);
}

/* rf scfg [rate 1|2] [crc 0|2] [len 1-40]: show/set sniffer PHY knobs. */
static int cmd_rf_scfg(const struct shell *sh, size_t argc, char **argv)
{
	int rate = s_sn_rate, crc = s_sn_crc, len = s_sn_len;

	if ((argc > 1 && (!parse_int(argv[1], 1, 2, &rate))) ||
	    (argc > 2 && (!parse_int(argv[2], 0, 2, &crc) || crc == 1)) ||
	    (argc > 3 && (!parse_int(argv[3], 1, SNIFF_MAX, &len)))) {
		usage_scfg(sh);
		return fck_done(sh, "rf_scfg", RC_USAGE);
	}
	s_sn_rate = rate;
	s_sn_crc = crc;
	s_sn_len = len;
	shell_print(sh, "#SCFG24 rate=%dM crc=%d len=%d", s_sn_rate, s_sn_crc, s_sn_len);
	return fck_done(sh, "rf_scfg", RC_OK);
}

/* rf saddr [balen 2-4] [base0 hex32] [prefix0 hex8]: show/set the RADIO match address
 * directly (register values). Default 0xAAAAAAAA / 0xAA sits on an 0xAA preamble run; try
 * base/prefix 0x55 if the remote's preamble is 0x55, or a discovered address to lock on. */
static int cmd_rf_saddr(const struct shell *sh, size_t argc, char **argv)
{
	int balen = s_sn_balen;
	char *end;

	if (argc > 1 && !parse_int(argv[1], 2, 4, &balen)) {
		usage_saddr(sh);
		return fck_done(sh, "rf_saddr", RC_USAGE);
	}
	if (argc > 2) {
		unsigned long v = strtoul(argv[2], &end, 16);

		if (*end != '\0') {
			usage_saddr(sh);
			return fck_done(sh, "rf_saddr", RC_USAGE);
		}
		s_sn_base = (uint32_t)v;
	}
	if (argc > 3) {
		unsigned long v = strtoul(argv[3], &end, 16);

		if (*end != '\0' || v > 0xFF) {
			usage_saddr(sh);
			return fck_done(sh, "rf_saddr", RC_USAGE);
		}
		s_sn_prefix = (uint8_t)v;
	}
	s_sn_balen = balen;
	shell_print(sh, "#SADDR24 balen=%d addr_bytes=%d base=0x%08x prefix=0x%02x", s_sn_balen,
		    s_sn_balen + 1, (unsigned)s_sn_base, s_sn_prefix);
	return fck_done(sh, "rf_saddr", RC_OK);
}

/* The sub-commands of `rf`. They are found here and not by Zephyr's shell, so that a line the
 * firmware does not understand still ends with #DONE: a sub-command it does not have, too many
 * arguments, `-h`. Each handler is given argv[0] = its own name and checks the rest itself. */
static const struct {
	const char *name;
	const char *verb; /* the token its #DONE carries */
	int (*run)(const struct shell *sh, size_t argc, char **argv);
	void (*usage)(const struct shell *sh);
} k_sub[] = {
	{"spec", "rf_spec", cmd_rf_spec, usage_spec},
	{"off", "rf_off", cmd_rf_off, usage_off},
	{"power", "rf_power", cmd_rf_power, usage_power},
	{"echo", "rf_echo", cmd_rf_echo, usage_echo},
	{"ping", "rf_ping", cmd_rf_ping, usage_ping},
	{"sniff", "rf_sniff", cmd_rf_sniff, usage_sniff},
	{"scfg", "rf_scfg", cmd_rf_scfg, usage_scfg},
	{"saddr", "rf_saddr", cmd_rf_saddr, usage_saddr},
};

int rf_command(const struct shell *sh, size_t argc, char **argv)
{
	for (size_t i = 0; argc > 0 && i < ARRAY_SIZE(k_sub); i++) {
		if (strcmp(argv[0], k_sub[i].name) != 0) {
			continue;
		}
		if (argc - 1 > FCK_ARGS_MAX || fck_help_asked(argc - 1, &argv[1])) {
			k_sub[i].usage(sh);
			return fck_done(sh, k_sub[i].verb, RC_USAGE);
		}
		return k_sub[i].run(sh, argc, argv);
	}
	shell_print(sh, "usage: rf <spec|off|power|echo|ping|sniff|scfg|saddr> ...");
	return fck_done(sh, "rf", RC_USAGE);
}

/* ===== Init ===================================================================== */

int rf_init(void)
{
	const struct fck_nrf_limits *l = &fck_nrf_limits;

	s_dbm = pwr_snap(0, &s_pwr_code);
	s_link_ch = CH_LINK_DEFAULT;
	/* A table that contradicts itself or the RADIO is said at boot (#ERR what=rf_init). */
	if (l->tx_mhz_min > l->tx_mhz_max || l->rx_mhz_min > l->rx_mhz_max ||
	    CH_RX_MIN < 0 || CH_RX_MAX > RADIO_CH_MAX || CH_TX_MIN < CH_RX_MIN ||
	    CH_TX_MAX > CH_RX_MAX || CH_LINK_DEFAULT < CH_TX_MIN || CH_LINK_DEFAULT > CH_TX_MAX ||
	    l->tx_dbm_max < PWR_DBM_MIN) {
		return -EINVAL;
	}
	IRQ_CONNECT(RADIO_0_IRQn, 1, radio_isr, NULL, 0);
	irq_enable(RADIO_0_IRQn);
	return 0;
}
