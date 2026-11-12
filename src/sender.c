/*
 * sender：按窗口发请求、收响应、记 RTT
 *
 * 在途表（outst / ts）与 done_txn 放在流上下文里：
 *   单核模式同一核调用 tx_step 与 rx_step；
 *   分核模式各发送核只调用 tx_step，RX 核只调用 rx_step。
 * --tx-cores N：一条流（同一 MAC、同一接收队列）由 N 个发送核发，每核独占一个发送队列和一段连续 ID
 *   （0..window-1 均分），各有一个空闲环。RX 核按 owner[id] 把完成的 ID 还回所属发送核的环，
 *   每个环都是单生产者 / 单消费者。
 */
#include <rte_cycles.h>

#include "worker.h"
#include "hdr.h"
#include "axi.h"
#include "stats.h"
#include "capture.h"
#include "hwts.h"

struct tsn_ctr g_tsn[MAX_PORTS][2];

/*
 * 超时回收，在 RX 侧做（每 tmo/16 扫一遍全部 ID）。
 * outst[id] 只有两个写者且互斥：TX 把空闲 ID 置 1，RX 把在途 ID 清 0（收到响应或超时），
 * 所以快路径不需要原子读改写。超时的 ID 与正常完成一样归还空闲池（--id-seq 时只清标志）。
 */
static void reclaim(struct flow_ctx *c, struct port_stat *s, uint64_t now, uint32_t *head)   /* head：各发送核环的生产位置 */
{
	uint64_t n = 0;
	for (uint32_t id = 0; id < ID_SPACE; id++) {
		if (!__atomic_load_n(&c->outst[id], __ATOMIC_ACQUIRE)) continue;
		/* 有符号比较：now 取得之后 TX 核可能刚发出这个 ID，ts 会比 now 新 */
		if ((int64_t)(now - c->ts[id]) <= (int64_t)g_cfg.tmo_cyc) continue;
		__atomic_store_n(&c->outst[id], 0, __ATOMIC_RELEASE);
		if (!g_cfg.id_seq) { const uint8_t k = c->owner[id]; c->lane[k].idq[head[k]++ & (ID_SPACE - 1)] = (uint16_t)id; }
		n++;
	}
	if (n) { s->lost += n; __atomic_store_n(&c->lost_txn, c->lost_txn + n, __ATOMIC_RELEASE); }
}

/* x / pack：pack 通常为 1、2、4，避免快路径上的整数除法（perf 里 idiv / div 约占 TX 核 5%） */
static inline int div_pack(int x, int pack)
{
	switch (pack) {
	case 1: return x;
	case 2: return x >> 1;
	case 4: return x >> 2;
	default: return x / pack;
	}
}

/* 响应只带线上 9 位 ID：对应的两个逻辑 ID 都在途时，算到先发出的那个上 */
static inline uint32_t id_resolve(const struct flow_ctx *c, uint32_t id)
{
	const uint32_t hi = id + WIRE_IDS;
	if (id < WIRE_IDS && __atomic_load_n(&c->outst[hi], __ATOMIC_ACQUIRE) &&
	    (!__atomic_load_n(&c->outst[id], __ATOMIC_ACQUIRE) || (int64_t)(c->ts[hi] - c->ts[id]) < 0))
		return hi;
	return id;
}

/* --random 读响应：两个逻辑 ID 都在途且拍数不同时，按响应的拍数挑（拍数相同时同 id_resolve，挑哪个都不影响数据校验） */
static inline uint32_t id_resolve_rd(const struct flow_ctx *c, uint32_t id, uint32_t nb)
{
	const uint32_t hi = id + WIRE_IDS;
	if (id < WIRE_IDS && __atomic_load_n(&c->outst[hi], __ATOMIC_ACQUIRE) && __atomic_load_n(&c->outst[id], __ATOMIC_ACQUIRE) &&
	    c->nb[id] != c->nb[hi])
		return c->nb[hi] == nb ? hi : id;
	return id_resolve(c, id);
}

