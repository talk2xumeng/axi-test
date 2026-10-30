/*
 * 统计输出
 */
#include <stdio.h>
#include <string.h>

#include <rte_cycles.h>
#include <rte_ethdev.h>

#include "stats.h"

uint64_t g_hz, g_ns_mult;

void stats_init(void)
{
	g_hz = rte_get_tsc_hz();
	g_ns_mult = (1000000000ULL << 20) / g_hz;
}

static double rxd_pct(const struct port_stat *st, double q)
{
	uint64_t target = (uint64_t)(st->rxd_n * q), acc = 0;
	for (int i = 0; i < RXD_N; i++) { acc += st->rxd_hist[i]; if (acc > target) return (i + 1) * RXD_NS / 1e3; }
	return RXD_N * RXD_NS / 1e3;
}

static double swd_pct(const uint64_t *hist, uint64_t n, double q)
{
	uint64_t target = (uint64_t)(n * q), acc = 0;
	for (int i = 0; i < SWD_N; i++) { acc += hist[i]; if (acc > target) return (i + 1) * SWD_NS / 1e3; }
	return SWD_N * SWD_NS / 1e3;
}

static uint64_t pct(const struct port_stat *st, uint64_t total, double q)
{
	uint64_t target = (uint64_t)(total * q), acc = 0;
	for (int i = 0; i < HIST_N; i++) { acc += st->hist[i]; if (acc > target) return (uint64_t)(i + 1) * HIST_NS; }
	for (int i = 0; i < HIST2_N; i++) { acc += st->hist2[i]; if (acc > target) return SLOW_NS + (uint64_t)(i + 1) * HIST2_US * 1000; }
	return SLOW_NS + (uint64_t)HIST2_N * HIST2_US * 1000;
}

/*
 * --brief：所有流、所有端口合并。rw / mix 分 w、r、all 三行，否则只有 all 一行。
 * 速率与 RTT 分位数是本次间隔的（final 时 prev 全零，即全程）；maxus、err、lost、bad、miss 是累计。
 * L2 = 帧长（不足 60 按 60），不含 FCS、前导码、帧间隔。
 */
