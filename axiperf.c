/*
 * axiperf - AXI over Ethernet 最小性能验证程序（sender / reflector）
 *
 * 帧格式（当前文档 v0.5）：
 *   DMAC(6) SMAC(6) 0x8100 PCP/DEI/VID(2) Length(2) | AXI 事务 x k
 *   PCP = VC；Length = 之后的有效字节数；头部字段按 32bit 大端
 * 一个 DPDK port = 一条流 = 一个 worker lcore（1 rxq + 1 txq）
 * 暂不处理 MAC：端口开混杂，DMAC 可用 --dmac 指定
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <getopt.h>
#include <stdbool.h>

#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_mbuf.h>
#include <rte_mempool.h>
#include <rte_lcore.h>
#include <rte_cycles.h>
#include <rte_byteorder.h>
#include <rte_ether.h>
#include <rte_hexdump.h>

#define MAX_PORTS   8
#define ID_SPACE    512
#define HDR_LEN     18          /* eth(12) + vlan tag(4) + length(2) */
#define AW_HDR      16
#define AR_HDR      16
#define B_RSP       12
#define R_HDR       4
#define BEAT        64
#define MAX_BEATS   8           /* 每包数据 ≤ 8 拍 */
#define NB_RXD      1024
#define NB_TXD      1024
#define RX_POOL_N   16383
#define TX_POOL_N   8191
#define POOL_CACHE  256
#define HIST_NS     50          /* 细档：50ns 一档，覆盖 0~200us */
#define HIST_N      4000
#define HIST2_US    100         /* 粗档：100us 一档，覆盖 200us~100ms */
#define HIST2_N     1000
#define SLOW_NS     200000      /* RTT > 200us 计为 slow */
#define MAX_BURST   64

enum { VC_AW = 0, VC_AR = 1, VC_B = 2, VC_R = 3 };
enum { FT_WRFULL = 0x0, FT_WR = 0x1, FT_B = 0x2, FT_AR = 0x3, FT_R = 0x4 };

/* ---------------- 配置 ---------------- */
static bool  g_sender = true;
static bool  g_read = false;          /* sender: false=写, true=读 */
static int   g_window = 511;
static int   g_pack = 2;              /* 每请求包事务数 */
static int   g_beats = 4;             /* 每事务拍数 (awlen/arlen+1) */
static int   g_burst = 32;
static int   g_time = 0;              /* 0 = 直到 Ctrl-C */
static int   g_vid = 0;
static bool  g_split = true;           /* sender：每条流收发分核 */               /* VLAN ID，过交换机时设为交换机上的 VLAN */
static struct rte_ether_addr g_dmac[MAX_PORTS];
static volatile bool g_quit;

/* ---------------- 每端口状态 ---------------- */
struct port_stat {
	uint64_t tx_pkts, tx_wire_bytes, rx_pkts, rx_wire_bytes;
	uint64_t txn_done, data_bytes, err, ign, pcpx, dumped;
	uint64_t hist[HIST_N], hist2[HIST2_N];
	uint64_t rtt_max_ns, rtt_sum_ns, slow;
	uint64_t busy_cyc, rx_hits;          /* 有活干的循环耗费的周期；rx_burst 非空次数 */
} __rte_cache_aligned;

struct port_ctx {
	uint16_t port;
	struct rte_ether_addr mac;
	struct rte_mempool *rx_pool;
	struct rte_mempool *tmpl_pool;    /* 预填模板：sender 请求 / reflector 读响应 */
	uint16_t tmpl_len;                /* 模板满载长度 */
	struct port_stat st;                  /* RX 核（或单核模式）写 */
	struct port_stat st_tx;               /* 分核模式下 TX 核写，避免伪共享 */
	/* 分核模式：TX 核与 RX 核共享的在途表（单生产者 / 单消费者） */
	uint64_t ts[ID_SPACE];
	uint8_t  outst[ID_SPACE];
	uint64_t tx_txn __rte_cache_aligned;  /* TX 核写：累计发出事务数 */
	uint64_t done_txn __rte_cache_aligned;/* RX 核写：累计完成事务数 */
} __rte_cache_aligned;

static struct port_ctx g_ctx[MAX_PORTS];
static uint16_t g_nb_ports;

/* ---------------- 字节序辅助（payload 从偏移 18 开始，非 4B 对齐） ---------------- */
static inline void put_be32(uint8_t *p, uint32_t v) { v = rte_cpu_to_be_32(v); memcpy(p, &v, 4); }
static inline uint32_t get_be32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return rte_be_to_cpu_32(v); }
static inline void put_be16(uint8_t *p, uint16_t v) { v = rte_cpu_to_be_16(v); memcpy(p, &v, 2); }
static inline uint16_t get_be16(const uint8_t *p) { uint16_t v; memcpy(&v, p, 2); return rte_be_to_cpu_16(v); }

