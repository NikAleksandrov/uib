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
 * DOC: UIB_ACTIVE_MTU
 * @uib-env: Transparent provider
 * @default: `auto`
 * @values: `auto`, `256`, `512`, `1024`, `2048`, or `4096`
 *
 * Set the active verbs port MTU advertised to applications. Auto selects the
 * largest verbs MTU with a 64-byte allowance below the interface MTU matched
 * by UIB_BIND_IP. Unknown interfaces and invalid values use 4096.
 *
 * Example:
 * UIB_ACTIVE_MTU=1024 LD_PRELOAD=$PWD/libuibv.so ibv_rc_pingpong
 */
#define UIB_ENV_ACTIVE_MTU "UIB_ACTIVE_MTU"

/**
 * DOC: UIB_UET_WIRE_MTU
 * @uib-env: Transparent provider
 * @default: The verbs-negotiated path MTU.
 * @values: `4096` or `8192`
 *
 * Promote a negotiated 4096-byte UET path to an 8192-byte wire packet. Both
 * peers must use UIB and the network interface MTU must be at least 8256.
 *
 * Example:
 * UIB_PROTOCOL=uet UIB_UET_WIRE_MTU=8192 app
 */
#define UIB_ENV_UET_WIRE_MTU "UIB_UET_WIRE_MTU"

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
 * DOC: UIB_RETRY_TIMEOUT_US
 * @uib-env: Library and transparent provider
 * @default: `20000` microseconds
 * @values: `1000` through `60000000`
 *
 * Set the initial retransmission timeout used by reliable RoCEv2 and UET
 * queue pairs. Increase it for high-latency paths; decrease it for tightly
 * controlled simulations where faster loss recovery is required.
 *
 * Example:
 * UIB_RETRY_TIMEOUT_US=50000 LD_PRELOAD=$PWD/libuibv.so app
 */
#define UIB_ENV_RETRY_TIMEOUT_US "UIB_RETRY_TIMEOUT_US"

/**
 * DOC: UIB_STATS_OUTPUT
 * @uib-env: Library and transparent provider
 * @default: `off`
 * @values: `off` or `stderr`
 *
 * Emit one machine-readable `UIB_STATS` JSON record when each context closes.
 * Simulation collectors use `stderr` to verify the selected UIB backend and
 * aggregate its packet and zero-copy counters without application changes.
 * Statistics are diagnostic evidence and do not change application results.
 *
 * Example:
 * UIB_STATS_OUTPUT=stderr LD_PRELOAD=$PWD/libuibv.so app
 */
#define UIB_ENV_STATS_OUTPUT "UIB_STATS_OUTPUT"

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
 * DOC: UIB_UDP_WORKERS
 * @uib-env: UDP transports
 * @default: `auto`, a bounded pool no larger than the allowed CPU count
 * @values: `auto` or a positive integer
 *
 * Set the number of UDP receive-dispatch workers. Packets for one QP always
 * use the same worker, while different QPs can be processed in parallel.
 * With affinity enabled, an explicit count cannot exceed the process's
 * allowed CPU count.
 *
 * Example:
 * UIB_UDP_WORKERS=8 LD_PRELOAD=$PWD/libuibv.so app
 */
#define UIB_ENV_UDP_WORKERS "UIB_UDP_WORKERS"

/**
 * DOC: UIB_UDP_WORKER_AFFINITY
 * @uib-env: UDP transports
 * @default: `on`
 * @values: `off` or `on`
 *
 * Pin UDP receive-dispatch workers and optional multi-flow RX/TX lane threads
 * to CPUs from the process's allowed CPU set. Set this to `off` when an
 * external scheduler manages thread placement.
 *
 * Example:
 * UIB_UDP_WORKERS=4 UIB_UDP_WORKER_AFFINITY=off app
 */
#define UIB_ENV_UDP_WORKER_AFFINITY "UIB_UDP_WORKER_AFFINITY"

