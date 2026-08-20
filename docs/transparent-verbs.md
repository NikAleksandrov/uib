Transparent use by existing libibverbs applications
====================================================

UIB's `libuibv.so` exports the ordinary libibverbs entry points and forwards
objects belonging to real RDMA devices to the system libibverbs. It also adds a
synthetic device named `uib0`. Preloading it means an existing dynamically
linked verbs application does not need source changes or recompilation.

Build the provider and verify that configure reports `libibverbs provider: 1`:

  ./configure
  make

Run an application with an absolute preload path. For example, from the source
tree:

  UIB_PROTOCOL=uet \
  UIB_BIND_IP=198.51.100.2 \
  UIB_BIND_PORT=5432 \
  LD_PRELOAD=$PWD/libuibv.so \
  ibv_rc_pingpong -d uib0 198.51.100.1

The peer uses its own local address and the same protocol. The application sees
normal `ibv_device`, context, PD, MR, CQ and QP objects. Its calls to
`ibv_post_send`, `ibv_post_recv` and `ibv_poll_cq` are translated internally;
the application itself remains unaware of UIB.

Configuration
-------------

The wrapper recognizes these environment variables before the application
opens `uib0`:

  UIB_PROTOCOL          rocev2, uet or dummy
  UIB_BIND_IP           local IPv4 address
  UIB_BIND_PORT         local UDP/TCP service port
  UIB_TRANSPORT_MODE    udp-io-uring, udp-zc-afxdp or tcp-devmem
  UIB_URING_RX_MODE     auto (default), multishot or batch
  UIB_UDP_MODE          socket (default) or raw (requires CAP_NET_RAW)
  UIB_ZEROCOPY          off, auto, on or required
  UIB_ZEROCOPY_THRESHOLD minimum packet size for auto mode
  UIB_UET_CC            nscc (default) or none
  UIB_UET_OPTIMIZED     off, auto, small or medium
  UIB_UET_MULTIPATH     off, oblivious or path-aware

If an application accepts a device option, select `uib0`. If it simply chooses
the first returned device, UIB deliberately places `uib0` first. A launcher
script or service-unit environment is usually the cleanest deployment because
it keeps preload and network selection outside the application.

Compatibility boundary
----------------------

The compatibility layer implements the common RC data path: device/port/GID
queries, PDs, registered memory, CQs, QPs, SEND/RECV, RDMA WRITE/READ, immediate
data, standard `ibv_reg_dmabuf_mr()`, standard 64-bit fetch-add/compare-swap,
completion channels, pollable channel descriptors and one-shot CQ
notifications. Native UIB APIs expose the
larger UET atomic matrix and UET-specific tuning/statistics. Applications that
require device async events, shared receive queues, memory windows, XRC/DC,
userspace provider discovery internals or other extended verbs must currently
be adapted or kept on a real provider.

`make uib-tools-test` and `make uet-tools-test` run the unmodified
`ibv_rc_pingpong` binary in separate network namespaces. The RXE test additionally
proves bidirectional SEND, WRITE and READ data transfer between this preload
provider and the kernel RXE device.