static inline uint32_t w0_aw(uint32_t id)  { return (FT_WRFULL << 28) | (id << 19) | ((uint32_t)(g_beats - 1) << 17) | (0xFu << 13); }
static inline uint32_t w0_ar(uint32_t id)  { return (FT_AR << 28) | (id << 19); }
static inline uint32_t w0_b(uint32_t id)   { return (FT_B << 28) | (id << 19); }                /* BRESP=00 */
static inline uint32_t w0_r(uint32_t id, uint32_t beats) { return (FT_R << 28) | (id << 19) | ((beats - 1) << 17) | (1u << 3); } /* rlast=1, RRESP=00 */

static void write_eth(uint8_t *p, const struct rte_ether_addr *d, const struct rte_ether_addr *s, uint8_t vc, uint16_t len)
{
	memcpy(p, d, 6); memcpy(p + 6, s, 6);
	p[12] = 0x81; p[13] = 0x00;
	put_be16(p + 14, (uint16_t)((vc << 13) | (g_vid & 0xFFF)));   /* DEI=0 */
	put_be16(p + 16, len);
}

static inline uint16_t frame_wire(uint16_t len) { return (uint16_t)((len < 60 ? 60 : len) + 4 + 20); } /* +FCS +前导码/IFG */

/* 识别 AXI 帧：802.1Q 且 tag 后为 802.3 Length（≤1500）。返回 0=AXI，1=非 AXI（静默忽略），2=AXI 但长度越界 */
static inline int classify(const uint8_t *f, uint32_t pkt_len, uint16_t *plen)
{
	if (pkt_len < HDR_LEN + 4 || f[12] != 0x81 || f[13] != 0x00) return 1;
	*plen = get_be16(f + 16);
	if (*plen > 1500) return 1;                           /* 是 EtherType（如 VLAN 内的 IPv4/DHCP），不是我们的帧 */
	if (*plen < 4 || (uint32_t)(HDR_LEN + *plen) > pkt_len) return 2;
	return 0;
}

/* 前 5 个异常包打印原因与十六进制，便于定位 */
static void dump_bad(struct port_stat *st, uint16_t port, const char *why, struct rte_mbuf *m)
{
	if (st->dumped >= 5) return;
	st->dumped++;
	printf("[port %u] bad frame: %s  pkt_len=%u  ol_flags=0x%lx%s  vlan_tci=0x%04x\n", port, why, m->pkt_len,
	       (unsigned long)m->ol_flags, (m->ol_flags & RTE_MBUF_F_RX_VLAN_STRIPPED) ? " (VLAN STRIPPED by NIC)" : "",
	       m->vlan_tci);
	rte_hexdump(stdout, "first 64B", rte_pktmbuf_mtod(m, void *), m->data_len < 64 ? m->data_len : 64);
	fflush(stdout);
}

/* ---------------- 模板 ---------------- */
struct tmpl { uint8_t buf[2048]; uint16_t len; };
static struct tmpl g_tmpl[MAX_PORTS];

static void tmpl_obj_init(struct rte_mempool *mp, void *arg, void *obj, unsigned i)
{
	(void)mp; (void)i;
	struct rte_mbuf *m = obj;
	const struct tmpl *t = arg;
	memcpy((uint8_t *)m->buf_addr + RTE_PKTMBUF_HEADROOM, t->buf, t->len);
}

static void build_tmpl(uint16_t pi)
{
	struct port_ctx *c = &g_ctx[pi];
	struct tmpl *t = &g_tmpl[pi];
	uint8_t *p = t->buf + HDR_LEN;
	memset(t->buf, 0, sizeof(t->buf));

	if (g_sender && !g_read) {            /* 写请求：k × (AW 头 + beats×64) */
		for (int k = 0; k < g_pack; k++) {
			put_be32(p, w0_aw(0)); put_be32(p + 12, 6);     /* awsize=110 */
			memset(p + AW_HDR, 0xA5, g_beats * BEAT);
			p += AW_HDR + g_beats * BEAT;
		}
		write_eth(t->buf, &g_dmac[pi], &c->mac, VC_AW, (uint16_t)(p - t->buf - HDR_LEN));
	} else if (g_sender && g_read) {      /* 读请求：k × AR 头 */
		for (int k = 0; k < g_pack; k++) {
			put_be32(p, w0_ar(0)); put_be32(p + 12, ((uint32_t)(g_beats - 1) << 3) | 6);
			p += AR_HDR;
		}
		write_eth(t->buf, &g_dmac[pi], &c->mac, VC_AR, (uint16_t)(p - t->buf - HDR_LEN));
	} else {                              /* reflector 读响应：最多 8 个 (R 头 + 1 拍)，按需截断 */
		for (int k = 0; k < MAX_BEATS; k++) {
			put_be32(p, w0_r(0, 1));
			memset(p + R_HDR, 0x5A, BEAT);
			p += R_HDR + BEAT;
		}
		/* 以太头在发送时按请求改写 */
	}
	t->len = (uint16_t)(p - t->buf);
	c->tmpl_len = t->len;
}