/**
 * DOC: UIB_TX_WORKERS
 * @uib-env: Protocol transmit paths
 * @default: `auto`, a bounded pool no larger than the allowed CPU count
 * @values: `auto` or a positive integer
 *
 * Set the number of persistent workers that prepare admitted UET or RoCEv2
 * work requests. PSN assignment, ordered publication, ACK handling, retries,
 * congestion control, and CQ production remain sequenced per QP. With
 * affinity enabled, an explicit count cannot exceed the process's allowed
 * CPU count.
 *
 * Example:
 * UIB_TX_WORKERS=8 LD_PRELOAD=$PWD/libuibv.so app
 */
#define UIB_ENV_TX_WORKERS "UIB_TX_WORKERS"

/**
 * DOC: UIB_TX_WORKER_AFFINITY
 * @uib-env: Protocol transmit paths
 * @default: `on`
 * @values: `off` or `on`
 *
 * Pin UET and RoCEv2 preparation workers to distinct CPUs in the process's
 * allowed CPU set. Set this to `off` when a scheduler or launcher controls
 * placement.
 *
 * Example:
 * UIB_TX_WORKERS=4 UIB_TX_WORKER_AFFINITY=off app
 */
#define UIB_ENV_TX_WORKER_AFFINITY "UIB_TX_WORKER_AFFINITY"

/**
 * DOC: UIB_UDP_FLOWS
 * @uib-env: UDP flow scaling
 * @default: `1`, one UDP flow
 * @values: a positive integer from `1` through `64`
 *
 * Select the number of parallel UDP lanes used with `udp-sync`. This mode is
 * optional and disabled by default. UET assigns each message to one lane and
 * uses the base destination port plus `(lane + 1) * 128`. RoCEv2 assigns each
 * QP to one stable source-port lane, keeps the configured destination port,
 * and distributes incoming flows with `SO_REUSEPORT`. Ordering within a QP is
 * unchanged in both modes.
 *
 * UET's additional destination ports and RoCEv2's local source ports starting
 * at 49152 must be available. RoCEv2 also requires kernel UDP reuseport BPF
 * support. Multi-flow mode requires `udp-sync`, cannot be used with raw UDP,
 * and rejects `UIB_ZEROCOPY=required`.
 *
 * Example:
 * UIB_PROTOCOL=rocev2 UIB_TRANSPORT_MODE=udp-sync UIB_UDP_FLOWS=2 app
 */
#define UIB_ENV_UDP_FLOWS "UIB_UDP_FLOWS"

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
 * DOC: UIB_AFXDP_PROGRAM
 * @uib-env: AF_XDP transport
 * @default: Unset; libxdp installs its generic XSK redirect program.
 * @values: A path to a compiled UIB AF_XDP BPF object.
 *
 * Load a selective XDP program that redirects only the configured UIB UDP
 * destination port to the AF_XDP socket and passes other traffic to the host
 * stack. Build the supplied object with `make afxdp-program`.
 *
 * Example:
 * UIB_AFXDP_PROGRAM=bpf/uib_afxdp_kern.o UIB_TRANSPORT_MODE=udp-zc-afxdp app
 */
#define UIB_ENV_AFXDP_PROGRAM "UIB_AFXDP_PROGRAM"

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

/**
 * DOC: UIB_HOST_ZC
 * @uib-env: Provider-owned host-memory receive
 * @default: `auto`
 * @values: `auto`, `off`, or `required`
 *
 * Enable TCP io_uring ZCRX when its NIC queue and kernel support are usable.
 *
 * Example:
 * UIB_HOST_ZC=required UIB_HOST_ZC_IFACE=eth0 app
 */
#define UIB_ENV_HOST_ZC "UIB_HOST_ZC"

