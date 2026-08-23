UIB userspace RDMA
==================

UIB implements RDMA wire protocols and packet transports in userspace. It
provides a native API and `libuibv.so`, a compatibility provider that lets
dynamically linked libibverbs applications run unchanged. It is useful for
CI, protocol validation and deployment simulation. UIB has been tested with
software RXE and hardware (Mellanox NICs).

Example: transparently running libibverbs software with UIB
-----------------------------------------------------------

`LD_PRELOAD` can redirect an unmodified dynamically linked program such as
perftest's `ib_write_bw` through UIB. The synthetic `uib0` device is listed
first, so `-d uib0` is optional.

Build UIB, then run a server whose data-plane address is `192.0.2.1`:

    env LD_PRELOAD="$PWD/libuibv.so" \
      UIB_PROTOCOL=rocev2 UIB_TRANSPORT_MODE=udp-io-uring \
      UIB_BIND_IP=192.0.2.1 UIB_BIND_PORT=4791 \
      ib_write_bw -i 1 -x 0 --connection=RC --size=65536 \
      --iters=10000 --report_gbits -F

Run the client on `192.0.2.2`:

    env LD_PRELOAD="$PWD/libuibv.so" \
      UIB_PROTOCOL=rocev2 UIB_TRANSPORT_MODE=udp-io-uring \
      UIB_BIND_IP=192.0.2.2 UIB_BIND_PORT=4791 \
      ib_write_bw -i 1 -x 0 --connection=RC --size=65536 \
      --iters=10000 --report_gbits -F 192.0.2.1

Both endpoints must use the same protocol and service port. Socket-mode
UIB-to-UIB traffic works without raw-socket privileges; raw mode is only for
kernel RXE interoperability and requires `CAP_NET_RAW`. To use UET, set
`UIB_PROTOCOL=uet` and port 5432 on both hosts. See the
[transparent verbs guide](docs/transparent-verbs.md) for provider behavior
and deployment details.

Requirements
------------

The core build needs Linux, a C11 compiler, GNU make, pthreads, liburing,
libmnl and userspace RCU (`liburcu-bp`). Zlib is optional but accelerates the
RoCEv2 invariant CRC. Distribution development packages are typically named
`liburing-dev`, `libmnl-dev`, `liburcu-dev` and `zlib1g-dev` on Debian/Ubuntu,
or `liburing-devel`, `libmnl-devel`, `userspace-rcu-devel` and `zlib-devel` on
Fedora.

Optional features use:

  * libibverbs/rdma-core for `libuibv.so`, verbs tools and RXE tests;
  * libxdp and libbpf for AF_XDP;
  * recent Linux DMA-BUF, `SO_DEVMEM_DMABUF`, `MSG_ZEROCOPY` and io_uring ZCRX
    headers for their corresponding memory paths;
  * virtme-ng, iproute2 and rdma-core tools for VM/RXE integration; and
  * perftest for `ib_write_bw` benchmarks; and
  * CUDA, NCCL 2.30 or newer, Open MPI, NVIDIA MPS and `nccl-tests` for the
    opt-in actual NCCL collective test.

Configure detects optional dependencies automatically:

    ./configure
    make -j$(nproc)

Use `--disable-FEATURE` to turn a detected feature off or `--enable-FEATURE`
to require it. Supported feature names include `provider`, `af-xdp`, `devmem`,
`dmabuf`, `zerocopy` and `host-zc`. Use `--with-liburing=PREFIX` for a
nonstandard liburing installation. Autoconf 2.69 or newer is needed only to
regenerate `configure`.

Protocols and transports
------------------------

`UIB_PROTOCOL` selects the wire protocol:

| Value | Description |
|---|---|
| `rocev2` | InfiniBand RC over RoCEv2; default, port 4791 |
| `uet` | Ultra Ethernet PDS/PDC, SES and NSCC; port 5432 |
| `dummy` | Minimal SEND protocol for abstraction tests |

`UIB_TRANSPORT_MODE` independently selects packet I/O:

| Value | Description |
|---|---|
| `udp-io-uring` | Asynchronous UDP; default |
| `udp-sync` | Synchronous UDP TX comparison/debug path |
| `tcp-devmem` | Framed TCP with optional host/device-memory facilities |
| `udp-zc-afxdp` | AF_XDP when libxdp and an interface are configured |

