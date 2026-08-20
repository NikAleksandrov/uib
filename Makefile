CC      ?= cc
AR      ?= ar
PREFIX  ?= /usr/local

-include config.mk

CFLAGS  ?= -O2 -g -Wall -Wextra -Wno-unused-parameter
CFLAGS  += -Iinclude -fPIC -pthread -D_GNU_SOURCE -MMD -MP

URING_CFLAGS ?= $(if $(wildcard ../liburing/src/include/liburing.h),-I../liburing/src/include)
URING_LIBS ?= $(if $(wildcard /usr/lib/x86_64-linux-gnu/liburing.so),-luring,-l:liburing.so.2)
HAVE_IBVERBS ?= $(shell printf '#include <infiniband/verbs.h>\n' | \
	$(CC) -x c -fsyntax-only - >/dev/null 2>&1 && echo 1 || echo 0)
ENABLE_PROVIDER ?= $(HAVE_IBVERBS)
HAVE_VIRTME ?= $(shell command -v vng >/dev/null 2>&1 && echo 1 || echo 0)
HAVE_IBV_TOOLS ?= $(shell command -v ibv_rc_pingpong >/dev/null 2>&1 && echo 1 || echo 0)
HAVE_DMABUF ?= $(shell printf '#include <linux/dma-buf.h>\nint x=DMA_BUF_IOCTL_SYNC;\n' | \
	$(CC) -x c -fsyntax-only - >/dev/null 2>&1 && echo 1 || echo 0)
HAVE_ZEROCOPY ?= $(shell printf '#include <sys/socket.h>\n#include <linux/errqueue.h>\nint x=MSG_ZEROCOPY+SO_ZEROCOPY+SO_EE_ORIGIN_ZEROCOPY;\n' | \
	$(CC) -D_GNU_SOURCE -x c -fsyntax-only - >/dev/null 2>&1 && echo 1 || echo 0)

CFLAGS += $(URING_CFLAGS) $(IBVERBS_CFLAGS)
LDFLAGS += -pthread
LDLIBS += $(URING_LIBS)

# AF_XDP backend (udp-zc-afxdp) is compiled with real libxdp glue only when
# libxdp/libbpf are available; otherwise transport_afxdp.c compiles to a stub
# whose probe() returns 0 and the dispatcher falls back to udp-io-uring.
HAVE_AFXDP ?= $(shell pkg-config --exists libxdp libbpf 2>/dev/null && echo 1 || echo 0)
ifeq ($(HAVE_AFXDP),1)
  AFXDP_CFLAGS ?= $(shell pkg-config --cflags libxdp libbpf)
  AFXDP_LIBS ?= $(shell pkg-config --libs libxdp libbpf)
  CFLAGS += -DUIB_HAVE_AFXDP $(AFXDP_CFLAGS)
  LDLIBS += $(AFXDP_LIBS)
endif

# devmem TCP RX (tcp-devmem C2) needs the SO_DEVMEM uapi. When present the
# tcp-devmem backend can compile its dma-buf-to-queue binding + devmem RX path.
HAVE_DEVMEM ?= $(shell printf 'int x=SO_DEVMEM_DMABUF;\n' | $(CC) \
	-D_GNU_SOURCE -include sys/socket.h -x c -fsyntax-only - \
	>/dev/null 2>&1 && echo 1 || echo 0)
ifeq ($(HAVE_DEVMEM),1)
  CFLAGS += -DUIB_HAVE_DEVMEM
endif

ifeq ($(HAVE_DMABUF),1)
  CFLAGS += -DUIB_HAVE_DMABUF
endif

ifeq ($(HAVE_ZEROCOPY),1)
  CFLAGS += -DUIB_HAVE_ZEROCOPY
endif

SRCS := \
	src/bth.c \
	src/pd.c \
	src/mr.c \
	src/cq.c \
	src/utils.c \
	src/qp.c \
	src/protocol.c \
	src/protocol_rocev2.c \
	src/protocol_dummy.c \
	src/protocol_uet.c \
	src/uet_nscc.c \
	src/context.c \
	src/transport.c \
	src/zerocopy.c \
	src/transport_udp.c \
	src/transport_afxdp.c \
	src/transport_tcp_devmem.c \
	src/afxdp_frame.c \
	src/neigh.c \
	src/devmem.c \
	src/verbs.c

OBJS := $(SRCS:.c=.o)

PROV_SRCS := src/provider.c
PROV_OBJS := $(PROV_SRCS:.c=.o)
DEPS := $(OBJS:.o=.d) $(PROV_OBJS:.o=.d)

