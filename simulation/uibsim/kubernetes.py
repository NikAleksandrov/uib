# SPDX-License-Identifier: Apache-2.0

import json
import pathlib
import re
import shutil
import sys
import tempfile


def kubernetes_resource(config):
    from .core import ConfigurationError

    kind = config["workload"]["kind"]
    if kind == "training":
        return "trainjob"
    if kind == "inference":
        return "rayjob"
    raise ConfigurationError(
        f"Kubernetes lifecycle does not support workload kind: {kind}")


def query_kubernetes_status(args, config, resource=None):
    from .core import ConfigurationError, KUBERNETES_TERMINAL_STATES
    from .faults import run_kubectl

    resource = resource or kubernetes_resource(config)
    result = run_kubectl(args, config, [
        "get", resource, args.run_id, "--namespace", args.run_id,
        "-o", "json",
    ], text=True, capture_output=True)
    if result.returncode:
        detail = result.stderr.strip() or result.stdout.strip()
        raise ConfigurationError(
            f"cannot query Kubernetes {resource}/{args.run_id}: {detail}")
    try:
        value = json.loads(result.stdout)
    except json.JSONDecodeError as error:
        raise ConfigurationError(
            f"kubectl returned invalid {resource} JSON: {error}") from error

    status = value.get("status", {})
    state = "PENDING"
    if resource == "rayjob":
        job_status = str(status.get("jobStatus", "")).upper()
        deployment = str(status.get("jobDeploymentStatus", "")).upper()
        if job_status == "SUCCEEDED" and deployment == "COMPLETE":
            state = "COMPLETED"
        elif job_status in {"FAILED", "STOPPED"}:
            state = "FAILED"
        elif job_status or deployment:
            state = job_status or deployment
    else:
        for condition in reversed(status.get("conditions", [])):
            if str(condition.get("status", "")).lower() != "true":
                continue
            condition_type = str(condition.get("type", "")).lower()
            if condition_type in {"complete", "completed", "succeeded"}:
                state = "COMPLETED"
                break
            if condition_type in {"failed", "failure"}:
                state = "FAILED"
                break

    pods_result = run_kubectl(args, config, [
        "get", "pods", "--namespace", args.run_id,
        "--selector", f"uib.dev/run-id={args.run_id}", "-o", "json",
    ], text=True, capture_output=True)
    if pods_result.returncode:
        detail = pods_result.stderr.strip() or pods_result.stdout.strip()
        raise ConfigurationError(f"cannot query Kubernetes pods: {detail}")
    try:
        pods = json.loads(pods_result.stdout).get("items", [])
    except json.JSONDecodeError as error:
        raise ConfigurationError(
            f"kubectl returned invalid pod JSON: {error}") from error
    phases = {}
    for pod in pods:
        phase = pod.get("status", {}).get("phase", "Unknown")
        phases[phase] = phases.get(phase, 0) + 1
    return {
        "resource": resource,
        "state": state,
        "terminal": state in KUBERNETES_TERMINAL_STATES,
        "object": value,
        "pods": pods,
        "podPhases": phases,
    }


