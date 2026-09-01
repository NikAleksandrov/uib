# SPDX-License-Identifier: Apache-2.0

import json
import os
import pathlib
import re
import shlex
import signal
import subprocess
import time


def kind_nodes(config):
    nodes_value = [{
        "role": "control-plane",
        "image": config["cluster"]["nodeImage"],
    }]
    nodes_value.extend({
        "role": "worker",
        "image": config["cluster"]["nodeImage"],
    } for unused in range(config["nodes"]["workers"]))
    return nodes_value


def kind_guest(config, name, state=None):
    from .core import quantity_mib

    disk = ((state / "node-storage" / "kind" / "controller-00.ext4")
            if state else pathlib.Path("STATE/controller-00.ext4"))
    public_key = ((state / "id_ed25519.pub") if state else
                  pathlib.Path("STATE/id_ed25519.pub"))
    command = ["vng", "--run"]
    kernel = config["infrastructure"]["kernel"]
    if kernel != "running":
        command.append(kernel)
    command.extend([
        "--name", name,
        "--user", "root",
        "--network", "user",
        "--cpus", str(config["nodes"]["cpu"]),
        "--memory", f"{quantity_mib(config['nodes']['memory'])}M",
        "--ssh", str(config["virtme"]["sshPortBase"]),
        "--ssh-tcp",
        "--quiet",
    ])
    if state is None:
        command.extend(["--disk", str(disk)])
    for argument in config["virtme"]["extraKernelArguments"]:
        command.extend(["--append", argument])
    bootstrap = (
        "set -eu; install -d -m 0700 /var/lib/docker; "
        "mount -t ext4 /dev/vda /var/lib/docker; "
        f"cat {shlex.quote(str(public_key))} "
        "> /run/sshd/etc/ssh/authorized_keys; "
        "chmod 600 /run/sshd/etc/ssh/authorized_keys; exec sleep infinity")
    command.extend(["--exec", bootstrap])
    return {
        "role": "controller",
        "index": 0,
        "hostname": name,
        "sshPort": config["virtme"]["sshPortBase"],
        "command": command,
    }


def kind_topology(config, name, state=None):
    return {
        "schemaVersion": 1,
        "runId": name,
        "provider": "virtme",
        "mode": "kind",
        "guest": kind_guest(config, name, state),
        "kindCluster": name,
        "kindNodes": len(kind_nodes(config)),
        "kindNodeImage": config["cluster"]["nodeImage"],
        "kubernetesApiPort": config["virtme"]["sshPortBase"] + 1,
        "ownership": {
            "processes": [],
            "storage": [],
        },
    }


