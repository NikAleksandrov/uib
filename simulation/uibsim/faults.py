# SPDX-License-Identifier: Apache-2.0

import json
import os
import pathlib
import re
import signal
import subprocess
import sys
import time


def kubectl_command(args, config):
    from .core import ConfigurationError
    from .state import state_path

    command = [os.environ.get("KUBECTL_COMMAND", "kubectl")]
    supplied = getattr(args, "kubeconfig", None)
    owned = state_path(args) / "infrastructure" / "kubeconfig"
    if supplied:
        command.extend(["--kubeconfig", str(
            pathlib.Path(supplied).expanduser().resolve())])
    elif owned.is_file():
        command.extend(["--kubeconfig", str(owned)])

    context = config.get("infrastructure", {}).get("kubernetesContext")
    provider = config["infrastructure"]["provider"]
    if context == "replace-me":
        raise ConfigurationError(
            "infrastructure.kubernetesContext must name the target context")
    if context:
        command.extend(["--context", context])
    elif provider == "kind" and not supplied:
        command.extend(["--context", f"kind-{args.run_id}"])
    return command


def run_kubectl(args, config, arguments, **kwargs):
    from .core import ConfigurationError, run_command

    command = [*kubectl_command(args, config), *arguments]
    try:
        return run_command(command, **kwargs)
    except OSError as error:
        raise ConfigurationError(f"cannot invoke kubectl: {error}") from error


def fault_offset_seconds(value):
    from .core import ConfigurationError

    match = re.fullmatch(r"([0-9]+)(ms|s|m|h)", value)
    if not match:
        raise ConfigurationError(f"invalid time offset: {value}")
    return int(match.group(1)) * {
        "ms": 0.001,
        "s": 1,
        "m": 60,
        "h": 3600,
    }[match.group(2)]


def invoke_kubernetes_fault(command, arguments, input_value=None):
    from .core import ConfigurationError

    try:
        completed = subprocess.run(
            [*command, *arguments], input=input_value, text=True,
            capture_output=True, check=False)
    except OSError as error:
        raise ConfigurationError(f"cannot invoke kubectl: {error}") from error
    return {
        "argv": [*command, *arguments],
        "exitCode": completed.returncode,
        "stdout": completed.stdout,
        "stderr": completed.stderr,
    }


def append_fault_record(output, record):
    output.write(json.dumps(record, sort_keys=True) + "\n")
    output.flush()


def kubernetes_workload_pod(command, namespace, rank, timeout):
    from .core import ConfigurationError

    deadline = time.monotonic() + timeout
    last_error = ""
    while time.monotonic() < deadline:
        result = invoke_kubernetes_fault(command, [
            "get", "pods", "--namespace", namespace, "-o", "json",
        ])
        if not result["exitCode"]:
            try:
                items = json.loads(result["stdout"]).get("items", [])
            except json.JSONDecodeError as error:
                last_error = str(error)
            else:
                pods = []
                for item in items:
                    init = item.get("spec", {}).get("initContainers", [])
                    if any(container.get("name") == "uib-runtime-check"
                           for container in init):
                        pods.append(item)
                pods.sort(key=lambda item: item["metadata"]["name"])
                if rank < len(pods):
                    return pods[rank]["metadata"]["name"]
        else:
            last_error = result["stderr"].strip()
        time.sleep(0.2)
    raise ConfigurationError(
        f"rank {rank} did not acquire a workload pod: {last_error}")


def kubernetes_chaos_manifest(namespace, name, fault):
    path = fault["target"].rstrip("/")
    return json.dumps({
        "apiVersion": "chaos-mesh.org/v1alpha1",
        "kind": "IOChaos",
        "metadata": {"name": name, "namespace": namespace},
        "spec": {
            "action": "latency",
            "mode": "all",
            "selector": {"namespaces": [namespace]},
            "volumePath": path,
            "path": f"{path}/**/*",
            "delay": fault["latency"],
            "percent": 100,
        },
    }) + "\n"


