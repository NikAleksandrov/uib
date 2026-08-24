#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
set -eu

project_dir=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
cli=$project_dir/simulation/bin/uib-sim
backend=$project_dir/simulation/backends/kind/cluster
test_dir=$(mktemp -d "${TMPDIR:-/tmp}/uib-kind-test.XXXXXX")
trap 'rm -rf -- "$test_dir"' EXIT INT TERM

"$cli" render --profile kind-kubernetes --format json > "$test_dir/config.json"
"$backend" render "$test_dir/config.json" > "$test_dir/kind.yaml"

python3 - "$test_dir/kind.yaml" <<'PY'
import sys
import yaml

config = yaml.safe_load(open(sys.argv[1], encoding="utf-8"))
assert config["apiVersion"] == "kind.x-k8s.io/v1alpha4"
assert [node["role"] for node in config["nodes"]] == [
    "control-plane", "worker", "worker"
]
assert all("@sha256:" in node["image"] for node in config["nodes"])
PY

cat > "$test_dir/kind" <<'EOF'
#!/bin/sh
set -eu
printf '%s\n' "$*" >> "$KIND_TEST_LOG"
case "$1 $2" in
'get clusters') printf '%s\n' uib-test ;;
'get kubeconfig') printf '%s\n' 'apiVersion: v1' ;;
'create cluster')
	while [ "$#" -gt 0 ]; do
		if [ "$1" = --config ]; then
			shift
			grep -q 'kind.x-k8s.io/v1alpha4' "$1"
			break
		fi
		shift
	done
	;;
esac
EOF
chmod +x "$test_dir/kind"

export KIND_COMMAND="$test_dir/kind"
export KIND_TEST_LOG="$test_dir/kind.log"
"$backend" up "$test_dir/config.json" --name uib-test --wait 30s
"$backend" status --name uib-test
"$backend" kubeconfig --name uib-test > "$test_dir/kubeconfig"
"$backend" down --name uib-test

grep -q '^create cluster --name uib-test ' "$test_dir/kind.log"
grep -q '^get clusters$' "$test_dir/kind.log"
grep -q '^get kubeconfig --name uib-test$' "$test_dir/kind.log"
grep -q '^delete cluster --name uib-test$' "$test_dir/kind.log"
grep -q '^apiVersion: v1$' "$test_dir/kubeconfig"

if "$backend" down --name 'unsafe/name' >/dev/null 2>&1; then
	echo "unsafe Kind cluster name was accepted" >&2
	exit 1
fi

echo "Kind backend test: PASS"
