/*
 * axiperf - AXI over Ethernet 性能验证程序
 * 公共定义：常量、运行配置、每流上下文、统计结构
 */
#ifndef AXIPERF_COMMON_H
#define AXIPERF_COMMON_H

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include <rte_ether.h>
#include <rte_mbuf.h>
#include <rte_mempool.h>
#include <rte_byteorder.h>

#define MAX_PORTS   8
#define MAX_FLOWS   32          /* 所有端口合计流数上限 */
#define MAX_FPP     16          /* 每端口流数上限 */
#define ID_SPACE    512
#define NB_RXD      1024
#define NB_TXD      1024
#define RX_POOL_N   16383
#define TX_POOL_N   8191
#define POOL_CACHE  256
#define MAX_BURST   64
#define TMPL_ROOM   2048        /* 模板 / 发送 mbuf 数据区 */

/* ---------------- 字节序辅助（payload 可能非 4B 对齐） ---------------- */
static inline void put_be32(uint8_t *p, uint32_t v) { v = rte_cpu_to_be_32(v); memcpy(p, &v, 4); }
static inline uint32_t get_be32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return rte_be_to_cpu_32(v); }
static inline void put_be16(uint8_t *p, uint16_t v) { v = rte_cpu_to_be_16(v); memcpy(p, &v, 2); }
static inline uint16_t get_be16(const uint8_t *p) { uint16_t v; memcpy(&v, p, 2); return rte_be_to_cpu_16(v); }

/* ---------------- 以太头格式 ---------------- */
enum hdr_kind {
	HDR_ETH = 0,                 /* 802.1Q + 802.3 Length（当前测试格式） */
	HDR_SUE = 1,                 /* SUEv1-标准ETH：GPU ID 在 MAC 位置，EtherType + PackNum（试验性） */
};

/* ---------------- 运行配置 ---------------- */
struct config {
	bool   sender;               /* true=sender，false=reflector */
	bool   read;                 /* sender：false=写，true=读 */
	bool   mix;                  /* sender：读写并发，每端口偶数号流写、奇数号流读 */
	bool   rw;                   /* 读写并发，同一 MAC 上同时读写：写、读各一个上下文（awid / arid 各一套 ID 空间）、
	                                各一对队列与核，接收按 PCP（= VC）导流；两端都要设 */
	int    window;               /* 每条流在途事务上限 */
	int    pack;                 /* 每请求包事务数 */
	int    beats;                /* 每事务拍数（awlen/arlen+1） */
	int    burst;
	int    time;                 /* 运行秒数，0=直到 Ctrl-C */
	bool   split;                /* sender：每条流收发分核 */
	int    fpp;                  /* 每端口流数 */
	int    dump;                 /* 打印前 N 帧十六进制 */
	const char *pcap_path;       /* 抓包模式输出文件 */
	uint64_t pcap_left;          /* 抓包模式最多写入帧数 */

	enum hdr_kind hdr;
	int    vid;                  /* 802.1Q VID（eth 与 sue 均使用） */
	uint32_t timeout_us;         /* sender：事务超时回收（0 = 不回收） */
	uint64_t tmo_cyc;            /* 同上，TSC 周期 */
	int    rpack;                /* sender：读请求每包事务数（0 = 同 --pack） */
	int    b_pack;               /* reflector：每个写响应包最多合并的 B 个数，1~16（默认 1） */
	bool   hwts;                 /* 开接收硬件时间戳，统计接收时延（网卡收到 → CPU 拿到） */
	int    txq;                  /* 每个上下文的发送队列数 1~4：同一核把每批包均分到这几个队列（默认 1） */
	bool   promisc;
	bool   id_seq;               /* sender：ID 按顺序分配（旧方式，最老的在途 ID 挡住后面的分配）；默认空闲 ID 池 */
	uint32_t drop_every;         /* reflector 测试用：每 N 个请求包丢 1 个（0 = 不丢） */              /* 强制混杂模式（默认 eth 头只收本端各流 MAC） */
	/* eth 头 */
	struct rte_ether_addr peer_mac[MAX_PORTS];   /* sender：各端口对端网卡真实 MAC */
	/* sue 头（试验性，取值待与 IP 侧确认） */
	uint16_t sue_ethertype;
	uint8_t  sue_format;
	uint8_t  sue_pkttype;
	int      gpu_id;             /* 本端 GPU ID 基址，流 k 使用 gpu_id + k；-1 表示未设置 */
	int      peer_gpu_id;        /* sender：对端 GPU ID 基址 */
};
extern struct config g_cfg;
extern volatile bool g_quit;

/* ---------------- 统计 ---------------- */
#define HIST_NS     50          /* 细档：50ns 一档，覆盖 0~200us */
#define HIST_N      4000
#define HIST2_US    100         /* 粗档：100us 一档，覆盖 200us~100ms */
#define HIST2_N     1000
#define SLOW_NS     200000      /* RTT > 200us 计为 slow */
#define RXD_NS      100         /* 接收时延直方图：100ns 一档，覆盖 0~100us */
#define RXD_N       1000