/* ---------------- 统计辅助 ---------------- */
static uint64_t g_hz, g_ns_mult;               /* ns = cyc * g_ns_mult >> 20，避免快路径除法 */
static inline void hist_add(struct port_stat *s, uint64_t cyc)
{
	uint64_t ns = (cyc * g_ns_mult) >> 20;
	uint64_t b = ns / HIST_NS;
	if (b < HIST_N) s->hist[b]++;
	else { uint64_t b2 = (ns - SLOW_NS) / (HIST2_US * 1000); s->hist2[b2 < HIST2_N ? b2 : HIST2_N - 1]++; s->slow++; }
	s->rtt_sum_ns += ns;
	if (ns > s->rtt_max_ns) s->rtt_max_ns = ns;
}

static inline void tx_all(uint16_t port, struct rte_mbuf **pk, uint16_t n)
{
	uint16_t sent = 0;
	while (sent < n && !g_quit)                    /* 不丢：发不完就重试 */
		sent += rte_eth_tx_burst(port, 0, pk + sent, n - sent);
	if (sent < n) rte_pktmbuf_free_bulk(pk + sent, n - sent);
}

/* ---------------- sender ---------------- */
static int sender_loop(struct port_ctx *c)
{
	struct port_stat *s = &c->st;
	uint64_t ts[ID_SPACE];
	uint8_t  outst[ID_SPACE] = {0};
	uint32_t next_id = 0;
	int inflight = 0;
	const uint16_t len = g_tmpl[c - g_ctx].len;
	const uint16_t txn_len = g_read ? AR_HDR : AW_HDR + g_beats * BEAT;
	struct rte_mbuf *tx[MAX_BURST], *rx[MAX_BURST];

	while (!g_quit) {
		uint64_t t_s = rte_rdtsc();
		bool did = false;
		/* ---- 发送：受窗口和 ID 占用约束 ---- */
		int can = (g_window - inflight) / g_pack;
		if (can > g_burst) can = g_burst;
		int n = 0;
		for (; n < can; n++) {                        /* 检查接下来 pack 个 ID 均空闲 */
			bool ok = true;
			for (int k = 0; k < g_pack; k++)
				if (outst[(next_id + n * g_pack + k) & (ID_SPACE - 1)]) { ok = false; break; }
			if (!ok) break;
		}
		if (n > 0 && rte_pktmbuf_alloc_bulk(c->tmpl_pool, tx, n) == 0) {
			uint64_t now = rte_rdtsc();
			for (int i = 0; i < n; i++) {
				uint8_t *p = rte_pktmbuf_mtod(tx[i], uint8_t *) + HDR_LEN;
				for (int k = 0; k < g_pack; k++, p += txn_len) {
					uint32_t id = next_id;
					put_be32(p, g_read ? w0_ar(id) : w0_aw(id));
					outst[id] = 1; ts[id] = now;
					next_id = (next_id + 1) & (ID_SPACE - 1);
				}
				tx[i]->data_len = tx[i]->pkt_len = len;
			}
			inflight += n * g_pack;
			did = true;
			tx_all(c->port, tx, (uint16_t)n);
			s->tx_pkts += n;
			s->tx_wire_bytes += (uint64_t)n * frame_wire(len);
		}

		/* ---- 接收响应 ---- */
		uint16_t nr = rte_eth_rx_burst(c->port, 0, rx, MAX_BURST);
		if (nr == 0) { if (did) s->busy_cyc += rte_rdtsc() - t_s; continue; }
		s->rx_hits++;
		uint64_t now = rte_rdtsc();
		for (uint16_t i = 0; i < nr; i++) {
			uint8_t *f = rte_pktmbuf_mtod(rx[i], uint8_t *);
			uint16_t plen = 0, off = 0;
			uint8_t *p = f + HDR_LEN;
			s->rx_pkts++; s->rx_wire_bytes += frame_wire(rx[i]->pkt_len);
			int cls = classify(f, rx[i]->pkt_len, &plen);
			if (cls == 1) { s->ign++; continue; }
			if (cls == 2) { s->err++; dump_bad(s, c->port, "Length > frame", rx[i]); continue; }
			uint8_t vc = f[14] >> 5, ft0 = p[0] >> 4;           /* 按 frame_type 分发，PCP 只做核对 */
			if (ft0 != FT_B && ft0 != FT_R) { s->err++; dump_bad(s, c->port, "unexpected frame_type", rx[i]); continue; }
			if (vc != (ft0 == FT_B ? VC_B : VC_R)) { s->pcpx++; if (s->pcpx <= 3) dump_bad(s, c->port, "PCP != VC (rewritten?)", rx[i]); }
			vc = ft0 == FT_B ? VC_B : VC_R;
			while (off < plen) {
				uint32_t w0 = get_be32(p + off), id = (w0 >> 19) & 0x1FF, ft = w0 >> 28, step, data = 0;
				if (vc == VC_B && ft == FT_B) step = B_RSP;
				else if (vc == VC_R && ft == FT_R) { data = (((w0 >> 17) & 3) + 1) * BEAT; step = R_HDR + data; }
				else { s->err++; break; }
				if (!outst[id]) s->err++;
				else { outst[id] = 0; inflight--; s->txn_done++; s->data_bytes += g_read ? data : (uint64_t)g_beats * BEAT; hist_add(s, now - ts[id]); }
				off += step;
			}
		}
		rte_pktmbuf_free_bulk(rx, nr);
		s->busy_cyc += rte_rdtsc() - t_s;
	}
	return 0;
}

