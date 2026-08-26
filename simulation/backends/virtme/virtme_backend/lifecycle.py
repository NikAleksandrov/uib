# SPDX-License-Identifier: Apache-2.0

import json
import pathlib
import subprocess


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
    from .core import VirtmeError, executable, load_config, validate_name, write_json
    from .kubernetes import inventory
    from .network import create_network, delete_network
    from .processes import launch_guest, terminate_process, wait_for_ssh
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
    try:
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
        ledger["state"] = "ready"
        ledger["topology"] = "topology.json"
        ledger["inventory"] = "inventory.json"
        write_json(ledger_path, ledger)
    except (OSError, VirtmeError) as error:
        ledger["state"] = "failed"
        ledger["error"] = str(error)
        if not args.retain:
            for process in reversed(processes):
                terminate_process(process)
            try:
                if network_created:
                    delete_network(topology_value["network"])
                ledger["cleanup"] = "destroyed"
            except VirtmeError as cleanup_error:
                ledger["cleanup"] = "failed"
                ledger["cleanupError"] = str(cleanup_error)
        write_json(ledger_path, ledger)
        raise
    print(state)


def command_status(args):
    from .core import read_json, validate_name
    from .processes import owned_process_alive

    validate_name(args.name)
    state = pathlib.Path(args.state_dir).expanduser().resolve()
    unused, ledger = load_ledger(state, args.name)
    topology_value = read_json(state / "topology.json", "virtme topology")
    processes = topology_value.get("ownership", {}).get("processes", [])
    expected = len(topology_value.get("guests", {}))
    live = sum(1 for process in processes if owned_process_alive(process))
    report = {
        "runId": args.name,
        "provider": "virtme",
        "state": ledger["state"],
        "guests": expected,
        "recordedGuests": len(processes),
        "liveGuests": live,
    }
    if (ledger["state"] == "ready" and
            (len(processes) != expected or live != expected)):
        report["state"] = "degraded"
    print(json.dumps(report, indent=2, sort_keys=True))
    return 0 if report["state"] == "ready" else 1


def command_down(args):
    from .core import VirtmeError, read_json, validate_name, write_json
    from .network import delete_network
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
    for process in reversed(processes):
        terminate_process(process)
    delete_network(topology_value["network"])
    ledger["state"] = "destroyed"
    write_json(ledger_path, ledger)
