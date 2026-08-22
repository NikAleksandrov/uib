DMA-BUF and zero-copy data paths
================================

UIB keeps these independent and optional:

  * DMA-BUF registration controls where an application's registered memory
    lives. Configure probes it automatically; use --disable-dmabuf or
    --enable-dmabuf to force the decision.
  * Socket zero-copy controls the copy from a completed UIB wire packet into
    the kernel networking stack. Use --disable-zerocopy or --enable-zerocopy
    at build time and UIB_ZEROCOPY=off, auto, on or required at runtime.
  * AF_XDP zero-copy controls the kernel/NIC queue path. It still constructs a
    protocol frame in UMEM, so it is not application-payload zero-copy. The
    backend requires an explicit next-hop MAC in UIB_AFXDP_PEER_MAC and does
    not perform route or neighbor discovery.
  * Netdev device-memory TCP is a separate hardware facility. The current
    queue-binding helper is present, but UIB does not activate it or advertise
    device RX/TX until framed protocol headers can be handled outside opaque
    GPU memory. Registration hooks with no advertised device capability are
    never called, preventing an unusable binding from consuming an RX queue.

None of these mechanisms performs direct placement into an arbitrary verbs
receive SGE. AF_XDP zero-copy ends at UMEM. The separate provider-owned host
receive API can return TCP ZCRX arena segments without a payload copy because
the application accepts the address chosen by the kernel/NIC and explicitly
returns it. `zerocopy_calls` and `zerocopy_bytes` count kernel zero-copy
requests, not proven copy-free application payloads.

DMA-BUF registration
--------------------

uib_reg_mr_dmabuf() chooses a synthetic IOVA. uib_reg_mr_dmabuf_iova() and
ibv_reg_dmabuf_mr() preserve a caller-selected IOVA. UIB duplicates the fd, so
the caller may close its descriptor after successful registration. Unaligned
offsets are supported by page-aligning the mmap internally.

A CPU-mappable exporter gets UIB_MR_CAP_CPU_MAPPED. UIB's gather, scatter,
SEND, WRITE, READ and atomic paths translate the IOVA and perform real copies;
they no longer treat range validation as successful data placement. Library
access is synchronized with DMA_BUF_IOCTL_SYNC. Direct application access to
mr->addr must be enclosed by:

  uib_mr_cpu_begin(mr, UIB_MR_CPU_WRITE);
  /* update bytes through mr->addr */
  uib_mr_cpu_end(mr, UIB_MR_CPU_WRITE);

Use UIB_MR_CPU_READ for CPU reads. A memfd is accepted as a coherent
file-backed test buffer; it reports ENOTTY for DMA_BUF_IOCTL_SYNC, which UIB
treats as an exporter that needs no dma-buf cache synchronization.

GPU DMA-BUFs
------------

GPU exporters differ. A CPU-mappable GPU DMA-BUF works through the synchronized
mapping path, but it is not a GPU-to-NIC payload-direct transfer. A non-mappable
GPU DMA-BUF returns EOPNOTSUPP unless the active transport advertises
UIB_MR_CAP_DEVICE_RX or UIB_MR_CAP_DEVICE_TX. UIB_DMABUF_REQUIRE_ZEROCOPY makes
that requirement explicit even when an mmap would succeed.

This boundary is intentional. Linux device-memory TCP can place TCP payload in
a DMA-BUF, but UIB's length prefix and RDMA/UE protocol headers are part of that
payload. The CPU cannot demultiplex a frame stored only in opaque GPU memory.
A complete GPU-direct implementation needs protocol-aware header split or a
NIC/kernel offload that leaves UIB headers linear and places only the selected
RDMA payload in the target DMA-BUF.

GPU CI can pass an inherited exporter fd as UIB_TEST_GPU_DMABUF_FD to
`make gpu-dmabuf-test`; UIB_TEST_GPU_DMABUF_OFFSET is optional. The test writes
and reads a synchronized pattern when the exporter is CPU-mappable and reports
an explicit skip otherwise. Remote benchmarks accept an inherited fd through
UIB_BENCH_DMABUF_FD with UIB_BENCH_MEMORY=gpu-dmabuf on each host. UIB itself
does not depend on CUDA, ROCm or a particular DRM allocator because it imports
the standard DMA-BUF fd produced by those stacks.

Socket zero-copy semantics
--------------------------

The UDP io_uring and framed TCP transports enable socket transmit zero-copy
when requested. UDP submits eligible packets with IORING_OP_SENDMSG_ZC, asks
Linux for `IORING_SEND_ZC_REPORT_USAGE`, and keeps each packet alive until its
notification CQE. A notification carrying `IORING_NOTIF_USAGE_ZC_COPIED`
increments `deferred_copies`. TCP and synchronous UDP use MSG_ZEROCOPY and
consume the error-queue completion before releasing the packet buffer;
SO_EE_CODE_ZEROCOPY_COPIED increments the same counter. Statistics are
available through uib_zerocopy_get_stats(). Loopback reports copied fallback
by design, so zero-copy is commonly slower in local smoke tests.

Ordinary UDP receive uses io_uring multishot provided buffers or a batch of
one-shot receive buffers. These remove receive submissions and allocation from
the hot path, but the kernel still copies into those buffers. AF_XDP native
mode lets the NIC DMA into UMEM, after which UIB parses the packet and copies
the payload into the selected posted receive SGE. TCP receive similarly copies
through its framing buffer. Consequently UIB_MR_CAP_DEVICE_RX and
UIB_MR_CAP_DEVICE_TX are not advertised by any current transport.

Provider-owned host RX loans
----------------------------

