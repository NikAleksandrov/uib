# SPDX-License-Identifier: Apache-2.0

import hashlib
import json
import os
import pathlib
import re
import shutil
import subprocess
import tempfile


BACKEND = pathlib.Path(__file__).resolve().parents[1]


SIMULATION = BACKEND.parents[1]


MODULE = BACKEND / "opentofu"


PLAYBOOK = BACKEND / "ansible" / "site-slurm.yaml"


DEFAULT_CACHE = SIMULATION / ".artifacts" / "downloads" / "libvirt"


NAME = re.compile(r"^[a-z](?:[a-z0-9-]{1,46}[a-z0-9])$")


SHA256 = re.compile(r"^[0-9a-f]{64}$")


class LibvirtError(Exception):
    pass


def load_config(path):
    try:
        with pathlib.Path(path).open(encoding="utf-8") as source:
            config = json.load(source)
    except (OSError, json.JSONDecodeError) as error:
        raise LibvirtError(f"cannot load {path}: {error}") from error
    if config.get("infrastructure", {}).get("provider") != "libvirt":
        raise LibvirtError("configuration does not select the libvirt provider")
    if config.get("nodes", {}).get("controlPlane") != 1:
        raise LibvirtError("libvirt requires exactly one control-plane node")
    if "libvirt" not in config:
        raise LibvirtError("configuration has no libvirt settings")
    if (config["libvirt"]["networkMode"] == "rootless" and
            config["libvirt"]["uri"] != "qemu:///session"):
        raise LibvirtError("rootless mode requires qemu:///session")
    if (config.get("cluster", {}).get("manager") == "slurm" and
            config["cluster"].get("version") != "23.11.4"):
        raise LibvirtError("Slurm provisioning requires version 23.11.4")
    return config


def validate_name(name):
    if not NAME.fullmatch(name):
        raise LibvirtError(
            "run ID must contain 3-48 lowercase letters, digits, or hyphens")


def quantity(value, unit):
    match = re.fullmatch(r"([1-9][0-9]*)(Mi|Gi|Ti)", value)
    if not match:
        raise LibvirtError(f"invalid node quantity: {value}")
    factors = {
        "bytes": {"Mi": 1024 ** 2, "Gi": 1024 ** 3, "Ti": 1024 ** 4},
        "mib": {"Mi": 1, "Gi": 1024, "Ti": 1024 ** 2},
    }
    return int(match.group(1)) * factors[unit][match.group(2)]


def mac_address(run_id, name, network):
    digest = hashlib.sha256(
        f"{run_id}:{name}:{network}".encode()).digest()
    return "52:54:00:" + ":".join(f"{byte:02x}" for byte in digest[:3])


def multicast_endpoint(run_id):
    digest = hashlib.sha256(f"{run_id}:data".encode()).digest()
    address = f"239.255.{digest[0]}.{digest[1]}"
    port = 30000 + int.from_bytes(digest[2:4], "big") % 20000
    return address, port


def write_json(path, value):
    temporary = None
    try:
        path.parent.mkdir(parents=True, exist_ok=True)
        with tempfile.NamedTemporaryFile(
                mode="w", encoding="utf-8", dir=path.parent,
                prefix=f".{path.name}.", delete=False) as output:
            temporary = pathlib.Path(output.name)
            json.dump(value, output, indent=2, sort_keys=True)
            output.write("\n")
        temporary.replace(path)
    except OSError as error:
        if temporary:
            temporary.unlink(missing_ok=True)
        raise LibvirtError(f"cannot write {path}: {error}") from error


def read_json(path, description):
    try:
        with path.open(encoding="utf-8") as source:
            value = json.load(source)
    except (OSError, json.JSONDecodeError) as error:
        raise LibvirtError(f"cannot load {description} {path}: {error}") from error
    if not isinstance(value, dict):
        raise LibvirtError(f"{description} {path} must contain an object")
    return value


def executable(variable, default):
    command = os.environ.get(variable, default)
    if os.path.sep in command:
        resolved = command if os.access(command, os.X_OK) else None
    else:
        resolved = shutil.which(command)
    if not resolved:
        raise LibvirtError(f"command not found: {command}")
    return resolved


def run_logged(command, log, environment=None, capture=False):
    try:
        with log.open("a", encoding="utf-8") as output:
            output.write("$ " + " ".join(command) + "\n")
            output.flush()
            result = subprocess.run(
                command, text=True, stdout=subprocess.PIPE if capture else output,
                stderr=output, env=environment, check=False)
            if capture and result.stdout:
                output.write(result.stdout)
    except OSError as error:
        raise LibvirtError(f"cannot execute {command[0]}: {error}") from error
    if result.returncode:
        raise LibvirtError(
            f"{pathlib.Path(command[0]).name} failed with status "
            f"{result.returncode}; inspect {log}")
    return result.stdout if capture else ""


def load_ledger(state, run_id):
    path = state / "ledger.json"
    try:
        with path.open(encoding="utf-8") as source:
            ledger = json.load(source)
    except (OSError, json.JSONDecodeError) as error:
        raise LibvirtError(f"cannot load infrastructure ledger {path}: {error}") from error
    if (ledger.get("schemaVersion") != 1 or ledger.get("runId") != run_id or
            ledger.get("provider") != "libvirt"):
        raise LibvirtError(f"infrastructure ledger ownership mismatch: {path}")
    return path, ledger
