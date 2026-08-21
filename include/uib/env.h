/* SPDX-License-Identifier: Apache-2.0 */
#ifndef UIB_ENV_H
#define UIB_ENV_H

/**
 * DOC: UIB_PROTOCOL
 * @uib-env: Library and transparent provider
 * @default: `rocev2`
 * @values: `rocev2`, `uet`, or `dummy`
 *
 * Select the wire protocol used by newly opened contexts.
 *
 * Example:
 * UIB_PROTOCOL=uet ./examples/direct_traffic uet
 */
#define UIB_ENV_PROTOCOL "UIB_PROTOCOL"

/**
 * DOC: UIB_BIND_IP
 * @uib-env: Library and transparent provider
 * @default: `0.0.0.0`
 * @values: An IPv4 address assigned to the local host.
 *
 * Select the local IPv4 address used by the verbs provider.
 *
 * Example:
 * UIB_BIND_IP=192.0.2.2 LD_PRELOAD=$PWD/libuibv.so app
 */
#define UIB_ENV_BIND_IP "UIB_BIND_IP"

/**
 * DOC: UIB_BIND_PORT
 * @uib-env: Library and transparent provider
 * @default: 4791 for RoCEv2 and 5432 for UET.
 * @values: `1` through `65535`
 *
 * Override the protocol's local UDP or TCP service port.
 *
 * Example:
 * UIB_BIND_PORT=5432 LD_PRELOAD=$PWD/libuibv.so app
 */
#define UIB_ENV_BIND_PORT "UIB_BIND_PORT"

/**
 * DOC: UIB_TRANSPORT_MODE
 * @uib-env: Library and transparent provider
 * @default: `udp-io-uring`
 * @values: `udp-io-uring`, `udp-sync`, `udp-zc-afxdp`, or `tcp-devmem`
 *
 * Select the packet transport independently of the wire protocol.
 *
 * Example:
 * UIB_TRANSPORT_MODE=udp-sync ./examples/direct_traffic rocev2
 */
#define UIB_ENV_TRANSPORT_MODE "UIB_TRANSPORT_MODE"

/**
 * DOC: UIB_UDP_MODE
 * @uib-env: Library and transparent provider
 * @default: `socket`
 * @values: `socket` or `raw`; raw requires `CAP_NET_RAW`.
 *
 * Select ordinary UDP sockets or raw RoCEv2 interoperability.
 *
 * Example:
 * UIB_UDP_MODE=raw LD_PRELOAD=$PWD/libuibv.so ibv_rc_pingpong
 */
#define UIB_ENV_UDP_MODE "UIB_UDP_MODE"

/**
 * DOC: UIB_URING_RX_MODE
 * @uib-env: Library and transparent provider
 * @default: `auto`
 * @values: `auto`, `multishot`, or `batch`
 *
 * Select multishot receive or the portable batched fallback.
 *
 * Example:
 * UIB_URING_RX_MODE=batch ./tests/loopback
 */
#define UIB_ENV_URING_RX_MODE "UIB_URING_RX_MODE"

/**
 * DOC: UIB_URING_RX_BATCH
 * @uib-env: Library and transparent provider
 * @default: `64`
 * @values: `1` through `256`
 *
 * Set RX CQEs reaped per worker pass and batched RX depth.
 *
 * Example:
 * UIB_URING_RX_BATCH=128 ./examples/stress_bench
 */
#define UIB_ENV_URING_RX_BATCH "UIB_URING_RX_BATCH"

/**
 * DOC: UIB_URING_TX_BATCH
 * @uib-env: Library and transparent provider
 * @default: `64`
 * @values: `1` through `256`
 *
 * Limit send SQEs submitted by the io_uring worker per pass.
 *
 * Example:
 * UIB_URING_TX_BATCH=128 ./examples/stress_bench
 */
#define UIB_ENV_URING_TX_BATCH "UIB_URING_TX_BATCH"

