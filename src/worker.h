/*
 * 收发核入口与公共发送辅助
 */
#ifndef AXIPERF_WORKER_H
#define AXIPERF_WORKER_H

#include <rte_ethdev.h>
#include <rte_prefetch.h>
#include "common.h"

int sender_loop(void *arg);       /* 单核：同一核收发 */
int sender_tx_loop(void *arg);    /* 分核：只发请求 */
int sender_rx_loop(void *arg);    /* 分核：只收响应 */
int reflector_loop(void *arg);

/*
 * 预取收到的帧中要解析的 cache line：
 *   ≤ 256B 的控制帧（读请求 18 + 4×16 = 82B、合并写响应 18 + 16×12 = 210B）整帧都要解析，全部预取；
 *   > 256B 的数据帧（写请求、读响应）只解析事务头：第 0 行，以及 256..319 那一行
 *   （2 × 256B 打包时第二个事务头在 290 / 278 字节），中间的数据行不碰。
 * 未预取时：读请求第 4 个读头在第 66 字节（第 1 行），perf 里这一条 load 占反射端 18%。
 */
static inline void rx_prefetch(struct rte_mbuf *m)
{
	const uint8_t *f = rte_pktmbuf_mtod(m, const uint8_t *);
	const uint32_t len = m->pkt_len;
	rte_prefetch0(f);
	if (len > 256) { rte_prefetch0(f + 256); return; }
	for (uint32_t off = 64; off < len; off += 64) rte_prefetch0(f + off);
}

/* 不丢包：发不完就重试，直到退出 */
static inline void tx_all(uint16_t port, uint16_t q, struct rte_mbuf **pk, uint16_t n)
{
	uint16_t sent = 0;
	while (sent < n && !g_quit)
		sent += rte_eth_tx_burst(port, q, pk + sent, n - sent);
	if (sent < n) rte_pktmbuf_free_bulk(pk + sent, n - sent);
}

/* 发到本上下文的发送队列：--txq N 时把这一批按顺序均分成 N 段，各段进一个队列，起始队列每批轮转 */
static inline void tx_ctx(struct flow_ctx *c, struct rte_mbuf **pk, uint16_t n)
{
	const uint16_t nq = (uint16_t)g_cfg.txq;
	if (nq <= 1) { tx_all(c->port, c->txq0, pk, n); return; }
	const uint32_t j0 = c->txrr++;
	uint16_t off = 0;
	for (uint16_t j = 0; j < nq && off < n; j++) {
		uint16_t k = (uint16_t)((n - off + (nq - j) - 1) / (nq - j));   /* 剩余均分，向上取整 */
		tx_all(c->port, (uint16_t)(c->txq0 + (j0 + j) % nq), pk + off, k);
		off = (uint16_t)(off + k);
	}
}

#endif