/* 发一批请求：受窗口与 ID 占用约束。返回发出的包数 */
static inline int tx_step(struct flow_ctx *c, struct tx_lane *L, struct port_stat *s)
{
	struct rte_mbuf *tx[MAX_BURST];
	if (g_cfg.gap_cyc && rte_rdtsc() < L->next_tx) return 0;   /* --gap-us：没到时间不发 */
	const int pack = req_pack(c->read), beats = g_cfg.beats;
	const bool rd = c->read;
	const uint16_t hl = hdr_len(), len = c->req_len, txn_len = axi_req_txn_len(rd, beats);
	const bool seq = g_cfg.id_seq;
	const uint32_t step = id_step();             /* E100c 的 ID 分配见 common.h */
	uint32_t next_id = L->next_id;               /* 局部副本，批末写回 */
	uint32_t tail = L->idq_tail;
	int n = 0;
	if (seq) {
		uint64_t done = __atomic_load_n(&c->done_txn, __ATOMIC_ACQUIRE);
		uint64_t lost = __atomic_load_n(&c->lost_txn, __ATOMIC_ACQUIRE);
		int can = div_pack((int)((uint64_t)g_cfg.window - (L->tx_txn - done - lost)), pack);
		if (can > g_cfg.burst) can = g_cfg.burst;
		for (; n < can; n++) {                   /* 接下来 pack 个 ID 均须空闲 */
			bool ok = true;
			for (int k = 0; k < pack; k++) {
				uint32_t id = (next_id + (uint32_t)(n * pack + k) * step) & (ID_SPACE - 1);
				if (__atomic_load_n(&c->outst[id], __ATOMIC_ACQUIRE)) { ok = false; break; }
			}
			if (!ok) break;
		}
	} else {
		/*
		 * 空闲池：环里初始只放 --window 个 ID，超时回收的也还回环，所以空闲 ID 数就是窗口余量，
		 * 只需读 RX 侧的 idq_head 这一条 cache line，不用再读 done_txn / lost_txn
		 */
		n = div_pack((int)(__atomic_load_n(&L->idq_head, __ATOMIC_ACQUIRE) - tail), pack);
		if (n > g_cfg.burst) n = g_cfg.burst;
	}
	if (g_cfg.count && (uint64_t)n > g_cfg.count - s->tx_pkts) n = (int)(g_cfg.count - s->tx_pkts);   /* --count：发满为止 */
	if (n <= 0) return 0;                        /* 窗口满或 ID 被占：不计 busy，也不读 TSC */
	if (g_cfg.gap_cyc) n = 1;
	uint64_t t_s = rte_rdtsc();
	if (g_cfg.gap_cyc) L->next_tx = t_s + g_cfg.gap_cyc;
	if (rte_pktmbuf_alloc_bulk(c->tmpl_pool, tx, n) != 0) return 0;

	const uint32_t uport = (rd && g_cfg.e100c_rport >= 0) ? (uint32_t)g_cfg.e100c_rport : g_cfg.e100c_port[c->port];
	uint32_t tsn = __atomic_fetch_add(&g_tsn[c->port][rd].v, (uint32_t)(n * pack), __ATOMIC_RELAXED);
	uint64_t now = rte_rdtsc();
	for (int i = 0; i < n; i++) {
		uint8_t *p = rte_pktmbuf_mtod(tx[i], uint8_t *) + hl;
		for (int k = 0; k < pack; k++, p += txn_len) {
			uint32_t id;
			if (seq) { id = next_id; next_id = (id + step) & (ID_SPACE - 1); }
			else id = L->idq[tail++ & (ID_SPACE - 1)];
			axi_req_set(p, rd, id & (WIRE_IDS - 1), beats, uport, tsn++);
			if (k == 0 && g_cfg.req_tag) put_be16(rte_pktmbuf_mtod(tx[i], uint8_t *) + 6, (uint16_t)(id & (WIRE_IDS - 1)));   /* --req-tag：OISA tag_id = AXI ID */
			c->ts[id] = now;
			__atomic_store_n(&c->outst[id], 1, __ATOMIC_RELEASE);
		}
		tx[i]->data_len = tx[i]->pkt_len = len;
		tap(&s->dump_tx, c->port, "TX", tx[i]);
	}
	L->next_id = next_id;
	L->idq_tail = tail;
	if (seq) __atomic_store_n(&L->tx_txn, L->tx_txn + (uint64_t)n * pack, __ATOMIC_RELEASE);
	tx_all(c->port, L->txq, tx, (uint16_t)n);
	swd_add(s, rte_rdtsc() - now, (uint32_t)n);  /* 软件发送时延：RTT 起点 → tx_burst 返回 */
	s->tx_pkts += n;
	s->tx_wire_bytes += (uint64_t)n * frame_wire(len);
	s->busy_cyc += rte_rdtsc() - t_s;
	return n;
}

