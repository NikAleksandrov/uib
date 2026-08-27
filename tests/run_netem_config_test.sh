#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
set -eu

project_dir=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
cli=$project_dir/simulation/bin/uib-sim
netem=$project_dir/simulation/network/netem
test_dir=$(mktemp -d "${TMPDIR:-/tmp}/uib-netem-config-test.XXXXXX")
trap 'rm -rf -- "$test_dir"' EXIT INT TERM

for condition in clean datacenter constrained lossy bursty reordered ecn; do
	"$cli" render --profile kind-kubernetes \
		--overlay "network/$condition" --format json \
		> "$test_dir/$condition.json"
	"$netem" render "$test_dir/$condition.json" eth-test \
		> "$test_dir/$condition.command"
done

"$cli" render --profile virtme-kubernetes --overlay network/asymmetric \
	--format json > "$test_dir/asymmetric.json"
"$netem" render "$test_dir/asymmetric.json" eth-test \
	> "$test_dir/asymmetric.command"

"$netem" render "$test_dir/asymmetric.json" eth-test \
	--target worker-01/data > "$test_dir/asymmetric-worker-01.command"
"$netem" render "$test_dir/asymmetric.json" eth-test \
	--target worker-02/data > "$test_dir/asymmetric-worker-02.command"

grep -F 'qdisc del dev eth-test root' "$test_dir/clean.command" >/dev/null
grep -F 'delay 100us 20us 10% distribution normal' \
	"$test_dir/datacenter.command" >/dev/null
grep -F 'rate 100Mbit' "$test_dir/constrained.command" >/dev/null
grep -F 'loss random 1% 25%' "$test_dir/lossy.command" >/dev/null
grep -F 'loss gemodel 0.5% 20% 80% 0.01%' \
	"$test_dir/bursty.command" >/dev/null
grep -F 'reorder 10% 25% gap 5' "$test_dir/reordered.command" >/dev/null
grep -F 'loss random 2% ecn' "$test_dir/ecn.command" >/dev/null
grep -F 'delay 250us' "$test_dir/asymmetric.command" >/dev/null
grep -F 'delay 100us' "$test_dir/asymmetric-worker-01.command" >/dev/null
grep -F 'delay 5ms' "$test_dir/asymmetric-worker-02.command" >/dev/null
grep -F 'rate 1Gbit' "$test_dir/asymmetric-worker-02.command" >/dev/null

if "$cli" render --profile kind-kubernetes \
	--set network.shaping.enabled=true \
	--set network.shaping.jitter=1ms > /dev/null 2>&1; then
	echo "jitter without base latency was accepted" >&2
	exit 1
fi
if "$cli" render --profile kind-kubernetes \
	--set network.shaping.enabled=true \
	--set network.shaping.ecn=true > /dev/null 2>&1; then
	echo "ECN without a loss model was accepted" >&2
	exit 1
fi
if "$cli" render --profile kind-kubernetes \
	--set network.shaping.enabled=true \
	--set network.shaping.lossModel=gemodel \
	--set network.shaping.lossPercent=1 > /dev/null 2>&1; then
	echo "incomplete burst-loss model was accepted" >&2
	exit 1
fi
if "$cli" render --profile kind-kubernetes \
	--set network.shaping.enabled=true \
	--set network.shaping.lossPercent=1 \
	--set network.shaping.lossRecoveryPercent=20 > /dev/null 2>&1; then
	echo "burst parameter with random loss was accepted" >&2
	exit 1
fi
if "$netem" render "$test_dir/datacenter.json" 'bad;interface' \
	> /dev/null 2>&1; then
	echo "unsafe interface name was accepted" >&2
	exit 1
fi
if "$netem" render "$test_dir/asymmetric.json" eth-test \
	--target 'bad/target' > /dev/null 2>&1; then
	echo "unsafe endpoint shaping target was accepted" >&2
	exit 1
fi

mkdir "$test_dir/bin"
cat > "$test_dir/bin/tc" <<'EOF'
#!/bin/sh
set -eu
case "$*" in
'qdisc add dev eth-test root netem help')
	echo 'Usage: netem [ delay TIME ] [ rate RATE ]' >&2
	exit 1
	;;
'qdisc show dev eth-test')
	echo 'qdisc fq_codel 0: root refcnt 2'
	;;
qdisc\ replace\ dev\ eth-test\ root\ netem*)
	printf '%s\n' "$*" > "$NETEM_TEST_APPLIED"
	;;
*)
	echo "unexpected tc command: $*" >&2
	exit 2
	;;
esac
EOF
chmod +x "$test_dir/bin/tc"
NETEM_TC="$test_dir/bin/tc" NETEM_TEST_APPLIED="$test_dir/applied" \
	"$netem" apply "$test_dir/datacenter.json" eth-test \
	--replace-existing > "$test_dir/apply.out" 2> "$test_dir/apply.err"
grep -F 'target netem has no seed support' "$test_dir/apply.err" >/dev/null
if grep -F ' seed ' "$test_dir/apply.out" "$test_dir/applied" >/dev/null; then
	echo "unsupported netem seed was applied" >&2
	exit 1
fi
grep -F 'delay 100us 20us 10% distribution normal' \
	"$test_dir/applied" >/dev/null

echo "netem configuration test: PASS"
