/*
 * 以太头抽象层
 *
 * 快路径操作（build / parse / reply / update）为 inline，按 g_cfg.hdr 分发，
 * 分支可预测，无函数指针开销。新增头格式：
 *   1. 新建 hdr_xxx.h，实现 xxx_build / xxx_parse / xxx_update
 *   2. 在 enum hdr_kind 中加一项，并在下面各 switch 中加一个 case
 *   3. 在 hdr.c 中补充名称、参数校验、流地址推导
 * 其余模块（AXI、sender、reflector、统计、抓包）不需要改。
 */
#ifndef AXIPERF_HDR_H
#define AXIPERF_HDR_H

#include "common.h"
#include "hdr_eth.h"
#include "hdr_sue.h"

/* hdr_parse 返回值 */
enum { HDR_OK = 0, HDR_IGNORE = 1, HDR_BAD = 2 };

struct hdr_info {
	uint16_t plen;       /* payload 长度（eth：Length；sue：帧长上界） */
	uint16_t ntxn;       /* 事务个数，0 表示头里没有，按 plen 解析 */
	uint8_t  vc;         /* 头里携带的 VC（PCP），仅用于核对 */
};

static inline uint16_t hdr_len(void)
{
	switch (g_cfg.hdr) {
	case HDR_SUE: return SUE_HDR_LEN;
	default:      return ETH_HDR_LEN;
	}
}

/* 写完整帧头 */
static inline void hdr_build(uint8_t *f, const struct rte_ether_addr *dst, const struct rte_ether_addr *src,
                             uint8_t vc, uint16_t ntxn, uint16_t plen)
{
	switch (g_cfg.hdr) {
	case HDR_SUE: sue_build(f, dst, src, vc, ntxn, plen); break;
	default:      eth_build(f, dst, src, vc, ntxn, plen); break;
	}
}

/* 识别并解析帧头：HDR_OK / HDR_IGNORE（非 AXI 帧，静默忽略）/ HDR_BAD（AXI 帧但头非法） */
static inline int hdr_parse(const uint8_t *f, uint32_t pkt_len, struct hdr_info *hi)
{
	int r;
	switch (g_cfg.hdr) {
	case HDR_SUE: r = sue_parse(f, pkt_len, &hi->plen, &hi->ntxn); break;
	default:      r = eth_parse(f, pkt_len, &hi->plen, &hi->ntxn); break;
	}
	hi->vc = f[14] >> 5;                 /* 两种格式 PCP 位置相同 */
	return r;
}

/* 原地把请求帧头改为响应帧头：交换源 / 目的地址，改 VC、事务个数、长度 */
static inline void hdr_reply(uint8_t *f, uint8_t vc, uint16_t ntxn, uint16_t plen)
{
	uint8_t t[6];
	memcpy(t, f, 6); memcpy(f, f + 6, 6); memcpy(f + 6, t, 6);
	switch (g_cfg.hdr) {
	case HDR_SUE: sue_update(f, vc, ntxn, plen); break;
	default:      eth_update(f, vc, ntxn, plen); break;
	}
}

/* 为请求帧 req 生成的新响应帧写头（读响应用） */
static inline void hdr_reply_to(uint8_t *f, const uint8_t *req, uint8_t vc, uint16_t ntxn, uint16_t plen)
{
	hdr_build(f, (const struct rte_ether_addr *)(req + 6), (const struct rte_ether_addr *)req, vc, ntxn, plen);
}

/* ---------------- 非快路径（hdr.c） ---------------- */
const char *hdr_name(enum hdr_kind k);
int  hdr_from_name(const char *s, enum hdr_kind *k);
int  hdr_validate(void);                                   /* 参数检查，失败返回 -1 并打印原因 */
void hdr_flow_addr(uint16_t port, uint16_t flow, uint16_t idx,
                   const struct rte_ether_addr *port_mac, struct flow_addr *a);
void hdr_addr_str(const struct rte_ether_addr *m, char *buf, size_t n);

#endif
