# SPDX-License-Identifier: Apache-2.0

def task_wrapper(uib_enabled):
	lines = [
		"set -eu",
		"export RANK=${SLURM_PROCID:-0}",
		"export WORLD_SIZE=${SLURM_NTASKS:-1}",
		"export LOCAL_RANK=${SLURM_LOCALID:-0}",
	]
	if uib_enabled:
		lines.extend([
			"export LD_LIBRARY_PATH=/opt/uib/lib"
			"${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}",
			"UIB_BIND_IP=$(ip -o -4 addr show dev \"$SLURM_UIB_DATA_INTERFACE\" "
			"scope global | awk 'NR == 1 { split($4, address, \"/\"); "
			"print address[1] }')",
			"if [ -z \"$UIB_BIND_IP\" ]; then",
			"\techo \"no IPv4 address on $SLURM_UIB_DATA_INTERFACE\" >&2",
			"\texit 2",
			"fi",
			"export UIB_BIND_IP",
			"export UIB_BIND_PORT=$((SLURM_UIB_BIND_PORT_BASE + LOCAL_RANK))",
			"/opt/uib/bin/uib-preflight --library-only",
		])
	lines.append('exec "$@"')
	return "\n".join(lines)
