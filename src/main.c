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
	.hdr = HDR_ETH, .vid = 0,
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
	"    --op write|read           sender：写 / 读测试（默认 write）\n"
	"    --window N                每条流在途事务上限 1~511（默认 511）\n"
	"    --pack N                  每请求包事务数（写：pack×beats≤8；读：≤4）（默认 2）\n"
	"    --beats N                 每事务拍数 1~4（默认 4）\n"
	"    --burst N                 收发 burst 1~64（默认 32）\n"
	"    --flows N                 每端口流数 1~16（默认 1）\n"
	"    --nosplit                 sender：每条流单核收发（默认收发分核）\n"
	"    --time SEC                运行秒数（默认直到 Ctrl-C）\n"
	"    --dump N                  打印前 N 帧十六进制\n"
	"    --pcap FILE               抓包模式：收 / 发的 AXI 帧写入 pcap\n"
	"    --pcap-count N            抓包模式最多写入帧数（默认 1000）\n"
	"  以太头：\n"
	"    --hdr eth|sue             头格式（默认 eth）\n"
	"    --vid N                   802.1Q VID（默认 0）\n"
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
	OPT_SUE_ET, OPT_SUE_FMT, OPT_SUE_PT, OPT_HELP,
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
		{"help", 0, 0, OPT_HELP}, {0, 0, 0, 0}};

	for (int i = 0; i < MAX_PORTS; i++) {           /* eth 默认对端 MAC：02:00:00:00:01:0i */
		uint8_t d[6] = {0x02, 0, 0, 0, 0x01, (uint8_t)i};
		memcpy(&g_cfg.peer_mac[i], d, 6);
	}
	int o;
	while ((o = getopt_long(argc, argv, "", lo, NULL)) != -1) {
		switch (o) {
		case OPT_MODE:    g_cfg.sender = strcmp(optarg, "reflector") != 0; break;
		case OPT_OP:      g_cfg.read = strcmp(optarg, "read") == 0; break;
		case OPT_WINDOW:  g_cfg.window = (int)num(optarg); break;
		case OPT_PACK:    g_cfg.pack = (int)num(optarg); break;
		case OPT_BEATS:   g_cfg.beats = (int)num(optarg); break;
		case OPT_BURST:   g_cfg.burst = (int)num(optarg); break;
		case OPT_TIME:    g_cfg.time = (int)num(optarg); break;
		case OPT_VID:     g_cfg.vid = (int)num(optarg) & 0xFFF; break;
		case OPT_NOSPLIT: g_cfg.split = false; break;
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
	    (!g_cfg.read && g_cfg.pack * g_cfg.beats > MAX_BEATS) || (g_cfg.read && g_cfg.pack > 4)) {
		printf("参数越界：flows 1~%d，window 1~511，beats 1~4，burst 1~%d，写 pack×beats≤8，读 pack≤4\n", MAX_FPP, MAX_BURST);
		exit(1);
	}
	if (hdr_validate() < 0) exit(1);
}

static void on_sig(int s) { (void)s; g_quit = true; }

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
	if (rte_lcore_count() < (unsigned)g_nb_ports * g_cfg.fpp * per + 1)
		rte_exit(EXIT_FAILURE, "需要 %u 个 lcore（1 统计 + 每条流 %u 个，共 %u 条流）\n",
		         (unsigned)g_nb_ports * g_cfg.fpp * per + 1, per, (unsigned)g_nb_ports * g_cfg.fpp);
	g_nb_flows = (uint16_t)(g_nb_ports * g_cfg.fpp);
	if (g_nb_flows > MAX_FLOWS) rte_exit(EXIT_FAILURE, "流数 %u 超过上限 %d\n", g_nb_flows, MAX_FLOWS);

	for (uint16_t i = 0; i < g_nb_ports; i++) port_init(i);
	printf("mode=%s op=%s hdr=%s window=%d pack=%d beats=%d burst=%d vid=%d split=%d ports=%u flows/port=%d\n",
	       g_cfg.sender ? "sender" : "reflector", g_cfg.read ? "read" : "write", hdr_name(g_cfg.hdr),
	       g_cfg.window, g_cfg.pack, g_cfg.beats, g_cfg.burst, g_cfg.vid, g_cfg.sender && g_cfg.split,
	       g_nb_ports, g_cfg.fpp);
	if (g_cfg.hdr == HDR_SUE)
		printf("sue: ethertype=0x%04x format=%u pkttype=%u gpu_id=0x%04x peer_gpu_id=0x%04x\n",
		       g_cfg.sue_ethertype, g_cfg.sue_format, g_cfg.sue_pkttype, g_cfg.gpu_id,
		       g_cfg.peer_gpu_id < 0 ? 0 : g_cfg.peer_gpu_id);

	/* 核分配：-l 第一个为统计核；之后按流展开，分核模式每流先 TX 后 RX */
	unsigned lc, wi = 0;
	RTE_LCORE_FOREACH_WORKER(lc) {
		if (wi >= (unsigned)g_nb_flows * per) break;
		struct flow_ctx *c = &g_flow[wi / per];
		if (per == 2) rte_eal_remote_launch((wi % 2) ? sender_rx_loop : sender_tx_loop, c, lc);
		else rte_eal_remote_launch(worker_main, c, lc);
		wi++;
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
