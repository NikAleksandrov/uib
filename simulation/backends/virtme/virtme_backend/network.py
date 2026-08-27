# SPDX-License-Identifier: Apache-2.0

import json
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