/**
 * DOC: UIB_HOST_ZC_IFACE
 * @uib-env: Provider-owned host-memory receive
 * @default: Unset; ZCRX remains inactive.
 * @values: A network-interface name configured for TCP header/data split.
 *
 * Select the interface whose hardware receive queue backs the ZCRX arena.
 *
 * Example:
 * UIB_HOST_ZC_IFACE=eth0 UIB_TRANSPORT_MODE=tcp-devmem app
 */
#define UIB_ENV_HOST_ZC_IFACE "UIB_HOST_ZC_IFACE"

/**
 * DOC: UIB_HOST_ZC_QUEUE
 * @uib-env: Provider-owned host-memory receive
 * @default: `0`
 * @values: A non-negative hardware receive-queue identifier.
 *
 * Select the flow-steered hardware queue registered with io_uring ZCRX.
 *
 * Example:
 * UIB_HOST_ZC_QUEUE=1 UIB_HOST_ZC_IFACE=eth0 app
 */
#define UIB_ENV_HOST_ZC_QUEUE "UIB_HOST_ZC_QUEUE"

/**
 * DOC: UIB_VIRTME_KERNEL
 * @uib-env: Provider-owned host-memory receive
 * @default: Unset; virtme-ng chooses its default kernel.
 * @values: A kernel image path accepted by `vng --run`.
 *
 * Select the kernel image used by the focused host zero-copy VM tests.
 *
 * Example:
 * UIB_VIRTME_KERNEL=/boot/vmlinuz-test make host-zc-virtme-test
 */
#define UIB_ENV_VIRTME_KERNEL "UIB_VIRTME_KERNEL"

/**
 * DOC: UIB_UET_CC
 * @uib-env: Ultra Ethernet
 * @default: `nscc`
 * @values: `nscc`, `rccc`, `none`, experimental `dcqcn`, or a registered name
 *
 * Select the congestion-control algorithm for UET queue pairs. Use `none`
 * only as an experimental baseline because it removes congestion-window
 * admission control. DCQCN is native to RoCEv2 and requires
 * `UIB_UET_CC_EXPERIMENTAL=on` when selected for UET.
 *
 * Example:
 * UIB_PROTOCOL=uet UIB_UET_CC=none ./examples/stress_bench
 */
#define UIB_ENV_UET_CC "UIB_UET_CC"

/**
 * DOC: UIB_UET_CC_EXPERIMENTAL
 * @uib-env: Ultra Ethernet
 * @default: `off`
 * @values: `off` or `on`
 *
 * Permit a congestion algorithm that is native to another protocol. This
 * gate is required for DCQCN-on-UET research and does not affect UET-native
 * NSCC or RCCC.
 *
 * Example:
 * UIB_PROTOCOL=uet UIB_UET_CC=dcqcn UIB_UET_CC_EXPERIMENTAL=on app
 */
#define UIB_ENV_UET_CC_EXPERIMENTAL "UIB_UET_CC_EXPERIMENTAL"

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
 * DOC: UIB_UET_RCCC_MAX_WND
 * @uib-env: Ultra Ethernet RCCC
 * @default: One and a half times the configured bandwidth-delay product.
 * @values: A byte count no smaller than the active MTU.
 *
 * Set the RCCC speculative and outstanding byte window.
 *
 * Example:
 * UIB_UET_CC=rccc UIB_UET_RCCC_MAX_WND=262144 app
 */
#define UIB_ENV_UET_RCCC_MAX_WND "UIB_UET_RCCC_MAX_WND"

/**
 * DOC: UIB_UET_RCCC_CREDIT_QUANTUM
 * @uib-env: Ultra Ethernet RCCC
 * @default: Four active-MTU packets.
 * @values: A byte count no smaller than the active MTU.
 *
 * Set the amount of source credit issued by one destination scheduling turn.
 *
 * Example:
 * UIB_UET_CC=rccc UIB_UET_RCCC_CREDIT_QUANTUM=16384 app
 */
#define UIB_ENV_UET_RCCC_CREDIT_QUANTUM "UIB_UET_RCCC_CREDIT_QUANTUM"

