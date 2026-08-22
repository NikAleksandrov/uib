UIB userspace RDMA
==================

UIB is a userspace RDMA implementation with independent wire-protocol and
packet-transport layers. It provides native UIB APIs and a transparent
libibverbs compatibility library.

Example: transparently running libibverbs software with UIB
-----------------------------------------------------------

The transparent provider can run an unmodified dynamically linked perftest
program such as `ib_write_bw`. `LD_PRELOAD` interposes the normal libibverbs
entry points. UIB places its synthetic `uib0` device first, so perftest's
default device selection chooses it without a `-d` argument. The perftest TCP
connection still exchanges QP information; benchmark WRITE traffic uses the
selected UIB protocol and transport. Add `-d uib0` only when an explicit
device selection is desirable.

Build `libuibv.so`, then start the server on a host whose data-plane address is
`192.0.2.1`:

    ./configure
    make
    env LD_PRELOAD=/absolute/path/to/uib/libuibv.so \
      UIB_PROTOCOL=rocev2 \
      UIB_TRANSPORT_MODE=udp-io-uring \
      UIB_UDP_MODE=socket \
      UIB_BIND_IP=192.0.2.1 \
      UIB_BIND_PORT=4791 \
      ib_write_bw -i 1 -x 0 --connection=RC \
          --size=65536 --iters=10000 --report_gbits -F

Run the matching client on a second host at `192.0.2.2`; the final argument is
the server address used by perftest's control connection:

    env LD_PRELOAD=/absolute/path/to/uib/libuibv.so \
      UIB_PROTOCOL=rocev2 \
      UIB_TRANSPORT_MODE=udp-io-uring \
      UIB_UDP_MODE=socket \
      UIB_BIND_IP=192.0.2.2 \
      UIB_BIND_PORT=4791 \
      ib_write_bw -i 1 -x 0 --connection=RC \
          --size=65536 --iters=10000 --report_gbits -F 192.0.2.1

Both endpoints must select the same protocol and service port. Ordinary
UIB-to-UIB runs use `UIB_UDP_MODE=socket` and therefore need no raw-socket
capability. Use raw mode only when interoperating with a kernel RXE endpoint;
that mode requires `CAP_NET_RAW`. UET can be selected by changing both hosts to
`UIB_PROTOCOL=uet` and `UIB_BIND_PORT=5432`.

Requirements
------------

The core library requires:

  * Linux and a C11-capable C compiler, GNU make, pthreads and standard Linux
    networking headers;
  * liburing headers and runtime library with multishot recvmsg and provided
    buffer-ring APIs (Debian/Ubuntu: liburing-dev; Fedora: liburing-devel);
  * libmnl headers and runtime library for generic-netlink netdev control
    (Debian/Ubuntu: libmnl-dev; Fedora: libmnl-devel); and
  * liburcu-bp headers and runtime library for lockless QP and MR lookup
    (Debian/Ubuntu: liburcu-dev; Fedora: userspace-rcu-devel); and
  * autoconf 2.69 or newer only when regenerating configure.

Optional functionality is enabled when configure finds its dependencies:

  * libibverbs headers/runtime (Debian/Ubuntu: libibverbs-dev and
    ibverbs-providers; Fedora: libibverbs-devel and rdma-core) build libuibv,
    the transparent verbs shim, verbs examples and RXE tests;
  * libxdp and libbpf development files build the AF_XDP transport
    (Debian/Ubuntu: libxdp-dev and libbpf-dev; Fedora: libxdp-devel and
    libbpf-devel);
  * Linux headers defining SO_DEVMEM_DMABUF build the explicit netdev DMA-BUF
    queue-binding helper; transports do not activate it without a complete
    device-memory receive datapath;
  * Linux dma-buf headers enable mmap-backed system/GPU DMA-BUF registration;
  * Linux headers defining SO_ZEROCOPY and MSG_ZEROCOPY enable optional socket
    transmit zero-copy;
  * current liburing headers defining IORING_OP_RECV_ZC enable optional TCP
    host-memory ZCRX; runtime activation additionally needs a supporting
    kernel, CONFIG_IO_URING_ZCRX, and a NIC queue configured for TCP
    header/data split, RSS isolation, and flow steering;
  * zlib development files accelerate the RoCEv2 invariant CRC
    (Debian/Ubuntu: zlib1g-dev; Fedora: zlib-devel);
  * virtme-ng, a kernel image, iproute2 and rdma-core tools run the VM/RXE
    integration tests; and
  * perftest provides `ib_write_bw` benchmark-matrix rows, while rdma-core's
    `ibv_rc_pingpong` provides the standard-tools interoperability test.

