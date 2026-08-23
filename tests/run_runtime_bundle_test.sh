#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
set -eu

project_dir=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
test_dir=$(mktemp -d "${TMPDIR:-/tmp}/uib-runtime-test.XXXXXX")
bundle=$test_dir/uib-runtime
second_bundle=$test_dir/uib-runtime-second
archive=$test_dir/uib-runtime.tar.gz
second_archive=$test_dir/uib-runtime-second.tar.gz
trap 'rm -rf -- "$test_dir"' EXIT INT TERM

env SOURCE_DATE_EPOCH=1 "$project_dir/simulation/runtime/build-bundle.py" \
	--no-build --output "$bundle" --archive "$archive"
env SOURCE_DATE_EPOCH=1 "$project_dir/simulation/runtime/build-bundle.py" \
	--no-build --output "$second_bundle" --archive "$second_archive"
"$bundle/bin/uib-preflight" --library-only
python3 -m json.tool "$bundle/share/uib/runtime-manifest.json" >/dev/null
python3 -m json.tool "$bundle/share/uib/compatibility-lock.json" >/dev/null
tar -tzf "$archive" uib-runtime/lib/libuibv.so >/dev/null
tar -tzf "$archive" uib-runtime/lib/libuib-standalone.a >/dev/null
test "$(readlink "$bundle/libuibv.so")" = lib/libuibv.so
cmp "$archive" "$second_archive"

echo "UIB runtime bundle test: PASS"
