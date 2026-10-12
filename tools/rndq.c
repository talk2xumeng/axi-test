/*
 * rndq：验证 mlx5 能否按 RTE_FLOW_ITEM_TYPE_RANDOM 把同一对 MAC 的包分到多个接收队列
 *
 * 收端（--rx N，N 为 2 的幂）：
 *   group 0：DMAC == 本网卡 MAC → JUMP group 1
 *   group 1 起：随机值二叉树，每层 random & 1 == 0 走左、否则走右，log2(N) 层后各 1/N（见 setup_rules）
 *   --no-random：group 0 直接送队列 0，作单队列对照
 *   每个队列一个核，只收包计数后释放。需 devargs dv_flow_en=2（HW steering，只能用模板 / 异步 API）。
 * 发端（--tx M）：M 个核各占一个发送队列，循环发与 axiperf 写请求同格式的帧
 *   （DMAC · SMAC · 802.1Q · Length · payload），不需要对端回包。
 * 每秒打印各队列包率及合计。
 */
#define ALLOW_EXPERIMENTAL_API
#include <stdio.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <getopt.h>
#include <unistd.h>

#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_flow.h>
#include <rte_mbuf.h>
#include <rte_lcore.h>
#include <rte_cycles.h>
#include <rte_byteorder.h>
#include <rte_errno.h>

#ifdef RNDQ_LOCAL_CHECK          /* 只用于在没有 RANDOM item 的 DPDK 上做语法检查，不能运行 */
#define RTE_FLOW_ITEM_TYPE_RANDOM ((enum rte_flow_item_type)0x7fff)
struct rte_flow_item_random { uint32_t value; };
#endif

#define MAXQ   16
#define BURST  64
#define NB_RXD 4096
#define NB_TXD 4096

static volatile bool quit;
static struct {
	bool tx, no_random, isolate;
	int nq, time, vid, len;
	struct rte_ether_addr dmac;
} cfg = { .nq = 4, .time = 20, .vid = 1, .len = 566 };

struct qstat { uint64_t pkts, bytes; } __rte_cache_aligned;
static struct qstat st[MAXQ];
static uint16_t port;
static struct rte_mempool *pool;

static void on_sig(int s) { (void)s; quit = true; }

static void usage(void)
{
	printf("用法：rndq [EAL 参数] -a <BDF>,<devargs> -- --rx N | --tx M [选项]\n"
	       "  --rx N          收端：N 个接收队列 / 核（2 的幂，≤ %d），devargs 须含 dv_flow_en=2\n"
	       "  --tx M          发端：M 个发送队列 / 核\n"
	       "  --dmac MAC      发端：目的 MAC（收端网卡 MAC）\n"
	       "  --vid N         VLAN ID [1]\n"
	       "  --len N         帧长，含 FCS [566]\n"
	       "  --time S        运行秒数 [20]\n"
	       "  --no-random     收端：不分流，全部进队列 0（对照）\n"
	       "  --isolate       收端：rte_flow_isolate，去掉驱动默认规则\n"
	       "lcore 数须 ≥ 1 + N（或 M）：第一个 lcore 打印统计，其余各管一个队列\n", MAXQ);
}

static void parse(int argc, char **argv)
{
	enum { O_RX = 256, O_TX, O_DMAC, O_VID, O_LEN, O_TIME, O_NORND, O_ISO };
	static const struct option lo[] = {
		{"rx", 1, 0, O_RX}, {"tx", 1, 0, O_TX}, {"dmac", 1, 0, O_DMAC}, {"vid", 1, 0, O_VID},
		{"len", 1, 0, O_LEN}, {"time", 1, 0, O_TIME},
		{"no-random", 0, 0, O_NORND}, {"isolate", 0, 0, O_ISO}, {0, 0, 0, 0}};
	int o, mode = 0;
	while ((o = getopt_long(argc, argv, "", lo, NULL)) != -1) {
		switch (o) {
		case O_RX:    cfg.tx = false; cfg.nq = atoi(optarg); mode++; break;
		case O_TX:    cfg.tx = true;  cfg.nq = atoi(optarg); mode++; break;
		case O_DMAC:  if (rte_ether_unformat_addr(optarg, &cfg.dmac) < 0) { usage(); exit(1); } break;
		case O_VID:   cfg.vid = atoi(optarg) & 0xFFF; break;
		case O_LEN:   cfg.len = atoi(optarg); break;
		case O_TIME:  cfg.time = atoi(optarg); break;
		case O_NORND: cfg.no_random = true; break;
		case O_ISO:   cfg.isolate = true; break;
		default: usage(); exit(1);
		}
	}
	if (mode != 1 || cfg.nq < 1 || cfg.nq > MAXQ || cfg.len < 64 || cfg.len > 1518 ||
	    (!cfg.tx && ((cfg.nq & (cfg.nq - 1)) || (cfg.nq == 1 && !cfg.no_random)))) {
		usage(); exit(1);
	}
}