/**
 * DOC: UIB_UET_RCCC_LINK_RATE
 * @uib-env: Ultra Ethernet RCCC
 * @default: `12500000000` bytes per second.
 * @values: A positive byte-per-second rate.
 *
 * Set the source and destination link rate used for RCCC credit pacing.
 *
 * Example:
 * UIB_UET_CC=rccc UIB_UET_RCCC_LINK_RATE=50000000000 app
 */
#define UIB_ENV_UET_RCCC_LINK_RATE "UIB_UET_RCCC_LINK_RATE"

/**
 * DOC: UIB_UET_RCCC_BASE_RTT
 * @uib-env: Ultra Ethernet RCCC
 * @default: `12000` nanoseconds.
 * @values: A positive nanosecond duration.
 *
 * Set the base RTT used to derive the RCCC bandwidth-delay product.
 *
 * Example:
 * UIB_UET_CC=rccc UIB_UET_RCCC_BASE_RTT=20000 app
 */
#define UIB_ENV_UET_RCCC_BASE_RTT "UIB_UET_RCCC_BASE_RTT"

/**
 * DOC: UIB_ROCEV2_CC
 * @uib-env: RoCEv2
 * @default: `none`
 * @values: `none`, `dcqcn`, a registered name, or experimental `nscc`
 *
 * Select the congestion-control algorithm for RoCEv2 RC queue pairs. DCQCN
 * is the protocol-native implementation. NSCC was designed for Ultra
 * Ethernet, not RoCEv2; selecting it here is an
 * experimental-only cross-protocol research mode, not a deployment default or
 * recommendation. RoCEv2 defaults to `none`.
 *
 * Example:
 * UIB_PROTOCOL=rocev2 UIB_ROCEV2_CC=nscc app
 */
#define UIB_ENV_ROCEV2_CC "UIB_ROCEV2_CC"

/**
 * DOC: UIB_ROCEV2_NSCC_MAX_WND
 * @uib-env: RoCEv2
 * @default: The maximum window calculated by NSCC configuration.
 * @values: A byte count no smaller than the active MTU plus wire overhead.
 *
 * Cap the initial NSCC congestion window for experimental RoCEv2 comparison
 * runs. NSCC was designed for Ultra Ethernet; its use with RoCEv2 is
 * experimental only and is not a production congestion-control recommendation.
 *
 * Example:
 * UIB_PROTOCOL=rocev2 UIB_ROCEV2_CC=nscc UIB_ROCEV2_NSCC_MAX_WND=65536 app
 */
#define UIB_ENV_ROCEV2_NSCC_MAX_WND "UIB_ROCEV2_NSCC_MAX_WND"

/**
 * DOC: UIB_DCQCN_INITIAL_RATE
 * @uib-env: DCQCN
 * @default: `12500000000` bytes per second.
 * @values: A positive rate between the configured minimum and maximum.
 *
 * Set the DCQCN source rate before congestion feedback arrives.
 *
 * Example:
 * UIB_ROCEV2_CC=dcqcn UIB_DCQCN_INITIAL_RATE=50000000000 app
 */
#define UIB_ENV_DCQCN_INITIAL_RATE "UIB_DCQCN_INITIAL_RATE"

/**
 * DOC: UIB_DCQCN_MINIMUM_RATE
 * @uib-env: DCQCN
 * @default: `12500000` bytes per second.
 * @values: A positive rate no greater than the initial rate.
 *
 * Set the minimum rate retained after congestion feedback.
 *
 * Example:
 * UIB_ROCEV2_CC=dcqcn UIB_DCQCN_MINIMUM_RATE=125000000 app
 */
#define UIB_ENV_DCQCN_MINIMUM_RATE "UIB_DCQCN_MINIMUM_RATE"