Run the checked-in configure script, then build:

    ./configure
    make -j$(nproc)

Use --disable-provider, --disable-af-xdp, --disable-devmem,
--disable-dmabuf or --disable-zerocopy to force a feature off. The matching
--disable-host-zc also removes the ZCRX engine. The matching --enable-* option
requires the feature and fails configuration when its dependency is
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

    udp-io-uring  asynchronous UDP io_uring backend (default)
    udp-sync      synchronous UDP transmit backend for comparison/debugging
    udp-zc-afxdp  AF_XDP backend when libxdp and a capable interface are present
    tcp-devmem    Framed TCP backend with optional netdev DMA-BUF queue binding

The default backend owns its io_uring from a dedicated worker thread. Senders
enqueue into a bounded nonblocking queue; the worker submits sendmsg SQEs in
batches and reaps TX and RX completions together. RX uses provided-buffer
multishot recvmsg on Linux 6.0 and newer. Set UIB_URING_RX_MODE=batch to force
the portable fallback with 32 receive SQEs outstanding, or set it to
multishot to require multishot support. The default auto mode falls back
automatically.

Set `UIB_TRANSPORT_MODE=udp-sync` to use blocking `sendmsg` transmission while
retaining the same io_uring receive worker. This compatibility/measurement
path is optional; an unset `UIB_TRANSPORT_MODE` always selects
`udp-io-uring`.

UIB_URING_RX_BATCH controls the maximum CQEs reaped per worker pass and the
number of receive SQEs used by batch RX. UIB_URING_TX_BATCH limits send SQEs
submitted per worker pass. Both accept values from 1 through 256; the defaults
are 64. Applications can read the effective values from rx_batch and tx_batch
in struct uib_transport_stats.

Completion queues can be attached to an event channel with
uib_create_comp_channel() and uib_create_cq_channel(). Arm one-shot delivery
with uib_req_notify_cq(), wait on the channel fd with poll/epoll, consume the
event with uib_get_cq_event(), and finish with uib_ack_cq_events(). The
transparent provider exposes the corresponding standard ibverbs calls.

Set UIB_UDP_MODE=raw only for RXE wire interoperability. Raw sockets require
CAP_NET_RAW; the default socket mode works in an ordinary rootless container.
UIB_ZEROCOPY=off, auto, on or required controls MSG_ZEROCOPY and AF_XDP copy
selection. Auto uses socket zero-copy for packets at least 10 KiB. On and
required use a 1 KiB floor so small protocol ACKs cannot stall waiting for a
TCP zero-copy completion. Override either threshold with UIB_ZEROCOPY_THRESHOLD.
Socket MSG_ZEROCOPY is transmit-only. Ordinary io_uring multishot receive is
batched, not zero-copy into a verbs MR; AF_XDP native zero-copy ends at its
UMEM frame. Applications that can accept provider-owned receive buffers may
use uib_post_recv_loan(), take the result from a receive completion with
uib_wc_take_rx_loan(), process its contiguous data or iovec segments, and
return or repost it with uib_release_rx_loan() or uib_repost_rx_loan().

