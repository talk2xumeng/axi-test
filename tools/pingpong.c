/*
 * 最小 DPDK 乒乓时延探针：单队列、不建 rte_flow、混杂模式，与 axiperf 无关的代码路径。
 * 两端在同一台主机（共用不变 TSC）时，帧内带 TSC，可分出去程、回程与对端停留：
 *   t0 ping 发前 → t1 pong 收到 → t2 pong 发前 → t3 ping 收到
 *   去程 = t1 − t0，回程 = t3 − t2，停留 = t2 − t1，RTT = t3 − t0
 *
 *   pong: ./pingpong -l 34 -a 0000:db:00.0,<devargs> --file-prefix pong -- --pong
 *   ping: ./pingpong -l 2  -a 0000:5c:00.0,<devargs> --file-prefix ping -- --size 64 --n 200000 --dmac <pong MAC>
 * 选项：--size 帧长（不含 FCS，≥ 64）  --n 次数  --warm 预热次数  --rxd / --txd 描述符数
 *       --busy-tx  发前先把 mbuf 写好，t0 紧贴 tx_burst（默认即如此）
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include <signal.h>
#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_mbuf.h>
#include <rte_cycles.h>
#include <rte_ether.h>

#define ETYPE 0x88B6
#define MAGIC 0x50494E47u

struct pp {                             /* 帧内载荷，紧跟以太头 */
	uint32_t magic, seq;
	uint64_t t0, t1, t2;
} __attribute__((packed));

static volatile int quit;
static void on_sig(int s) { (void)s; quit = 1; }

static int cmp_u64(const void *a, const void *b)
{
	uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
	return x < y ? -1 : x > y;
}

static void report(const char *name, uint64_t *v, uint32_t n, double hz)
{
	qsort(v, n, sizeof(*v), cmp_u64);
	const double k = 1e6 / hz;
	printf("%-6s min %6.3f  p50 %6.3f  p99 %6.3f  p999 %6.3f  max %8.3f µs\n", name,
	       v[0] * k, v[n / 2] * k, v[(uint64_t)n * 99 / 100] * k, v[(uint64_t)n * 999 / 1000] * k, v[n - 1] * k);
}