/**
 * DOC: UIB_DCQCN_MAXIMUM_RATE
 * @uib-env: DCQCN
 * @default: `12500000000` bytes per second.
 * @values: A positive rate no smaller than the initial rate.
 *
 * Cap DCQCN rate recovery at the configured link rate.
 *
 * Example:
 * UIB_ROCEV2_CC=dcqcn UIB_DCQCN_MAXIMUM_RATE=50000000000 app
 */
#define UIB_ENV_DCQCN_MAXIMUM_RATE "UIB_DCQCN_MAXIMUM_RATE"

/**
 * DOC: UIB_DCQCN_ADDITIVE_INCREASE
 * @uib-env: DCQCN
 * @default: `625000` bytes per second per recovery interval.
 * @values: A positive byte-per-second rate increment.
 *
 * Set the target-rate increase after DCQCN fast recovery.
 *
 * Example:
 * UIB_ROCEV2_CC=dcqcn UIB_DCQCN_ADDITIVE_INCREASE=1250000 app
 */
#define UIB_ENV_DCQCN_ADDITIVE_INCREASE "UIB_DCQCN_ADDITIVE_INCREASE"

/**
 * DOC: UIB_DCQCN_RECOVERY_INTERVAL
 * @uib-env: DCQCN
 * @default: `55000` nanoseconds.
 * @values: A positive nanosecond duration.
 *
 * Set the DCQCN alpha update and rate-recovery interval.
 *
 * Example:
 * UIB_ROCEV2_CC=dcqcn UIB_DCQCN_RECOVERY_INTERVAL=100000 app
 */
#define UIB_ENV_DCQCN_RECOVERY_INTERVAL "UIB_DCQCN_RECOVERY_INTERVAL"

/**
 * DOC: UIB_DCQCN_ALPHA_GAIN
 * @uib-env: DCQCN
 * @default: `256`, or 1/256 in Q16 fixed-point units.
 * @values: An integer from `1` through `65536`.
 *
 * Set the EWMA gain used for the DCQCN congestion estimate.
 *
 * Example:
 * UIB_ROCEV2_CC=dcqcn UIB_DCQCN_ALPHA_GAIN=512 app
 */
#define UIB_ENV_DCQCN_ALPHA_GAIN "UIB_DCQCN_ALPHA_GAIN"

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

/**
 * DOC: UIB_BENCH_PIN_CPU
 * @uib-env: Complete benchmark matrix
 * @default: First allowed CPU; namespace peers use first two when available.
 * @values: A CPU identifier accepted by `taskset -c`.
 *
 * Select the pinned CPU; an explicit value pins both namespace peers to it.
 *
 * Example:
 * UIB_BENCH_PIN_CPU=3 make benchmark-all
 */
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
 * DOC: UIB_CC_BENCH_VARIANTS
 * @uib-env: Complete benchmark matrix
 * @default: `rocev2:dcqcn rocev2:none uet:rccc uet:nscc uet:none`
 * @values: Space-separated `protocol:algorithm` pairs supported by the runner.
 *
 * Select protocol and congestion-control pairs for the resilience matrix.
 * UET with DCQCN must use the explicit `dcqcn-experimental` name.
 *
 * Example:
 * UIB_CC_BENCH_VARIANTS='rocev2:dcqcn uet:rccc' make benchmark-all
 */
#define UIB_ENV_CC_BENCH_VARIANTS "UIB_CC_BENCH_VARIANTS"

#define UIB_ENV_CC_BENCH_CONDITIONS "UIB_CC_BENCH_CONDITIONS"

/**
 * DOC: UIB_CC_BENCH_MATRIX
 * @uib-env: Complete benchmark matrix
 * @default: Unset; the variants and conditions form a Cartesian product.
 * @values: Space-separated `protocol:algorithm:condition` entries.
 *
 * Replace the Cartesian product with an exact resilience benchmark matrix.
 * Do not set this together with variant or condition selectors.
 *
 * Example:
 * UIB_CC_BENCH_MATRIX='rocev2:dcqcn:drops uet:rccc:reordered' make benchmark-all
 */
