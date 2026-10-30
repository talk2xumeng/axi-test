/*
 * 端口与流初始化
 */
#define ALLOW_EXPERIMENTAL_API          /* 模板 / 异步 rte_flow（--rx-split） */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <net/if.h>
#include <sys/socket.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <linux/dcbnl.h>

#include <rte_ethdev.h>
#include <rte_flow.h>
#include <rte_debug.h>
#include <rte_cycles.h>
#include <rte_dev.h>
#include <rte_devargs.h>
#include <rte_errno.h>

#ifdef AXIPERF_NO_RANDOM_ITEM           /* 仅供在没有 RANDOM item 的 DPDK 上做编译检查，不能运行 */
#define RTE_FLOW_ITEM_TYPE_RANDOM ((enum rte_flow_item_type)0x7fff)
struct rte_flow_item_random { uint32_t value; };
#endif

#include "port.h"
#include "hdr.h"
#include "axi.h"
#include "hwts.h"

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


/* ---------------- --rx-split：同一对 MAC 按随机值均分到 N 个接收队列（mlx5 HW steering，模板 / 异步 API） ----------------
 *
 * 经 E100c 后反射端只看到一对 MAC、一个 VID，按地址导流只能进一个队列、一个核。
 * mlx5 可以匹配网卡给每个包生成的随机值（RTE_FLOW_ITEM_TYPE_RANDOM，需 dv_flow_en=2），但每查一张表重新取一次值，
 * 且每张表只能放一条带随机值的规则（nb_flows = 1）。所以用二叉树，每个节点一个 group、两张表：
 *   优先级 0：random & 1 == 0 → 左子；优先级 1：全匹配 → 右子。每层独立 1/2，log2(N) 层后各队列 1/N，没有包落空。
 * 节点按堆编号：根为 1，节点 n 的子节点 2n、2n+1，n ≥ N 为叶子 = 第 n − N 个上下文。
 * group 0：DMAC == 网卡 MAC（rw 时再按 PCP 分写、读）→ 跳到该树的根。rw 时写树占 group 1 ~ N−1，读树占 N ~ 2N−2。
 * 2026-10-12 rndq 在 03 上实测：4 个队列各 21.1 Mpps，合计 84.6 Mpps（线速 85.3），须开 MPRQ 与 CQE 压缩。
 */
#ifdef HAVE_FLOW_RANDOM
static struct rte_flow_template_table *hws_table(uint16_t pi, uint32_t group, uint32_t prio,
                                                 struct rte_flow_pattern_template *pt,
                                                 struct rte_flow_actions_template *at, const char *what)
{
	struct rte_flow_error e = {0};
	struct rte_flow_template_table_attr ta;
	memset(&ta, 0, sizeof(ta));
	ta.flow_attr.group = group;
	ta.flow_attr.priority = prio;
	ta.flow_attr.ingress = 1;
	ta.nb_flows = 1;
	struct rte_flow_template_table *t = rte_flow_template_table_create(pi, &ta, &pt, 1, &at, 1, &e);
	if (!t) rte_exit(EXIT_FAILURE, "port %u --rx-split：建表 %s 失败：%s\n", pi, what, e.message ? e.message : "?");
	return t;
}

static void hws_rule(uint16_t pi, struct rte_flow_template_table *t, const struct rte_flow_item *pat,
                     const struct rte_flow_action *act, const char *what)
{
	struct rte_flow_error e = {0};
	const struct rte_flow_op_attr oa = { .postpone = 0 };
	if (!rte_flow_async_create(pi, 0, &oa, t, pat, 0, act, 0, NULL, &e))
		rte_exit(EXIT_FAILURE, "port %u --rx-split：规则 %s 失败：%s\n", pi, what, e.message ? e.message : "?");
	rte_flow_push(pi, 0, &e);
	struct rte_flow_op_result r[4];
	const uint64_t t0 = rte_get_tsc_cycles();
	for (;;) {
		int n = rte_flow_pull(pi, 0, r, 4, &e);
		if (n < 0) rte_exit(EXIT_FAILURE, "port %u --rx-split：规则 %s 下发失败\n", pi, what);
		if (n > 0) {
			if (r[0].status != RTE_FLOW_OP_SUCCESS) rte_exit(EXIT_FAILURE, "port %u --rx-split：规则 %s 结果失败\n", pi, what);
			return;
		}
		if (rte_get_tsc_cycles() - t0 > rte_get_tsc_hz()) rte_exit(EXIT_FAILURE, "port %u --rx-split：规则 %s 超时\n", pi, what);
	}
}

