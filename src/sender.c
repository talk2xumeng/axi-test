/*
 * sender：按窗口发请求、收响应、记 RTT
 *
 * 在途表（outst / ts）与 tx_txn / done_txn 放在流上下文里：
 *   单核模式同一核调用 tx_step 与 rx_step；
 *   分核模式 TX 核只调用 tx_step，RX 核只调用 rx_step（单生产者 / 单消费者）。
 */
#include <rte_cycles.h>

#include "worker.h"
#include "hdr.h"
#include "axi.h"
#include "stats.h"
#include "capture.h"
#include "hwts.h"

/*
 * 超时回收。与 RX 侧用原子交换抢清零，只有一方拿到 1。
 *   --id-seq：ID 按顺序分配，从 next_id 开始往后即由老到新（next_id 本身若仍在途，
 *             就是最老的那个——正是它挡住了分配）；遇到未超时的在途 ID 即停。
 *   空闲池：ID 不按年龄排列，扫全部；回收的 ID 放进 TX 本地 stash 复用。
 */
static void reclaim(struct flow_ctx *c, struct port_stat *s, uint64_t now)
{
	const bool seq = g_cfg.id_seq;
	uint32_t id = seq ? c->next_id : 0;
	for (int i = 0; i < ID_SPACE; i++, id = (id + 1) & (ID_SPACE - 1)) {
		if (!__atomic_load_n(&c->outst[id], __ATOMIC_ACQUIRE)) continue;
		if (now - c->ts[id] <= g_cfg.tmo_cyc) { if (seq) break; continue; }
		if (__atomic_exchange_n(&c->outst[id], 0, __ATOMIC_ACQ_REL)) {
			c->lost_txn++; s->lost++;
			if (!seq) c->stash[c->nstash++] = (uint16_t)id;
		}
	}
}

/* 空闲池取一个 ID：先 stash，再环（调用方已确认数量足够） */
static inline uint32_t id_pop(struct flow_ctx *c, uint32_t *tail)
{
	if (c->nstash) return c->stash[--c->nstash];
	return c->idq[(*tail)++ & (ID_SPACE - 1)];
}

/* 发一批请求：受窗口与 ID 占用约束。返回发出的包数 */
static inline int tx_step(struct flow_ctx *c, struct port_stat *s)
{
	struct rte_mbuf *tx[MAX_BURST];
	const int pack = req_pack(c->read), beats = g_cfg.beats;
	const bool rd = c->read;
	const uint16_t hl = hdr_len(), len = c->req_len, txn_len = axi_req_txn_len(rd, beats);
	uint64_t t_s = rte_rdtsc();

	uint64_t done = __atomic_load_n(&c->done_txn, __ATOMIC_ACQUIRE);
	int can = (int)((uint64_t)g_cfg.window - (c->tx_txn - done - c->lost_txn)) / pack;
	if (can > g_cfg.burst) can = g_cfg.burst;
	const bool seq = g_cfg.id_seq;
	const uint64_t tmo = g_cfg.tmo_cyc;
	/* 空闲池：丢失的 ID 不会挡住分配，只是悄悄缩小窗口，所以定期回收，而不是等到发不出去 */
	if (!seq && tmo && t_s - c->rc_last > tmo / 16) { c->rc_last = t_s; reclaim(c, s, t_s); }
	uint32_t next_id = c->next_id;               /* 局部副本，批末写回 */
	uint32_t tail = c->idq_tail;
	int n = 0;
	if (seq) {
		for (; n < can; n++) {                   /* 接下来 pack 个 ID 均须空闲 */
			bool ok = true;
			for (int k = 0; k < pack; k++) {
				uint32_t id = (next_id + n * pack + k) & (ID_SPACE - 1);
				if (__atomic_load_n(&c->outst[id], __ATOMIC_ACQUIRE)) { ok = false; break; }
			}
			if (!ok) break;
		}
	} else {                                     /* 空闲池：有多少空闲 ID 就能发多少 */
		uint32_t avail = __atomic_load_n(&c->idq_head, __ATOMIC_ACQUIRE) - tail + c->nstash;
		n = can < (int)(avail / pack) ? can : (int)(avail / pack);
	}
	if (n <= 0) {                                /* 窗口满或 ID 被占：顺便检查超时 */
		if (seq && tmo && t_s - c->rc_last > tmo / 16) { c->rc_last = t_s; reclaim(c, s, t_s); }
		return 0;
	}
	if (rte_pktmbuf_alloc_bulk(c->tmpl_pool, tx, n) != 0) return 0;

	uint64_t now = rte_rdtsc();
	for (int i = 0; i < n; i++) {
		uint8_t *p = rte_pktmbuf_mtod(tx[i], uint8_t *) + hl;
		for (int k = 0; k < pack; k++, p += txn_len) {
			uint32_t id;
			if (seq) { id = next_id; next_id = (id + 1) & (ID_SPACE - 1); }
			else id = id_pop(c, &tail);
			axi_req_set_id(p, rd, id, beats);
			c->ts[id] = now;
			__atomic_store_n(&c->outst[id], 1, __ATOMIC_RELEASE);
		}
		tx[i]->data_len = tx[i]->pkt_len = len;
		tap(&s->dump_tx, c->port, "TX", tx[i]);
	}
	c->next_id = next_id;
	c->idq_tail = tail;
	__atomic_store_n(&c->tx_txn, c->tx_txn + (uint64_t)n * pack, __ATOMIC_RELEASE);
	tx_ctx(c, tx, (uint16_t)n);
	swd_add(s, rte_rdtsc() - now, (uint32_t)n);  /* 软件发送时延：RTT 起点 → tx_burst 返回 */
	s->tx_pkts += n;
	s->tx_wire_bytes += (uint64_t)n * frame_wire(len);
	s->busy_cyc += rte_rdtsc() - t_s;
	return n;
}

