# SPDX-License-Identifier: Apache-2.0

import json
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile

from .dependencies import yaml


def command_render(args):
    from .config import resolve

    config = resolve(args)
    if args.format == "json":
        print(json.dumps(config, indent=2, sort_keys=True))
    else:
        print(yaml.safe_dump(config, sort_keys=True), end="")
    return 0


def command_doctor(args):
    from .config import find_overlay, find_profile, resolve, tools_for

    config = resolve(args)
    tools = {name: shutil.which(name) for name in tools_for(config)}
    report = {
        "schemaVersion": 1,
        "readOnly": True,
        "profile": str(find_profile(args.profile)),
        "overlays": [str(find_overlay(name)) for name in args.overlay],
        "provider": config["infrastructure"]["provider"],
        "manager": config["cluster"]["manager"],
        "frontend": config["communication"]["frontend"],
        "tools": tools,
        "missing": [name for name, path in tools.items() if not path],
    }
    print(json.dumps(report, indent=2, sort_keys=True))
    return 1 if report["missing"] else 0


def invoke_backend(config, arguments):
    from .core import ConfigurationError, KIND_BACKEND

    provider = config["infrastructure"]["provider"]
    if provider != "kind":
        raise ConfigurationError(
            f"lifecycle is not implemented for provider: {provider}")
    with tempfile.NamedTemporaryFile(mode="w", encoding="utf-8",
                                     prefix="uib-sim-", suffix=".json") as output:
        json.dump(config, output, indent=2, sort_keys=True)
        output.flush()
        try:
            result = subprocess.run(
                [str(KIND_BACKEND), *arguments(output.name)], check=False)
            return result.returncode
        except OSError as error:
            raise ConfigurationError(f"cannot invoke Kind backend: {error}") from error


def invoke_libvirt(config, arguments):
    from .core import ConfigurationError, LIBVIRT_BACKEND

    with tempfile.NamedTemporaryFile(mode="w", encoding="utf-8",
                                     prefix="uib-sim-", suffix=".json") as output:
        json.dump(config, output, indent=2, sort_keys=True)
        output.flush()
        try:
            result = subprocess.run(
                [str(LIBVIRT_BACKEND), *arguments(output.name)], check=False)
        except OSError as error:
            raise ConfigurationError(
                f"cannot invoke libvirt backend: {error}") from error
    return result.returncode


def command_up(args):
    from .config import resolve
    from .core import ConfigurationError
    from .state import state_path

    config = resolve(args)

    if config["infrastructure"]["provider"] == "libvirt":
        if config["cluster"]["manager"] != "slurm":
            raise ConfigurationError(
                "libvirt Kubernetes provisioning is not implemented yet")

        def libvirt_arguments(path):
            command = [
                "up", path, "--name", args.run_id,
                "--state-dir", str(state_path(args) / "infrastructure"),
            ]
            if args.retain:
                command.append("--retain")
            return command

        return invoke_libvirt(config, libvirt_arguments)

    def arguments(path):
        command = ["up", path, "--name", args.run_id, "--wait", args.wait]
        if args.kubeconfig:
            command.extend(["--kubeconfig", args.kubeconfig])
        if args.retain:
            command.append("--retain")
        return command

    return invoke_backend(config, arguments)


def command_status(args):
    from .config import resolve
    from .slurm import command_slurm_status
    from .state import state_path

    config = resolve(args)
    provider = config["infrastructure"]["provider"]
    if provider == "libvirt" and not (state_path(args) / "ledger.json").is_file():
        return invoke_libvirt(config, lambda unused: [
            "status", "--name", args.run_id,
            "--state-dir", str(state_path(args) / "infrastructure"),
        ])
    if config["cluster"]["manager"] == "slurm":
        return command_slurm_status(args)
    return invoke_backend(
        config, lambda unused: ["status", "--name", args.run_id])


def command_kubeconfig(args):
    from .config import resolve

    config = resolve(args)
    return invoke_backend(
        config, lambda unused: ["kubeconfig", "--name", args.run_id])