LIB_STATIC   := libuib.a
LIB_SHARED   := libuib.so
LIB_SHARED_SONAME := libuib.so.0
LIB_SHARED_REAL := libuib.so.0.1.0
LIB_MAP      := src/libuib.map
LIB_PROVIDER := libuibv.so

LIB_TARGETS := $(LIB_STATIC) $(LIB_SHARED)
ifeq ($(ENABLE_PROVIDER),1)
  LIB_TARGETS += $(LIB_PROVIDER)
endif

all: $(LIB_TARGETS)

examples: all
	$(MAKE) -C examples HAVE_IBVERBS=$(HAVE_IBVERBS) \
		IBVERBS_CFLAGS='$(IBVERBS_CFLAGS)' IBVERBS_LIBS='$(IBVERBS_LIBS)'

shared-abi-test: $(LIB_SHARED)
	./tests/run_shared_abi_test.sh

$(LIB_STATIC): $(OBJS)
	$(AR) rcs $@ $^

$(LIB_SHARED): $(LIB_SHARED_SONAME)
	ln -sfn $(LIB_SHARED_SONAME) $@

$(LIB_SHARED_SONAME): $(LIB_SHARED_REAL)
	ln -sfn $(LIB_SHARED_REAL) $@

$(LIB_SHARED_REAL): $(OBJS) $(LIB_MAP)
	$(CC) -shared -Wl,-soname,$(LIB_SHARED_SONAME) -Wl,--version-script=$(LIB_MAP) -o $@ $(OBJS) $(LDFLAGS) $(LDLIBS)

$(LIB_PROVIDER): $(PROV_OBJS) $(LIB_SHARED)
	$(CC) -shared -o $@ $(PROV_OBJS) -L. -Wl,-rpath,'$$ORIGIN' -luib -ldl $(LDFLAGS) $(LDLIBS)

src/%.o: src/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

TEST_BIN    := tests/loopback
FRAME_TEST  := tests/afxdp_frame_test
DMABUF_TEST := tests/dmabuf_mr_test
NEIGH_TEST  := tests/neigh_test
BTH_TEST    := tests/bth_test
RXE_TEST    := tests/rxe_interop
PROTOCOL_TEST := tests/protocol_test
UET_TEST     := tests/uet_test
NSCC_TEST    := tests/nscc_test
ZEROCOPY_TEST := tests/zerocopy_test
PROVIDER_DMABUF_TEST := tests/provider_dmabuf_test
EXTERNAL_DMABUF_TEST := tests/external_dmabuf_test
CQ_EVENT_TEST := tests/cq_event_test
URING_ASYNC_TEST := tests/uring_async_test
PROVIDER_CQ_EVENT_TEST := tests/provider_cq_event_test

DEPS += $(TEST_BIN).d $(FRAME_TEST).d $(DMABUF_TEST).d $(NEIGH_TEST).d \
	$(BTH_TEST).d $(RXE_TEST).d $(PROTOCOL_TEST).d $(UET_TEST).d \
	$(NSCC_TEST).d

DEPS += $(ZEROCOPY_TEST).d $(PROVIDER_DMABUF_TEST).d \
	$(EXTERNAL_DMABUF_TEST).d

DEPS += $(CQ_EVENT_TEST).d $(URING_ASYNC_TEST).d
DEPS += $(PROVIDER_CQ_EVENT_TEST).d

$(TEST_BIN): tests/loopback.c $(LIB_SHARED)
	$(CC) $(CFLAGS) -o $@ $< -L. -Wl,-rpath,'$$ORIGIN/..' -luib $(LDFLAGS) $(LDLIBS)

$(FRAME_TEST): tests/afxdp_frame_test.c $(LIB_STATIC)
	$(CC) $(CFLAGS) -o $@ $< $(LIB_STATIC) $(LDFLAGS) $(LDLIBS)

$(DMABUF_TEST): tests/dmabuf_mr_test.c $(LIB_SHARED)
	$(CC) $(CFLAGS) -o $@ $< -L. -Wl,-rpath,'$$ORIGIN/..' -luib $(LDFLAGS) $(LDLIBS)

$(NEIGH_TEST): tests/neigh_test.c $(LIB_SHARED)
	$(CC) $(CFLAGS) -o $@ $< -L. -Wl,-rpath,'$$ORIGIN/..' -luib $(LDFLAGS) $(LDLIBS)