/**
 * DOC: UIB_UDP_GSO
 * @uib-env: UDP transports
 * @default: `auto`
 * @values: `auto`, `off`, or `on`
 *
 * Combine equal-sized RDMA packets into UDP GSO sends. `on` fails context
 * creation when the kernel does not support UDP GSO; `auto` falls back.
 *
 * Example:
 * UIB_UDP_GSO=off ./examples/stress_bench --protocol rocev2
 */
#define UIB_ENV_UDP_GSO "UIB_UDP_GSO"

/**
 * DOC: UIB_URING_SQPOLL
 * @uib-env: Library and transparent provider
 * @default: `off`
 * @values: `off` or `on`
 *
 * Enable the optional io_uring kernel submission polling thread.
 *
 * Example:
 * UIB_URING_SQPOLL=on ./examples/stress_bench
 */
#define UIB_ENV_URING_SQPOLL "UIB_URING_SQPOLL"

/**
 * DOC: UIB_URING_SQPOLL_IDLE
 * @uib-env: Library and transparent provider
 * @default: `2000`
 * @values: A positive number of milliseconds.
 *
 * Set how long an idle io_uring SQPOLL thread keeps polling.
 *
 * Example:
 * UIB_URING_SQPOLL=on UIB_URING_SQPOLL_IDLE=100 ./examples/stress_bench
 */
#define UIB_ENV_URING_SQPOLL_IDLE "UIB_URING_SQPOLL_IDLE"

/**
 * DOC: UIB_URING_SQPOLL_CPU
 * @uib-env: Library and transparent provider
 * @default: Unset.
 * @values: An online CPU number.
 *
 * Pin the optional io_uring SQPOLL kernel thread to one CPU.
 *
 * Example:
 * UIB_URING_SQPOLL=on UIB_URING_SQPOLL_CPU=3 ./examples/stress_bench
 */
#define UIB_ENV_URING_SQPOLL_CPU "UIB_URING_SQPOLL_CPU"

/**
 * DOC: UIB_ZEROCOPY
 * @uib-env: Library and transparent provider
 * @default: `auto`
 * @values: `off`, `auto`, `on`, or `required`
 *
 * Control optional socket and AF_XDP transmit zero-copy.
 *
 * Example:
 * UIB_ZEROCOPY=required ./examples/stress_bench
 */
#define UIB_ENV_ZEROCOPY "UIB_ZEROCOPY"

/**
 * DOC: UIB_ZEROCOPY_THRESHOLD
 * @uib-env: Library and transparent provider
 * @default: 10240 in auto mode; 1024 in on or required mode.
 * @values: A non-negative byte count.
 *
 * Override the minimum packet size used for socket zero-copy.
 *
 * Example:
 * UIB_ZEROCOPY_THRESHOLD=4096 ./examples/stress_bench
 */
#define UIB_ENV_ZEROCOPY_THRESHOLD "UIB_ZEROCOPY_THRESHOLD"

/**
 * DOC: UIB_AFXDP_IFACE
 * @uib-env: AF_XDP transport
 * @default: Unset; AF_XDP is unavailable until it is set.
 * @values: A local network-interface name.
 *
 * Select the netdev to which the AF_XDP socket is attached.
 *
 * Example:
 * UIB_AFXDP_IFACE=eth0 UIB_TRANSPORT_MODE=udp-zc-afxdp app
 */
#define UIB_ENV_AFXDP_IFACE "UIB_AFXDP_IFACE"

/**
 * DOC: UIB_AFXDP_QUEUE
 * @uib-env: AF_XDP transport
 * @default: `0`
 * @values: A non-negative queue identifier.
 *
 * Select the hardware RX/TX queue used by the AF_XDP socket.
 *
 * Example:
 * UIB_AFXDP_QUEUE=1 UIB_TRANSPORT_MODE=udp-zc-afxdp app
 */
#define UIB_ENV_AFXDP_QUEUE "UIB_AFXDP_QUEUE"

