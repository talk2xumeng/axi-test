/*
 * 只链进 axiperf-static（Makefile 用 -Wl,--wrap=dlopen）。
 *
 * 全静态链接时 glibc 的 dlopen 不可用：rte_eal_init → eal_plugins_init 调 dlopen
 * 判断是否共享库构建时直接段错误（Ubuntu 20.04 glibc 2.31 实测）。静态版本的
 * DPDK 驱动和 rdma-core mlx5 provider 都已链进程序，不需要加载任何 .so，
 * 所以让 dlopen 总是返回“找不到”，调用方按没有插件处理。
 */
#include <stddef.h>

void *__wrap_dlopen(const char *file, int mode);

void *__wrap_dlopen(const char *file, int mode)
{
	(void)file;
	(void)mode;
	return NULL;
}
