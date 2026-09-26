/*
 * 统计输出
 */
#include <stdio.h>

#include <rte_cycles.h>
#include <rte_ethdev.h>

#include "stats.h"

uint64_t g_hz, g_ns_mult;

void stats_init(void)
{
	g_hz = rte_get_tsc_hz();
	g_ns_mult = (1000000000ULL << 20) / g_hz;
}

static uint64_t pct(const struct port_stat *st, uint64_t total, double q)
{
	uint64_t target = (uint64_t)(total * q), acc = 0;
	for (int i = 0; i < HIST_N; i++) { acc += st->hist[i]; if (acc > target) return (uint64_t)(i + 1) * HIST_NS; }
	for (int i = 0; i < HIST2_N; i++) { acc += st->hist2[i]; if (acc > target) return SLOW_NS + (uint64_t)(i + 1) * HIST2_US * 1000; }
	return SLOW_NS + (uint64_t)HIST2_N * HIST2_US * 1000;
}

void stats_print(struct port_stat *prev, struct port_stat *prev_tx, double dt, bool final)
{
	static int line;
	if (final || line++ % 20 == 0)
		printf("%sflow txMpps txWireG rxMpps rxWireG txnM/s dataG  meanus  p50us  p99us p999us   maxus     slow busyT%% busyR%% cyc/pk  rxB     txPkts     rxPkts   err   ign  pcpx imissed nombuf\n",
		       final ? "---- total/avg ----\n" : "");
	for (uint16_t i = 0; i < g_nb_flows; i++) {
		struct port_stat *st = &g_flow[i].st, *o = &prev[i], *tt = &g_flow[i].st_tx, *ot = &prev_tx[i];
		struct rte_eth_stats es; rte_eth_stats_get(g_flow[i].port, &es);
		uint64_t txp = st->tx_pkts + tt->tx_pkts, otxp = o->tx_pkts + ot->tx_pkts;
		uint64_t txw = st->tx_wire_bytes + tt->tx_wire_bytes, otxw = o->tx_wire_bytes + ot->tx_wire_bytes;
		uint64_t bT = tt->busy_cyc - ot->busy_cyc, bR = st->busy_cyc - o->busy_cyc;
		uint64_t pk = (txp - otxp) + (st->rx_pkts - o->rx_pkts);
		uint64_t tot = st->txn_done;
		printf("%u.%-2u %6.2f %7.1f %6.2f %7.1f %6.2f %5.1f %7.2f %6.2f %6.2f %6.1f %7.1f %8lu %6.1f %6.1f %6.0f %4.1f %10lu %10lu %5lu %5lu %5lu %7lu %6lu\n",
		       g_flow[i].port, g_flow[i].flow,
		       (txp - otxp) / dt / 1e6, (txw - otxw) * 8 / dt / 1e9,
		       (st->rx_pkts - o->rx_pkts) / dt / 1e6, (st->rx_wire_bytes - o->rx_wire_bytes) * 8 / dt / 1e9,
		       (st->txn_done - o->txn_done) / dt / 1e6, (st->data_bytes - o->data_bytes) * 8 / dt / 1e9,
		       tot ? st->rtt_sum_ns / 1e3 / tot : 0,
		       tot ? pct(st, tot, 0.5) / 1e3 : 0, tot ? pct(st, tot, 0.99) / 1e3 : 0,
		       tot ? pct(st, tot, 0.999) / 1e3 : 0, st->rtt_max_ns / 1e3, (unsigned long)st->slow,
		       bT * 100.0 / (dt * g_hz), bR * 100.0 / (dt * g_hz),
		       pk ? (double)(bT + bR) / pk : 0,
		       (st->rx_hits - o->rx_hits) ? (double)(st->rx_pkts - o->rx_pkts) / (st->rx_hits - o->rx_hits) : 0,
		       (unsigned long)txp, (unsigned long)st->rx_pkts, (unsigned long)st->err, (unsigned long)st->ign,
		       (unsigned long)st->pcpx, (unsigned long)es.imissed, (unsigned long)es.rx_nombuf);
		if (!final) { *o = *st; *ot = *tt; }
	}
	fflush(stdout);
}