def apply_kubernetes_fault(command, namespace, index, fault, phase,
                           wait_timeout):
    action = fault["action"]
    if action == "delay-worker":
        node = fault["target"].removesuffix("/data")
        operation = "cordon" if phase == "apply" else "uncordon"
        return invoke_kubernetes_fault(command, [operation, node])
    if action == "terminate-rank":
        pod = kubernetes_workload_pod(
            command, namespace, fault["rank"], wait_timeout)
        result = invoke_kubernetes_fault(command, [
            "delete", "pod", pod, "--namespace", namespace,
            "--wait=false",
        ])
        result["pod"] = pod
        return result
    name = f"uib-checkpoint-delay-{index}"
    if phase == "apply":
        return invoke_kubernetes_fault(
            command, ["apply", "-f", "-"],
            kubernetes_chaos_manifest(namespace, name, fault))
    return invoke_kubernetes_fault(command, [
        "delete", "iochaos", name, "--namespace", namespace,
        "--ignore-not-found", "--wait=false",
    ])


def kubernetes_fault_target(fault):
    if fault["action"] == "terminate-rank":
        return f"rank-{fault['rank']}"
    return fault["target"]


def run_kubernetes_fault_schedule(config, namespace, output, ready, command,
                                  wait_timeout):
    from .core import ConfigurationError

    actions = {
        "checkpoint-storage-delay", "delay-worker", "terminate-rank"}
    events = []
    active_workers = {}
    active_storage = {}
    ready_written = False
    completed = False

    for index, fault in enumerate(config.get("faults", [])):
        if fault["action"] not in actions:
            continue
        if "afterEvent" in fault:
            raise ConfigurationError(
                f"faults[{index}] is event-relative; use an event executor")
        start = fault_offset_seconds(fault["at"])
        events.append((start, 1, index, "apply", fault))
        if fault["action"] != "terminate-rank":
            events.append((
                start + fault_offset_seconds(fault["duration"]),
                0, index, "recover", fault))

    if any(fault["action"] == "checkpoint-storage-delay"
           for fault in config.get("faults", [])):
        result = invoke_kubernetes_fault(command, [
            "api-resources", "--api-group=chaos-mesh.org", "-o", "name",
        ])
        if (result["exitCode"] or
                "iochaos.chaos-mesh.org" not in result["stdout"].split()):
            raise ConfigurationError(
                "checkpoint delay requires the IOChaos API")

    began = time.monotonic()
    try:
        for planned, unused, index, phase, fault in sorted(events):
            if not ready_written and not (
                    planned == 0 and phase == "apply" and
                    fault["action"] == "delay-worker"):
                ready.write_text("ready\n", encoding="utf-8")
                ready_written = True
            remaining = began + planned - time.monotonic()
            if remaining > 0:
                time.sleep(remaining)
            result = apply_kubernetes_fault(
                command, namespace, index, fault, phase, wait_timeout)
            if not result["exitCode"]:
                if fault["action"] == "delay-worker":
                    if phase == "apply":
                        active_workers[index] = fault
                    else:
                        active_workers.pop(index, None)
                elif fault["action"] == "checkpoint-storage-delay":
                    if phase == "apply":
                        active_storage[index] = fault
                    else:
                        active_storage.pop(index, None)
            append_fault_record(output, {
                "schemaVersion": 1,
                "sourceIndex": index,
                "phase": phase,
                "action": fault["action"],
                "target": kubernetes_fault_target(fault),
                "plannedOffsetSeconds": planned,
                "actualOffsetSeconds": time.monotonic() - began,
                "command": result,
                "result": "applied" if not result["exitCode"] else "failed",
            })
            if result["exitCode"]:
                raise ConfigurationError(
                    f"faults[{index}] {phase} command failed")
            if not ready_written:
                ready.write_text("ready\n", encoding="utf-8")
                ready_written = True
        if not ready_written:
            ready.write_text("ready\n", encoding="utf-8")
        completed = True
    finally:
        if not completed:
            active = [
                *sorted(active_storage.items(), reverse=True),
                *sorted(active_workers.items(), reverse=True),
            ]
            for index, fault in active:
                result = apply_kubernetes_fault(
                    command, namespace, index, fault, "recover", wait_timeout)
                append_fault_record(output, {
                    "schemaVersion": 1,
                    "sourceIndex": index,
                    "phase": "cleanup",
                    "action": fault["action"],
                    "target": kubernetes_fault_target(fault),
                    "command": result,
                    "result": (
                        "restored" if not result["exitCode"] else "failed"),
                })


