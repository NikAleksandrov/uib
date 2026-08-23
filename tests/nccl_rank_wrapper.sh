#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
set -eu

rank=${OMPI_COMM_WORLD_RANK:-${PMI_RANK:-${PMIX_RANK:-}}}
if [ -z "$rank" ]; then
	echo "cannot determine the NCCL test rank" >&2
	exit 1
fi
if [ "$rank" -gt 253 ]; then
	echo "local NCCL simulation supports at most 254 ranks" >&2
	exit 1
fi

UIB_BIND_IP=127.0.0.$((rank + 1))
export UIB_BIND_IP

if [ "${NCCL_TEST_UTS_INNER:-0}" = 1 ]; then
	hostname "uib-nccl-rank-$rank"
	if [ "$(id -u)" = 0 ] && [ "${NCCL_TEST_RUN_UID:-0}" != 0 ]; then
		exec setpriv --reuid "$NCCL_TEST_RUN_UID" \
			--regid "$NCCL_TEST_RUN_GID" --init-groups "$@"
	fi
	exec "$@"
fi

NCCL_TEST_UTS_INNER=1
NCCL_TEST_RUN_UID=$(id -u)
NCCL_TEST_RUN_GID=$(id -g)
export NCCL_TEST_UTS_INNER NCCL_TEST_RUN_UID NCCL_TEST_RUN_GID

if [ "$(id -u)" = 0 ]; then
	exec unshare --uts "$0" "$@"
fi

privilege=${NCCL_TEST_PRIVILEGE:-sudo}
exec "$privilege" -E unshare --uts "$0" "$@"