def command_load_image(args):
    from .config import resolve
    from .images import stage_libvirt_image

    config = resolve(args)

    if config["infrastructure"]["provider"] == "libvirt":
        for reference in args.image:
            print(stage_libvirt_image(args, reference,
                                      args.allow_mutable_tags))
        return 0

    def arguments(unused):
        command = ["load-image", "--name", args.run_id]
        for image in args.image:
            command.extend(["--image", image])
        if args.allow_mutable_tags:
            command.append("--allow-mutable-tags")
        return command

    return invoke_backend(config, arguments)


def command_load_cache(args):
    from .config import resolve
    from .core import ConfigurationError
    from .images import stage_huggingface_cache

    config = resolve(args)
    if config["infrastructure"]["provider"] != "libvirt":
        raise ConfigurationError(
            "load-cache is currently implemented only for libvirt")
    print(stage_huggingface_cache(args, pathlib.Path(args.source)))
    return 0


def command_render_workload(args):
    from .config import resolve
    from .core import ConfigurationError, KUBERNETES_RENDERER, SLURM_RENDERER

    config = resolve(args)
    manager = config["cluster"]["manager"]
    renderers = {
        "kubernetes": KUBERNETES_RENDERER,
        "slurm": SLURM_RENDERER,
    }
    renderer = renderers[manager]
    with tempfile.NamedTemporaryFile(mode="w", encoding="utf-8",
                                     prefix="uib-sim-", suffix=".json") as output:
        json.dump(config, output, indent=2, sort_keys=True)
        output.flush()
        command = [str(renderer), output.name,
                   "--run-id", args.run_id]
        if args.allow_mutable_tags:
            command.append("--allow-mutable-tags")
        try:
            return subprocess.run(command, check=False).returncode
        except OSError as error:
            raise ConfigurationError(
                f"cannot invoke {manager} renderer: {error}") from error