static void split_configure(uint16_t pi)       /* 须在 rte_eth_dev_start 之前 */
{
	struct rte_flow_error e = {0};
	const struct rte_flow_port_attr pa = { 0 };
	const struct rte_flow_queue_attr qa = { .size = 64 };
	const struct rte_flow_queue_attr *qav[1] = { &qa };
	int r = rte_flow_configure(pi, &pa, 1, qav, &e);
	if (r < 0) rte_exit(EXIT_FAILURE, "port %u --rx-split：rte_flow_configure 失败（%d %s），devargs 须加 dv_flow_en=2\n",
	                    pi, r, e.message ? e.message : "");
}

static void split_rules(uint16_t pi, const struct rte_ether_addr *mac, int m, int N)
{
	struct rte_flow_error e = {0};
	const struct rte_flow_pattern_template_attr pa = { .relaxed_matching = 1, .ingress = 1 };
	const struct rte_flow_actions_template_attr aa = { .ingress = 1 };
	char what[64];

	/* 模板：group 0 匹配 DMAC（rw 再加 PCP）；树内 random 位 / 全匹配；动作 JUMP / QUEUE 的参数由每条规则给出 */
	struct rte_flow_item_eth em, es, anym, anys;
	struct rte_flow_item_vlan vm, vs;
	memset(&em, 0, sizeof(em)); memset(&es, 0, sizeof(es)); memset(&anym, 0, sizeof(anym)); memset(&anys, 0, sizeof(anys));
	memset(&vm, 0, sizeof(vm)); memset(&vs, 0, sizeof(vs));
	memset(&em.hdr.dst_addr, 0xFF, RTE_ETHER_ADDR_LEN);
	es.hdr.dst_addr = *mac;
	vm.tci = rte_cpu_to_be_16(0xE000);
	const struct rte_flow_item p0[] = {
		{ .type = RTE_FLOW_ITEM_TYPE_ETH, .mask = &em },
		{ .type = m == 2 ? RTE_FLOW_ITEM_TYPE_VLAN : RTE_FLOW_ITEM_TYPE_END, .mask = &vm },
		{ .type = RTE_FLOW_ITEM_TYPE_END } };
	struct rte_flow_item_random rm = { .value = 1 }, rs = { .value = 0 };
	const struct rte_flow_item prnd[] = {
		{ .type = RTE_FLOW_ITEM_TYPE_RANDOM, .mask = &rm }, { .type = RTE_FLOW_ITEM_TYPE_END } };
	const struct rte_flow_item pany[] = {
		{ .type = RTE_FLOW_ITEM_TYPE_ETH, .mask = &anym }, { .type = RTE_FLOW_ITEM_TYPE_END } };
	struct rte_flow_pattern_template *pt0 = rte_flow_pattern_template_create(pi, &pa, p0, &e);
	struct rte_flow_pattern_template *ptr = pt0 ? rte_flow_pattern_template_create(pi, &pa, prnd, &e) : NULL;
	struct rte_flow_pattern_template *pta = ptr ? rte_flow_pattern_template_create(pi, &pa, pany, &e) : NULL;
	if (!pta) rte_exit(EXIT_FAILURE, "port %u --rx-split：匹配模板失败：%s\n", pi, e.message ? e.message : "?");
	const struct rte_flow_action aj[] = { { .type = RTE_FLOW_ACTION_TYPE_JUMP }, { .type = RTE_FLOW_ACTION_TYPE_END } };
	const struct rte_flow_action aq[] = { { .type = RTE_FLOW_ACTION_TYPE_QUEUE }, { .type = RTE_FLOW_ACTION_TYPE_END } };
	struct rte_flow_actions_template *atj = rte_flow_actions_template_create(pi, &aa, aj, aj, &e);
	struct rte_flow_actions_template *atq = atj ? rte_flow_actions_template_create(pi, &aa, aq, aq, &e) : NULL;
	if (!atq) rte_exit(EXIT_FAILURE, "port %u --rx-split：动作模板失败：%s\n", pi, e.message ? e.message : "?");

	const struct rte_flow_item rrnd[] = { { .type = RTE_FLOW_ITEM_TYPE_RANDOM, .spec = &rs }, { .type = RTE_FLOW_ITEM_TYPE_END } };
	const struct rte_flow_item rany[] = { { .type = RTE_FLOW_ITEM_TYPE_ETH, .spec = &anys }, { .type = RTE_FLOW_ITEM_TYPE_END } };

	for (int c = 0; c < m; c++) {                  /* c：上下文类型（rw 时 0 = 写，1 = 读） */
		const uint32_t off = (uint32_t)(c * (N - 1));   /* 本树节点 n 在 group off + n */
		/* group 0：DMAC（+ PCP）→ 本树根。每个 (group, 优先级) 一张表 */
		struct rte_flow_template_table *t0 = hws_table(pi, 0, (uint32_t)c, pt0, atj, "group 0");
		vs.tci = rte_cpu_to_be_16((uint16_t)((c ? VC_AR : VC_AW) << 13));
		const struct rte_flow_item r0[] = {
			{ .type = RTE_FLOW_ITEM_TYPE_ETH, .spec = &es },
			{ .type = m == 2 ? RTE_FLOW_ITEM_TYPE_VLAN : RTE_FLOW_ITEM_TYPE_END, .spec = &vs },
			{ .type = RTE_FLOW_ITEM_TYPE_END } };
		struct rte_flow_action_jump j0 = { .group = off + 1 };
		const struct rte_flow_action a0[] = { { .type = RTE_FLOW_ACTION_TYPE_JUMP, .conf = &j0 }, { .type = RTE_FLOW_ACTION_TYPE_END } };
		hws_rule(pi, t0, r0, a0, "group 0");
		/* 树：内部节点 1 .. N−1 */
		for (int n = 1; n < N; n++) {
			for (int side = 0; side < 2; side++) {
				const int child = 2 * n + side;
				const bool leaf = child >= N;
				snprintf(what, sizeof(what), "group %u %s", off + (uint32_t)n, side ? "全匹配" : "random");
				struct rte_flow_template_table *t = hws_table(pi, off + (uint32_t)n, (uint32_t)side,
				                                              side ? pta : ptr, leaf ? atq : atj, what);
				struct rte_flow_action_queue qa = { .index = (uint16_t)((child - N) * m + c) };   /* 与 port_init 的队列编排一致 */
				struct rte_flow_action_jump ja = { .group = off + (uint32_t)child };
				const struct rte_flow_action act[] = {
					{ .type = leaf ? RTE_FLOW_ACTION_TYPE_QUEUE : RTE_FLOW_ACTION_TYPE_JUMP,
					  .conf = leaf ? (const void *)&qa : (const void *)&ja },
					{ .type = RTE_FLOW_ACTION_TYPE_END } };
				hws_rule(pi, t, side ? rany : rrnd, act, what);
			}
		}
	}
	printf("port %u: --rx-split %d：网卡按随机值均分到 %d 个%s接收队列（%d 层二叉树）\n", pi, N, N,
	       m == 2 ? "写、读各 " : "", __builtin_ctz((unsigned)N));
}
#else                                           /* main 已拒绝 --rx-split，不会走到这里 */
static void split_configure(uint16_t pi) { (void)pi; }
static void split_rules(uint16_t pi, const struct rte_ether_addr *mac, int m, int N) { (void)pi; (void)mac; (void)m; (void)N; }
#endif