$(BTH_TEST): tests/bth_test.c $(LIB_STATIC)
	$(CC) $(CFLAGS) -o $@ $< $(LIB_STATIC) $(LDFLAGS) $(LDLIBS)

$(RXE_TEST): tests/rxe_interop.c
	$(CC) $(CFLAGS) -o $@ $< $(IBVERBS_LIBS) -libverbs

$(PROTOCOL_TEST): tests/protocol_test.c $(LIB_STATIC)
	$(CC) $(CFLAGS) -o $@ $< $(LIB_STATIC) $(LDFLAGS) $(LDLIBS)

$(UET_TEST): tests/uet_test.c $(LIB_SHARED)
	$(CC) $(CFLAGS) -o $@ $< -L. -Wl,-rpath,'$$ORIGIN/..' -luib $(LDFLAGS) $(LDLIBS)

$(NSCC_TEST): tests/nscc_test.c $(LIB_SHARED)
	$(CC) $(CFLAGS) -o $@ $< -L. -Wl,-rpath,'$$ORIGIN/..' -luib $(LDFLAGS) $(LDLIBS)

$(ZEROCOPY_TEST): tests/zerocopy_test.c $(LIB_SHARED)
	$(CC) $(CFLAGS) -o $@ $< -L. -Wl,-rpath,'$$ORIGIN/..' -luib $(LDFLAGS) $(LDLIBS)

$(PROVIDER_DMABUF_TEST): tests/provider_dmabuf_test.c $(LIB_PROVIDER)
	$(CC) $(CFLAGS) -o $@ $< $(IBVERBS_LIBS) -libverbs

$(PROVIDER_CQ_EVENT_TEST): tests/provider_cq_event_test.c $(LIB_PROVIDER)
	$(CC) $(CFLAGS) -o $@ $< $(IBVERBS_LIBS) -libverbs

$(EXTERNAL_DMABUF_TEST): tests/external_dmabuf_test.c $(LIB_SHARED)
	$(CC) $(CFLAGS) -o $@ $< -L. -Wl,-rpath,'$$ORIGIN/..' -luib $(LDFLAGS) $(LDLIBS)

$(CQ_EVENT_TEST): tests/cq_event_test.c $(LIB_SHARED)
	$(CC) $(CFLAGS) -o $@ $< -L. -Wl,-rpath,'$$ORIGIN/..' -luib $(LDFLAGS) $(LDLIBS)

$(URING_ASYNC_TEST): tests/uring_async_test.c $(LIB_SHARED)
	$(CC) $(CFLAGS) -o $@ $< -L. -Wl,-rpath,'$$ORIGIN/..' -luib $(LDFLAGS) $(LDLIBS)

ifeq ($(HAVE_IBVERBS),1)
rxe-test-build: $(RXE_TEST)

rxe-test: all rxe-test-build
	./tests/run_rxe_interop.sh

rxe-tools-test: all
	./tests/run_rxe_tools.sh

uib-tools-test: all
	./tests/run_uib_tools.sh

uet-tools-test: all
	UIB_TEST_PROTOCOL=uet UIB_TEST_PORT=5432 ./tests/run_uib_tools.sh

provider-dmabuf-test: all $(PROVIDER_DMABUF_TEST)
	LD_PRELOAD=$(CURDIR)/$(LIB_PROVIDER) UIB_BIND_PORT=19101 \
		UIB_UDP_MODE=socket \
		./$(PROVIDER_DMABUF_TEST)

provider-cq-event-test: all $(PROVIDER_CQ_EVENT_TEST)
	UIB_BIND_PORT=19610 LD_PRELOAD=$(CURDIR)/$(LIB_PROVIDER) \
		./$(PROVIDER_CQ_EVENT_TEST)
else
rxe-test-build rxe-test rxe-tools-test uib-tools-test uet-tools-test \
provider-dmabuf-test provider-cq-event-test:
	@echo "SKIP $@: libibverbs development files are unavailable"
endif

uet-loss-test: all
	UIB_TEST_PROTOCOL=uet UIB_TEST_PORT=5432 UIB_TEST_DROP_TX=1 \
		UIB_TEST_ITERS=8 \
		./tests/run_uib_tools.sh

rocev2-loss-test: all $(TEST_BIN)
	UIB_TEST_DROP_TX=1 ./$(TEST_BIN)

uet-congestion-test: all $(TEST_BIN) $(NSCC_TEST)
	./$(NSCC_TEST)
	UIB_PROTOCOL=uet UIB_UET_NSCC_MAX_WND=5000 ./$(TEST_BIN)