def launch_kind_guest(vng, guest, log):
    from .core import VirtmeError
    from .processes import process_start_time

    command = list(guest["command"])
    command[0] = vng
    try:
        with log.open("x", encoding="utf-8") as output:
            child = subprocess.Popen(
                command, stdin=subprocess.DEVNULL, stdout=output,
                stderr=output, start_new_session=True)
    except OSError as error:
        raise VirtmeError(f"cannot launch virtme Kind: {error}") from error
    start = process_start_time(child.pid)
    if start is None:
        try:
            os.killpg(child.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        raise VirtmeError("cannot record virtme Kind process identity")
    return {
        "node": guest["hostname"],
        "pid": child.pid,
        "startTime": start,
        "log": log.name,
    }


def kind_docker_start(ssh, key, guest, docker, dockerd):
    from .core import VirtmeError
    from .processes import remote_command

    docker_socket = "/run/uib-kind/docker.sock"
    command = (
        "set -eu; install -d -m 0700 /run/uib-kind; "
        f"nohup {shlex.quote(dockerd)} --host=unix://{docker_socket} "
        "--data-root=/var/lib/docker --exec-root=/run/uib-kind/exec "
        "> /run/uib-kind/dockerd.log 2>&1 & "
        "echo $! > /run/uib-kind/dockerd.pid")
    remote_command(ssh, key, guest, command)
    deadline = time.monotonic() + 60
    command = (
        f"env DOCKER_HOST=unix://{docker_socket} "
        f"{shlex.quote(docker)} info")
    while time.monotonic() < deadline:
        result = remote_command(
            ssh, key, guest, command, check=False)
        if result is not None and result.returncode == 0:
            return docker_socket
        time.sleep(0.5)
    raise VirtmeError("Docker inside virtme did not become ready")


def provision_kind(config, name, ssh, key, guest, kind, docker_socket):
    from .core import VirtmeError
    from .processes import remote_command, remote_write

    configuration = json.dumps({
        "kind": "Cluster",
        "apiVersion": "kind.x-k8s.io/v1alpha4",
        "nodes": kind_nodes(config),
    }, indent=2, sort_keys=True) + "\n"
    remote_write(
        ssh, key, guest, "/run/uib-kind/cluster.yaml", configuration)
    environment = f"env DOCKER_HOST=unix://{docker_socket}"
    remote_command(
        ssh, key, guest,
        f"{environment} {shlex.quote(kind)} create cluster "
        f"--name {shlex.quote(name)} "
        "--config /run/uib-kind/cluster.yaml --wait 120s",
        timeout=180)
    result = remote_command(
        ssh, key, guest,
        f"{environment} {shlex.quote(kind)} get kubeconfig "
        f"--name {shlex.quote(name)}", capture=True)
    match = re.search(
        r"server:\s*https://(?:127\.0\.0\.1|localhost):([0-9]+)",
        result.stdout)
    if not match:
        raise VirtmeError("nested Kind kubeconfig has an unexpected server URL")
    return result.stdout, int(match.group(1))


def command_up_kind(args, config):
    from .core import VirtmeError, executable, write_json
    from .kubernetes import write_kubeconfig
    from .processes import launch_api_forward, terminate_process, wait_for_ssh
    from .topology import create_node_disk

    state = pathlib.Path(args.state_dir).expanduser().resolve()
    try:
        state.mkdir(parents=True)
    except FileExistsError as error:
        raise VirtmeError(
            f"infrastructure state already exists: {state}") from error
    ledger_path = state / "ledger.json"
    ledger = {
        "schemaVersion": 1,
        "runId": args.name,
        "provider": "virtme",
        "mode": "kind",
        "clusterManager": "kubernetes",
        "state": "preparing",
    }
    write_json(ledger_path, ledger)
    write_json(state / "resolved-simulation.json", config)
    topology_value = kind_topology(config, args.name, state)
    write_json(state / "topology.json", topology_value)
    processes = []
    storage = None
    try:
        guest = topology_value["guest"]
        root = state / "node-storage" / "kind"
        root.mkdir(parents=True)
        storage = create_node_disk(config, guest, root, "uib-kind")
        topology_value["ownership"]["storage"] = [str(storage)]
        key = state / "id_ed25519"
        keygen = executable("SSH_KEYGEN_COMMAND", "ssh-keygen")
        result = subprocess.run([
            keygen, "-q", "-t", "ed25519", "-N", "", "-f", str(key),
        ], check=False)
        if result.returncode:
            raise VirtmeError("ssh-keygen failed")
        key.chmod(0o600)
        vng = executable("VNG_COMMAND", "vng")
        ssh = executable("SSH_COMMAND", "ssh")
        kind = executable("KIND_COMMAND", "kind")
        docker = executable("DOCKER_COMMAND", "docker")
        dockerd = executable("DOCKERD_COMMAND", "dockerd")
        logs = state / "logs"
        logs.mkdir()
        process = launch_kind_guest(vng, guest, logs / "kind.log")
        processes.append(process)
        topology_value["ownership"]["processes"] = processes
        write_json(state / "topology.json", topology_value)
        wait_for_ssh(ssh, key, guest, process, logs / "kind.log")
        docker_socket = kind_docker_start(
            ssh, key, guest, docker, dockerd)
        kubeconfig_value, remote_port = provision_kind(
            config, args.name, ssh, key, guest, kind, docker_socket)
        local_port = topology_value["kubernetesApiPort"]
        forward = launch_api_forward(
            ssh, key, guest, local_port,
            logs / "kubernetes-api-forward.log", remote_port)
        processes.append(forward)
        topology_value["ownership"]["processes"] = processes
        topology_value["dockerSocket"] = docker_socket
        topology_value["dockerCommand"] = docker
        topology_value["kindCommand"] = kind
        write_json(state / "topology.json", topology_value)
        write_kubeconfig(
            state, kubeconfig_value, local_port, remote_port)
        ledger["state"] = "ready"
        ledger["kubeconfig"] = "kubeconfig"
        ledger["topology"] = "topology.json"
        write_json(ledger_path, ledger)
    except (OSError, VirtmeError) as error:
        ledger["state"] = "failed"
        ledger["error"] = str(error)
        if not args.retain:
            for process in reversed(processes):
                terminate_process(process)
            if storage is not None:
                storage.unlink(missing_ok=True)
            ledger["cleanup"] = "destroyed"
        write_json(ledger_path, ledger)
        raise
    print(state)


def kind_processes(topology_value):
    from .core import VirtmeError

    processes = topology_value.get("ownership", {}).get("processes")
    if not isinstance(processes, list):
        raise VirtmeError("virtme Kind topology has invalid process ownership")
    return processes


def command_status_kind(args, ledger, topology_value):
    from .processes import owned_process_alive

    processes = kind_processes(topology_value)
    live = sum(1 for process in processes if owned_process_alive(process))
    state = ledger["state"]
    if state == "ready" and live != len(processes):
        state = "degraded"
    print(json.dumps({
        "runId": args.name,
        "provider": "virtme",
        "mode": "kind",
        "state": state,
        "recordedProcesses": len(processes),
        "liveProcesses": live,
        "kindNodes": topology_value["kindNodes"],
    }, indent=2, sort_keys=True))
    return 0 if state == "ready" else 1


def command_down_kind(args, ledger_path, ledger, topology_value):
    from .core import VirtmeError, executable, write_json
    from .processes import owned_process_alive, remote_command, terminate_process

    if ledger.get("state") == "destroyed":
        return
    state = pathlib.Path(args.state_dir).expanduser().resolve()
    processes = kind_processes(topology_value)
    guest_process = next((
        process for process in processes
        if process.get("node") == topology_value["guest"]["hostname"]), None)
    if guest_process is not None and owned_process_alive(guest_process):
        ssh = executable("SSH_COMMAND", "ssh")
        command = (
            f"env DOCKER_HOST=unix://{topology_value['dockerSocket']} "
            f"{shlex.quote(topology_value['kindCommand'])} delete cluster "
            f"--name {shlex.quote(args.name)}")
        remote_command(
            ssh, state / "id_ed25519", topology_value["guest"], command,
            check=False)
    for process in reversed(processes):
        terminate_process(process)
    storage = topology_value["ownership"].get("storage", [])
    if len(storage) != 1:
        raise VirtmeError("virtme Kind storage ownership is invalid")
    image = pathlib.Path(storage[0]).resolve()
    expected = (state / "node-storage" / "kind" / "controller-00.ext4").resolve()
    if image != expected:
        raise VirtmeError("virtme Kind storage ownership mismatch")
    image.unlink(missing_ok=True)
    ledger["state"] = "destroyed"
    write_json(ledger_path, ledger)


def command_load_kind_image(args):
    from .core import IMAGE_DIGEST, IMAGE_REFERENCE, VirtmeError, executable, read_json, validate_name, write_json
    from .lifecycle import load_ledger
    from .processes import owned_process_alive, remote_command, ssh_command

    validate_name(args.name)
    for reference in args.image:
        if not IMAGE_REFERENCE.fullmatch(reference):
            raise VirtmeError(f"invalid image reference: {reference}")
        if not args.allow_mutable_tags and not IMAGE_DIGEST.fullmatch(reference):
            raise VirtmeError(
                f"image reference is not digest-pinned: {reference}")
    state = pathlib.Path(args.state_dir).expanduser().resolve()
    unused, ledger = load_ledger(state, args.name)
    topology_value = read_json(state / "topology.json", "virtme topology")
    if ledger.get("mode") != "kind" or ledger.get("state") != "ready":
        raise VirtmeError("the owned virtme Kind cluster is not ready")
    processes = kind_processes(topology_value)
    guest = topology_value["guest"]
    process = next((item for item in processes
                    if item.get("node") == guest["hostname"]), None)
    if process is None or not owned_process_alive(process):
        raise VirtmeError("the virtme Kind guest is not running")
    ssh = executable("SSH_COMMAND", "ssh")
    engine = executable("CONTAINER_ENGINE_COMMAND", "docker")
    docker = topology_value["dockerCommand"]
    kind = topology_value["kindCommand"]
    docker_socket = topology_value["dockerSocket"]
    images = {}
    for reference in args.image:
        inspected = subprocess.run([
            engine, "image", "inspect", "--format={{.Id}}", reference,
        ], text=True, capture_output=True, check=False)
        if inspected.returncode:
            raise VirtmeError(f"local image is unavailable: {reference}")
        exporter = subprocess.Popen(
            [engine, "image", "save", reference], stdout=subprocess.PIPE,
            start_new_session=True)
        remote = (
            f"env DOCKER_HOST=unix://{docker_socket} "
            f"{shlex.quote(docker)} load")
        loaded = subprocess.run(
            ssh_command(
                ssh, state / "id_ed25519", guest["sshPort"], remote),
            stdin=exporter.stdout, stdout=subprocess.DEVNULL,
            stderr=subprocess.PIPE, check=False)
        exporter.stdout.close()
        export_status = exporter.wait()
        if export_status or loaded.returncode:
            raise VirtmeError(f"cannot load image in guest: {reference}")
        remote_command(
            ssh, state / "id_ed25519", guest,
            f"env DOCKER_HOST=unix://{docker_socket} {shlex.quote(kind)} "
            f"load docker-image {shlex.quote(reference)} "
            f"--name {shlex.quote(args.name)}")
        images[reference] = {"imageId": inspected.stdout.strip()}
    write_json(state / "kubernetes-images.json", {
        "schemaVersion": 1,
        "runId": args.name,
        "images": images,
    })