static void print_brief(struct port_stat *prev, struct port_stat (*prev_tx)[MAX_TXC], double dt, bool final)
{
	static int line;
	static struct port_stat m[3];                /* 合并后的本次增量直方图：0 = 写，1 = 读，2 = 全部 */
	struct { uint64_t txp, txw, rxp, rxw, txn, data, err, lost, bad, maxns; double busy; } s[3];
	const bool two = (g_cfg.sender && g_cfg.mix) || g_cfg.rw;
	memset(s, 0, sizeof(s));
	for (int g = 0; g < 3; g++) { memset(m[g].hist, 0, sizeof(m[g].hist)); memset(m[g].hist2, 0, sizeof(m[g].hist2)); }

	for (uint16_t i = 0; i < g_nb_flows; i++) {
		const struct flow_ctx *c = &g_flow[i];
		const struct port_stat *st = &c->st, *o = &prev[i];
		uint64_t txp = st->tx_pkts - o->tx_pkts, txw = st->tx_wire_bytes - o->tx_wire_bytes, tlost = 0;
		double busy = (st->busy_cyc - o->busy_cyc) * 100.0 / (dt * g_hz);
		for (uint16_t k = 0; k < c->ntx; k++) {
			const struct port_stat *tt = &c->lane[k].st, *ot = &prev_tx[i][k];
			const double b = (tt->busy_cyc - ot->busy_cyc) * 100.0 / (dt * g_hz);
			txp += tt->tx_pkts - ot->tx_pkts;
			txw += tt->tx_wire_bytes - ot->tx_wire_bytes;
			tlost += tt->lost;
			if (b > busy) busy = b;
		}
		const int gs[2] = { two ? (c->read ? 1 : 0) : 2, 2 };
		for (int j = 0; j < (two ? 2 : 1); j++) {
			const int g = gs[j];
			s[g].txp += txp; s[g].txw += txw;
			s[g].rxp += st->rx_pkts - o->rx_pkts; s[g].rxw += st->rx_wire_bytes - o->rx_wire_bytes;
			s[g].txn += st->txn_done - o->txn_done; s[g].data += st->data_bytes - o->data_bytes;
			s[g].err += st->err; s[g].lost += st->lost + tlost; s[g].bad += st->xmac + st->pcpx;
			if (st->rtt_max_ns > s[g].maxns) s[g].maxns = st->rtt_max_ns;
			if (busy > s[g].busy) s[g].busy = busy;
			for (int b = 0; b < HIST_N; b++) m[g].hist[b] += st->hist[b] - o->hist[b];
			for (int b = 0; b < HIST2_N; b++) m[g].hist2[b] += st->hist2[b] - o->hist2[b];
		}
	}
	uint64_t miss = 0;                           /* 网卡侧丢包是端口级的，只在 all 行显示 */
	for (uint16_t p = 0; p < g_nb_ports; p++) {
		struct rte_eth_stats es;
		if (rte_eth_stats_get(p, &es) == 0) miss += es.imissed + es.rx_nombuf;
	}

	if (final || line++ % 20 == 0) {
		if (final) printf("---- total/avg ----\n");
		if (g_cfg.sender) printf("      txL2G   rxL2G   dataG  txnM/s   p50us   p99us  p999us    maxus    err   lost    bad    miss busy%%\n");
		else              printf("      txL2G   rxL2G    err    bad    miss busy%%\n");
	}
	static const char *const name[3] = { "w", "r", "all" };
	for (int g = two ? 0 : 2; g < 3; g++) {
		const double txl2 = ((double)s[g].txw - s[g].txp * 24.0) * 8 / dt / 1e9;
		const double rxl2 = ((double)s[g].rxw - s[g].rxp * 24.0) * 8 / dt / 1e9;
		char ms[24] = "-";
		if (g == 2) snprintf(ms, sizeof(ms), "%lu", (unsigned long)miss);
		if (g_cfg.sender) {
			const uint64_t n = s[g].txn;         /* txn_done 与直方图同步增加，分位数按直方图增量算 */
			uint64_t h = 0;
			for (int b = 0; b < HIST_N; b++) h += m[g].hist[b];
			for (int b = 0; b < HIST2_N; b++) h += m[g].hist2[b];
			printf("%-4s %7.1f %7.1f %7.1f %7.2f %7.2f %7.2f %7.1f %8.1f %6lu %6lu %6lu %7s %5.0f\n", name[g], txl2, rxl2,
			       s[g].data * 8 / dt / 1e9, n / dt / 1e6,
			       h ? pct(&m[g], h, 0.5) / 1e3 : 0, h ? pct(&m[g], h, 0.99) / 1e3 : 0, h ? pct(&m[g], h, 0.999) / 1e3 : 0,
			       s[g].maxns / 1e3, (unsigned long)s[g].err, (unsigned long)s[g].lost, (unsigned long)s[g].bad, ms, s[g].busy);
		} else {
			printf("%-4s %7.1f %7.1f %6lu %6lu %7s %5.0f\n", name[g], txl2, rxl2,
			       (unsigned long)s[g].err, (unsigned long)s[g].bad, ms, s[g].busy);
		}
	}
	if (!final)
		for (uint16_t i = 0; i < g_nb_flows; i++) {
			prev[i] = g_flow[i].st;
			for (uint16_t k = 0; k < g_flow[i].ntx; k++) prev_tx[i][k] = g_flow[i].lane[k].st;
		}
	fflush(stdout);
}

