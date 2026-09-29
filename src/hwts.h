/*
 * 接收硬件时间戳（--hwts）：测每个包“网卡收到 → CPU 拿到”的时延（接收时延）
 *
 * 网卡时间戳与 TSC 是两个时钟：统计核每秒用 rte_eth_read_clock 取一对（TSC, 网卡时钟）
 * 作为锚点，并用相邻两次锚点算出比例；收发核按最近的锚点把网卡时间戳换算成 TSC。
 * 读网卡时钟要走一次 PCIe，锚点取 5 次中最快的一次、按中点对齐，偏差在约 ±0.5 µs 内。
 */
#ifndef AXIPERF_HWTS_H
#define AXIPERF_HWTS_H

#include <rte_mbuf_dyn.h>
#include "common.h"
#include "stats.h"

struct hwclk { double r; uint64_t tsc0, clk0; };   /* tsc = tsc0 + (clk - clk0) * r */
extern struct hwclk g_hwclk[MAX_PORTS][2];
extern uint32_t g_hwclk_idx[MAX_PORTS];
extern int g_ts_off;
extern uint64_t g_ts_flag;

int  hwts_enable(void);           /* 注册 mbuf 时间戳字段；失败返回 -1 */
void hwclk_update(uint16_t port); /* 统计核调用：取一次锚点 */

static inline void rxd_record(struct port_stat *s, uint16_t port, struct rte_mbuf **m, uint16_t n, uint64_t now)
{
	if (!g_cfg.hwts) return;
	const struct hwclk *k = &g_hwclk[port][__atomic_load_n(&g_hwclk_idx[port], __ATOMIC_ACQUIRE) & 1];
	if (k->r == 0) return;
	for (uint16_t i = 0; i < n; i++) {
		if (!(m[i]->ol_flags & g_ts_flag)) { s->rxd_bad++; continue; }
		uint64_t ts = *RTE_MBUF_DYNFIELD(m[i], g_ts_off, rte_mbuf_timestamp_t *);
		double d = (double)now - ((double)k->tsc0 + (double)(int64_t)(ts - k->clk0) * k->r);
		if (d < 0) { s->rxd_neg++; d = 0; }
		uint64_t ns = ((uint64_t)d * g_ns_mult) >> 20, b = ns / RXD_NS;
		s->rxd_hist[b < RXD_N ? b : RXD_N - 1]++;
		s->rxd_n++;
	}
}

#endif
