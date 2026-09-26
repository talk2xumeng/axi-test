/*
 * 以太头格式 eth：802.1Q + 802.3 Length（raw 802.3，无 LLC）
 *
 *   0      6      12       14            16        18
 *   | DMAC | SMAC | 0x8100 | PCP/DEI/VID | Length | AXI 事务 × k
 *
 *   PCP = VC；DEI = 0；VID = --vid；Length = 之后的有效字节数（≤1500）
 *   事务个数不在头里，接收端按 Length 解析到末尾，补齐字节被忽略
 */
#ifndef AXIPERF_HDR_ETH_H
#define AXIPERF_HDR_ETH_H

#include "common.h"

#define ETH_HDR_LEN 18

static inline void eth_build(uint8_t *f, const struct rte_ether_addr *dst, const struct rte_ether_addr *src,
                             uint8_t vc, uint16_t ntxn, uint16_t plen)
{
	(void)ntxn;
	memcpy(f, dst, 6);
	memcpy(f + 6, src, 6);
	f[12] = 0x81; f[13] = 0x00;
	put_be16(f + 14, (uint16_t)((vc << 13) | (g_cfg.vid & 0xFFF)));
	put_be16(f + 16, plen);
}

/* 返回 hdr_parse 的结果码（见 hdr.h） */
static inline int eth_parse(const uint8_t *f, uint32_t pkt_len, uint16_t *plen, uint16_t *ntxn)
{
	if (pkt_len < ETH_HDR_LEN + 4 || f[12] != 0x81 || f[13] != 0x00) return 1;
	uint16_t l = get_be16(f + 16);
	if (l > 1500) return 1;                  /* 是 EtherType（VLAN 内的 IPv4/DHCP 等），不是 AXI 帧 */
	if (l < 4 || (uint32_t)(ETH_HDR_LEN + l) > pkt_len) return 2;
	*plen = l; *ntxn = 0;
	return 0;
}

/* 原地改写：VC、事务个数、payload 长度（MAC 交换由 hdr 层统一做） */
static inline void eth_update(uint8_t *f, uint8_t vc, uint16_t ntxn, uint16_t plen)
{
	(void)ntxn;
	put_be16(f + 14, (uint16_t)((vc << 13) | (g_cfg.vid & 0xFFF)));
	put_be16(f + 16, plen);
}

#endif
