UIB examples
============

Build the library and examples from the repository root:

    make examples

The programs use loopback addresses and create both endpoints themselves. The
default transport uses ordinary UDP sockets and works without CAP_NET_RAW,
including in rootless containers. RXE interoperability alone selects raw UDP.

direct_traffic
--------------

This is a compact walkthrough of the native UIB API. It creates contexts,
protection domains, completion queues, memory regions and connected QPs; posts
SEND, WRITE, READ, fetch-add and compare-swap work requests; and verifies the
actual bytes and atomic values. For UET it also demonstrates SES identity and
NSCC configuration/query APIs.

    ./examples/direct_traffic uet
    ./examples/direct_traffic rocev2
    ./examples/direct_traffic dummy

Append `dmabuf` to use mmap-backed DMA-BUF MRs for every local and remote
buffer, including SEND, WRITE, READ and UET atomics:

    ./examples/direct_traffic uet dmabuf

The dummy protocol intentionally implements SEND only. RoCEv2 exercises SEND,
WRITE and READ. UET exercises all of those plus the two standard verbs atomic
operations.

verbs_traffic
-------------

This example performs the same traffic through standard libibverbs calls. It
links to libuibv, finds the synthetic uib0 device, and uses ibv_post_send(),
ibv_post_recv() and ibv_poll_cq() exactly like another verbs application.

    ./examples/verbs_traffic uet

The source and target live in separate contexts bound to 127.0.0.1 and
127.0.0.2. Real multi-process libibverbs coverage is provided by the
uib-tools-test and uet-tools-test targets, which run the standard
ibv_rc_pingpong program in separate network namespaces.

stress_bench
------------

The stress benchmark keeps a configurable window of SEND operations in flight,
checks every received message, and supports every UIB wire protocol without
changing the workload:

    ./examples/stress_bench --protocol uet --iterations 100000 \
      --size 4096 --window 64

It reports payload bandwidth, operation rate, process/user/system CPU time,
CPU utilization, and min/average/p50/p95/p99/max completion latency. UET runs
also print the final NSCC congestion-window, inflight-byte and base-RTT state.
The protocol path MTU defaults to 1500 bytes; select another value with
`--mtu BYTES` or `UIB_BENCH_MTU`.

Select DMA-BUF memory with `--memory dmabuf`. Select copied or socket
zero-copy transport with UIB_ZEROCOPY=off or required. The stats distinguish
zero-copy requests from kernel-deferred copies.

For io_uring tuning, UIB_URING_RX_BATCH and UIB_URING_TX_BATCH select the RX
completion/one-shot depth and TX submission batch respectively. Both default
to 64 and accept values from 1 through 256.

Run the complete protocol/transport/memory matrix and generate CSV comparisons:

    make benchmark-local

The output directory contains raw results, copied-vs-zero-copy percentage
changes, DMA-BUF-vs-host percentage changes, and explicit skips for unavailable
AF_XDP hardware. `make benchmark-smoke` runs the same matrix with short inputs.

Use the reporting target to run the same complete matrix and also render aligned
plain text and GitHub Markdown from the resulting CSV files:

    make benchmark-report

Every combination is run both normally and with the process pinned to the first
CPU in the current cpuset. Set `UIB_BENCH_PIN_CPU` to choose another allowed
CPU. The reports include actual values and percentage changes for each
comparison, rather than percentages alone. Each combination is measured with
1500-, 4096-, and 8192-byte protocol path MTUs, and MTU is part of every
comparison key. RoCEv2's standard 4096-byte maximum makes its 8192-byte rows
explicit skips; UET and dummy exercise all three values.

The target writes `benchmark.txt`, `benchmark.md`, `results.csv`,
`zerocopy-comparison.csv`, `dmabuf-comparison.csv`, and `skips.txt` in a
timestamped directory under `/tmp`; unexpected failures are recorded in
`failures.txt` and make the target fail. Set
`UIB_BENCH_OUTPUT=/path/to/results` to choose a stable output directory.

Remote client/server benchmark
------------------------------

The same binary runs across hosts. The TCP control connection exchanges QP and
data-address metadata and batch barriers; it is not included in measured UIB
traffic. All settings have environment-variable forms, which is convenient for
containers, SSH launchers and orchestration.

On the receiving host (replace the addresses with routable interface addresses):

    UIB_BENCH_ROLE=server \
    UIB_BENCH_BIND=192.0.2.10 \
    UIB_BENCH_PROTOCOL=uet \
    UIB_BENCH_MEMORY=dmabuf \
    UIB_BENCH_ITERATIONS=100000 \
    UIB_BENCH_SIZE=32768 \
    UIB_BENCH_WINDOW=64 \
    UIB_BENCH_MTU=4096 \
    UIB_TRANSPORT_MODE=udp-io-uring \
    UIB_ZEROCOPY=required \
    ./examples/stress_bench

On the sending host:

    UIB_BENCH_ROLE=client \
    UIB_BENCH_BIND=192.0.2.11 \
    UIB_BENCH_PEER=192.0.2.10 \
    UIB_BENCH_PROTOCOL=uet \
    UIB_BENCH_MEMORY=dmabuf \
    UIB_BENCH_ITERATIONS=100000 \
    UIB_BENCH_SIZE=32768 \
    UIB_BENCH_WINDOW=64 \
    UIB_BENCH_MTU=4096 \
    UIB_TRANSPORT_MODE=udp-io-uring \
    UIB_ZEROCOPY=required \
    ./examples/stress_bench

Both sides must use the same protocol, iteration count, message size, window,
transport family, control port and compatible data ports. The handshake rejects
mismatched workload settings, including MTU. UIB_BENCH_CONTROL_PORT defaults to
19190 and UIB_BENCH_DATA_PORT to 19100. CLI equivalents are available through
--role, --bind, --peer, --mtu, --control-port and --data-port.

On a GPU host, an allocator/launcher can inherit a GPU-exported DMA-BUF fd into
the benchmark and set UIB_BENCH_MEMORY=gpu-dmabuf,
UIB_BENCH_DMABUF_FD=<number> and optionally UIB_BENCH_DMABUF_OFFSET=<bytes>.
This mode is remote-only because each host supplies its own fd. It currently
requires a CPU-mappable exporter; an opaque GPU fd is rejected explicitly.

`make remote-benchmark-test` launches separate client/server processes through
environment variables on 127.0.0.1/127.0.0.2 and covers every protocol over
UDP/TCP, host/DMA-BUF memory and copied/zero-copy modes.

`make namespace-benchmark-test` creates two isolated Linux network namespaces
joined only by a veth pair and runs that matrix between separate server/client
processes. It adds unpinned and CPU-pinned rows, thereby simulating two remote
container network stacks without requiring an OCI runtime. Its veth pair uses a
9000-byte link MTU and verifies 1500-, 4096-, and 8192-byte protocol MTUs. The
host target reports a skip when namespace capabilities are unavailable;
`guest-test` makes the test mandatory inside the privileged virtme-ng guest.

The test normally kills any remaining server, deletes both namespaces and their
veth pair, and removes its logs on success, failure, or a signal. To stop on the
first failure and preserve the namespaces, process and logs for inspection, use:

    UIB_NAMESPACE_KEEP_ON_FAILURE=1 make namespace-benchmark-test

Calling `tests/run_namespace_benchmark.sh --keep-on-failure` is equivalent.

Use `make examples-test` for short correctness runs over all protocols and
`make examples-stress-test` for short windowed traffic runs over all protocols.
