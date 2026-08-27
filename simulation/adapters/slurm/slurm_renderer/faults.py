# SPDX-License-Identifier: Apache-2.0

import shlex


def task_wrapper(uib_enabled):
	lines = [
		"set -eu",
		"export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin",
		"export RANK=${SLURM_PROCID:-0}",
		"export WORLD_SIZE=${SLURM_NTASKS:-1}",
		"export LOCAL_RANK=${SLURM_LOCALID:-0}",
	]
	if uib_enabled:
		lines.extend([
			"export LD_LIBRARY_PATH=/opt/uib/lib"
			"${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}",
			"actual_mtu=$(/opt/uib/bin/ip -o link show dev "
			"\"$SLURM_UIB_DATA_INTERFACE\" | awk '{ for (field = 1; "
			"field <= NF; field++) if ($field == \"mtu\") { "
			"print $(field + 1); exit } }')",
			"if [ \"$actual_mtu\" != \"$SLURM_UIB_NETWORK_MTU\" ]; then",
			"\techo \"$SLURM_UIB_DATA_INTERFACE has MTU $actual_mtu; "
			"expected $SLURM_UIB_NETWORK_MTU\" >&2",
			"\texit 2",
			"fi",
			"UIB_BIND_IP=$(/opt/uib/bin/ip -o -4 addr show dev "
			"\"$SLURM_UIB_DATA_INTERFACE\" "
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


def fault_step(config):
	if not config.get("faults"):
		return []
	parallel = config["workload"]["parallelism"]
	provider = config["infrastructure"]["provider"]
	if provider == "virtme":
		executor_options = "--netem /run/uib-netem"
	else:
		executor_options = (
			"--netem /usr/local/libexec/uib-netem "
			"--ip-command /usr/sbin/ip --privilege-command sudo"
		)
	selector = (
		'.faults // [] | any(.target == $target and has("at") and '
		'(.action == "add-delay" or .action == "packet-loss" or '
		'.action == "link-down"))'
	)
	remote = (
		"set -eu; target=$(cat /run/uib-fault-target); "
		f"if jq -e --arg target \"$target\" '{selector}' \"$1\" "
		"> /dev/null; then exec /run/uib-network-schedule \"$1\" "
		"--target \"$target\" --interface \"$2\" "
		"--output \"$3/faults-${SLURMD_NODENAME}.jsonl\" "
		f"{executor_options}; fi"
	)
	command = [
		"srun", "--overlap", f"--nodes={parallel['nodes']}",
		f"--ntasks={parallel['nodes']}", "--ntasks-per-node=1",
		"/bin/sh", "-c", remote, "uib-fault-step",
		'$RUN_DIR/resolved-simulation.json',
		config["network"]["dataInterface"], '$RUN_DIR',
	]
	quoted = []
	for argument in command:
		if argument.startswith("$"):
			quoted.append(f'"{argument}"')
		else:
			quoted.append(shlex.quote(argument))
	return [
		"FAULT_STEP_PID=",
		"FAULT_STEP_STATUS=0",
		"FAULT_STEP_INTERRUPTED=0",
		"stop_fault_step() {",
		'\t[ -n "$FAULT_STEP_PID" ] || return 0',
		'\tif kill -0 "$FAULT_STEP_PID" 2>/dev/null; then',
		"\t\tFAULT_STEP_INTERRUPTED=1",
		'\t\tkill -TERM "$FAULT_STEP_PID" 2>/dev/null || true',
		"\tfi",
		'\tif wait "$FAULT_STEP_PID"; then',
		"\t\tfault_rc=0",
		"\telse",
		"\t\tfault_rc=$?",
		"\tfi",
		"\tif [ \"$FAULT_STEP_INTERRUPTED\" -eq 0 ] && "
		"[ \"$fault_rc\" -ne 0 ]; then",
		"\t\tFAULT_STEP_STATUS=$fault_rc",
		"\tfi",
		"\tFAULT_STEP_PID=",
		"}",
		" \\\n\t".join(quoted) + " &",
		"FAULT_STEP_PID=$!",
		"trap stop_fault_step EXIT INT TERM",
	]


def fault_evidence(config):
	if not config.get("faults"):
		return []
	return [
		"stop_fault_step",
		"trap - EXIT INT TERM",
		"FAULT_EXPECTED_TARGETS=$(jq -r '[.faults[] | select(has(\"at\") and "
		"(.action == \"add-delay\" or .action == \"packet-loss\" or "
		".action == \"link-down\")) | .target] | unique | length' "
		'"$RUN_DIR/resolved-simulation.json")',
		"FAULT_EXPECTED_EVENTS=$(jq -r '[.faults[] | select(has(\"at\") and "
		"(.action == \"add-delay\" or .action == \"packet-loss\" or "
		".action == \"link-down\"))] | length' "
		'"$RUN_DIR/resolved-simulation.json")',
		"FAULT_RECORDED=0",
		"FAULT_APPLIED=0",
		"FAULT_RECOVERED=0",
		'for evidence in "$RUN_DIR"/faults-*.jsonl; do',
		'\t[ -f "$evidence" ] || continue',
		"\tFAULT_RECORDED=$((FAULT_RECORDED + 1))",
		'\tcount=$(jq -s \'[.[] | select(.phase == "apply")] | length\' '
		'"$evidence")',
		"\tFAULT_APPLIED=$((FAULT_APPLIED + count))",
		'\tcount=$(jq -s \'[.[] | select(.phase == "recover")] | length\' '
		'"$evidence")',
		"\tFAULT_RECOVERED=$((FAULT_RECOVERED + count))",
		"done",
		'if [ "$FAULT_RECORDED" -ne "$FAULT_EXPECTED_TARGETS" ]; then',
		'\techo "fault targets do not match the allocated nodes" >&2',
		"\tFAULT_STEP_STATUS=2",
		"fi",
		'if [ "$FAULT_APPLIED" -ne "$FAULT_EXPECTED_EVENTS" ] || '
		'[ "$FAULT_RECOVERED" -ne "$FAULT_EXPECTED_EVENTS" ]; then',
		'\techo "fault schedule did not complete every transition" >&2',
		"\tFAULT_STEP_STATUS=2",
		"fi",
		'if [ "$FAULT_STEP_STATUS" -ne 0 ] && [ "$status" -eq 0 ]; then',
		"\tstatus=$FAULT_STEP_STATUS",
		"fi",
	]
