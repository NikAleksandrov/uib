<!-- SPDX-License-Identifier: Apache-2.0 -->
# Physical RoCE and AF_XDP interoperability

The physical endpoint runners deliberately do not configure a host. They make
the test commands reproducible while leaving interface MTU, queue steering,
privilege policy, and provider selection visible to the operator.

## AF_XDP setup

Build UIB and its selective XDP redirect program:

```sh
make -j"$(nproc)" libuibv.so examples
make afxdp-program
```

Choose one RX queue and steer the UIB UDP destination port to it on both
hosts. The example uses queue 62 and data port 19681:

```sh
sudo ethtool -N enp79s0np0 flow-type udp4 dst-port 19681 action 62
```

The interface MTU must include the IPv4 and UDP headers in addition to the UIB
protocol packet. For example, a 3000-byte UIB path packet needs an interface
MTU of at least 3028. It must also remain within the NIC driver's single-buffer
XDP limit unless the loaded XDP program supports multi-buffer packets. Check
the kernel log if XDP attachment returns `EINVAL`. A physical MTU of 3498 and
a UIB path MTU of 3000 are a useful mlx5 single-buffer configuration.

Run the server on the first host:

```sh
sudo -n env \
  UIB_AFXDP_ROLE=server \
  UIB_AFXDP_BIND=192.0.2.1 \
  UIB_AFXDP_PEER=192.0.2.2 \
  UIB_AFXDP_IFACE=enp79s0np0 \
  UIB_AFXDP_QUEUE=62 \
  UIB_AFXDP_PEER_MAC=02:00:00:00:00:02 \
  UIB_AFXDP_DATA_PORT=19681 \
  UIB_AFXDP_CONTROL_PORT=19781 \
  tests/run_physical_afxdp.sh
```

Run the client on the second host with its addresses and the first host's MAC:

```sh
sudo -n env \
  UIB_AFXDP_ROLE=client \
  UIB_AFXDP_BIND=192.0.2.2 \
  UIB_AFXDP_PEER=192.0.2.1 \
  UIB_AFXDP_IFACE=enp79s0np0 \
  UIB_AFXDP_QUEUE=62 \
  UIB_AFXDP_PEER_MAC=02:00:00:00:00:01 \
  UIB_AFXDP_DATA_PORT=19681 \
  UIB_AFXDP_CONTROL_PORT=19781 \
  tests/run_physical_afxdp.sh
```

The runner defaults to UET, required zero-copy, 60,000-byte operations, a
128-operation window, and a 3000-byte protocol MTU. Override any value through
the `UIB_AFXDP_*` variables documented at the top of the script. Set
`UIB_AFXDP_PROGRAM=off` to let libxdp install its generic XSK redirect program.

Remove a temporary steering rule by its reported rule ID when testing ends:

```sh
sudo ethtool -N enp79s0np0 delete 1023
```

## UIB and hardware RoCE

`tests/run_physical_roce_interop.sh` runs one endpoint of an
`ibv_rc_pingpong` pair. Start a native mlx5 or RXE server without preloading
UIB:

```sh
env \
  UIB_INTEROP_ENDPOINT=native \
  UIB_INTEROP_ROLE=server \
  UIB_INTEROP_DEVICE=mlx5_2 \
  UIB_INTEROP_GID_INDEX=3 \
  UIB_INTEROP_CONTROL_PORT=19801 \
  tests/run_physical_roce_interop.sh
```

Run a raw RoCEv2 UIB client on the peer:

```sh
sudo -n env \
  UIB_INTEROP_ENDPOINT=uib \
  UIB_INTEROP_ROLE=client \
  UIB_INTEROP_TRANSPORT=raw \
  UIB_INTEROP_BIND=192.0.2.2 \
  UIB_INTEROP_SERVER=192.0.2.1 \
  UIB_INTEROP_CONTROL_PORT=19801 \
  tests/run_physical_roce_interop.sh
```

Set `UIB_INTEROP_TRANSPORT=udp` to run the same mixed-provider test through
UIB's ordinary kernel UDP socket. This is a separate control from raw RoCEv2:
the kernel creates and consumes the IP/UDP headers, while raw mode gives UIB
the complete frame metadata used by its interoperability path.

Reverse the roles and repeat. For an AF_XDP UIB endpoint, set the transport to
`afxdp`, configure `UIB_INTEROP_IFACE`, `UIB_INTEROP_QUEUE`, and
`UIB_INTEROP_PEER_MAC`, and steer UDP destination port 4791 to that queue.
Native RoCE offload and netdev XDP steering are hardware- and driver-specific;
the raw mode is the control for wire-format interoperability.

### Dedicated mlx5 port mode

An mlx5 PF with native RoCE enabled consumes UDP/4791 in its hardware RDMA
pipeline before PF_PACKET, raw sockets, XDP, AF_XDP, or UDP can receive it.
UIB queue-pair numbers are not registered as mlx5 hardware QPs, so a UIB
endpoint cannot receive mixed-provider traffic through those host transports
while native RoCE remains enabled on the same PF.

For an mlx5 port dedicated to the UIB endpoint, disable only RoCE processing.
Leave the Ethernet and general RDMA auxiliary functions enabled:

```sh
devlink dev param show pci/0000:4f:00.0 name enable_roce
sudo devlink dev param set pci/0000:4f:00.0 \
  name enable_roce value false cmode driverinit
sudo devlink dev reload pci/0000:4f:00.0
```

Replace the PCI address with the one reported by `ethtool -i INTERFACE`.
The parameter has `driverinit` mode, so the reload is required. Confirm that
the netdev retained its address and that the parameter is false before
starting UIB. Keep RoCE enabled on the native mlx5 peer.

Use UIB raw or AF_XDP mode for the data path. Ordinary UIB UDP socket mode is
not a native RoCEv2 interoperability path because it does not provide the
complete outer-header metadata needed by the wire ICRC implementation.

Restore native RoCE after the test:

```sh
sudo devlink dev param set pci/0000:4f:00.0 \
  name enable_roce value true cmode driverinit
sudo devlink dev reload pci/0000:4f:00.0
devlink dev param show pci/0000:4f:00.0 name enable_roce
```

This mode dedicates the PF to UIB for the duration of the run. Concurrent
native mlx5 and UIB QPs on one RoCE-enabled PF require mlx5 hardware flow
steering before the native RoCE classifier, such as a DevX/direct-rule
transport that steers a reserved UIB destination-QPN range to a userspace raw
packet receive queue. Netdev ntuple rules and XDP run too late to provide that
coexistence.

For RXE-to-RXE comparison, use `native` endpoints on both hosts, select each
RXE device, and use its IPv4 RoCEv2 GID index. The runner always removes
`LD_PRELOAD` from native endpoints so a UIB provider cannot be selected by
accident.
