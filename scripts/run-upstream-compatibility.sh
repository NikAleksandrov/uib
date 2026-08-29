#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
set -eu

project_dir=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
rdma_core_version=${UIB_RDMA_CORE_VERSION:?set UIB_RDMA_CORE_VERSION}
libfabric_version=${UIB_LIBFABRIC_VERSION:?set UIB_LIBFABRIC_VERSION}
jobs=${UIB_TEST_JOBS:-2}
cache_root=${UIB_UPSTREAM_CACHE:-${TMPDIR:-/tmp}/uib-upstream-cache}
report=${UIB_UPSTREAM_REPORT:-$project_dir/simulation/.artifacts/upstream-compatibility.txt}
stack_dir=$cache_root/rdma-$rdma_core_version-fabric-$libfabric_version
marker=$stack_dir/.complete
multiarch=$(cc -print-multiarch 2>/dev/null || true)
stack_lib_paths=$stack_dir/lib:$stack_dir/lib64
stack_pkg_paths=$stack_dir/lib/pkgconfig:$stack_dir/lib64/pkgconfig
stack_ldflags="-L$stack_dir/lib -L$stack_dir/lib64"
work_dir=

if [ -n "$multiarch" ]; then
	stack_lib_paths=$stack_dir/lib/$multiarch:$stack_lib_paths
	stack_pkg_paths=$stack_dir/lib/$multiarch/pkgconfig:$stack_pkg_paths
	stack_ldflags="-L$stack_dir/lib/$multiarch $stack_ldflags"
fi

cleanup()
{
	if [ -n "$work_dir" ]; then
		rm -rf -- "$work_dir"
	fi
}
trap cleanup EXIT INT TERM

require_command()
{
	command -v "$1" >/dev/null 2>&1 || {
		echo "required command not found: $1" >&2
		exit 1
	}
}

download()
{
	url=$1
	output=$2

	curl --fail --location --retry 4 --retry-all-errors \
		--silent --show-error --output "$output" "$url"
}

build_stack()
{
	rdma_archive=$work_dir/rdma-core.tar.gz
	libfabric_archive=$work_dir/libfabric.tar.bz2
	fabtests_archive=$work_dir/fabtests.tar.bz2
	rdma_source=$work_dir/rdma-core-$rdma_core_version
	rdma_build=$work_dir/rdma-build
	libfabric_source=$work_dir/libfabric-$libfabric_version
	fabtests_source=$work_dir/fabtests-$libfabric_version

	mkdir -p -- "$work_dir" "$stack_dir"
	download \
		"https://github.com/linux-rdma/rdma-core/archive/refs/tags/v$rdma_core_version.tar.gz" \
		"$rdma_archive"
	download \
		"https://github.com/ofiwg/libfabric/releases/download/v$libfabric_version/libfabric-$libfabric_version.tar.bz2" \
		"$libfabric_archive"
	download \
		"https://github.com/ofiwg/libfabric/releases/download/v$libfabric_version/fabtests-$libfabric_version.tar.bz2" \
		"$fabtests_archive"
	tar -C "$work_dir" -xzf "$rdma_archive"
	tar -C "$work_dir" -xjf "$libfabric_archive"
	tar -C "$work_dir" -xjf "$fabtests_archive"

	cmake -S "$rdma_source" -B "$rdma_build" -G Ninja \
		-DCMAKE_BUILD_TYPE=Release \
		-DCMAKE_INSTALL_PREFIX="$stack_dir" \
		-DCMAKE_INSTALL_SYSCONFDIR="$stack_dir/etc" \
		-DCMAKE_INSTALL_INITDDIR="$stack_dir/etc/init.d" \
		-DCMAKE_INSTALL_MODPROBEDIR="$stack_dir/etc/modprobe.d" \
		-DCMAKE_INSTALL_RUNDIR="$stack_dir/var/run" \
		-DCMAKE_INSTALL_SYSTEMD_BINDIR="$stack_dir/lib/systemd" \
		-DCMAKE_INSTALL_SYSTEMD_SERVICEDIR="$stack_dir/lib/systemd/system" \
		-DCMAKE_INSTALL_UDEV_RULESDIR="$stack_dir/lib/udev/rules.d" \
		-DSYSUSERS_DIR="$stack_dir/lib/sysusers.d" \
		-DNO_MAN_PAGES=1 -DNO_PYVERBS=1
	cmake --build "$rdma_build" --parallel "$jobs"
	cmake --install "$rdma_build"

	(
		cd "$libfabric_source"
		PKG_CONFIG_PATH="$stack_pkg_paths" \
		CPPFLAGS="-I$stack_dir/include" \
		LDFLAGS="$stack_ldflags" \
		LD_LIBRARY_PATH="$stack_lib_paths" \
		LIBRARY_PATH="$stack_lib_paths" \
			./configure --prefix="$stack_dir" \
			--enable-verbs=yes --enable-rxd=yes
		make -j"$jobs"
		make install
	)

	(
		cd "$fabtests_source"
		PKG_CONFIG_PATH="$stack_pkg_paths" \
		CPPFLAGS="-I$stack_dir/include" \
		LDFLAGS="$stack_ldflags" \
		LD_LIBRARY_PATH="$stack_lib_paths" \
		LIBRARY_PATH="$stack_lib_paths" \
			./configure --prefix="$stack_dir" \
			--with-libfabric="$stack_dir"
		make -j"$jobs"
		make install
	)
	touch "$marker"
}

