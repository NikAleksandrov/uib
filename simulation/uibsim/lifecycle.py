# SPDX-License-Identifier: Apache-2.0

import json
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

    config = resolve(args)
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


def command_down(args):
    from .config import resolve

    config = resolve(args)
    return invoke_backend(
        config, lambda unused: ["down", "--name", args.run_id])
