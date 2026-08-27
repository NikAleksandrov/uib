# SPDX-License-Identifier: Apache-2.0

import copy
import json
import shlex


def workload_command(config):
	workload = config["workload"]
	command = [workload["entrypoint"], *workload.get("arguments", [])]
	if workload["kind"] == "training":
		return command
	config_path = "/etc/uib-workload/workload.json"
	executable = shlex.quote(workload["entrypoint"])
	quoted_config = shlex.quote(config_path)
	lifecycle = (
		"set -eu; "
		f"{executable} serve --config {quoted_config} & server=$!; "
		"cleanup() { kill \"$server\" >/dev/null 2>&1 || true; }; "
		"trap cleanup EXIT INT TERM; "
		f"{executable} probe --config {quoted_config}; "
		"kill \"$server\" >/dev/null 2>&1 || true; "
		"wait \"$server\" || true; trap - EXIT INT TERM"
	)
	return ["/bin/sh", "-c", lifecycle]


def render(config, run_id, allow_mutable_tags):
	from .core import shell_assignment, validate_config
	from .environment import container_mounts, directives, environment
	from .faults import fault_evidence, fault_step, rank_fault_step, task_wrapper

	validate_config(config, run_id, allow_mutable_tags)
	config = copy.deepcopy(config)
	config["runId"] = run_id
	workload_config = copy.deepcopy(config["workload"]["parameters"]["config"])
	workload_config["runId"] = run_id
	storage = config["storage"]
	values = environment(config)
	environment_names = sorted(values)
	environment_names.extend([
		"MASTER_ADDR",
		"SLURM_LOCALID",
		"SLURM_NTASKS",
		"SLURM_PROCID",
	])

	lines = ["#!/bin/sh", *directives(config, run_id), "", "set -eu", ""]
	lines.extend([
		shell_assignment("RUN_ID", run_id),
		shell_assignment("RESULTS_ROOT", storage["resultsPath"]),
		'RUN_DIR="$RESULTS_ROOT/$RUN_ID"',
		'if [ ! -d "$RESULTS_ROOT" ]; then',
		'\techo "results root does not exist: $RESULTS_ROOT" >&2',
		'\texit 2',
		'fi',
		'if [ -e "$RUN_DIR" ]; then',
		'\techo "run result directory already exists: $RUN_DIR" >&2',
		'\texit 2',
		'fi',
		'mkdir "$RUN_DIR"',
		'umask 077',
		"",
		'cat > "$RUN_DIR/workload.json" <<\'UIB_WORKLOAD_JSON_EOF\'',
		json.dumps(workload_config, indent=2, sort_keys=True),
		"UIB_WORKLOAD_JSON_EOF",
		'cat > "$RUN_DIR/resolved-simulation.json" <<\'UIB_RESOLVED_JSON_EOF\'',
		json.dumps(config, indent=2, sort_keys=True),
		"UIB_RESOLVED_JSON_EOF",
		"",
		'scontrol show job "$SLURM_JOB_ID" > "$RUN_DIR/slurm-job.txt"',
		'scontrol show hostnames "$SLURM_JOB_NODELIST" > "$RUN_DIR/nodes.txt"',
		'MASTER_NODE=$(sed -n \'1p\' "$RUN_DIR/nodes.txt")',
		'MASTER_ADDR=$(scontrol show node "$MASTER_NODE" --oneliner | '
		"awk '{ for (field = 1; field <= NF; field++) "
		"if ($field ~ /^NodeAddr=/) { sub(/^NodeAddr=/, \"\", $field); "
		"print $field; exit } }')",
		'if [ -z "$MASTER_ADDR" ]; then',
		'\techo "Slurm allocation has no node address" >&2',
		'\texit 2',
		'fi',
		'export MASTER_ADDR',
	])
	for name, value in sorted(values.items()):
		lines.append(f"export {shell_assignment(name, value)}")
	lines.extend([
		'if [ ! -d ' + shlex.quote(storage["modelCachePath"]) + ' ]; then',
		'\techo "model cache does not exist: '
		+ storage["modelCachePath"] + '" >&2',
		'\texit 2',
		'fi',
	])
	if config["uib"]["enabled"]:
		lines.extend([
			'if [ ! -d ' + shlex.quote(storage["uibRuntimePath"]) + ' ]; then',
			'\techo "UIB runtime does not exist: '
			+ storage["uibRuntimePath"] + '" >&2',
			'\texit 2',
			'fi',
		])
	lines.extend(["", *fault_step(config), *rank_fault_step(config)])

	mounts = container_mounts(config)
	lines.extend([
		"",
		'MOUNTS=' + shlex.quote(",".join(mounts)),
		'MOUNTS="$MOUNTS,$RUN_DIR/workload.json:'
		'/etc/uib-workload/workload.json:ro,$RUN_DIR:/results"',
		"",
	])

	parallel = config["workload"]["parallelism"]
	srun = [
		"srun",
		f"--nodes={parallel['nodes']}",
		f"--ntasks={parallel['nodes'] * parallel['processesPerNode']}",
		f"--ntasks-per-node={parallel['processesPerNode']}",
		f"--cpus-per-task={config['nodes']['cpu'] // parallel['processesPerNode']}",
		f"--container-image={config['workload']['image']}",
		'--container-mounts=$MOUNTS',
		"--container-workdir=/results",
		"--no-container-mount-home",
		"--no-container-entrypoint",
		f"--container-env={','.join(environment_names)}",
		"/bin/sh",
		"-c",
		task_wrapper(
			config["uib"]["enabled"],
			any(fault["action"] == "terminate-rank"
			    for fault in config.get("faults", []))),
		"uib-slurm",
		*workload_command(config),
	]
	if config["nodes"]["gpus"]["perWorker"]:
		gpus_per_task = (
			config["nodes"]["gpus"]["perWorker"]
			// parallel["processesPerNode"]
		)
		srun.insert(5, f"--gpus-per-task={gpus_per_task}")
	quoted = []
	for argument in srun:
		if argument == "--container-mounts=$MOUNTS":
			quoted.append('"--container-mounts=$MOUNTS"')
		else:
			quoted.append(shlex.quote(argument))
	lines.append(" \\\n\t".join(quoted) + " || status=$?")
	lines.extend([
		'workload_status=${status:-0}',
		'status=$workload_status',
		'RANK_FAULT_EXPECTED=false',
		*fault_evidence(config),
		'cat > "$RUN_DIR/status.json" <<UIB_STATUS_JSON_EOF',
		'{',
		f'  "runId": "{run_id}",',
		'  "scheduler": "slurm",',
		'  "jobId": "$SLURM_JOB_ID",',
		'  "exitCode": $status,',
		'  "workloadExitCode": $workload_status,',
		'  "expectedFaultOutcome": $RANK_FAULT_EXPECTED',
		'}',
		'UIB_STATUS_JSON_EOF',
		'exit "$status"',
	])
	print("\n".join(lines))
