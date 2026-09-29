/*
 * 接收硬件时间戳：时间戳字段注册与时钟锚点
 */
#define ALLOW_EXPERIMENTAL_API
#include <stdio.h>
#include <rte_ethdev.h>
#include <rte_cycles.h>

#include "hwts.h"

struct hwclk g_hwclk[MAX_PORTS][2];
uint32_t g_hwclk_idx[MAX_PORTS];
int g_ts_off = -1;
uint64_t g_ts_flag;

int hwts_enable(void)
{
	if (g_ts_off >= 0) return 0;
	return rte_mbuf_dyn_rx_timestamp_register(&g_ts_off, &g_ts_flag);
}

void hwclk_update(uint16_t p)
{
	static uint64_t pt[MAX_PORTS], pc[MAX_PORTS];
	static bool warned[MAX_PORTS];
	uint64_t best = UINT64_MAX, tm = 0, c = 0;
	for (int i = 0; i < 5; i++) {
		uint64_t clk, a = rte_rdtsc();
		if (rte_eth_read_clock(p, &clk) != 0) {
			if (!warned[p]) { printf("警告：port %u 不支持 rte_eth_read_clock，--hwts 无效\n", p); warned[p] = true; }
			return;
		}
		uint64_t b = rte_rdtsc();
		if (b - a < best) { best = b - a; tm = a + (b - a) / 2; c = clk; }
	}
	if (pc[p] && c > pc[p]) {
		uint32_t nx = g_hwclk_idx[p] + 1;
		struct hwclk *k = &g_hwclk[p][nx & 1];
		k->r = (double)(tm - pt[p]) / (double)(c - pc[p]);
		k->tsc0 = tm; k->clk0 = c;
		if (!warned[p]) {
			printf("port %u 网卡时钟 %.1f MHz，读时钟耗时 %.2f µs\n", p, rte_get_tsc_hz() / k->r / 1e6, best * 1e6 / rte_get_tsc_hz());
			warned[p] = true;
		}
		__atomic_store_n(&g_hwclk_idx[p], nx, __ATOMIC_RELEASE);
	}
	pt[p] = tm; pc[p] = c;
}
