/*
 * AXI 事务层：事务头编码、请求 payload 构造、响应 / 请求解析
 * 与以太头格式无关，只处理以太头之后的 payload
 */
#ifndef AXIPERF_AXI_H
#define AXIPERF_AXI_H

#include "common.h"

#define AW_HDR      16
#define AR_HDR      16
#define WSTRB_LEN   8
#define B_RSP       12
#define R_HDR       4
#define BEAT        64
#define MAX_BEATS   8           /* 每包数据 ≤ 8 拍 */
#define MAX_TXN     16          /* 每包事务数上限（VC2 ≤ 16） */

enum { VC_AW = 0, VC_AR = 1, VC_B = 2, VC_R = 3, VC_NONE = 0xFF };
enum { FT_WRFULL = 0x0, FT_WR = 0x1, FT_B = 0x2, FT_AR = 0x3, FT_R = 0x4 };

/* ---------------- 事务头 W0 ---------------- */
static inline uint32_t axi_w0_aw(uint32_t id, uint32_t beats)   /* writefull，writeFull=1111 */
{ return ((uint32_t)FT_WRFULL << 28) | (id << 19) | ((beats - 1) << 17) | (0xFu << 13); }
static inline uint32_t axi_w0_ar(uint32_t id)
{ return ((uint32_t)FT_AR << 28) | (id << 19); }
static inline uint32_t axi_w0_b(uint32_t id)                    /* BRESP=00 */
{ return ((uint32_t)FT_B << 28) | (id << 19); }
static inline uint32_t axi_w0_r(uint32_t id, uint32_t beats)    /* rlast=1，RRESP=00 */
{ return ((uint32_t)FT_R << 28) | (id << 19) | ((beats - 1) << 17) | (1u << 3); }

static inline uint32_t axi_id(uint32_t w0) { return (w0 >> 19) & 0x1FF; }
static inline uint32_t axi_ft(uint32_t w0) { return w0 >> 28; }
static inline uint32_t axi_beats(uint32_t w0) { return ((w0 >> 17) & 3) + 1; }   /* awlen / rlen */

/* 请求 frame_type 对应的 VC；非请求返回 VC_NONE */
static inline uint8_t axi_req_vc(uint32_t ft)
{ return (ft == FT_WRFULL || ft == FT_WR) ? VC_AW : (ft == FT_AR ? VC_AR : VC_NONE); }

/* ---------------- 请求 payload 构造（模板，ID 发送时再填） ---------------- */
static inline uint16_t axi_req_txn_len(bool read, int beats)
{ return read ? AR_HDR : (uint16_t)(AW_HDR + beats * BEAT); }

/* 在 p 处写 pack 个请求事务，返回 payload 长度 */
static inline uint16_t axi_req_build(uint8_t *p, bool read, int pack, int beats)
{
	uint8_t *s = p;
	for (int k = 0; k < pack; k++) {
		if (!read) {                                         /* writefull + beats × 64B */
			memset(p, 0, AW_HDR);
			put_be32(p, axi_w0_aw(0, beats));
			put_be32(p + 12, 6);                             /* awsize = 110 */
			memset(p + AW_HDR, 0xA5, beats * BEAT);
			p += AW_HDR + beats * BEAT;
		} else {                                             /* read */
			memset(p, 0, AR_HDR);
			put_be32(p, axi_w0_ar(0));
			put_be32(p + 12, ((uint32_t)(beats - 1) << 3) | 6); /* arlen，arsize = 110 */
			p += AR_HDR;
		}
	}
	return (uint16_t)(p - s);
}

/*
 * user 域（E100c 要求软件填）：
 *   AW：awuser[51:49] = port，awuser[48:33] = tsn
 *   AR：aruser[48:46] = port，aruser[45:30] = tsn
 * 两者都是 {port[2:0], tsn[15:0]} 连续 19 bit，awuser 从 bit 33 起、aruser 从 bit 30 起。
 * 线上位置：awuser[56:44] = W0[12:0]，awuser[43:12] = W1；aruser[60:42] = W0[18:0]，aruser[41:10] = W1。
 * 所以 AW：port → W0[7:5]，tsn[15:11] → W0[4:0]，tsn[10:0] → W1[31:21]；
 *      AR：port → W0[6:4]，tsn[15:12] → W0[3:0]，tsn[11:0] → W1[31:20]。
 * B / R 响应不带 tsn / port。
 */
#define AW_USER_LSB 33
#define AR_USER_LSB 30

static inline uint64_t axi_user_tp(uint32_t port, uint32_t tsn)
{ return ((uint64_t)(port & 7) << 16) | (tsn & 0xFFFF); }

