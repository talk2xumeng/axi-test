APP     := axiperf
PKGCONF ?= pkg-config
SRCS    := $(wildcard src/*.c)
OBJS    := $(SRCS:src/%.c=build/%.o)
HDRS    := $(wildcard src/*.h)

CFLAGS  += -O3 -g -Wall -Wextra -Isrc $(shell $(PKGCONF) --cflags libdpdk)
LDLIBS  += $(shell $(PKGCONF) --libs libdpdk)

$(APP): $(OBJS)
	$(CC) $(CFLAGS) $^ -o $@ $(LDLIBS)

build/%.o: src/%.c $(HDRS) | build
	$(CC) $(CFLAGS) -c $< -o $@

build:
	mkdir -p build

clean:
	rm -rf build $(APP) rndq

.PHONY: clean

# RANDOM 分队列探针（需带 RTE_FLOW_ITEM_TYPE_RANDOM 的 DPDK，如 DOCA 22.11）
rndq: tools/rndq.c
	$(CC) $(CFLAGS) $< -o $@ $(LDLIBS)
