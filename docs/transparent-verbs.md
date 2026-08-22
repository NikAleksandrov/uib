<!-- SPDX-License-Identifier: Apache-2.0 -->

# Running libibverbs applications transparently with UIB

UIB can run an existing dynamically linked libibverbs application without
changing or recompiling it. `libuibv.so` is loaded before the system
libibverbs library and adds a synthetic RDMA device named `uib0`.

When an application opens `uib0`, verbs objects and data-path operations are
implemented by UIB. When it opens a real RDMA device, the same interposition
library forwards supported calls to the system libibverbs implementation.

## Contents

- [Terminology](#terminology)
- [How transparent loading works](#how-transparent-loading-works)
- [Build and verify the provider](#build-and-verify-the-provider)
- [Minimal UIB-to-UIB example](#minimal-uib-to-uib-example)
- [Settings needed for transparent operation](#settings-needed-for-transparent-operation)
- [Application behavior and compatibility](#application-behavior-and-compatibility)
- [Deploying outside an interactive shell](#deploying-outside-an-interactive-shell)
- [Troubleshooting](#troubleshooting)
- [Validation targets](#validation-targets)
- [Verbs hooked by libuibv.so](#verbs-hooked-by-libuibvso)

## Terminology

This guide uses four distinct terms:

- A **command** is the executable being run and its arguments, such as
  `ibv_rc_pingpong -d uib0`.
- An **environment variable** is a named setting passed to that command, such
  as `UIB_PROTOCOL=rocev2`.
- A **verbs call** is a C API function called inside the application, such as
  `ibv_create_qp()` or `ibv_post_send()`.
- `uib0` is the synthetic verbs **device** provided by `libuibv.so`. It is not
  a Linux network-interface name.

The examples use `env` to make the boundary visible:

```sh
env NAME=value OTHER_NAME=value command argument
```

Everything between `env` and `command` configures the new process. The final
words are the command and its arguments.

## How transparent loading works

`LD_PRELOAD` is a dynamic-loader environment variable, not a UIB setting. It
asks the loader to resolve symbols from `libuibv.so` before resolving them from
the normal libibverbs library. UIB uses those hooks to:

1. add `uib0` to the device list;
2. create UIB-backed contexts, PDs, MRs, CQs, channels, and QPs for `uib0`;
3. translate supported work requests and completions; and
4. forward interposed calls for real-device objects to system libibverbs.

This mechanism only applies to dynamically linked programs. Static binaries,
programs that do not use libibverbs, and security-restricted executables that
discard `LD_PRELOAD` cannot be redirected this way.

UIB places `uib0` first in the list returned by `ibv_get_device_list()`.
Applications that automatically select the first device therefore normally
choose it. Passing the application's device option explicitly, for example
`-d uib0`, is still recommended: it documents intent and avoids depending on
device-order behavior.

## Build and verify the provider

The provider requires the libibverbs development files when UIB is configured.
Build it from the source tree:

```sh
./configure
make -j"$(nproc)"
```

The configure summary must report:

```text
libibverbs provider: 1
```

If it reports `0`, install the libibverbs development package and reconfigure,
or use `./configure --enable-provider` to make a missing dependency a hard
error.

Before starting a distributed test, verify that the library exists, that its
dependencies resolve, and that the synthetic device is visible:

```sh
test -f ./libuibv.so
ldd ./libuibv.so
env LD_PRELOAD="$(realpath ./libuibv.so)" ibv_devices
```

The last command should list `uib0`. Use an absolute `LD_PRELOAD` path in
services, containers, remote shells, and scripts; their working directory may
not be the source tree.

## Minimal UIB-to-UIB example

The following example runs the unmodified `ibv_rc_pingpong` tool between two
hosts. Replace the example addresses with IPv4 addresses actually assigned to
the two hosts.

On the server at `198.51.100.1`:

```sh
env \
    LD_PRELOAD=/absolute/path/to/uib/libuibv.so \
    UIB_PROTOCOL=rocev2 \
    UIB_TRANSPORT_MODE=udp-io-uring \
    UIB_BIND_IP=198.51.100.1 \
    UIB_BIND_PORT=4791 \
    ibv_rc_pingpong -d uib0 -g 0 -m 1024 -s 2051 -n 1000 -c
```

On the client at `198.51.100.2`:

```sh
env \
    LD_PRELOAD=/absolute/path/to/uib/libuibv.so \
    UIB_PROTOCOL=rocev2 \
    UIB_TRANSPORT_MODE=udp-io-uring \
    UIB_BIND_IP=198.51.100.2 \
    UIB_BIND_PORT=4791 \
    ibv_rc_pingpong -d uib0 -g 0 -m 1024 -s 2051 -n 1000 -c \
    198.51.100.1
```

The final `198.51.100.1` is an argument to `ibv_rc_pingpong`; the tool uses it
for its client/server control connection. `UIB_BIND_IP` selects each process's
local UIB data-plane address. The server address appears in both roles here,
but the command argument and environment variable have different meanings.

Both endpoints must use the same wire protocol and service port, and must use
compatible transports. Using the same settings on both sides is the simplest
configuration. The selected port must be reachable through the host firewall
and network path.

For UET, change both endpoints to:

```sh
UIB_PROTOCOL=uet
UIB_BIND_PORT=5432
```

UET requires a UIB peer. It does not interoperate with a kernel RoCE device.

## Settings needed for transparent operation

These are the settings most likely to matter when launching an existing verbs
program:

- `LD_PRELOAD` loads the UIB verbs interposition library. Use the absolute path
  to `libuibv.so`, and set it in every process that should use `uib0`.
- `UIB_BIND_IP` selects the local UIB data-plane IPv4 address. Set a concrete
  address assigned to the host. The default `0.0.0.0` cannot be returned as a
  usable GID to applications that query one.
- `UIB_PROTOCOL` selects the RDMA wire protocol. Use `rocev2` (default) or
  `uet`; both endpoints must agree.
- `UIB_BIND_PORT` selects the local protocol service port. It defaults to 4791
  for RoCEv2 and 5432 for UET. Both endpoints must agree.
- `UIB_TRANSPORT_MODE` selects how UIB moves protocol packets.
  `udp-io-uring` is the default; `udp-sync` is a simpler comparison and debug
  path.
- `UIB_UDP_MODE` selects ordinary UDP sockets or raw RoCEv2 packets. Keep the
  default `socket` for UIB-to-UIB traffic. Use `raw` only for RXE
  interoperability; it requires `CAP_NET_RAW`.

UIB reads these values when the application opens `uib0`. Set them before the
program starts rather than changing them after verbs objects have been
created. The complete generated list, including io_uring, AF_XDP, zero-copy,
TCP devmem, and UET tuning, is in the
[environment-variable reference](environment-variables.md).

### Protocol and transport are separate choices

`UIB_PROTOCOL` controls the RDMA packet format and behavior. For example,
RoCEv2 and UET have different headers, default ports, reliability logic, and
congestion-control behavior.

`UIB_TRANSPORT_MODE` controls how those packets enter and leave the process:

- `udp-io-uring` uses an asynchronous UDP worker and is the normal default.
- `udp-sync` uses synchronous UDP operations and is useful for debugging and
  comparisons.
- `udp-zc-afxdp` requires AF_XDP configuration, a suitable interface and
  queue, and a peer MAC address.
- `tcp-devmem` uses UIB's framed TCP transport and requires a compatible UIB
  endpoint.

The two UDP socket modes, `udp-io-uring` and `udp-sync`, use the same wire
framing and can communicate with each other. Optional transports have extra
requirements documented in the
[environment-variable reference](environment-variables.md) and the
[DMA-BUF and zero-copy guide](dmabuf-zerocopy.md).

## Application behavior and compatibility

The application continues to use ordinary `struct ibv_*` objects. UIB wraps
those objects and translates supported operations to its native objects. The
application does not call the `uib_*` API directly.

Important compatibility details are:

- Port number `1`, GID index `0`, and P_Key index `0` describe the synthetic
  device. The GID is an IPv4-mapped address derived from `UIB_BIND_IP`.
- UIB's common transparent data path is RC. It supports SEND/RECV, RDMA WRITE,
  RDMA READ, immediate data, and standard 64-bit fetch-add and compare-swap.
- Host memory registered with `ibv_reg_mr_iova()` or `ibv_reg_mr_iova2()` must
  use the process virtual address as its IOVA. A different host-memory IOVA is
  rejected.
- `ibv_reg_dmabuf_mr()` is supported. Device-memory operation still depends on
  the selected transport and the DMA-BUF exporter's capabilities.
- Completion channels expose a pollable file descriptor and support one-shot
  CQ notification, `ibv_get_cq_event()`, and event acknowledgment.
- Device async events, shared receive queues, memory windows, XRC/DC, and
  other extended verbs are not implemented for `uib0`.
- Objects from `uib0` must not be mixed with PDs, CQs, QPs, or other objects
  belonging to a real device.

If an application selects a real RDMA device while `libuibv.so` is preloaded,
the hooks listed below forward that real-device operation to system
libibverbs. A call unsupported for `uib0` cannot be made to work by forwarding
it, because its objects belong to the UIB context.

## Deploying outside an interactive shell

For a service, put `LD_PRELOAD` and the `UIB_*` settings in the service's
environment and ensure the service can read `libuibv.so` and its dependent
libraries. For a container, mount or install the library and dependencies in
the container and assign `UIB_BIND_IP` to an address visible in its network
namespace.

Privilege-changing launchers may remove `LD_PRELOAD`. If `uib0` is missing,
inspect the environment seen by the final application process rather than only
the parent shell. Raw RoCEv2 and some optional transports also need additional
capabilities; ordinary UIB-to-UIB UDP socket mode does not require raw-socket
privileges.

## Troubleshooting

### `uib0` is not listed

- Confirm configure reported `libibverbs provider: 1`.
- Use an absolute path in `LD_PRELOAD`.
- Run `ldd /absolute/path/to/libuibv.so` and resolve missing libraries.
- Confirm the application is dynamically linked and receives `LD_PRELOAD`.
- Run `env LD_PRELOAD=/absolute/path/to/libuibv.so ibv_devices` separately
  from the application.

### Opening `uib0` fails

- Confirm `UIB_BIND_IP` is assigned in the process's network namespace.
- Check that `UIB_BIND_PORT` is valid and available.
- If an optional transport was selected, verify all of its required interface,
  queue, capability, and library settings.

### The connection starts but data does not move

- Check protocol and port agreement on both endpoints.
- Check transport compatibility and firewall rules.
- Use `UIB_TRANSPORT_MODE=udp-sync` on both endpoints to simplify debugging.
- Keep `UIB_UDP_MODE=socket` unless intentionally testing kernel RXE.
- For UET packet-level diagnostics, set `UIB_UET_DEBUG=1`.

### Device or GID selection fails

- Pass `-d uib0` or the application's equivalent device option.
- Use port `1` and GID index `0`.
- Set `UIB_BIND_IP` to a concrete local address rather than `0.0.0.0`.

## Validation targets

UIB includes tests using unmodified rdma-core tools:

```sh
make uib-tools-test
make uet-tools-test
make rxe-tools-test
```

`uib-tools-test` and `uet-tools-test` run `ibv_rc_pingpong` between two UIB
network namespaces. `rxe-tools-test` runs the UIB preload provider against a
kernel RXE peer in raw RoCEv2 mode. The RXE test needs the appropriate kernel
modules and administrative network privileges.

## Verbs hooked by `libuibv.so`

The provider directly interposes the following libibverbs entry points. For
objects that belong to real RDMA devices, it forwards these calls to the next
libibverbs implementation.

Device discovery and contexts:

- `ibv_get_device_list()`
- `ibv_free_device_list()`
- `ibv_get_device_name()`
- `ibv_open_device()`
- `ibv_close_device()`

Device and QP queries:

- `ibv_query_device()`
- `ibv_query_port()`
- `ibv_query_gid()`
- `ibv_query_pkey()`
- `ibv_query_qp()`

Protection domains and memory regions:

- `ibv_alloc_pd()`
- `ibv_dealloc_pd()`
- `ibv_reg_mr()`
- `ibv_reg_mr_iova()`
- `ibv_reg_mr_iova2()`
- `ibv_reg_dmabuf_mr()`
- `ibv_dereg_mr()`

Completion queues and event channels:

- `ibv_create_cq()`
- `ibv_destroy_cq()`
- `ibv_create_comp_channel()`
- `ibv_destroy_comp_channel()`
- `ibv_get_cq_event()`
- `ibv_ack_cq_events()`

Queue pairs:

- `ibv_create_qp()`
- `ibv_modify_qp()`
- `ibv_destroy_qp()`

Four common data-path verbs are normally inline libibverbs wrappers rather
than exported functions. UIB hooks them through the operation table installed
in the `uib0` context:

- `ibv_post_send()`
- `ibv_post_recv()`
- `ibv_poll_cq()`
- `ibv_req_notify_cq()`
