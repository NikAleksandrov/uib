# SPDX-License-Identifier: Apache-2.0

import re


def record_network_evidence(state, config, values, run_id):
    from .core import LibvirtError, read_json, write_json

    root = state / "network"
    expected = {
        name for name, node in values["nodes"].items()
        if node["role"] == "worker"
    }
    try:
        entries = list(root.iterdir())
    except OSError as error:
        raise LibvirtError(f"cannot inspect network evidence: {error}") from error
    if (any(entry.is_symlink() or not entry.is_file() or
            entry.suffix != ".json" for entry in entries) or
            {entry.stem for entry in entries} != expected):
        raise LibvirtError("network evidence does not match the worker set")

    interface = config["network"]["dataInterface"]
    shaping = config["network"]["shaping"]
    endpoints = {}
    for node in sorted(expected):
        evidence = read_json(root / f"{node}.json", "network evidence")
        if (evidence.get("schemaVersion") != 1 or
                evidence.get("node") != node or
                evidence.get("interface") != interface or
                evidence.get("shaping") != shaping):
            raise LibvirtError(f"network evidence mismatch for {node}")
        fields = ("appliedCommand", "qdisc", "iproute2", "kernel")
        if any(not isinstance(evidence.get(field), str) or
               not evidence[field].strip() for field in fields):
            raise LibvirtError(f"incomplete network evidence for {node}")
        active = "qdisc netem " in evidence["qdisc"]
        if active != shaping["enabled"]:
            raise LibvirtError(f"unexpected qdisc state for {node}")
        if active and not re.search(
                r"Sent [1-9][0-9]* bytes", evidence["qdisc"]):
            raise LibvirtError(f"network evidence has no traffic for {node}")
        endpoints[node] = {field: evidence[field] for field in fields}
        seed = shaping.get("seed")
        endpoints[node]["seedApplied"] = (
            None if seed is None else
            f" seed {seed}" in evidence["appliedCommand"])

    path = state / "network.json"
    write_json(path, {
        "schemaVersion": 1,
        "runId": run_id,
        "scope": "worker-data-egress",
        "interface": interface,
        "replaceExisting": True,
        "shaping": shaping,
        "endpoints": endpoints,
    })
    return path
