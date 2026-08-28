# SPDX-License-Identifier: Apache-2.0

import json
import pathlib
import subprocess


def delete_node_storage(topology_value, state):
    from .core import VirtmeError

    storages = topology_value.get("nodeStorage", {})
    if not isinstance(storages, dict):
        raise VirtmeError("virtme topology has invalid node storage")
    if not storages:
        return
    for name, storage in storages.items():
        if name not in {"enroot", "k3s"} or not isinstance(storage, dict):
            raise VirtmeError("virtme topology has invalid node storage")
        images = storage.get("images")
        if not isinstance(images, dict):
            raise VirtmeError("virtme topology has invalid node storage")
        root = (state / "node-storage" / name).resolve()
        for value in images.values():
            if not isinstance(value, str):
                raise VirtmeError(
                    "virtme topology has invalid node storage path")
            image = pathlib.Path(value)
            try:
                if image.resolve().parent != root or image.suffix != ".ext4":
                    raise VirtmeError(
                        f"virtme node storage escapes its owned root: {image}")
                image.unlink(missing_ok=True)
            except OSError as error:
                raise VirtmeError(
                    f"cannot remove virtme node storage {image}: {error}") \
                    from error


def load_ledger(state, run_id):
    from .core import VirtmeError, read_json

    path = state / "ledger.json"
    ledger = read_json(path, "infrastructure ledger")
    if (ledger.get("schemaVersion") != 1 or ledger.get("runId") != run_id or
            ledger.get("provider") != "virtme"):
        raise VirtmeError(f"infrastructure ledger ownership mismatch: {path}")
    return path, ledger


def command_render(args):
    from .core import load_config, validate_name
    from .topology import topology

    validate_name(args.name)
    config = load_config(args.config)
    print(json.dumps(topology(config, args.name), indent=2, sort_keys=True))


