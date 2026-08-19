CC      ?= cc
AR      ?= ar
PREFIX  ?= /usr/local

CFLAGS  ?= -O2 -g -Wall -Wextra -Wno-unused-parameter
CFLAGS  += -Iinclude -fPIC -pthread -D_GNU_SOURCE

LIBURING_INC ?= /usr/include
LIBURING_LIB ?= /usr/lib/x86_64-linux-gnu

ifneq ("$(wildcard $(LIBURING_INC)/liburing.h)","")
  CFLAGS += -I$(LIBURING_INC)
else ifneq ("$(wildcard /home/user/Work/Trees/liburing/src/include/liburing.h)","")
  CFLAGS += -I/home/user/Work/Trees/liburing/src/include
endif

LDFLAGS += -L$(LIBURING_LIB) -pthread

URING_LIB := $(if $(wildcard $(LIBURING_LIB)/liburing.so),-luring,-l:liburing.so.2)
LDLIBS  += $(URING_LIB)

# AF_XDP backend (udp-zc-afxdp) is compiled with real libxdp glue only when
# libxdp/libbpf are available; otherwise transport_afxdp.c compiles to a stub
HAVE_AFXDP := $(shell pkg-config --exists libxdp libbpf 2>/dev/null && echo 1)
ifeq ($(HAVE_AFXDP),1)
  CFLAGS += -DUIB_HAVE_AFXDP $(shell pkg-config --cflags libxdp libbpf)
  LDLIBS += $(shell pkg-config --libs libxdp libbpf)
endif

# devmem TCP RX (tcp-devmem C2) needs the SO_DEVMEM uapi. When present the
# tcp-devmem backend can compile its dma-buf-to-queue binding + devmem RX path.
HAVE_DEVMEM := $(shell printf 'int x=SO_DEVMEM_DMABUF;\n' | $(CC) -D_GNU_SOURCE -include sys/socket.h -x c -fsyntax-only - >/dev/null 2>&1 && echo 1)
ifeq ($(HAVE_DEVMEM),1)
  CFLAGS += -DUIB_HAVE_DEVMEM
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
	src/context.c \
	src/transport.c \
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

LIB_STATIC   := libuib.a
LIB_SHARED   := libuib.so
LIB_SHARED_SONAME := libuib.so.0
LIB_SHARED_REAL := libuib.so.0.1.0
LIB_MAP      := src/libuib.map
LIB_PROVIDER := libuibv.so

all: $(LIB_STATIC) $(LIB_SHARED) $(LIB_PROVIDER)

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
	$(CC) $(CFLAGS) -o $@ $< -libverbs

$(PROTOCOL_TEST): tests/protocol_test.c $(LIB_STATIC)
	$(CC) $(CFLAGS) -o $@ $< $(LIB_STATIC) $(LDFLAGS) $(LDLIBS)

rxe-test-build: $(RXE_TEST)

rxe-test: all rxe-test-build
	./tests/run_rxe_interop.sh

rxe-tools-test: all
	./tests/run_rxe_tools.sh

uib-tools-test: all
	./tests/run_uib_tools.sh

test: shared-abi-test $(TEST_BIN) $(FRAME_TEST) $(DMABUF_TEST) $(NEIGH_TEST) $(BTH_TEST) \
	$(PROTOCOL_TEST)
	./$(BTH_TEST)
	./$(FRAME_TEST)
	./$(DMABUF_TEST)
	./$(NEIGH_TEST)
	./$(PROTOCOL_TEST)
	UIB_TRANSPORT_MODE=tcp-devmem ./$(TEST_BIN)
	./$(TEST_BIN)

guest-test: all rxe-test-build
	$(MAKE) test
	$(MAKE) rxe-test
	$(MAKE) rxe-tools-test
	$(MAKE) uib-tools-test

virtme-test:
	./tests/run_virtme_tests.sh

test-all: virtme-test

install: $(LIB_STATIC) $(LIB_STATIC) $(LIB_PROVIDER)
	install -d $(DESTDIR)$(PREFIX)/lib
	install -d $(DESTDIR)$(PREFIX)/include/uib
	install -m 644 $(LIB_STATIC) $(DESTDIR)$(PREFIX)/lib/
	install -m 755 $(LIB_SHARED_REAL) $(DESTDIR)$(PREFIX)/lib/
	ln -sfn $(LIB_SHARED_REAL) $(DESTDIR)$(PREFIX)/lib/$(LIB_SHARED_SONAME)
	ln -sfn $(LIB_SHARED_SONAME) $(DESTDIR)$(PREFIX)/lib/$(LIB_STATIC)
	install -m 755 $(LIB_PROVIDER) $(DESTDIR)$(PREFIX)/lib/
	install -m 644 include/uib/*.h $(DESTDIR)$(PREFIX)/include/uib/

clean:
	rm -f $(OBJS) $(PROV_OBJS) $(LIB_STATIC) $(LIB_STATIC) $(LIB_SHARED_SONAME) $(LIB_SHARED_REAL) $(LIB_PROVIDER) $(TEST_BIN) $(FRAME_TEST) $(DMABUF_TEST) $(NEIGH_TEST) $(BTH_TEST) $(RXE_TEST) $(PROTOCOL_TEST)

.PHONY: shared-abi-test all clean install test rxe-test-build rxe-test rxe-tools-test \
	uib-tools-test guest-test virtme-test test-all
