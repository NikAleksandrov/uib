# SPDX-License-Identifier: Apache-2.0

import ipaddress


def networks(config, node_count):
    from .core import LibvirtError

    try:
        management = ipaddress.ip_network(
            config["network"]["managementCIDR"], strict=True)
        data = ipaddress.ip_network(config["network"]["dataCIDR"], strict=True)
    except (KeyError, ValueError) as error:
        raise LibvirtError(f"invalid libvirt network: {error}") from error
    if management.version != 4 or data.version != 4:
        raise LibvirtError("the current libvirt backend requires IPv4 networks")
    if management.overlaps(data):
        raise LibvirtError("management and data networks overlap")
    maximum = 20 + node_count
    if maximum >= management.num_addresses - 1 or maximum >= data.num_addresses - 1:
        raise LibvirtError("libvirt networks are too small for the requested nodes")
    return management, data


def node_values(config, run_id):
    from .core import mac_address, quantity

    names = [("controller", "controller", 0)]
    names.extend((f"worker-{index:02d}", "worker", index)
                 for index in range(1, config["nodes"]["workers"] + 1))
    management, data = networks(config, len(names))
    result = {}
    for name, role, index in names:
        address_index = 10 if role == "controller" else 20 + index
        result[name] = {
            "role": role,
            "index": index,
            "cpu": config["nodes"]["cpu"],
            "memory_mib": quantity(config["nodes"]["memory"], "mib"),
            "disk_bytes": quantity(config["nodes"]["disk"], "bytes"),
            "management_ip": str(management[address_index]),
            "management_mac": mac_address(run_id, name, "management"),
            "ssh_port": 22000 + index,
            "data_ip": str(data[address_index]),
            "data_mac": mac_address(run_id, name, "data"),
        }
    return result


def inventory(values, private_key):
    hosts = {}
    for name, node in values["nodes"].items():
        hosts[name] = {
            "ansible_host": (
                "127.0.0.1" if values["network_mode"] == "rootless"
                else node["management_ip"]),
            "ansible_port": (
                node["ssh_port"] if values["network_mode"] == "rootless"
                else 22),
            "uib_data_ip": node["data_ip"],
            "uib_role": node["role"],
        }
    controller = {name: {} for name, node in values["nodes"].items()
                  if node["role"] == "controller"}
    workers = {name: {} for name, node in values["nodes"].items()
               if node["role"] == "worker"}
    return {
        "all": {
            "vars": {
                "ansible_user": values["ssh_user"],
                "ansible_ssh_private_key_file": str(private_key),
                "ansible_ssh_common_args": (
                    "-o StrictHostKeyChecking=no "
                    "-o UserKnownHostsFile=/dev/null"),
            },
            "children": {
                "slurm_controller": {"hosts": controller},
                "slurm_workers": {"hosts": workers},
                "slurm_cluster": {"hosts": hosts},
            },
        },
    }