/*
 * --random：逐帧现场构造请求（不用预填模板的 payload），比 tx_step 慢。
 *   每帧事务数在 1..--pack（读为 --rpack）间随机，每个事务拍数在 1..--beats 间随机，
 *   写帧总拍数不超过 8；写数据为自校验随机图样。各 ID 的拍数记在 c->nb，收到响应时用来核对读数据和统计。
 *   只支持每条流 1 个发送核（--tx-cores 1）。
 */
static int tx_step_rand(struct flow_ctx *c, struct tx_lane *L, struct port_stat *s)
{
	struct rte_mbuf *tx[MAX_BURST];
	if (g_cfg.gap_cyc && rte_rdtsc() < L->next_tx) return 0;
	const bool rd = c->read, seq = g_cfg.id_seq;
	const int pmax = req_pack(rd), bmax = g_cfg.beats;
	const uint16_t hl = hdr_len();
	const uint32_t step = id_step();
	int nmax = g_cfg.gap_cyc ? 1 : g_cfg.burst;
	if (g_cfg.count) {
		if (s->tx_pkts >= g_cfg.count) return 0;
		if ((uint64_t)nmax > g_cfg.count - s->tx_pkts) nmax = (int)(g_cfg.count - s->tx_pkts);
	}
	int avail;                                   /* 还能发出的事务数 */
	if (seq) avail = (int)((uint64_t)g_cfg.window - (L->tx_txn - __atomic_load_n(&c->done_txn, __ATOMIC_ACQUIRE)
	                                                            - __atomic_load_n(&c->lost_txn, __ATOMIC_ACQUIRE)));
	else avail = (int)(__atomic_load_n(&L->idq_head, __ATOMIC_ACQUIRE) - L->idq_tail);
	if (avail <= 0) return 0;

	const uint64_t t_s = rte_rdtsc();
	if (g_cfg.gap_cyc) L->next_tx = t_s + g_cfg.gap_cyc;
	const uint32_t uport = (rd && g_cfg.e100c_rport >= 0) ? (uint32_t)g_cfg.e100c_rport : g_cfg.e100c_port[c->port];
	uint32_t next_id = L->next_id, tail = L->idq_tail;
	const uint64_t now = rte_rdtsc();
	uint64_t ntxn = 0, wire = 0;
	int n = 0;
	for (; n < nmax && avail > 0; n++) {
		/* 先定本帧的事务数和各事务拍数 */
		int k = 1 + (int)(rng_next(&L->rng) % (uint64_t)pmax), bl[MAX_TXN], used = 0, j;
		if (k > avail) k = avail;
		for (j = 0; j < k; j++) {
			int b = 1 + (int)(rng_next(&L->rng) % (uint64_t)bmax);
			if (!rd) { if (used >= MAX_BEATS) break; if (used + b > MAX_BEATS) b = MAX_BEATS - used; used += b; }
			bl[j] = b;
		}
		k = j;
		if (seq)                                 /* --id-seq：接下来 k 个 ID 须空闲 */
			for (j = 0; j < k; j++)
				if (__atomic_load_n(&c->outst[(next_id + (uint32_t)j * step) & (ID_SPACE - 1)], __ATOMIC_ACQUIRE)) { k = j; break; }
		if (k <= 0) break;
		struct rte_mbuf *m = rte_pktmbuf_alloc(c->tmpl_pool);   /* 模板里有帧头，payload 现场写 */
		if (!m) break;
		uint8_t *f = rte_pktmbuf_mtod(m, uint8_t *), *p = f + hl;
		uint32_t tsn = __atomic_fetch_add(&g_tsn[c->port][rd].v, (uint32_t)k, __ATOMIC_RELAXED);
		for (j = 0; j < k; j++) {
			uint32_t id;
			if (seq) { id = next_id; next_id = (id + step) & (ID_SPACE - 1); }
			else id = L->idq[tail++ & (ID_SPACE - 1)];
			if (j == 0 && g_cfg.req_tag) put_be16(f + 6, (uint16_t)(id & (WIRE_IDS - 1)));
			p += axi_txn_rand(p, rd, id & (WIRE_IDS - 1), bl[j], uport, tsn++, &L->rng);
			c->nb[id] = (uint8_t)bl[j];
			c->ts[id] = now;
			__atomic_store_n(&c->outst[id], 1, __ATOMIC_RELEASE);
		}
		const uint16_t plen = (uint16_t)(p - (f + hl));
		uint16_t len = (uint16_t)(hl + plen);
		if (g_cfg.ethertype) {                   /* OISA 请求头：length、packing_num 按本帧改，source id 不变 */
			put_be16(f + 2, len);
			put_be16(f + 10, (uint16_t)(((uint16_t)k << 10) | (get_be16(f + 10) & 0x3FF)));
		} else if (!g_cfg.no_len) put_be16(f + 16, plen);
		if (len < g_cfg.min_frame) { memset(f + len, 0, g_cfg.min_frame - len); len = g_cfg.min_frame; }
		m->data_len = m->pkt_len = len;
		tap(&s->dump_tx, c->port, "TX", m);
		tx[n] = m;
		avail -= k; ntxn += (uint64_t)k; wire += frame_wire(len);
	}
	if (n == 0) return 0;
	L->next_id = next_id;
	L->idq_tail = tail;
	if (seq) __atomic_store_n(&L->tx_txn, L->tx_txn + ntxn, __ATOMIC_RELEASE);
	tx_all(c->port, L->txq, tx, (uint16_t)n);
	swd_add(s, rte_rdtsc() - now, (uint32_t)n);
	s->tx_pkts += n;
	s->tx_wire_bytes += wire;
	s->busy_cyc += rte_rdtsc() - t_s;
	return n;
}