/* 发送时填 ID 与 user.tsn / user.port：改每个事务的 W0、W1（W1 其余位为 awuser / aruser 低位，模板为 0） */
static inline void axi_req_set(uint8_t *txn, bool read, uint32_t id, int beats, uint32_t port, uint32_t tsn)
{
	uint32_t w0, w1;
	if (!read) {
		const uint64_t u = axi_user_tp(port, tsn) << AW_USER_LSB;   /* awuser[56:0] */
		w0 = axi_w0_aw(id, (uint32_t)beats) | (uint32_t)(u >> 44);
		w1 = (uint32_t)(u >> 12);
	} else {
		const uint64_t u = axi_user_tp(port, tsn) << AR_USER_LSB;   /* aruser[60:0] */
		w0 = axi_w0_ar(id) | (uint32_t)(u >> 42);
		w1 = (uint32_t)(u >> 10);
	}
	put_be32(txn, w0);
	put_be32(txn + 4, w1);
}

/* 从请求事务头取 user.tsn / user.port（reflector 校验、测试用） */
static inline void axi_req_user(const uint8_t *txn, bool read, uint32_t *port, uint32_t *tsn)
{
	const uint64_t w0 = get_be32(txn), w1 = get_be32(txn + 4);
	const uint64_t v = read ? ((((w0 & 0x7FFFF) << 32) | w1) << 10) >> AR_USER_LSB
	                        : ((((w0 & 0x1FFF) << 32) | w1) << 12) >> AW_USER_LSB;
	*tsn = (uint32_t)(v & 0xFFFF);
	*port = (uint32_t)((v >> 16) & 7);
}

/*
 * 解析响应 payload（sender 用）。ntxn=0 表示事务个数未知，按 plen 解析到末尾。
 * 输出 ids / data 字节数，返回事务个数；*bad 置位表示遇到非法事务。
 */
static inline int axi_rsp_parse(const uint8_t *p, uint16_t plen, uint16_t ntxn, uint8_t vc,
                                uint32_t *ids, uint32_t *data, bool *bad)
{
	int k = 0; uint16_t off = 0;
	*bad = false;
	while (off < plen && k < MAX_TXN && (ntxn == 0 || k < ntxn)) {
		if (off + 4 > plen) { *bad = true; break; }
		uint32_t w0 = get_be32(p + off), ft = axi_ft(w0), step;
		if (vc == VC_B && ft == FT_B) { step = B_RSP; data[k] = 0; }
		else if (vc == VC_R && ft == FT_R) { data[k] = axi_beats(w0) * BEAT; step = R_HDR + data[k]; }
		else { *bad = true; break; }
		if (off + step > plen) { *bad = true; break; }
		ids[k++] = axi_id(w0);
		off += (uint16_t)step;
	}
	if (ntxn && k != ntxn) *bad = true;
	return k;
}

/* 解析写请求 payload（reflector 用），返回事务个数，输出 ids */
static inline int axi_aw_parse(const uint8_t *p, uint16_t plen, uint16_t ntxn, uint32_t *ids, bool *bad)
{
	int k = 0; uint16_t off = 0;
	*bad = false;
	while (off < plen && k < MAX_TXN && (ntxn == 0 || k < ntxn)) {
		if (off + AW_HDR > plen) { *bad = true; break; }
		uint32_t w0 = get_be32(p + off), ft = axi_ft(w0), b = axi_beats(w0);
		if (ft == FT_WRFULL) off += (uint16_t)(AW_HDR + b * BEAT);
		else if (ft == FT_WR) off += (uint16_t)(AW_HDR + b * (WSTRB_LEN + BEAT));
		else { *bad = true; break; }
		ids[k++] = axi_id(w0);
	}
	if (ntxn && k != ntxn) *bad = true;
	return k;
}

/* 在 p 处写 k 个 wrRsp（原地覆盖写请求），返回 payload 长度 */
static inline uint16_t axi_b_build(uint8_t *p, const uint32_t *ids, int k)
{
	for (int j = 0; j < k; j++, p += B_RSP) {
		put_be32(p, axi_w0_b(ids[j]));
		put_be32(p + 4, 0);
		put_be32(p + 8, 0);
	}
	return (uint16_t)(k * B_RSP);
}

/* 解析读请求 payload（reflector 用），输出 ids 与各自拍数 */
static inline int axi_ar_parse(const uint8_t *p, uint16_t plen, uint16_t ntxn, uint32_t *ids, uint32_t *beats, bool *bad)
{
	int k = 0; uint16_t off = 0;
	*bad = false;
	while (off + AR_HDR <= plen && k < MAX_TXN && (ntxn == 0 || k < ntxn)) {
		uint32_t w0 = get_be32(p + off), w3 = get_be32(p + off + 12);
		if (axi_ft(w0) != FT_AR) { *bad = true; break; }
		ids[k] = axi_id(w0);
		beats[k] = ((w3 >> 3) & 3) + 1;                      /* arlen */
		k++; off += AR_HDR;
	}
	if (ntxn && k != ntxn) *bad = true;
	return k;
}

#endif
