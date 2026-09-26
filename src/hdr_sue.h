/*
 * 以太头格式 sue：SUEv1-标准ETH（试验性，字段取值待与 IP 侧确认）
 *
 *   字节  0-1  Dest GPU ID[15:0]     2-5  Reserved          （DMAC 位置）
 *         6-7  Source GPU ID[15:0]   8-11 Reserved          （SMAC 位置）
 *        12-13 TPID 0x8100          14-15 OPCP(3) OCFI(1) OVID(12)
 *        16-17 EtherType            18-19 RSV(2) PackNum(6) Format(3) PktType(5)
 *
 *   OPCP = VC；OCFI = 0；OVID = --vid
 *   PackNum = 本包事务个数（1~16）；接收端按 PackNum 解析，补齐字节被忽略
 *   EtherType / Format / PktType 由参数给出，默认 0x88B5 / 0 / 0
 */
#ifndef AXIPERF_HDR_SUE_H
#define AXIPERF_HDR_SUE_H

#include "common.h"

#define SUE_HDR_LEN 20

/* GPU ID 放在 MAC 字段的前 2 字节，其余为保留字段填 0 */
static inline void sue_id_to_mac(uint16_t id, struct rte_ether_addr *m)
{
	memset(m, 0, sizeof(*m));
	m->addr_bytes[0] = (uint8_t)(id >> 8);
	m->addr_bytes[1] = (uint8_t)id;
}

static inline uint16_t sue_ext(uint16_t ntxn)
{
	return (uint16_t)(((ntxn & 0x3F) << 8) | ((g_cfg.sue_format & 0x7) << 5) | (g_cfg.sue_pkttype & 0x1F));
}

static inline void sue_build(uint8_t *f, const struct rte_ether_addr *dst, const struct rte_ether_addr *src,
                             uint8_t vc, uint16_t ntxn, uint16_t plen)
{
	(void)plen;
	memcpy(f, dst, 6);
	memcpy(f + 6, src, 6);
	f[12] = 0x81; f[13] = 0x00;
	put_be16(f + 14, (uint16_t)((vc << 13) | (g_cfg.vid & 0xFFF)));
	put_be16(f + 16, g_cfg.sue_ethertype);
	put_be16(f + 18, sue_ext(ntxn));
}

static inline int sue_parse(const uint8_t *f, uint32_t pkt_len, uint16_t *plen, uint16_t *ntxn)
{
	if (pkt_len < SUE_HDR_LEN + 4 || f[12] != 0x81 || f[13] != 0x00) return 1;
	if (get_be16(f + 16) != g_cfg.sue_ethertype) return 1;
	uint16_t pn = (get_be16(f + 18) >> 8) & 0x3F;
	if (pn == 0) return 2;                    /* PackNum 为 0 非法 */
	*plen = (uint16_t)(pkt_len - SUE_HDR_LEN);/* 上界；实际按 PackNum 解析 */
	*ntxn = pn;
	return 0;
}

static inline void sue_update(uint8_t *f, uint8_t vc, uint16_t ntxn, uint16_t plen)
{
	(void)plen;
	put_be16(f + 14, (uint16_t)((vc << 13) | (g_cfg.vid & 0xFFF)));
	put_be16(f + 18, sue_ext(ntxn));
}

#endif