The default transport uses one io_uring worker per context for batched TX and
RX. Runtime settings control queue depths, batching, SQPOLL, zero-copy,
congestion control and multipath. See the generated
[environment-variable reference](docs/environment-variables.md) for every
setting and example.

Testing, examples and benchmarks
--------------------------------

Common targets are:

    make test                 # native unit and traffic tests
    make nccl-test            # actual two-rank NCCL collective through UIB
    make virtme-test          # complete virtme-ng/RXE integration suite
    make test-all             # alias for the complete guest suite
    make examples-test        # native and verbs examples
    make examples-stress-test # windowed traffic example
    make scaling-test         # bounded lookup and traffic regressions
    make benchmark-report     # full CSV, ASCII and Markdown matrix
    make benchmark-doc        # compare and refresh checked-in results

The benchmark matrix covers all protocols, available transports, host and
DMA-BUF memory, copy and zero-copy modes, pinned and unpinned processes, and
MTUs 1500, 4096 and 8192 where valid. It records actual bandwidth, latency,
CPU time and zero-copy fallbacks. Results are split into like-for-like tables
in the [benchmark report](docs/benchmark-results.md); the focused mode
comparison is in the [sync/async report](docs/sync-async-benchmark.md).

See the [examples guide](examples/) for local and remote traffic commands.
`make docs` regenerates the [public API](docs/public-api.md), environment and
`uib(7)` references from comments beside their definitions; `make docs-check`
verifies them.

`make nccl-test` needs `NCCL_TESTS_BIN` to name an unmodified
`all_reduce_perf` binary. It starts and cleans up an isolated MPS instance,
forces NCCL's `NET/IB` path through `libuibv.so`, rejects Socket fallback, and
checks the collective result. See the
[transparent verbs guide](docs/transparent-verbs.md) for prerequisites,
configurable parameters, and real-cluster use.

The [LLM cluster simulation guide](docs/llm-cluster-simulation.md) describes a
planned, layered validation environment for unchanged NCCL workloads. It covers
Kind, virtme, libvirt/OpenTofu, Kubernetes, Kubeflow, KubeRay and
Slurm/Pyxis/Enroot, including how to exchange individual layers and promote the
same workloads to physical GPU and RDMA clusters.

Memory and zero-copy
--------------------

UIB supports ordinary registered memory and mmap-capable system or GPU
DMA-BUFs through native and verbs registration APIs. Non-mappable GPU
DMA-BUFs require a transport with a complete device TX/RX path; the current
framed TCP netdev binding does not claim that capability.

Socket zero-copy is optional and transmit-only. Provider-owned receive loans
can avoid the final RX copy when io_uring ZCRX and suitable NIC queue steering
are available; ordinary pre-posted verbs receives retain their normal SGE
semantics. The [DMA-BUF and zero-copy guide](docs/dmabuf-zerocopy.md) describes
capabilities, ownership, configuration and measured limits.

Ultra Ethernet scope
--------------------

UIB is an independent implementation of selected UET mechanisms. It is not
affiliated with or certified by the Ultra Ethernet Consortium, and it does
not claim complete conformance to the Ultra Ethernet Specification or
interoperability with third-party UET implementations. Current UET
interoperability testing covers UIB-to-UIB endpoints only.

The UET implementation covers PDS/PDC, reliable SES SEND and RDMA operations,
optimized headers, integer atomics, NSCC congestion control and entropy
multipath/failover. ROD and UUD are intentionally excluded.

Contributing
------------

Contributions should follow the
[Linux kernel coding style](https://www.kernel.org/doc/html/latest/process/coding-style.html):
use tabs for indentation, keep lines reasonably short, place braces and labels
consistently with kernel code, and avoid typedefs for structures. Public API
comments use kernel-doc style and must document every parameter and every
non-void return value. Comments should explain intent, constraints or unusual
behavior instead of restating the code.

Keep changes focused and include tests for behavior changes. Before submitting
a change, run the relevant tests and at least:

    make test
    make docs-check
    git diff --check

Run `make docs` and include the regenerated files whenever public API or
environment-variable documentation changes.

License
-------

UIB's original code is licensed under the [Apache License 2.0](LICENSE). The
source repository is a mixed-license distribution: files incorporating
separately licensed material identify their terms with SPDX headers. See the
[license map](LICENSES/README.md), [NOTICE](NOTICE), and [AUTHORS](AUTHORS).
