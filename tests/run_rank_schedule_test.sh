#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
set -eu

project_dir=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
scheduler=$project_dir/simulation/faults/rank-schedule
test_dir=$(mktemp -d "${TMPDIR:-/tmp}/uib-rank-schedule.XXXXXX")
trap 'rm -rf -- "$test_dir"' EXIT INT TERM

cat > "$test_dir/config.json" <<'EOF'
{
  "faults": [
    {"at": "10ms", "action": "terminate-rank", "rank": 1}
  ]
}
EOF
"$scheduler" "$test_dir/config.json" --marker-dir "$test_dir" \
	--output "$test_dir/evidence.jsonl"

python3 - "$test_dir/evidence.jsonl" \
	"$test_dir/.uib-terminate-rank-1" <<'PY'
import json
import pathlib
import sys


record = json.loads(pathlib.Path(sys.argv[1]).read_text(encoding="utf-8"))
assert record["schemaVersion"] == 1
assert record["sourceIndex"] == 0
assert record["phase"] == "apply"
assert record["action"] == "terminate-rank"
assert record["target"] == "rank-1"
assert record["rank"] == 1
assert record["plannedOffsetSeconds"] == 0.01
assert record["actualOffsetSeconds"] >= 0.01
assert record["result"] == "applied"
assert pathlib.Path(sys.argv[2]).read_text(encoding="utf-8") == "0\n"
PY

cat > "$test_dir/duplicate.json" <<'EOF'
{
  "faults": [
    {"at": "0ms", "action": "terminate-rank", "rank": 0},
    {"at": "1s", "action": "terminate-rank", "rank": 1}
  ]
}
EOF
if "$scheduler" "$test_dir/duplicate.json" --marker-dir "$test_dir" \
	--output "$test_dir/duplicate.jsonl" > /dev/null 2>&1; then
	echo "multiple rank faults were accepted" >&2
	exit 1
fi

mkdir "$test_dir/integration"
cat > "$test_dir/integration.json" <<'EOF'
{
  "faults": [
    {"at": "20ms", "action": "terminate-rank", "rank": 0}
  ]
}
EOF
sh -c '
	rank_fault_pid=$$
	(
		marker=$1/.uib-terminate-rank-0
		while [ ! -e "$marker" ]; do sleep 0.01; done
		printf "%s\n" "$rank_fault_pid" > "$marker.ack"
		kill -TERM "$rank_fault_pid"
	) &
	exec sleep 10
' uib-rank-victim "$test_dir/integration" &
victim=$!
"$scheduler" "$test_dir/integration.json" \
	--marker-dir "$test_dir/integration" \
	--output "$test_dir/integration.jsonl"
if wait "$victim"; then
	echo "rank marker did not terminate the wrapped process" >&2
	exit 1
fi
[ -s "$test_dir/integration/.uib-terminate-rank-0.ack" ]

echo "rank fault schedule test: PASS"
