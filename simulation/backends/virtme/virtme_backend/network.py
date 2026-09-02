# SPDX-License-Identifier: Apache-2.0

import hashlib
import json
import os
import re
import shlex
import subprocess


def apply_network_shaping(config, topology_value, ssh, key):
    from .core import SIMULATION, VirtmeError
    from .processes import remote_command, remote_write, worker_guests, worker_targets

    try:
        adapter = (SIMULATION / "network/netem").read_text(encoding="utf-8")
        schedule = (SIMULATION / "faults/network-schedule").read_text(
            encoding="utf-8")
        rank_schedule = (SIMULATION / "faults/rank-schedule").read_text(
            encoding="utf-8")
    except OSError as error:
        raise VirtmeError(
            f"cannot read the network adapters: {error}") from error
    resolved = json.dumps(config, sort_keys=True)
    commands = {}
    interface = config["network"]["dataInterface"]
    targets = worker_targets(topology_value)
    for guest in worker_guests(topology_value):
        target = targets[guest["hostname"]]
        remote_command(ssh, key, guest, shlex.join([
            "ip", "link", "set", "dev", interface, "mtu",
            str(config["network"]["mtu"]),
        ]))
        remote_write(ssh, key, guest, "/run/uib-netem", adapter, mode="0755")
        remote_write(ssh, key, guest, "/run/uib-network-schedule", schedule,
                     mode="0755")
        remote_write(ssh, key, guest, "/run/uib-rank-schedule",
                     rank_schedule, mode="0755")
        remote_write(ssh, key, guest, "/run/uib-fault-target",
                     f"{target}\n")
        remote_write(
            ssh, key, guest, "/run/uib-simulation.json", resolved)
        result = remote_command(
            ssh, key, guest, shlex.join([
                "/run/uib-netem", "apply", "/run/uib-simulation.json",
                interface, "--target", target, "--replace-existing",
            ]), capture=True)
        commands[guest["hostname"]] = result.stdout.strip()
    return commands


def record_network_evidence(config, topology_value, ssh, key, commands, state):
    from .core import VirtmeError, verbs_active_mtu, write_json
    from .processes import remote_command, worker_guests, worker_targets

    default_shaping = config["network"]["shaping"]
    endpoint_shaping = config["network"].get("endpointShaping", {})
    interface = config["network"]["dataInterface"]
    targets = worker_targets(topology_value)
    endpoints = {}
    for guest in worker_guests(topology_value):
        hostname = guest["hostname"]
        target = targets[hostname]
        shaping = endpoint_shaping.get(target, default_shaping)
        qdisc = remote_command(
            ssh, key, guest,
            shlex.join(["/run/uib-netem", "show", interface]),
            capture=True).stdout.strip()
        iproute2 = remote_command(
            ssh, key, guest, "tc -Version", capture=True).stdout.strip()
        kernel = remote_command(
            ssh, key, guest, "uname -r", capture=True).stdout.strip()
        mtu_output = remote_command(
            ssh, key, guest,
            f"cat /sys/class/net/{shlex.quote(interface)}/mtu",
            capture=True).stdout.strip()
        applied = commands.get(hostname, "")
        if not all((applied, qdisc, iproute2, kernel)):
            raise VirtmeError(
                f"incomplete network evidence for {hostname}")
        active = "qdisc netem " in qdisc
        if active != shaping["enabled"]:
            raise VirtmeError(f"unexpected qdisc state for {hostname}")
        if active and not re.search(r"Sent [1-9][0-9]* bytes", qdisc):
            raise VirtmeError(
                f"network evidence has no traffic for {hostname}")
        try:
            mtu = int(mtu_output)
        except ValueError as error:
            raise VirtmeError(
                f"invalid link MTU evidence for {hostname}") from error
        if mtu != config["network"]["mtu"]:
            raise VirtmeError(f"unexpected link MTU for {hostname}")
        seed = shaping.get("seed")
        endpoints[hostname] = {
            "target": target,
            "shaping": shaping,
            "appliedCommand": applied,
            "qdisc": qdisc,
            "iproute2": iproute2,
            "kernel": kernel,
            "mtu": mtu,
            "seedApplied": (
                None if seed is None else f" seed {seed}" in applied),
        }
    path = state / "network.json"
    write_json(path, {
        "schemaVersion": 1,
        "runId": topology_value["runId"],
        "scope": "worker-data-egress",
        "interface": interface,
        "mtu": config["network"]["mtu"],
        "activeMtu": verbs_active_mtu(config["network"]["mtu"]),
        "replaceExisting": True,
        "shaping": default_shaping,
        "endpoints": endpoints,
    })
    return path


