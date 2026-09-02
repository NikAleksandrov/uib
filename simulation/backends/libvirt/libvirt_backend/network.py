# SPDX-License-Identifier: Apache-2.0

import hashlib
import os
import re
import shlex
import subprocess


def record_network_evidence(state, config, values, run_id):
    from .core import LibvirtError, read_json, verbs_active_mtu, write_json

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
    default_shaping = config["network"]["shaping"]
    endpoint_shaping = config["network"].get("endpointShaping", {})
    endpoints = {}
    for index, node in enumerate(sorted(expected), 1):
        target = f"worker-{index:02d}/data"
        shaping = endpoint_shaping.get(target, default_shaping)
        evidence = read_json(root / f"{node}.json", "network evidence")
        if (evidence.get("schemaVersion") != 1 or
                evidence.get("node") != node or
                evidence.get("target") != target or
                evidence.get("interface") != interface or
                evidence.get("shaping") != shaping or
                evidence.get("mtu") != config["network"]["mtu"]):
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
        endpoints[node]["target"] = target
        endpoints[node]["shaping"] = shaping
        endpoints[node]["mtu"] = evidence["mtu"]
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
        "mtu": config["network"]["mtu"],
        "activeMtu": verbs_active_mtu(config["network"]["mtu"]),
        "replaceExisting": True,
        "shaping": default_shaping,
        "endpoints": endpoints,
    })
    return path


def run_host_command(command, capture=False, check=True):
    from .core import LibvirtError

    try:
        result = subprocess.run(
            command, text=True, capture_output=capture, check=False)
    except OSError as error:
        raise LibvirtError(
            f"cannot execute {command[0]}: {error}") from error
    if check and result.returncode:
        detail = result.stderr.strip() if capture else ""
        suffix = f": {detail}" if detail else ""
        raise LibvirtError(
            f"host network command failed with status {result.returncode}: "
            f"{shlex.join(command)}{suffix}")
    return result


def host_ifb_name(run_id, target):
    digest = hashlib.sha256(f"{run_id}:{target}".encode()).hexdigest()[:12]
    return f"ui{digest}"


def libvirt_data_tap(config, run_id, node):
    from .core import LibvirtError, executable

    virsh = executable("VIRSH_COMMAND", "virsh")
    domain = f"{run_id}-{node}"
    source = f"{run_id}-data"
    result = run_host_command([
        virsh, "-c", config["libvirt"]["uri"], "domiflist", domain,
    ], capture=True)
    for line in result.stdout.splitlines():
        fields = line.split()
        if len(fields) >= 5 and fields[1] == "network" and fields[2] == source:
            tap = fields[0]
            if not re.fullmatch(r"[A-Za-z0-9_.:@-]{1,15}", tap):
                break
            return tap
    raise LibvirtError(f"cannot resolve the data tap for {domain}")


def load_host_network(state, run_id):
    from .core import LibvirtError, read_json

    path = state / "host-network.json"
    record = read_json(path, "host network ownership")
    if (record.get("schemaVersion") != 1 or record.get("runId") != run_id or
            record.get("emulationPoint") != "host-tap-ingress" or
            not isinstance(record.get("endpoints"), dict)):
        raise LibvirtError(f"host network ownership mismatch: {path}")
    for endpoint in record["endpoints"].values():
        if (not isinstance(endpoint, dict) or
                not re.fullmatch(r"[A-Za-z0-9_.:@-]{1,15}",
                                 endpoint.get("tap", "")) or
                (endpoint.get("ifb") is not None and
                 not re.fullmatch(r"ui[0-9a-f]{12}", endpoint["ifb"]))):
            raise LibvirtError(f"invalid host network ownership: {path}")
    return record


def clear_host_network(state, run_id):
    from .core import SIMULATION

    path = state / "host-network.json"
    if not path.is_file():
        return
    record = load_host_network(state, run_id)
    netem = os.environ.get(
        "NETEM_COMMAND", str(SIMULATION / "network" / "netem"))
    for endpoint in record["endpoints"].values():
        run_host_command(
            [netem, "clear-ingress", endpoint["tap"]], check=False)
    for ifb in sorted({endpoint["ifb"]
                       for endpoint in record["endpoints"].values()
                       if endpoint["ifb"] is not None}):
        run_host_command([netem, "destroy-ingress", ifb], check=False)


