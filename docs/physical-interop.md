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

Reverse the roles and repeat. For an AF_XDP UIB endpoint, set the transport to
`afxdp`, configure `UIB_INTEROP_IFACE`, `UIB_INTEROP_QUEUE`, and
`UIB_INTEROP_PEER_MAC`, and steer UDP destination port 4791 to that queue.
Native RoCE offload and netdev XDP steering are hardware- and driver-specific;
the raw mode is the control for wire-format interoperability.

For RXE-to-RXE comparison, use `native` endpoints on both hosts, select each
RXE device, and use its IPv4 RoCEv2 GID index. The runner always removes
`LD_PRELOAD` from native endpoints so a UIB provider cannot be selected by
accident.