def clear_network_shaping(config, topology_value, ssh, key):
    from .processes import remote_command, worker_guests

    interface = config["network"]["dataInterface"]
    command = (
        "if test -x /run/uib-netem; then "
        f"/run/uib-netem clear {shlex.quote(interface)}; fi")
    for guest in worker_guests(topology_value):
        remote_command(
            ssh, key, guest, command, check=False, timeout=5)


def run_host_command(command, capture=False, check=True):
    from .core import VirtmeError

    try:
        result = subprocess.run(
            command, text=True, capture_output=capture, check=False)
    except OSError as error:
        raise VirtmeError(f"cannot execute {command[0]}: {error}") from error
    if check and result.returncode:
        detail = result.stderr.strip() if capture else ""
        suffix = f": {detail}" if detail else ""
        raise VirtmeError(
            f"host network command failed with status {result.returncode}: "
            f"{shlex.join(command)}{suffix}")
    return result


def host_ifb_name(run_id, target):
    digest = hashlib.sha256(f"{run_id}:{target}".encode()).hexdigest()[:12]
    return f"ui{digest}"


def bridge_worker_taps(config, topology_value, ssh, key):
    from .core import VirtmeError, executable
    from .processes import remote_command, worker_guests

    network = topology_value["network"]
    bridge = executable("BRIDGE_COMMAND", "bridge")
    gateway = network["address"]
    for guest in worker_guests(topology_value):
        remote_command(
            ssh, key, guest,
            shlex.join(["ping", "-c", "1", "-W", "1", gateway]),
            check=False)
    result = run_host_command(
        [bridge, "-j", "fdb", "show", "br", network["name"]],
        capture=True)
    try:
        entries = json.loads(result.stdout)
    except json.JSONDecodeError as error:
        raise VirtmeError("invalid bridge FDB output") from error
    if not isinstance(entries, list):
        raise VirtmeError("invalid bridge FDB output")
    by_mac = {
        entry.get("mac", "").lower(): entry.get("dev")
        for entry in entries if isinstance(entry, dict)
    }
    taps = {}
    for guest in worker_guests(topology_value):
        tap = by_mac.get(guest["dataMac"].lower())
        if not isinstance(tap, str) or not re.fullmatch(
                r"[A-Za-z0-9_.:@-]{1,15}", tap):
            raise VirtmeError(
                f"cannot resolve the data tap for {guest['hostname']}")
        taps[guest["hostname"]] = tap
    if len(set(taps.values())) != len(taps):
        raise VirtmeError("bridge FDB mapped multiple workers to one tap")
    return taps


def load_host_network(state, run_id):
    from .core import VirtmeError, read_json

    path = state / "host-network.json"
    record = read_json(path, "host network ownership")
    if (record.get("schemaVersion") != 1 or record.get("runId") != run_id or
            record.get("emulationPoint") != "host-tap-ingress" or
            not isinstance(record.get("endpoints"), dict)):
        raise VirtmeError(f"host network ownership mismatch: {path}")
    for endpoint in record["endpoints"].values():
        if (not isinstance(endpoint, dict) or
                not re.fullmatch(r"[A-Za-z0-9_.:@-]{1,15}",
                                 endpoint.get("tap", "")) or
                (endpoint.get("ifb") is not None and
                 not re.fullmatch(r"ui[0-9a-f]{12}", endpoint["ifb"]))):
            raise VirtmeError(f"invalid host network ownership: {path}")
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