/*
 * mlx5 devargs 检查，按 --flows 区分：
 *   单流：关多包接收（不带 mprq_en=1）、关 CQE 压缩。单流收的是 542~566B 大包，多包接收要给每包挂外挂缓冲、
 *         做引用计数和回调释放，占反射端约 40% 的周期；关掉后单流写 63.0 → 66.3G。关 CQE 压缩满载 RTT 约低 3.5 µs。
 *   多流：开多包接收（发送端要以几十 Mpps 收 64B 写响应）、开 CQE 压缩。单卡 8 流写关压缩时只有 45 Mpps
 *         （开时 78.6），16 流仍约 46 Mpps，开销随网卡总包率上升，加核无效。
 *   发送内联：每端口发送队列少于 txqs_min_inline（默认 8）时 mlx5 整个关掉内联，txq_inline_mpw 不生效，
 *         须加 txqs_min_inline=1。只内联 ≤ 128B 的小包：10-26 直连实测空载 RTT 6.6 → 5.85 µs，单流读 18.2 → 19.5 Mpps；
 *         大包也内联（txq_inline_max ≥ 566）空载 5.15 µs，但网卡每发送队列只有约 2.1 Mpps，满载不可用。
 */
#define DEV_SINGLE "txqs_min_inline=1,txq_inline_mpw=128,rxq_pkt_pad_en=1,rxq_cqe_comp_en=0"
#define DEV_MULTI  "mprq_en=1,rxqs_min_mprq=1,mprq_log_stride_num=9,txqs_min_inline=1,txq_inline_mpw=128,rxq_pkt_pad_en=1,rxq_cqe_comp_en=1"
static void check_devargs(uint16_t pi)
{
	struct rte_eth_dev_info di;
	if (rte_eth_dev_info_get(pi, &di) != 0 || !di.driver_name || !strstr(di.driver_name, "mlx5")) return;
	const struct rte_devargs *da = di.device ? rte_dev_devargs(di.device) : NULL;
	const char *a = (da && da->args) ? da->args : "";
	const bool single = g_cfg.fpp == 1;
	const bool mprq = strstr(a, "mprq_en=1") != NULL, cqe_off = strstr(a, "rxq_cqe_comp_en=0") != NULL;
	const char *why = NULL;
	printf("port %u（%s）devargs: \"%s\"\n", pi, rte_dev_name(di.device), a);
	if (!strstr(a, "txq_inline_mpw="))
		why = "txq_inline_mpw=128（发送内联，未带时包率约低 20%）";
	else if (g_cfg.fpp * ctx_per_flow() * tx_lanes() < 8 && !strstr(a, "txqs_min_inline="))
		why = "txqs_min_inline=1（发送队列少于 8 个时 mlx5 不做内联，txq_inline_mpw 不生效；加上后空载 RTT 约低 0.75 µs）";
	else if (single && mprq)
		why = "去掉 mprq_en=1（单流收大包，多包接收的外挂缓冲开销大，关掉后带宽约高 5%）";
	else if (single && !cqe_off)
		why = "rxq_cqe_comp_en=0（单流关 CQE 压缩，满载 RTT 约低 3.5 µs）";
	else if (!single && !mprq)
		why = "mprq_en=1（多流高包率时须开；总包率在约 40 Mpps 以下、要低时延时可不开，如总在途 512 的 E100c 等效测试）";
	else if (!single && cqe_off)
		why = "rxq_cqe_comp_en=1（多流高包率时须开，关闭时单卡约 46 Mpps 即丢包；总包率低、要低时延时可关，满载 RTT 约低 2 µs）";
	if (g_cfg.rx_split && !strstr(a, "dv_flow_en=2"))
		rte_exit(EXIT_FAILURE, "port %u（%s）：--rx-split 需要 HW steering，devargs 须加 dv_flow_en=2，例如 -a <BDF>,dv_flow_en=2,%s\n",
		         pi, rte_dev_name(di.device), DEV_MULTI);
	if (why)
		printf("\n*** 警告：port %u（%s）devargs 建议 %s，当前为 \"%s\"。\n"
		       "*** %s建议 -a <BDF>,%s\n\n", pi, rte_dev_name(di.device), why, a,
		       single ? "单流" : "多流", single ? DEV_SINGLE : DEV_MULTI);
}

