UIB userspace RDMA
==================

UIB implements RDMA wire protocols, packet transports and congestion-control
algorithms in userspace. It provides a native API and `libuibv.so`, a
compatibility provider that lets dynamically linked libibverbs applications
run unchanged. It is useful for CI, protocol validation and deployment
simulation. UIB has been tested with software RXE and hardware (Mellanox
NICs).

Example: transparently running libibverbs software with UIB
-----------------------------------------------------------

`LD_PRELOAD` can redirect an unmodified dynamically linked program such as
perftest's `ib_write_bw` through UIB. The synthetic `uib0` device is listed
first, so `-d uib0` is optional.

Build UIB, then run a server whose data-plane address is `192.0.2.1`:

    env LD_PRELOAD="$PWD/libuibv.so" \
      UIB_PROTOCOL=rocev2 UIB_ROCEV2_CC=dcqcn \
      UIB_TRANSPORT_MODE=udp-io-uring \
      UIB_BIND_IP=192.0.2.1 UIB_BIND_PORT=4791 \
      ib_write_bw -i 1 -x 0 --connection=RC --size=65536 \
      --iters=10000 --report_gbits -F

Run the client on `192.0.2.2`:

    env LD_PRELOAD="$PWD/libuibv.so" \
      UIB_PROTOCOL=rocev2 UIB_ROCEV2_CC=dcqcn \
      UIB_TRANSPORT_MODE=udp-io-uring \
      UIB_BIND_IP=192.0.2.2 UIB_BIND_PORT=4791 \
      ib_write_bw -i 1 -x 0 --connection=RC --size=65536 \
      --iters=10000 --report_gbits -F 192.0.2.1

For Ultra Ethernet with NSCC, start the server with the UET service port:

    env LD_PRELOAD="$PWD/libuibv.so" \
      UIB_PROTOCOL=uet UIB_UET_CC=nscc \
      UIB_TRANSPORT_MODE=udp-io-uring \
      UIB_BIND_IP=192.0.2.1 UIB_BIND_PORT=5432 \
      ib_write_bw -i 1 -x 0 --connection=RC --size=65536 \
      --iters=10000 --report_gbits -F

Run the matching UET/NSCC client on `192.0.2.2`:

    env LD_PRELOAD="$PWD/libuibv.so" \
      UIB_PROTOCOL=uet UIB_UET_CC=nscc \
      UIB_TRANSPORT_MODE=udp-io-uring \
      UIB_BIND_IP=192.0.2.2 UIB_BIND_PORT=5432 \
      ib_write_bw -i 1 -x 0 --connection=RC --size=65536 \
      --iters=10000 --report_gbits -F 192.0.2.1

Both endpoints must use the same protocol and service port. Socket-mode
UIB-to-UIB traffic works without raw-socket privileges; raw mode is only for
kernel RXE interoperability and requires `CAP_NET_RAW`. See the
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
    opt-in actual NCCL collective matrix; and
  * Docker or another compatible OCI engine plus NVIDIA CDI for the opt-in
    GPU training and two-rank UIB training tests.

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
| `uet` | Ultra Ethernet PDS/PDC and SES with NSCC or RCCC; port 5432 |
| `dummy` | Minimal SEND protocol for abstraction tests |

The implemented congestion-control algorithms are:

| Value | Description |
|---|---|
| `nscc` | UET sender-window control driven by receiver feedback; UET default |
| `rccc` | UET receiver-credit control with paced destination grants |
| `dcqcn` | RoCEv2 rate control driven by ECN congestion feedback |

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

Standalone static library
-------------------------

`libuib.a` is the conventional UIB-only archive and expects the application
to link UIB's dependencies. For a deployable artifact with no dynamic library
dependencies, build the combined archive and its static-link test:

    make static-test

This produces `libuib-standalone.a` by combining UIB with the static liburing,
libmnl, userspace-RCU, and zlib dependency archives. An application can link
it without additional libraries:

    cc -I/path/to/uib/include -static -o app app.c \
      /path/to/uib/libuib-standalone.a

The build needs the static development form of every detected feature. Use
the normal `--disable-FEATURE` configure options before building when a
feature or its static library is not wanted. `make install-static` installs
the standalone archive separately, so ordinary shared-library installations
do not unexpectedly embed third-party code.

AF_XDP is excluded from the portable standalone profile because some
distributions do not provide a complete statically linkable
libxdp/libbpf/libelf chain. Enable it with `STANDALONE_AFXDP=1` when that
chain is available; `static-test` will reject an incomplete result. The
ordinary libraries continue to use AF_XDP whenever configure detects it.