def apply_host_network(config, topology_value, ssh, key, state):
    from .core import SIMULATION, VirtmeError, write_json
    from .processes import worker_guests, worker_targets

    if config["network"]["emulationPoint"] != "host-tap-ingress":
        return False
    taps = bridge_worker_taps(config, topology_value, ssh, key)
    default_shaping = config["network"]["shaping"]
    endpoint_shaping = config["network"].get("endpointShaping", {})
    oversubscription = config["network"]["oversubscription"]
    targets = worker_targets(topology_value)
    endpoints = {}
    shared_ifb = (host_ifb_name(topology_value["runId"], "oversubscription")
                  if oversubscription["enabled"] else None)
    for guest in worker_guests(topology_value):
        hostname = guest["hostname"]
        target = targets[hostname]
        shaping = endpoint_shaping.get(target, default_shaping)
        endpoints[hostname] = {
            "target": target,
            "tap": taps[hostname],
            "ifb": (shared_ifb if shared_ifb else
                    (host_ifb_name(topology_value["runId"], target)
                     if shaping["enabled"] else None)),
            "shaping": shaping,
            "appliedCommand": "",
        }
    record = {
        "schemaVersion": 1,
        "runId": topology_value["runId"],
        "emulationPoint": "host-tap-ingress",
        "oversubscription": oversubscription,
        "endpoints": endpoints,
    }
    write_json(state / "host-network.json", record)
    topology_value["ownership"]["taps"] = sorted(taps.values())
    topology_value["ownership"]["ifbs"] = sorted({
        endpoint["ifb"] for endpoint in endpoints.values()
        if endpoint["ifb"] is not None
    })
    write_json(state / "topology.json", topology_value)
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
    except VirtmeError:
        clear_host_network(state, topology_value["runId"])
        raise
    return True


def record_host_network_evidence(config, topology_value, state):
    from .core import SIMULATION, VirtmeError, executable, verbs_active_mtu, write_json

    record = load_host_network(state, topology_value["runId"])
    netem = os.environ.get(
        "NETEM_COMMAND", str(SIMULATION / "network" / "netem"))
    tc = executable("TC_COMMAND", "tc")
    iproute2 = run_host_command(
        [tc, "-Version"], capture=True).stdout.strip()
    kernel = run_host_command(
        ["uname", "-r"], capture=True).stdout.strip()
    endpoints = {}
    for hostname, endpoint in record["endpoints"].items():
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
            raise VirtmeError(
                f"host ingress evidence has no traffic for {hostname}")
        if not active and "qdisc netem " in qdisc:
            raise VirtmeError(
                f"unexpected host qdisc state for {hostname}")
        endpoints[hostname] = {
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
        "runId": topology_value["runId"],
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


def run_network_command(command):
    from .core import VirtmeError, executable

    resolved = executable("IP_COMMAND", "ip")
    actual = [resolved, *command[1:]]
    try:
        result = subprocess.run(actual, check=False)
    except OSError as error:
        raise VirtmeError(f"cannot execute {resolved}: {error}") from error
    if result.returncode:
        raise VirtmeError(
            f"network command failed with status {result.returncode}: "
            f"{shlex.join(command)}")


def create_network(network):
    from .core import VirtmeError

    if network["type"] != "linux-bridge":
        return False
    commands = network["create"]
    run_network_command(commands[0])
    try:
        for command in commands[1:]:
            run_network_command(command)
    except VirtmeError:
        try:
            delete_network(network)
        except VirtmeError:
            pass
        raise
    return True


def delete_network(network):
    if network["type"] == "linux-bridge":
        run_network_command(network["delete"])
