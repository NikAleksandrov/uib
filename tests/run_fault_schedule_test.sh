#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
set -eu

project_dir=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
cli=$project_dir/simulation/bin/uib-sim
schedule=$project_dir/simulation/faults/network-schedule
test_dir=$(mktemp -d "${TMPDIR:-/tmp}/uib-fault-schedule.XXXXXX")
trap 'rm -rf -- "$test_dir"' EXIT INT TERM

mkdir "$test_dir/bin"
cat > "$test_dir/bin/netem" <<'EOF'
#!/bin/sh
set -eu
[ "$1" = apply ]
jq -c '.network.shaping' "$2" >> "$FAULT_TEST_COMMANDS"
EOF
cat > "$test_dir/bin/ip" <<'EOF'
#!/bin/sh
set -eu
printf 'ip %s\n' "$*" >> "$FAULT_TEST_COMMANDS"
EOF
cat > "$test_dir/bin/sudo" <<'EOF'
#!/bin/sh
set -eu
[ "$1" = -n ]
shift
printf 'sudo %s\n' "$*" >> "$FAULT_TEST_PRIVILEGE"
exec "$@"
EOF
chmod +x "$test_dir/bin/netem" "$test_dir/bin/ip" \
	"$test_dir/bin/sudo"

cat > "$test_dir/faults.yaml" <<'EOF'
faults:
  - at: 1ms
    action: add-delay
    target: worker-01/data
    latency: 5ms
    duration: 3ms
  - at: 2ms
    action: packet-loss
    target: worker-01/data
    percent: 2
    duration: 1ms
  - at: 2ms
    action: link-down
    target: worker-01/data
    duration: 1ms
EOF
"$cli" render --profile virtme-kubernetes --overlay network/clean \
	--request "$test_dir/faults.yaml" --format json \
	--set 'network.endpointShaping={worker-01/data: {enabled: true, latency: 7ms}}' \
	> "$test_dir/config.json"

FAULT_NETEM="$test_dir/bin/netem" FAULT_IP="$test_dir/bin/ip" \
FAULT_TEST_COMMANDS="$test_dir/commands" \
	"$schedule" "$test_dir/config.json" --target worker-01/data \
	--interface eth-test --output "$test_dir/faults.jsonl"

python3 - "$test_dir/faults.jsonl" "$test_dir/commands" <<'PY'
import json
import pathlib
import sys

records = [json.loads(line) for line in pathlib.Path(sys.argv[1]).read_text(
    encoding="utf-8").splitlines()]
assert len(records) == 6
assert all(record["result"] == "applied" for record in records)
assert [record["phase"] for record in records].count("apply") == 3
assert [record["phase"] for record in records].count("recover") == 3
assert records[-1]["activeFaults"] == []
assert records[-1]["commands"][-1]["shaping"] == {
    "enabled": True, "latency": "7ms"}
commands = pathlib.Path(sys.argv[2]).read_text(encoding="utf-8")
assert '"latency":"5ms"' in commands
assert '"lossPercent":2' in commands
assert '"latency":"7ms"' in commands
assert "ip link set dev eth-test down" in commands
assert "ip link set dev eth-test up" in commands
PY

: > "$test_dir/privilege.commands"
FAULT_TEST_COMMANDS="$test_dir/commands" \
FAULT_TEST_PRIVILEGE="$test_dir/privilege.commands" \
	"$schedule" "$test_dir/config.json" --target worker-01/data \
	--interface eth-test --output "$test_dir/privileged.jsonl" \
	--netem "$test_dir/bin/netem" --ip-command "$test_dir/bin/ip" \
	--privilege-command "$test_dir/bin/sudo"
grep -F 'sudo ' "$test_dir/privilege.commands" > /dev/null
grep -F "$test_dir/bin/netem apply" \
	"$test_dir/privilege.commands" > /dev/null
grep -F "$test_dir/bin/ip link set dev eth-test down" \
	"$test_dir/privilege.commands" > /dev/null

if FAULT_NETEM="$test_dir/bin/netem" FAULT_IP="$test_dir/bin/ip" \
	FAULT_TEST_COMMANDS="$test_dir/commands" \
	"$schedule" "$test_dir/config.json" --target missing/data \
	--interface eth-test --output "$test_dir/missing.jsonl" \
	> /dev/null 2>&1; then
	echo "a missing fault target was accepted" >&2
	exit 1
fi

cat > "$test_dir/interrupted.yaml" <<'EOF'
faults:
  - at: 1ms
    action: link-down
    target: worker-01/data
    duration: 10s
EOF
"$cli" render --profile kind-kubernetes --overlay network/clean \
	--request "$test_dir/interrupted.yaml" --format json \
	> "$test_dir/interrupted.json"
: > "$test_dir/interrupted.commands"
FAULT_NETEM="$test_dir/bin/netem" FAULT_IP="$test_dir/bin/ip" \
FAULT_TEST_COMMANDS="$test_dir/interrupted.commands" \
	"$schedule" "$test_dir/interrupted.json" --target worker-01/data \
	--interface eth-test --output "$test_dir/interrupted.jsonl" \
	> /dev/null 2>&1 &
schedule_pid=$!
for _ in 1 2 3 4 5 6 7 8 9 10; do
	grep -q ' down$' "$test_dir/interrupted.commands" && break
	sleep 0.01
done
kill -TERM "$schedule_pid"
if wait "$schedule_pid"; then
	echo "an interrupted fault schedule reported success" >&2
	exit 1
fi
grep -q ' down$' "$test_dir/interrupted.commands"
grep -q ' up$' "$test_dir/interrupted.commands"
python3 - "$test_dir/interrupted.jsonl" <<'PY'
import json
import pathlib
import sys

records = [json.loads(line) for line in pathlib.Path(sys.argv[1]).read_text(
    encoding="utf-8").splitlines()]
assert records[-1]["phase"] == "cleanup"
assert records[-1]["result"] == "restored"
assert records[-1]["commands"][-1]["shaping"] == {"enabled": False}
PY

echo "network fault schedule test: PASS"
