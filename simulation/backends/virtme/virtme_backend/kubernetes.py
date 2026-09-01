# SPDX-License-Identifier: Apache-2.0

import os
import re
import secrets
import shlex
import time


def k3s_launch_command(binary, arguments, log):
    command = shlex.join([str(binary), *arguments])
    return (
        f"nohup {command} > {shlex.quote(log)} 2>&1 < /dev/null & "
        f"echo $! > {shlex.quote(log + '.pid')}")


def wait_for_kubernetes(ssh, key, controller, binary):
    from .core import VirtmeError
    from .processes import remote_command

    try:
        timeout = float(os.environ.get("VIRTME_KUBERNETES_WAIT", "300"))
    except ValueError as error:
        raise VirtmeError("VIRTME_KUBERNETES_WAIT must be a number") from error
    if timeout <= 0:
        raise VirtmeError("VIRTME_KUBERNETES_WAIT must be positive")
    deadline = time.monotonic() + timeout
    command = shlex.join([str(binary), "kubectl", "get", "--raw=/readyz"])
    while time.monotonic() < deadline:
        result = remote_command(
            ssh, key, controller, command, capture=True, check=False)
        if result is not None and result.returncode == 0:
            return
        time.sleep(2)
    raise VirtmeError("the virtme Kubernetes API did not become ready")


def write_kubeconfig(state, value, port):
    from .core import VirtmeError

    pattern = r"server:\s*https://(?:127\.0\.0\.1|localhost):6443"
    replacement = f"server: https://127.0.0.1:{port}"
    updated, count = re.subn(pattern, replacement, value)
    if count != 1:
        raise VirtmeError("guest k3s configuration has an unexpected server URL")
    path = state / "kubeconfig"
    try:
        path.write_text(updated, encoding="utf-8")
        path.chmod(0o600)
    except OSError as error:
        raise VirtmeError(f"cannot write Kubernetes configuration: {error}") from error
    return path


def provision_kubernetes(config, topology_value, ssh, key, binary):
    from .core import VirtmeError
    from .processes import remote_command

    guests = topology_value["guests"]
    controller = guests["controller"]
    token = secrets.token_hex(32)
    common = [
        "--token", token,
        "--node-ip", controller["dataAddress"],
        "--flannel-iface", config["network"]["dataInterface"],
        "--snapshotter", "native",
        "--kubelet-arg",
        "root-dir=/var/lib/rancher/k3s/agent/kubelet",
    ]
    server = [
        "server", *common,
        "--advertise-address", controller["dataAddress"],
        "--node-taint", "node-role.kubernetes.io/control-plane=true:NoSchedule",
        "--tls-san", "127.0.0.1",
        "--cluster-cidr", config["kubernetes"]["podCIDR"],
        "--service-cidr", config["kubernetes"]["serviceCIDR"],
    ]
    for component in config["kubernetes"]["disable"]:
        server.extend(["--disable", component])
    remote_command(ssh, key, controller,
                   k3s_launch_command(binary, server, "/var/log/uib-k3s.log"))
    wait_for_kubernetes(ssh, key, controller, binary)
    for name, guest in guests.items():
        if guest["role"] != "worker":
            continue
        agent = [
            "agent",
            "--server", f"https://{controller['dataAddress']}:6443",
            "--token", token,
            "--node-ip", guest["dataAddress"],
            "--flannel-iface", config["network"]["dataInterface"],
            "--snapshotter", "overlayfs",
            "--kubelet-arg",
            "root-dir=/var/lib/rancher/k3s/agent/kubelet",
        ]
        remote_command(
            ssh, key, guest,
            k3s_launch_command(binary, agent, "/var/log/uib-k3s-agent.log"))
    expected = len(guests)
    try:
        timeout = float(os.environ.get("VIRTME_KUBERNETES_WAIT", "300"))
    except ValueError as error:
        raise VirtmeError("VIRTME_KUBERNETES_WAIT must be a number") from error
    if timeout <= 0:
        raise VirtmeError("VIRTME_KUBERNETES_WAIT must be positive")
    deadline = time.monotonic() + timeout
    nodes_command = shlex.join([
        str(binary), "kubectl", "get", "nodes", "-o", "name"])
    while time.monotonic() < deadline:
        result = remote_command(
            ssh, key, controller, nodes_command, capture=True, check=False)
        if (result is not None and result.returncode == 0 and
                len(result.stdout.splitlines()) == expected):
            break
        time.sleep(2)
    else:
        raise VirtmeError("not all virtme Kubernetes nodes became ready")
    remote_command(ssh, key, controller, shlex.join([
        str(binary), "kubectl", "wait", "--for=condition=Ready", "nodes",
        "--all", "--timeout=300s",
    ]), timeout=310)
    for guest in guests.values():
        if guest["role"] == "worker":
            remote_command(
                ssh, key, controller,
                shlex.join(["ping", "-c", "3", "-W", "2",
                            guest["dataAddress"]]))
    kubeconfig = remote_command(
        ssh, key, controller,
        shlex.join([str(binary), "kubectl", "config", "view", "--raw"]),
        capture=True)
    return kubeconfig.stdout


def inventory(topology_value, key, manager):
    hosts = {}
    for name, guest in topology_value["guests"].items():
        hosts[name] = {
            "ansible_host": "127.0.0.1",
            "ansible_port": guest["sshPort"],
            "uib_data_ip": guest["dataAddress"],
            "uib_role": guest["role"],
        }
    controller = {name: {} for name, guest in
                  topology_value["guests"].items()
                  if guest["role"] == "controller"}
    workers = {name: {} for name, guest in topology_value["guests"].items()
               if guest["role"] == "worker"}
    return {
        "all": {
            "vars": {
                "ansible_user": "root",
                "ansible_ssh_private_key_file": str(key),
                "ansible_ssh_common_args": (
                    "-o IdentitiesOnly=yes -o StrictHostKeyChecking=no "
                    "-o UserKnownHostsFile=/dev/null"),
            },
            "children": {
                f"{manager}_controller": {"hosts": controller},
                f"{manager}_workers": {"hosts": workers},
                f"{manager}_cluster": {"hosts": hosts},
            },
        },
    }
