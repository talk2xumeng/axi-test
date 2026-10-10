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
#include "hwts.h"

struct config g_cfg = {
	.sender = true, .read = false, .window = 511, .pack = 2, .beats = 4, .burst = 32,
	.time = 0, .split = true, .fpp = 1, .dump = 0, .pcap_path = NULL, .pcap_left = 1000,
	.hdr = HDR_ETH, .vid = 0, .timeout_us = 10000, .b_pack = 1, .tx_cores = 1,
	.sue_ethertype = 0x88B5, .sue_format = 0, .sue_pkttype = 0, .gpu_id = -1, .peer_gpu_id = -1,
};
volatile bool g_quit;
struct flow_ctx g_flow[MAX_FLOWS];
uint16_t g_nb_ports, g_nb_flows;

static void usage(void)
{
	fputs(
	"axiperf — AXI over Ethernet 吞吐与 RTT 测试\n"
	"\n"
	"用法\n"
	"  ./axiperf -l <CPU列表> -a <网卡BDF> -- <测试参数>\n"
	"  ./axiperf --help                 查看本帮助，不初始化 DPDK / 网卡\n"
	"  ./axiperf -- --help              同上；也支持 -h\n"
	"  分隔符 -- 前是 DPDK EAL 参数，后是 axiperf 参数。\n"
	"\n"
	"快速开始（请替换 BDF、CPU 和对端 MAC；两端使用相同 VID / flows）\n"
	"  反射端：\n"
	"    ./axiperf -l 0-1 -a 0000:12:00.0 -- --mode reflector --vid 1\n"
	"  单流写：\n"
	"    ./axiperf -l 0-2 -a 0000:12:00.0 -- --mode sender --op write \\\n"
	"      --vid 1 --dmac 0,aa:bb:cc:dd:ee:ff --time 10\n"
	"  单流读：在上述发送端命令中把 --op write 改为 --op read。\n"
	"  同 MAC 读写：两端都加 --op rw；默认 sender 至少 5 核，reflector 至少 3 核。\n"
	"\n"
	"角色与流量\n"
	"  --mode sender|reflector   sender 发请求并统计 RTT；reflector 回响应 [sender]\n"
	"  --op write|read|mix|rw    [write]\n"
	"      write  只写；read 只读（reflector 自动识别读 / 写请求）\n"
	"      mix    偶数号流写、奇数号流读；至少 2 流才能同时包含读写\n"
	"      rw     同一 MAC 同时读写，各自一套窗口 / ID / 队列；两端必须设置\n"
	"  --flows N                每端口 MAC 流数，1~16 [1]\n"
	"  --time SEC               测试时长；0 表示直到 Ctrl-C [0]\n"
	"\n"
	"发送端：窗口、报文和 CPU\n"
	"  --window N               每个读 / 写上下文在途事务数，1~511 [511]\n"
	"  --pack N                 每请求包事务数 [2]\n"
	"                            写要求 pack×beats≤8；读最多 4 个事务\n"
	"  --rpack N                单独设置读请求每包事务数，1~4 [跟随 --pack]\n"
	"  --beats N                每事务 64B beat 数，1~4 [4，即 256B 数据]\n"
	"  --burst N                发送批量 / 反射端响应提交阈值，1~64 [32]\n"
	"                            RX 每次轮询最多取 64 包，不随此参数改变\n"
	"  --tx-cores N             每个上下文发送核数，1~4；窗口均分 [1]\n"
	"                            多核共用同一 MAC / RX 核，各核独占 TX 队列\n"
	"                            N>1 不能与 --nosplit / --id-seq 同用\n"
	"  --nosplit                每个上下文由同一核收发 [默认收发分核]\n"
	"  --id-seq                 按顺序分配 ID [默认空闲 ID 池，完成即可复用]\n"
	"  --timeout-us N           超时回收并计入 lost；0 禁用 [10000]\n"
	"  CPU 数（含 1 个统计核）：\n"
	"      上下文数 = 端口数×flows×(rw 模式为 2，否则为 1)\n"
	"      sender 默认 = 1+上下文数×(tx-cores+1)\n"
	"      sender --nosplit / reflector = 1+上下文数\n"
	"\n"
	"反射端\n"
	"  --b-pack N               B 写响应合并上限，1~16 [1：每请求包一个响应包]\n"
	"                            只合并当前 RX 批次，不额外等待凑包\n"
	"  --drop-every N           每 N 个请求包丢 1 个，用于丢包测试；0 禁用 [0]\n"
	"\n"
	"二层地址与封装（无需 IP / ARP）\n"
	"  --hdr eth|sue            eth：VLAN + 802.3 Length；sue 为试验性 [eth]\n"
	"  --vid N                  VLAN ID；两端保持一致 [0]\n"
	"  --dmac PORT,MAC          eth sender 的对端基准 MAC，可为各端口重复设置\n"
	"                            PORT 是 DPDK 端口号，如 0；不是 PCI BDF\n"
	"                            默认 02:00:00:00:01:0PORT；测试时请显式设置\n"
	"  --promisc                强制混杂接收 [默认仅接收本端各流 MAC]\n"
	"\n"
	"诊断与抓包（性能对比时建议关闭）\n"
	"  --hwts                   开启 RX 硬件时间戳，统计网卡接收→CPU取包时延\n"
	"  --dump N                 打印前 N 帧十六进制 [0]\n"
	"  --pcap FILE              保存程序收 / 发的 AXI 帧 [关闭]\n"
	"  --pcap-count N           整个进程合计抓包帧数上限 [1000]\n"
	"\n"
	"SUE 试验性参数（--hdr sue）\n"
	"  --gpu-id N               本端 GPU ID 基址，流 k 使用 N+k [必填]\n"
	"  --peer-gpu-id N          sender 的对端 GPU ID 基址 [必填]\n"
	"  --sue-ethertype X        EtherType，须 >1500 [0x88B5]\n"
	"  --sue-format N           Format[2:0] [0]\n"
	"  --sue-pkttype N          PktType[4:0] [0]\n"
	"\n"
	"[方括号] 表示默认值。Ctrl-C 正常结束；再次 Ctrl-C 强制退出。\n"
	, stdout);
}

