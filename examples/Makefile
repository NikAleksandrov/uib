# SPDX-License-Identifier: Apache-2.0

CC ?= cc
HAVE_IBVERBS ?= 1

CFLAGS ?= -O2 -g -Wall -Wextra
CFLAGS += -I../include -pthread -D_GNU_SOURCE
LDFLAGS += -L.. -Wl,-rpath,'$$ORIGIN/..' -pthread

DIRECT_COMMON := direct_common.c
PROGRAMS := direct_traffic qp_scaling_bench stress_bench
ifeq ($(HAVE_IBVERBS),1)
  PROGRAMS += verbs_traffic
endif

all: $(PROGRAMS)

direct_traffic: direct_traffic.c $(DIRECT_COMMON) ../libuib.so
	$(CC) $(CFLAGS) -o $@ direct_traffic.c $(DIRECT_COMMON) \
		$(LDFLAGS) -luib

stress_bench: stress_bench.c $(DIRECT_COMMON) ../libuib.so
	$(CC) $(CFLAGS) -o $@ stress_bench.c $(DIRECT_COMMON) \
		$(LDFLAGS) -luib

qp_scaling_bench: qp_scaling_bench.c ../libuib.so
	$(CC) $(CFLAGS) -o $@ qp_scaling_bench.c $(LDFLAGS) -luib

verbs_traffic: verbs_traffic.c ../libuibv.so
	$(CC) $(CFLAGS) $(IBVERBS_CFLAGS) -o $@ verbs_traffic.c $(LDFLAGS) \
		-luibv $(IBVERBS_LIBS) -libverbs

clean:
	rm -f direct_traffic qp_scaling_bench verbs_traffic stress_bench

.PHONY: all clean
