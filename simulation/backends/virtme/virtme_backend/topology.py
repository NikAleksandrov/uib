# SPDX-License-Identifier: Apache-2.0

import ipaddress


def data_network(config, node_count):
    from .core import VirtmeError

    try:
        network = ipaddress.ip_network(
            config["network"]["dataCIDR"], strict=True)
    except (KeyError, ValueError) as error:
        raise VirtmeError(f"invalid virtme data network: {error}") from error
    if network.version != 4:
        raise VirtmeError(
            "the current virtme backend requires an IPv4 data network")
    if 20 + node_count >= network.num_addresses - 1:
        raise VirtmeError("virtme data network is too small for the guest set")
    return network


def nodes(config, run_id):
    from .core import VirtmeError, mac_address

    descriptions = [("controller", "controller", 0)]
    descriptions.extend((f"worker-{index:02d}", "worker", index)
                        for index in range(1, config["nodes"]["workers"] + 1))
    network = data_network(config, len(descriptions))
    port_base = config["virtme"]["sshPortBase"]
    if port_base + config["nodes"]["workers"] > 65535:
        raise VirtmeError("virtme SSH port range exceeds 65535")
    result = {}
    for name, role, index in descriptions:
        address_index = 10 if role == "controller" else 20 + index
        result[name] = {
            "role": role,
            "index": index,
            "hostname": f"{run_id}-{name}",
            "dataAddress": str(network[address_index]),
            "dataPrefix": network.prefixlen,
            "dataMac": mac_address(run_id, name),
            "sshPort": port_base + index,
        }
    return result


def vng_command(config, bridge, node):
    from .core import quantity_mib

    command = ["vng", "--run"]
    kernel = config["infrastructure"]["kernel"]
    if kernel != "running":
        command.append(kernel)
    command.extend([
        "--user", "root",
        "--name", node["hostname"],
        "--cpus", str(config["nodes"]["cpu"]),
        "--memory", f"{quantity_mib(config['nodes']['memory'])}M",
        "--network", f"bridge={bridge}",
        "--net-mac-address", node["dataMac"],
        "--ssh", str(node["sshPort"]),
        "--quiet",
    ])
    for argument in config["virtme"]["extraKernelArguments"]:
        command.extend(["--append", argument])
    return command


def topology(config, run_id):
    from .core import bridge_name

    bridge = bridge_name(config, run_id)
    guests = nodes(config, run_id)
    network = data_network(config, len(guests))
    gateway = str(network[1])
    return {
        "schemaVersion": 1,
        "runId": run_id,
        "provider": "virtme",
        "state": "rendered",
        "kernel": config["infrastructure"]["kernel"],
        "bridge": {
            "name": bridge,
            "address": gateway,
            "prefix": network.prefixlen,
            "create": [
                ["ip", "link", "add", "name", bridge, "type", "bridge"],
                ["ip", "address", "add", f"{gateway}/{network.prefixlen}",
                 "dev", bridge],
                ["ip", "link", "set", "dev", bridge, "up"],
            ],
            "delete": [
                "ip", "link", "delete", "dev", bridge, "type", "bridge"],
        },
        "guests": {
            name: {**node, "command": vng_command(config, bridge, node)}
            for name, node in guests.items()
        },
        "ownership": {
            "processes": [],
            "taps": [],
            "bridges": [bridge],
            "temporaryRoots": [],
        },
    }
