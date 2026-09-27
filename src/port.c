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
static int steer_flow(uint16_t pi, const struct rte_ether_addr *mac, uint16_t q)
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
	if (rte_flow_create(pi, &attr, pat, act, &err)) return 0;
	printf("警告：port %u 为 queue %u 建立导流规则失败（%s）\n", pi, q, err.message ? err.message : "?");
	return -1;
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

	/*
	 * 接收过滤：每条流一条 rte_flow 规则（DMAC == 本流地址 → 本流队列），eth 头默认不开混杂，
	 * 其余单播由网卡丢弃。多网卡接同一交换机同一 VLAN 时，交换机对未学到的 MAC 泛洪，
	 * 开混杂会让副本进到另一个口：sender 记成重复响应，reflector 从错误的口回包，
	 * 交换机随之把 MAC 学到错误端口。
	 * sue 头的 GPU ID 可能让 MAC 组播位为 1，仍开混杂 + allmulticast，靠软件核对 DMAC。
	 */
	int bad = 0;
	for (uint16_t f = 0; f < nq; f++) {
		const struct rte_ether_addr *m = &g_flow[pi * nq + f].addr.src;
		if (g_cfg.hdr == HDR_ETH && f > 0 && rte_eth_dev_mac_addr_add(pi, (struct rte_ether_addr *)m, 0) < 0)
			printf("提示：port %u 添加 MAC 过滤 %u 失败，依赖导流规则\n", pi, f);
		bad |= steer_flow(pi, m, f);
	}
	if (g_cfg.promisc || g_cfg.hdr == HDR_SUE || bad) {
		rte_eth_promiscuous_enable(pi);
		if (g_cfg.hdr == HDR_SUE) rte_eth_allmulticast_enable(pi);
		printf("port %u: 混杂模式%s\n", pi, bad ? "（导流规则失败，退回）" : "");
	} else {
		rte_eth_promiscuous_disable(pi);
	}

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
