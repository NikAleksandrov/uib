#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
set -eu

project_dir=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)

run()
{
	protocol=$1
	port=$2
	mtu=$3
	printf 'Testing %s with link MTU %s\n' "$protocol" "$mtu"
	UIB_TEST_PROTOCOL=$protocol UIB_TEST_PORT=$port \
		UIB_TEST_LINK_MTU=$mtu "$project_dir/tests/run_uib_tools.sh"
}

run rocev2 19491 576
run rocev2 19492 1280
run uet 19493 576

echo "Simulation MTU test: PASS"
