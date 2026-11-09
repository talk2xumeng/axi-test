APP     := axiperf
STATIC_APP := axiperf-static
PKGCONF ?= pkg-config
SRCS    := $(wildcard src/*.c)
OBJS    := $(SRCS:src/%.c=build/%.o)
HDRS    := $(wildcard src/*.h)

CFLAGS  += -O3 -g -Wall -Wextra -Isrc $(shell $(PKGCONF) --cflags libdpdk)
# --rx-split 需要 RTE_FLOW_ITEM_TYPE_RANDOM（DOCA 22.11、上游 24.03 起有；Ubuntu 24.04 的 23.11 没有）
CFLAGS  += $(shell grep -qs RTE_FLOW_ITEM_TYPE_RANDOM $(addsuffix /rte_flow.h,$(patsubst -I%,%,$(filter -I%,$(shell $(PKGCONF) --cflags-only-I libdpdk)))) && echo -DHAVE_FLOW_RANDOM)
LDLIBS  += $(shell $(PKGCONF) --libs libdpdk)

$(APP): $(OBJS)
	$(CC) $(CFLAGS) $^ -o $@ $(LDLIBS)

# 单文件发布构建。要求 libdpdk.pc 引用的 DPDK、rdma-core、libnuma 等依赖
# 均提供版本匹配的静态库；链接器缺少任何静态依赖时会直接失败，避免产出
# 表面叫 static、运行时仍依赖 DPDK .so 的文件。
$(STATIC_APP): $(OBJS)
	$(CC) $(CFLAGS) -static $^ -o $@ $(shell $(PKGCONF) --static --libs libdpdk)
	@file $@ | grep -q 'statically linked' || { echo "$@ 不是全静态 ELF" >&2; rm -f $@; exit 1; }
	@! ldd $@ >/dev/null 2>&1 || { echo "$@ 仍存在动态依赖" >&2; rm -f $@; exit 1; }

static: $(STATIC_APP)

build/%.o: src/%.c $(HDRS) | build
	$(CC) $(CFLAGS) -c $< -o $@

build:
	mkdir -p build

clean:
	rm -rf build $(APP) $(STATIC_APP) rndq pingpong

.PHONY: clean static

# RANDOM 分队列探针（需带 RTE_FLOW_ITEM_TYPE_RANDOM 的 DPDK，如 DOCA 22.11）
rndq: tools/rndq.c
	$(CC) $(CFLAGS) $< -o $@ $(LDLIBS)

# 最小 DPDK 乒乓时延探针（单队列、无 rte_flow），同主机两端时分出去程 / 回程
pingpong: tools/pingpong.c
	$(CC) $(CFLAGS) $< -o $@ $(LDLIBS)
