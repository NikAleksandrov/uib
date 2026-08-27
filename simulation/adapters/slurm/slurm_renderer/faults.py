# SPDX-License-Identifier: Apache-2.0

import shlex


def task_wrapper(uib_enabled, terminate_rank):
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
	if terminate_rank:
		lines.extend([
			"rank_fault_pid=$$",
			"(",
			"\tmarker=/results/.uib-terminate-rank-$RANK",
			"\twhile [ ! -e \"$marker\" ]; do sleep 0.05; done",
			"\tprintf '%s\\n' \"$rank_fault_pid\" > \"$marker.ack\"",
			"\tkill -TERM \"$rank_fault_pid\"",
			") &",
		])
	lines.append('exec "$@"')
	return "\n".join(lines)


def fault_step(config):
	if not any(fault["action"] in (
		"add-delay", "packet-loss", "link-down"
	) for fault in config.get("faults", [])):
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


def rank_fault_step(config):
	if not any(fault["action"] == "terminate-rank"
	           for fault in config.get("faults", [])):
		return []
	adapter = ("/run/uib-rank-schedule"
	           if config["infrastructure"]["provider"] == "virtme"
	           else "/usr/local/libexec/uib-rank-schedule")
	command = [
		adapter, "$RUN_DIR/resolved-simulation.json",
		"--marker-dir", "$RUN_DIR", "--output",
		"$RUN_DIR/faults-ranks.jsonl",
	]
	quoted = [f'"{argument}"' if argument.startswith("$") else
	          shlex.quote(argument) for argument in command]
	stop = "stop_rank_fault"
	if any(fault["action"] in ("add-delay", "packet-loss", "link-down")
	       for fault in config["faults"]):
		stop = "stop_fault_step; stop_rank_fault"
	return [
		"RANK_FAULT_PID=",
		"RANK_FAULT_STATUS=0",
		"RANK_FAULT_INTERRUPTED=0",
		"stop_rank_fault() {",
		'\t[ -n "$RANK_FAULT_PID" ] || return 0',
		"\trank_fault_wait=0",
		'\twhile kill -0 "$RANK_FAULT_PID" 2>/dev/null && '
		'[ "$rank_fault_wait" -lt 20 ]; do',
		"\t\tsleep 0.05",
		"\t\trank_fault_wait=$((rank_fault_wait + 1))",
		"\tdone",
		'\tif kill -0 "$RANK_FAULT_PID" 2>/dev/null; then',
		"\t\tRANK_FAULT_INTERRUPTED=1",
		'\t\tkill -TERM "$RANK_FAULT_PID" 2>/dev/null || true',
		"\tfi",
		'\tif wait "$RANK_FAULT_PID"; then',
		"\t\trank_fault_rc=0",
		"\telse",
		"\t\trank_fault_rc=$?",
		"\tfi",
		"\tif [ \"$RANK_FAULT_INTERRUPTED\" -eq 0 ] && "
		"[ \"$rank_fault_rc\" -ne 0 ]; then",
		"\t\tRANK_FAULT_STATUS=$rank_fault_rc",
		"\tfi",
		"\tRANK_FAULT_PID=",
		"}",
		" \\\n\t".join(quoted) + " &",
		"RANK_FAULT_PID=$!",
		f"trap '{stop}' EXIT INT TERM",
	]


def fault_evidence(config):
	if not config.get("faults"):
		return []
	network = any(fault["action"] in (
		"add-delay", "packet-loss", "link-down"
	) for fault in config["faults"])
	rank = any(fault["action"] == "terminate-rank"
	           for fault in config["faults"])
	lines = []
	if network:
		lines.append("stop_fault_step")
	if rank:
		lines.append("stop_rank_fault")
	lines.append("trap - EXIT INT TERM")
	if network:
		lines.extend([
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
		])
	if rank:
		lines.extend([
			"RANK_FAULT_RANK=$(jq -r '[.faults[] | select(.action == "
			"\"terminate-rank\")][0].rank' "
			'"$RUN_DIR/resolved-simulation.json")',
			"RANK_FAULT_APPLIED=$(jq -s '[.[] | select(.phase == "
			"\"apply\" and .action == \"terminate-rank\" and "
			".result == \"applied\")] | length' "
			'"$RUN_DIR/faults-ranks.jsonl" 2>/dev/null || echo 0)',
			'if [ "$RANK_FAULT_APPLIED" -ne 1 ]; then',
			'\techo "rank termination did not execute" >&2',
			"\tRANK_FAULT_STATUS=2",
			"fi",
			'if [ ! -s "$RUN_DIR/.uib-terminate-rank-'
			'$RANK_FAULT_RANK.ack" ]; then',
			'\techo "selected rank did not acknowledge termination" >&2',
			"\tRANK_FAULT_STATUS=2",
			"fi",
			'if [ "$workload_status" -eq 0 ]; then',
			'\techo "terminated rank did not fail the workload step" >&2',
			"\tRANK_FAULT_STATUS=2",
			"fi",
			'if [ "$RANK_FAULT_STATUS" -eq 0 ]; then',
			"\tRANK_FAULT_EXPECTED=true",
			"\tstatus=0",
			"else",
			"\tstatus=$RANK_FAULT_STATUS",
			"fi",
			'rm -f "$RUN_DIR"/.uib-terminate-rank-*',
		])
	return lines