/**
 * DOC: UIB_AFXDP_PEER_MAC
 * @uib-env: AF_XDP transport
 * @default: Unset; it is mandatory for AF_XDP.
 * @values: A six-octet colon-separated MAC address.
 *
 * Set the fixed next-hop destination MAC for emitted frames.
 *
 * Example:
 * UIB_AFXDP_PEER_MAC=02:00:00:00:00:02 app
 */
#define UIB_ENV_AFXDP_PEER_MAC "UIB_AFXDP_PEER_MAC"

/**
 * DOC: UIB_DEVMEM_IFACE
 * @uib-env: TCP devmem transport
 * @default: Unset; device-memory queue binding is disabled.
 * @values: A local network-interface name.
 *
 * Select the netdev whose RX queue receives DMA-BUF binding.
 *
 * Example:
 * UIB_DEVMEM_IFACE=eth0 UIB_TRANSPORT_MODE=tcp-devmem app
 */
#define UIB_ENV_DEVMEM_IFACE "UIB_DEVMEM_IFACE"

/**
 * DOC: UIB_DEVMEM_QUEUE
 * @uib-env: TCP devmem transport
 * @default: `0`
 * @values: A non-negative queue identifier.
 *
 * Select the netdev RX queue used for DMA-BUF binding.
 *
 * Example:
 * UIB_DEVMEM_QUEUE=1 UIB_TRANSPORT_MODE=tcp-devmem app
 */
#define UIB_ENV_DEVMEM_QUEUE "UIB_DEVMEM_QUEUE"

#define UIB_ENV_UET_CC "UIB_UET_CC"

/**
 * DOC: UIB_UET_NSCC_MAX_WND
 * @uib-env: Ultra Ethernet
 * @default: The maximum window calculated by NSCC configuration.
 * @values: A byte count no smaller than the active MTU.
 *
 * Cap the initial NSCC congestion window for experiments.
 *
 * Example:
 * UIB_PROTOCOL=uet UIB_UET_NSCC_MAX_WND=65536 app
 */
#define UIB_ENV_UET_NSCC_MAX_WND "UIB_UET_NSCC_MAX_WND"

/**
 * DOC: UIB_UET_OPTIMIZED
 * @uib-env: Ultra Ethernet
 * @default: `auto`
 * @values: `off`, `auto`, `small`, or `medium`
 *
 * Select the SES request-header optimization policy.
 *
 * Example:
 * UIB_PROTOCOL=uet UIB_UET_OPTIMIZED=medium app
 */
#define UIB_ENV_UET_OPTIMIZED "UIB_UET_OPTIMIZED"

/**
 * DOC: UIB_UET_MULTIPATH
 * @uib-env: Ultra Ethernet
 * @default: `off`
 * @values: `off`, `oblivious`, or `path-aware`
 *
 * Select entropy multipathing and path-failure tracking mode.
 *
 * Example:
 * UIB_PROTOCOL=uet UIB_UET_MULTIPATH=path-aware app
 */
#define UIB_ENV_UET_MULTIPATH "UIB_UET_MULTIPATH"

/**
 * DOC: UIB_UET_DEBUG
 * @uib-env: Ultra Ethernet
 * @default: Unset and disabled.
 * @values: Any non-empty value enables logging.
 *
 * Enable diagnostic UET packet and completion logging.
 *
 * Example:
 * UIB_PROTOCOL=uet UIB_UET_DEBUG=1 ./tests/loopback
 */
#define UIB_ENV_UET_DEBUG "UIB_UET_DEBUG"

/**
 * DOC: UIB_BENCH_PROTOCOL
 * @uib-env: Stress benchmark executable
 * @default: `uet`
 * @values: `rocev2`, `uet`, or `dummy`
 *
 * Select the protocol when `--protocol` is not supplied.
 *
 * Example:
 * UIB_BENCH_PROTOCOL=rocev2 ./examples/stress_bench
 */
#define UIB_ENV_BENCH_PROTOCOL "UIB_BENCH_PROTOCOL"