/*
 * 网卡 TX 按 TC 严格优先级调度（mlnx_qos 默认 tsa vendor；PCP 1 默认在 tc0，最低）。同一端口既发写又发读时
 * （发送端 AW / AR，反射端 B / R），只要高 TC 有帧排队，低 TC 就发不出去：10-26 直连读写并发 max RTT 103 ms，
 * 超时计入 lost / err；全部优先级归一个 TC 后 max < 210 µs。
 * 用 DCB netlink 读 IEEE ETS（与 mlnx_qos 同源）：PCP 0~3 分在不同 TC、且其中有非 ETS 的 TC 时告警。
 */
static int ieee_ets_get(const char *ifname, struct ieee_ets *ets)
{
	int fd = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
	if (fd < 0) return -1;
	struct { struct nlmsghdr nh; struct dcbmsg dm; char buf[64]; } req;
	memset(&req, 0, sizeof(req));
	req.nh.nlmsg_type = RTM_GETDCB;
	req.nh.nlmsg_flags = NLM_F_REQUEST;
	req.dm.dcb_family = AF_UNSPEC;
	req.dm.cmd = DCB_CMD_IEEE_GET;
	struct rtattr *a = (struct rtattr *)req.buf;
	a->rta_type = DCB_ATTR_IFNAME;
	a->rta_len = (unsigned short)RTA_LENGTH(strlen(ifname) + 1);
	memcpy(RTA_DATA(a), ifname, strlen(ifname) + 1);
	req.nh.nlmsg_len = NLMSG_LENGTH(sizeof(req.dm)) + RTA_ALIGN(a->rta_len);
	static char rsp[16384];
	int r = -1;
	ssize_t n = send(fd, &req, req.nh.nlmsg_len, 0) < 0 ? -1 : recv(fd, rsp, sizeof(rsp), 0);
	const struct nlmsghdr *nh = (const struct nlmsghdr *)rsp;
	if (n > 0 && NLMSG_OK(nh, (unsigned)n) && nh->nlmsg_type == RTM_GETDCB) {
		int len = (int)NLMSG_PAYLOAD(nh, sizeof(struct dcbmsg));
		for (struct rtattr *t = (struct rtattr *)((char *)NLMSG_DATA(nh) + NLMSG_ALIGN(sizeof(struct dcbmsg)));
		     RTA_OK(t, len); t = RTA_NEXT(t, len)) {
			if (t->rta_type != DCB_ATTR_IEEE) continue;
			int l2 = (int)RTA_PAYLOAD(t);
			for (struct rtattr *u = RTA_DATA(t); RTA_OK(u, l2); u = RTA_NEXT(u, l2))
				if (u->rta_type == DCB_ATTR_IEEE_ETS && RTA_PAYLOAD(u) >= sizeof(*ets)) { memcpy(ets, RTA_DATA(u), sizeof(*ets)); r = 0; }
		}
	}
	close(fd);
	return r;
}