void stats_print(struct port_stat *prev, struct port_stat (*prev_tx)[MAX_TXC], double dt, bool final)
{
	if (g_cfg.brief) { print_brief(prev, prev_tx, dt, final); return; }
	static int line;
	if (final || line++ % 20 == 0)
		printf("%sflow txMpps txWireG rxMpps rxWireG txnM/s dataG  meanus  p50us  p99us p999us   maxus     slow busyT%% busyR%% cyc/pk  rxB     txPkts     rxPkts   err  lost   ign  xmac  pcpx imissed nombuf%s  swd50us swd99us\n",
		       final ? "---- total/avg ----\n" : "", g_cfg.hwts ? "  rxd50us rxd99us rxdneg" : "");
	for (uint16_t i = 0; i < g_nb_flows; i++) {
		struct flow_ctx *c = &g_flow[i];
		struct port_stat *st = &c->st, *o = &prev[i];
		struct rte_eth_stats es; rte_eth_stats_get(c->port, &es);
		/* 分核模式各发送核的统计在 lane[k].st；单核模式（及 reflector）全在 st，lane 里为 0 */
		uint64_t txp = st->tx_pkts, otxp = o->tx_pkts, txw = st->tx_wire_bytes, otxw = o->tx_wire_bytes;
		uint64_t bT = 0, bR = st->busy_cyc - o->busy_cyc, tlost = 0, swn = 0;
		static uint64_t swh[SWD_N];
		memset(swh, 0, sizeof(swh));
		for (uint16_t k = 0; k < c->ntx; k++) {
			const struct port_stat *tt = &c->lane[k].st, *ot = &prev_tx[i][k];
			txp += tt->tx_pkts; otxp += ot->tx_pkts;
			txw += tt->tx_wire_bytes; otxw += ot->tx_wire_bytes;
			bT += tt->busy_cyc - ot->busy_cyc;
			tlost += tt->lost;
			for (int b = 0; b < SWD_N; b++) swh[b] += tt->swd_hist[b];
			swn += tt->swd_n;
		}
		const double bTpct = bT * 100.0 / (dt * g_hz) / (c->ntx ? c->ntx : 1);   /* 多个发送核时为每核平均 */
		uint64_t pk = (txp - otxp) + (st->rx_pkts - o->rx_pkts);
		uint64_t tot = st->txn_done;
		char lab[24];
		snprintf(lab, sizeof(lab), "%u.%u%s", c->port, c->flow,
		         ((g_cfg.sender && g_cfg.mix) || g_cfg.rw) ? (c->read ? "r" : "w") : "");
		printf("%-4s %6.2f %7.1f %6.2f %7.1f %6.2f %5.1f %7.2f %6.2f %6.2f %6.1f %7.1f %8lu %6.1f %6.1f %6.0f %4.1f %10lu %10lu %5lu %5lu %5lu %5lu %5lu %7lu %6lu",
		       lab,
		       (txp - otxp) / dt / 1e6, (txw - otxw) * 8 / dt / 1e9,
		       (st->rx_pkts - o->rx_pkts) / dt / 1e6, (st->rx_wire_bytes - o->rx_wire_bytes) * 8 / dt / 1e9,
		       (st->txn_done - o->txn_done) / dt / 1e6, (st->data_bytes - o->data_bytes) * 8 / dt / 1e9,
		       tot ? st->rtt_sum_ns / 1e3 / tot : 0,
		       tot ? pct(st, tot, 0.5) / 1e3 : 0, tot ? pct(st, tot, 0.99) / 1e3 : 0,
		       tot ? pct(st, tot, 0.999) / 1e3 : 0, st->rtt_max_ns / 1e3, (unsigned long)st->slow,
		       bTpct, bR * 100.0 / (dt * g_hz),
		       pk ? (double)(bT + bR) / pk : 0,
		       (st->rx_hits - o->rx_hits) ? (double)(st->rx_pkts - o->rx_pkts) / (st->rx_hits - o->rx_hits) : 0,
		       (unsigned long)txp, (unsigned long)st->rx_pkts, (unsigned long)st->err, (unsigned long)(st->lost + tlost), (unsigned long)st->ign, (unsigned long)st->xmac,
		       (unsigned long)st->pcpx, (unsigned long)es.imissed, (unsigned long)es.rx_nombuf);
		if (g_cfg.hwts)
			printf("  %7.2f %7.2f %6lu", st->rxd_n ? rxd_pct(st, 0.5) : 0, st->rxd_n ? rxd_pct(st, 0.99) : 0, (unsigned long)st->rxd_neg);
		const uint64_t *sh = swn ? swh : st->swd_hist;      /* sender 分核时在各发送核的统计里，合并 */
		const uint64_t sn = swn ? swn : st->swd_n;
		printf("  %7.2f %7.2f", sn ? swd_pct(sh, sn, 0.5) : 0, sn ? swd_pct(sh, sn, 0.99) : 0);
		printf("\n");
		if (!final) { *o = *st; for (uint16_t k = 0; k < c->ntx; k++) prev_tx[i][k] = c->lane[k].st; }
	}
	fflush(stdout);
}
