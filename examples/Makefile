CC ?= cc

CFLAGS ?= -O2 -g -Wall -Wextra
CFLAGS += -I../include -pthread -D_GNU_SOURCE
LDFLAGS += -L.. -Wl,-rpath,'$$ORIGIN/..' -pthread

DIRECT_COMMON := direct_common.c
PROGRAMS := direct_traffic verbs_traffic stress_bench

all: $(PROGRAMS)

direct_traffic: direct_traffic.c $(DIRECT_COMMON) ../libuib.so
	$(CC) $(CFLAGS) -o $@ direct_traffic.c $(DIRECT_COMMON) \
		$(LDFLAGS) -luib

stress_bench: stress_bench.c $(DIRECT_COMMON) ../libuib.so
	$(CC) $(CFLAGS) -o $@ stress_bench.c $(DIRECT_COMMON) \
		$(LDFLAGS) -luib

verbs_traffic: verbs_traffic.c ../libuibv.so
	$(CC) $(CFLAGS) -o $@ verbs_traffic.c $(LDFLAGS) -luibv -libverbs

clean:
	rm -f $(PROGRAMS)

.PHONY: all clean