uet-multipath-test: all $(TEST_BIN)
	UIB_PROTOCOL=uet UIB_UET_MULTIPATH=oblivious ./$(TEST_BIN)
	UIB_PROTOCOL=uet UIB_UET_MULTIPATH=path-aware UIB_TEST_DROP_TX=1 \
		./$(TEST_BIN)

dmabuf-e2e-test: examples
	./examples/direct_traffic rocev2 dmabuf
	./examples/direct_traffic uet dmabuf
	./examples/direct_traffic dummy dmabuf
	./examples/stress_bench --protocol rocev2 --memory dmabuf --iterations 64 --size 16384 --window 16
	./examples/stress_bench --protocol uet --memory dmabuf --iterations 64 --size 16384 --window 16
	./examples/stress_bench --protocol dummy --memory dmabuf --iterations 64 --size 16384 --window 16

zerocopy-e2e-test: examples
	UIB_UDP_MODE=socket UIB_ZEROCOPY=off ./examples/stress_bench --protocol rocev2 --iterations 64 --size 16384 --window 16
	UIB_UDP_MODE=socket UIB_ZEROCOPY=required ./examples/stress_bench --protocol rocev2 --iterations 64 --size 16384 --window 16
	UIB_TRANSPORT_MODE=tcp-devmem UIB_ZEROCOPY=off ./examples/stress_bench --protocol uet --iterations 64 --size 16384 --window 16
	UIB_TRANSPORT_MODE=tcp-devmem UIB_ZEROCOPY=required ./examples/stress_bench --protocol uet --iterations 64 --size 16384 --window 16

benchmark-all: examples
	./examples/benchmark_matrix.sh

benchmark-local: benchmark-all

benchmark-report: examples
	@output=$${UIB_BENCH_OUTPUT:-/tmp/uib-benchmark-report-$$(date +%Y%m%d-%H%M%S)}; \
		status=0; \
		./examples/benchmark_matrix.sh "$$output" || status=$$?; \
		./examples/benchmark_report.sh "$$output"; \
		exit $$status

benchmark-smoke: examples
	UIB_BENCH_ITERATIONS=64 UIB_BENCH_SIZE=16384 UIB_BENCH_WINDOW=16 \
		./examples/benchmark_matrix.sh /tmp/uib-benchmark-smoke

gpu-dmabuf-test: all $(EXTERNAL_DMABUF_TEST)
	./$(EXTERNAL_DMABUF_TEST)

external-dmabuf-test: all $(EXTERNAL_DMABUF_TEST)
	./tests/run_external_dmabuf.sh

optional-features-test:
	./tests/run_optional_features.sh

remote-benchmark-test: examples
	./tests/run_remote_benchmark.sh

namespace-benchmark-test: examples
	./tests/run_namespace_benchmark.sh

examples-test: examples
	./examples/direct_traffic rocev2
	./examples/direct_traffic uet
	./examples/direct_traffic dummy
ifeq ($(HAVE_IBVERBS),1)
	UIB_BIND_PORT=19100 LD_PRELOAD=$(CURDIR)/$(LIB_PROVIDER) \
		./examples/verbs_traffic rocev2
	UIB_BIND_PORT=19100 LD_PRELOAD=$(CURDIR)/$(LIB_PROVIDER) \
		./examples/verbs_traffic uet
	UIB_BIND_PORT=19100 LD_PRELOAD=$(CURDIR)/$(LIB_PROVIDER) \
		./examples/verbs_traffic dummy
endif

examples-stress-test: examples
	./examples/stress_bench --protocol rocev2 --iterations 256 --size 1024 --window 32
	./examples/stress_bench --protocol uet --iterations 256 --size 1024 --window 32
	./examples/stress_bench --protocol dummy --iterations 256 --size 1024 --window 32

uet-test: all $(TEST_BIN) $(UET_TEST)
	./$(UET_TEST)
	./$(NSCC_TEST)
	UIB_PROTOCOL=uet ./$(TEST_BIN)
	UIB_PROTOCOL=uet UIB_UET_OPTIMIZED=off ./$(TEST_BIN)
	UIB_PROTOCOL=uet UIB_UET_OPTIMIZED=small ./$(TEST_BIN)
	UIB_PROTOCOL=uet UIB_UET_OPTIMIZED=medium ./$(TEST_BIN)
	UIB_PROTOCOL=uet UIB_TRANSPORT_MODE=tcp-devmem ./$(TEST_BIN)

