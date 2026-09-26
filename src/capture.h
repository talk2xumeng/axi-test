/*
 * 诊断与抓包：异常帧打印、--dump 十六进制、--pcap 文件
 */
#ifndef AXIPERF_CAPTURE_H
#define AXIPERF_CAPTURE_H

#include "common.h"

extern bool g_pcap_on;

void dump_bad(struct port_stat *st, uint16_t port, const char *why, struct rte_mbuf *m);
void dump_frame(uint64_t *cnt, uint16_t port, const char *dir, struct rte_mbuf *m);
void pcap_open(const char *path);
void pcap_write(struct rte_mbuf *m);
void pcap_close(void);

/* 收发路径旁路点；两者都关闭时只有一次判断 */
static inline void tap(uint64_t *cnt, uint16_t port, const char *dir, struct rte_mbuf *m)
{
	if (likely(!g_cfg.dump && !g_pcap_on)) return;
	if (g_cfg.dump) dump_frame(cnt, port, dir, m);
	if (g_pcap_on) pcap_write(m);
}

#endif