The combined archive contains third-party code under its respective licenses.
Distributors remain responsible for the corresponding license and relinking
requirements; the regular `libuib.a` avoids embedding that code.

Distribution compatibility and releases
---------------------------------------

CI builds UIB against the stock development libraries in Ubuntu 24.04,
Ubuntu 26.04, Debian 13, Fedora 44 and Alpine 3.24. The Fedora image uses the
same repositories and userspace libraries as Fedora Server. Each matrix entry
enables the verbs provider and AF_XDP, runs the native and provider tests,
checks an unchanged rdma-core tool through `libuibv.so`, verifies generated
documentation and tests a staged installation. Run the same test on a
supported development system with:

    make distribution-compat-test

A separate source-compatibility matrix builds UIB against pinned upstream
rdma-core/libibverbs and libfabric releases. It covers the current release,
one maintained older generation and one legacy generation. The gate rejects
system-libibverbs fallback, checks provider ABI and CQ behavior, discovers
`uib0` with an unchanged rdma-core tool, and runs libfabric RxD message, RMA
and atomic traffic through UIB. Its versioned source builds run in CI with:

    make upstream-compat-test \
    UIB_RDMA_CORE_VERSION=64.0 UIB_LIBFABRIC_VERSION=2.6.0

Independent-context scaling uses the existing verified stress workload and
records per-lane and aggregate bandwidth plus client/server CPU time:

    UIB_CONTEXT_SCALE_LANES="1 2 4 8" \
    UIB_BENCH_PROTOCOL=uet make context-scaling-benchmark

The harness inherits the normal benchmark, transport, and congestion-control
variables. It uses one UDP receive worker per process unless
`UIB_UDP_WORKERS` is set, which avoids multiplying an automatic CPU-count
worker pool across every benchmark process. The result directory retains the
exact commit, kernel, CPU set, inherited UIB variables, per-lane logs, summed
steady-state lane rate and whole-trial wall-clock rate.

Shared-context QP scaling isolates contention inside one UIB context and one
transport endpoint. Unlike the independent-context test, every QP shares the
same CQ, registered-memory region and UDP backend:

    UIB_QP_SCALE_COUNTS="1 2 4 8" \
    UIB_BENCH_PROTOCOL=uet make qp-scaling-benchmark

The harness verifies every payload and completion, records wall and process
CPU time, and inherits `UIB_BENCH_ITERATIONS`, `UIB_BENCH_SIZE`,
`UIB_BENCH_WINDOW`, `UIB_BENCH_MTU`, transport and congestion-control
settings. It uses persistent per-QP posting threads by default; set
`UIB_QP_SCALE_POSTING=serial` for the single-producer control. Compare its
results with `context-scaling-benchmark` to distinguish shared-context limits
from host-wide or kernel limits.

Fedora 44 and Alpine 3.24 are the current stable release lines represented by
the explicit image tags. Update the matrix and dependency installer together
when a newer stable line is adopted; explicit tags keep old results
reproducible.

A version tag publishes packages built and tested in their matching
distribution:

| Build environment | Release artifact |
|---|---|
| Ubuntu 24.04 | `.deb` |
| Ubuntu 26.04 | `.deb` |
| Debian 13 | `.deb` |
| Fedora 44 | `.rpm` |
| Alpine 3.24 | `.apk` |
| Portable static builder | `uib-VERSION-static-linux-amd64.tar.gz` |

The static archive contains `libuib-standalone.a`, UIB headers, its build
manifest and the licenses for embedded dependencies. It does not require UIB
or its dependency libraries to be installed on the target. Native packages
use each distribution's shared libraries and carry dependencies generated by
that distribution's package tools.

The Alpine release also includes the public key used to sign its APK. Copy
that key to `/etc/apk/keys` before installing the package, or use apk's
`--allow-untrusted` option for a one-off local installation.

The tag must be `v` followed by the exact version in `configure.ac`. For
example, version `0.1` is released by pushing tag `v0.1`. A mismatched tag is
rejected before any packages are published.

Testing, examples and benchmarks
--------------------------------

