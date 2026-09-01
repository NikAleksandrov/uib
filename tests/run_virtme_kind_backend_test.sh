#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
set -eu

project_dir=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
cli=$project_dir/simulation/bin/uib-sim
test_dir=$(mktemp -d "${TMPDIR:-/tmp}/uib-virtme-kind.XXXXXX")
trap 'rm -rf -- "$test_dir"' EXIT INT TERM

mkdir -p "$test_dir/fakes"
cat > "$test_dir/fakes/ssh-keygen" <<'EOF'
#!/bin/sh
set -eu
while test "$1" != -f; do
	shift
done
printf '%s\n' private > "$2"
printf '%s\n' 'ssh-ed25519 AAAAvirtmekind' > "$2.pub"
EOF
cat > "$test_dir/fakes/mkfs.ext4" <<'EOF'
#!/bin/sh
exit 0
EOF
cat > "$test_dir/fakes/vng" <<'EOF'
#!/bin/sh
set -eu
printf '%s\n' "$*" > "$VIRTME_KIND_VNG_LOG"
trap 'exit 0' INT TERM
while :; do
	sleep 1
done
EOF
cat > "$test_dir/fakes/ssh" <<'EOF'
#!/bin/sh
set -eu
forward=false
remote=
for argument in "$@"; do
	remote=$argument
	if test "$argument" = -N; then
		forward=true
	fi
done
if test "$forward" = true; then
	trap 'exit 0' INT TERM
	while :; do
		sleep 1
	done
fi
printf '%s\n' "$remote" >> "$VIRTME_KIND_SSH_LOG"
case "$remote" in
*'get kubeconfig'*)
	cat <<'CONFIG'
apiVersion: v1
clusters:
- cluster:
    certificate-authority-data: dGVzdA==
    server: https://127.0.0.1:38127
  name: kind-uib-kind-life
contexts: []
current-context: ""
kind: Config
users: []
CONFIG
	;;
*'docker load'*)
	cat > /dev/null
	;;
esac
exit 0
EOF
cat > "$test_dir/fakes/docker" <<'EOF'
#!/bin/sh
set -eu
case "$1 $2" in
'image inspect')
	printf '%s\n' 'sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa'
	;;
'image save')
	printf '%s\n' fake-image
	;;
*)
	exit 2
	;;
esac
EOF
cat > "$test_dir/fakes/kind" <<'EOF'
#!/bin/sh
exit 0
EOF
cat > "$test_dir/fakes/dockerd" <<'EOF'
#!/bin/sh
exit 0
EOF
chmod +x "$test_dir/fakes/ssh-keygen" "$test_dir/fakes/mkfs.ext4" \
	"$test_dir/fakes/vng" "$test_dir/fakes/ssh" \
	"$test_dir/fakes/docker" "$test_dir/fakes/kind" \
	"$test_dir/fakes/dockerd"

export SSH_KEYGEN_COMMAND=$test_dir/fakes/ssh-keygen
export MKFS_EXT4_COMMAND=$test_dir/fakes/mkfs.ext4
export VNG_COMMAND=$test_dir/fakes/vng
export SSH_COMMAND=$test_dir/fakes/ssh
export KIND_COMMAND=$test_dir/fakes/kind
export DOCKER_COMMAND=$test_dir/fakes/docker
export DOCKERD_COMMAND=$test_dir/fakes/dockerd
export CONTAINER_ENGINE_COMMAND=$test_dir/fakes/docker
export VIRTME_API_FORWARD_CHECK_COMMAND=/bin/true
export VIRTME_SSH_WAIT=2
export VIRTME_KIND_VNG_LOG=$test_dir/vng.log
export VIRTME_KIND_SSH_LOG=$test_dir/ssh.log

state=$test_dir/runs
common="--profile virtme-kind --set nodes.cpu=2 --set nodes.memory=2Gi --set nodes.disk=1Gi"
run_id=uib-kind-life
"$cli" up $common --run-id "$run_id" --state-dir "$state" > /dev/null
infrastructure=$state/$run_id/infrastructure

python3 - "$infrastructure" <<'PY'
import json
import os
import sys


root = sys.argv[1]
with open(os.path.join(root, "ledger.json"), encoding="utf-8") as source:
    ledger = json.load(source)
assert ledger["state"] == "ready"
assert ledger["mode"] == "kind"
assert ledger["clusterManager"] == "kubernetes"
with open(os.path.join(root, "topology.json"), encoding="utf-8") as source:
    topology = json.load(source)
assert topology["mode"] == "kind"
assert topology["kindNodes"] == 2
assert topology["kubernetesApiPort"] == 23001
processes = topology["ownership"]["processes"]
assert len(processes) == 2
assert {item["node"] for item in processes} == {
    "uib-kind-life", "kubernetes-api-forward"}
assert all(item["pid"] > 1 and item["startTime"] for item in processes)
assert len(topology["ownership"]["storage"]) == 1
assert os.path.isfile(topology["ownership"]["storage"][0])
PY

"$cli" status $common --run-id "$run_id" --state-dir "$state" \
	> "$test_dir/status.json"
python3 - "$test_dir/status.json" <<'PY'
import json
import sys


with open(sys.argv[1], encoding="utf-8") as source:
    status = json.load(source)
assert status["state"] == "ready"
assert status["recordedProcesses"] == status["liveProcesses"] == 2
assert status["kindNodes"] == 2
PY

"$cli" kubeconfig $common --run-id "$run_id" --state-dir "$state" \
	> "$test_dir/kubeconfig"
grep -q 'server: https://127.0.0.1:23001' "$test_dir/kubeconfig"
"$cli" load-image $common --run-id "$run_id" --state-dir "$state" \
	--allow-mutable-tags --image uib-training:test
python3 - "$infrastructure/kubernetes-images.json" <<'PY'
import json
import sys


with open(sys.argv[1], encoding="utf-8") as source:
    images = json.load(source)["images"]
assert images["uib-training:test"]["imageId"] == "sha256:" + "a" * 64
PY

"$cli" down $common --run-id "$run_id" --state-dir "$state"
python3 - "$infrastructure" <<'PY'
import json
import os
import sys


root = sys.argv[1]
with open(os.path.join(root, "ledger.json"), encoding="utf-8") as source:
    assert json.load(source)["state"] == "destroyed"
with open(os.path.join(root, "topology.json"), encoding="utf-8") as source:
    topology = json.load(source)
assert not os.path.exists(topology["ownership"]["storage"][0])
for process in topology["ownership"]["processes"]:
    try:
        with open(f"/proc/{process['pid']}/stat", encoding="utf-8") as source:
            fields = source.read().rpartition(") ")[2].split()
    except OSError:
        continue
    assert len(fields) <= 19 or fields[19] != process["startTime"]
PY
grep -q 'create cluster --name uib-kind-life' "$test_dir/ssh.log"
grep -q 'delete cluster --name uib-kind-life' "$test_dir/ssh.log"

echo "virtme Kind lifecycle test: PASS"
