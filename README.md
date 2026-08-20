UIB userspace RDMA
==================

UIB is a userspace RDMA implementation with independent wire-protocol and
packet-transport layers. It provides native UIB APIs and a transparent
libibverbs compatibility library.

Requirements
------------

The core library requires:

  * Linux and a C11-capable C compiler, GNU make, pthreads and standard Linux
    networking headers;
  * liburing headers and runtime library (Debian/Ubuntu: liburing-dev;
    Fedora: liburing-devel); and
  * autoconf 2.69 or newer only when regenerating configure.

Optional functionality is enabled when configure finds its dependencies:

  * libibverbs headers/runtime (Debian/Ubuntu: libibverbs-dev and
    ibverbs-providers; Fedora: libibverbs-devel and rdma-core) build libuibv,
    the transparent verbs shim, verbs examples and RXE tests;
  * libxdp and libbpf development files build the AF_XDP transport
    (Debian/Ubuntu: libxdp-dev and libbpf-dev; Fedora: libxdp-devel and
    libbpf-devel);
  * Linux headers defining SO_DEVMEM_DMABUF enable TCP netdev DMA-BUF queue
    binding scaffolding;
  * Linux dma-buf headers enable mmap-backed system/GPU DMA-BUF registration;
  * Linux headers defining SO_ZEROCOPY and MSG_ZEROCOPY enable optional socket
    transmit zero-copy;
  * virtme-ng, a kernel image, iproute2 and rdma-core tools run the VM/RXE
    integration tests; and
  * perftest or rdma-core's ibv_rc_pingpong provides standard tools coverage.

Run the checked-in configure script, then build:

    ./configure
    make -j$(nproc)

Use --disable-provider, --disable-af-xdp, --disable-devmem,
--disable-dmabuf or --disable-zerocopy to force a feature off. The matching
--enable-* option requires it and fails configuration when its dependency is
absent. A nonstandard liburing installation can be selected with
--with-liburing=PREFIX. The detected values are written to config.mk. Building
directly with make remains supported and performs equivalent best-effort
probes.

Protocols and transports
------------------------

Select the wire protocol with UIB_PROTOCOL:

    rocev2  InfiniBand RC over RoCEv2 (default, UDP port 4791)
    uet     Ultra Ethernet PDS/RUD PDC, SES and NSCC (UDP port 5432)
    dummy   Minimal SEND-only protocol used to test the protocol boundary

Select the packet transport with UIB_TRANSPORT_MODE:

    udp-blocking  unprivileged UDP socket backend (default)
    udp-zc-afxdp  AF_XDP backend when libxdp and a capable interface are present
    tcp-devmem    Framed TCP backend with optional netdev DMA-BUF queue binding

Set UIB_UDP_MODE=raw only for RXE wire interoperability. Raw sockets require
CAP_NET_RAW; the default socket mode works in an ordinary rootless container.
UIB_ZEROCOPY=off, auto, on or required controls MSG_ZEROCOPY and AF_XDP copy
selection. Auto uses socket zero-copy for packets at least 10 KiB. On and
required use a 1 KiB floor so small protocol ACKs cannot stall waiting for a
TCP zero-copy completion. Override either threshold with UIB_ZEROCOPY_THRESHOLD.

UET enables the specification-defined NSCC congestion controller by default.
Set UIB_UET_CC=none only for comparison testing. UIB_UET_NSCC_MAX_WND can cap
the initial congestion window in tests; applications can configure and query a
QP with uib_uet_nscc_configure_qp() and uib_uet_nscc_query_qp().

UIB_UET_OPTIMIZED selects off, auto, small or medium SES request headers.
UIB_UET_MULTIPATH selects off, oblivious or path-aware entropy multipathing.
The corresponding public configuration/query APIs allow per-QP selection and
statistics without environment variables.

Testing and examples
--------------------

Run the complete test matrix, including configure probing, virtme-ng, kernel
RXE interoperability, standard ibverbs tools, every UIB protocol, UET wire
layouts, optimized headers, integer atomics, NSCC, loss recovery, entropy
multipath/failover and stress traffic, with:

    make test-all

Build and run the native UIB, libibverbs and windowed traffic examples with:

    make examples
    make examples-test
    make examples-stress-test

See the [examples guide](examples/) for individual commands and benchmark
options. The complete reference run and comparison tables are in
[the benchmark report](docs/benchmark-results.md). See the
[transparent verbs guide](docs/transparent-verbs.md) for running an existing,
unmodified verbs application through UIB.

DMA-BUF and zero-copy
---------------------

System DMA-BUFs and CPU-mappable GPU DMA-BUFs can be registered with
uib_reg_mr_dmabuf(), uib_reg_mr_dmabuf_iova() or standard
ibv_reg_dmabuf_mr(). UIB owns a duplicate of the fd, maps the requested range,
uses synthetic or caller-selected IOVA addressing, and brackets library CPU
access with DMA_BUF_IOCTL_SYNC. Applications directly accessing mr->addr use
uib_mr_cpu_begin() and uib_mr_cpu_end().

Non-mappable GPU DMA-BUFs are rejected unless a selected transport advertises
a complete device RX/TX datapath. The current TCP netdev binding is setup
scaffolding and deliberately does not advertise that capability: its framed
UIB headers cannot yet be parsed from opaque GPU memory. This avoids silently
claiming a transfer whose bytes were never placed. See
docs/dmabuf-zerocopy.md for the exact capability boundary.

Ultra Ethernet scope
--------------------

UIB is an independent implementation of selected UET mechanisms. It is not
affiliated with or certified by the Ultra Ethernet Consortium, and it does
not claim complete conformance to the Ultra Ethernet Specification or
interoperability with third-party UET implementations. Current UET
interoperability testing covers UIB-to-UIB endpoints only.

The UET implementation follows Ultra Ethernet Specification 1.0.3. It provides
RUD connection establishment, the 128-packet PDC receive window, cumulative and
exact acknowledgements, retransmission, standard and optimized SES headers,
MTU-sized response-data packets, SEND with header data, RDMA WRITE/READ, the
required 8/16/32/64-bit signed and unsigned atomic operation matrix, exact-once
fetching atomics, NSCC, and oblivious/path-aware entropy multipath with timeout
failover. ROD and UUD are intentionally not implemented.
