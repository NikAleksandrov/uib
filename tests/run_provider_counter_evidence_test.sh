#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
set -eu

project_dir=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
python3 - "$project_dir/simulation/bin/uib-sim" <<'PY'
import importlib.machinery
import importlib.util
import json
import pathlib
import sys
import tempfile


loader = importlib.machinery.SourceFileLoader("uib_sim", sys.argv[1])
spec = importlib.util.spec_from_loader(loader.name, loader)
module = importlib.util.module_from_spec(spec)
loader.exec_module(module)


def config(ranks=2, enabled=True):
    return {
        "uib": {
            "enabled": enabled,
            "protocol": "rocev2",
            "transport": "udp-sync",
        },
        "workload": {
            "parallelism": {"nodes": ranks, "processesPerNode": 1},
        },
    }


def record(protocol="rocev2", transport="udp-sync", tx=4, rx=3,
           errors=0, available=True, process=1):
    counters = None
    if available:
        counters = {
            "txQueued": tx,
            "txSubmitted": tx,
            "txCompleted": tx,
            "txErrors": errors,
            "txGsoBatches": 0,
            "txGsoPackets": 0,
            "rxPackets": rx,
            "rxGroBatches": 0,
            "rxCompletionBatches": rx,
        }
    return {
        "schemaVersion": 1,
        "pid": 100,
        "processToken": process,
        "contextSequence": 1,
        "protocol": protocol,
        "transport": transport,
        "transportCountersAvailable": available,
        "transportCounters": counters,
        "zeroCopyCounters": {
            "copiedCalls": tx,
            "copiedBytes": tx * 64,
            "zeroCopyCalls": 0,
            "zeroCopyBytes": 0,
            "completions": 0,
            "deferredCopies": 0,
            "errors": 0,
        },
    }


def classify(root, records, ranks=2, enabled=True):
    content = "\n".join("launcher: UIB_STATS " + json.dumps(item)
                        for item in records)
    (root / "run.err").write_text(content + "\n", encoding="utf-8")
    return module.provider_counter_evidence(
        config(ranks, enabled), [("logs", root)])


with tempfile.TemporaryDirectory(prefix="uib-provider-counters.") as value:
    root = pathlib.Path(value)
    observed = classify(root, [record(), record(tx=2, rx=5, process=2)])
    assert observed["classification"] == "traffic-observed"
    assert observed["recordCount"] == 2
    assert observed["aggregateTransportCounters"]["txCompleted"] == 6
    assert observed["aggregateTransportCounters"]["rxPackets"] == 8
    assert observed["aggregateZeroCopyCounters"]["copiedCalls"] == 6

    no_traffic = classify(root, [record(tx=0, rx=0),
                                 record(tx=0, rx=0, process=2)])
    assert no_traffic["classification"] == "no-traffic"
    failed = classify(root, [record(errors=1), record(process=2)])
    assert failed["classification"] == "errors-observed"
    mismatch = classify(root, [record(transport="udp-io-uring"),
                               record(process=2)])
    assert mismatch["classification"] == "backend-mismatch"
    unavailable = classify(root, [record(available=False), record(process=2)])
    assert unavailable["classification"] == "counters-unavailable"
    invalid = classify(root, [{"schemaVersion": 1}])
    assert invalid["classification"] == "invalid"
    partial = classify(root, [record()])
    assert partial["classification"] == "incomplete-rank-coverage"
    single = classify(root, [record()], ranks=1)
    assert single["classification"] == "not-exercised-single-rank"
    native = classify(root, [], enabled=False)
    assert native["classification"] == "native-baseline"
    empty = classify(root, [])
    assert empty["classification"] == "insufficient"

print("provider counter evidence test: PASS")
PY