def command_up(args):
    from .artifacts import acquire_k3s, acquire_slurm_packages, configure_kubernetes_storage, configure_slurm_storage, create_container_smoke_image, extract_slurm_runtime, prepare_container_runtime
    from .core import ENROOT_VERSION, PYXIS_VERSION, SLURM_VERSION, VirtmeError, executable, load_config, validate_name, write_json
    from .kubernetes import inventory, provision_kubernetes, write_kubeconfig
    from .network import apply_network_shaping, clear_network_shaping, create_network, delete_network, record_network_evidence
    from .processes import launch_api_forward, launch_guest, terminate_process, wait_for_ssh
    from .slurm import provision_slurm
    from .topology import topology

    validate_name(args.name)
    config = load_config(args.config)
    state = pathlib.Path(args.state_dir).expanduser().resolve()
    try:
        state.mkdir(parents=True)
    except FileExistsError as error:
        raise VirtmeError(f"infrastructure state already exists: {state}") from error
    except OSError as error:
        raise VirtmeError(f"cannot create infrastructure state: {error}") from error

    ledger_path = state / "ledger.json"
    ledger = {
        "schemaVersion": 1,
        "runId": args.name,
        "provider": "virtme",
        "clusterManager": config["cluster"]["manager"],
        "state": "preparing",
    }
    write_json(ledger_path, ledger)
    write_json(state / "resolved-simulation.json", config)
    topology_value = topology(config, args.name)
    write_json(state / "topology.json", topology_value)
    processes = []
    network_created = False
    shaping_commands = {}
    try:
        if config["cluster"]["manager"] == "kubernetes":
            configure_kubernetes_storage(config, topology_value, state)
            write_json(state / "topology.json", topology_value)
        elif config["cluster"]["manager"] == "slurm":
            configure_slurm_storage(config, topology_value, state)
            write_json(state / "topology.json", topology_value)
        key = state / "id_ed25519"
        keygen = executable("SSH_KEYGEN_COMMAND", "ssh-keygen")
        result = subprocess.run([
            keygen, "-q", "-t", "ed25519", "-N", "", "-f", str(key),
        ], check=False)
        if result.returncode:
            raise VirtmeError("ssh-keygen failed")
        key.chmod(0o600)
        public_key = key.with_suffix(".pub")
        vng = executable("VNG_COMMAND", "vng")
        ssh = executable("SSH_COMMAND", "ssh")
        k3s = None
        slurm_runtime = None
        smoke_image = None
        if config["cluster"]["manager"] == "kubernetes":
            cache = pathlib.Path(args.cache_dir).expanduser().resolve()
            k3s = acquire_k3s(config, cache)
        elif config["cluster"]["manager"] == "slurm":
            cache = pathlib.Path(args.cache_dir).expanduser().resolve()
            packages = acquire_slurm_packages(cache)
            slurm_runtime = extract_slurm_runtime(packages, cache)
            slurm_runtime = prepare_container_runtime(slurm_runtime, cache)
            smoke_image = create_container_smoke_image(slurm_runtime, cache)
        network_created = create_network(topology_value["network"])
        logs = state / "logs"
        logs.mkdir()
        ledger["state"] = "starting"
        write_json(ledger_path, ledger)
        for name, guest in topology_value["guests"].items():
            process = launch_guest(
                vng, config, guest, public_key, logs / f"{name}.log")
            processes.append(process)
            topology_value["ownership"]["processes"] = processes
            write_json(state / "topology.json", topology_value)
        for name, guest in topology_value["guests"].items():
            process = next(item for item in processes
                           if item["node"] == guest["hostname"])
            wait_for_ssh(ssh, key, guest, process, logs / f"{name}.log")
        write_json(state / "inventory.json", inventory(
            topology_value, key, config["cluster"]["manager"]))
        shaping_commands = apply_network_shaping(
            config, topology_value, ssh, key)
        if k3s:
            ledger["state"] = "provisioning"
            write_json(ledger_path, ledger)
            kubeconfig_value = provision_kubernetes(
                config, topology_value, ssh, key, k3s)
            forward = launch_api_forward(
                ssh, key, topology_value["guests"]["controller"],
                topology_value["kubernetesApiPort"],
                logs / "kubernetes-api-forward.log")
            processes.append(forward)
            topology_value["ownership"]["processes"] = processes
            write_json(state / "topology.json", topology_value)
            kubeconfig = write_kubeconfig(
                state, kubeconfig_value, topology_value["kubernetesApiPort"])
            ledger["kubeconfig"] = kubeconfig.name
            ledger["k3sBinary"] = str(k3s)
        elif slurm_runtime:
            ledger["state"] = "provisioning"
            write_json(ledger_path, ledger)
            provision_slurm(
                config, topology_value, ssh, key, slurm_runtime, smoke_image,
                state)
            ledger["slurmVersion"] = SLURM_VERSION
            ledger["slurmRuntime"] = str(slurm_runtime)
            ledger["enrootVersion"] = ENROOT_VERSION
            ledger["pyxisVersion"] = PYXIS_VERSION
            ledger["uibTraffic"] = (
                "passed-direct-and-pyxis" if config["uib"]["enabled"]
                else "disabled")
        network_evidence = record_network_evidence(
            config, topology_value, ssh, key, shaping_commands, state)
        ledger["networkEvidence"] = network_evidence.name
        ledger["state"] = "ready"
        ledger["topology"] = "topology.json"
        ledger["inventory"] = "inventory.json"
        write_json(ledger_path, ledger)
    except (OSError, VirtmeError) as error:
        ledger["state"] = "failed"
        ledger["error"] = str(error)
        if not args.retain:
            if "ssh" in locals() and "key" in locals():
                clear_network_shaping(
                    config, topology_value, ssh, key)
            for process in reversed(processes):
                terminate_process(process)
            try:
                if network_created:
                    delete_network(topology_value["network"])
                delete_node_storage(topology_value, state)
                ledger["cleanup"] = "destroyed"
            except VirtmeError as cleanup_error:
                ledger["cleanup"] = "failed"
                ledger["cleanupError"] = str(cleanup_error)
        write_json(ledger_path, ledger)
        raise
    print(state)


