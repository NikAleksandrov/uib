#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
set -eu

project_dir=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
cli=$project_dir/simulation/bin/uib-sim
netem=$project_dir/simulation/network/netem
test_dir=$(mktemp -d "${TMPDIR:-/tmp}/uib-netem-config-test.XXXXXX")
priv_bridge=
priv_ifb=
priv_link_a=
priv_link_b=
priv_ns_a=
priv_ns_b=
privilege_command=${NETEM_PRIVILEGE_COMMAND:-sudo}

privileged()
{
	if [ "$(id -u)" -eq 0 ]; then
		"$@"
	else
		"$privilege_command" "$@"
	fi
}

cleanup()
{
	if [ -n "$priv_link_a" ]; then
		privileged "$netem" clear-ingress "$priv_link_a" \
			>/dev/null 2>&1 || true
	fi
	if [ -n "$priv_link_b" ]; then
		privileged "$netem" clear-ingress "$priv_link_b" \
			>/dev/null 2>&1 || true
	fi
	if [ -n "$priv_ifb" ] && \
	   privileged ip link show "$priv_ifb" >/dev/null 2>&1; then
		privileged "$netem" destroy-ingress "$priv_ifb" \
			>/dev/null 2>&1 || true
	fi
	[ -z "$priv_link_a" ] || privileged ip link delete "$priv_link_a" \
		>/dev/null 2>&1 || true
	[ -z "$priv_link_b" ] || privileged ip link delete "$priv_link_b" \
		>/dev/null 2>&1 || true
	[ -z "$priv_bridge" ] || privileged ip link delete "$priv_bridge" \
		type bridge >/dev/null 2>&1 || true
	[ -z "$priv_ns_a" ] || privileged ip netns delete "$priv_ns_a" \
		>/dev/null 2>&1 || true
	[ -z "$priv_ns_b" ] || privileged ip netns delete "$priv_ns_b" \
		>/dev/null 2>&1 || true
	rm -rf -- "$test_dir"
}

trap cleanup EXIT INT TERM

for condition in clean datacenter constrained lossy bursty reordered ecn \
	duplicated corrupted jittery wan mixed; do
	"$cli" render --profile kind-kubernetes \
		--overlay "network/$condition" --format json \
		> "$test_dir/$condition.json"
	"$netem" render "$test_dir/$condition.json" eth-test \
		> "$test_dir/$condition.command"
done

"$cli" render --profile virtme-kubernetes --overlay network/asymmetric \
	--format json > "$test_dir/asymmetric.json"
"$cli" render --profile libvirt-kubernetes --overlay network/datacenter \
	--overlay network/host-tap-ingress --format json \
	> "$test_dir/host-ingress.json"
"$cli" render --profile libvirt-kubernetes \
	--overlay network/oversubscribed --format json \
	> "$test_dir/oversubscribed.json"
"$netem" render "$test_dir/asymmetric.json" eth-test \
	> "$test_dir/asymmetric.command"

"$netem" render "$test_dir/asymmetric.json" eth-test \
	--target worker-01/data > "$test_dir/asymmetric-worker-01.command"
"$netem" render "$test_dir/asymmetric.json" eth-test \
	--target worker-02/data > "$test_dir/asymmetric-worker-02.command"
"$netem" render-ingress "$test_dir/host-ingress.json" tap-test ifb-test \
	--target worker-01/data > "$test_dir/host-ingress.command"
"$netem" render-ingress "$test_dir/oversubscribed.json" tap-test ifb-test \
	--oversubscription > "$test_dir/oversubscribed.command"

grep -F 'qdisc del dev eth-test root' "$test_dir/clean.command" >/dev/null
grep -F 'delay 100us 20us 10% distribution normal' \
	"$test_dir/datacenter.command" >/dev/null
grep -F 'rate 100Mbit' "$test_dir/constrained.command" >/dev/null
grep -F 'loss random 1% 25%' "$test_dir/lossy.command" >/dev/null
grep -F 'loss gemodel 0.5% 20% 80% 0.01%' \
	"$test_dir/bursty.command" >/dev/null
grep -F 'reorder 10% 25% gap 5' "$test_dir/reordered.command" >/dev/null
grep -F 'loss random 2% ecn' "$test_dir/ecn.command" >/dev/null
grep -F 'duplicate 2% 25%' "$test_dir/duplicated.command" >/dev/null
grep -F 'corrupt 1% 10%' "$test_dir/corrupted.command" >/dev/null
grep -F 'delay 5ms 4ms 25% distribution paretonormal' \
	"$test_dir/jittery.command" >/dev/null