static void check_tc(uint16_t pi)
{
	if (!g_cfg.rw && !g_cfg.mix) return;         /* 只写或只读时一个端口只发一个 PCP，不受影响 */
	struct rte_eth_dev_info di;
	if (rte_eth_dev_info_get(pi, &di) != 0 || !di.device || !di.driver_name || !strstr(di.driver_name, "mlx5")) return;
	char path[128], ifname[IF_NAMESIZE] = "";
	snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/net", rte_dev_name(di.device));
	DIR *d = opendir(path);
	if (d) {
		for (struct dirent *e; (e = readdir(d)) != NULL; )
			if (e->d_name[0] != '.' && strlen(e->d_name) < sizeof(ifname)) { memcpy(ifname, e->d_name, strlen(e->d_name) + 1); break; }
		closedir(d);
	}
	struct ieee_ets ets;
	if (!ifname[0] || ieee_ets_get(ifname, &ets) < 0) {
		printf("提示：port %u 读不到网卡 TC 配置（%s），未检查 TC 严格优先级\n", pi, ifname[0] ? ifname : "无 netdev");
		return;
	}
	bool multi = false, strict = false;
	for (int p = VC_AW; p <= VC_R; p++) {
		if (ets.prio_tc[p] != ets.prio_tc[VC_AW]) multi = true;
		if (ets.tc_tsa[ets.prio_tc[p]] != IEEE_8021QAZ_TSA_ETS) strict = true;
	}
	if (multi && strict)
		printf("\n*** 警告：port %u（%s）PCP 0~3 分在 tc %u/%u/%u/%u 且为严格优先级，读写并发时低 TC 的帧会被饿死几十 ms，"
		       "超时计入 lost / err。\n*** 两端都改为单 TC：mlnx_qos -i %s --prio_tc=0,0,0,0,0,0,0,0\n\n",
		       pi, ifname, ets.prio_tc[0], ets.prio_tc[1], ets.prio_tc[2], ets.prio_tc[3], ifname);
}