for command_name in cmake curl make ninja pkg-config tar; do
	require_command "$command_name"
done

mkdir -p -- "$cache_root"
mkdir -p -- "$(dirname -- "$report")"
if [ ! -f "$marker" ]; then
	rm -rf -- "$stack_dir"
	work_dir=$(mktemp -d "${TMPDIR:-/tmp}/uib-upstream-build.XXXXXX")
	build_stack
	cleanup
	work_dir=
fi

export PATH="$stack_dir/bin:$stack_dir/sbin:$PATH"
export PKG_CONFIG_PATH="$stack_pkg_paths"
export CPPFLAGS="-I$stack_dir/include"
export LDFLAGS="$stack_ldflags"
export LD_LIBRARY_PATH="$stack_lib_paths"
export LIBRARY_PATH="$stack_lib_paths"

cd "$project_dir"
make clean >/dev/null 2>&1 || true
./configure --enable-provider --disable-af-xdp
grep -Fx 'ENABLE_PROVIDER = 1' config.mk >/dev/null
grep -F "IBVERBS_CFLAGS = -I$stack_dir/include" config.mk >/dev/null
make -j"$jobs" all
make provider-cq-event-test
make provider-nccl-abi-test

ldd tests/provider_cq_event_test | grep -F libibverbs | \
	grep -F "$stack_dir/" >/dev/null
ldd "$(command -v ibv_devices)" | grep -F libibverbs | \
	grep -F "$stack_dir/" >/dev/null
env LD_PRELOAD="$project_dir/libuibv.so" \
	UIB_BIND_IP=127.0.0.1 UIB_BIND_PORT=19640 \
	UIB_TRANSPORT_MODE=udp-sync UIB_ZEROCOPY=off \
	ibv_devices | grep -F uib0 >/dev/null

COMM_TEST_UCX_TESTS='' COMM_TEST_UCT_TESTS='' \
	COMM_TEST_ITERATIONS=20 COMM_TEST_WARMUPS=2 \
	COMM_TEST_MESSAGE_SIZES=l:1,2051,65536 \
	./simulation/runtime/with-device-anchor \
	./tests/run_ucx_libfabric_test.sh

{
	echo 'UIB upstream compatibility report'
	echo
	printf 'rdma-core requested=%s\n' "$rdma_core_version"
	printf 'libibverbs detected=%s\n' \
		"$(pkg-config --modversion libibverbs)"
	printf 'libfabric requested=%s\n' "$libfabric_version"
	printf 'libfabric detected=%s\n' \
		"$(pkg-config --modversion libfabric)"
	printf 'fabtests fi_rdm_pingpong=%s\n' \
		"$(command -v fi_rdm_pingpong)"
	echo
	sed -n '/^HAVE_\|^ENABLE_PROVIDER\|^IBVERBS_/p' config.mk
	echo
	ldd tests/provider_cq_event_test
	echo
	ldd "$(command -v ibv_devices)"
	echo
	fi_info --version
} > "$report"

cat "$report"
echo "upstream rdma-core/libfabric compatibility test: PASS"