int main(int argc, char **argv)
{
	int r = rte_eal_init(argc, argv);
	if (r < 0) rte_exit(EXIT_FAILURE, "EAL init failed\n");
	argc -= r; argv += r;

	int pong = 0;
	uint32_t size = 64, n = 200000, warm = 20000;
	uint16_t rxd = 1024, txd = 1024;
	struct rte_ether_addr dmac = { .addr_bytes = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff } };
	static const struct option lo[] = { {"pong", 0, 0, 'p'}, {"size", 1, 0, 's'}, {"n", 1, 0, 'n'}, {"warm", 1, 0, 'w'},
		{"dmac", 1, 0, 'd'}, {"rxd", 1, 0, 'r'}, {"txd", 1, 0, 't'}, {0, 0, 0, 0} };
	int c;
	while ((c = getopt_long(argc, argv, "", lo, NULL)) != -1) {
		switch (c) {
		case 'p': pong = 1; break;
		case 's': size = (uint32_t)atoi(optarg); break;
		case 'n': n = (uint32_t)atoi(optarg); break;
		case 'w': warm = (uint32_t)atoi(optarg); break;
		case 'd': if (rte_ether_unformat_addr(optarg, &dmac)) rte_exit(EXIT_FAILURE, "bad --dmac\n"); break;
		case 'r': rxd = (uint16_t)atoi(optarg); break;
		case 't': txd = (uint16_t)atoi(optarg); break;
		default: rte_exit(EXIT_FAILURE, "unknown option\n");
		}
	}
	if (size < 64 || size > 1500) rte_exit(EXIT_FAILURE, "--size 64~1500\n");
	signal(SIGINT, on_sig);

	const uint16_t pi = 0;
	if (!rte_eth_dev_is_valid_port(pi)) rte_exit(EXIT_FAILURE, "no port\n");
	const int sock = rte_eth_dev_socket_id(pi);
	struct rte_mempool *mp = rte_pktmbuf_pool_create("pp", 8191, 256, 0, RTE_MBUF_DEFAULT_BUF_SIZE, sock < 0 ? 0 : sock);
	if (!mp) rte_exit(EXIT_FAILURE, "mempool\n");
	struct rte_eth_conf conf;
	memset(&conf, 0, sizeof(conf));
	if (rte_eth_dev_configure(pi, 1, 1, &conf) < 0 ||
	    rte_eth_dev_adjust_nb_rx_tx_desc(pi, &rxd, &txd) < 0 ||
	    rte_eth_rx_queue_setup(pi, 0, rxd, sock, NULL, mp) < 0 ||
	    rte_eth_tx_queue_setup(pi, 0, txd, sock, NULL) < 0 ||
	    rte_eth_dev_start(pi) < 0)
		rte_exit(EXIT_FAILURE, "port setup\n");
	rte_eth_promiscuous_enable(pi);
	struct rte_ether_addr smac;
	rte_eth_macaddr_get(pi, &smac);
	struct rte_eth_burst_mode bm;
	if (!rte_eth_rx_burst_mode_get(pi, 0, &bm)) printf("rx burst mode \"%s\"", bm.info);
	if (!rte_eth_tx_burst_mode_get(pi, 0, &bm)) printf(", tx burst mode \"%s\"", bm.info);
	printf("\n");
	const double hz = (double)rte_get_tsc_hz();
	struct rte_mbuf *rx[32];

	if (pong) {
		printf("pong ready (port MAC " RTE_ETHER_ADDR_PRT_FMT ")\n", RTE_ETHER_ADDR_BYTES(&smac));
		uint64_t cnt = 0;
		while (!quit) {
			const uint16_t k = rte_eth_rx_burst(pi, 0, rx, 32);
			if (!k) continue;
			const uint64_t t1 = rte_rdtsc();
			uint16_t m = 0;
			for (uint16_t i = 0; i < k; i++) {
				struct rte_ether_hdr *e = rte_pktmbuf_mtod(rx[i], struct rte_ether_hdr *);
				struct pp *p = (struct pp *)(e + 1);
				if (e->ether_type != rte_cpu_to_be_16(ETYPE) || p->magic != MAGIC) { rte_pktmbuf_free(rx[i]); continue; }
				struct rte_ether_addr t = e->dst_addr;
				e->dst_addr = e->src_addr;
				e->src_addr = t;
				p->t1 = t1;
				rx[m++] = rx[i];
			}
			const uint64_t t2 = rte_rdtsc();
			for (uint16_t i = 0; i < m; i++) rte_pktmbuf_mtod_offset(rx[i], struct pp *, sizeof(struct rte_ether_hdr))->t2 = t2;
			uint16_t s = 0;
			while (s < m) s += rte_eth_tx_burst(pi, 0, rx + s, (uint16_t)(m - s));
			cnt += m;
		}
		printf("pong: %lu frames\n", (unsigned long)cnt);
	} else {
		uint64_t *rtt = malloc(n * sizeof(uint64_t)), *go = malloc(n * sizeof(uint64_t)),
		         *back = malloc(n * sizeof(uint64_t)), *dwell = malloc(n * sizeof(uint64_t)),
		         *txc = malloc(n * sizeof(uint64_t));
		if (!rtt || !go || !back || !dwell || !txc) rte_exit(EXIT_FAILURE, "malloc\n");
		uint32_t got = 0, timeouts = 0;
		for (uint32_t it = 0; it < warm + n && !quit; it++) {
			struct rte_mbuf *m = rte_pktmbuf_alloc(mp);
			if (!m) rte_exit(EXIT_FAILURE, "mbuf\n");
			char *d = rte_pktmbuf_append(m, (uint16_t)size);
			memset(d, 0, size);
			struct rte_ether_hdr *e = (struct rte_ether_hdr *)d;
			e->dst_addr = dmac;
			e->src_addr = smac;
			e->ether_type = rte_cpu_to_be_16(ETYPE);
			struct pp *p = (struct pp *)(e + 1);
			p->magic = MAGIC;
			p->seq = it;
			const uint64_t t0 = rte_rdtsc();
			p->t0 = t0;
			while (rte_eth_tx_burst(pi, 0, &m, 1) == 0) ;
			const uint64_t ta = rte_rdtsc();
			int done = 0;
			while (!done && !quit) {
				const uint16_t k = rte_eth_rx_burst(pi, 0, rx, 32);
				const uint64_t t3 = rte_rdtsc();
				for (uint16_t i = 0; i < k; i++) {
					struct rte_ether_hdr *re = rte_pktmbuf_mtod(rx[i], struct rte_ether_hdr *);
					struct pp *q = (struct pp *)(re + 1);
					if (re->ether_type == rte_cpu_to_be_16(ETYPE) && q->magic == MAGIC && q->seq == it) {
						if (it >= warm) {
							rtt[got] = t3 - q->t0; go[got] = q->t1 - q->t0; back[got] = t3 - q->t2;
							dwell[got] = q->t2 - q->t1; txc[got] = ta - t0; got++;
						}
						done = 1;
					}
					rte_pktmbuf_free(rx[i]);
				}
				if (!done && t3 - t0 > (uint64_t)hz / 100) { timeouts++; break; }   /* 10 ms */
			}
		}
		printf("ping: size %u B, %u samples, %u timeouts\n", size, got, timeouts);
		if (got) {
			report("RTT", rtt, got, hz);
			report("去程", go, got, hz);
			report("回程", back, got, hz);
			report("停留", dwell, got, hz);
			report("tx调用", txc, got, hz);
		}
	}
	rte_eth_dev_stop(pi);
	rte_eth_dev_close(pi);
	rte_eal_cleanup();
	return 0;
}