struct port_stat {
	uint64_t tx_pkts, tx_wire_bytes, rx_pkts, rx_wire_bytes;
	uint64_t txn_done, data_bytes, err, lost, ign, xmac, pcpx, dumped, dropped;
	uint64_t hist[HIST_N], hist2[HIST2_N];
	uint64_t rtt_max_ns, rtt_sum_ns, slow;
	uint64_t rxd_hist[RXD_N], rxd_n, rxd_neg, rxd_bad;   /* --hwts：接收时延直方图、样本数、换算为负、无时间戳 */
	uint64_t busy_cyc, rx_hits, dump_tx, dump_rx;   /* 有活干的循环周期；rx_burst 非空次数；已打印帧数 */
} __rte_cache_aligned;

/* ---------------- 流地址（线上 SMAC / DMAC 字段的实际取值，由头格式决定） ---------------- */
struct flow_addr {
	struct rte_ether_addr src;   /* 本流发出帧的 SMAC 字段；也是本流接收帧的 DMAC 字段 */
	struct rte_ether_addr dst;   /* sender：请求帧的 DMAC 字段 */
};

/* ---------------- 每流上下文 ---------------- */
struct flow_ctx {
	uint16_t port, q, flow, idx;          /* 端口、接收队列号、端口内流序号、全局流序号 */
	uint16_t txq0;                        /* 首个发送队列号（本上下文用 txq0 .. txq0 + g_cfg.txq - 1） */
	struct flow_addr addr;
	struct rte_mempool *rx_pool;
	struct rte_mempool *tmpl_pool;        /* 预填模板：sender 请求 / reflector 读响应 */
	uint16_t req_len;                     /* sender：请求帧长 */
	bool     read;                        /* sender：本流为读（false 为写） */
	struct port_stat st;                  /* RX 核（或单核模式）写 */
	struct port_stat st_tx;               /* 分核模式 TX 核写，避免伪共享 */
	/*
	 * 以上字段初始化后只读，收发两侧每次轮询都会读，禁止在快路径上写（否则伪共享）。
	 * 以下按写入方分 cache line：
	 */
	/* 在途表：TX 侧置位、RX 侧清零（单生产者 / 单消费者） */
	uint64_t ts[ID_SPACE];
	uint8_t  outst[ID_SPACE];
	/*
	 * 空闲 ID 池（默认）：RX 侧把完成的 ID 写入环 idq 并推进 idq_head，TX 侧从 idq_tail 取。
	 * 系统里 ID 总数 = --window ≤ 511 < 环长 512，生产者不会追上消费者，TX 侧不用发布 tail。
	 * 超时回收的 ID 由 TX 侧放进本地 stash，优先使用。
	 */
	uint16_t idq[ID_SPACE] __rte_cache_aligned;   /* RX 侧写 */
	uint64_t tx_txn __rte_cache_aligned;  /* TX 侧写：累计发出事务数 */
	uint32_t next_id;                     /* TX 侧写：下一个分配的 ID（--id-seq） */
	uint32_t idq_tail;                    /* TX 侧写：空闲环消费位置 */
	uint16_t nstash;                      /* TX 侧写：stash 中的 ID 数 */
	uint16_t stash[ID_SPACE];             /* TX 侧写：超时回收的 ID */
	uint32_t txrr;                        /* TX 侧写：--txq 轮转起点 */
	uint64_t lost_txn;                    /* TX 侧写：超时回收的事务数 */
	uint64_t rc_last;                     /* TX 侧写：上次超时检查的 TSC */
	uint64_t done_txn __rte_cache_aligned;/* RX 侧写：累计完成事务数 */
	uint32_t idq_head;                    /* RX 侧写：空闲环生产位置（release 发布） */
} __rte_cache_aligned;

extern struct flow_ctx g_flow[MAX_FLOWS];
extern uint16_t g_nb_ports, g_nb_flows;   /* g_nb_flows = 上下文数（rw 时每条流 2 个） */

/* 每条流（每个 MAC）的上下文数：rw 时写、读各一个，共用 MAC，各自队列与核 */
static inline int ctx_per_flow(void) { return g_cfg.rw ? 2 : 1; }

/* 本上下文每请求包的事务数：读用 --rpack（未设时同 --pack），写用 --pack */
static inline int req_pack(bool read) { return (read && g_cfg.rpack) ? g_cfg.rpack : g_cfg.pack; }

/* 线上占用：不足 60B 补齐 + FCS 4B + 前导码/SFD/IFG 20B */
static inline uint16_t frame_wire(uint32_t len) { return (uint16_t)((len < 60 ? 60 : len) + 4 + 20); }

#endif
