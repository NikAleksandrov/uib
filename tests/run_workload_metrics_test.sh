#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
set -eu

project_dir=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
python3 - "$project_dir/simulation/bin/uib-sim" <<'PY'
import importlib.machinery
import importlib.util
import json
import pathlib
import tempfile
import sys


loader = importlib.machinery.SourceFileLoader("uib_sim", sys.argv[1])
spec = importlib.util.spec_from_loader(loader.name, loader)
module = importlib.util.module_from_spec(spec)
loader.exec_module(module)


def config(kind):
    return {
        "runId": "uib-metrics",
        "workload": {
            "kind": kind,
            "parallelism": {"nodes": 2, "processesPerNode": 1},
        },
        "uib": {
            "enabled": True,
            "protocol": "rocev2",
            "memoryMode": "host-staged",
            "congestionControl": {
                "algorithm": "dcqcn", "abi": 1, "parameters": {},
            },
        },
        "communication": {"frontend": "nccl"},
    }


training = {
    "schemaVersion": 1,
    "runId": "uib-metrics",
    "status": "passed",
    "worldSize": 2,
    "device": "cpu",
    "strategy": "ddp",
    "completedSteps": 4,
    "resumedFromStep": 0,
    "durationSeconds": 2.0,
    "samplesPerSecond": 8.0,
    "loss": {"first": 4.0, "last": 2.0},
}
inference = {
    "schemaVersion": 1,
    "runId": "uib-metrics",
    "status": "passed",
    "backend": "transformers",
    "durationSeconds": 4.0,
    "requests": 4,
    "outputTokens": 16,
    "requestsPerSecond": 1.0,
    "outputTokensPerSecond": 4.0,
    "latencySeconds": {
        "minimum": 0.5,
        "median": 0.75,
        "p95": 1.0,
        "maximum": 1.25,
    },
    "parallelism": {
        "tensorParallel": 1,
        "pipelineParallel": 1,
        "dataParallel": 1,
        "replicas": 1,
    },
}

with tempfile.TemporaryDirectory(prefix="uib-workload-metrics.") as value:
    root = pathlib.Path(value)
    destination = root / "metrics.jsonl"
    (root / "training.json").write_text(
        json.dumps(training, indent=2) + "\n", encoding="utf-8")
    (root / "training.log").write_text(
        json.dumps(training) + "\n", encoding="utf-8")
    records = module.collect_workload_metrics(
        config("training"), [("workload", root)], destination)
    assert len(records) == 1
    assert records[0]["samplesPerSecond"] == 8.0
    assert records[0]["loss"] == {"first": 4.0, "last": 2.0}
    assert records[0]["sourceFiles"] == [
        "workload/training.json", "workload/training.log"]
    training_records = records

    (root / "inference.json").write_text(
        json.dumps(inference) + "\n", encoding="utf-8")
    records = module.collect_workload_metrics(
        config("inference"), [("workload", root)], destination)
    assert len(records) == 1
    assert records[0]["requestsPerSecond"] == 1.0
    assert records[0]["outputTokensPerSecond"] == 4.0
    assert records[0]["latencySeconds"]["p95"] == 1.0

    invalid = dict(inference)
    invalid["durationSeconds"] = float("nan")
    assert module.normalize_workload_result(
        config("inference"), invalid) is None
    invalid = dict(training)
    invalid["runId"] = "another-run"
    assert module.normalize_workload_result(config("training"), invalid) is None

    evidence = {"classification": "confirmed"}
    kubernetes = module.normalized_result(
        config("training"), "kubernetes", "COMPLETED", True,
        training_records,
        evidence, evidence, 2, ["scheduler/events.yaml"])
    slurm = module.normalized_result(
        config("training"), "slurm", "COMPLETED", True, training_records,
        evidence, evidence, 2, ["scheduler/events.yaml"])
    assert kubernetes["workload"] == slurm["workload"]
    assert kubernetes["communication"] == slurm["communication"]
    assert kubernetes["evidence"] == slurm["evidence"]
    assert kubernetes["scheduler"]["kind"] == "kubernetes"
    assert slurm["scheduler"]["kind"] == "slurm"

print("normalized workload metrics test: PASS")
PY
