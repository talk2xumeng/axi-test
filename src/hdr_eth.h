/*
 * 以太头格式 eth：802.1Q + 802.3 Length（raw 802.3，无 LLC）
 *
 *   0      6      12       14            16        18
 *   | DMAC | SMAC | 0x8100 | PCP/DEI/VID | Length | AXI 事务 × k
 *
 *   PCP = VC；DEI = 0；VID = --vid；Length = 之后的有效字节数（≤1500）
 *   事务个数不在头里，接收端按 Length 解析到末尾，补齐字节被忽略
 *
 *   --no-len：没有 Length，事务头从第 16 字节开始；接收端按帧长解析，遇到全 0 的 W0 视为补齐
 *   --ethertype X（E100c 格式）：同上，且第 12~15 字节为 EtherType + 0000，没有 802.1Q 标签
 */
#ifndef AXIPERF_HDR_ETH_H
#define AXIPERF_HDR_ETH_H

#include "common.h"

#define ETH_HDR_LEN 18
#define ETH_HDR_NOLEN 16

#define VC_B_TAG 2                       /* 写响应的 VC，与 axi.h 的 VC_B 相同（本文件不包含 axi.h） */

static inline void eth_build(uint8_t *f, const struct rte_ether_addr *dst, const struct rte_ether_addr *src,
                             uint8_t vc, uint16_t ntxn, uint16_t plen)
{
	(void)ntxn;
	memcpy(f, dst, 6);
	memcpy(f + 6, src, 6);
	if (g_cfg.ethertype) { put_be16(f + 12, g_cfg.ethertype); put_be16(f + 14, 0); return; }
	f[12] = 0x81; f[13] = 0x00;
	put_be16(f + 14, (uint16_t)((vc << 13) | (g_cfg.vid & 0xFFF)));
	if (!g_cfg.no_len) put_be16(f + 16, plen);
}

/* 返回 hdr_parse 的结果码（见 hdr.h） */
static inline int eth_parse(const uint8_t *f, uint32_t pkt_len, uint16_t *plen, uint16_t *ntxn)
{
	if (g_cfg.no_len) {
		if (pkt_len < ETH_HDR_NOLEN + 4 || get_be16(f + 12) != (g_cfg.ethertype ? g_cfg.ethertype : 0x8100)) return 1;
		if ((f[16] >> 4) > 4) return 1;      /* frame_type 不是 AXI（如启动时的 EtherType 0x9000 广播帧） */
		*plen = (uint16_t)(pkt_len - ETH_HDR_NOLEN); *ntxn = 0;
		return 0;
	}
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
	if (g_cfg.ethertype) { if (vc == VC_B_TAG) put_be16(f + 14, g_cfg.b_tag); return; }   /* --b-tag：写响应 EtherType 后的 16 位 */
	put_be16(f + 14, (uint16_t)((vc << 13) | (g_cfg.vid & 0xFFF)));
	if (!g_cfg.no_len) put_be16(f + 16, plen);
}

#endif