/* ---------------- sender（收发分核） ---------------- */
static int sender_tx_loop(struct port_ctx *c)
{
	struct port_stat *s = &c->st_tx;
	uint32_t next_id = 0;
	const uint16_t len = g_tmpl[c - g_ctx].len;
	const uint16_t txn_len = g_read ? AR_HDR : AW_HDR + g_beats * BEAT;
	const uint16_t wire = frame_wire(len);
	struct rte_mbuf *tx[MAX_BURST];

	while (!g_quit) {
		uint64_t t_s = rte_rdtsc();
		uint64_t done = __atomic_load_n(&c->done_txn, __ATOMIC_ACQUIRE);
		int can = (int)((uint64_t)g_window - (c->tx_txn - done)) / g_pack;
		if (can > g_burst) can = g_burst;
		int n = 0;
		for (; n < can; n++) {
			bool ok = true;
			for (int k = 0; k < g_pack; k++)
				if (__atomic_load_n(&c->outst[(next_id + n * g_pack + k) & (ID_SPACE - 1)], __ATOMIC_ACQUIRE)) { ok = false; break; }
			if (!ok) break;
		}
		if (n <= 0 || rte_pktmbuf_alloc_bulk(c->tmpl_pool, tx, n) != 0) continue;
		uint64_t now = rte_rdtsc();
		for (int i = 0; i < n; i++) {
			uint8_t *p = rte_pktmbuf_mtod(tx[i], uint8_t *) + HDR_LEN;
			for (int k = 0; k < g_pack; k++, p += txn_len) {
				uint32_t id = next_id;
				put_be32(p, g_read ? w0_ar(id) : w0_aw(id));
				c->ts[id] = now;
				__atomic_store_n(&c->outst[id], 1, __ATOMIC_RELEASE);
				next_id = (next_id + 1) & (ID_SPACE - 1);
			}
			tx[i]->data_len = tx[i]->pkt_len = len;
		}
		__atomic_store_n(&c->tx_txn, c->tx_txn + (uint64_t)n * g_pack, __ATOMIC_RELEASE);
		tx_all(c->port, tx, (uint16_t)n);
		s->tx_pkts += n;
		s->tx_wire_bytes += (uint64_t)n * wire;
		s->busy_cyc += rte_rdtsc() - t_s;
	}
	return 0;
}

