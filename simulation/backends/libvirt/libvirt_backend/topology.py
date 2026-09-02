# SPDX-License-Identifier: Apache-2.0

import grp
import ipaddress
import os
import pathlib
import pwd
import re


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
    from .core import LibvirtError, mac_address, quantity

    names = [("controller", "controller", 0)]
    names.extend((f"worker-{index:02d}", "worker", index)
                 for index in range(1, config["nodes"]["workers"] + 1))
    management, data = networks(config, len(names))
    ssh_port_base = config["libvirt"].get("sshPortBase", 22000)
    if ssh_port_base + config["nodes"]["workers"] > 65535:
        raise LibvirtError("libvirt SSH port range exceeds 65535")
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
            "ssh_port": ssh_port_base + index,
            "data_ip": str(data[address_index]),
            "data_mac": mac_address(run_id, name, "data"),
        }
    return result


def account_id(value, database, attribute, description):
    from .core import LibvirtError

    if value.isdigit():
        return value
    try:
        return str(getattr(database(value), attribute))
    except KeyError as error:
        raise LibvirtError(f"unknown {description}: {value}") from error


def qemu_config_value(name):
    try:
        value = pathlib.Path("/etc/libvirt/qemu.conf").read_text(
            encoding="utf-8")
    except OSError:
        return None
    match = re.search(rf'^\s*{name}\s*=\s*"([^"]+)"\s*$', value,
                      re.MULTILINE)
    return match.group(1) if match else None


def qemu_identity(config):
    from .core import LibvirtError

    settings = config["libvirt"]
    uri = settings["uri"]
    owner = settings.get("volumeOwner")
    group = settings.get("volumeGroup")

    if uri == "qemu:///session":
        return str(os.getuid()), str(os.getgid())
    if uri != "qemu:///system":
        if owner is None or group is None:
            raise LibvirtError(
                "remote libvirt requires volumeOwner and volumeGroup")
        if not owner.isdigit() or not group.isdigit():
            raise LibvirtError(
                "remote volumeOwner and volumeGroup must be decimal IDs")
        return owner, group
    if owner is None:
        owner = qemu_config_value("user")
    if owner is None:
        for candidate in ("libvirt-qemu", "qemu"):
            try:
                pwd.getpwnam(candidate)
                owner = candidate
                break
            except KeyError:
                continue
    if owner is None:
        raise LibvirtError("cannot determine the libvirt QEMU account")
    owner_id = account_id(owner, pwd.getpwnam, "pw_uid", "QEMU user")
    if group is None:
        group = qemu_config_value("group")
    if group is None:
        account = pwd.getpwuid(int(owner_id))
        try:
            group_id = str(grp.getgrnam(account.pw_name).gr_gid)
        except KeyError:
            group_id = str(account.pw_gid)
    else:
        group_id = account_id(group, grp.getgrnam, "gr_gid", "QEMU group")
    return owner_id, group_id


def inventory(values, private_key, manager):
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
            "uib_management_ip": node["management_ip"],
            "uib_role": node["role"],
        }
    controller = {name: {} for name, node in values["nodes"].items()
                  if node["role"] == "controller"}
    workers = {name: {} for name, node in values["nodes"].items()
               if node["role"] == "worker"}
    children = {
        f"{manager}_controller": {"hosts": controller},
        f"{manager}_workers": {"hosts": workers},
        f"{manager}_cluster": {"hosts": hosts},
    }
    return {
        "all": {
            "vars": {
                "ansible_user": values["ssh_user"],
                "ansible_ssh_private_key_file": str(private_key),
                "ansible_ssh_common_args": (
                    "-o StrictHostKeyChecking=no "
                    "-o UserKnownHostsFile=/dev/null"),
            },
            "children": children,
        },
    }
