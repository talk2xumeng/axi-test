/*
 * 诊断与抓包
 */
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include <rte_hexdump.h>
#include <rte_spinlock.h>
#include <rte_debug.h>

#include "capture.h"

bool g_pcap_on;
static FILE *g_pcap;
static rte_spinlock_t g_pcap_lock = RTE_SPINLOCK_INITIALIZER;
static uint64_t g_pcap_written;

/* 前 5 个异常帧打印原因与前 64 字节 */
void dump_bad(struct port_stat *st, uint16_t port, const char *why, struct rte_mbuf *m)
{
	if (st->dumped >= 5) return;
	st->dumped++;
	printf("[port %u] bad frame: %s  pkt_len=%u  ol_flags=0x%lx%s  vlan_tci=0x%04x\n", port, why, m->pkt_len,
	       (unsigned long)m->ol_flags, (m->ol_flags & RTE_MBUF_F_RX_VLAN_STRIPPED) ? " (VLAN STRIPPED by NIC)" : "",
	       m->vlan_tci);
	rte_hexdump(stdout, "first 64B", rte_pktmbuf_mtod(m, void *), m->data_len < 64 ? m->data_len : 64);
	fflush(stdout);
}

void dump_frame(uint64_t *cnt, uint16_t port, const char *dir, struct rte_mbuf *m)
{
	if (*cnt >= (uint64_t)g_cfg.dump) return;
	(*cnt)++;
	char title[64];
	snprintf(title, sizeof(title), "[port %u] %s #%lu len=%u", port, dir, (unsigned long)*cnt, m->pkt_len);
	rte_hexdump(stdout, title, rte_pktmbuf_mtod(m, void *), m->data_len);
	fflush(stdout);
}

void pcap_open(const char *path)
{
	g_pcap = fopen(path, "wb");
	if (!g_pcap) rte_exit(EXIT_FAILURE, "无法创建 %s\n", path);
	struct { uint32_t magic; uint16_t vmaj, vmin; int32_t tz; uint32_t sig, snap, link; } gh =
		{ 0xa1b23c4d, 2, 4, 0, 0, 65535, 1 };            /* 纳秒 pcap，以太网 */
	fwrite(&gh, sizeof(gh), 1, g_pcap);
	g_pcap_on = true;
}

void pcap_write(struct rte_mbuf *m)
{
	if (!g_pcap || __atomic_load_n(&g_cfg.pcap_left, __ATOMIC_RELAXED) == 0) return;
	struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
	struct { uint32_t sec, nsec, caplen, len; } rh =
		{ (uint32_t)ts.tv_sec, (uint32_t)ts.tv_nsec, m->data_len, m->pkt_len };
	rte_spinlock_lock(&g_pcap_lock);
	if (g_cfg.pcap_left) {
		fwrite(&rh, sizeof(rh), 1, g_pcap);
		fwrite(rte_pktmbuf_mtod(m, void *), m->data_len, 1, g_pcap);
		g_cfg.pcap_left--; g_pcap_written++;
	}
	rte_spinlock_unlock(&g_pcap_lock);
}

void pcap_close(void)
{
	if (!g_pcap) return;
	g_pcap_on = false;
	fclose(g_pcap);
	g_pcap = NULL;
	printf("pcap: 写入 %lu 帧到 %s\n", (unsigned long)g_pcap_written, g_cfg.pcap_path);
}