static int sender_rx_loop(struct port_ctx *c)
{
	struct port_stat *s = &c->st;
	struct rte_mbuf *rx[MAX_BURST];

	while (!g_quit) {
		uint64_t t_s = rte_rdtsc();
		uint16_t nr = rte_eth_rx_burst(c->port, 0, rx, MAX_BURST);
		if (nr == 0) continue;
		s->rx_hits++;
		uint64_t now = rte_rdtsc(), done = 0;
		for (uint16_t i = 0; i < nr; i++) {
			uint8_t *f = rte_pktmbuf_mtod(rx[i], uint8_t *);
			uint16_t plen = 0, off = 0;
			uint8_t *p = f + HDR_LEN;
			s->rx_pkts++; s->rx_wire_bytes += frame_wire(rx[i]->pkt_len);
			int cls = classify(f, rx[i]->pkt_len, &plen);
			if (cls == 1) { s->ign++; continue; }
			if (cls == 2) { s->err++; dump_bad(s, c->port, "Length > frame", rx[i]); continue; }
			uint8_t vc = f[14] >> 5, ft0 = p[0] >> 4;
			if (ft0 != FT_B && ft0 != FT_R) { s->err++; dump_bad(s, c->port, "unexpected frame_type", rx[i]); continue; }
			if (vc != (ft0 == FT_B ? VC_B : VC_R)) { s->pcpx++; if (s->pcpx <= 3) dump_bad(s, c->port, "PCP != VC (rewritten?)", rx[i]); }
			vc = ft0 == FT_B ? VC_B : VC_R;
			while (off < plen) {
				uint32_t w0 = get_be32(p + off), id = (w0 >> 19) & 0x1FF, ft = w0 >> 28, step, data = 0;
				if (vc == VC_B && ft == FT_B) step = B_RSP;
				else if (vc == VC_R && ft == FT_R) { data = (((w0 >> 17) & 3) + 1) * BEAT; step = R_HDR + data; }
				else { s->err++; break; }
				if (!__atomic_load_n(&c->outst[id], __ATOMIC_ACQUIRE)) s->err++;
				else {
					hist_add(s, now - c->ts[id]);
					__atomic_store_n(&c->outst[id], 0, __ATOMIC_RELEASE);
					done++; s->data_bytes += g_read ? data : (uint64_t)g_beats * BEAT;
				}
				off += step;
			}
		}
		s->txn_done += done;
		__atomic_store_n(&c->done_txn, c->done_txn + done, __ATOMIC_RELEASE);
		rte_pktmbuf_free_bulk(rx, nr);
		s->busy_cyc += rte_rdtsc() - t_s;
	}
	return 0;
}

static int worker_tx(void *arg) { return sender_tx_loop(arg); }
static int worker_rx(void *arg) { return sender_rx_loop(arg); }

/* ---------------- reflector ---------------- */
static int reflector_loop(struct port_ctx *c)
{
	struct port_stat *s = &c->st;
	struct rte_mbuf *rx[MAX_BURST], *tx[MAX_BURST * 4];

	while (!g_quit) {
		uint64_t t_s = rte_rdtsc();
		uint16_t nr = rte_eth_rx_burst(c->port, 0, rx, MAX_BURST), nt = 0;
		if (nr == 0) continue;
		s->rx_hits++;
		for (uint16_t i = 0; i < nr; i++) {
			struct rte_mbuf *m = rx[i];
			uint8_t *f = rte_pktmbuf_mtod(m, uint8_t *);
			uint16_t plen = 0;
			s->rx_pkts++; s->rx_wire_bytes += frame_wire(m->pkt_len);
			int cls = classify(f, m->pkt_len, &plen);
			if (cls == 1) { s->ign++; rte_pktmbuf_free(m); continue; }
			if (cls == 2) { s->err++; dump_bad(s, c->port, "Length > frame", m); rte_pktmbuf_free(m); continue; }
			uint8_t ft0 = f[HDR_LEN] >> 4, pcp = f[14] >> 5, vc;     /* 按 frame_type 分发，PCP 只做核对 */
			vc = (ft0 == FT_WRFULL || ft0 == FT_WR) ? VC_AW : (ft0 == FT_AR ? VC_AR : 0xFF);
			if (vc != 0xFF && pcp != vc) { s->pcpx++; if (s->pcpx <= 3) dump_bad(s, c->port, "PCP != VC (rewritten?)", m); }
			if (vc == VC_AW) {                         /* 写：原地改写为 k 个 wrRsp */
				uint32_t ids[16]; int k = 0; uint16_t off = 0;
				while (off < plen && k < 16) {
					uint32_t w0 = get_be32(f + HDR_LEN + off), ft = w0 >> 28, b = ((w0 >> 17) & 3) + 1;
					if (ft == FT_WRFULL) off += AW_HDR + b * BEAT;
					else if (ft == FT_WR) off += AW_HDR + b * (8 + BEAT);
					else { s->err++; break; }
					ids[k++] = (w0 >> 19) & 0x1FF;
				}
				uint8_t *p = f + HDR_LEN;
				for (int j = 0; j < k; j++, p += B_RSP) { put_be32(p, w0_b(ids[j])); put_be32(p + 4, 0); put_be32(p + 8, 0); }
				struct rte_ether_addr d; memcpy(&d, f, 6);
				write_eth(f, (struct rte_ether_addr *)(f + 6), &d, VC_B, (uint16_t)(k * B_RSP));
				m->data_len = m->pkt_len = HDR_LEN + k * B_RSP;
				tx[nt++] = m;
			} else if (vc == VC_AR) {                  /* 读：按 ≤8 拍组 R 包，用预填模板 */
				struct rte_mbuf *r = NULL; uint8_t *rp = NULL; uint32_t beats_in = 0;
				uint16_t nt0 = nt;                     /* 本请求生成的 R 包从 tx[nt0] 开始 */
				for (uint16_t off = 0; off + AR_HDR <= plen; off += AR_HDR) {
					uint32_t w0 = get_be32(f + HDR_LEN + off), w3 = get_be32(f + HDR_LEN + off + 12);
					if ((w0 >> 28) != FT_AR) { s->err++; break; }
					uint32_t id = (w0 >> 19) & 0x1FF, b = ((w3 >> 3) & 3) + 1;
					if (r && beats_in + b > MAX_BEATS) { r->data_len = r->pkt_len = (uint16_t)(rp - rte_pktmbuf_mtod(r, uint8_t *)); tx[nt++] = r; r = NULL; }
					if (!r) {
						r = rte_pktmbuf_alloc(c->tmpl_pool);
						if (!r) { s->err++; break; }
						rp = rte_pktmbuf_mtod(r, uint8_t *) + HDR_LEN; beats_in = 0;
					}
					put_be32(rp, w0_r(id, b));
					rp += R_HDR + b * BEAT;   /* rdata 已由模板预填（按 1 拍步长；b>1 时数据区覆盖后续模板 R 头，内容不做要求） */
					beats_in += b;
				}
				if (r) { r->data_len = r->pkt_len = (uint16_t)(rp - rte_pktmbuf_mtod(r, uint8_t *)); tx[nt++] = r; }
				/* 为本请求生成的所有 R 包写以太头 */
				struct rte_ether_addr d, sm; memcpy(&d, f + 6, 6); memcpy(&sm, f, 6);
				for (uint16_t j = nt0; j < nt; j++)
					write_eth(rte_pktmbuf_mtod(tx[j], uint8_t *), &d, &sm, VC_R, (uint16_t)(tx[j]->pkt_len - HDR_LEN));
				rte_pktmbuf_free(m);
			} else { s->err++; dump_bad(s, c->port, "unexpected frame_type", m); rte_pktmbuf_free(m); }
		}
		for (uint16_t j = 0; j < nt; j++) s->tx_wire_bytes += frame_wire(tx[j]->pkt_len);
		s->tx_pkts += nt;
		tx_all(c->port, tx, nt);
		s->busy_cyc += rte_rdtsc() - t_s;
	}
	return 0;
}