#define UIB_ENV_CC_BENCH_MATRIX "UIB_CC_BENCH_MATRIX"

/**
 * DOC: UIB_CC_BENCH_ITERATIONS
 * @uib-env: Complete benchmark matrix
 * @default: `1000`
 * @values: A positive 32-bit integer.
 *
 * Set measured operations per resilience matrix entry.
 *
 * Example:
 * UIB_CC_BENCH_ITERATIONS=10000 make benchmark-all
 */
#define UIB_ENV_CC_BENCH_ITERATIONS "UIB_CC_BENCH_ITERATIONS"

/**
 * DOC: UIB_CC_BENCH_SIZE
 * @uib-env: Complete benchmark matrix
 * @default: `16384` bytes
 * @values: A positive 32-bit byte count.
 *
 * Set the payload size for resilience matrix operations.
 *
 * Example:
 * UIB_CC_BENCH_SIZE=65536 make benchmark-all
 */
#define UIB_ENV_CC_BENCH_SIZE "UIB_CC_BENCH_SIZE"

/**
 * DOC: UIB_CC_BENCH_WINDOW
 * @uib-env: Complete benchmark matrix
 * @default: `16`
 * @values: A positive operation count.
 *
 * Set the maximum in-flight operation count for each resilience entry.
 *
 * Example:
 * UIB_CC_BENCH_WINDOW=64 make benchmark-all
 */
#define UIB_ENV_CC_BENCH_WINDOW "UIB_CC_BENCH_WINDOW"

/**
 * DOC: UIB_CC_BENCH_MTU
 * @uib-env: Complete benchmark matrix
 * @default: `1024` bytes
 * @values: `256`, `512`, `1024`, `2048`, or `4096` when RoCEv2 is selected.
 *
 * Set one common path MTU for all resilience matrix entries.
 *
 * Example:
 * UIB_CC_BENCH_MTU=4096 make benchmark-all
 */
#define UIB_ENV_CC_BENCH_MTU "UIB_CC_BENCH_MTU"

/**
 * DOC: UIB_CC_BENCH_TIMEOUT
 * @uib-env: Complete benchmark matrix
 * @default: `120` seconds
 * @values: A positive integer number of seconds.
 *
 * Set the per-entry timeout, including retry delays caused by impairments.
 *
 * Example:
 * UIB_CC_BENCH_TIMEOUT=300 make benchmark-all
 */
#define UIB_ENV_CC_BENCH_TIMEOUT "UIB_CC_BENCH_TIMEOUT"

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
 * DOC: UIB_TEST_LINK_MTU
 * @uib-env: Test and scaling harnesses
 * @default: `1500`
 * @values: An integer from `576` through `65535`.
 *
 * Set both veth link MTUs used by standard ibverbs tool tests. The harness
 * derives and advertises the largest UIB active MTU that fits the link.
 *
 * Example:
 * UIB_TEST_LINK_MTU=576 make uib-tools-test
 */
#define UIB_ENV_TEST_LINK_MTU "UIB_TEST_LINK_MTU"

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
 * DOC: UIB_UET_ERROR_TEST
 * @uib-env: Test and scaling harnesses
 * @default: Unset and disabled.
 * @values: Any non-empty value.
 *
 * Run UET semantic error checks after the normal loopback operation matrix.
 *
 * Example:
 * UIB_PROTOCOL=uet UIB_UET_ERROR_TEST=1 ./tests/loopback
 */
#define UIB_ENV_UET_ERROR_TEST "UIB_UET_ERROR_TEST"

/**
 * DOC: UIB_UET_ERROR_ONLY
 * @uib-env: Test and scaling harnesses
 * @default: Unset and disabled.
 * @values: Any non-empty value.
 *
 * Run only UET semantic error checks. This keeps deterministic loss injection
 * available for the first request and its guaranteed response.
 *
 * Example:
 * UIB_PROTOCOL=uet UIB_UET_ERROR_ONLY=1 UIB_TEST_DROP_TX=1 ./tests/loopback
 */
