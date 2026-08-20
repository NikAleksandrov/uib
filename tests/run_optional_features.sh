#!/bin/sh
# SPDX-License-Identifier: MIT
set -eu

project_dir=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)

restore()
{
	cd "$project_dir"
	./configure >/dev/null
	make clean >/dev/null
	make -j"${UIB_TEST_JOBS:-4}" all >/dev/null
}
trap restore EXIT INT TERM

cd "$project_dir"
./configure --disable-dmabuf --disable-zerocopy
make clean
make -j"${UIB_TEST_JOBS:-4}" tests/dmabuf_mr_test tests/zerocopy_test
./tests/dmabuf_mr_test
./tests/zerocopy_test

trap - EXIT INT TERM
restore
printf '%s\n' 'optional feature build: PASS'