static int worker(void *arg)
{
	struct port_ctx *c = arg;
	return g_sender ? sender_loop(c) : reflector_loop(c);
}

/* ---------------- 端口初始化 ---------------- */
static void port_init(uint16_t pi)
{
	struct port_ctx *c = &g_ctx[pi];
	int socket = rte_eth_dev_socket_id(pi);
	char name[32];
	uint16_t nrxd = NB_RXD, ntxd = NB_TXD;
	struct rte_eth_conf conf; memset(&conf, 0, sizeof(conf));   /* 不开 VLAN strip */

	c->port = pi;
	snprintf(name, sizeof(name), "rx%u", pi);
	c->rx_pool = rte_pktmbuf_pool_create(name, RX_POOL_N, POOL_CACHE, 0, RTE_MBUF_DEFAULT_BUF_SIZE, socket);
	if (!c->rx_pool) rte_exit(EXIT_FAILURE, "rx pool %u\n", pi);

	if (rte_eth_dev_configure(pi, 1, 1, &conf) < 0 ||
	    rte_eth_dev_adjust_nb_rx_tx_desc(pi, &nrxd, &ntxd) < 0 ||
	    rte_eth_rx_queue_setup(pi, 0, nrxd, socket, NULL, c->rx_pool) < 0 ||
	    rte_eth_tx_queue_setup(pi, 0, ntxd, socket, NULL) < 0)
		rte_exit(EXIT_FAILURE, "port %u setup failed\n", pi);
	rte_eth_macaddr_get(pi, &c->mac);

	build_tmpl(pi);
	snprintf(name, sizeof(name), "tmpl%u", pi);
	c->tmpl_pool = rte_pktmbuf_pool_create(name, TX_POOL_N, POOL_CACHE, 0, RTE_PKTMBUF_HEADROOM + 2048, socket);
	if (!c->tmpl_pool) rte_exit(EXIT_FAILURE, "tmpl pool %u\n", pi);
	rte_mempool_obj_iter(c->tmpl_pool, tmpl_obj_init, &g_tmpl[pi]);

	if (rte_eth_dev_start(pi) < 0) rte_exit(EXIT_FAILURE, "port %u start\n", pi);
	rte_eth_promiscuous_enable(pi);
}

