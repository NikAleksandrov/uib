#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
set -eu

project_dir=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
cli=$project_dir/simulation/bin/uib-sim
schedule=$project_dir/simulation/faults/network-schedule
loopback=$project_dir/tests/loopback
test_dir=$(mktemp -d "${TMPDIR:-/tmp}/uib-fault-e2e.XXXXXX")
trap 'rm -rf -- "$test_dir"' EXIT INT TERM

if [ "${1:-}" = inner ]; then
	config=$2
	evidence=$3
	action=$4
	tc_command=${NETEM_TC:-/sbin/tc}
	schedule_pid=
	workload_pid=

	cleanup()
	{
		if [ -n "$schedule_pid" ] && kill -0 "$schedule_pid" 2>/dev/null; then
			kill -TERM "$schedule_pid"
			wait "$schedule_pid" || true
		fi
		if [ -n "$workload_pid" ] && kill -0 "$workload_pid" 2>/dev/null; then
			kill -TERM "$workload_pid"
			wait "$workload_pid" || true
		fi
	}
	trap cleanup EXIT INT TERM
	ip link set lo up
	"$schedule" "$config" --target loopback/data --interface lo \
		--output "$evidence" &
	schedule_pid=$!
	active=false
	for _ in $(seq 1 100); do
		if "$tc_command" qdisc show dev lo | grep -q 'qdisc netem '; then
			active=true
			break
		fi
		sleep 0.01
	done
	if [ "$active" != true ]; then
		echo "scheduled netem condition did not become active" >&2
		exit 1
	fi
	UIB_PROTOCOL=rocev2 UIB_TRANSPORT_MODE=udp-sync UIB_ZEROCOPY=off \
		"$loopback" > "$evidence.workload" &
	workload_pid=$!
	traffic=false
	for _ in $(seq 1 50); do
		"$tc_command" -s qdisc show dev lo > "$evidence.qdisc"
		pattern='Sent [1-9][0-9]* bytes'
		[ "$action" = partition ] && pattern='dropped [1-9][0-9]*'
		if grep -Eq "$pattern" "$evidence.qdisc"; then
			traffic=true
			break
		fi
		sleep 0.01
	done
	if [ "$traffic" != true ]; then
		echo "scheduled condition recorded no UIB traffic" >&2
		exit 1
	fi
	wait "$workload_pid"
	workload_pid=
	cat "$evidence.workload"
	wait "$schedule_pid"
	schedule_pid=
	if "$tc_command" qdisc show dev lo | grep -q 'qdisc netem '; then
		echo "scheduled netem condition was not removed" >&2
		exit 1
	fi
python3 - "$evidence" "$action" <<'PY'
import json
import pathlib
import sys

records = [json.loads(line) for line in pathlib.Path(sys.argv[1]).read_text(
    encoding="utf-8").splitlines()]
assert len(records) == 2
assert records[0]["phase"] == "apply"
assert records[0]["action"] == sys.argv[2]
if sys.argv[2] == "add-delay":
    assert records[0]["commands"][0]["shaping"]["latency"] == "2ms"
else:
    assert records[0]["commands"][0]["shaping"]["lossPercent"] == 100
assert records[1]["phase"] == "recover"
assert records[1]["activeFaults"] == []
assert records[1]["commands"][0]["shaping"] == {"enabled": False}
PY
	echo "UIB scheduled network fault test: PASS"
	exit
fi

tc_command=${NETEM_TC:-$(command -v tc || true)}
[ -n "$tc_command" ] || tc_command=/sbin/tc
for command_name in ip jq seq timeout unshare; do
	if ! command -v "$command_name" >/dev/null 2>&1; then
		echo "required fault test command not found: $command_name" >&2
		exit 1
	fi
done
if [ ! -x "$tc_command" ]; then
	echo "required fault test tc command not found: $tc_command" >&2
	exit 1
fi

cat > "$test_dir/fault.yaml" <<'EOF'
faults:
  - at: 20ms
    action: add-delay
    target: loopback/data
    latency: 2ms
    duration: 1s
EOF
cat > "$test_dir/partition.yaml" <<'EOF'
faults:
  - at: 20ms
    action: partition
    target: loopback/data
    duration: 500ms
EOF
"$cli" render --profile kind-kubernetes --overlay network/clean \
	--request "$test_dir/fault.yaml" --format json > "$test_dir/config.json"
"$cli" render --profile kind-kubernetes --overlay network/clean \
	--request "$test_dir/partition.yaml" --format json \
	> "$test_dir/partition.json"

if [ "$(id -u)" -eq 0 ]; then
	runner=(unshare --net)
elif unshare --user --map-root-user --net true >/dev/null 2>&1; then
	runner=(unshare --user --map-root-user --net)
elif [ -n "${SIMULATION_NETNS_PRIVILEGE:-}" ]; then
	read -r -a privilege <<<"$SIMULATION_NETNS_PRIVILEGE"
	runner=("${privilege[@]}" unshare --net)
else
	echo "network-namespace privilege is required; set SIMULATION_NETNS_PRIVILEGE" >&2
	exit 1
fi

NETEM_TC="$tc_command" FAULT_IP="$(command -v ip)" \
	timeout -k 5 10 "${runner[@]}" "$0" inner \
	"$test_dir/config.json" "$test_dir/faults.jsonl" add-delay
NETEM_TC="$tc_command" FAULT_IP="$(command -v ip)" \
	timeout -k 5 10 "${runner[@]}" "$0" inner \
	"$test_dir/partition.json" "$test_dir/partition.jsonl" partition