def command_run(args):
    from .config import resolve
    from .core import ConfigurationError, SLURM_RENDERER
    from .scheduler import remote_path_exists, run_scheduler, scheduler_remote
    from .state import read_json, record_run_inputs, state_path, utc_now, write_json

    config = resolve(args)
    if config["cluster"]["manager"] != "slurm":
        raise ConfigurationError("run is currently implemented only for Slurm")
    config["runId"] = args.run_id
    run_dir = state_path(args)
    try:
        run_dir.parent.mkdir(parents=True, exist_ok=True)
        if run_dir.exists():
            infrastructure = run_dir / "infrastructure" / "ledger.json"
            with infrastructure.open(encoding="utf-8") as source:
                infrastructure_ledger = json.load(source)
            if (config["infrastructure"]["provider"] != "libvirt" or
                    infrastructure_ledger.get("schemaVersion") != 1 or
                    infrastructure_ledger.get("runId") != args.run_id or
                    infrastructure_ledger.get("provider") != "libvirt" or
                    infrastructure_ledger.get("state") != "ready" or
                    (run_dir / "ledger.json").exists()):
                raise FileExistsError
            infrastructure_config = read_json(
                run_dir / "infrastructure" / "resolved-simulation.json",
                "infrastructure configuration")
            if infrastructure_config.get("network") != config["network"]:
                raise ConfigurationError(
                    "workload network configuration differs from the "
                    "provisioned libvirt network")
        else:
            run_dir.mkdir()
    except (json.JSONDecodeError, IsADirectoryError) as error:
        raise ConfigurationError(
            f"invalid infrastructure state in {run_dir}") from error
    except FileExistsError as error:
        raise ConfigurationError(f"run state already exists: {run_dir}") from error
    except OSError as error:
        raise ConfigurationError(
            f"cannot create run state {run_dir}: {error}") from error

    ledger_path = run_dir / "ledger.json"
    ledger = {
        "schemaVersion": 1,
        "runId": args.run_id,
        "scheduler": "slurm",
        "state": "preparing",
        "createdAt": utc_now(),
        "batchFile": "workload.sbatch",
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

    required_paths = [
        config["storage"]["modelCachePath"],
        config["storage"]["resultsPath"],
    ]
    if config["uib"]["enabled"]:
        required_paths.append(config["storage"]["uibRuntimePath"])
    missing = [path for path in required_paths
               if not remote_path_exists(args, path, "directory")]
    if missing:
        ledger["state"] = "preflight-failed"
        ledger["missingPaths"] = missing
        write_json(ledger_path, ledger)
        raise ConfigurationError(
            "Slurm shared paths do not exist: " + ", ".join(missing))

    batch_path = run_dir / "workload.sbatch"
    render_error = run_dir / "render.stderr"
    command = [str(SLURM_RENDERER), str(config_path),
               "--run-id", args.run_id]
    if args.allow_mutable_tags:
        command.append("--allow-mutable-tags")
    try:
        with batch_path.open("w", encoding="utf-8") as batch, \
                render_error.open("w", encoding="utf-8") as errors:
            rendered = subprocess.run(
                command, stdout=batch, stderr=errors, check=False)
    except OSError as error:
        ledger["state"] = "render-failed"
        ledger["error"] = str(error)
        write_json(ledger_path, ledger)
        raise ConfigurationError(f"cannot render Slurm workload: {error}") from error
    if rendered.returncode:
        ledger["state"] = "render-failed"
        ledger["renderExitCode"] = rendered.returncode
        write_json(ledger_path, ledger)
        try:
            print(render_error.read_text(encoding="utf-8"),
                  file=sys.stderr, end="")
        except OSError:
            pass
        return rendered.returncode

    ledger["state"] = "rendered"
    write_json(ledger_path, ledger)
    try:
        if scheduler_remote(args):
            submitted = run_scheduler(
                args, "SBATCH_COMMAND", "sbatch", ["--parsable"],
                input=batch_path.read_text(encoding="utf-8"), text=True,
                capture_output=True)
            ledger["schedulerTransport"] = "libvirt-ssh"
        else:
            submitted = run_scheduler(
                args, "SBATCH_COMMAND", "sbatch",
                ["--parsable", str(batch_path)], text=True,
                capture_output=True)
            ledger["schedulerTransport"] = "local"
    except (ConfigurationError, OSError) as error:
        ledger["state"] = "submission-not-started"
        ledger["error"] = str(error)
        write_json(ledger_path, ledger)
        raise ConfigurationError(f"cannot invoke sbatch: {error}") from error
    try:
        (run_dir / "sbatch.stdout").write_text(
            submitted.stdout, encoding="utf-8")
        (run_dir / "sbatch.stderr").write_text(
            submitted.stderr, encoding="utf-8")
    except OSError as error:
        ledger["state"] = "submission-unknown"
        ledger["error"] = f"cannot record sbatch output: {error}"
        write_json(ledger_path, ledger)
        raise ConfigurationError(
            f"sbatch returned but its output cannot be recorded: {error}") from error
    if submitted.returncode:
        ledger["state"] = "submission-failed"
        ledger["submissionExitCode"] = submitted.returncode
        write_json(ledger_path, ledger)
        if submitted.stderr:
            print(submitted.stderr, file=sys.stderr, end="")
        return submitted.returncode

    match = re.fullmatch(r"([0-9]+)(?:;[^\s;]+)?\s*", submitted.stdout)
    if not match:
        ledger["state"] = "submission-unknown"
        ledger["submissionOutput"] = submitted.stdout
        write_json(ledger_path, ledger)
        raise ConfigurationError(
            f"sbatch succeeded without a parseable job ID; inspect {run_dir}")
    ledger["state"] = "submitted"
    ledger["jobId"] = match.group(1)
    ledger["submittedAt"] = utc_now()
    write_json(ledger_path, ledger)
    print(ledger["jobId"])
    return 0


def command_collect(args):
    from .core import COMPATIBILITY, ConfigurationError, DEFAULT_RESULT_DIR, SLURM_TERMINAL_STATES
    from .evidence import file_manifest
    from .scheduler import copy_remote_path, remote_path_exists, run_scheduler, scheduler_remote
    from .slurm import query_slurm_status
    from .state import copy_file, copy_tree, load_ledger, owned_state_file, path_is_within, read_json, state_path, utc_now, write_json

    run_dir = state_path(args)
    unused, ledger = load_ledger(args)
    job_id = ledger.get("jobId")
    if not isinstance(job_id, str) or not job_id.isdecimal():
        raise ConfigurationError("run ledger has no recorded numeric Slurm job ID")
    config_path = owned_state_file(
        run_dir, ledger.get("resolvedConfig", "resolved-simulation.json"),
        "resolved configuration")
    config = read_json(config_path, "resolved configuration")
    if config.get("runId") != args.run_id:
        raise ConfigurationError("resolved configuration ownership mismatch")

    status = query_slurm_status(args, job_id)
    scheduler = status["record"]
    terminal = scheduler["state"] in SLURM_TERMINAL_STATES
    if not terminal and not args.allow_incomplete:
        raise ConfigurationError(
            f"Slurm job {job_id} is {scheduler['state']}; "
            "use --allow-incomplete only for a diagnostic snapshot")

    results_root = pathlib.Path(config["storage"]["resultsPath"]).resolve()
    workload_results = results_root / args.run_id
    remote = scheduler_remote(args)
    output = (pathlib.Path(args.output_dir).expanduser().resolve()
              if args.output_dir else
              (DEFAULT_RESULT_DIR / args.run_id).resolve())
    if output.exists():
        raise ConfigurationError(f"result bundle already exists: {output}")
    if path_is_within(output, workload_results):
        raise ConfigurationError("result bundle cannot be inside workload results")
    if path_is_within(output, run_dir):
        raise ConfigurationError("result bundle cannot be inside run state")
    try:
        output.parent.mkdir(parents=True, exist_ok=True)
        temporary = pathlib.Path(tempfile.mkdtemp(
            prefix=f".{args.run_id}.", dir=output.parent))
    except OSError as error:
        raise ConfigurationError(f"cannot create result bundle: {error}") from error

    missing = []
    workload_status = None
    staged_images = None
    staged_caches = None
    network_emulation = None
    try:
        for directory in ("logs", "metrics", "scheduler", "state", "traces"):
            (temporary / directory).mkdir()
        copy_file(run_dir / "ledger.json", temporary / "state" / "ledger.json")
        batch_path = owned_state_file(
            run_dir, ledger.get("batchFile", "workload.sbatch"),
            "batch file")
        copy_file(batch_path, temporary / "scheduler" / "workload.sbatch")
        copy_file(config_path, temporary / "resolved-values.json")
        copy_file(COMPATIBILITY, temporary / "compatibility-lock.json")
        staged_path = run_dir / "infrastructure" / "images.json"
        if staged_path.is_file() and not staged_path.is_symlink():
            staged_images = read_json(staged_path, "staged image ledger")
            if (staged_images.get("schemaVersion") != 1 or
                    staged_images.get("runId") != args.run_id or
                    not isinstance(staged_images.get("images"), dict)):
                raise ConfigurationError(
                    f"staged image ledger ownership mismatch: {staged_path}")
            copy_file(staged_path, temporary / "staged-images.json")
        cache_path = run_dir / "infrastructure" / "caches.json"
        if cache_path.is_file() and not cache_path.is_symlink():
            staged_caches = read_json(cache_path, "staged cache ledger")
            if (staged_caches.get("schemaVersion") != 1 or
                    staged_caches.get("runId") != args.run_id or
                    not isinstance(staged_caches.get("caches"), dict)):
                raise ConfigurationError(
                    f"staged cache ledger ownership mismatch: {cache_path}")
            copy_file(cache_path, temporary / "staged-caches.json")
        network_path = run_dir / "infrastructure" / "network.json"
        if network_path.is_file() and not network_path.is_symlink():
            network_emulation = read_json(
                network_path, "network-emulation evidence")
            if (network_emulation.get("schemaVersion") != 1 or
                    network_emulation.get("runId") != args.run_id or
                    network_emulation.get("interface") !=
                    config["network"]["dataInterface"] or
                    network_emulation.get("shaping") !=
                    config["network"]["shaping"] or
                    not isinstance(network_emulation.get("endpoints"), dict)):
                raise ConfigurationError(
                    f"network-emulation evidence mismatch: {network_path}")
            copy_file(network_path, temporary / "network-emulation.json")
        for name in ("render.stderr", "sbatch.stdout", "sbatch.stderr"):
            source = run_dir / name
            if source.is_file():
                copy_file(source, temporary / "scheduler" / name)
            else:
                missing.append(f"run-state/{name}")

        inputs = ledger.get("inputs", {})
        if not isinstance(inputs, dict):
            raise ConfigurationError("invalid inputs in run ledger")
        for name, destination in (
                (inputs.get("profile"), "profile.yaml"),
                (inputs.get("request"), "request.yaml")):
            if name:
                source = owned_state_file(run_dir, name, destination)
                copy_file(source, temporary / destination)
        overlays = inputs.get("overlays", [])
        if not isinstance(overlays, list):
            raise ConfigurationError("invalid overlays in run ledger")
        for index, item in enumerate(overlays):
            if not isinstance(item, dict) or not isinstance(item.get("name"), str):
                raise ConfigurationError("invalid overlay in run ledger")
            source = owned_state_file(
                run_dir, item.get("path"), "overlay input")
            copy_file(source,
                      temporary / "overlays" / f"{index:02d}.yaml")
        write_json(temporary / "request-provenance.json", inputs)

        collected_workload = workload_results
        if remote:
            if remote_path_exists(args, str(workload_results), "directory"):
                checked = run_scheduler(
                    args, "", "find", [str(workload_results), "-type", "l",
                                        "-print", "-quit"],
                    text=True, capture_output=True)
                if checked.returncode or checked.stdout.strip():
                    raise ConfigurationError(
                        "remote workload results contain a symbolic link")
                staged_workload = temporary / ".remote-workload"
                copy_remote_path(
                    args, str(workload_results), staged_workload,
                    recursive=True)
                copy_tree(staged_workload, temporary / "workload")
                shutil.rmtree(staged_workload)
                collected_workload = temporary / "workload"
            else:
                collected_workload = None
        elif workload_results.is_symlink():
            raise ConfigurationError(
                f"workload result directory is a symbolic link: {workload_results}")

        if collected_workload and collected_workload.is_dir():
            status_path = collected_workload / "status.json"
            if status_path.is_file() and not status_path.is_symlink():
                workload_status = read_json(status_path, "workload status")
                if (workload_status.get("runId") != args.run_id or
                        workload_status.get("scheduler") != "slurm" or
                        str(workload_status.get("jobId")) != job_id or
                        not isinstance(workload_status.get("exitCode"), int)):
                    raise ConfigurationError(
                        f"workload status ownership mismatch: {status_path}")
            if not remote:
                copy_tree(workload_results, temporary / "workload")
        else:
            missing.append(f"workload/{args.run_id}")

        for extension in ("out", "err"):
            source = results_root / f"{args.run_id}-{job_id}.{extension}"
            if remote and remote_path_exists(args, str(source), "file"):
                copy_remote_path(
                    args, str(source),
                    temporary / "logs" / f"batch.{extension}")
            elif not remote and source.is_file() and not source.is_symlink():
                copy_file(source, temporary / "logs" / f"batch.{extension}")
            else:
                missing.append(f"logs/batch.{extension}")
        (temporary / "scheduler" / "status.psv").write_text(
            status["normalized"], encoding="utf-8")
        write_json(temporary / "scheduler" / "status-source.json", {
            "schemaVersion": 1,
            "source": status["source"],
        })
        (temporary / "scheduler" / "sacct.stderr").write_text(
            status["sacct"].stderr, encoding="utf-8")
        if status["source"] == "sacct":
            (temporary / "scheduler" / "sacct.psv").write_text(
                status["sacct"].stdout, encoding="utf-8")
        else:
            (temporary / "scheduler" / "scontrol.txt").write_text(
                status["scontrol"].stdout, encoding="utf-8")
            (temporary / "scheduler" / "scontrol.stderr").write_text(
                status["scontrol"].stderr, encoding="utf-8")
        (temporary / "faults.jsonl").write_text("", encoding="utf-8")

        nodes = []
        nodes_path = ((temporary / "workload" / "nodes.txt") if remote else
                      workload_results / "nodes.txt")
        if nodes_path.is_file() and not nodes_path.is_symlink():
            nodes = [line for line in nodes_path.read_text(
                encoding="utf-8").splitlines() if line]
        parallel = config["workload"]["parallelism"]
        write_json(temporary / "topology.json", {
            "schemaVersion": 1,
            "requestedNodes": parallel["nodes"],
            "processesPerNode": parallel["processesPerNode"],
            "requestedRanks": parallel["nodes"] * parallel["processesPerNode"],
            "allocatedNodes": nodes,
            "schedulerNodeList": scheduler["nodeList"],
        })
        write_json(temporary / "images.json", {
            "schemaVersion": 1,
            "workload": config["workload"]["image"],
            "uibRuntime": config["uib"].get("bundle"),
            "stagedImages": (
                "staged-images.json" if staged_images is not None else None),
            "stagedCaches": (
                "staged-caches.json" if staged_caches is not None else None),
        })
        write_json(temporary / "versions.json", {
            "schemaVersion": 1,
            "clusterKind": config["cluster"]["manager"],
            "configuredClusterVersion": config["cluster"]["version"],
            "compatibilityLock": "compatibility-lock.json",
        })
        write_json(temporary / "preflight.json", {
            "schemaVersion": 1,
            "uibEnabled": config["uib"]["enabled"],
            "runtimePreflight": (
                "invoked-by-batch-script" if config["uib"]["enabled"]
                else "not-applicable"),
            "verifiedByCollector": False,
            "evidence": "logs/batch.out and logs/batch.err",
        })
        summary = {
            "schemaVersion": 1,
            "runId": args.run_id,
            "collectedAt": utc_now(),
            "completeSchedulerSnapshot": terminal,
            "scheduler": "slurm",
            "schedulerEvidenceSource": status["source"],
            "jobId": job_id,
            "schedulerState": scheduler["state"],
            "schedulerExitCode": scheduler["exitCode"],
            "workloadExitCode": (workload_status.get("exitCode")
                                 if workload_status else None),
            "workloadStatusRecorded": workload_status is not None,
            "uibEnabled": config["uib"]["enabled"],
            "protocol": config["uib"]["protocol"] if config["uib"]["enabled"] else None,
            "congestionControl": (config["uib"]["congestionControl"]
                                  if config["uib"]["enabled"] else None),
            "memoryMode": (config["uib"]["memoryMode"]
                           if config["uib"]["enabled"] else None),
            "communicationFrontend": config["communication"]["frontend"],
            "validationLevel": "not-assigned",
            "uibTrafficEvidence": "not-evaluated",
            "networkEmulation": (
                "network-emulation.json"
                if network_emulation is not None else None),
            "promotionEligible": False,
            "missingArtifacts": missing,
        }
        write_json(temporary / "summary.json", summary)
        summary_text = (
            f"# UIB simulation result: {args.run_id}\n\n"
            f"Slurm job `{job_id}` was `{scheduler['state']}` with scheduler "
            f"exit code `{scheduler['exitCode']}`. The bundle is "
            f"{'a terminal' if terminal else 'an incomplete diagnostic'} snapshot.\n\n"
            "Collection alone does not prove that application traffic used UIB. "
            "Inspect the preflight, transport logs, correctness output, and "
            "network evidence before assigning a validation level or promoting "
            "this result.\n"
        )
        (temporary / "summary.md").write_text(summary_text, encoding="utf-8")
        write_json(temporary / "manifest.json", file_manifest(temporary))
        temporary.rename(output)
    except (ConfigurationError, OSError) as error:
        shutil.rmtree(temporary, ignore_errors=True)
        if isinstance(error, ConfigurationError):
            raise
        raise ConfigurationError(f"cannot collect result bundle: {error}") from error
    print(output)
    return 0


def command_down(args):
    from .config import resolve
    from .slurm import command_slurm_down
    from .state import load_ledger, state_path

    config = resolve(args)
    if config["infrastructure"]["provider"] == "libvirt":
        run_dir = state_path(args)
        if (run_dir / "ledger.json").is_file():
            unused, ledger = load_ledger(args)
            if ledger.get("jobId"):
                result = command_slurm_down(args)
                if result:
                    return result
            if not (run_dir / "infrastructure" / "ledger.json").is_file():
                return 0
        return invoke_libvirt(config, lambda unused: [
            "down", "--name", args.run_id,
            "--state-dir", str(run_dir / "infrastructure"),
        ])
    if config["cluster"]["manager"] == "slurm":
        return command_slurm_down(args)
    return invoke_backend(
        config, lambda unused: ["down", "--name", args.run_id])
