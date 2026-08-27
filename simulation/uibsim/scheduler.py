# SPDX-License-Identifier: Apache-2.0

import os
import pathlib
import re
import shlex
import shutil
import subprocess


def slurm_command(variable, default):
    return os.environ.get(variable, default)


def scheduler_remote(args):
    from .config import load
    from .core import ConfigurationError
    from .state import read_json, state_path

    infrastructure = state_path(args) / "infrastructure"
    ledger_path = infrastructure / "ledger.json"
    if not ledger_path.is_file():
        return None
    ledger = read_json(ledger_path, "infrastructure ledger")
    provider = ledger.get("provider")
    if (ledger.get("schemaVersion") != 1 or
            ledger.get("runId") != args.run_id or
            provider not in {"libvirt", "virtme"}):
        raise ConfigurationError(
            f"infrastructure ledger ownership mismatch: {ledger_path}")
    if ledger.get("state") != "ready":
        raise ConfigurationError(
            f"{provider} infrastructure is not ready: {ledger.get('state')}")

    inventory_name = "inventory.json" if provider == "virtme" else "inventory.yaml"
    inventory_path = infrastructure / inventory_name
    inventory = load(inventory_path)
    try:
        all_hosts = inventory["all"]
        variables = all_hosts["vars"]
        children = all_hosts["children"]
        controllers = children["slurm_controller"]["hosts"]
        cluster_hosts = children["slurm_cluster"]["hosts"]
        if len(controllers) != 1:
            raise KeyError
        controller = next(iter(controllers))
        endpoint = cluster_hosts[controller]
        user = variables["ansible_user"]
        host = endpoint["ansible_host"]
        port = endpoint["ansible_port"]
        key = pathlib.Path(
            variables["ansible_ssh_private_key_file"]).resolve()
    except (KeyError, TypeError, StopIteration) as error:
        raise ConfigurationError(
            f"invalid {provider} controller inventory: {inventory_path}") from error
    expected_key = (infrastructure / "id_ed25519").resolve()
    if key != expected_key or not key.is_file():
        raise ConfigurationError(
            f"{provider} controller key ownership mismatch: {key}")
    if (not isinstance(user, str) or
            not re.fullmatch(r"[a-z_][a-z0-9_-]*", user) or
            not isinstance(host, str) or not host or
            isinstance(port, bool) or not isinstance(port, int) or
            port < 1 or port > 65535):
        raise ConfigurationError(
            f"invalid {provider} controller endpoint: {inventory_path}")
    environment = []
    if provider == "virtme":
        runtime_value = ledger.get("slurmRuntime")
        if not isinstance(runtime_value, str):
            raise ConfigurationError(
                "virtme infrastructure has no Slurm runtime")
        runtime = pathlib.Path(runtime_value).resolve()
        if not (runtime / "usr/bin/sbatch").is_file():
            raise ConfigurationError(
                f"virtme Slurm runtime is unavailable: {runtime}")
        environment = [
            "env",
            (f"PATH={runtime}/usr/bin:{runtime}/usr/sbin:"
             "/usr/bin:/usr/sbin"),
            (f"LD_LIBRARY_PATH={runtime}/usr/lib/x86_64-linux-gnu/slurm-wlm:"
             f"{runtime}/usr/lib/x86_64-linux-gnu"),
            "SLURM_CONF=/run/uib-slurm/slurm.conf",
            f"ENROOT_LIBRARY_PATH={runtime}/usr/lib/enroot",
            "ENROOT_SYSCONF_PATH=/run/uib-enroot/etc",
            "ENROOT_RUNTIME_PATH=/run/uib-enroot/runtime",
            "ENROOT_CACHE_PATH=/run/uib-enroot/cache",
            "ENROOT_DATA_PATH=/var/lib/uib/enroot/data",
            "ENROOT_CONFIG_PATH=/run/uib-enroot/config",
            "ENROOT_ALLOW_SUPERUSER=yes",
            "ENROOT_LOGIN_SHELL=no",
            "XDG_CACHE_HOME=/run/uib-enroot/cache-home",
            "XDG_CONFIG_HOME=/run/uib-enroot/config-home",
            "XDG_DATA_HOME=/var/lib/uib/enroot/data-home",
            "XDG_RUNTIME_DIR=/run/uib-enroot/runtime-home",
        ]
    return {
        "environment": environment,
        "host": host,
        "key": key,
        "port": port,
        "provider": provider,
        "target": f"{user}@{host}",
    }


def command_executable(variable, default):
    from .core import ConfigurationError

    command = os.environ.get(variable, default)
    if os.path.sep in command:
        path = pathlib.Path(command)
        if path.is_file() and os.access(path, os.X_OK):
            return command
    else:
        resolved = shutil.which(command)
        if resolved:
            return resolved
    raise ConfigurationError(f"command not found: {command}")


def ssh_command(remote):
    return [
        command_executable("SSH_COMMAND", "ssh"),
        "-o", "BatchMode=yes",
        "-o", "IdentitiesOnly=yes",
        "-o", "StrictHostKeyChecking=no",
        "-o", "UserKnownHostsFile=/dev/null",
        "-o", "LogLevel=ERROR",
        "-o", "ConnectTimeout=10",
        "-i", str(remote["key"]),
        "-p", str(remote["port"]),
        remote["target"],
    ]


def run_scheduler(args, variable, program, arguments, **kwargs):
    from .core import ConfigurationError

    remote = scheduler_remote(args)
    if remote:
        command = ssh_command(remote)
        command.append(shlex.join([
            *remote["environment"], program, *arguments]))
    else:
        command = [slurm_command(variable, program), *arguments]
    try:
        return subprocess.run(command, check=False, **kwargs)
    except OSError as error:
        raise ConfigurationError(f"cannot invoke {program}: {error}") from error


def remote_path_exists(args, path, kind):
    remote = scheduler_remote(args)
    if not remote:
        candidate = pathlib.Path(path)
        return candidate.is_dir() if kind == "directory" else candidate.is_file()
    if kind == "file":
        result = run_scheduler(
            args, "", "find", [path, "-maxdepth", "0", "-type", "f",
                               "-print", "-quit"],
            text=True, capture_output=True)
        return result.returncode == 0 and result.stdout.strip() == path
    option = "-d" if kind == "directory" else "-f"
    result = run_scheduler(
        args, "", "test", [option, path],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    return result.returncode == 0


def copy_remote_path(args, source, destination, recursive=False):
    from .core import ConfigurationError

    remote = scheduler_remote(args)
    if not remote:
        raise ConfigurationError("remote copy requires owned infrastructure")
    command = [
        command_executable("SCP_COMMAND", "scp"),
        "-q",
        "-o", "BatchMode=yes",
        "-o", "IdentitiesOnly=yes",
        "-o", "StrictHostKeyChecking=no",
        "-o", "UserKnownHostsFile=/dev/null",
        "-o", "LogLevel=ERROR",
        "-o", "ConnectTimeout=10",
        "-i", str(remote["key"]),
        "-P", str(remote["port"]),
    ]
    if recursive:
        command.append("-r")
    command.extend([f"{remote['target']}:{source}", str(destination)])
    try:
        result = subprocess.run(command, check=False)
    except OSError as error:
        raise ConfigurationError(f"cannot invoke scp: {error}") from error
    if result.returncode:
        raise ConfigurationError(
            f"cannot copy controller result path: {source}")
