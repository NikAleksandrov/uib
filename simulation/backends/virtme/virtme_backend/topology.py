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


def network_topology(config, run_id, node_count):
    from .core import bridge_name, socket_endpoint

    mode = config["virtme"]["networkMode"]
    if mode == "socket":
        address, port = socket_endpoint(run_id)
        return {
            "type": "qemu-socket",
            "multicastAddress": address,
            "multicastPort": port,
            "dataInterface": "eth1",
        }
    network = data_network(config, node_count)
    name = bridge_name(config, run_id)
    gateway = str(network[1])
    return {
        "type": "linux-bridge",
        "name": name,
        "address": gateway,
        "prefix": network.prefixlen,
        "dataInterface": "eth0",
        "create": [
            ["ip", "link", "add", "name", name, "type", "bridge"],
            ["ip", "address", "add", f"{gateway}/{network.prefixlen}",
             "dev", name],
            ["ip", "link", "set", "dev", name, "up"],
        ],
        "delete": [
            "ip", "link", "delete", "dev", name, "type", "bridge"],
    }


def qemu_network_options(network, node):
    return (
        f"-device virtio-net-pci,netdev=data,mac={node['dataMac']} "
        "-netdev socket,id=data,mcast="
        f"{network['multicastAddress']}:{network['multicastPort']}")


def vng_command(config, network, node):
    from .core import quantity_mib

    command = ["vng", "--run"]
    kernel = config["infrastructure"]["kernel"]
    if kernel != "running":
        command.append(kernel)
    command.extend([
        "--name", node["hostname"],
        "--cpus", str(config["nodes"]["cpu"]),
        "--memory", f"{quantity_mib(config['nodes']['memory'])}M",
    ])
    if network["type"] == "qemu-socket":
        command.append(f"--qemu-opts={qemu_network_options(network, node)}")
    else:
        command.extend([
            "--network", f"bridge={network['name']}",
            "--net-mac-address", node["dataMac"],
        ])
    command.extend([
        "--ssh", str(node["sshPort"]),
        "--ssh-tcp",
        "--quiet",
    ])
    for argument in config["virtme"]["extraKernelArguments"]:
        command.extend(["--append", argument])
    return command


def topology(config, run_id):
    guests = nodes(config, run_id)
    network = network_topology(config, run_id, len(guests))
    bridges = [network["name"]] if network["type"] == "linux-bridge" else []
    return {
        "schemaVersion": 1,
        "runId": run_id,
        "provider": "virtme",
        "state": "rendered",
        "kernel": config["infrastructure"]["kernel"],
        "network": network,
        "guests": {
            name: {**node, "command": vng_command(config, network, node)}
            for name, node in guests.items()
        },
        "ownership": {
            "processes": [],
            "taps": [],
            "bridges": bridges,
            "temporaryRoots": [],
        },
    }
