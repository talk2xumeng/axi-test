/*
 * 以太头抽象层：非快路径部分（名称、参数校验、流地址推导、显示）
 */
#include <stdio.h>
#include "hdr.h"

static const char *const k_names[] = { [HDR_ETH] = "eth", [HDR_SUE] = "sue" };

const char *hdr_name(enum hdr_kind k)
{
	return (unsigned)k < RTE_DIM(k_names) && k_names[k] ? k_names[k] : "?";
}

int hdr_from_name(const char *s, enum hdr_kind *k)
{
	for (unsigned i = 0; i < RTE_DIM(k_names); i++)
		if (k_names[i] && strcmp(s, k_names[i]) == 0) { *k = (enum hdr_kind)i; return 0; }
	return -1;
}

int hdr_validate(void)
{
	if (g_cfg.hdr != HDR_SUE) return 0;

	if (g_cfg.gpu_id < 0) { printf("sue 头需要 --gpu-id\n"); return -1; }
	if (g_cfg.sender && g_cfg.peer_gpu_id < 0) { printf("sue 头的 sender 需要 --peer-gpu-id\n"); return -1; }
	if (g_cfg.sue_ethertype <= 1500) { printf("--sue-ethertype 须 > 1500（否则会被当作 Length）\n"); return -1; }
	if (g_cfg.pack > 63) { printf("sue 头 PackNum 为 6 bit，--pack 须 ≤ 63\n"); return -1; }

	/* GPU ID 高字节占 MAC 首字节，最低位是组播位，置 1 会被网卡 / 交换机当作组播 */
	int ids[2] = { g_cfg.gpu_id, g_cfg.peer_gpu_id };
	for (int i = 0; i < 2; i++) {
		if (ids[i] < 0) continue;
		for (int f = 0; f < MAX_FLOWS; f++)
			if (((ids[i] + f) >> 8) & 1) {
				printf("警告：GPU ID 0x%04x 的高字节最低位为 1，线上会被当作组播 MAC\n", ids[i] + f);
				break;
			}
	}
	return 0;
}

/* 流 f 的 eth MAC：f=0 用网卡真实 MAC；f>0 首字节置本地管理位、清组播位，末字节 +f。两端按同一规则推导 */
static void eth_flow_mac(const struct rte_ether_addr *base, uint16_t f, struct rte_ether_addr *out)
{
	*out = *base;
	if (f == 0) return;
	out->addr_bytes[0] = (uint8_t)((base->addr_bytes[0] | 0x02) & 0xFE);
	out->addr_bytes[5] = (uint8_t)(base->addr_bytes[5] + f);
}

void hdr_flow_addr(uint16_t port, uint16_t flow, uint16_t idx,
                   const struct rte_ether_addr *port_mac, struct flow_addr *a)
{
	switch (g_cfg.hdr) {
	case HDR_SUE:                    /* 全局第 idx 条流：本端 gpu_id + idx，对端 peer_gpu_id + idx */
		sue_id_to_mac((uint16_t)(g_cfg.gpu_id + idx), &a->src);
		sue_id_to_mac((uint16_t)((g_cfg.peer_gpu_id < 0 ? 0 : g_cfg.peer_gpu_id) + idx), &a->dst);
		break;
	default:
		eth_flow_mac(port_mac, flow, &a->src);
		eth_flow_mac(&g_cfg.peer_mac[port], flow, &a->dst);
		break;
	}
}

void hdr_addr_str(const struct rte_ether_addr *m, char *buf, size_t n)
{
	if (g_cfg.hdr == HDR_SUE)
		snprintf(buf, n, "GPU 0x%04x", (m->addr_bytes[0] << 8) | m->addr_bytes[1]);
	else
		rte_ether_format_addr(buf, (uint16_t)n, m);
}