grep -F 'delay 20ms 5ms 10% distribution normal' \
	"$test_dir/wan.command" >/dev/null
grep -F 'loss random 0.1%' "$test_dir/wan.command" >/dev/null
grep -F 'rate 1Gbit 24' "$test_dir/wan.command" >/dev/null
grep -F 'loss random 0.5% duplicate 0.2% corrupt 0.1%' \
	"$test_dir/mixed.command" >/dev/null
grep -F 'reorder 2% 10% gap 10 rate 5Gbit' \
	"$test_dir/mixed.command" >/dev/null
grep -F 'delay 250us' "$test_dir/asymmetric.command" >/dev/null
grep -F 'delay 100us' "$test_dir/asymmetric-worker-01.command" >/dev/null
grep -F 'delay 5ms' "$test_dir/asymmetric-worker-02.command" >/dev/null
grep -F 'rate 1Gbit' "$test_dir/asymmetric-worker-02.command" >/dev/null
grep -F 'link add name ifb-test type ifb' \
	"$test_dir/host-ingress.command" >/dev/null
grep -F 'qdisc replace dev ifb-test root netem' \
	"$test_dir/host-ingress.command" >/dev/null
grep -F 'redirect dev ifb-test' "$test_dir/host-ingress.command" >/dev/null
grep -F 'rate 10Gbit' "$test_dir/oversubscribed.command" >/dev/null

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

if [ "${NETEM_PRIVILEGED:-0}" = 1 ]; then
	priv_bridge=uibnb$$
	priv_ifb=uibni$$
	priv_link_a=uibna$$
	priv_link_b=uibnbx$$
	peer_a=uibnap$$
	peer_b=uibnbp$$
	priv_ns_a=uib-netem-a-$$
	priv_ns_b=uib-netem-b-$$
	privileged ip netns add "$priv_ns_a"
	privileged ip netns add "$priv_ns_b"
	privileged ip link add name "$priv_bridge" type bridge
	privileged ip link set dev "$priv_bridge" up
	privileged ip link add name "$priv_link_a" type veth peer name "$peer_a"
	privileged ip link add name "$priv_link_b" type veth peer name "$peer_b"
	privileged ip link set dev "$priv_link_a" master "$priv_bridge"
	privileged ip link set dev "$priv_link_b" master "$priv_bridge"
	privileged ip link set dev "$priv_link_a" up
	privileged ip link set dev "$priv_link_b" up
	privileged ip link set dev "$peer_a" netns "$priv_ns_a"
	privileged ip link set dev "$peer_b" netns "$priv_ns_b"
	privileged ip -n "$priv_ns_a" link set dev lo up
	privileged ip -n "$priv_ns_b" link set dev lo up
	privileged ip -n "$priv_ns_a" link set dev "$peer_a" name eth0
	privileged ip -n "$priv_ns_b" link set dev "$peer_b" name eth0
	privileged ip -n "$priv_ns_a" address add 198.18.0.1/24 dev eth0
	privileged ip -n "$priv_ns_b" address add 198.18.0.2/24 dev eth0
	privileged ip -n "$priv_ns_a" link set dev eth0 up
	privileged ip -n "$priv_ns_b" link set dev eth0 up
	privileged "$netem" apply-ingress "$test_dir/oversubscribed.json" \
		"$priv_link_a" "$priv_ifb" --oversubscription \
		--replace-existing > "$test_dir/privileged-apply.out"
	privileged "$netem" attach-ingress "$priv_link_b" "$priv_ifb" \
		--replace-existing
	privileged ip netns exec "$priv_ns_a" ping -q -c 4 -W 1 198.18.0.2 \
		> "$test_dir/privileged-ping.out"
	privileged "$netem" show-ingress "$priv_link_a" "$priv_ifb" \
		> "$test_dir/privileged-qdisc.out"
	grep -E 'Sent [1-9][0-9]* bytes' \
		"$test_dir/privileged-qdisc.out" >/dev/null
	privileged "$netem" clear-ingress "$priv_link_a"
	privileged "$netem" clear-ingress "$priv_link_b"
	privileged "$netem" destroy-ingress "$priv_ifb"
	if privileged ip link show "$priv_ifb" >/dev/null 2>&1; then
		echo "owned IFB survived cleanup" >&2
		exit 1
	fi
	echo "privileged host network test: PASS"
fi

echo "netem configuration test: PASS"