/* ---------------- 统计输出 ---------------- */
static uint64_t pct(const struct port_stat *st, uint64_t total, double q)
{
	uint64_t target = (uint64_t)(total * q), acc = 0;
	for (int i = 0; i < HIST_N; i++) { acc += st->hist[i]; if (acc > target) return (uint64_t)(i + 1) * HIST_NS; }
	for (int i = 0; i < HIST2_N; i++) { acc += st->hist2[i]; if (acc > target) return SLOW_NS + (uint64_t)(i + 1) * HIST2_US * 1000; }
	return SLOW_NS + (uint64_t)HIST2_N * HIST2_US * 1000;
}

static void print_stats(struct port_stat *prev, struct port_stat *prev_tx, double dt, bool final)
{
	static int line;
	if (final || line++ % 20 == 0)
		printf("%sport txMpps txWireG rxMpps rxWireG txnM/s dataG  meanus  p50us  p99us p999us   maxus     slow busyT%% busyR%% cyc/pk  rxB     txPkts     rxPkts   err   ign  pcpx imissed nombuf\n",
		       final ? "---- total/avg ----\n" : "");
	for (uint16_t i = 0; i < g_nb_ports; i++) {
		struct port_stat *st = &g_ctx[i].st, *o = &prev[i], *tt = &g_ctx[i].st_tx, *ot = &prev_tx[i];
		struct rte_eth_stats es; rte_eth_stats_get(i, &es);
		uint64_t txp = st->tx_pkts + tt->tx_pkts, otxp = o->tx_pkts + ot->tx_pkts;
		uint64_t txw = st->tx_wire_bytes + tt->tx_wire_bytes, otxw = o->tx_wire_bytes + ot->tx_wire_bytes;
		uint64_t bT = tt->busy_cyc - ot->busy_cyc, bR = st->busy_cyc - o->busy_cyc;
		uint64_t pk = (txp - otxp) + (st->rx_pkts - o->rx_pkts);
		uint64_t tot = st->txn_done;
		/* 单核模式下全部计在 st 里：busyT 显示 '-' 的等价值 0 */
		printf("%-4u %6.2f %7.1f %6.2f %7.1f %6.2f %5.1f %7.2f %6.2f %6.2f %6.1f %7.1f %8lu %6.1f %6.1f %6.0f %4.1f %10lu %10lu %5lu %5lu %5lu %7lu %6lu\n", i,
		       (txp - otxp) / dt / 1e6, (txw - otxw) * 8 / dt / 1e9,
		       (st->rx_pkts - o->rx_pkts) / dt / 1e6, (st->rx_wire_bytes - o->rx_wire_bytes) * 8 / dt / 1e9,
		       (st->txn_done - o->txn_done) / dt / 1e6, (st->data_bytes - o->data_bytes) * 8 / dt / 1e9,
		       tot ? st->rtt_sum_ns / 1e3 / tot : 0,
		       tot ? pct(st, tot, 0.5) / 1e3 : 0, tot ? pct(st, tot, 0.99) / 1e3 : 0,
		       tot ? pct(st, tot, 0.999) / 1e3 : 0, st->rtt_max_ns / 1e3, (unsigned long)st->slow,
		       bT * 100.0 / (dt * g_hz), bR * 100.0 / (dt * g_hz),
		       pk ? (double)(bT + bR) / pk : 0,
		       (st->rx_hits - o->rx_hits) ? (double)(st->rx_pkts - o->rx_pkts) / (st->rx_hits - o->rx_hits) : 0,
		       (unsigned long)txp, (unsigned long)st->rx_pkts, (unsigned long)st->err, (unsigned long)st->ign,
		       (unsigned long)st->pcpx, (unsigned long)es.imissed, (unsigned long)es.rx_nombuf);
		if (!final) { *o = *st; *ot = *tt; }
	}
	fflush(stdout);
}

/* ---------------- 参数 ---------------- */
static void usage(void)
{
	printf("axiperf [EAL] -- --mode sender|reflector [--op write|read] [--window N] [--pack N]\n"
	       "        [--beats N] [--burst N] [--time SEC] [--dmac PORT,xx:xx:xx:xx:xx:xx] [--vid N] [--nosplit]\n");
}

