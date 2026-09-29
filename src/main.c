/*
 * axiperf - AXI over Ethernet 性能验证程序（sender / reflector）
 *
 * 分层：
 *   hdr_*.h / hdr.c  以太头格式（eth：802.3 + Length；sue：SUEv1-标准ETH，试验性）
 *   axi.h            AXI 事务编码与解析（与头格式无关）
 *   sender.c         发请求、收响应、窗口与 RTT
 *   reflector.c      收请求、回响应
 *   port.c           端口 / 队列 / 模板 / 导流
 *   stats.c          每秒统计输出
 *   capture.c        异常帧打印、--dump、--pcap
 */
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <getopt.h>
#include <unistd.h>

#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_lcore.h>
#include <rte_cycles.h>
#include <rte_pdump.h>
#include <rte_debug.h>

#include "common.h"
#include "hdr.h"
#include "axi.h"
#include "port.h"
#include "worker.h"
#include "stats.h"
#include "capture.h"

struct config g_cfg = {
	.sender = true, .read = false, .window = 511, .pack = 2, .beats = 4, .burst = 32,
	.time = 0, .split = true, .fpp = 1, .dump = 0, .pcap_path = NULL, .pcap_left = 1000,
	.hdr = HDR_ETH, .vid = 0, .timeout_us = 10000, .b_pack = 1,
	.sue_ethertype = 0x88B5, .sue_format = 0, .sue_pkttype = 0, .gpu_id = -1, .peer_gpu_id = -1,
};
volatile bool g_quit;
struct flow_ctx g_flow[MAX_FLOWS];
uint16_t g_nb_ports, g_nb_flows;

static void usage(void)
{
	printf(
	"axiperf [EAL 参数] -- [选项]\n"
	"  通用：\n"
	"    --mode sender|reflector   角色（默认 sender）\n"
	"    --op write|read|mix|rw    sender：写 / 读 / 读写并发（mix：偶数号流写、奇数号流读；rw：每条流同一 MAC 上同时读写，写、读各一套 ID / 队列 / 核，两端都要设）（默认 write）\n"
	"    --window N                每条流在途事务上限 1~511（默认 511）\n"
	"    --pack N                  每请求包事务数（写：pack×beats≤8；读：≤4）（默认 2）\n"
	"    --rpack N                 读请求每包事务数 1~4，mix / rw 时读写可分别设置（默认同 --pack）\n"
	"    --beats N                 每事务拍数 1~4（默认 4）\n"
	"    --burst N                 收发 burst 1~64（默认 32）\n"
	"    --flows N                 每端口流数 1~16（默认 1）\n"
	"    --nosplit                 sender：每条流单核收发（默认收发分核）\n"
	"    --time SEC                运行秒数（默认直到 Ctrl-C）\n"
	"    --timeout-us N            sender：事务超时回收，记入 lost（默认 10000，0 为不回收）\n"
	"    --drop-every N            reflector：每 N 个请求包丢 1 个，用于验证丢包处理（默认 0）\n"
	"    --b-pack N                reflector：同一批写请求的 B 合并进一个响应包，每包最多 N 个（1~16，默认 1）\n"
	"    --dump N                  打印前 N 帧十六进制\n"
	"    --pcap FILE               抓包模式：收 / 发的 AXI 帧写入 pcap\n"
	"    --pcap-count N            抓包模式最多写入帧数（默认 1000）\n"
	"  以太头：\n"
	"    --hdr eth|sue             头格式（默认 eth）\n"
	"    --vid N                   802.1Q VID（默认 0）\n"
	"    --promisc                 强制混杂模式（默认 eth 头只收本端各流 MAC）\n"
	"  eth 头：\n"
	"    --dmac PORT,MAC           sender：对端网卡真实 MAC，可重复\n"
	"  sue 头（试验性）：\n"
	"    --gpu-id N                本端 GPU ID 基址，流 k 用 N+k（必填）\n"
	"    --peer-gpu-id N           sender：对端 GPU ID 基址（必填）\n"
	"    --sue-ethertype X         EtherType（默认 0x88B5）\n"
	"    --sue-format N            Format[2:0]（默认 0）\n"
	"    --sue-pkttype N           PktType[4:0]（默认 0）\n");
}

