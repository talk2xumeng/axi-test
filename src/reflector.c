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

static inline void flush(struct flow_ctx *c, struct port_stat *s, struct rte_mbuf **tx, uint16_t nt)
{
	for (uint16_t j = 0; j < nt; j++) {
		s->tx_wire_bytes += frame_wire(tx[j]->pkt_len);
		tap(&s->dump_tx, c->port, "TX", tx[j]);
	}
	s->tx_pkts += nt;
	tx_all(c->port, c->q, tx, nt);
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

	while (!g_quit) {
		uint64_t t_s = rte_rdtsc();
		uint16_t nr = rte_eth_rx_burst(c->port, c->q, rx, MAX_BURST), nt = 0;
		if (nr == 0) continue;
		s->rx_hits++;

		for (uint16_t i = 0; i < nr; i++) {
			struct rte_mbuf *m = rx[i];
			uint8_t *f = rte_pktmbuf_mtod(m, uint8_t *);
			struct hdr_info hi;
			s->rx_pkts++; s->rx_wire_bytes += frame_wire(m->pkt_len);

			int r = hdr_parse(f, m->pkt_len, &hi);
			if (r == HDR_IGNORE) { s->ign++; rte_pktmbuf_free(m); continue; }
			if (r == HDR_BAD) { s->err++; dump_bad(s, c->port, "bad header", m); rte_pktmbuf_free(m); continue; }

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
				uint16_t plen = axi_b_build(p, ids, k);
				hdr_reply(f, VC_B, (uint16_t)k, plen);
				m->data_len = m->pkt_len = (uint32_t)(hl + plen);
				tx[nt++] = m;
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
		if (nt) flush(c, s, tx, nt);
		s->busy_cyc += rte_rdtsc() - t_s;
	}
	return 0;
}