/* ---------------- 收端：模板表 ---------------- */

static int ferr(const char *what, int ret, struct rte_flow_error *e)
{
	printf("%s 失败：%d %s\n", what, ret, e->message ? e->message : "(无说明)");
	return -1;
}

/* 异步下发一条规则并等它完成 */
static int async_rule(struct rte_flow_template_table *t, const struct rte_flow_item *pat,
                      const struct rte_flow_action *act, const char *what)
{
	struct rte_flow_error e = {0};
	const struct rte_flow_op_attr oa = { .postpone = 0 };
	struct rte_flow *f = rte_flow_async_create(port, 0, &oa, t, pat, 0, act, 0, NULL, &e);
	if (!f) return ferr(what, -rte_errno, &e);
	rte_flow_push(port, 0, &e);
	struct rte_flow_op_result r[4];
	uint64_t t0 = rte_get_tsc_cycles();
	for (;;) {
		int n = rte_flow_pull(port, 0, r, 4, &e);
		if (n < 0) return ferr(what, n, &e);
		if (n > 0) {
			if (r[0].status != RTE_FLOW_OP_SUCCESS) { printf("%s：规则下发结果失败\n", what); return -1; }
			return 0;
		}
		if (rte_get_tsc_cycles() - t0 > rte_get_tsc_hz()) { printf("%s：1 s 内未完成\n", what); return -1; }
	}
}

static struct rte_flow_template_table *table(uint32_t group, uint32_t prio, uint32_t nb,
                                             struct rte_flow_pattern_template *pt,
                                             struct rte_flow_actions_template *at, const char *what)
{
	struct rte_flow_error e = {0};
	struct rte_flow_template_table_attr ta;
	memset(&ta, 0, sizeof(ta));
	ta.flow_attr.group = group;
	ta.flow_attr.priority = prio;
	ta.flow_attr.ingress = 1;
	ta.nb_flows = nb;
	struct rte_flow_template_table *t = rte_flow_template_table_create(port, &ta, &pt, 1, &at, 1, &e);
	if (!t) ferr(what, -rte_errno, &e);
	return t;
}

