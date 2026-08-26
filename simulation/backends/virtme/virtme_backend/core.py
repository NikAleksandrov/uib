# SPDX-License-Identifier: Apache-2.0

import hashlib
import json
import pathlib
import re


NAME = re.compile(r"^[a-z](?:[a-z0-9-]{1,46}[a-z0-9])$")


class VirtmeError(Exception):
    pass


def load_config(path):
    try:
        with pathlib.Path(path).open(encoding="utf-8") as source:
            config = json.load(source)
    except (OSError, json.JSONDecodeError) as error:
        raise VirtmeError(f"cannot load {path}: {error}") from error
    if config.get("infrastructure", {}).get("provider") != "virtme":
        raise VirtmeError("configuration does not select the virtme provider")
    if config.get("nodes", {}).get("controlPlane") != 1:
        raise VirtmeError("virtme requires exactly one control-plane guest")
    if "virtme" not in config:
        raise VirtmeError("configuration has no virtme settings")
    if config["virtme"]["mode"] != "multi-guest":
        raise VirtmeError("the topology renderer requires multi-guest mode")
    return config


def validate_name(name):
    if not NAME.fullmatch(name):
        raise VirtmeError(
            "run ID must contain 3-48 lowercase letters, digits, or hyphens")


def quantity_mib(value):
    match = re.fullmatch(r"([1-9][0-9]*)(Mi|Gi|Ti)", value)
    if not match:
        raise VirtmeError(f"invalid node memory: {value}")
    factor = {"Mi": 1, "Gi": 1024, "Ti": 1024 ** 2}[match.group(2)]
    return int(match.group(1)) * factor


def mac_address(run_id, node):
    digest = hashlib.sha256(f"{run_id}:{node}:data".encode()).digest()
    return "52:54:00:" + ":".join(f"{byte:02x}" for byte in digest[:3])


def bridge_name(config, run_id):
    suffix = hashlib.sha256(run_id.encode()).hexdigest()[:8]
    return f"{config['virtme']['bridgePrefix']}-{suffix}"
