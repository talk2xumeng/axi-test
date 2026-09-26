/*
 * 统计：RTT 直方图（快路径 inline）与每秒输出
 */
#ifndef AXIPERF_STATS_H
#define AXIPERF_STATS_H

#include "common.h"

extern uint64_t g_hz, g_ns_mult;       /* ns = cyc * g_ns_mult >> 20，避免快路径除法 */

static inline void hist_add(struct port_stat *s, uint64_t cyc)
{
	uint64_t ns = (cyc * g_ns_mult) >> 20;
	uint64_t b = ns / HIST_NS;
	if (b < HIST_N) s->hist[b]++;
	else {
		uint64_t b2 = (ns - SLOW_NS) / (HIST2_US * 1000);
		s->hist2[b2 < HIST2_N ? b2 : HIST2_N - 1]++;
		s->slow++;
	}
	s->rtt_sum_ns += ns;
	if (ns > s->rtt_max_ns) s->rtt_max_ns = ns;
}

void stats_init(void);
/* prev / prev_tx 为上次快照；final=true 时打印全程汇总且不更新快照 */
void stats_print(struct port_stat *prev, struct port_stat *prev_tx, double dt, bool final);

#endif
