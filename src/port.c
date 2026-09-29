/*
 * 端口与流初始化
 */
#include <stdio.h>
#include <stdlib.h>

#include <rte_ethdev.h>
#include <rte_flow.h>
#include <rte_debug.h>
#include <rte_cycles.h>
#include <rte_dev.h>
#include <rte_devargs.h>

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
		uint16_t plen = axi_req_build(t->buf + hl, c->read, req_pack(c->read), g_cfg.beats);
		hdr_build(t->buf, &c->addr.dst, &c->addr.src, c->read ? VC_AR : VC_AW, (uint16_t)req_pack(c->read), plen);
		t->len = (uint16_t)(hl + plen);
		c->req_len = t->len;
	} else {
		memset(t->buf + hl, 0x5A, MAX_BEATS * (R_HDR + BEAT));
		t->len = (uint16_t)(hl + MAX_BEATS * (R_HDR + BEAT));
	}
}

/* ---------------- 导流：接收帧的 DMAC 字段 == 本流地址 → 本流队列 ---------------- */
static int steer_flow(uint16_t pi, const struct rte_ether_addr *mac, int pcp, uint16_t q)
{
	struct rte_flow_attr attr = { .ingress = 1 };
	struct rte_flow_item_eth spec, mask;
	struct rte_flow_item_vlan vspec, vmask;
	memset(&spec, 0, sizeof(spec)); memset(&mask, 0, sizeof(mask));
	memset(&vspec, 0, sizeof(vspec)); memset(&vmask, 0, sizeof(vmask));
	spec.hdr.dst_addr = *mac;
	memset(&mask.hdr.dst_addr, 0xFF, 6);
	vspec.tci = rte_cpu_to_be_16((uint16_t)((pcp & 7) << 13));
	vmask.tci = rte_cpu_to_be_16(0xE000);                     /* 只匹配 PCP */
	struct rte_flow_item pat[] = {
		{ .type = RTE_FLOW_ITEM_TYPE_ETH, .spec = &spec, .mask = &mask },
		{ .type = pcp < 0 ? RTE_FLOW_ITEM_TYPE_END : RTE_FLOW_ITEM_TYPE_VLAN, .spec = &vspec, .mask = &vmask },
		{ .type = RTE_FLOW_ITEM_TYPE_END } };
	struct rte_flow_action_queue qa = { .index = q };
	struct rte_flow_action act[] = {
		{ .type = RTE_FLOW_ACTION_TYPE_QUEUE, .conf = &qa },
		{ .type = RTE_FLOW_ACTION_TYPE_END } };
	struct rte_flow_error err;
	if (rte_flow_create(pi, &attr, pat, act, &err)) return 0;
	printf("警告：port %u 为 queue %u 建立导流规则%s失败（%s）\n", pi, q, pcp < 0 ? "" : "（按 PCP）",
	       err.message ? err.message : "?");
	return -1;
}

/* mlx5 未带推荐 devargs 时告警（未带时包率约低 20%，cyc/pk 明显升高） */
static void check_devargs(uint16_t pi)
{
	struct rte_eth_dev_info di;
	if (rte_eth_dev_info_get(pi, &di) != 0 || !di.driver_name || !strstr(di.driver_name, "mlx5")) return;
	const struct rte_devargs *da = di.device ? rte_dev_devargs(di.device) : NULL;
	const char *a = (da && da->args) ? da->args : "";
	static const char *need[] = { "mprq_en=1", "txq_inline_mpw=" };
	for (unsigned i = 0; i < RTE_DIM(need); i++)
		if (!strstr(a, need[i])) {
			printf("\n*** 警告：port %u（%s）devargs 缺少 %s，当前为 \"%s\"。\n"
			       "*** 建议 -a <BDF>,mprq_en=1,rxqs_min_mprq=1,mprq_log_stride_num=9,txq_inline_mpw=128,rxq_pkt_pad_en=1\n\n",
			       pi, rte_dev_name(di.device), need[i], a);
			return;
		}
}

