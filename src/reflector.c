/*
 * reflector：收请求、回响应
 *   写请求：原地改写为 k 个 wrRsp（1 请求包 → 1 响应包）
 *   读请求：按每包 ≤ 8 拍组读响应，从预填模板池分配（1 请求包 → 1..n 响应包）
 *   每攒够 --burst 个响应即发出，降低批处理时延
 */
#include <rte_cycles.h>

#include "worker.h"
#include "hdr.h"
#include "axi.h"
#include "capture.h"
#include "hwts.h"

static inline void flush(struct flow_ctx *c, struct port_stat *s, struct rte_mbuf **tx, uint16_t nt)
{
	for (uint16_t j = 0; j < nt; j++) {
		s->tx_wire_bytes += frame_wire(tx[j]->pkt_len);
		tap(&s->dump_tx, c->port, "TX", tx[j]);
	}
	s->tx_pkts += nt;
	tx_ctx(c, tx, nt);
}

/* 合并写响应包收尾：交换地址、写头、定长 */
static inline void b_finish(struct rte_mbuf *bm, uint16_t n, uint16_t hl)
{
	uint8_t *f = rte_pktmbuf_mtod(bm, uint8_t *);
	uint16_t plen = (uint16_t)(n * B_RSP);
	hdr_reply(f, VC_B, n, plen);
	bm->data_len = bm->pkt_len = (uint32_t)(hl + plen);
}

/* 读响应包收尾：写头、定长 */
static inline void r_finish(struct rte_mbuf *r, uint8_t *rp, const uint8_t *req, uint16_t cnt, uint16_t hl)
{
	uint8_t *f = rte_pktmbuf_mtod(r, uint8_t *);
	uint16_t plen = (uint16_t)(rp - f - hl);
	hdr_reply_to(f, req, VC_R, cnt, plen);
	r->data_len = r->pkt_len = (uint32_t)(hl + plen);
}

int reflector_loop(void *arg)
{
	struct flow_ctx *c = arg;
	struct port_stat *s = &c->st;
	struct rte_mbuf *rx[MAX_BURST], *tx[MAX_BURST * 4];
	const uint16_t hl = hdr_len();
	const int bpack = g_cfg.b_pack;          /* 每个写响应包最多合并的 B 个数（1 = 每请求包一个响应包） */

	while (!g_quit) {
		uint64_t t_s = rte_rdtsc();
		uint16_t nr = rte_eth_rx_burst(c->port, c->q, rx, MAX_BURST), nt = 0;
		if (nr == 0) continue;
		struct rte_mbuf *bm = NULL;              /* 正在合并 B 的响应包（复用第一个写请求 mbuf） */
		uint16_t bn = 0;
		s->rx_hits++;
		rxd_record(s, c->port, rx, nr, rte_rdtsc());

		for (uint16_t i = 0; i < nr; i++) {
			struct rte_mbuf *m = rx[i];
			uint8_t *f = rte_pktmbuf_mtod(m, uint8_t *);
			struct hdr_info hi;
			s->rx_pkts++; s->rx_wire_bytes += frame_wire(m->pkt_len);

			int r = hdr_parse(f, m->pkt_len, &hi);
			if (r == HDR_IGNORE) { s->ign++; rte_pktmbuf_free(m); continue; }
			if (r == HDR_BAD) { s->err++; dump_bad(s, c->port, "bad header", m); rte_pktmbuf_free(m); continue; }
			if (!hdr_to_me(f, &c->addr.src)) { s->xmac++; rte_pktmbuf_free(m); continue; }   /* 不是发给本流的：不回 */

			if (g_cfg.drop_every && ++s->dropped % g_cfg.drop_every == 0) { rte_pktmbuf_free(m); continue; }  /* 模拟丢包 */
			uint8_t *p = f + hl;
			uint8_t vc = axi_req_vc(p[0] >> 4);        /* 按 frame_type 分发，PCP 只做核对 */
			if (vc != VC_NONE && hi.vc != vc) { s->pcpx++; if (s->pcpx <= 3) dump_bad(s, c->port, "PCP != VC (rewritten?)", m); }
			tap(&s->dump_rx, c->port, "RX", m);

			uint32_t ids[MAX_TXN], beats[MAX_TXN];
			bool bad;
			if (vc == VC_AW) {
				int k = axi_aw_parse(p, hi.plen, hi.ntxn, ids, &bad);
				if (bad) { s->err++; dump_bad(s, c->port, "bad AXI write payload", m); }
				if (k == 0) { rte_pktmbuf_free(m); continue; }
				if (bpack <= 1) {
					uint16_t plen = axi_b_build(p, ids, k);
					hdr_reply(f, VC_B, (uint16_t)k, plen);
					m->data_len = m->pkt_len = (uint32_t)(hl + plen);
					tx[nt++] = m;
				} else {
					/* 同一批收到的写请求，B 合并进同一个响应包（VC2 每包 ≤ 16），不额外等待 */
					if (bm && bn + k > bpack) { b_finish(bm, bn, hl); tx[nt++] = bm; bm = NULL; }
					if (!bm) { bm = m; bn = 0; }     /* ID 已取出，本包 payload 可覆盖 */
					else rte_pktmbuf_free(m);
					axi_b_build(rte_pktmbuf_mtod(bm, uint8_t *) + hl + bn * B_RSP, ids, k);
					bn = (uint16_t)(bn + k);
				}
			} else if (vc == VC_AR) {
				int k = axi_ar_parse(p, hi.plen, hi.ntxn, ids, beats, &bad);
				if (bad) { s->err++; dump_bad(s, c->port, "bad AXI read payload", m); }
				struct rte_mbuf *rm = NULL;
				uint8_t *rp = NULL;
				uint32_t beats_in = 0;
				uint16_t cnt = 0;
				for (int j = 0; j < k; j++) {
					uint32_t b = beats[j];
					if (rm && beats_in + b > MAX_BEATS) {  /* 放不下：当前包先收尾 */
						r_finish(rm, rp, f, cnt, hl);
						tx[nt++] = rm; rm = NULL;
					}
					if (!rm) {
						rm = rte_pktmbuf_alloc(c->tmpl_pool);
						if (!rm) { s->err++; break; }
						rp = rte_pktmbuf_mtod(rm, uint8_t *) + hl;
						beats_in = 0; cnt = 0;
					}
					put_be32(rp, axi_w0_r(ids[j], b));  /* rdata 已由模板预填 */
					rp += R_HDR + b * BEAT;
					beats_in += b; cnt++;
				}
				if (rm) { r_finish(rm, rp, f, cnt, hl); tx[nt++] = rm; }
				rte_pktmbuf_free(m);
			} else {
				s->err++; dump_bad(s, c->port, "unexpected frame_type", m); rte_pktmbuf_free(m);
			}

			if (nt >= g_cfg.burst) { flush(c, s, tx, nt); nt = 0; }
		}
		if (bm) { b_finish(bm, bn, hl); tx[nt++] = bm; }
		if (nt) flush(c, s, tx, nt);
		s->busy_cyc += rte_rdtsc() - t_s;
	}
	return 0;
}
