#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
set -eu

project_dir=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
python3 - "$project_dir/simulation/bin/uib-sim" <<'PY'
import importlib.machinery
import importlib.util
import pathlib
import tempfile
import sys


loader = importlib.machinery.SourceFileLoader("uib_sim", sys.argv[1])
spec = importlib.util.spec_from_loader(loader.name, loader)
module = importlib.util.module_from_spec(spec)
loader.exec_module(module)


def config(frontend="nccl", ranks=2, enabled=True):
    return {
        "communication": {"frontend": frontend},
        "uib": {"enabled": enabled},
        "workload": {
            "parallelism": {"nodes": ranks, "processesPerNode": 1},
        },
    }


def classify(root, content, frontend="nccl", ranks=2, enabled=True):
    (root / "run.log").write_text(content, encoding="utf-8")
    return module.transport_evidence(
        config(frontend, ranks, enabled), [("logs", root)])


with tempfile.TemporaryDirectory(prefix="uib-transport-evidence.") as value:
    root = pathlib.Path(value)
    nccl = classify(root,
                    "NCCL_IBVERBS_LIB set by environment to "
                    "/opt/uib/lib/libuibv.so\nvia NET/IB/0\n")
    assert nccl["classification"] == "confirmed"
    assert nccl["positiveMatches"] == ["uib-library", "ib-data-path"]

    rccl = classify(root,
                    '"ncclIbverbsLibrary": "/opt/uib/lib/libuibv.so"\n'
                    "Initialized NET plugin IB\n", frontend="rccl")
    assert rccl["classification"] == "confirmed"

    fallback = classify(root,
                        "NCCL_IBVERBS_LIB=/opt/uib/lib/libuibv.so\n"
                        "Using network IB\nvia NET/Socket/0\n")
    assert fallback["classification"] == "fallback-detected"
    assert fallback["forbiddenMatches"] == ["socket-data-path"]

    ucx = classify(root, "selected ud_verbs/uib0:1\n", frontend="ucx")
    assert ucx["classification"] == "confirmed"
    ucx_fallback = classify(
        root, "selected rc_verbs/uib0:1, tcp/eth0\n", frontend="ucx")
    assert ucx_fallback["classification"] == "fallback-detected"

    fabric = classify(
        root, "provider verbs;ofi_rxd device uib0-dgram\n",
        frontend="libfabric")
    assert fabric["classification"] == "confirmed"
    fabric_fallback = classify(
        root,
        "provider verbs;ofi_rxd device uib0-dgram\nprovider: sockets\n",
        frontend="libfabric")
    assert fabric_fallback["classification"] == "fallback-detected"

    insufficient = classify(root, "Using network IB\n")
    assert insufficient["classification"] == "insufficient"
    single = classify(root, "via NET/IB/0\n", ranks=1)
    assert single["classification"] == "not-exercised-single-rank"
    gloo = classify(root, '{"distributedBackend": "gloo"}\n')
    assert gloo["classification"] == "not-exercised-non-rdma-backend"
    native = classify(root, "", enabled=False)
    assert native["classification"] == "native-baseline"

    (root / "large.log").write_bytes(b"x" * (4 * 1024 * 1024 + 1))
    bounded = module.transport_evidence(config(), [("logs", root)])
    assert "logs/large.log" in bounded["skippedFiles"]
    assert bounded["bytesInspected"] <= 32 * 1024 * 1024

print("transport evidence classifier test: PASS")
PY
