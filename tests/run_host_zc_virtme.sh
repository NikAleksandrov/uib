#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
set -eu

project_dir=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
guest_command='make tests/host_zc_test tests/host_zc_e2e_test tests/protocol_test && ./tests/host_zc_test && ./tests/host_zc_e2e_test && ./tests/protocol_test'

if ! command -v vng >/dev/null 2>&1; then
	echo "virtme-ng is required" >&2
	exit 1
fi

if test -n "${UIB_VIRTME_KERNEL:-}"; then
	exec vng --run "$UIB_VIRTME_KERNEL" --user root --network loop \
		--memory 2G --cpus 2 --cwd "$project_dir" \
		--exec "$guest_command"
fi

exec vng --run --user root --network loop --memory 2G --cpus 2 \
	--cwd "$project_dir" --exec "$guest_command"
