#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
set -eu

project_dir=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
cli=$project_dir/simulation/bin/uib-sim
test_dir=$(mktemp -d "${TMPDIR:-/tmp}/uib-interruption.XXXXXX")
trap 'rm -rf -- "$test_dir"' EXIT HUP INT TERM

wait_for_file()
{
	file=$1
	count=0
	while [ ! -f "$file" ]; do
		count=$((count + 1))
		if [ "$count" -ge 200 ]; then
			echo "timed out waiting for $file" >&2
			exit 1
		fi
		sleep 0.05
	done
}

interrupt()
{
	pid=$1
	kill -TERM "$pid"
	set +e
	wait "$pid"
	status=$?
	set -e
	if [ "$status" -ne 143 ]; then
		echo "interrupted lifecycle exited with $status, expected 143" >&2
		exit 1
	fi
}

mkdir -p "$test_dir/kind-bin"
cat > "$test_dir/kind-bin/kind" <<'EOF'
#!/bin/sh
set -eu
case "$1 $2" in
'create cluster')
	name=
	while [ "$#" -gt 0 ]; do
		if [ "$1" = --name ]; then
			shift
			name=$1
		fi
		shift
	done
	: > "$INTERRUPT_ROOT/kind-create-$name"
	trap 'exit 143' INT TERM
	while :; do sleep 1; done
	;;
'delete cluster')
	printf '%s\n' "$*" >> "$INTERRUPT_ROOT/kind-delete.log"
	;;
*) exit 2 ;;
esac
EOF
chmod +x "$test_dir/kind-bin/kind"

INTERRUPT_ROOT=$test_dir KIND_COMMAND=$test_dir/kind-bin/kind \
	"$cli" up --profile kind-kubernetes --run-id interrupt-kind \
	--state-dir "$test_dir/state" > "$test_dir/kind.out" 2> "$test_dir/kind.err" &
kind_pid=$!
wait_for_file "$test_dir/kind-create-interrupt-kind"
interrupt "$kind_pid"
grep -Fx 'delete cluster --name interrupt-kind' \
	"$test_dir/kind-delete.log" > /dev/null

INTERRUPT_ROOT=$test_dir KIND_COMMAND=$test_dir/kind-bin/kind \
	"$cli" up --profile kind-kubernetes --run-id retain-kind --retain \
	--state-dir "$test_dir/state" > "$test_dir/retain.out" 2> "$test_dir/retain.err" &
kind_pid=$!
wait_for_file "$test_dir/kind-create-retain-kind"
interrupt "$kind_pid"
if grep -F 'retain-kind' "$test_dir/kind-delete.log" > /dev/null; then
	echo "Kind --retain deleted the interrupted cluster" >&2
	exit 1
fi

mkdir -p "$test_dir/libvirt-bin" "$test_dir/libvirt-cache" \
	"$test_dir/libvirt-log"
printf '%s\n' 'cached image' > "$test_dir/base.img"
image_sha=$(sha256sum "$test_dir/base.img" | awk '{print $1}')
cp "$test_dir/base.img" \
	"$test_dir/libvirt-cache/ubuntu-noble-$(printf '%.16s' "$image_sha").img"
cat > "$test_dir/libvirt-bin/ssh-keygen" <<'EOF'
#!/bin/sh
set -eu
while [ "$1" != -f ]; do shift; done
key=$2
printf '%s\n' private > "$key"
printf '%s\n' 'ssh-ed25519 AAAAinterruption' > "$key.pub"
EOF
cat > "$test_dir/libvirt-bin/tofu" <<'EOF'
#!/bin/sh
set -eu
case " $* " in
*' apply '*)
	: > "$INTERRUPT_ROOT/libvirt-apply"
	trap 'exit 143' INT TERM
	while :; do sleep 1; done
	;;
*' destroy '*) : > "$INTERRUPT_ROOT/libvirt-destroy" ;;
esac
EOF
cat > "$test_dir/libvirt-bin/ansible-playbook" <<'EOF'
#!/bin/sh
exit 2
EOF
chmod +x "$test_dir/libvirt-bin/ssh-keygen" \
	"$test_dir/libvirt-bin/tofu" "$test_dir/libvirt-bin/ansible-playbook"