static void parse_args(int argc, char **argv)
{
	static struct option lo[] = {
		{"mode", 1, 0, 'm'}, {"op", 1, 0, 'o'}, {"window", 1, 0, 'w'}, {"pack", 1, 0, 'p'},
		{"beats", 1, 0, 'b'}, {"burst", 1, 0, 'u'}, {"time", 1, 0, 't'}, {"dmac", 1, 0, 'd'}, {"vid", 1, 0, 'v'}, {"nosplit", 0, 0, 'n'}, {0, 0, 0, 0}};
	int o;
	for (int i = 0; i < MAX_PORTS; i++) {           /* 默认 DMAC：02:00:00:00:01:0i */
		uint8_t d[6] = {0x02, 0, 0, 0, 0x01, (uint8_t)i};
		memcpy(&g_dmac[i], d, 6);
	}
	while ((o = getopt_long(argc, argv, "", lo, NULL)) != -1) {
		switch (o) {
		case 'm': g_sender = strcmp(optarg, "reflector") != 0; break;
		case 'o': g_read = strcmp(optarg, "read") == 0; break;
		case 'w': g_window = atoi(optarg); break;
		case 'p': g_pack = atoi(optarg); break;
		case 'b': g_beats = atoi(optarg); break;
		case 'u': g_burst = atoi(optarg); break;
		case 't': g_time = atoi(optarg); break;
		case 'v': g_vid = atoi(optarg) & 0xFFF; break;
		case 'n': g_split = false; break;
		case 'd': {
			int pi = atoi(optarg); char *mac = strchr(optarg, ',');
			if (!mac || pi >= MAX_PORTS || rte_ether_unformat_addr(mac + 1, &g_dmac[pi]) < 0) { usage(); exit(1); }
			break; }
		default: usage(); exit(1);
		}
	}
	if (g_window < 1 || g_window > 511 || g_beats < 1 || g_beats > 4 || g_burst < 1 || g_burst > MAX_BURST ||
	    g_pack < 1 || (!g_read && g_pack * g_beats > MAX_BEATS) || (g_read && g_pack > 4)) {
		printf("参数越界：window 1~511，beats 1~4，burst 1~%d，写 pack×beats≤8，读 pack≤4\n", MAX_BURST);
		exit(1);
	}
}

static void on_sig(int s) { (void)s; g_quit = true; }

int main(int argc, char **argv)
{
	int ret = rte_eal_init(argc, argv);
	if (ret < 0) rte_exit(EXIT_FAILURE, "EAL init failed\n");
	parse_args(argc - ret, argv + ret);
	signal(SIGINT, on_sig); signal(SIGTERM, on_sig);
	g_hz = rte_get_tsc_hz();
	g_ns_mult = (1000000000ULL << 20) / g_hz;

	g_nb_ports = rte_eth_dev_count_avail();
	if (g_nb_ports == 0 || g_nb_ports > MAX_PORTS) rte_exit(EXIT_FAILURE, "ports: %u\n", g_nb_ports);
	unsigned per = (g_sender && g_split) ? 2 : 1;
	if (rte_lcore_count() < g_nb_ports * per + 1)
		rte_exit(EXIT_FAILURE, "需要 %u 个 lcore（1 统计 + 每端口 %u 个）\n", g_nb_ports * per + 1, per);

	for (uint16_t i = 0; i < g_nb_ports; i++) port_init(i);
	printf("mode=%s op=%s window=%d pack=%d beats=%d burst=%d vid=%d split=%d ports=%u\n", g_sender ? "sender" : "reflector",
	       g_read ? "read" : "write", g_window, g_pack, g_beats, g_burst, g_vid, g_sender && g_split, g_nb_ports);

	unsigned lc, wi = 0;
	RTE_LCORE_FOREACH_WORKER(lc) {
		if (wi >= g_nb_ports * per) break;
		struct port_ctx *c = &g_ctx[wi / per];
		if (per == 2) rte_eal_remote_launch((wi % 2) ? worker_rx : worker_tx, c, lc);   /* 端口 i：TX 核在前，RX 核在后 */
		else rte_eal_remote_launch(worker, c, lc);
		wi++;
	}

	static struct port_stat prev[MAX_PORTS], prev_tx[MAX_PORTS];
	uint64_t t0 = rte_get_timer_cycles(), last = t0, hz = rte_get_timer_hz();
	while (!g_quit) {
		rte_delay_ms(1000);
		uint64_t now = rte_get_timer_cycles();
		print_stats(prev, prev_tx, (double)(now - last) / hz, false);
		last = now;
		if (g_time && now - t0 >= (uint64_t)g_time * hz) g_quit = true;
	}
	rte_eal_mp_wait_lcore();
	{ static struct port_stat zero[MAX_PORTS], zero_tx[MAX_PORTS]; print_stats(zero, zero_tx, (double)(rte_get_timer_cycles() - t0) / hz, true); }
	for (uint16_t i = 0; i < g_nb_ports; i++) { rte_eth_dev_stop(i); rte_eth_dev_close(i); }
	rte_eal_cleanup();
	return 0;
}
