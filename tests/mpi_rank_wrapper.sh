#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
set -eu

rank=${OMPI_COMM_WORLD_RANK:-${PMI_RANK:-${PMIX_RANK:-}}}
if [ -z "$rank" ]; then
	echo "cannot determine the MPI test rank" >&2
	exit 1
fi
if [ "$rank" -gt 253 ]; then
	echo "local MPI simulation supports at most 254 ranks" >&2
	exit 1
fi

MPI_UIB_PROVIDER=${MPI_UIB_PROVIDER:?MPI_UIB_PROVIDER is required}
UIB_BIND_IP=127.0.0.$((rank + 1))
LD_PRELOAD=$MPI_UIB_PROVIDER
export LD_PRELOAD UIB_BIND_IP

exec "$@"