void port_init(uint16_t pi)
{
	check_devargs(pi);
	check_tc(pi);
	int socket = rte_eth_dev_socket_id(pi);
	const int m = ctx_per_flow();
	uint16_t nrxd = (uint16_t)g_cfg.rxd, ntxd = NB_TXD, nf = (uint16_t)g_cfg.fpp, nq = (uint16_t)(nf * m);   /* nf：流（MAC）数；nq：队列数 */
	char name[32];
	struct rte_ether_addr pmac;
	struct rte_eth_conf conf; memset(&conf, 0, sizeof(conf));   /* 不开 VLAN strip */

	if (g_cfg.hwts) {
		struct rte_eth_dev_info di;
		if (rte_eth_dev_info_get(pi, &di) != 0 || !(di.rx_offload_capa & RTE_ETH_RX_OFFLOAD_TIMESTAMP))
			rte_exit(EXIT_FAILURE, "port %u 不支持接收硬件时间戳\n", pi);
		if (hwts_enable() < 0) rte_exit(EXIT_FAILURE, "注册 mbuf 时间戳字段失败\n");
		conf.rxmode.offloads |= RTE_ETH_RX_OFFLOAD_TIMESTAMP;
	}
	const uint16_t ntx = (uint16_t)tx_lanes(), ntq = (uint16_t)(nq * ntx);   /* 发送队列数：每个上下文 ntx 个（每个发送核一个） */
	if (rte_eth_dev_configure(pi, nq, ntq, &conf) < 0 ||
	    rte_eth_dev_adjust_nb_rx_tx_desc(pi, &nrxd, &ntxd) < 0)
		rte_exit(EXIT_FAILURE, "port %u configure failed\n", pi);
	rte_eth_macaddr_get(pi, &pmac);

	for (uint16_t q = 0; q < nq; q++) {
		uint16_t f = (uint16_t)(q / m), mf = (uint16_t)(pi * nf + f), fi = (uint16_t)(pi * nq + q);  /* f：端口内流号；mf：全局流号；fi：上下文下标 */
		struct flow_ctx *c = &g_flow[fi];
		c->port = pi; c->q = q; c->flow = f; c->idx = fi; c->txq0 = (uint16_t)(q * ntx); c->ntx = ntx;
		c->read = g_cfg.rw ? (q % m == 1) : g_cfg.mix ? (f & 1) : g_cfg.read;   /* rw：偶数队列写、奇数队列读 */
		hdr_flow_addr(pi, f, mf, &pmac, &c->addr);

		snprintf(name, sizeof(name), "rx%u_%u", pi, q);
		c->rx_pool = rte_pktmbuf_pool_create(name, RTE_MAX((unsigned)RX_POOL_N, 2u * (unsigned)g_cfg.rxd + 4095u), POOL_CACHE, 0, RTE_MBUF_DEFAULT_BUF_SIZE, socket);
		if (!c->rx_pool) rte_exit(EXIT_FAILURE, "rx pool %u.%u\n", pi, q);
		if (rte_eth_rx_queue_setup(pi, q, nrxd, socket, NULL, c->rx_pool) < 0)
			rte_exit(EXIT_FAILURE, "port %u rx queue %u setup failed\n", pi, q);
		for (uint16_t j = 0; j < ntx; j++) {
			c->lane[j].txq = (uint16_t)(c->txq0 + j);
			if (rte_eth_tx_queue_setup(pi, (uint16_t)(c->txq0 + j), ntxd, socket, NULL) < 0)
				rte_exit(EXIT_FAILURE, "port %u tx queue %u setup failed\n", pi, c->txq0 + j);
		}

		tmpl_build(c, &g_tmpl[fi]);
		snprintf(name, sizeof(name), "tmpl%u_%u", pi, q);
		c->tmpl_pool = rte_pktmbuf_pool_create(name, TX_POOL_N, POOL_CACHE, 0, RTE_PKTMBUF_HEADROOM + TMPL_ROOM, socket);
		if (!c->tmpl_pool) rte_exit(EXIT_FAILURE, "tmpl pool %u.%u\n", pi, q);
		rte_mempool_obj_iter(c->tmpl_pool, tmpl_obj_init, &g_tmpl[fi]);
	}
	if (g_cfg.rx_split) split_configure(pi);
	if (rte_eth_dev_start(pi) < 0) rte_exit(EXIT_FAILURE, "port %u start\n", pi);
	{                                   /* 驱动实际选用的收发函数（向量化 / MPRQ / 标量等），影响时延 */
		struct rte_eth_burst_mode bm;
		char rx[RTE_ETH_BURST_MODE_INFO_SIZE] = "?", tx[RTE_ETH_BURST_MODE_INFO_SIZE] = "?";
		if (rte_eth_rx_burst_mode_get(pi, 0, &bm) == 0) snprintf(rx, sizeof(rx), "%s", bm.info);
		if (rte_eth_tx_burst_mode_get(pi, 0, &bm) == 0) snprintf(tx, sizeof(tx), "%s", bm.info);
		printf("port %u: rx burst mode \"%s\", tx burst mode \"%s\"%s\n", pi, rx, tx,
		       g_cfg.hwts ? "（已开接收时间戳）" : "");
	}

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
	if (g_cfg.rx_split) split_rules(pi, &pmac, m, nf);
	for (uint16_t q = 0; q < nq && !g_cfg.rx_split; q++) {
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
		if (g_cfg.brief) continue;              /* 不打每流地址，MAC 用完整输出查看 */
		if (g_cfg.sender)
			printf("flow %u.%u: rxq %u txq %u-%u  %s  my %s  peer %s\n", pi, c->flow, q, c->txq0, c->txq0 + c->ntx - 1,
			       c->read ? "read " : "write", a, b);
		else
			printf("flow %u.%u: rxq %u txq %u-%u  %s  my %s\n", pi, c->flow, q, c->txq0, c->txq0 + c->ntx - 1,
			       m == 2 ? (c->read ? "AR" : "AW") : "", a);
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
		uint16_t s = rte_eth_tx_burst(c->port, c->txq0, m, 4);
		if (s < 4) rte_pktmbuf_free_bulk(m + s, 4 - s);
	}
	rte_delay_ms(200);
}

void port_fini(void)
{
	for (uint16_t i = 0; i < g_nb_ports; i++) {
		if (!g_cfg.rx_split) rte_flow_flush(i, NULL);   /* HW steering 的规则随端口关闭释放 */
		rte_eth_dev_stop(i);
		rte_eth_dev_close(i);
	}
}