/* 帮助无需 hugepages、网卡探测或运行权限；EAL 参数仍交由 EAL 解析。 */
static bool help_requested(int argc, char **argv)
{
	if (argc == 2 && (!strcmp(argv[1], "--help") || !strcmp(argv[1], "-h"))) return true;
	bool app = false;
	for (int i = 1; i < argc; i++) {
		if (!app) { if (!strcmp(argv[i], "--")) app = true; continue; }
		if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) return true;
		/* 以下为不取值的应用选项；其他长选项的下一项是它的值。 */
		if (!strcmp(argv[i], "--nosplit") || !strcmp(argv[i], "--id-seq") ||
		    !strcmp(argv[i], "--hwts") || !strcmp(argv[i], "--promisc")) continue;
		if (!strncmp(argv[i], "--", 2) && !strchr(argv[i], '=') && i + 1 < argc) i++;
	}
	return false;
}

enum {
	OPT_MODE = 256, OPT_OP, OPT_WINDOW, OPT_PACK, OPT_BEATS, OPT_BURST, OPT_TIME, OPT_DMAC, OPT_VID,
	OPT_NOSPLIT, OPT_DUMP, OPT_FLOWS, OPT_PCAP, OPT_PCAP_COUNT, OPT_HDR, OPT_GPU_ID, OPT_PEER_GPU_ID,
	OPT_SUE_ET, OPT_SUE_FMT, OPT_SUE_PT, OPT_PROMISC, OPT_TIMEOUT, OPT_DROP, OPT_BPACK, OPT_RPACK, OPT_TXC, OPT_HWTS, OPT_IDSEQ, OPT_HELP,
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
		{"promisc", 0, 0, OPT_PROMISC}, {"timeout-us", 1, 0, OPT_TIMEOUT}, {"drop-every", 1, 0, OPT_DROP}, {"b-pack", 1, 0, OPT_BPACK}, {"rpack", 1, 0, OPT_RPACK}, {"tx-cores", 1, 0, OPT_TXC}, {"hwts", 0, 0, OPT_HWTS}, {"id-seq", 0, 0, OPT_IDSEQ}, {"help", 0, 0, OPT_HELP}, {0, 0, 0, 0}};

	for (int i = 0; i < MAX_PORTS; i++) {           /* eth 默认对端 MAC：02:00:00:00:01:0i */
		uint8_t d[6] = {0x02, 0, 0, 0, 0x01, (uint8_t)i};
		memcpy(&g_cfg.peer_mac[i], d, 6);
	}
	int o;
	while ((o = getopt_long(argc, argv, "h", lo, NULL)) != -1) {
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
		case OPT_HWTS:    g_cfg.hwts = true; break;
		case OPT_IDSEQ:   g_cfg.id_seq = true; break;
		case OPT_TXC:     g_cfg.tx_cores = (int)num(optarg); break;
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
		case 'h':
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
	if (g_cfg.tx_cores < 1 || g_cfg.tx_cores > MAX_TXC) { printf("--tx-cores 须为 1~%d\n", MAX_TXC); exit(1); }
	if (g_cfg.tx_cores > 1 && (!g_cfg.sender || !g_cfg.split || g_cfg.id_seq || g_cfg.tx_cores * req_pack(g_cfg.read || g_cfg.rw || g_cfg.mix) > g_cfg.window)) {
		printf("--tx-cores > 1 只用于 sender 收发分核、空闲 ID 池模式（不能与 --nosplit / --id-seq 同用），且每核分到的 ID 数须不少于每包事务数\n");
		exit(1);
	}
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
	if (help_requested(argc, argv)) { usage(); return 0; }
	int ret = rte_eal_init(argc, argv);
	if (ret < 0) rte_exit(EXIT_FAILURE, "EAL init failed\n");
	parse_args(argc - ret, argv + ret);
	rte_pdump_init();                       /* 允许 dpdk-dumpcap 旁路抓包 */
	if (g_cfg.pcap_path) pcap_open(g_cfg.pcap_path);
	signal(SIGINT, on_sig); signal(SIGTERM, on_sig);
	stats_init();

	g_nb_ports = rte_eth_dev_count_avail();
	if (g_nb_ports == 0 || g_nb_ports > MAX_PORTS) rte_exit(EXIT_FAILURE, "ports: %u\n", g_nb_ports);
	const unsigned ntx = (unsigned)tx_lanes();
	unsigned per = (g_cfg.sender && g_cfg.split) ? ntx + 1 : 1;   /* 每个上下文的核数：分核时 ntx 个发送核 + 1 个接收核 */
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
	if (g_cfg.hwts) {                       /* 起两个锚点再开跑 */
		for (uint16_t i = 0; i < g_nb_ports; i++) hwclk_update(i);
		rte_delay_ms(200);
		for (uint16_t i = 0; i < g_nb_ports; i++) hwclk_update(i);
	}
	port_announce();
	g_cfg.tmo_cyc = (uint64_t)g_cfg.timeout_us * rte_get_tsc_hz() / 1000000;
	for (uint16_t i = 0; i < g_nb_flows; i++) {      /* 空闲 ID 池：0 .. window-1 按连续段均分给各发送核 */
		struct flow_ctx *c = &g_flow[i];
		const int W = g_cfg.window, N = c->ntx;
		for (int k = 0; k < N; k++) {
			struct tx_lane *L = &c->lane[k];
			const int lo = k * W / N, hi = (k + 1) * W / N;
			for (int id = lo; id < hi; id++) { L->idq[id - lo] = (uint16_t)id; c->owner[id] = (uint8_t)k; }
			L->idq_head = (uint32_t)(hi - lo);
		}
	}
	printf("mode=%s op=%s hdr=%s window=%d pack=%d beats=%d burst=%d vid=%d split=%d ports=%u flows/port=%d tx_cores=%d%s\n",
	       g_cfg.sender ? "sender" : "reflector", g_cfg.rw ? "rw" : g_cfg.mix ? "mix" : g_cfg.read ? "read" : "write", hdr_name(g_cfg.hdr),
	       g_cfg.window, g_cfg.pack, g_cfg.beats, g_cfg.burst, g_cfg.vid, g_cfg.sender && g_cfg.split,
	       g_nb_ports, g_cfg.fpp, (int)ntx, g_cfg.sender ? (g_cfg.id_seq ? " id=seq" : " id=pool") : "");
	if (g_cfg.hdr == HDR_SUE)
		printf("sue: ethertype=0x%04x format=%u pkttype=%u gpu_id=0x%04x peer_gpu_id=0x%04x\n",
		       g_cfg.sue_ethertype, g_cfg.sue_format, g_cfg.sue_pkttype, g_cfg.gpu_id,
		       g_cfg.peer_gpu_id < 0 ? 0 : g_cfg.peer_gpu_id);

	/*
	 * 核分配：-l 第一个为统计核；其余按流展开（分核模式每流先各发送核、后 RX）。
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
			if (per > 1) {
				static struct tx_arg ta[MAX_FLOWS][MAX_TXC];
				const unsigned k = wi % per;
				if (k == ntx) {
					rte_eal_remote_launch(sender_rx_loop, c, pick);
					printf("flow %u.%u RX -> lcore %u\n", c->port, c->flow, pick);
				} else {
					struct tx_arg *a = &ta[wi / per][k];
					a->c = c; a->k = (uint16_t)k;
					rte_eal_remote_launch(sender_tx_loop, a, pick);
					if (ntx > 1) printf("flow %u.%u TX%u (txq %u, ID %d-%d) -> lcore %u\n", c->port, c->flow, k, c->lane[k].txq,
					                    (int)k * g_cfg.window / (int)ntx, ((int)k + 1) * g_cfg.window / (int)ntx - 1, pick);
					else printf("flow %u.%u TX -> lcore %u\n", c->port, c->flow, pick);
				}
			} else {
				rte_eal_remote_launch(worker_main, c, pick);
				printf("flow %u.%u -> lcore %u\n", c->port, c->flow, pick);
			}
		}
	}

	static struct port_stat prev[MAX_FLOWS], prev_tx[MAX_FLOWS][MAX_TXC];
	uint64_t t0 = rte_get_timer_cycles(), last = t0, hz = rte_get_timer_hz();
	while (!g_quit) {
		rte_delay_ms(1000);
		if (g_cfg.hwts) for (uint16_t i = 0; i < g_nb_ports; i++) hwclk_update(i);
		uint64_t now = rte_get_timer_cycles();
		stats_print(prev, prev_tx, (double)(now - last) / hz, false);
		last = now;
		if (g_cfg.time && now - t0 >= (uint64_t)g_cfg.time * hz) g_quit = true;
	}
	printf("退出中：等待各核停止并关闭端口（再按一次 Ctrl-C 强制退出）\n"); fflush(stdout);
	rte_eal_mp_wait_lcore();
	{
		static struct port_stat zero[MAX_FLOWS], zero_tx[MAX_FLOWS][MAX_TXC];
		stats_print(zero, zero_tx, (double)(rte_get_timer_cycles() - t0) / hz, true);
	}
	port_fini();
	pcap_close();
	rte_eal_cleanup();
	return 0;
}