test: shared-abi-test $(TEST_BIN) $(FRAME_TEST) $(DMABUF_TEST) $(NEIGH_TEST) $(BTH_TEST) \
	$(PROTOCOL_TEST) $(UET_TEST) $(NSCC_TEST) $(ZEROCOPY_TEST) \
	$(CQ_EVENT_TEST) $(URING_ASYNC_TEST)
	./$(BTH_TEST)
	./$(FRAME_TEST)
	./$(DMABUF_TEST)
	./$(NEIGH_TEST)
	./$(PROTOCOL_TEST)
	./$(UET_TEST)
	./$(NSCC_TEST)
	./$(ZEROCOPY_TEST)
	./$(CQ_EVENT_TEST)
	./$(URING_ASYNC_TEST)
	UIB_TRANSPORT_MODE=tcp-devmem ./$(TEST_BIN)
	./$(TEST_BIN)

guest-test: all rxe-test-build
	$(MAKE) test
	$(MAKE) rxe-test
	$(MAKE) rxe-tools-test
	$(MAKE) uib-tools-test
	$(MAKE) uet-test
	$(MAKE) uet-tools-test
	$(MAKE) uet-loss-test
	$(MAKE) rocev2-loss-test
	$(MAKE) uet-congestion-test
	$(MAKE) uet-multipath-test
	$(MAKE) examples-test
	$(MAKE) examples-stress-test
	$(MAKE) dmabuf-e2e-test
	$(MAKE) zerocopy-e2e-test
	$(MAKE) benchmark-smoke
	$(MAKE) provider-dmabuf-test
	$(MAKE) provider-cq-event-test
	$(MAKE) optional-features-test
	$(MAKE) remote-benchmark-test
	UIB_NAMESPACE_REQUIRED=1 $(MAKE) namespace-benchmark-test
	$(MAKE) gpu-dmabuf-test
	$(MAKE) external-dmabuf-test

virtme-test:
	./tests/run_virtme_tests.sh

test-all: virtme-test

configure-test: configure
	./configure
	$(MAKE) all

test-all: configure-test

install: $(LIB_TARGETS)
	install -d $(DESTDIR)$(PREFIX)/lib
	install -d $(DESTDIR)$(PREFIX)/include/uib
	install -m 644 $(LIB_STATIC) $(DESTDIR)$(PREFIX)/lib/
	install -m 755 $(LIB_SHARED_REAL) $(DESTDIR)$(PREFIX)/lib/
	ln -sfn $(LIB_SHARED_REAL) $(DESTDIR)$(PREFIX)/lib/$(LIB_SHARED_SONAME)
	ln -sfn $(LIB_SHARED_SONAME) $(DESTDIR)$(PREFIX)/lib/$(LIB_SHARED)
ifeq ($(ENABLE_PROVIDER),1)
	install -m 755 $(LIB_PROVIDER) $(DESTDIR)$(PREFIX)/lib/
endif
	install -m 644 include/uib/*.h $(DESTDIR)$(PREFIX)/include/uib/

clean:
	$(MAKE) -C examples clean
	rm -f $(OBJS) $(PROV_OBJS) $(DEPS) $(LIB_STATIC) $(LIB_SHARED) $(LIB_SHARED_SONAME) $(LIB_SHARED_REAL) \
		$(LIB_PROVIDER) $(TEST_BIN) $(FRAME_TEST) $(DMABUF_TEST) \
		$(NEIGH_TEST) $(BTH_TEST) $(RXE_TEST) $(PROTOCOL_TEST) \
		$(UET_TEST) $(NSCC_TEST) $(ZEROCOPY_TEST) \
		$(PROVIDER_DMABUF_TEST) $(EXTERNAL_DMABUF_TEST) \
		$(CQ_EVENT_TEST) $(URING_ASYNC_TEST) $(PROVIDER_CQ_EVENT_TEST)

.PHONY: shared-abi-test all examples clean install test rxe-test-build rxe-test rxe-tools-test \
	uib-tools-test uet-test uet-tools-test uet-loss-test rocev2-loss-test \
	uet-congestion-test uet-multipath-test examples-test \
	examples-stress-test dmabuf-e2e-test zerocopy-e2e-test \
	benchmark-all benchmark-local benchmark-report benchmark-smoke \
	provider-dmabuf-test provider-cq-event-test \
	gpu-dmabuf-test external-dmabuf-test remote-benchmark-test \
	namespace-benchmark-test guest-test \
	optional-features-test virtme-test configure-test test-all

-include $(DEPS)
