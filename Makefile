APP     := axiperf
STATIC_APP := axiperf-static
DPDK_ROOT ?= /opt/dpdk-24.11
RDMA_ROOT ?= /opt/rdma-core-50
DPDK_VERSION := 24.11.4
RDMA_VERSION := 50.0
TOOLCHAIN_PKG_CONFIG_PATH := $(DPDK_ROOT)/lib/x86_64-linux-gnu/pkgconfig:$(DPDK_ROOT)/lib/pkgconfig:$(RDMA_ROOT)/lib/pkgconfig
PKGCONF ?= env PKG_CONFIG_PATH="$(TOOLCHAIN_PKG_CONFIG_PATH)" pkg-config
STRIP ?= strip
SRCS    := $(wildcard src/*.c)
OBJS    := $(SRCS:src/%.c=build/%.o)
HDRS    := $(wildcard src/*.h)

CFLAGS  += -O3 -g -Wall -Wextra -Isrc $(shell $(PKGCONF) --cflags libdpdk)
# --rx-split 需要 RTE_FLOW_ITEM_TYPE_RANDOM（DOCA 22.11、上游 24.03 起有；Ubuntu 24.04 的 23.11 没有）
CFLAGS  += $(shell grep -qs RTE_FLOW_ITEM_TYPE_RANDOM $(addsuffix /rte_flow.h,$(patsubst -I%,%,$(filter -I%,$(shell $(PKGCONF) --cflags-only-I libdpdk)))) && echo -DHAVE_FLOW_RANDOM)
LDLIBS  += $(shell $(PKGCONF) --libs libdpdk)

# 日常构建不检查版本，沿用机器上已有的 DPDK；固定版本只对 make static 强制。
$(APP): $(OBJS)
	$(CC) $(CFLAGS) $^ -o $@ $(LDLIBS)

# 单文件发布构建。要求 libdpdk.pc 引用的 DPDK、rdma-core、libnuma 等依赖
# 均提供版本匹配的静态库；链接器缺少任何静态依赖时会直接失败，避免产出
# 表面叫 static、运行时仍依赖 DPDK .so 的文件。
# 全静态时 glibc 的 dlopen 会段错误（EAL 启动时探测插件就会调用），用 --wrap 换成
# 总是返回“找不到”的空实现：驱动都已静态链接，不需要加载任何 .so。
$(STATIC_APP): $(OBJS) static/nodlopen.c | check-static-deps
	$(CC) $(CFLAGS) -static -Wl,--wrap=dlopen $^ -o $@ $(shell $(PKGCONF) --static --libs libdpdk)
	@file $@ | grep -q 'statically linked' || { echo "$@ 不是全静态 ELF" >&2; rm -f $@; exit 1; }
	@! ldd $@ >/dev/null 2>&1 || { echo "$@ 仍存在动态依赖" >&2; rm -f $@; exit 1; }
	$(STRIP) --strip-unneeded $@

static: $(STATIC_APP)

check-deps:
	@v=`$(PKGCONF) --modversion libdpdk 2>/dev/null` || { echo "找不到 $(DPDK_ROOT) 下的 DPDK $(DPDK_VERSION)" >&2; exit 1; }; \
	 test "$$v" = "$(DPDK_VERSION)" || { echo "DPDK 版本 $$v，不是固定版本 $(DPDK_VERSION)" >&2; exit 1; }
	@v=`$(PKGCONF) --modversion libibverbs 2>/dev/null` || { echo "找不到 $(RDMA_ROOT) 下的 rdma-core $(RDMA_VERSION)" >&2; exit 1; }; \
	 case "$$v" in *.50.0) ;; *) echo "libibverbs 版本 $$v，不属于 rdma-core $(RDMA_VERSION)" >&2; exit 1;; esac

check-static-deps: check-deps
	@test -f "$(RDMA_ROOT)/lib/libibverbs.a" -a -f "$(RDMA_ROOT)/lib/libmlx5.a" || { \
	 echo "$(RDMA_ROOT) 缺少静态 rdma-core；先运行 scripts/build-static-toolchain.sh" >&2; exit 1; }

build/%.o: src/%.c $(HDRS) | build
	$(CC) $(CFLAGS) -c $< -o $@

build:
	mkdir -p build

clean:
	rm -rf build $(APP) $(STATIC_APP) rndq pingpong

.PHONY: clean static check-deps check-static-deps

# RANDOM 分队列探针（需带 RTE_FLOW_ITEM_TYPE_RANDOM 的 DPDK，如 DOCA 22.11）
rndq: tools/rndq.c
	$(CC) $(CFLAGS) $< -o $@ $(LDLIBS)

# 最小 DPDK 乒乓时延探针（单队列、无 rte_flow），同主机两端时分出去程 / 回程
pingpong: tools/pingpong.c
	$(CC) $(CFLAGS) $< -o $@ $(LDLIBS)