enum {
	OPT_MODE = 256, OPT_OP, OPT_WINDOW, OPT_PACK, OPT_BEATS, OPT_BURST, OPT_TIME, OPT_DMAC, OPT_VID,
	OPT_NOSPLIT, OPT_DUMP, OPT_FLOWS, OPT_PCAP, OPT_PCAP_COUNT, OPT_HDR, OPT_GPU_ID, OPT_PEER_GPU_ID,
	OPT_SUE_ET, OPT_SUE_FMT, OPT_SUE_PT, OPT_PROMISC, OPT_TIMEOUT, OPT_DROP, OPT_BPACK, OPT_RPACK, OPT_HELP,
};

static long num(const char *s) { return strtol(s, NULL, 0); }

static void parse_args(int argc, char **argv)
{
	static const struct option lo[] = {
		{"mode", 1, 0, OPT_MODE}, {"op", 1, 0, OPT_OP}, {"window", 1, 0, OPT_WINDOW}, {"pack", 1, 0, OPT_PACK},
		{"beats", 1, 0, OPT_BEATS}, {"burst", 1, 0, OPT_BURST}, {"time", 1, 0, OPT_TIME}, {"dmac", 1, 0, OPT_DMAC},
		{"vid", 1, 0, OPT_VID}, {"nosplit", 0, 0, OPT_NOSPLIT}, {"dump", 1, 0, OPT_DUMP}, {"flows", 1, 0, OPT_FLOWS},
		{"pcap", 1, 0, OPT_PCAP}, {"pcap-count", 1, 0, OPT_PCAP_COUNT}, {"hdr", 1, 0, OPT_HDR},
		{"gpu-id", 1, 0, OPT_GPU_ID}, {"peer-gpu-id", 1, 0, OPT_PEER_GPU_ID},
		{"sue-ethertype", 1, 0, OPT_SUE_ET}, {"sue-format", 1, 0, OPT_SUE_FMT}, {"sue-pkttype", 1, 0, OPT_SUE_PT},
		{"promisc", 0, 0, OPT_PROMISC}, {"timeout-us", 1, 0, OPT_TIMEOUT}, {"drop-every", 1, 0, OPT_DROP}, {"b-pack", 1, 0, OPT_BPACK}, {"rpack", 1, 0, OPT_RPACK}, {"help", 0, 0, OPT_HELP}, {0, 0, 0, 0}};

	for (int i = 0; i < MAX_PORTS; i++) {           /* eth 默认对端 MAC：02:00:00:00:01:0i */
		uint8_t d[6] = {0x02, 0, 0, 0, 0x01, (uint8_t)i};
		memcpy(&g_cfg.peer_mac[i], d, 6);
	}
	int o;
	while ((o = getopt_long(argc, argv, "", lo, NULL)) != -1) {
		switch (o) {
		case OPT_MODE:    g_cfg.sender = strcmp(optarg, "reflector") != 0; break;
		case OPT_OP:
			if (!strcmp(optarg, "write")) g_cfg.read = false;
			else if (!strcmp(optarg, "read")) g_cfg.read = true;
			else if (!strcmp(optarg, "mix")) g_cfg.mix = true;
			else if (!strcmp(optarg, "rw")) g_cfg.rw = true;
			else { printf("未知 --op %s\n", optarg); usage(); exit(1); }
			break;
		case OPT_WINDOW:  g_cfg.window = (int)num(optarg); break;
		case OPT_PACK:    g_cfg.pack = (int)num(optarg); break;
		case OPT_BEATS:   g_cfg.beats = (int)num(optarg); break;
		case OPT_BURST:   g_cfg.burst = (int)num(optarg); break;
		case OPT_TIME:    g_cfg.time = (int)num(optarg); break;
		case OPT_VID:     g_cfg.vid = (int)num(optarg) & 0xFFF; break;
		case OPT_NOSPLIT: g_cfg.split = false; break;
		case OPT_PROMISC: g_cfg.promisc = true; break;
		case OPT_TIMEOUT: g_cfg.timeout_us = (uint32_t)num(optarg); break;
		case OPT_DROP:    g_cfg.drop_every = (uint32_t)num(optarg); break;
		case OPT_BPACK:   g_cfg.b_pack = (int)num(optarg); break;
		case OPT_RPACK:   g_cfg.rpack = (int)num(optarg); break;
		case OPT_DUMP:    g_cfg.dump = (int)num(optarg); break;
		case OPT_FLOWS:   g_cfg.fpp = (int)num(optarg); break;
		case OPT_PCAP:    g_cfg.pcap_path = optarg; break;
		case OPT_PCAP_COUNT: g_cfg.pcap_left = strtoull(optarg, NULL, 0); break;
		case OPT_HDR:
			if (hdr_from_name(optarg, &g_cfg.hdr) < 0) { printf("未知头格式 %s\n", optarg); usage(); exit(1); }
			break;
		case OPT_GPU_ID:      g_cfg.gpu_id = (int)num(optarg) & 0xFFFF; break;
		case OPT_PEER_GPU_ID: g_cfg.peer_gpu_id = (int)num(optarg) & 0xFFFF; break;
		case OPT_SUE_ET:  g_cfg.sue_ethertype = (uint16_t)num(optarg); break;
		case OPT_SUE_FMT: g_cfg.sue_format = (uint8_t)(num(optarg) & 0x7); break;
		case OPT_SUE_PT:  g_cfg.sue_pkttype = (uint8_t)(num(optarg) & 0x1F); break;
		case OPT_DMAC: {
			int pi = atoi(optarg); char *mac = strchr(optarg, ',');
			if (!mac || pi < 0 || pi >= MAX_PORTS || rte_ether_unformat_addr(mac + 1, &g_cfg.peer_mac[pi]) < 0) { usage(); exit(1); }
			break; }
		case OPT_HELP: usage(); exit(0);
		default: usage(); exit(1);
		}
	}
	if (g_cfg.fpp < 1 || g_cfg.fpp > MAX_FPP || g_cfg.window < 1 || g_cfg.window > 511 ||
	    g_cfg.beats < 1 || g_cfg.beats > 4 || g_cfg.burst < 1 || g_cfg.burst > MAX_BURST || g_cfg.pack < 1 ||
	    ((!g_cfg.read || g_cfg.mix || g_cfg.rw) && g_cfg.pack * g_cfg.beats > MAX_BEATS) || ((g_cfg.read || g_cfg.mix || g_cfg.rw) && req_pack(true) > 4) || g_cfg.rpack < 0) {
		printf("参数越界：flows 1~%d，window 1~511，beats 1~4，burst 1~%d，写 pack×beats≤8，读 pack≤4\n", MAX_FPP, MAX_BURST);
		exit(1);
	}
	if (g_cfg.b_pack < 1 || g_cfg.b_pack > MAX_TXN) { printf("--b-pack 须为 1~%d\n", MAX_TXN); exit(1); }
	if (hdr_validate() < 0) exit(1);
}

