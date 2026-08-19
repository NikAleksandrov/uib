#!/bin/sh
set -eu

project_dir=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)

if ! command -v vng >/dev/null 2>&1; then
	echo "virtme-ng (vng) is required" >&2
	exit 1
fi

exec vng --run --network loop --memory 2G --cpus 2 \
	--cwd "$project_dir" --exec "make guest-test"
