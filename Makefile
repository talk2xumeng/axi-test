APP := axiperf
PKGCONF ?= pkg-config
CFLAGS += -O3 -g -Wall -Wextra $(shell $(PKGCONF) --cflags libdpdk)
LDLIBS += $(shell $(PKGCONF) --libs libdpdk)

$(APP): axiperf.c
	$(CC) $(CFLAGS) $< -o $@ $(LDLIBS)

clean:
	rm -f $(APP)