def command_kubernetes_run(args):
    from .config import resolve
    from .core import ConfigurationError, KUBERNETES_RENDERER, LifecycleInterrupted, run_command
    from .faults import run_kubectl, start_kubernetes_fault_scheduler, stop_kubernetes_fault_scheduler
    from .state import prepare_run_directory, record_run_inputs, utc_now, write_json

    config = resolve(args)
    supported_faults = {
        "checkpoint-storage-delay", "delay-worker", "terminate-rank"}
    for index, fault in enumerate(config.get("faults", [])):
        if fault["action"] not in supported_faults:
            raise ConfigurationError(
                f"faults[{index}] {fault['action']} has no Kubernetes executor")
    config["runId"] = args.run_id
    run_dir = prepare_run_directory(args, config)
    resource = kubernetes_resource(config)
    ledger_path = run_dir / "ledger.json"
    ledger = {
        "schemaVersion": 1,
        "runId": args.run_id,
        "scheduler": "kubernetes",
        "state": "preparing",
        "createdAt": utc_now(),
        "manifest": "workload.yaml",
        "namespace": args.run_id,
        "resourceKind": resource,
        "resourceName": args.run_id,
        "resolvedConfig": "resolved-simulation.json",
    }
    write_json(ledger_path, ledger)
    config_path = run_dir / "resolved-simulation.json"
    write_json(config_path, config)
    try:
        ledger["inputs"] = record_run_inputs(args, run_dir)
    except ConfigurationError as error:
        ledger["state"] = "input-record-failed"
        ledger["error"] = str(error)
        write_json(ledger_path, ledger)
        raise
    write_json(ledger_path, ledger)

    manifest = run_dir / "workload.yaml"
    render_error = run_dir / "render.stderr"
    command = [str(KUBERNETES_RENDERER), str(config_path),
               "--run-id", args.run_id]
    if args.allow_mutable_tags:
        command.append("--allow-mutable-tags")
    try:
        with manifest.open("w", encoding="utf-8") as output, \
                render_error.open("w", encoding="utf-8") as errors:
            rendered = run_command(
                command, stdout=output, stderr=errors)
    except LifecycleInterrupted as error:
        ledger["state"] = "interrupted"
        ledger["interruptedStage"] = "render"
        ledger["error"] = str(error)
        write_json(ledger_path, ledger)
        raise
    except OSError as error:
        ledger["state"] = "render-failed"
        ledger["error"] = str(error)
        write_json(ledger_path, ledger)
        raise ConfigurationError(
            f"cannot render Kubernetes workload: {error}") from error
    if rendered.returncode:
        ledger["state"] = "render-failed"
        ledger["renderExitCode"] = rendered.returncode
        write_json(ledger_path, ledger)
        print(render_error.read_text(encoding="utf-8"), file=sys.stderr, end="")
        return rendered.returncode

    ledger["state"] = "rendered"
    write_json(ledger_path, ledger)
    if config.get("faults"):
        try:
            ledger["faultScheduler"] = start_kubernetes_fault_scheduler(
                args, config, config_path, run_dir)
        except ConfigurationError as error:
            ledger["state"] = "fault-scheduler-failed"
            ledger["error"] = str(error)
            write_json(ledger_path, ledger)
            raise
        write_json(ledger_path, ledger)
    try:
        submitted = run_kubectl(
            args, config, ["apply", "-f", str(manifest)],
            text=True, capture_output=True)
    except LifecycleInterrupted as error:
        ledger["state"] = "interrupted"
        ledger["interruptedStage"] = "submission"
        ledger["error"] = str(error)
        ledger["faultSchedulerCleanup"] = \
            stop_kubernetes_fault_scheduler(run_dir, ledger)
        cleanup = run_kubectl(args, config, [
            "delete", "namespace", args.run_id, "--ignore-not-found",
            "--wait=false",
        ])
        ledger["cleanup"] = (
            "requested" if cleanup.returncode == 0 else "failed")
        ledger["cleanupExitCode"] = cleanup.returncode
        write_json(ledger_path, ledger)
        raise
    (run_dir / "kubectl-apply.stdout").write_text(
        submitted.stdout, encoding="utf-8")
    (run_dir / "kubectl-apply.stderr").write_text(
        submitted.stderr, encoding="utf-8")
    if submitted.returncode:
        ledger["state"] = "submission-failed"
        ledger["submissionExitCode"] = submitted.returncode
        ledger["faultSchedulerCleanup"] = \
            stop_kubernetes_fault_scheduler(run_dir, ledger)
        write_json(ledger_path, ledger)
        if submitted.stderr:
            print(submitted.stderr, file=sys.stderr, end="")
        return submitted.returncode
    ledger["state"] = "submitted"
    ledger["submittedAt"] = utc_now()
    write_json(ledger_path, ledger)
    print(f"{resource}/{args.run_id}")
    return 0


def command_kubernetes_status(args):
    from .state import load_ledger, owned_state_file, read_json, state_path

    unused, ledger = load_ledger(args, "kubernetes")
    config_path = owned_state_file(
        state_path(args), ledger["resolvedConfig"], "resolved configuration")
    config = read_json(config_path, "resolved configuration")
    status = query_kubernetes_status(args, config, ledger["resourceKind"])
    print(json.dumps({
        "namespace": args.run_id,
        "podPhases": status["podPhases"],
        "resource": f"{status['resource']}/{args.run_id}",
        "state": status["state"],
        "terminal": status["terminal"],
    }, indent=2, sort_keys=True))
    return 0


from .kubernetes_collection import command_kubernetes_collect


def command_kubernetes_down(args):
    from .core import ConfigurationError
    from .faults import run_kubectl, stop_kubernetes_fault_scheduler
    from .state import load_ledger, owned_state_file, read_json, state_path, utc_now, write_json

    ledger_path, ledger = load_ledger(args, "kubernetes")
    run_dir = state_path(args)
    config = read_json(owned_state_file(
        run_dir, ledger["resolvedConfig"], "resolved configuration"),
        "resolved configuration")
    ledger["faultSchedulerCleanup"] = \
        stop_kubernetes_fault_scheduler(run_dir, ledger)
    if ledger["faultSchedulerCleanup"] == "failed":
        write_json(ledger_path, ledger)
        raise ConfigurationError(
            "cannot stop the owned Kubernetes fault scheduler")
    result = run_kubectl(args, config, [
        "delete", "namespace", args.run_id, "--ignore-not-found",
        "--wait=false",
    ])
    if result.returncode:
        return result.returncode
    ledger["state"] = "cleanup-requested"
    ledger["cleanupRequestedAt"] = utc_now()
    write_json(ledger_path, ledger)
    return 0