/**
 * DOC: UIB_BENCH_MEMORY
 * @uib-env: Stress benchmark executable
 * @default: `host`
 * @values: `host`, `dmabuf`, or `gpu-dmabuf`
 *
 * Select the memory kind used for benchmark payloads.
 *
 * Example:
 * UIB_BENCH_MEMORY=dmabuf ./examples/stress_bench
 */
#define UIB_ENV_BENCH_MEMORY "UIB_BENCH_MEMORY"

/**
 * DOC: UIB_BENCH_ROLE
 * @uib-env: Stress benchmark executable
 * @default: `local`
 * @values: `local`, `server`, or `client`
 *
 * Select local or remote endpoint operation.
 *
 * Example:
 * UIB_BENCH_ROLE=server ./examples/stress_bench
 */
#define UIB_ENV_BENCH_ROLE "UIB_BENCH_ROLE"

/**
 * DOC: UIB_BENCH_BIND
 * @uib-env: Stress benchmark executable
 * @default: A role-specific loopback address.
 * @values: An IPv4 address assigned to the local host.
 *
 * Set the local IPv4 address in remote benchmark mode.
 *
 * Example:
 * UIB_BENCH_BIND=192.0.2.1 UIB_BENCH_ROLE=server ./examples/stress_bench
 */
#define UIB_ENV_BENCH_BIND "UIB_BENCH_BIND"

/**
 * DOC: UIB_BENCH_PEER
 * @uib-env: Stress benchmark executable
 * @default: Unset; it is required in client mode.
 * @values: A reachable IPv4 address.
 *
 * Set the remote server address used by a benchmark client.
 *
 * Example:
 * UIB_BENCH_PEER=192.0.2.1 UIB_BENCH_ROLE=client ./examples/stress_bench
 */
#define UIB_ENV_BENCH_PEER "UIB_BENCH_PEER"

/**
 * DOC: UIB_BENCH_ITERATIONS
 * @uib-env: Stress benchmark executable
 * @default: `10000`
 * @values: A positive 32-bit integer.
 *
 * Set the number of measured RDMA operations.
 *
 * Example:
 * UIB_BENCH_ITERATIONS=100000 ./examples/stress_bench
 */
#define UIB_ENV_BENCH_ITERATIONS "UIB_BENCH_ITERATIONS"

/**
 * DOC: UIB_BENCH_SIZE
 * @uib-env: Stress benchmark executable
 * @default: 4096 bytes; the full matrix selects 32768.
 * @values: A positive 32-bit byte count.
 *
 * Set the payload size of each measured operation.
 *
 * Example:
 * UIB_BENCH_SIZE=65536 ./examples/stress_bench
 */
#define UIB_ENV_BENCH_SIZE "UIB_BENCH_SIZE"

/**
 * DOC: UIB_BENCH_WINDOW
 * @uib-env: Stress benchmark executable
 * @default: `64`
 * @values: A positive queue-window size.
 *
 * Set the maximum number of operations kept in flight.
 *
 * Example:
 * UIB_BENCH_WINDOW=128 ./examples/stress_bench
 */
#define UIB_ENV_BENCH_WINDOW "UIB_BENCH_WINDOW"

/**
 * DOC: UIB_BENCH_MTU
 * @uib-env: Stress benchmark executable
 * @default: `1500`
 * @values: A positive byte count; RoCEv2 is limited to 4096.
 *
 * Set the protocol path MTU used by benchmark QPs.
 *
 * Example:
 * UIB_BENCH_MTU=4096 ./examples/stress_bench
 */
#define UIB_ENV_BENCH_MTU "UIB_BENCH_MTU"

/**
 * DOC: UIB_BENCH_CONTROL_PORT
 * @uib-env: Stress benchmark executable
 * @default: `19190`
 * @values: `1` through `65535`
 *
 * Select the TCP control-plane port for remote mode.
 *
 * Example:
 * UIB_BENCH_CONTROL_PORT=20190 ./examples/stress_bench
 */