def command_internal_kubernetes_fault_schedule(args):
    from .core import ConfigurationError, RUN_ID
    from .state import read_json

    config = read_json(
        pathlib.Path(args.config).resolve(), "resolved configuration")
    if not RUN_ID.fullmatch(args.namespace):
        raise ConfigurationError("invalid namespace")
    if not args.kubectl:
        raise ConfigurationError("kubectl command is required")
    output_path = pathlib.Path(args.output).resolve()
    ready_path = pathlib.Path(args.ready).resolve()
    try:
        output_path.parent.mkdir(parents=True, exist_ok=True)
        with output_path.open("x", encoding="utf-8") as output:
            run_kubernetes_fault_schedule(
                config, args.namespace, output, ready_path, args.kubectl,
                args.wait_timeout)
    except OSError as error:
        raise ConfigurationError(
            f"cannot execute Kubernetes fault schedule: {error}") from error
    return 0


def process_start_time(pid):
    try:
        value = pathlib.Path(f"/proc/{pid}/stat").read_text(encoding="utf-8")
    except OSError:
        return None
    fields = value.rpartition(") ")[2].split()
    return fields[19] if len(fields) > 19 else None


def owned_process_alive(process):
    pid = process.get("pid")
    start = process.get("startTime")
    return (isinstance(pid, int) and pid > 1 and isinstance(start, str) and
            process_start_time(pid) == start)


def stop_kubernetes_fault_scheduler(run_dir, ledger):
    process = ledger.get("faultScheduler")
    if not isinstance(process, dict):
        return "not-configured"
    if not owned_process_alive(process):
        return "complete"
    pid = process["pid"]
    try:
        os.killpg(pid, signal.SIGTERM)
    except ProcessLookupError:
        return "complete"
    deadline = time.monotonic() + 15
    while owned_process_alive(process) and time.monotonic() < deadline:
        time.sleep(0.05)
    if owned_process_alive(process):
        try:
            os.killpg(pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
    return "stopped" if not owned_process_alive(process) else "failed"


def start_kubernetes_fault_scheduler(args, config, config_path, run_dir):
    from .core import CLI, ConfigurationError, terminate_command

    output = run_dir / "faults-kubernetes.jsonl"
    ready = run_dir / ".fault-scheduler-ready"
    stdout_path = run_dir / "fault-scheduler.stdout"
    stderr_path = run_dir / "fault-scheduler.stderr"
    command = [
        sys.executable, str(CLI),
        "_kubernetes-fault-schedule", str(config_path),
        "--namespace", args.run_id,
        "--output", str(output),
        "--ready", str(ready),
        "--kubectl", *kubectl_command(args, config),
    ]
    try:
        with stdout_path.open("x", encoding="utf-8") as stdout, \
                stderr_path.open("x", encoding="utf-8") as stderr:
            process = subprocess.Popen(
                command, stdout=stdout, stderr=stderr,
                start_new_session=True)
    except OSError as error:
        raise ConfigurationError(
            f"cannot start Kubernetes fault scheduler: {error}") from error
    start = process_start_time(process.pid)
    if start is None:
        terminate_command(process)
        raise ConfigurationError(
            "cannot record Kubernetes fault scheduler ownership")
    ownership = {
        "pid": process.pid,
        "startTime": start,
        "evidence": output.name,
        "stdout": stdout_path.name,
        "stderr": stderr_path.name,
    }
    deadline = time.monotonic() + 30
    while not ready.is_file() and time.monotonic() < deadline:
        if process.poll() is not None:
            detail = stderr_path.read_text(encoding="utf-8").strip()
            raise ConfigurationError(
                "Kubernetes fault scheduler failed before readiness: " +
                (detail or f"exit code {process.returncode}"))
        time.sleep(0.05)
    if not ready.is_file():
        terminate_command(process)
        raise ConfigurationError(
            "Kubernetes fault scheduler readiness timed out")
    ready.unlink(missing_ok=True)
    return ownership
