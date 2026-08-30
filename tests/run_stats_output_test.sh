#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
set -eu

project_dir=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
test_dir=$(mktemp -d)
trap 'rm -rf "$test_dir"' EXIT HUP INT TERM

UIB_STATS_OUTPUT=stderr UIB_TRANSPORT_MODE=udp-sync UIB_UDP_WORKERS=2 \
	UIB_UDP_WORKER_AFFINITY=off \
	"$project_dir/tests/loopback" > "$test_dir/stdout" 2> "$test_dir/stderr"
python3 - "$test_dir/stderr" <<'PY'
import json
import pathlib
import sys


lines = pathlib.Path(sys.argv[1]).read_text(encoding="utf-8").splitlines()
records = [
    json.loads(line.removeprefix("UIB_STATS "))
    for line in lines if line.startswith("UIB_STATS ")
]
assert len(records) == 2
assert [record["contextSequence"] for record in records] == [1, 2]
assert len({record["processToken"] for record in records}) == 1
assert all(record["schemaVersion"] == 1 for record in records)
assert all(record["protocol"] == "rocev2" for record in records)
assert all(record["transport"] == "udp-sync" for record in records)
assert all(record["transportCountersAvailable"] for record in records)
assert sum(record["transportCounters"]["txCompleted"]
           for record in records) > 0
assert sum(record["transportCounters"]["rxPackets"]
           for record in records) > 0
assert all(record["transportCounters"]["rxWorkers"] == 2
           for record in records)
assert all(record["transportCounters"]["rxWorkersPinned"] == 0
           for record in records)
assert all(record["transportCounters"]["rxDispatchDrops"] == 0
           for record in records)
assert sum(record["transportCounters"]["rxPoolHits"]
           for record in records) > 0
assert sum(record["transportCounters"]["rxPoolHits"] +
           record["transportCounters"]["rxPoolMisses"]
           for record in records) == sum(
               record["transportCounters"]["rxPackets"]
               for record in records)
assert all("zeroCopyCounters" in record for record in records)
PY

echo "stats output test: PASS"