#define UIB_ENV_BENCH_CONTROL_PORT "UIB_BENCH_CONTROL_PORT"

/**
 * DOC: UIB_BENCH_DATA_PORT
 * @uib-env: Stress benchmark executable
 * @default: `19100`
 * @values: `1` through `65535`
 *
 * Select the UIB data-plane port for remote mode.
 *
 * Example:
 * UIB_BENCH_DATA_PORT=20100 ./examples/stress_bench
 */
#define UIB_ENV_BENCH_DATA_PORT "UIB_BENCH_DATA_PORT"

/**
 * DOC: UIB_BENCH_DMABUF_FD
 * @uib-env: Stress benchmark executable
 * @default: Unset; required by `gpu-dmabuf` memory mode.
 * @values: A valid inherited file-descriptor number.
 *
 * Supply an already-open GPU DMA-BUF descriptor to import.
 *
 * Example:
 * UIB_BENCH_DMABUF_FD=9 ./examples/stress_bench --memory gpu-dmabuf
 */
#define UIB_ENV_BENCH_DMABUF_FD "UIB_BENCH_DMABUF_FD"

/**
 * DOC: UIB_BENCH_DMABUF_OFFSET
 * @uib-env: Stress benchmark executable
 * @default: `0`
 * @values: A non-negative byte offset.
 *
 * Set the byte offset into an imported GPU DMA-BUF.
 *
 * Example:
 * UIB_BENCH_DMABUF_OFFSET=4096 UIB_BENCH_DMABUF_FD=9 app
 */
#define UIB_ENV_BENCH_DMABUF_OFFSET "UIB_BENCH_DMABUF_OFFSET"

/**
 * DOC: UIB_BENCH_PROTOCOLS
 * @uib-env: Complete benchmark matrix
 * @default: `rocev2 uet dummy`
 * @values: A space-separated subset of supported protocols.
 *
 * Select the protocols traversed by the matrix runner.
 *
 * Example:
 * UIB_BENCH_PROTOCOLS='rocev2 uet' make benchmark-all
 */
#define UIB_ENV_BENCH_PROTOCOLS "UIB_BENCH_PROTOCOLS"

/**
 * DOC: UIB_BENCH_MEMORY_MODES
 * @uib-env: Complete benchmark matrix
 * @default: `host dmabuf`
 * @values: A space-separated subset of `host` and `dmabuf`.
 *
 * Select the memory kinds traversed by the matrix runner.
 *
 * Example:
 * UIB_BENCH_MEMORY_MODES=host make benchmark-all
 */
#define UIB_ENV_BENCH_MEMORY_MODES "UIB_BENCH_MEMORY_MODES"

/**
 * DOC: UIB_BENCH_AFFINITIES
 * @uib-env: Complete benchmark matrix
 * @default: `unpinned pinned`
 * @values: A space-separated subset of `unpinned` and `pinned`.
 *
 * Select pinned and unpinned process variants.
 *
 * Example:
 * UIB_BENCH_AFFINITIES=pinned make benchmark-all
 */
#define UIB_ENV_BENCH_AFFINITIES "UIB_BENCH_AFFINITIES"

/**
 * DOC: UIB_BENCH_MTUS
 * @uib-env: Complete benchmark matrix
 * @default: `1500 4096 8192`
 * @values: A space-separated list of positive byte counts.
 *
 * Select protocol MTUs traversed by the matrix runner.
 *
 * Example:
 * UIB_BENCH_MTUS='1500 4096' make benchmark-all
 */
#define UIB_ENV_BENCH_MTUS "UIB_BENCH_MTUS"

#define UIB_ENV_BENCH_PIN_CPU "UIB_BENCH_PIN_CPU"

/**
 * DOC: UIB_BENCH_OUTPUT
 * @uib-env: Complete benchmark matrix
 * @default: A timestamped directory below `/tmp`.
 * @values: A writable directory path.
 *
 * Select the directory receiving CSV, text, and Markdown output.
 *
 * Example:
 * UIB_BENCH_OUTPUT=/tmp/uib-results make benchmark-all
 */