/* 收一批响应并销账。返回收到的包数 */
static inline uint16_t rx_step(struct flow_ctx *c, struct port_stat *s)
{
	struct rte_mbuf *rx[MAX_BURST];
	uint64_t t_s = rte_rdtsc();
	const bool pool = !g_cfg.id_seq;
	const uint16_t ntx = c->ntx;
	uint32_t head[MAX_TXC];                      /* 空闲池：各发送核环的生产位置，局部副本，批末发布 */
	if (g_cfg.tmo_cyc && t_s - c->rc_last > g_cfg.tmo_cyc / 16) {   /* 超时回收（收不到包时也要做） */
		for (uint16_t k = 0; k < ntx; k++) head[k] = c->lane[k].idq_head;
		c->rc_last = t_s;
		reclaim(c, s, t_s, head);
		if (pool) for (uint16_t k = 0; k < ntx; k++) __atomic_store_n(&c->lane[k].idq_head, head[k], __ATOMIC_RELEASE);
	}
	uint16_t nr = rte_eth_rx_burst(c->port, c->q, rx, MAX_BURST);
	void *xfer[MAX_BURST];                       /* 别的流转交来的响应（E100c 合并的帧只能进一个队列） */
	const unsigned nx = c->xq ? rte_ring_sc_dequeue_burst(c->xq, xfer, MAX_BURST, NULL) : 0;
	if (nr == 0 && nx == 0) return 0;
	if (nr) s->rx_hits++;

	const uint16_t hl = hdr_len();
	const uint64_t wdata = (uint64_t)g_cfg.beats * BEAT;
	uint64_t now = rte_rdtsc(), done = 0;
	for (uint16_t k = 0; k < ntx; k++) head[k] = c->lane[k].idq_head;
	for (unsigned x = 0; x < nx; x++) {
		const uint64_t v = (uint64_t)(uintptr_t)xfer[x];
		const uint32_t id = id_resolve(c, (uint32_t)(v & 0xFFFF));
		if (id >= ID_SPACE || !__atomic_load_n(&c->outst[id], __ATOMIC_ACQUIRE)) { s->err++; continue; }
		hist_add(s, now - c->ts[id]);
		const uint64_t wb = g_cfg.rand ? (uint64_t)c->nb[id] * BEAT : wdata;   /* 清 outst 前读 */
		__atomic_store_n(&c->outst[id], 0, __ATOMIC_RELEASE);
		if (pool) { const uint8_t o = c->owner[id]; c->lane[o].idq[head[o]++ & (ID_SPACE - 1)] = (uint16_t)id; }
		done++;
		s->data_bytes += c->read ? (v >> 16) : wb;   /* 转交来的读响应不校验数据（帧已在原队列释放） */
	}
	rxd_record(s, c->port, rx, nr, now);
	for (uint16_t i = 0; i < nr && i < RX_PREFETCH; i++) rx_prefetch(rx[i]);
	for (uint16_t i = 0; i < nr; i++) {
		struct rte_mbuf *m = rx[i];
		if (i + RX_PREFETCH < nr) rx_prefetch(rx[i + RX_PREFETCH]);
		uint8_t *f = rte_pktmbuf_mtod(m, uint8_t *);
		struct hdr_info hi;
		s->rx_pkts++; s->rx_wire_bytes += frame_wire(m->pkt_len);

		int r = hdr_parse(f, m->pkt_len, &hi);
		if (r == HDR_IGNORE) { s->ign++; continue; }
		if (r == HDR_BAD) { s->err++; dump_bad(s, c->port, "bad header", m); continue; }
		if (!g_cfg.ethertype && !hdr_to_me(f, &c->addr.src)) { s->xmac++; continue; }   /* 不是发给本流的（泛洪副本等） */

		uint8_t *p = f + hl, ft0 = p[0] >> 4;          /* 按 frame_type 分发，PCP 只做核对 */
		if (ft0 != FT_B && ft0 != FT_R) { s->err++; dump_bad(s, c->port, "unexpected frame_type", m); continue; }
		uint8_t vc = ft0 == FT_B ? VC_B : VC_R;
		if (hi.vc != vc && !g_cfg.ethertype) { s->pcpx++; if (s->pcpx <= 3) dump_bad(s, c->port, "PCP != VC (rewritten?)", m); }
		tap(&s->dump_rx, c->port, "RX", m);

		uint32_t ids[MAX_TXN], data[MAX_TXN];
		bool bad;
		int k = axi_rsp_parse(p, hi.plen, hi.ntxn, vc, ids, data, &bad);
		if (bad) { s->err++; dump_bad(s, c->port, "bad AXI payload", m); }
		uint16_t off = 0;                        /* 当前事务在 payload 里的位置（--random 校验读数据用） */
		for (int j = 0; j < k; j++) {
			uint32_t id = ids[j];
			const uint16_t toff = off;
			off = (uint16_t)(off + (vc == VC_R ? R_HDR + data[j] : B_RSP));
			const uint16_t own = c->xq ? id_owner(id, vc == VC_R) : c->flow;
			if (own != c->flow) {                  /* 各流 source id 不同，这样的合并是 E100c 的错误：记错并转交销账 */
				struct flow_ctx *t = c - c->flow + own;
				s->err++;
				if (s->err <= 3) dump_bad(s, c->port, "E100c merged responses of different sources", m);
				if (rte_ring_mp_enqueue(t->xq, (void *)(uintptr_t)(id | ((uint64_t)data[j] << 16))) != 0) s->err++;
				continue;
			}
			id = (g_cfg.rand && vc == VC_R) ? id_resolve_rd(c, id, data[j] / BEAT) : id_resolve(c, id);
			if (id >= ID_SPACE || !__atomic_load_n(&c->outst[id], __ATOMIC_ACQUIRE)) { s->err++; continue; }   /* 重复 / 超时回收后迟到 */
			hist_add(s, now - c->ts[id]);        /* 清零前读：清零后 TX 可能立即复用并改写 ts */
			uint64_t wb = wdata;
			if (g_cfg.rand) {                    /* --random：拍数按本 ID 发出时记的；读数据逐拍校验 */
				const uint32_t nb = c->nb[id];
				wb = (uint64_t)nb * BEAT;
				const uint32_t got = data[j] / BEAT;
				if (vc == VC_R && !rand_data_ok(p + toff + R_HDR, got, nb)) {
					if (rand_data_ok(p + toff + R_HDR, got, got)) {   /* 数据完好、拍数和请求不同：算协议错误 */
						s->err++;
						if (s->err <= 3) dump_bad(s, c->port, "read beats != request arlen (data intact)", m);
					} else {
						s->derr++;
						if (s->derr <= 3) dump_bad(s, c->port, "read data corrupt", m);
					}
				}
			}
			__atomic_store_n(&c->outst[id], 0, __ATOMIC_RELEASE);
			if (pool) { const uint8_t o = c->owner[id]; c->lane[o].idq[head[o]++ & (ID_SPACE - 1)] = (uint16_t)id; }
			done++;
			s->data_bytes += c->read ? data[j] : wb;
		}
	}
	if (pool)                                    /* 只发布有变化的环，免得无谓地让发送核的 cache line 失效 */
		for (uint16_t k = 0; k < ntx; k++)
			if (head[k] != c->lane[k].idq_head) __atomic_store_n(&c->lane[k].idq_head, head[k], __ATOMIC_RELEASE);
	s->txn_done += done;
	__atomic_store_n(&c->done_txn, c->done_txn + done, __ATOMIC_RELEASE);
	if (nr) rte_pktmbuf_free_bulk(rx, nr);
	s->busy_cyc += rte_rdtsc() - t_s;
	return nr;
}

int sender_loop(void *arg)
{
	struct flow_ctx *c = arg;
	if (g_cfg.rand) {
		c->lane[0].rng = rng_seed(g_cfg.seed, (uint64_t)c->idx * MAX_TXC);
		while (!g_quit) { tx_step_rand(c, &c->lane[0], &c->st); rx_step(c, &c->st); }
		return 0;
	}
	while (!g_quit) {
		tx_step(c, &c->lane[0], &c->st);
		rx_step(c, &c->st);
	}
	return 0;
}

int sender_tx_loop(void *arg)
{
	const struct tx_arg *a = arg;
	struct flow_ctx *c = a->c;
	struct tx_lane *L = &c->lane[a->k];
	if (g_cfg.rand) {
		L->rng = rng_seed(g_cfg.seed, (uint64_t)c->idx * MAX_TXC + a->k);
		while (!g_quit) tx_step_rand(c, L, &L->st);
		return 0;
	}
	while (!g_quit) tx_step(c, L, &L->st);
	return 0;
}

int sender_rx_loop(void *arg)
{
	struct flow_ctx *c = arg;
	while (!g_quit) rx_step(c, &c->st);
	return 0;
}
