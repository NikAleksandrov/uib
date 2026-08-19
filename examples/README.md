UIB examples
============

Build the library and examples from the repository root:

    make examples

The programs use loopback addresses and create both endpoints themselves, so
they are easy to run in a virtme-ng guest or as root on a development host.
The default UDP transport opens a raw socket. The automated targets run them in
virtme-ng, where the required network capability is already available.

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

Use `make examples-test` for short correctness runs over all protocols and
`make examples-stress-test` for short windowed traffic runs over all protocols.