uib_post_recv_loan() posts capacity rather than a caller-selected address. A
receive completion is converted with uib_wc_take_rx_loan(). UIB_RX_LOAN_COPIED
has one contiguous `data` range. UIB_RX_LOAN_DIRECT has one or more `segments`;
`data` is also set when there is exactly one segment. The application must call
uib_release_rx_loan(), or use uib_repost_rx_loan() to recycle the same fallback
registration without allocation, pinning, or MR-table work.

The tcp-devmem backend optionally registers a TCP io_uring ZCRX arena. Set
UIB_HOST_ZC_IFACE to a header/data-split interface, UIB_HOST_ZC_QUEUE to the
flow-steered RX queue, and UIB_HOST_ZC=auto or required. The kernel currently
requires NIC header/data split, RSS separation, flow steering, and
`CAP_NET_ADMIN` to bind the queue. `auto` preserves the copied TCP fallback;
`required` rejects the context if the queue cannot be registered.

When a ZCRX completion contains complete length-prefixed UIB frames, their
payloads are loaned directly. Coalesced frames share a reference-counted arena
buffer, and fragmented RoCEv2/UET SENDs become an iovec loan. If TCP splits a
UIB frame across ZCRX completions, UIB copies that frame into the loan's
preallocated fallback because a single contiguous frame header must be parsed
before QP demultiplexing. Standard verbs receives remain copied: their ABI
requires data at the already posted SGE, while the kernel chooses a ZCRX arena
address before UIB parses the packet.

Each loan preallocates between 4 and 1024 segment entries, sized at one entry
per 256 bytes of capacity. A receive fragmented more finely than that limit
switches to the same preallocated copied fallback instead of allocating on the
receive fast path.

`make host-zc-benchmark` prints actual nanoseconds per ordinary receive
post/completion and recycled loan. `tests/host_zc_e2e_test` transfers 8 KiB
messages through RoCEv2, UET, and dummy; `tests/protocol_test` verifies direct
single- and multi-segment ownership and delayed recycling. On systems without
a suitable ZCRX kernel/NIC, `tests/host_zc_test` reports the runtime probe as a
skip while still validating the compile-time ABI and fallback behavior.

One five-run local sample on Linux 6.12.100 and an AMD Ryzen 9 3950X produced:

| Control path | Median ns/op | Observed ns/op range |
|---|---:|---:|
| Ordinary post, complete, poll | 47.00 | 40.67-55.08 |
| Provider loan repost, complete, poll | 56.35 | 53.61-63.96 |
| Paired loan premium | 6.83 | 1.28-23.29 |

These are actual control-path values, not a claimed payload-throughput gain.
The available kernel is older than io_uring ZCRX, so the runtime ZCRX probe
skipped and no kernel-to-application copy-avoidance speedup is reported. A
ZCRX-capable NIC/kernel measurement must be made on the target hardware.

A paired five-run provider receive-WR translation comparison against commit
60fd4c01bd0a measured a 291.785 ns median with the host-loan implementation
and 287.509 ns without it (1.49% slower). Ordinary verbs therefore remain near
the previous fast path, while only applications opting into provider loans pay
the ownership cost shown above.

UIB_ZEROCOPY=auto uses a 10 KiB threshold, matching the kernel guidance that
small sends rarely recover the completion-processing cost. `on` and `required`
use a 1 KiB minimum so TCP protocol ACKs cannot stall a receive worker while
waiting for a delayed zero-copy completion. `required` rejects a socket when
SO_ZEROCOPY is unavailable. It cannot force the kernel or driver to avoid a
deferred copy; applications must inspect deferred_copies. Set
UIB_ZEROCOPY_THRESHOLD explicitly to change either threshold.

Benchmarking
------------

`make benchmark-all` runs rocev2, uet and dummy over UDP, framed TCP and AF_XDP
where available, with host and DMA-BUF memory and copied/zero-copy modes. It
records bandwidth, operation rate, wall/user/system/total CPU time, CPU
utilization, latency percentiles, zero-copy completions and deferred copies.
It produces:

  * results.csv: every successful raw run;
  * zerocopy-comparison.csv: percentage change from copied mode;
  * dmabuf-comparison.csv: percentage change from host memory; and
  * skips.txt: unavailable hardware/capability combinations.

`make benchmark-report` runs that matrix and additionally creates
`benchmark.txt`, an aligned ASCII report, and `benchmark.md`, a GitHub-ready
Markdown report. Both include actual values and percentage changes. Every row
shows its actual operation window and has unpinned and CPU-pinned variants;
`UIB_BENCH_PIN_CPU` overrides the first CPU allowed by the current cpuset. The
dummy UDP protocol has ordering but intentionally has no ACK-based flow
control, so the matrix limits its burst to 16 operations. The matrix also
measures 1500-, 4096-, and 8192-byte path MTUs and keeps comparisons within the
same MTU. RoCEv2 rows at 8192 bytes are reported as unsupported because the
protocol's maximum path MTU is 4096 bytes. Set `UIB_BENCH_OUTPUT` to select the
report directory. Unexpected matrix failures make the target fail.

Results are measurements, not an assertion that zero-copy is faster. Message
size, NIC, IOMMU, exporter, NUMA placement and deferred-copy behavior determine
whether it wins. docs/benchmark-results.md contains a complete checked-in local
run covering every available protocol, transport, memory and send mode.

Use `make benchmark-local` for the complete self-contained matrix. For
cross-host measurements, run examples/stress_bench with UIB_BENCH_ROLE=server
on the receiver and UIB_BENCH_ROLE=client plus UIB_BENCH_PEER on the sender.
examples/README.md lists every environment variable and complete commands.
