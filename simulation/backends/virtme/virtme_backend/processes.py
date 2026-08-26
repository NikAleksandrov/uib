# SPDX-License-Identifier: Apache-2.0

import os
import pathlib
import shlex
import signal
import socket
import subprocess
import time


def process_start_time(pid):
    try:
        value = pathlib.Path(f"/proc/{pid}/stat").read_text(encoding="utf-8")
    except OSError:
        return None
    fields = value.rpartition(") ")[2].split()
    return fields[19] if len(fields) > 19 else None


def owned_process_alive(process):
    pid = process.get("pid")
    start = process.get("startTime")
    return (isinstance(pid, int) and pid > 1 and isinstance(start, str) and
            process_start_time(pid) == start)


def terminate_process(process):
    if not owned_process_alive(process):
        return
    pid = process["pid"]
    try:
        os.killpg(pid, signal.SIGTERM)
    except ProcessLookupError:
        return
    deadline = time.monotonic() + 10
    while time.monotonic() < deadline:
        if not owned_process_alive(process):
            return
        time.sleep(0.1)
    if owned_process_alive(process):
        try:
            os.killpg(pid, signal.SIGKILL)
        except ProcessLookupError:
            pass


def bootstrap_command(config, node, public_key):
    interface = config["network"]["dataInterface"]
    address = f"{node['dataAddress']}/{node['dataPrefix']}"
    key = shlex.quote(str(public_key))
    return (
        f"set -eu; ip address replace {address} dev {interface}; "
        f"ip link set dev {interface} up; "
        f"cat {key} > /run/sshd/etc/ssh/authorized_keys; "
        "chmod 600 /run/sshd/etc/ssh/authorized_keys; "
        "exec sleep infinity")


def ssh_command(ssh, key, port, remote):
    return [
        ssh,
        "-i", str(key),
        "-o", "BatchMode=yes",
        "-o", "IdentitiesOnly=yes",
        "-o", "StrictHostKeyChecking=no",
        "-o", "UserKnownHostsFile=/dev/null",
        "-o", "LogLevel=ERROR",
        "-o", "ConnectTimeout=2",
        "-p", str(port),
        "root@127.0.0.1",
        "--", remote,
    ]


def wait_for_ssh(ssh, key, guest, process, log):
    from .core import VirtmeError

    try:
        timeout = float(os.environ.get("VIRTME_SSH_WAIT", "90"))
    except ValueError as error:
        raise VirtmeError("VIRTME_SSH_WAIT must be a number") from error
    if timeout <= 0:
        raise VirtmeError("VIRTME_SSH_WAIT must be positive")
    deadline = time.monotonic() + timeout
    command = ssh_command(ssh, key, guest["sshPort"], "true")
    while time.monotonic() < deadline:
        if not owned_process_alive(process):
            raise VirtmeError(f"virtme guest exited: {guest['hostname']}")
        try:
            result = subprocess.run(
                command, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                timeout=5, check=False)
        except subprocess.TimeoutExpired:
            pass
        except OSError as error:
            raise VirtmeError(f"cannot execute {ssh}: {error}") from error
        else:
            if result.returncode == 0:
                return
        time.sleep(1)
    raise VirtmeError(
        f"SSH did not become ready for {guest['hostname']}; inspect {log}")


def remote_command(ssh, key, guest, command, capture=False, check=True,
                   timeout=30):
    from .core import VirtmeError

    try:
        result = subprocess.run(
            ssh_command(ssh, key, guest["sshPort"], command),
            text=True, stdout=subprocess.PIPE if capture else subprocess.DEVNULL,
            stderr=subprocess.DEVNULL, timeout=timeout, check=False)
    except (OSError, subprocess.TimeoutExpired) as error:
        if check:
            raise VirtmeError(
                f"cannot run command on {guest['hostname']}: {error}") from error
        return None
    if check and result.returncode:
        raise VirtmeError(
            f"remote command failed on {guest['hostname']} with status "
            f"{result.returncode}")
    return result


def launch_guest(vng, config, guest, public_key, log):
    from .core import VirtmeError

    command = list(guest["command"])
    command[0] = vng
    command.extend(["--exec", bootstrap_command(config, guest, public_key)])
    try:
        with log.open("x", encoding="utf-8") as output:
            process = subprocess.Popen(
                command, stdin=subprocess.DEVNULL, stdout=output, stderr=output,
                start_new_session=True)
    except OSError as error:
        raise VirtmeError(f"cannot launch {guest['hostname']}: {error}") from error
    start = None
    for unused in range(20):
        start = process_start_time(process.pid)
        if start:
            break
        time.sleep(0.01)
    if not start:
        try:
            os.killpg(process.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        raise VirtmeError(f"cannot record {guest['hostname']} process identity")
    return {
        "node": guest["hostname"],
        "pid": process.pid,
        "startTime": start,
        "log": log.name,
    }


def launch_api_forward(ssh, key, controller, port, log):
    from .core import VirtmeError

    command = [
        ssh,
        "-i", str(key),
        "-o", "BatchMode=yes",
        "-o", "IdentitiesOnly=yes",
        "-o", "StrictHostKeyChecking=no",
        "-o", "UserKnownHostsFile=/dev/null",
        "-o", "LogLevel=ERROR",
        "-o", "ExitOnForwardFailure=yes",
        "-o", "ServerAliveInterval=10",
        "-N",
        "-L", f"127.0.0.1:{port}:127.0.0.1:6443",
        "-p", str(controller["sshPort"]),
        "root@127.0.0.1",
    ]
    try:
        with log.open("x", encoding="utf-8") as output:
            child = subprocess.Popen(
                command, stdin=subprocess.DEVNULL, stdout=output, stderr=output,
                start_new_session=True)
    except OSError as error:
        raise VirtmeError(f"cannot launch Kubernetes API forward: {error}") from error
    process = {
        "node": "kubernetes-api-forward",
        "pid": child.pid,
        "startTime": process_start_time(child.pid) or "",
        "log": log.name,
    }
    deadline = time.monotonic() + 10
    while time.monotonic() < deadline:
        if not owned_process_alive(process):
            raise VirtmeError(
                f"Kubernetes API forward exited; inspect {log}")
        checker = os.environ.get("VIRTME_API_FORWARD_CHECK_COMMAND")
        if checker:
            try:
                result = subprocess.run(
                    [checker, str(port)], stdout=subprocess.DEVNULL,
                    stderr=subprocess.DEVNULL, check=False)
            except OSError as error:
                raise VirtmeError(
                    f"cannot check Kubernetes API forward: {error}") from error
            if result.returncode == 0:
                return process
        else:
            try:
                with socket.create_connection(("127.0.0.1", port), timeout=1):
                    return process
            except OSError:
                pass
        time.sleep(0.2)
    terminate_process(process)
    raise VirtmeError("Kubernetes API forward did not become ready")