#define UIB_ENV_BENCH_OUTPUT "UIB_BENCH_OUTPUT"

/**
 * DOC: UIB_BENCH_DOCUMENT
 * @uib-env: Complete benchmark matrix
 * @default: `docs/benchmark-results.md`
 * @values: A writable Markdown file path.
 *
 * Select the benchmark reference document to update.
 *
 * Example:
 * UIB_BENCH_DOCUMENT=/tmp/results.md make benchmark-doc
 */
#define UIB_ENV_BENCH_DOCUMENT "UIB_BENCH_DOCUMENT"

/**
 * DOC: UIB_BENCH_SYNC_DOCUMENT
 * @uib-env: Complete benchmark matrix
 * @default: `docs/sync-async-benchmark.md`
 * @values: A writable Markdown file path.
 *
 * Select the sync/async comparison document to update.
 *
 * Example:
 * UIB_BENCH_SYNC_DOCUMENT=/tmp/sync.md make benchmark-doc
 */
#define UIB_ENV_BENCH_SYNC_DOCUMENT "UIB_BENCH_SYNC_DOCUMENT"

/**
 * DOC: UIB_BENCH_REUSE
 * @uib-env: Complete benchmark matrix
 * @default: `0`
 * @values: `0` or `1`
 *
 * Reuse CSV files in the output directory instead of rerunning.
 *
 * Example:
 * UIB_BENCH_REUSE=1 UIB_BENCH_OUTPUT=/tmp/run make benchmark-doc
 */
#define UIB_ENV_BENCH_REUSE "UIB_BENCH_REUSE"

/**
 * DOC: UIB_BENCH_MAX_REGRESSION_PERCENT
 * @uib-env: Complete benchmark matrix
 * @default: `5` percent
 * @values: A non-negative percentage.
 *
 * Set the maximum aggregate UDP regression accepted by docs.
 *
 * Example:
 * UIB_BENCH_MAX_REGRESSION_PERCENT=3 make benchmark-doc
 */
#define UIB_ENV_BENCH_MAX_REGRESSION_PERCENT "UIB_BENCH_MAX_REGRESSION_PERCENT"

/**
 * DOC: UIB_NAMESPACE_KEEP_ON_FAILURE
 * @uib-env: Namespace benchmark harness
 * @default: `0`
 * @values: `0` or `1`
 *
 * Keep failed namespaces, interfaces, processes, and logs.
 *
 * Example:
 * UIB_NAMESPACE_KEEP_ON_FAILURE=1 make namespace-benchmark-test
 */
#define UIB_ENV_NAMESPACE_KEEP_ON_FAILURE "UIB_NAMESPACE_KEEP_ON_FAILURE"

/**
 * DOC: UIB_NAMESPACE_REQUIRED
 * @uib-env: Namespace benchmark harness
 * @default: `0`
 * @values: `0` or `1`
 *
 * Fail instead of skipping when namespace prerequisites are absent.
 *
 * Example:
 * UIB_NAMESPACE_REQUIRED=1 make namespace-benchmark-test
 */
#define UIB_ENV_NAMESPACE_REQUIRED "UIB_NAMESPACE_REQUIRED"

/**
 * DOC: UIB_NAMESPACE_AFFINITIES
 * @uib-env: Namespace benchmark harness
 * @default: `unpinned pinned`
 * @values: A space-separated subset of `unpinned` and `pinned`.
 *
 * Select pinned and unpinned namespace benchmark variants.
 *
 * Example:
 * UIB_NAMESPACE_AFFINITIES=pinned make namespace-benchmark-test
 */
#define UIB_ENV_NAMESPACE_AFFINITIES "UIB_NAMESPACE_AFFINITIES"

/**
 * DOC: UIB_NAMESPACE_MTUS
 * @uib-env: Namespace benchmark harness
 * @default: `1500 4096 8192`
 * @values: A space-separated list of positive byte counts.
 *
 * Select path MTUs for namespace benchmark endpoints.
 *
 * Example:
 * UIB_NAMESPACE_MTUS='1500 4096' make namespace-benchmark-test
 */