With tcp-devmem, UIB_HOST_ZC_IFACE and UIB_HOST_ZC_QUEUE select an io_uring
ZCRX hardware queue. UIB_HOST_ZC=auto falls back to the copied TCP parser when
registration is unavailable; required makes context creation fail instead.
Queue registration requires `CAP_NET_ADMIN` in addition to a ZCRX-capable
kernel, NIC header/data split, RSS separation, and flow steering.
Arena fragments remain owned by UIB until the application releases the loan.
Whole length-prefixed frames are passed through without a payload copy, and
fragmented RoCEv2/UET messages are returned as iovec loans. A TCP stream
fragment that splits a UIB frame uses the preallocated copied fallback.
uib_host_zc_capabilities() reports what the active context actually enabled.
Standard verbs receives keep their caller-selected SGE semantics and remain
copied because Linux ZCRX chooses an arena buffer before UIB sees the WQE.

AF_XDP transmits complete Ethernet frames and deliberately performs no route,
ARP or neighbor-table discovery. Set `UIB_AFXDP_IFACE` to the bound interface
and `UIB_AFXDP_PEER_MAC` to the next-hop MAC address. The peer MAC is mandatory
and fixed for the lifetime of a UIB context; applications using a routed path
must configure the gateway MAC rather than the remote host's MAC.

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

Run the short scaling regression test, including 4-thread QPN demultiplexing
and maximum-window RoCEv2/UET transfers, with:

    make scaling-test

`make scaling-benchmark` runs the corresponding full measurement. It prints
actual lookup throughput, nanoseconds per lookup and CPU seconds for the old
serialized lookup model and the direct lookup path, followed by actual
bandwidth, operation rate, CPU time and latency percentiles for both protocols
at windows 64 and 128. `UIB_SCALE_THREADS`, `UIB_SCALE_LOOKUPS` and
`UIB_SCALE_TRAFFIC_ITERS` control its workload.

See the [examples guide](examples/) for individual commands and benchmark
options. The complete reference run and comparison tables are in
[the benchmark report](docs/benchmark-results.md). See the
[transparent verbs guide](docs/transparent-verbs.md) for running an existing,
unmodified verbs application through UIB.

The generated [environment-variable reference](docs/environment-variables.md)
and [public API reference](docs/public-api.md) cover every supported setting
and every `uib_*` or `uibv_*` declaration in the installed headers. Run
`make docs` to regenerate both files from the kernel-doc comments above their
definitions, or `make docs-check` to verify that checked-in documentation is
current and that no public declaration or consumed environment variable lacks
an annotation. `make install` also installs the generated `uib(7)` manual;
run `man 7 uib` for the overview, transparent-provider example, API reference,
and environment-variable reference.

`make benchmark-doc` regenerates that reference. It first writes an old/new
comparison containing actual values and refuses to replace the document when
the UDP aggregate exceeds the configured regression limit.
Every benchmark run includes matching synchronous and asynchronous UDP rows
and writes their actual bandwidth, latency and CPU comparison to
`sync-async-comparison.csv`, the ASCII report and the Markdown report.
The latest checked-in same-run comparison is in
[the sync/async benchmark](docs/sync-async-benchmark.md). The comparison
document is refreshed even when the separate historical-reference regression
gate rejects replacing `benchmark-results.md`.

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

Provider-owned host-memory receive loans are separate from DMA-BUF imports.
They make full kernel-to-application RX zero-copy possible when TCP ZCRX can
lend complete framed payload segments. They do not pretend that an arbitrary
pre-posted verbs address can be selected by the NIC. Run `make test` for loan
ownership and cross-protocol transfers, and `make host-zc-benchmark` for the
ordinary-post versus recycled-loan control-path cost.
`make host-zc-virtme-test` runs the loan, capability, and cross-protocol tests
inside virtme-ng. Set UIB_VIRTME_KERNEL=/path/to/bzImage to exercise a kernel
new enough for IORING_OP_RECV_ZC; without it the target uses virtme-ng's
default kernel and validates the automatic copied fallback.

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

License
-------

UIB's original code is licensed under the [Apache License 2.0](LICENSE). The
source repository is a mixed-license distribution: files incorporating
separately licensed material identify their terms with SPDX headers. See the
[license map](LICENSES/README.md), [NOTICE](NOTICE), and [AUTHORS](AUTHORS).
