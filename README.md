UIB is a userspace RDMA implementation with independent wire-protocol and
packet-transport layers.

Select the wire protocol with UIB_PROTOCOL:

    rocev2  InfiniBand RC over RoCEv2 (default, UDP port 4791)
    uet     Ultra Ethernet PDS/RUD PDC with SES SEND/WRITE/READ (UDP port 5432)
    dummy   Minimal SEND-only protocol used to test the protocol boundary

Select the packet transport with UIB_TRANSPORT_MODE:

    udp-blocking  UDP/raw IPv4 backend (default)
    udp-zc-afxdp  AF_XDP backend when libxdp and a capable interface are present
    tcp-devmem    framed TCP backend with optional device-memory receive support

UET enables the specification-defined NSCC congestion controller by default.
Set UIB_UET_CC=none only for comparison testing. UIB_UET_NSCC_MAX_WND can cap
the initial congestion window in tests; applications can configure and query a
QP with uib_uet_nscc_configure_qp() and uib_uet_nscc_query_qp().

Run the complete test matrix, including virtme-ng, kernel RXE interoperability,
standard ibverbs tools, UET wire-layout checks, and deterministic UET packet-loss
recovery, with:

    make test-all

The current UET implementation follows the PDS/PDC and SES layouts in the
uet-linux-kernel prototype. It implements RUD connection establishment, a
128-packet receive window, cumulative plus exact ACK processing, retransmission,
and UIB's existing SEND, RDMA WRITE, and RDMA READ operations. The reference
kernel driver is a PDS netdevice/character-device prototype rather than an RDMA
verbs provider, so UET kernel-verbs interoperability is not claimed.