#define UIB_ENV_NAMESPACE_MTUS "UIB_NAMESPACE_MTUS"

/**
 * DOC: UIB_NAMESPACE_PROTOCOLS
 * @uib-env: Namespace benchmark harness
 * @default: `rocev2 uet dummy`
 * @values: A space-separated subset of supported protocols.
 *
 * Select protocols for the namespace benchmark matrix.
 *
 * Example:
 * UIB_NAMESPACE_PROTOCOLS=uet make namespace-benchmark-test
 */
#define UIB_ENV_NAMESPACE_PROTOCOLS "UIB_NAMESPACE_PROTOCOLS"

/**
 * DOC: UIB_NAMESPACE_MEMORY
 * @uib-env: Namespace benchmark harness
 * @default: `host dmabuf`
 * @values: A space-separated subset of `host` and `dmabuf`.
 *
 * Select host and DMA-BUF namespace benchmark variants.
 *
 * Example:
 * UIB_NAMESPACE_MEMORY=host make namespace-benchmark-test
 */
#define UIB_ENV_NAMESPACE_MEMORY "UIB_NAMESPACE_MEMORY"

/**
 * DOC: UIB_TEST_PROTOCOL
 * @uib-env: Test and scaling harnesses
 * @default: `rocev2`
 * @values: `rocev2`, `uet`, or `dummy`
 *
 * Select the protocol used by standard ibverbs tool tests.
 *
 * Example:
 * UIB_TEST_PROTOCOL=uet make uet-tools-test
 */
#define UIB_ENV_TEST_PROTOCOL "UIB_TEST_PROTOCOL"

/**
 * DOC: UIB_TEST_PORT
 * @uib-env: Test and scaling harnesses
 * @default: `4791`
 * @values: `1` through `65535`
 *
 * Select the protocol service port used by standard tool tests.
 *
 * Example:
 * UIB_TEST_PORT=5432 UIB_TEST_PROTOCOL=uet make uib-tools-test
 */
#define UIB_ENV_TEST_PORT "UIB_TEST_PORT"

/**
 * DOC: UIB_TEST_ITERS
 * @uib-env: Test and scaling harnesses
 * @default: `64`
 * @values: A positive integer.
 *
 * Set the operation count used by standard tool tests.
 *
 * Example:
 * UIB_TEST_ITERS=256 make uib-tools-test
 */
#define UIB_ENV_TEST_ITERS "UIB_TEST_ITERS"

/**
 * DOC: UIB_TEST_DROP_TX
 * @uib-env: Test and scaling harnesses
 * @default: Unset and disabled.
 * @values: Any non-empty value drops one eligible packet.
 *
 * Inject one transmit loss to exercise protocol recovery.
 *
 * Example:
 * UIB_TEST_DROP_TX=1 ./tests/loopback
 */
#define UIB_ENV_TEST_DROP_TX "UIB_TEST_DROP_TX"

/**
 * DOC: UIB_TEST_GPU_DMABUF_FD
 * @uib-env: Test and scaling harnesses
 * @default: Unset; the external GPU DMA-BUF test skips.
 * @values: A valid inherited file-descriptor number.
 *
 * Supply an external GPU DMA-BUF descriptor to the import test.
 *
 * Example:
 * UIB_TEST_GPU_DMABUF_FD=9 ./tests/external_dmabuf_test
 */
#define UIB_ENV_TEST_GPU_DMABUF_FD "UIB_TEST_GPU_DMABUF_FD"

/**
 * DOC: UIB_TEST_GPU_DMABUF_OFFSET
 * @uib-env: Test and scaling harnesses
 * @default: `0`
 * @values: A non-negative byte offset.
 *
 * Set the starting offset in an external GPU DMA-BUF.
 *
 * Example:
 * UIB_TEST_GPU_DMABUF_OFFSET=4096 ./tests/external_dmabuf_test
 */