static int setup_rules(void)
{
	struct rte_flow_error e = {0};
	struct rte_ether_addr my;
	rte_eth_macaddr_get(port, &my);

	/* group 0：DMAC == 本网卡 MAC */
	const struct rte_flow_pattern_template_attr pa = { .relaxed_matching = 1, .ingress = 1 };
	struct rte_flow_item_eth eth_m;
	memset(&eth_m, 0, sizeof(eth_m));
	memset(&eth_m, 0xFF, RTE_ETHER_ADDR_LEN);            /* 目的 MAC 在结构体开头 */
	const struct rte_flow_item p0[] = {
		{ .type = RTE_FLOW_ITEM_TYPE_ETH, .mask = &eth_m },
		{ .type = RTE_FLOW_ITEM_TYPE_END },
	};
	struct rte_flow_pattern_template *pt0 = rte_flow_pattern_template_create(port, &pa, p0, &e);
	if (!pt0) return ferr("group 0 pattern template", -rte_errno, &e);

	const struct rte_flow_actions_template_attr aa = { .ingress = 1 };
	struct rte_flow_action_jump jmp = { .group = 1 };
	struct rte_flow_action_queue q0 = { .index = 0 };
	const struct rte_flow_action a_jump[] = {
		{ .type = RTE_FLOW_ACTION_TYPE_JUMP, .conf = &jmp }, { .type = RTE_FLOW_ACTION_TYPE_END } };
	const struct rte_flow_action a_q[] = {                 /* 掩码 conf 为空：队列号由每条规则给出 */
		{ .type = RTE_FLOW_ACTION_TYPE_QUEUE }, { .type = RTE_FLOW_ACTION_TYPE_END } };
	struct rte_flow_actions_template *at_jump = rte_flow_actions_template_create(port, &aa, a_jump, a_jump, &e);
	if (!at_jump) return ferr("jump actions template", -rte_errno, &e);
	struct rte_flow_actions_template *at_q = rte_flow_actions_template_create(port, &aa, a_q, a_q, &e);
	if (!at_q) return ferr("queue actions template", -rte_errno, &e);

	struct rte_flow_template_table *t0 = table(0, 0, 1, pt0, cfg.no_random ? at_q : at_jump, "group 0 table");
	if (!t0) return -1;
	struct rte_flow_item_eth eth_s;
	memset(&eth_s, 0, sizeof(eth_s));
	memcpy(&eth_s, &my, RTE_ETHER_ADDR_LEN);
	const struct rte_flow_item r0[] = {
		{ .type = RTE_FLOW_ITEM_TYPE_ETH, .spec = &eth_s }, { .type = RTE_FLOW_ITEM_TYPE_END } };
	const struct rte_flow_action ra_q0[] = {
		{ .type = RTE_FLOW_ACTION_TYPE_QUEUE, .conf = &q0 }, { .type = RTE_FLOW_ACTION_TYPE_END } };
	if (async_rule(t0, r0, cfg.no_random ? ra_q0 : a_jump, "group 0 规则") < 0) return -1;
	if (cfg.no_random) { printf("--no-random：DMAC 匹配后全部进队列 0\n"); return 0; }

	/*
	 * group 1 起：随机值分流。mlx5 每查一张表重新取一次随机值，所以 N 张表各匹配 random & (N-1) == i
	 * 会得到 1/4、3/16、9/64… 的几何分布，且约 (3/4)^N 的包一张都不中而被丢（10-12 03 实测）。
	 * 改为二叉树：每个节点一个 group、两张表——
	 *   优先级 0：random & 1 == 0 → 左子（1/2）；优先级 1：全匹配 → 右子（其余 1/2）。
	 * 每层独立 1/2，log2(N) 层后各队列 1/N，且每个包都有去处。
	 * 节点按堆编号：根 = group 1，节点 n 的子节点 2n、2n+1；n ≥ N 为叶子 = 队列 n − N。
	 */
	struct rte_flow_item_random rnd_m = { .value = 1 };
	const struct rte_flow_item p1[] = {
		{ .type = RTE_FLOW_ITEM_TYPE_RANDOM, .mask = &rnd_m }, { .type = RTE_FLOW_ITEM_TYPE_END } };
	struct rte_flow_pattern_template *pt1 = rte_flow_pattern_template_create(port, &pa, p1, &e);
	if (!pt1) return ferr("random pattern template", -rte_errno, &e);
	struct rte_flow_item_eth any_m;                        /* 全匹配：ETH 掩码全 0 */
	memset(&any_m, 0, sizeof(any_m));
	const struct rte_flow_item pany[] = {
		{ .type = RTE_FLOW_ITEM_TYPE_ETH, .mask = &any_m }, { .type = RTE_FLOW_ITEM_TYPE_END } };
	struct rte_flow_pattern_template *ptany = rte_flow_pattern_template_create(port, &pa, pany, &e);
	if (!ptany) return ferr("match-all pattern template", -rte_errno, &e);
	const struct rte_flow_action a_j[] = {                 /* 掩码 conf 为空：跳转目标由每条规则给出 */
		{ .type = RTE_FLOW_ACTION_TYPE_JUMP }, { .type = RTE_FLOW_ACTION_TYPE_END } };
	struct rte_flow_actions_template *at_j = rte_flow_actions_template_create(port, &aa, a_j, a_j, &e);
	if (!at_j) return ferr("jump(any) actions template", -rte_errno, &e);

	struct rte_flow_item_random rnd_s = { .value = 0 };
	const struct rte_flow_item r_rnd[] = {
		{ .type = RTE_FLOW_ITEM_TYPE_RANDOM, .spec = &rnd_s }, { .type = RTE_FLOW_ITEM_TYPE_END } };
	struct rte_flow_item_eth any_s;
	memset(&any_s, 0, sizeof(any_s));
	const struct rte_flow_item r_any[] = {
		{ .type = RTE_FLOW_ITEM_TYPE_ETH, .spec = &any_s }, { .type = RTE_FLOW_ITEM_TYPE_END } };

	const int N = cfg.nq;
	for (int n = 1; n < N; n++) {                          /* 内部节点 */
		for (int side = 0; side < 2; side++) {
			const int child = 2 * n + side;
			const bool leaf = child >= N;
			char what[64];
			snprintf(what, sizeof(what), "group %d %s表", n, side ? "全匹配" : "random ");
			struct rte_flow_template_table *t = table((uint32_t)n, (uint32_t)side, 1, side ? ptany : pt1,
			                                          leaf ? at_q : at_j, what);
			if (!t) return -1;
			struct rte_flow_action_queue qa = { .index = (uint16_t)(child - N) };
			struct rte_flow_action_jump ja = { .group = (uint32_t)child };
			const struct rte_flow_action act[] = {
				{ .type = leaf ? RTE_FLOW_ACTION_TYPE_QUEUE : RTE_FLOW_ACTION_TYPE_JUMP,
				  .conf = leaf ? (const void *)&qa : (const void *)&ja },
				{ .type = RTE_FLOW_ACTION_TYPE_END } };
			snprintf(what, sizeof(what), "group %d %s规则", n, side ? "全匹配" : "random ");
			if (async_rule(t, side ? r_any : r_rnd, act, what) < 0) return -1;
		}
	}
	printf("随机值二叉树分流：%d 个队列，group 1~%d，每层 1/2\n", N, N - 1);
	return 0;
}

