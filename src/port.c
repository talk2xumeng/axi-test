/*
 * 端口与流初始化
 */
#include <stdio.h>
#include <stdlib.h>

#include <rte_ethdev.h>
#include <rte_flow.h>
#include <rte_debug.h>

#include "port.h"
#include "hdr.h"
#include "axi.h"

/* ---------------- 预填模板 ---------------- */
struct tmpl { uint8_t buf[TMPL_ROOM]; uint16_t len; };
static struct tmpl g_tmpl[MAX_FLOWS];

static void tmpl_obj_init(struct rte_mempool *mp, void *arg, void *obj, unsigned i)
{
	(void)mp; (void)i;
	struct rte_mbuf *m = obj;
	const struct tmpl *t = arg;
	memcpy((uint8_t *)m->buf_addr + RTE_PKTMBUF_HEADROOM, t->buf, t->len);
}

/*
 * sender：完整请求帧（头 + pack 个事务），发送时只改各事务的 ID
 * reflector：读响应数据区（8 × (R 头 + 1 拍)，填 0x5A），头和 R 头在发送时按实际写
 */
static void tmpl_build(struct flow_ctx *c, struct tmpl *t)
{
	const uint16_t hl = hdr_len();
	memset(t->buf, 0, sizeof(t->buf));
	if (g_cfg.sender) {
		uint16_t plen = axi_req_build(t->buf + hl, g_cfg.read, g_cfg.pack, g_cfg.beats);
		hdr_build(t->buf, &c->addr.dst, &c->addr.src, g_cfg.read ? VC_AR : VC_AW, (uint16_t)g_cfg.pack, plen);
		t->len = (uint16_t)(hl + plen);
		c->req_len = t->len;
	} else {
		memset(t->buf + hl, 0x5A, MAX_BEATS * (R_HDR + BEAT));
		t->len = (uint16_t)(hl + MAX_BEATS * (R_HDR + BEAT));
	}
}

/* ---------------- 导流：接收帧的 DMAC 字段 == 本流地址 → 本流队列 ---------------- */
static void steer_flow(uint16_t pi, const struct rte_ether_addr *mac, uint16_t q)
{
	struct rte_flow_attr attr = { .ingress = 1 };
	struct rte_flow_item_eth spec, mask;
	memset(&spec, 0, sizeof(spec)); memset(&mask, 0, sizeof(mask));
	spec.hdr.dst_addr = *mac;
	memset(&mask.hdr.dst_addr, 0xFF, 6);
	struct rte_flow_item pat[] = {
		{ .type = RTE_FLOW_ITEM_TYPE_ETH, .spec = &spec, .mask = &mask },
		{ .type = RTE_FLOW_ITEM_TYPE_END } };
	struct rte_flow_action_queue qa = { .index = q };
	struct rte_flow_action act[] = {
		{ .type = RTE_FLOW_ACTION_TYPE_QUEUE, .conf = &qa },
		{ .type = RTE_FLOW_ACTION_TYPE_END } };
	struct rte_flow_error err;
	if (!rte_flow_create(pi, &attr, pat, act, &err))
		printf("警告：port %u 为 queue %u 建立导流规则失败（%s），多流时响应可能进错队列\n",
		       pi, q, err.message ? err.message : "?");
}

void port_init(uint16_t pi)
{
	int socket = rte_eth_dev_socket_id(pi);
	uint16_t nrxd = NB_RXD, ntxd = NB_TXD, nq = (uint16_t)g_cfg.fpp;
	char name[32];
	struct rte_ether_addr pmac;
	struct rte_eth_conf conf; memset(&conf, 0, sizeof(conf));   /* 不开 VLAN strip */

	if (rte_eth_dev_configure(pi, nq, nq, &conf) < 0 ||
	    rte_eth_dev_adjust_nb_rx_tx_desc(pi, &nrxd, &ntxd) < 0)
		rte_exit(EXIT_FAILURE, "port %u configure failed\n", pi);
	rte_eth_macaddr_get(pi, &pmac);

	for (uint16_t f = 0; f < nq; f++) {
		uint16_t fi = (uint16_t)(pi * nq + f);
		struct flow_ctx *c = &g_flow[fi];
		c->port = pi; c->q = f; c->flow = f; c->idx = fi;
		hdr_flow_addr(pi, f, fi, &pmac, &c->addr);

		snprintf(name, sizeof(name), "rx%u_%u", pi, f);
		c->rx_pool = rte_pktmbuf_pool_create(name, RX_POOL_N, POOL_CACHE, 0, RTE_MBUF_DEFAULT_BUF_SIZE, socket);
		if (!c->rx_pool) rte_exit(EXIT_FAILURE, "rx pool %u.%u\n", pi, f);
		if (rte_eth_rx_queue_setup(pi, f, nrxd, socket, NULL, c->rx_pool) < 0 ||
		    rte_eth_tx_queue_setup(pi, f, ntxd, socket, NULL) < 0)
			rte_exit(EXIT_FAILURE, "port %u queue %u setup failed\n", pi, f);

		tmpl_build(c, &g_tmpl[fi]);
		snprintf(name, sizeof(name), "tmpl%u_%u", pi, f);
		c->tmpl_pool = rte_pktmbuf_pool_create(name, TX_POOL_N, POOL_CACHE, 0, RTE_PKTMBUF_HEADROOM + TMPL_ROOM, socket);
		if (!c->tmpl_pool) rte_exit(EXIT_FAILURE, "tmpl pool %u.%u\n", pi, f);
		rte_mempool_obj_iter(c->tmpl_pool, tmpl_obj_init, &g_tmpl[fi]);
	}
	if (rte_eth_dev_start(pi) < 0) rte_exit(EXIT_FAILURE, "port %u start\n", pi);
	rte_eth_promiscuous_enable(pi);
	if (g_cfg.hdr == HDR_SUE) rte_eth_allmulticast_enable(pi);   /* GPU ID 可能让 MAC 首字节组播位为 1 */
	if (nq > 1)
		for (uint16_t f = 0; f < nq; f++) steer_flow(pi, &g_flow[pi * nq + f].addr.src, f);

	for (uint16_t f = 0; f < nq; f++) {
		const struct flow_ctx *c = &g_flow[pi * nq + f];
		char a[32], b[32];
		hdr_addr_str(&c->addr.src, a, sizeof(a));
		hdr_addr_str(&c->addr.dst, b, sizeof(b));
		if (g_cfg.sender)
			printf("flow %u.%u: queue %u  my %s  peer %s\n", pi, f, f, a, b);
		else
			printf("flow %u.%u: queue %u  my %s\n", pi, f, f, a);
	}
}

void port_fini(void)
{
	for (uint16_t i = 0; i < g_nb_ports; i++) {
		rte_flow_flush(i, NULL);
		rte_eth_dev_stop(i);
		rte_eth_dev_close(i);
	}
}
