#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
set -eu

project_dir=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
mpi_test=$project_dir/tests/mpi_compat_test
rank_wrapper=$project_dir/tests/mpi_rank_wrapper.sh
mpiexec=${MPIEXEC:-mpirun}
frontends=${MPI_TEST_FRONTENDS:-ucx ofi}
test_timeout=${MPI_TEST_TIMEOUT:-45}
bind_port=${MPI_TEST_BIND_PORT:-19820}
test_dir=$(mktemp -d /tmp/uib-mpi-test.XXXXXX)

cleanup()
{
	status=$?
	if [ "$status" -eq 0 ]; then
		rm -rf -- "$test_dir"
	else
		echo "MPI logs retained in $test_dir" >&2
	fi
}
trap cleanup EXIT INT TERM

if [ ! -c /dev/infiniband/uib0 ]; then
	echo "MPI over UCX/libfabric requires /dev/infiniband/uib0; run" \
		"this gate through simulation/runtime/with-device-anchor" >&2
	exit 1
fi
for command_name in "$mpiexec" timeout; do
	if ! command -v "$command_name" >/dev/null 2>&1; then
		echo "required MPI command not found: $command_name" >&2
		exit 1
	fi
done
if [ ! -x "$mpi_test" ]; then
	echo "build tests/mpi_compat_test before running the MPI gate" >&2
	exit 1
fi
if [ -z "$frontends" ]; then
	echo "MPI_TEST_FRONTENDS must select at least one UIB front end" >&2
	exit 2
fi

run_mpi()
{
	frontend=$1
	log=$test_dir/$frontend.log
	common_env=(
		MPI_UIB_PROVIDER="$project_dir/libuibv.so"
		UIB_BIND_PORT="$bind_port"
		UIB_PROTOCOL=rocev2
		UIB_TRANSPORT_MODE=udp-sync
		UIB_ZEROCOPY=off
		OMPI_ALLOW_RUN_AS_ROOT=1
		OMPI_ALLOW_RUN_AS_ROOT_CONFIRM=1
	)
	case $frontend in
	ucx)
		timeout -k 2 "$test_timeout" env "${common_env[@]}" \
			UCX_TLS=ud UCX_NET_DEVICES=uib0:1 UCX_LOG_LEVEL=info \
			"$mpiexec" -np 2 --bind-to none --mca pml ucx \
			--mca pml_ucx_tls ud_verbs \
			--mca pml_ucx_devices uib0 \
			"$rank_wrapper" "$mpi_test" >"$log" 2>&1
		grep -F 'ud_verbs/uib0:1' "$log" >/dev/null
		if grep -Ei 'transport.*(tcp|self|posix|sysv|cma)' "$log" \
			>/dev/null; then
			echo "MPI/UCX selected a forbidden transport" >&2
			exit 1
		fi
		;;
	ofi)
		timeout -k 2 "$test_timeout" env "${common_env[@]}" \
			FI_PROVIDER='verbs;ofi_rxd' FI_LOG_LEVEL=info \
			FI_VERBS_DGRAM_USE_NAME_SERVER=0 \
			"$mpiexec" -np 2 --bind-to none --mca pml cm \
			--mca mtl ofi \
			--mca mtl_ofi_provider_include 'verbs;ofi_rxd' \
			"$rank_wrapper" "$mpi_test" >"$log" 2>&1
		grep -F 'Using verbs provider' "$log" >/dev/null
		grep -F 'Using ofi_rxd provider' "$log" >/dev/null
		grep -F uib0-dgram "$log" >/dev/null
		if grep -Ei 'Using (tcp|sockets) provider' "$log" >/dev/null; then
			echo "MPI/libfabric selected a forbidden provider" >&2
			exit 1
		fi
		;;
	*)
		echo "unknown MPI test front end: $frontend" >&2
		exit 2
		;;
	esac
	grep -F 'MPI communication over UIB: PASS' "$log" >/dev/null
}

for frontend in $frontends; do
	run_mpi "$frontend"
done

echo "actual MPI/UIB compatibility test: PASS"