/* ---------------- 数据面 ---------------- */

static int rx_loop(void *arg)
{
	uint16_t q = (uint16_t)(uintptr_t)arg;
	struct rte_mbuf *m[BURST];
	struct qstat *s = &st[q];
	while (!quit) {
		uint16_t n = rte_eth_rx_burst(port, q, m, BURST);
		if (!n) continue;
		uint64_t b = 0;
		for (uint16_t i = 0; i < n; i++) b += m[i]->pkt_len;
		rte_pktmbuf_free_bulk(m, n);
		s->pkts += n; s->bytes += b;
	}
	return 0;
}

static int tx_loop(void *arg)
{
	uint16_t q = (uint16_t)(uintptr_t)arg;
	struct rte_mbuf *m[BURST];
	struct qstat *s = &st[q];
	const uint16_t dl = (uint16_t)(cfg.len - RTE_ETHER_CRC_LEN);
	while (!quit) {
		if (rte_pktmbuf_alloc_bulk(pool, m, BURST) != 0) continue;
		for (int i = 0; i < BURST; i++) m[i]->data_len = m[i]->pkt_len = dl;   /* 内容已在池初始化时写好 */
		uint16_t n = rte_eth_tx_burst(port, q, m, BURST);
		if (n < BURST) rte_pktmbuf_free_bulk(m + n, BURST - n);
		s->pkts += n; s->bytes += (uint64_t)n * dl;
	}
	return 0;
}

/* 发端模板帧：DMAC · SMAC · 0x8100 · PCP0/VID · Length · payload(0xA5) */
static void fill_frame(struct rte_mempool *mp, void *arg, void *obj, unsigned idx)
{
	(void)mp; (void)idx;
	const struct rte_ether_addr *src = arg;
	struct rte_mbuf *mb = obj;
	uint8_t *f = rte_pktmbuf_mtod(mb, uint8_t *);
	const uint16_t dl = (uint16_t)(cfg.len - RTE_ETHER_CRC_LEN), plen = (uint16_t)(dl - 18);
	memcpy(f, &cfg.dmac, 6); memcpy(f + 6, src, 6);
	f[12] = 0x81; f[13] = 0x00;
	f[14] = (uint8_t)(cfg.vid >> 8); f[15] = (uint8_t)cfg.vid;
	f[16] = (uint8_t)(plen >> 8); f[17] = (uint8_t)plen;
	memset(f + 18, 0xA5, plen);
}

