#!/bin/sh
# SPDX-License-Identifier: MIT
set -eu

project_dir=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
backing=$(mktemp /tmp/uib-external-dmabuf.XXXXXX)

cleanup()
{
	exec 9>&-
	rm -f "$backing"
}
trap cleanup EXIT INT TERM

truncate -s 8192 "$backing"
exec 9<>"$backing"
UIB_TEST_GPU_DMABUF_FD=9 UIB_TEST_GPU_DMABUF_OFFSET=4096 \
	"$project_dir/tests/external_dmabuf_test"