INTERRUPT_ROOT=$test_dir \
TOFU_COMMAND=$test_dir/libvirt-bin/tofu \
SSH_KEYGEN_COMMAND=$test_dir/libvirt-bin/ssh-keygen \
ANSIBLE_PLAYBOOK_COMMAND=$test_dir/libvirt-bin/ansible-playbook \
LIBVIRT_DOWNLOAD_CACHE=$test_dir/libvirt-cache \
	"$cli" up --profile libvirt-slurm --run-id interrupt-libvirt \
	--state-dir "$test_dir/state" --set uib.enabled=false \
	--set "libvirt.baseImage=https://example.invalid/base.img" \
	--set "libvirt.baseImageSha256=$image_sha" \
	> "$test_dir/libvirt.out" 2> "$test_dir/libvirt.err" &
libvirt_pid=$!
wait_for_file "$test_dir/libvirt-apply"
interrupt "$libvirt_pid"
wait_for_file "$test_dir/libvirt-destroy"
python3 - "$test_dir/state/interrupt-libvirt/infrastructure/ledger.json" <<'PY'
import json
import sys

with open(sys.argv[1], encoding="utf-8") as source:
    ledger = json.load(source)
assert ledger["state"] == "failed"
assert ledger["cleanup"] == "destroyed"
assert "SIGTERM" in ledger["error"]
PY

mkdir -p "$test_dir/virtme-bin"
cat > "$test_dir/virtme-bin/ssh-keygen" <<'EOF'
#!/bin/sh
set -eu
while [ "$1" != -f ]; do shift; done
key=$2
printf '%s\n' private > "$key"
printf '%s\n' 'ssh-ed25519 AAAAinterruption' > "$key.pub"
EOF
cat > "$test_dir/virtme-bin/vng" <<'EOF'
#!/bin/sh
set -eu
printf '%s\n' "$$" > "$INTERRUPT_ROOT/virtme-vng-pid"
: > "$INTERRUPT_ROOT/virtme-started"
trap 'exit 0' INT TERM
while :; do sleep 1; done
EOF
cat > "$test_dir/virtme-bin/ssh" <<'EOF'
#!/bin/sh
set -eu
case "$*" in
*'/run/uib-netem clear '*) exit 0 ;;
*) exit 1 ;;
esac
EOF
cat > "$test_dir/virtme-bin/k3s" <<'EOF'
#!/bin/sh
exit 0
EOF
chmod +x "$test_dir/virtme-bin/ssh-keygen" "$test_dir/virtme-bin/vng" \
	"$test_dir/virtme-bin/ssh" "$test_dir/virtme-bin/k3s"

INTERRUPT_ROOT=$test_dir \
SSH_KEYGEN_COMMAND=$test_dir/virtme-bin/ssh-keygen \
VNG_COMMAND=$test_dir/virtme-bin/vng \
SSH_COMMAND=$test_dir/virtme-bin/ssh \
K3S_BINARY=$test_dir/virtme-bin/k3s VIRTME_SSH_WAIT=30 \
	"$cli" up --profile virtme-kubernetes --run-id interrupt-virtme \
	--state-dir "$test_dir/state" > "$test_dir/virtme.out" \
	2> "$test_dir/virtme.err" &
virtme_pid=$!
wait_for_file "$test_dir/virtme-started"
interrupt "$virtme_pid"
python3 - "$test_dir/state/interrupt-virtme/infrastructure/ledger.json" <<'PY'
import json
import sys

with open(sys.argv[1], encoding="utf-8") as source:
    ledger = json.load(source)
assert ledger["state"] == "failed"
assert ledger["cleanup"] == "destroyed"
assert "SIGTERM" in ledger["error"]
PY
vng_pid=$(cat "$test_dir/virtme-vng-pid")
if kill -0 "$vng_pid" 2> /dev/null; then
	echo "interrupted virtme guest process survived cleanup" >&2
	exit 1
fi

echo "lifecycle interruption cleanup test: PASS"