def command_status(args):
    from .core import VirtmeError, executable, read_json, validate_name
    from .processes import owned_process_alive, remote_command
    from .slurm import slurm_command

    validate_name(args.name)
    state = pathlib.Path(args.state_dir).expanduser().resolve()
    unused, ledger = load_ledger(state, args.name)
    topology_value = read_json(state / "topology.json", "virtme topology")
    processes = topology_value.get("ownership", {}).get("processes", [])
    expected = len(topology_value.get("guests", {}))
    guests = [process for process in processes
              if process.get("node") != "kubernetes-api-forward"]
    live = sum(1 for process in guests if owned_process_alive(process))
    forward = next((process for process in processes
                    if process.get("node") == "kubernetes-api-forward"), None)
    needs_forward = ledger.get("clusterManager") == "kubernetes"
    forward_ready = (not needs_forward if forward is None
                     else owned_process_alive(forward))
    report = {
        "runId": args.name,
        "provider": "virtme",
        "state": ledger["state"],
        "guests": expected,
        "recordedGuests": len(guests),
        "liveGuests": live,
    }
    if needs_forward:
        report["kubernetesApiForward"] = forward_ready
    if (ledger.get("clusterManager") == "slurm" and
            ledger.get("state") == "ready" and live == expected):
        runtime = ledger.get("slurmRuntime")
        if not isinstance(runtime, str):
            raise VirtmeError("virtme Slurm runtime is not recorded")
        ssh = executable("SSH_COMMAND", "ssh")
        result = remote_command(
            ssh, state / "id_ed25519",
            topology_value["guests"]["controller"],
            slurm_command(runtime, "scontrol", ["ping"]),
            capture=True, check=False)
        report["slurmController"] = (
            result is not None and result.returncode == 0)
        forward_ready = report["slurmController"]
    if (ledger["state"] == "ready" and
            (len(guests) != expected or live != expected or not forward_ready)):
        report["state"] = "degraded"
    print(json.dumps(report, indent=2, sort_keys=True))
    return 0 if report["state"] == "ready" else 1


def command_kubeconfig(args):
    from .core import VirtmeError, validate_name

    validate_name(args.name)
    state = pathlib.Path(args.state_dir).expanduser().resolve()
    unused, ledger = load_ledger(state, args.name)
    if (ledger.get("clusterManager") != "kubernetes" or
            ledger.get("state") != "ready" or
            ledger.get("kubeconfig") != "kubeconfig"):
        raise VirtmeError("the owned virtme Kubernetes cluster is not ready")
    try:
        print((state / "kubeconfig").read_text(encoding="utf-8"), end="")
    except OSError as error:
        raise VirtmeError(f"cannot read Kubernetes configuration: {error}") from error


def command_down(args):
    from .core import VirtmeError, executable, read_json, validate_name, write_json
    from .network import clear_network_shaping, delete_network
    from .processes import terminate_process

    validate_name(args.name)
    state = pathlib.Path(args.state_dir).expanduser().resolve()
    ledger_path, ledger = load_ledger(state, args.name)
    if ledger.get("state") == "destroyed" or ledger.get("cleanup") == "destroyed":
        return
    topology_value = read_json(state / "topology.json", "virtme topology")
    processes = topology_value.get("ownership", {}).get("processes")
    if not isinstance(processes, list):
        raise VirtmeError("virtme topology has invalid process ownership")
    config = read_json(
        state / "resolved-simulation.json", "resolved simulation")
    ssh = executable("SSH_COMMAND", "ssh")
    clear_network_shaping(
        config, topology_value, ssh, state / "id_ed25519")
    for process in reversed(processes):
        terminate_process(process)
    delete_network(topology_value["network"])
    delete_node_storage(topology_value, state)
    ledger["state"] = "destroyed"
    write_json(ledger_path, ledger)