#define UIB_ENV_UET_ERROR_ONLY "UIB_UET_ERROR_ONLY"

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
 * DOC: UIB_CONTEXT_SCALE_LANES
 * @uib-env: Test and scaling harnesses
 * @default: `1 2 4 8`
 * @values: Space-separated positive integers from `1` through `32`.
 *
 * Select independent context/QP lane counts for the context-scaling benchmark.
 * Each lane uses a distinct UDP port pair.
 *
 * Example:
 * UIB_CONTEXT_SCALE_LANES="1 4 8" make context-scaling-benchmark
 */
#define UIB_ENV_CONTEXT_SCALE_LANES "UIB_CONTEXT_SCALE_LANES"

/**
 * DOC: UIB_CONTEXT_SCALE_TRIALS
 * @uib-env: Test and scaling harnesses
 * @default: `3`
 * @values: A positive integer.
 *
 * Set the number of measurements for every context/QP lane count.
 *
 * Example:
 * UIB_CONTEXT_SCALE_TRIALS=5 make context-scaling-benchmark
 */
#define UIB_ENV_CONTEXT_SCALE_TRIALS "UIB_CONTEXT_SCALE_TRIALS"

/**
 * DOC: UIB_CONTEXT_SCALE_PORT_BASE
 * @uib-env: Test and scaling harnesses
 * @default: `22400`
 * @values: A positive port number no greater than `65400`.
 *
 * Set the first loopback control port. Data ports begin 64 ports later.
 * Change this value when another local service uses the default range.
 *
 * Example:
 * UIB_CONTEXT_SCALE_PORT_BASE=24000 make context-scaling-benchmark
 */
#define UIB_ENV_CONTEXT_SCALE_PORT_BASE "UIB_CONTEXT_SCALE_PORT_BASE"

/**
 * DOC: UIB_CONTEXT_SCALE_TIMEOUT
 * @uib-env: Test and scaling harnesses
 * @default: `120`
 * @values: A positive number of seconds.
 *
 * Set the timeout for each server or client process in a scaling trial.
 *
 * Example:
 * UIB_CONTEXT_SCALE_TIMEOUT=300 make context-scaling-benchmark
 */
#define UIB_ENV_CONTEXT_SCALE_TIMEOUT "UIB_CONTEXT_SCALE_TIMEOUT"

/**
 * DOC: UIB_QP_SCALE_COUNTS
 * @uib-env: Test and scaling harnesses
 * @default: `1 2 4 8`
 * @values: Space-separated positive integers from `1` through `256`.
 *
 * Select QP counts for the shared-context scaling benchmark. All QPs in one
 * measurement share a context, CQ, memory region, and transport endpoint.
 *
 * Example:
 * UIB_QP_SCALE_COUNTS="1 4 8" make qp-scaling-benchmark
 */
#define UIB_ENV_QP_SCALE_COUNTS "UIB_QP_SCALE_COUNTS"

/**
 * DOC: UIB_QP_SCALE_QPS
 * @uib-env: Test and scaling harnesses
 * @default: `1`
 * @values: A positive integer from `1` through `256`.
 *
 * Select one QP count when running `examples/qp_scaling_bench` directly. The
 * matrix harness sets this variable from `UIB_QP_SCALE_COUNTS`.
 *
 * Example:
 * UIB_QP_SCALE_QPS=4 ./examples/qp_scaling_bench
 */
#define UIB_ENV_QP_SCALE_QPS "UIB_QP_SCALE_QPS"

/**
 * DOC: UIB_QP_SCALE_POSTING
 * @uib-env: Test and scaling harnesses
 * @default: `parallel`
 * @values: `parallel` or `serial`
 *
 * Select persistent per-QP posting workers or one serial posting thread in
 * the shared-context benchmark. Use `serial` as the single-producer control.
 *
 * Example:
 * UIB_QP_SCALE_POSTING=serial make qp-scaling-benchmark
 */
