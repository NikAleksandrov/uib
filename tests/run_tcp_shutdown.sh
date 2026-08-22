#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
set -eu

project_dir=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
data_port=$((30000 + ($$ % 20000)))

timeout 5 env UIB_TRANSPORT_MODE=tcp-devmem UIB_ZEROCOPY=off \
	"$project_dir/examples/stress_bench" --protocol rocev2 --memory host \
	--iterations 1000 --size 16384 --window 16 --mtu 1500 \
	--data-port "$data_port" --csv
