#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
set -eu

project_dir=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
jobs=${UIB_TEST_JOBS:-4}
report=${DISTRIBUTION_COMPAT_REPORT:-$project_dir/distribution-compatibility.txt}
install_root=$(mktemp -d "${TMPDIR:-/tmp}/uib-install.XXXXXX")
tools_output=$(mktemp "${TMPDIR:-/tmp}/uib-tools.XXXXXX")

cleanup()
{
	rm -rf -- "$install_root"
	rm -f -- "$tools_output"
}
trap cleanup EXIT INT TERM

cd "$project_dir"

make clean >/dev/null 2>&1 || true
./configure --prefix=/usr --enable-provider --enable-af-xdp

grep -Fx 'ENABLE_PROVIDER = 1' config.mk >/dev/null
grep -Fx 'HAVE_AFXDP = 1' config.mk >/dev/null
grep -Fx 'HAVE_ZLIB = 1' config.mk >/dev/null

make -j"$jobs" all
make test
make examples-test
make provider-cq-event-test
make provider-nccl-abi-test
make optional-features-test
make docs-check

env LD_PRELOAD="$project_dir/libuibv.so" \
	UIB_BIND_IP=127.0.0.1 UIB_BIND_PORT=19640 \
	UIB_TRANSPORT_MODE=udp-sync UIB_ZEROCOPY=off \
	ibv_devices > "$tools_output"
grep -F 'uib0' "$tools_output" >/dev/null

make DESTDIR="$install_root" PREFIX=/usr MANDIR=/usr/share/man \
	DOCDIR=/usr/share/doc/uib install
test -f "$install_root/usr/lib/libuib.a"
test -x "$install_root/usr/lib/libuib.so"
test -L "$install_root/usr/lib/libuib.so"
test "$(readlink "$install_root/usr/lib/libuib.so")" = libuib.so.0
test -L "$install_root/usr/lib/libuib.so.0"
test "$(readlink "$install_root/usr/lib/libuib.so.0")" = libuib.so.0.1.0
test -x "$install_root/usr/lib/libuib.so.0.1.0"
test -x "$install_root/usr/lib/libuibv.so"
test -f "$install_root/usr/include/uib/uib.h"

{
	echo 'UIB distribution compatibility report'
	echo
	cat /etc/os-release
	echo
	printf 'compiler=%s\n' "$(cc --version | sed -n '1p')"
	if command -v getconf >/dev/null 2>&1; then
		printf 'libc=%s\n' "$(getconf GNU_LIBC_VERSION 2>/dev/null || echo musl)"
	else
		printf 'libc=musl\n'
	fi
	for module in libibverbs liburing libmnl liburcu-bp zlib libxdp libbpf; do
		printf '%s=%s\n' "$module" "$(pkg-config --modversion "$module")"
	done
	echo
	sed -n '/^HAVE_\|^ENABLE_PROVIDER/p' config.mk
	echo
	ldd libuib.so
	ldd libuibv.so
	echo
	cat "$tools_output"
} > "$report"

cat "$report"
echo "distribution compatibility test: PASS"