void port_init(uint16_t pi)
{
	check_devargs(pi);
	int socket = rte_eth_dev_socket_id(pi);
	const int m = ctx_per_flow();
	uint16_t nrxd = NB_RXD, ntxd = NB_TXD, nf = (uint16_t)g_cfg.fpp, nq = (uint16_t)(nf * m);   /* nf：流（MAC）数；nq：队列数 */
	char name[32];
	struct rte_ether_addr pmac;
	struct rte_eth_conf conf; memset(&conf, 0, sizeof(conf));   /* 不开 VLAN strip */

	if (rte_eth_dev_configure(pi, nq, nq, &conf) < 0 ||
	    rte_eth_dev_adjust_nb_rx_tx_desc(pi, &nrxd, &ntxd) < 0)
		rte_exit(EXIT_FAILURE, "port %u configure failed\n", pi);
	rte_eth_macaddr_get(pi, &pmac);

	for (uint16_t q = 0; q < nq; q++) {
		uint16_t f = (uint16_t)(q / m), mf = (uint16_t)(pi * nf + f), fi = (uint16_t)(pi * nq + q);  /* f：端口内流号；mf：全局流号；fi：上下文下标 */
		struct flow_ctx *c = &g_flow[fi];
		c->port = pi; c->q = q; c->flow = f; c->idx = fi;
		c->read = g_cfg.rw ? (q % m == 1) : g_cfg.mix ? (f & 1) : g_cfg.read;   /* rw：偶数队列写、奇数队列读 */
		hdr_flow_addr(pi, f, mf, &pmac, &c->addr);

		snprintf(name, sizeof(name), "rx%u_%u", pi, q);
		c->rx_pool = rte_pktmbuf_pool_create(name, RX_POOL_N, POOL_CACHE, 0, RTE_MBUF_DEFAULT_BUF_SIZE, socket);
		if (!c->rx_pool) rte_exit(EXIT_FAILURE, "rx pool %u.%u\n", pi, q);
		if (rte_eth_rx_queue_setup(pi, q, nrxd, socket, NULL, c->rx_pool) < 0 ||
		    rte_eth_tx_queue_setup(pi, q, ntxd, socket, NULL) < 0)
			rte_exit(EXIT_FAILURE, "port %u queue %u setup failed\n", pi, q);

		tmpl_build(c, &g_tmpl[fi]);
		snprintf(name, sizeof(name), "tmpl%u_%u", pi, q);
		c->tmpl_pool = rte_pktmbuf_pool_create(name, TX_POOL_N, POOL_CACHE, 0, RTE_PKTMBUF_HEADROOM + TMPL_ROOM, socket);
		if (!c->tmpl_pool) rte_exit(EXIT_FAILURE, "tmpl pool %u.%u\n", pi, q);
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
	/*
	 * rw：同一 MAC 的写、读分到两个队列，按 PCP（= VC）区分：
	 *   sender 收 B（VC2）→ 写队列，R（VC3）→ 读队列；reflector 收 AW（VC0）→ 写队列，AR（VC1）→ 读队列
	 */
	int bad = 0;
	for (uint16_t q = 0; q < nq; q++) {
		const struct flow_ctx *c = &g_flow[pi * nq + q];
		const struct rte_ether_addr *a = &c->addr.src;
		if (q % m == 0 && g_cfg.hdr == HDR_ETH && c->flow > 0 &&
		    rte_eth_dev_mac_addr_add(pi, (struct rte_ether_addr *)a, 0) < 0)
			printf("提示：port %u 添加 MAC 过滤 %u 失败，依赖导流规则\n", pi, c->flow);
		int pcp = m == 1 ? -1 : g_cfg.sender ? (c->read ? VC_R : VC_B) : (c->read ? VC_AR : VC_AW);
		bad |= steer_flow(pi, a, pcp, q);
	}
	if (bad && m == 2) rte_exit(EXIT_FAILURE, "rw 需要按 PCP 导流，规则建立失败\n");
	if (g_cfg.promisc || g_cfg.hdr == HDR_SUE || bad) {
		rte_eth_promiscuous_enable(pi);
		if (g_cfg.hdr == HDR_SUE) rte_eth_allmulticast_enable(pi);
		printf("port %u: 混杂模式%s\n", pi, bad ? "（导流规则失败，退回）" : "");
	} else {
		rte_eth_promiscuous_disable(pi);
	}

	for (uint16_t q = 0; q < nq; q++) {
		const struct flow_ctx *c = &g_flow[pi * nq + q];
		char a[32], b[32];
		hdr_addr_str(&c->addr.src, a, sizeof(a));
		hdr_addr_str(&c->addr.dst, b, sizeof(b));
		if (g_cfg.sender)
			printf("flow %u.%u: queue %u  %s  my %s  peer %s\n", pi, c->flow, q, c->read ? "read " : "write", a, b);
		else
			printf("flow %u.%u: queue %u  %s  my %s\n", pi, c->flow, q, m == 2 ? (c->read ? "AR" : "AW") : "", a);
	}
}

/*
 * 让交换机学习本端所有流地址：每条流发几个广播帧（SMAC = 流地址，0x8100 + EtherType 0x9000），
 * 对端按非 AXI 帧计入 ign。纠正此前学错端口的 MAC 表项，避免开头的响应被送到错误端口而丢失。
 */
void port_announce(void)
{
	for (uint16_t i = 0; i < g_nb_flows; i++) {
		struct flow_ctx *c = &g_flow[i];
		if (c->q % ctx_per_flow()) continue;       /* rw：读上下文与写上下文同一 MAC，只发一次 */
		struct rte_mbuf *m[4];
		if (rte_pktmbuf_alloc_bulk(c->rx_pool, m, 4) != 0) continue;
		for (int k = 0; k < 4; k++) {
			uint8_t *f = rte_pktmbuf_mtod(m[k], uint8_t *);
			memset(f, 0, 60);
			memset(f, 0xFF, 6);
			memcpy(f + 6, &c->addr.src, 6);
			f[12] = 0x81; f[13] = 0x00;
			put_be16(f + 14, (uint16_t)(g_cfg.vid & 0xFFF));
			put_be16(f + 16, 0x9000);
			m[k]->data_len = m[k]->pkt_len = 60;
		}
		uint16_t s = rte_eth_tx_burst(c->port, c->q, m, 4);
		if (s < 4) rte_pktmbuf_free_bulk(m + s, 4 - s);
	}
	rte_delay_ms(200);
}

void port_fini(void)
{
	for (uint16_t i = 0; i < g_nb_ports; i++) {
		rte_flow_flush(i, NULL);
		rte_eth_dev_stop(i);
		rte_eth_dev_close(i);
	}
}