#define UIB_ENV_TEST_GPU_DMABUF_OFFSET "UIB_TEST_GPU_DMABUF_OFFSET"

/**
 * DOC: UIB_TEST_JOBS
 * @uib-env: Test and scaling harnesses
 * @default: `4`
 * @values: A positive make job count.
 *
 * Set parallel jobs for optional-feature rebuild tests.
 *
 * Example:
 * UIB_TEST_JOBS=16 make optional-features-test
 */
#define UIB_ENV_TEST_JOBS "UIB_TEST_JOBS"

/**
 * DOC: UIB_SCALE_THREADS
 * @uib-env: Test and scaling harnesses
 * @default: `4`
 * @values: A positive 32-bit integer.
 *
 * Set worker threads in the QP lookup scaling benchmark.
 *
 * Example:
 * UIB_SCALE_THREADS=8 make scaling-benchmark
 */
#define UIB_ENV_SCALE_THREADS "UIB_SCALE_THREADS"

/**
 * DOC: UIB_SCALE_LOOKUPS
 * @uib-env: Test and scaling harnesses
 * @default: `1000000`
 * @values: A positive 32-bit integer.
 *
 * Set lookups per worker in the QP scaling benchmark.
 *
 * Example:
 * UIB_SCALE_LOOKUPS=5000000 make scaling-benchmark
 */
#define UIB_ENV_SCALE_LOOKUPS "UIB_SCALE_LOOKUPS"

/**
 * DOC: UIB_SCALE_TRAFFIC_ITERS
 * @uib-env: Test and scaling harnesses
 * @default: `30000`
 * @values: A positive integer.
 *
 * Set traffic iterations for scaling protocol measurements.
 *
 * Example:
 * UIB_SCALE_TRAFFIC_ITERS=100000 make scaling-benchmark
 */
#define UIB_ENV_SCALE_TRAFFIC_ITERS "UIB_SCALE_TRAFFIC_ITERS"

/**
 * DOC: UIB_MR_SCALE_LOOKUPS
 * @uib-env: Test and scaling harnesses
 * @default: `1000000`
 * @values: A positive 32-bit integer.
 *
 * Set lookups in the registered-memory scaling benchmark.
 *
 * Example:
 * UIB_MR_SCALE_LOOKUPS=5000000 make scaling-benchmark
 */
#define UIB_ENV_MR_SCALE_LOOKUPS "UIB_MR_SCALE_LOOKUPS"

/**
 * DOC: UIB_PROVIDER_CONTEXTS
 * @uib-env: Test and scaling harnesses
 * @default: `64`
 * @values: A positive 32-bit integer.
 *
 * Set contexts created by the provider dispatch benchmark.
 *
 * Example:
 * UIB_PROVIDER_CONTEXTS=128 make scaling-benchmark
 */
#define UIB_ENV_PROVIDER_CONTEXTS "UIB_PROVIDER_CONTEXTS"

/**
 * DOC: UIB_PROVIDER_LOOKUPS
 * @uib-env: Test and scaling harnesses
 * @default: `1000000`
 * @values: A positive 32-bit integer.
 *
 * Set calls made by the provider dispatch benchmark.
 *
 * Example:
 * UIB_PROVIDER_LOOKUPS=5000000 make scaling-benchmark
 */
#define UIB_ENV_PROVIDER_LOOKUPS "UIB_PROVIDER_LOOKUPS"

/**
 * DOC: UIB_PROVIDER_WR_ITERS
 * @uib-env: Test and scaling harnesses
 * @default: `100000`
 * @values: A positive 32-bit integer.
 *
 * Set calls made by the provider WR translation benchmark.
 *
 * Example:
 * UIB_PROVIDER_WR_ITERS=1000000 make scaling-benchmark
 */
#define UIB_ENV_PROVIDER_WR_ITERS "UIB_PROVIDER_WR_ITERS"

#endif
