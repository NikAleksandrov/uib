# SPDX-License-Identifier: Apache-2.0

import json
import pathlib
import shutil
import subprocess
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
    from .core import ConfigurationError, KIND_BACKEND, VIRTME_BACKEND, run_backend

    provider = config["infrastructure"]["provider"]
    backends = {
        "kind": KIND_BACKEND,
        "virtme": VIRTME_BACKEND,
    }
    if provider not in backends:
        raise ConfigurationError(
            f"lifecycle is not implemented for provider: {provider}")
    with tempfile.NamedTemporaryFile(mode="w", encoding="utf-8",
                                     prefix="uib-sim-", suffix=".json") as output:
        json.dump(config, output, indent=2, sort_keys=True)
        output.flush()
        return run_backend(
            [str(backends[provider]), *arguments(output.name)])


def invoke_libvirt(config, arguments):
    from .core import LIBVIRT_BACKEND, run_backend

    with tempfile.NamedTemporaryFile(mode="w", encoding="utf-8",
                                     prefix="uib-sim-", suffix=".json") as output:
        json.dump(config, output, indent=2, sort_keys=True)
        output.flush()
        return run_backend([str(LIBVIRT_BACKEND), *arguments(output.name)])


def command_up(args):
    from .config import resolve
    from .state import state_path

    config = resolve(args)

    if config["infrastructure"]["provider"] == "libvirt":
        def libvirt_arguments(path):
            command = [
                "up", path, "--name", args.run_id,
                "--state-dir", str(state_path(args) / "infrastructure"),
            ]
            if args.retain:
                command.append("--retain")
            return command

        return invoke_libvirt(config, libvirt_arguments)

    if config["infrastructure"]["provider"] == "virtme":
        def virtme_arguments(path):
            command = [
                "up", path, "--name", args.run_id,
                "--state-dir", str(state_path(args) / "infrastructure"),
            ]
            if args.retain:
                command.append("--retain")
            return command

        return invoke_backend(config, virtme_arguments)

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
    from .kubernetes import command_kubernetes_status
    from .slurm import command_slurm_status
    from .state import state_path

    config = resolve(args)
    provider = config["infrastructure"]["provider"]
    if (provider in {"libvirt", "virtme"} and
            not (state_path(args) / "ledger.json").is_file()):
        def infrastructure_arguments(unused):
            return [
                "status", "--name", args.run_id,
                "--state-dir", str(state_path(args) / "infrastructure"),
            ]
        if provider == "libvirt":
            return invoke_libvirt(config, infrastructure_arguments)
        return invoke_backend(config, infrastructure_arguments)
    if config["cluster"]["manager"] == "slurm":
        return command_slurm_status(args)
    if (state_path(args) / "ledger.json").is_file():
        return command_kubernetes_status(args)
    return invoke_backend(
        config, lambda unused: ["status", "--name", args.run_id])


def command_kubeconfig(args):
    from .config import resolve
    from .state import state_path

    config = resolve(args)
    if config["infrastructure"]["provider"] == "libvirt":
        return invoke_libvirt(config, lambda unused: [
            "kubeconfig", "--name", args.run_id,
            "--state-dir", str(state_path(args) / "infrastructure"),
        ])
    if config["infrastructure"]["provider"] == "virtme":
        return invoke_backend(config, lambda unused: [
            "kubeconfig", "--name", args.run_id,
            "--state-dir", str(state_path(args) / "infrastructure"),
        ])
    return invoke_backend(
        config, lambda unused: ["kubeconfig", "--name", args.run_id])


def command_load_image(args):
    from .config import resolve
    from .core import ConfigurationError
    from .images import stage_slurm_image

    config = resolve(args)

    if config["infrastructure"]["provider"] in {"libvirt", "virtme"}:
        if config["cluster"]["manager"] != "slurm":
            raise ConfigurationError(
                "Kubernetes image loading is not implemented for this "
                "provider; "
                "use a registry or an immutable image already present on "
                "nodes")
        for reference in args.image:
            print(stage_slurm_image(args, reference,
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
    provider = config["infrastructure"]["provider"]
    if (provider not in {"libvirt", "virtme"} or
            config["cluster"]["manager"] != "slurm"):
        raise ConfigurationError(
            "load-cache is currently implemented only for libvirt and "
            "virtme Slurm")
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
    from .kubernetes import command_kubernetes_run
    from .slurm import command_slurm_run

    config = resolve(args)
    if config["cluster"]["manager"] == "kubernetes":
        return command_kubernetes_run(args)
    return command_slurm_run(args)


def command_collect(args):
    from .kubernetes import command_kubernetes_collect
    from .slurm import command_slurm_collect
    from .state import load_ledger

    unused, ledger = load_ledger(args)
    if ledger["scheduler"] == "kubernetes":
        return command_kubernetes_collect(args)
    return command_slurm_collect(args)


def command_down(args):
    from .config import resolve
    from .kubernetes import command_kubernetes_down
    from .slurm import command_slurm_down
    from .state import load_ledger, state_path

    config = resolve(args)
    run_dir = state_path(args)
    if (config["cluster"]["manager"] == "kubernetes" and
            (run_dir / "ledger.json").is_file()):
        result = command_kubernetes_down(args)
        if (result or not args.infrastructure or
                config["infrastructure"]["provider"] == "existing"):
            return result
    if config["infrastructure"]["provider"] == "libvirt":
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
    if config["infrastructure"]["provider"] == "virtme":
        if (run_dir / "ledger.json").is_file():
            unused, ledger = load_ledger(args)
            if ledger.get("jobId"):
                result = command_slurm_down(args)
                if result:
                    return result
            if not (run_dir / "infrastructure" / "ledger.json").is_file():
                return 0
        return invoke_backend(config, lambda unused: [
            "down", "--name", args.run_id,
            "--state-dir", str(run_dir / "infrastructure"),
        ])
    if config["cluster"]["manager"] == "slurm":
        return command_slurm_down(args)
    return invoke_backend(
        config, lambda unused: ["down", "--name", args.run_id])
