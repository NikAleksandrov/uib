# SPDX-License-Identifier: Apache-2.0

import json
import pathlib
import shutil
import yaml


def command_render(args):
    from .core import load_config, validate_name
    from .tofu import tofu_values

    validate_name(args.name)
    config = load_config(args.config)
    public_key = pathlib.Path(args.ssh_public_key).read_text(encoding="utf-8")
    values = tofu_values(config, args.name, args.base_image, public_key)
    print(json.dumps(values, indent=2, sort_keys=True))


def command_up(args):
    from .core import LibvirtError, MODULE, PLAYBOOK, SIMULATION, executable, load_config, quantity, run_logged, validate_name, write_json
    from .tofu import acquire_base_image, destroy, tofu_environment, tofu_values
    from .topology import inventory

    validate_name(args.name)
    config = load_config(args.config)
    if config["cluster"]["manager"] != "slurm":
        raise LibvirtError(
            "libvirt Kubernetes provisioning is not implemented yet")
    state = pathlib.Path(args.state_dir).expanduser().resolve()
    cache = pathlib.Path(args.cache_dir).expanduser().resolve()
    try:
        state.mkdir(parents=True)
    except FileExistsError as error:
        raise LibvirtError(f"infrastructure state already exists: {state}") from error
    except OSError as error:
        raise LibvirtError(f"cannot create infrastructure state: {error}") from error

    ledger_path = state / "ledger.json"
    ledger = {
        "schemaVersion": 1,
        "runId": args.name,
        "provider": "libvirt",
        "clusterManager": config["cluster"]["manager"],
        "state": "preparing",
    }
    write_json(ledger_path, ledger)
    write_json(state / "resolved-simulation.json", config)
    log = state / "lifecycle.log"
    apply_started = False
    try:
        image = acquire_base_image(config, cache)
        key = state / "id_ed25519"
        ssh_keygen = executable("SSH_KEYGEN_COMMAND", "ssh-keygen")
        run_logged([ssh_keygen, "-q", "-t", "ed25519", "-N", "", "-f",
                    str(key)], log)
        public_key = key.with_suffix(".pub").read_text(encoding="utf-8")
        values = tofu_values(config, args.name, image, public_key)
        runtime_source = SIMULATION / ".artifacts" / "uib-runtime"
        if config["uib"]["enabled"] and not runtime_source.is_dir():
            raise LibvirtError(
                "portable UIB runtime is missing; run make runtime-bundle")
        write_json(state / "terraform.tfvars.json", values)
        with (state / "inventory.yaml").open("x", encoding="utf-8") as output:
            yaml.safe_dump(inventory(values, key), output, sort_keys=False)
        shutil.copytree(MODULE, state / "opentofu")

        tofu = executable("TOFU_COMMAND", "tofu")
        environment = tofu_environment(cache)
        run_logged([tofu, f"-chdir={state / 'opentofu'}", "init",
                    "-input=false"], log, environment)
        ledger["state"] = "applying"
        write_json(ledger_path, ledger)
        apply_started = True
        run_logged([
            tofu, f"-chdir={state / 'opentofu'}", "apply", "-auto-approve",
            f"-var-file={state / 'terraform.tfvars.json'}",
        ], log, environment)
        ledger["state"] = "provisioning"
        write_json(ledger_path, ledger)

        ansible = executable("ANSIBLE_PLAYBOOK_COMMAND", "ansible-playbook")
        run_logged([
            ansible, "-i", str(state / "inventory.yaml"), str(PLAYBOOK),
            "--extra-vars", json.dumps({
                "uib_run_id": args.name,
                "uib_run_state": str(state),
                "uib_config": str(state / "resolved-simulation.json"),
                "uib_runtime_source": str(runtime_source),
                "uib_runtime_path": config["storage"].get(
                    "uibRuntimePath", "/var/lib/uib/runtime"),
                "uib_model_cache_path": config["storage"]["modelCachePath"],
                "uib_results_path": config["storage"]["resultsPath"],
                "uib_enabled": config["uib"]["enabled"],
                "uib_ssh_user": config["libvirt"]["sshUser"],
                "uib_data_interface": config["network"]["dataInterface"],
                "uib_network_mode": config["libvirt"]["networkMode"],
                "uib_node_cpu": config["nodes"]["cpu"],
                "uib_node_memory_mib": quantity(
                    config["nodes"]["memory"], "mib"),
                "uib_slurm_partition": config["slurm"]["partition"],
                "uib_slurm_time_limit": config["slurm"]["timeLimit"],
                "uib_diagnostic_image": config["libvirt"]["diagnosticImage"],
                "uib_slurm_packages": [
                    "libslurm-dev=23.11.4-1.2ubuntu5",
                    "slurm-client=23.11.4-1.2ubuntu5",
                    "slurm-wlm-basic-plugins=23.11.4-1.2ubuntu5",
                    ("slurmctld=23.11.4-1.2ubuntu5"
                     if config["cluster"]["manager"] == "slurm" else ""),
                    ("slurmd=23.11.4-1.2ubuntu5"
                     if config["cluster"]["manager"] == "slurm" else ""),
                ],
                "uib_stack_downloads": [
                    {
                        "name": "enroot_4.2.1-1_amd64.deb",
                        "url": "https://github.com/NVIDIA/enroot/releases/download/v4.2.1/enroot_4.2.1-1_amd64.deb",
                        "sha256": "5955c51e88df0c5ed538d87abb5264609d4bad55fa8570745ed5d4ec66c8ab91",
                    },
                    {
                        "name": "enroot+caps_4.2.1-1_amd64.deb",
                        "url": "https://github.com/NVIDIA/enroot/releases/download/v4.2.1/enroot+caps_4.2.1-1_amd64.deb",
                        "sha256": "ca22652dcbdd3ebb6e2c68df781ef4301647c79dd7a41ed62899c2fd0f692750",
                    },
                    {
                        "name": "pyxis-0.24.0.tar.gz",
                        "url": "https://codeload.github.com/NVIDIA/pyxis/tar.gz/107519944221822ea1dace4db8e7234b2eaa4cd5",
                        "sha256": "32d0a56d506a3876dd59202e6a209fefae94af9803f4dd8b542e11ef2ead7907",
                    },
                ],
            }),
        ], log)
        ledger["state"] = "ready"
        ledger["baseImage"] = str(image)
        write_json(ledger_path, ledger)
    except (LibvirtError, OSError) as error:
        ledger["state"] = "failed"
        ledger["error"] = str(error)
        write_json(ledger_path, ledger)
        if apply_started and not args.retain:
            try:
                destroy(state, cache, log)
                ledger["cleanup"] = "destroyed"
                write_json(ledger_path, ledger)
            except LibvirtError as cleanup_error:
                ledger["cleanup"] = "failed"
                ledger["cleanupError"] = str(cleanup_error)
                write_json(ledger_path, ledger)
        raise
    print(state)


def command_status(args):
    from .core import load_ledger, validate_name

    validate_name(args.name)
    state = pathlib.Path(args.state_dir).expanduser().resolve()
    unused, ledger = load_ledger(state, args.name)
    report = {
        "runId": args.name,
        "provider": "libvirt",
        "state": ledger["state"],
    }
    print(json.dumps(report, indent=2, sort_keys=True))
    return 0 if ledger["state"] == "ready" else 1


def command_down(args):
    from .core import load_ledger, validate_name, write_json
    from .tofu import destroy

    validate_name(args.name)
    state = pathlib.Path(args.state_dir).expanduser().resolve()
    cache = pathlib.Path(args.cache_dir).expanduser().resolve()
    ledger_path, ledger = load_ledger(state, args.name)
    if ledger.get("state") == "destroyed" or ledger.get("cleanup") == "destroyed":
        return
    log = state / "lifecycle.log"
    destroy(state, cache, log)
    ledger["state"] = "destroyed"
    write_json(ledger_path, ledger)
