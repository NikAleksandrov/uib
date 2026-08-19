UIB is a userspace RDMA implementation with independent wire-protocol and
packet-transport layers.

Select the wire protocol with UIB_PROTOCOL:

    rocev2  InfiniBand RC over RoCEv2 (default, UDP port 4791)
    uet     Ultra Ethernet PDS/RUD PDC, SES and NSCC (UDP port 5432)
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

Build and run the native UIB, libibverbs and windowed traffic examples with:

    make examples
    make examples-test
    make examples-stress-test

See examples/README.md for the individual commands and benchmark options.

The UET implementation follows Ultra Ethernet Specification 1.0.3. It provides
RUD connection establishment, the 128-packet PDC receive window, cumulative and
exact acknowledgement processing, retransmission, standard SES identity and
fragment fields, MTU-sized response-data packets, SEND with header data, RDMA
WRITE/READ, fetching 64-bit sum and compare-swap atomics, and the specification
NSCC algorithm. Retransmitted fetching atomics use cached responses so an ACK
loss cannot apply an atomic operation twice.

The current implementation is an interoperable subset rather than a claim to
implement every UE 1.0.3 profile. It does not yet implement ROD/RUDI/UUD PDS
delivery, optimized small/medium SES request headers, tagged/unexpected matching,
rendezvous and deferrable-send transactions, the optional full datatype/atomic
matrix, RCCC/TFC, TSS/security, or multipath. The reference kernel driver is a
PDS netdevice/character-device prototype rather than an RDMA verbs provider, so
UET kernel-verbs interoperability is not claimed.