int main(int argc, char **argv)
{
	int ret = rte_eal_init(argc, argv);
	if (ret < 0) { printf("EAL 初始化失败\n"); return 1; }
	parse(argc - ret, argv + ret);
	if (rte_eth_dev_count_avail() < 1) { printf("没有可用端口\n"); return 1; }
	if (rte_lcore_count() < (unsigned)cfg.nq + 1) { printf("lcore 不够：要 1 + %d 个\n", cfg.nq); return 1; }
	signal(SIGINT, on_sig); signal(SIGTERM, on_sig);
	port = 0;
	const int sock = rte_eth_dev_socket_id(port);

	const unsigned npool = cfg.tx ? 65535u : (unsigned)cfg.nq * NB_RXD * 2 + 8191u;
	pool = rte_pktmbuf_pool_create("rndq", npool, 512, 0, RTE_MBUF_DEFAULT_BUF_SIZE, sock < 0 ? 0 : sock);
	if (!pool) { printf("mbuf 池创建失败\n"); return 1; }

	struct rte_eth_conf pc;
	memset(&pc, 0, sizeof(pc));
	const uint16_t nrx = cfg.tx ? 1 : (uint16_t)cfg.nq, ntx = cfg.tx ? (uint16_t)cfg.nq : 1;
	if ((ret = rte_eth_dev_configure(port, nrx, ntx, &pc)) < 0) { printf("configure 失败 %d\n", ret); return 1; }
	for (uint16_t q = 0; q < nrx; q++)
		if (rte_eth_rx_queue_setup(port, q, NB_RXD, sock, NULL, pool) < 0) { printf("rxq %u 失败\n", q); return 1; }
	for (uint16_t q = 0; q < ntx; q++)
		if (rte_eth_tx_queue_setup(port, q, NB_TXD, sock, NULL) < 0) { printf("txq %u 失败\n", q); return 1; }

	struct rte_ether_addr my;
	rte_eth_macaddr_get(port, &my);
	char ms[RTE_ETHER_ADDR_FMT_SIZE];
	rte_ether_format_addr(ms, sizeof(ms), &my);

	if (!cfg.tx) {
		struct rte_flow_error e = {0};
		if (cfg.isolate && rte_flow_isolate(port, 1, &e) < 0) return ferr("rte_flow_isolate", -rte_errno, &e), 1;
		const struct rte_flow_port_attr pa = { 0 };
		const struct rte_flow_queue_attr qa = { .size = 64 };
		const struct rte_flow_queue_attr *qav[1] = { &qa };
		if ((ret = rte_flow_configure(port, &pa, 1, qav, &e)) < 0) return ferr("rte_flow_configure（devargs 要 dv_flow_en=2）", ret, &e), 1;
	} else {
		rte_mempool_obj_iter(pool, fill_frame, &my);
	}
	if ((ret = rte_eth_dev_start(port)) < 0) { printf("start 失败 %d\n", ret); return 1; }
	if (!cfg.tx && setup_rules() < 0) { rte_eth_dev_stop(port); return 1; }
	printf("%s 端口 %u MAC %s，%d 个队列，帧长 %d\n", cfg.tx ? "发端" : "收端", port, ms, cfg.nq, cfg.len);

	unsigned lc = rte_get_next_lcore(-1, 1, 0);
	for (int q = 0; q < cfg.nq; q++, lc = rte_get_next_lcore(lc, 1, 0))
		rte_eal_remote_launch(cfg.tx ? tx_loop : rx_loop, (void *)(uintptr_t)q, lc);

	struct qstat prev[MAXQ];
	memset(prev, 0, sizeof(prev));
	for (int t = 1; t <= cfg.time && !quit; t++) {
		sleep(1);
		uint64_t tp = 0, tb = 0;
		printf("%3ds", t);
		for (int q = 0; q < cfg.nq; q++) {
			uint64_t p = st[q].pkts, b = st[q].bytes;
			uint64_t dp = p - prev[q].pkts, db = b - prev[q].bytes;
			prev[q].pkts = p; prev[q].bytes = b;
			tp += dp; tb += db;
			printf("  q%d %6.2f", q, dp / 1e6);
		}
		/* 二层 Gbps：帧长含 FCS */
		printf("  | 合计 %6.2f Mpps  %6.1f Gbps\n", tp / 1e6, (tb + tp * RTE_ETHER_CRC_LEN) * 8 / 1e9);
	}
	quit = true;
	rte_eal_mp_wait_lcore();

	struct rte_eth_stats es;
	if (rte_eth_stats_get(port, &es) == 0)
		printf("网卡计数：ipackets %" PRIu64 " imissed %" PRIu64 " ierrors %" PRIu64 " rx_nombuf %" PRIu64 " opackets %" PRIu64 "\n",
		       es.ipackets, es.imissed, es.ierrors, es.rx_nombuf, es.opackets);
	/* 网卡扩展计数：物理口收到多少、在哪丢（rx_packets_phy、rx_discards_phy、rx_out_of_buffer 等） */
	int nx = rte_eth_xstats_get_names(port, NULL, 0);
	if (nx > 0) {
		struct rte_eth_xstat_name *nm = calloc((size_t)nx, sizeof(*nm));
		struct rte_eth_xstat *xv = calloc((size_t)nx, sizeof(*xv));
		if (nm && xv && rte_eth_xstats_get_names(port, nm, (unsigned)nx) == nx && rte_eth_xstats_get(port, xv, (unsigned)nx) == nx) {
			static const char *keys[] = { "phy", "discard", "out_of_buffer", "miss", "drop", "steer", NULL };
			for (int i = 0; i < nx; i++) {
				if (!xv[i].value) continue;
				for (const char **k = keys; *k; k++)
					if (strstr(nm[xv[i].id].name, *k)) { printf("  %-36s %" PRIu64 "\n", nm[xv[i].id].name, xv[i].value); break; }
			}
		}
		free(nm); free(xv);
	}
	rte_eth_dev_stop(port);
	rte_eal_cleanup();
	return 0;
}