Common targets are:

    make test                 # native unit and traffic tests
    make nccl-test            # actual two-rank NCCL collective through UIB
    make rccl-test            # opt-in two-or-more-rank RCCL test on AMD GPUs
    make ucx-libfabric-test   # opt-in UCX and libfabric transfers through UIB
    make simulation-cli-test  # validate simulation profiles and layer overlays
    make simulation-kind-test # validate standard Kind rendering and delegation
    make simulation-kind-e2e-test # opt-in real disposable Kind lifecycle
    make simulation-kubernetes-render-test # validate Kubeflow workload YAML
    make simulation-kubeflow-e2e-test # opt-in real two-worker CPU TrainJob
    make training-smoke-test  # pinned two-rank CPU DDP and restart workload
    make training-public-test # dynamically cached tiny public model and data
    make training-gpu-test    # opt-in NCCL and FSDP restart on NVIDIA CDI
    make training-uib-preflight-test # compose training image and UIB runtime
    make training-uib-test    # actual two-rank NCCL training traffic via UIB
    make inference-smoke-test # pinned CPU OpenAI API and local Ray task
    make inference-gpu-test   # opt-in vLLM GPU API and standard benchmark
    make virtme-test          # complete virtme-ng/RXE integration suite
    make test-all             # alias for the complete guest suite
    make examples-test        # native and verbs examples
    make examples-stress-test # windowed traffic example
    make scaling-test         # bounded lookup and traffic regressions
    make benchmark-report     # full CSV, ASCII and Markdown matrix
    make benchmark-doc        # compare and refresh checked-in results

The benchmark matrix covers all protocols, available transports, host and
DMA-BUF memory, copy and zero-copy modes, pinned and unpinned processes, and
MTUs 1500, 4096 and 8192 where valid. A resilience section compares
RoCEv2/DCQCN and UET algorithms under latency, loss, reordering, and combined
impairments. It records actual bandwidth, latency, CPU time and zero-copy
fallbacks. Results are split into like-for-like tables
in the [benchmark report](docs/benchmark-results.md); the focused mode
comparison is in the [sync/async report](docs/sync-async-benchmark.md).

See the [examples guide](examples/) for local and remote traffic commands.
The [LLM cluster simulation guide](docs/llm-cluster-simulation.md) documents
the pinned training and inference images, their CPU-sized public profiles,
external large-model configuration, evidence contracts, and identical local,
Kubernetes, and Slurm launch shapes. Model weights and datasets are downloaded
only when a public workload is run and remain in an ignored, reusable cache;
they are never committed or built into an image.
`make docs` regenerates the [public API](docs/public-api.md), environment and
`uib(7)` references from comments beside their definitions; `make docs-check`
verifies them.

`make nccl-test` needs `NCCL_TESTS_BIN` to name an unmodified
`all_reduce_perf` binary and locates the other selected `nccl-tests` programs
beside it. It starts and cleans up an isolated MPS instance, forces NCCL's
`NET/IB` path through `libuibv.so`, rejects Socket fallback, and checks every
collective result. See the
[transparent verbs guide](docs/transparent-verbs.md) for prerequisites,
configurable parameters, and real-cluster use.

`make training-uib-test` applies the same proof to the unchanged PyTorch
training entry point in the pinned OCI image. It starts two containerized NCCL
ranks, forces gradient collectives through `uib0`, checks the workload result,
and rejects Socket fallback. The local gate can share one physical GPU through
NVIDIA MPS, but it still needs two communicating ranks: a one-process GPU run
cannot validate UIB traffic. Set `TRAINING_CONTAINER_ENGINE` to the appropriate
container command when the default engine is not directly accessible.

The [LLM cluster simulation guide](docs/llm-cluster-simulation.md) describes a
layered validation environment for unchanged collective and LLM workloads. It
covers
Kind, virtme, libvirt/OpenTofu, Kubernetes, Kubeflow, KubeRay and
Slurm/Pyxis/Enroot, including how to exchange individual layers and promote the
same workloads to physical GPU and RDMA clusters.

Simulation configuration uses ordinary YAML overlays and JSON Schema. Swap
only the desired layers and inspect the result before invoking standard tools:

    simulation/bin/uib-sim render --profile virtme-kubernetes \
      --overlay communication/ucx --overlay protocol/uet-nscc

Replace `communication/ucx` with `communication/libfabric`, `nccl`, or `rccl`;
replace `protocol/uet-nscc` with `protocol/rocev2` or `uet-none`. The thin
wrapper does not replace Kind, OpenTofu, Ansible, Helm, Kubernetes, or Slurm.

Render the checked-in CPU-sized training request as standard Kubeflow Trainer
resources without deploying it:

    simulation/bin/uib-sim render-workload --profile kind-kubernetes \
      --request simulation/requests/training-smoke.yaml \
      --run-id uib-smoke --allow-mutable-tags

Published and real-cluster runs omit `--allow-mutable-tags` and use immutable
digests for both the workload image and `uib.bundle`.

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
