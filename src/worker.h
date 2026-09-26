/*
 * 收发核入口与公共发送辅助
 */
#ifndef AXIPERF_WORKER_H
#define AXIPERF_WORKER_H

#include <rte_ethdev.h>
#include "common.h"

int sender_loop(void *arg);       /* 单核：同一核收发 */
int sender_tx_loop(void *arg);    /* 分核：只发请求 */
int sender_rx_loop(void *arg);    /* 分核：只收响应 */
int reflector_loop(void *arg);

/* 不丢包：发不完就重试，直到退出 */
static inline void tx_all(uint16_t port, uint16_t q, struct rte_mbuf **pk, uint16_t n)
{
	uint16_t sent = 0;
	while (sent < n && !g_quit)
		sent += rte_eth_tx_burst(port, q, pk + sent, n - sent);
	if (sent < n) rte_pktmbuf_free_bulk(pk + sent, n - sent);
}

#endif