/* 收一批响应并销账。返回收到的包数 */
static inline uint16_t rx_step(struct flow_ctx *c, struct port_stat *s)
{
	struct rte_mbuf *rx[MAX_BURST];
	uint64_t t_s = rte_rdtsc();
	uint16_t nr = rte_eth_rx_burst(c->port, c->q, rx, MAX_BURST);
	if (nr == 0) return 0;
	s->rx_hits++;

	const uint16_t hl = hdr_len();
	const uint64_t wdata = (uint64_t)g_cfg.beats * BEAT;
	uint64_t now = rte_rdtsc(), done = 0;
	const bool pool = !g_cfg.id_seq;
	uint32_t head = c->idq_head;                 /* 空闲池：本批完成的 ID 写入环，批末一次发布 */
	rxd_record(s, c->port, rx, nr, now);
	for (uint16_t i = 0; i < nr; i++) {
		struct rte_mbuf *m = rx[i];
		uint8_t *f = rte_pktmbuf_mtod(m, uint8_t *);
		struct hdr_info hi;
		s->rx_pkts++; s->rx_wire_bytes += frame_wire(m->pkt_len);

		int r = hdr_parse(f, m->pkt_len, &hi);
		if (r == HDR_IGNORE) { s->ign++; continue; }
		if (r == HDR_BAD) { s->err++; dump_bad(s, c->port, "bad header", m); continue; }
		if (!hdr_to_me(f, &c->addr.src)) { s->xmac++; continue; }   /* 不是发给本流的（泛洪副本等） */

		uint8_t *p = f + hl, ft0 = p[0] >> 4;          /* 按 frame_type 分发，PCP 只做核对 */
		if (ft0 != FT_B && ft0 != FT_R) { s->err++; dump_bad(s, c->port, "unexpected frame_type", m); continue; }
		uint8_t vc = ft0 == FT_B ? VC_B : VC_R;
		if (hi.vc != vc) { s->pcpx++; if (s->pcpx <= 3) dump_bad(s, c->port, "PCP != VC (rewritten?)", m); }
		tap(&s->dump_rx, c->port, "RX", m);

		uint32_t ids[MAX_TXN], data[MAX_TXN];
		bool bad;
		int k = axi_rsp_parse(p, hi.plen, hi.ntxn, vc, ids, data, &bad);
		if (bad) { s->err++; dump_bad(s, c->port, "bad AXI payload", m); }
		for (int j = 0; j < k; j++) {
			uint32_t id = ids[j];
			if (id >= ID_SPACE || !__atomic_load_n(&c->outst[id], __ATOMIC_ACQUIRE)) { s->err++; continue; }   /* 重复 / 超时回收后迟到 */
			uint64_t t0 = c->ts[id];             /* 清零前读：清零后 TX 可能立即复用并改写 ts */
			if (!__atomic_exchange_n(&c->outst[id], 0, __ATOMIC_ACQ_REL)) { s->err++; continue; }  /* 刚被超时回收 */
			hist_add(s, now - t0);
			if (pool) c->idq[head++ & (ID_SPACE - 1)] = (uint16_t)id;
			done++;
			s->data_bytes += c->read ? data[j] : wdata;
		}
	}
	if (pool) __atomic_store_n(&c->idq_head, head, __ATOMIC_RELEASE);
	s->txn_done += done;
	__atomic_store_n(&c->done_txn, c->done_txn + done, __ATOMIC_RELEASE);
	rte_pktmbuf_free_bulk(rx, nr);
	s->busy_cyc += rte_rdtsc() - t_s;
	return nr;
}

int sender_loop(void *arg)
{
	struct flow_ctx *c = arg;
	while (!g_quit) {
		tx_step(c, &c->st);
		rx_step(c, &c->st);
	}
	return 0;
}

int sender_tx_loop(void *arg)
{
	struct flow_ctx *c = arg;
	while (!g_quit) tx_step(c, &c->st_tx);
	return 0;
}

int sender_rx_loop(void *arg)
{
	struct flow_ctx *c = arg;
	while (!g_quit) rx_step(c, &c->st);
	return 0;
}