#define UIB_ENV_QP_SCALE_POSTING "UIB_QP_SCALE_POSTING"

/**
 * DOC: UIB_QP_SCALE_TRIALS
 * @uib-env: Test and scaling harnesses
 * @default: `3`
 * @values: A positive integer.
 *
 * Set the number of measurements for each shared-context QP count.
 *
 * Example:
 * UIB_QP_SCALE_TRIALS=5 make qp-scaling-benchmark
 */
#define UIB_ENV_QP_SCALE_TRIALS "UIB_QP_SCALE_TRIALS"

/**
 * DOC: UIB_QP_SCALE_TIMEOUT
 * @uib-env: Test and scaling harnesses
 * @default: `120`
 * @values: A positive number of seconds.
 *
 * Set the timeout for one shared-context scaling measurement.
 *
 * Example:
 * UIB_QP_SCALE_TIMEOUT=300 make qp-scaling-benchmark
 */
#define UIB_ENV_QP_SCALE_TIMEOUT "UIB_QP_SCALE_TIMEOUT"

/**
 * DOC: UIB_QP_SCALE_PORT
 * @uib-env: Test and scaling harnesses
 * @default: `22550`
 * @values: A positive port number no greater than `65534`.
 *
 * Set the first local UDP port used by the shared-context scaling benchmark.
 * Its peer uses the following port.
 *
 * Example:
 * UIB_QP_SCALE_PORT=24550 make qp-scaling-benchmark
 */
#define UIB_ENV_QP_SCALE_PORT "UIB_QP_SCALE_PORT"

/**
 * DOC: UIB_SINGLE_QP_CPU_COUNTS
 * @uib-env: Test and scaling harnesses
 * @default: `1 2 4 8`
 * @values: A whitespace-separated list of positive worker counts up to `256`.
 *
 * Set the ordered TX preparation worker counts measured for one queue pair.
 *
 * Example:
 * UIB_SINGLE_QP_CPU_COUNTS="1 4 8" make single-qp-cpu-benchmark
 */
#define UIB_ENV_SINGLE_QP_CPU_COUNTS "UIB_SINGLE_QP_CPU_COUNTS"

/**
 * DOC: UIB_SINGLE_QP_CPU_TRIALS
 * @uib-env: Test and scaling harnesses
 * @default: `11`
 * @values: A positive integer.
 *
 * Set the number of measurements collected for each worker count.
 *
 * Example:
 * UIB_SINGLE_QP_CPU_TRIALS=5 make single-qp-cpu-benchmark
 */
#define UIB_ENV_SINGLE_QP_CPU_TRIALS "UIB_SINGLE_QP_CPU_TRIALS"

/**
 * DOC: UIB_SINGLE_QP_CPU_TIMEOUT
 * @uib-env: Test and scaling harnesses
 * @default: `120`
 * @values: A positive number of seconds.
 *
 * Set the timeout for one single-queue-pair CPU scaling measurement.
 *
 * Example:
 * UIB_SINGLE_QP_CPU_TIMEOUT=300 make single-qp-cpu-benchmark
 */
#define UIB_ENV_SINGLE_QP_CPU_TIMEOUT "UIB_SINGLE_QP_CPU_TIMEOUT"

/**
 * DOC: UIB_SINGLE_QP_CPU_PORT
 * @uib-env: Test and scaling harnesses
 * @default: `22650`
 * @values: A positive port number no greater than `65534`.
 *
 * Set the first UDP port used by the single-queue-pair CPU scaling benchmark.
 * Its peer uses the following port.
 *
 * Example:
 * UIB_SINGLE_QP_CPU_PORT=24650 make single-qp-cpu-benchmark
 */
#define UIB_ENV_SINGLE_QP_CPU_PORT "UIB_SINGLE_QP_CPU_PORT"

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