/* 第一次 Ctrl-C：通知各核退出并正常收尾；第二次：直接退出（网卡卡死时 stop/close 可能挂住） */
static void on_sig(int s)
{
	(void)s;
	if (g_quit) {
		static const char m[] = "\n强制退出（未释放端口；建议 EAL 加 --huge-unlink 以免残留大页文件）\n";
		if (write(2, m, sizeof(m) - 1) < 0) {}
		_exit(1);
	}
	g_quit = true;
}

static int worker_main(void *arg)       /* 单核模式：sender 或 reflector */
{
	return g_cfg.sender ? sender_loop(arg) : reflector_loop(arg);
}

int main(int argc, char **argv)
{
	int ret = rte_eal_init(argc, argv);
	if (ret < 0) rte_exit(EXIT_FAILURE, "EAL init failed\n");
	parse_args(argc - ret, argv + ret);
	rte_pdump_init();                       /* 允许 dpdk-dumpcap 旁路抓包 */
	if (g_cfg.pcap_path) pcap_open(g_cfg.pcap_path);
	signal(SIGINT, on_sig); signal(SIGTERM, on_sig);
	stats_init();

	g_nb_ports = rte_eth_dev_count_avail();
	if (g_nb_ports == 0 || g_nb_ports > MAX_PORTS) rte_exit(EXIT_FAILURE, "ports: %u\n", g_nb_ports);
	unsigned per = (g_cfg.sender && g_cfg.split) ? 2 : 1;
	g_nb_flows = (uint16_t)(g_nb_ports * g_cfg.fpp * ctx_per_flow());   /* 上下文数：rw 时每条流写、读各一个 */
	if (g_nb_flows > MAX_FLOWS) rte_exit(EXIT_FAILURE, "上下文数 %u 超过上限 %d（rw 时每条流计 2）\n", g_nb_flows, MAX_FLOWS);
	if (rte_lcore_count() < (unsigned)g_nb_flows * per + 1)
		rte_exit(EXIT_FAILURE, "需要 %u 个 lcore（1 统计 + 每个上下文 %u 个，共 %u 个上下文%s）\n",
		         (unsigned)g_nb_flows * per + 1, per, g_nb_flows, g_cfg.rw ? "，rw 时每条流写、读各一个" : "");

	for (uint16_t i = 0; i < g_nb_ports; i++) port_init(i);
	for (uint16_t i = 0; i < g_nb_ports; i++) {
		struct rte_eth_link lk;
		if (rte_eth_link_get(i, &lk) == 0 && !lk.link_status) printf("警告：port %u 链路未 up\n", i);
	}
	port_announce();
	g_cfg.tmo_cyc = (uint64_t)g_cfg.timeout_us * rte_get_tsc_hz() / 1000000;
	printf("mode=%s op=%s hdr=%s window=%d pack=%d beats=%d burst=%d vid=%d split=%d ports=%u flows/port=%d\n",
	       g_cfg.sender ? "sender" : "reflector", g_cfg.rw ? "rw" : g_cfg.mix ? "mix" : g_cfg.read ? "read" : "write", hdr_name(g_cfg.hdr),
	       g_cfg.window, g_cfg.pack, g_cfg.beats, g_cfg.burst, g_cfg.vid, g_cfg.sender && g_cfg.split,
	       g_nb_ports, g_cfg.fpp);
	if (g_cfg.hdr == HDR_SUE)
		printf("sue: ethertype=0x%04x format=%u pkttype=%u gpu_id=0x%04x peer_gpu_id=0x%04x\n",
		       g_cfg.sue_ethertype, g_cfg.sue_format, g_cfg.sue_pkttype, g_cfg.gpu_id,
		       g_cfg.peer_gpu_id < 0 ? 0 : g_cfg.peer_gpu_id);

	/*
	 * 核分配：-l 第一个为统计核；其余按流展开（分核模式每流先 TX 后 RX）。
	 * 每条流优先取与其网卡同一 NUMA 节点、编号最小的空闲核；该节点核不够时取其它节点的核并告警。
	 */
	{
		static bool used[RTE_MAX_LCORE];
		unsigned wl[RTE_MAX_LCORE], nw = 0, lc;
		RTE_LCORE_FOREACH_WORKER(lc) wl[nw++] = lc;
		for (unsigned wi = 0; wi < (unsigned)g_nb_flows * per; wi++) {
			struct flow_ctx *c = &g_flow[wi / per];
			int sock = rte_eth_dev_socket_id(c->port);
			unsigned pick = RTE_MAX_LCORE;
			for (unsigned j = 0; j < nw && pick == RTE_MAX_LCORE; j++)
				if (!used[wl[j]] && (sock < 0 || (int)rte_lcore_to_socket_id(wl[j]) == sock)) pick = wl[j];
			if (pick == RTE_MAX_LCORE) {
				for (unsigned j = 0; j < nw && pick == RTE_MAX_LCORE; j++) if (!used[wl[j]]) pick = wl[j];
				printf("警告：flow %u.%u 所在 NUMA %d 的核不够，使用 NUMA %u 的核 %u\n",
				       c->port, c->flow, sock, rte_lcore_to_socket_id(pick), pick);
			}
			used[pick] = true;
			if (per == 2) {
				rte_eal_remote_launch((wi % 2) ? sender_rx_loop : sender_tx_loop, c, pick);
				printf("flow %u.%u %s -> lcore %u\n", c->port, c->flow, (wi % 2) ? "RX" : "TX", pick);
			} else {
				rte_eal_remote_launch(worker_main, c, pick);
				printf("flow %u.%u -> lcore %u\n", c->port, c->flow, pick);
			}
		}
	}

	static struct port_stat prev[MAX_FLOWS], prev_tx[MAX_FLOWS];
	uint64_t t0 = rte_get_timer_cycles(), last = t0, hz = rte_get_timer_hz();
	while (!g_quit) {
		rte_delay_ms(1000);
		uint64_t now = rte_get_timer_cycles();
		stats_print(prev, prev_tx, (double)(now - last) / hz, false);
		last = now;
		if (g_cfg.time && now - t0 >= (uint64_t)g_cfg.time * hz) g_quit = true;
	}
	printf("退出中：等待各核停止并关闭端口（再按一次 Ctrl-C 强制退出）\n"); fflush(stdout);
	rte_eal_mp_wait_lcore();
	{
		static struct port_stat zero[MAX_FLOWS], zero_tx[MAX_FLOWS];
		stats_print(zero, zero_tx, (double)(rte_get_timer_cycles() - t0) / hz, true);
	}
	port_fini();
	pcap_close();
	rte_eal_cleanup();
	return 0;
}
