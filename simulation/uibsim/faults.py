# SPDX-License-Identifier: Apache-2.0

import os
import pathlib
import subprocess


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
    from .core import ConfigurationError

    command = [*kubectl_command(args, config), *arguments]
    try:
        return subprocess.run(command, check=False, **kwargs)
    except OSError as error:
        raise ConfigurationError(f"cannot invoke kubectl: {error}") from error
