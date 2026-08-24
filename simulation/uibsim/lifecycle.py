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


def command_up(args):
    from .config import resolve

    config = resolve(args)

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

    config = resolve(args)
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

    config = resolve(args)

    def arguments(unused):
        command = ["load-image", "--name", args.run_id]
        for image in args.image:
            command.extend(["--image", image])
        if args.allow_mutable_tags:
            command.append("--allow-mutable-tags")
        return command

    return invoke_backend(config, arguments)


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
    from .scheduler import slurm_command
    from .state import state_path, utc_now, write_json

    config = resolve(args)
    if config["cluster"]["manager"] != "slurm":
        raise ConfigurationError("run is currently implemented only for Slurm")
    config["runId"] = args.run_id
    run_dir = state_path(args)
    try:
        run_dir.parent.mkdir(parents=True, exist_ok=True)
        run_dir.mkdir()
    except FileExistsError as error:
        raise ConfigurationError(f"run state already exists: {run_dir}") from error
    except OSError as error:
        raise ConfigurationError(f"cannot create run state {run_dir}: {error}") from error

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

    required_paths = [
        config["storage"]["modelCachePath"],
        config["storage"]["resultsPath"],
    ]
    if config["uib"]["enabled"]:
        required_paths.append(config["storage"]["uibRuntimePath"])
    missing = [path for path in required_paths if not pathlib.Path(path).is_dir()]
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
    sbatch = slurm_command("SBATCH_COMMAND", "sbatch")
    try:
        submitted = subprocess.run(
            [sbatch, "--parsable", str(batch_path)], text=True,
            capture_output=True, check=False)
    except OSError as error:
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


def command_down(args):
    from .config import resolve
    from .slurm import command_slurm_down

    config = resolve(args)
    if config["cluster"]["manager"] == "slurm":
        return command_slurm_down(args)
    return invoke_backend(
        config, lambda unused: ["down", "--name", args.run_id])