def apply_host_network(state, config, values, run_id):
    from .core import LibvirtError, SIMULATION, write_json

    if config["network"]["emulationPoint"] != "host-tap-ingress":
        return False
    default_shaping = config["network"]["shaping"]
    endpoint_shaping = config["network"].get("endpointShaping", {})
    oversubscription = config["network"]["oversubscription"]
    workers = sorted(
        name for name, node in values["nodes"].items()
        if node["role"] == "worker")
    endpoints = {}
    shared_ifb = (host_ifb_name(run_id, "oversubscription")
                  if oversubscription["enabled"] else None)
    for index, node in enumerate(workers, 1):
        target = f"worker-{index:02d}/data"
        shaping = endpoint_shaping.get(target, default_shaping)
        endpoints[node] = {
            "target": target,
            "tap": libvirt_data_tap(config, run_id, node),
            "ifb": (shared_ifb if shared_ifb else
                    (host_ifb_name(run_id, target)
                     if shaping["enabled"] else None)),
            "shaping": shaping,
            "appliedCommand": "",
        }
    record = {
        "schemaVersion": 1,
        "runId": run_id,
        "emulationPoint": "host-tap-ingress",
        "oversubscription": oversubscription,
        "endpoints": endpoints,
    }
    write_json(state / "host-network.json", record)
    netem = os.environ.get(
        "NETEM_COMMAND", str(SIMULATION / "network" / "netem"))
    config_path = str(state / "resolved-simulation.json")
    try:
        if oversubscription["enabled"]:
            first = True
            for endpoint in endpoints.values():
                if first:
                    command = [
                        netem, "apply-ingress", config_path, endpoint["tap"],
                        endpoint["ifb"], "--oversubscription",
                        "--replace-existing",
                    ]
                    first = False
                else:
                    command = [
                        netem, "attach-ingress", endpoint["tap"],
                        endpoint["ifb"], "--replace-existing",
                    ]
                result = run_host_command(command, capture=True)
                endpoint["appliedCommand"] = (
                    result.stdout.strip() or shlex.join(command))
        else:
            for endpoint in endpoints.values():
                if endpoint["ifb"] is None:
                    endpoint["appliedCommand"] = "shaping disabled"
                    continue
                command = [
                    netem, "apply-ingress", config_path, endpoint["tap"],
                    endpoint["ifb"], "--target", endpoint["target"],
                    "--replace-existing",
                ]
                result = run_host_command(command, capture=True)
                endpoint["appliedCommand"] = result.stdout.strip()
        write_json(state / "host-network.json", record)
    except LibvirtError:
        clear_host_network(state, run_id)
        raise
    return True


def record_host_network_evidence(state, config, run_id):
    from .core import LibvirtError, SIMULATION, executable, verbs_active_mtu, write_json

    record = load_host_network(state, run_id)
    netem = os.environ.get(
        "NETEM_COMMAND", str(SIMULATION / "network" / "netem"))
    tc = executable("TC_COMMAND", "tc")
    iproute2 = run_host_command(
        [tc, "-Version"], capture=True).stdout.strip()
    kernel = run_host_command(
        ["uname", "-r"], capture=True).stdout.strip()
    endpoints = {}
    for node, endpoint in record["endpoints"].items():
        if endpoint["ifb"] is None:
            qdisc = run_host_command(
                [tc, "-s", "qdisc", "show", "dev", endpoint["tap"]],
                capture=True).stdout.strip()
            active = False
        else:
            qdisc = run_host_command([
                netem, "show-ingress", endpoint["tap"], endpoint["ifb"],
            ], capture=True).stdout.strip()
            active = True
        if active and ("qdisc ingress " not in qdisc or
                       "qdisc netem " not in qdisc or
                       not re.search(r"Sent [1-9][0-9]* bytes", qdisc)):
            raise LibvirtError(
                f"host ingress evidence has no traffic for {node}")
        if not active and "qdisc netem " in qdisc:
            raise LibvirtError(f"unexpected host qdisc state for {node}")
        endpoints[node] = {
            **endpoint,
            "qdisc": qdisc,
            "iproute2": iproute2,
            "kernel": kernel,
            "mtu": config["network"]["mtu"],
            "seedApplied": None,
        }
    path = state / "network.json"
    write_json(path, {
        "schemaVersion": 1,
        "runId": run_id,
        "scope": "worker-data-host-tap-ingress",
        "interface": config["network"]["dataInterface"],
        "mtu": config["network"]["mtu"],
        "activeMtu": verbs_active_mtu(config["network"]["mtu"]),
        "replaceExisting": True,
        "emulationPoint": "host-tap-ingress",
        "shaping": config["network"]["shaping"],
        "oversubscription": record["oversubscription"],
        "endpoints": endpoints,
    })
    return path
