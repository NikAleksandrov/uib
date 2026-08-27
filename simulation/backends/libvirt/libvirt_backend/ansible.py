# SPDX-License-Identifier: Apache-2.0

import secrets
import yaml


def common_ansible_values(config, values, state, run_id):
    from .core import SIMULATION, quantity

    manager = config["cluster"]["manager"]
    workers = sorted(
        name for name, node in values["nodes"].items()
        if node["role"] == "worker")
    targets = {
        name: f"worker-{index:02d}/data"
        for index, name in enumerate(workers, 1)
    }
    default_shaping = config["network"]["shaping"]
    endpoint_shaping = config["network"].get("endpointShaping", {})
    return {
        "uib_run_id": run_id,
        "uib_run_state": str(state),
        "uib_config": str(state / "resolved-simulation.json"),
        "uib_netem_source": str(SIMULATION / "network" / "netem"),
        "uib_fault_schedule_source": str(
            SIMULATION / "faults" / "network-schedule"),
        "uib_fault_targets": targets,
        "uib_endpoint_shaping": {
            name: endpoint_shaping.get(target, default_shaping)
            for name, target in targets.items()
        },
        "uib_stage_image_source": str(
            SIMULATION / "adapters" / "slurm" / "stage-image"),
        "uib_stage_cache_source": str(
            SIMULATION / "adapters" / "slurm" /
            "stage-huggingface-cache"),
        "uib_enabled": config["uib"]["enabled"],
        "uib_ssh_user": config["libvirt"]["sshUser"],
        "uib_data_interface": config["network"]["dataInterface"],
        "uib_network_shaping": config["network"]["shaping"],
        "uib_network_mode": config["libvirt"]["networkMode"],
        "uib_node_cpu": config["nodes"]["cpu"],
        "uib_node_memory_mib": quantity(config["nodes"]["memory"], "mib"),
        "uib_cluster_group": f"{manager}_cluster",
        "uib_controller_group": f"{manager}_controller",
        "uib_worker_group": f"{manager}_workers",
        "uib_controller_management_ip": next(
            node["management_ip"] for node in values["nodes"].values()
            if node["role"] == "controller"),
    }


def slurm_ansible_values(config):
    from .core import SIMULATION

    return {
        "uib_manager_packages": [
            "build-essential",
            "git",
            "make",
            "munge",
            "nfs-common",
            "squashfs-tools",
        ],
        "uib_runtime_source": str(
            SIMULATION / ".artifacts" / "uib-runtime"),
        "uib_runtime_path": config["storage"]["uibRuntimePath"],
        "uib_model_cache_path": config["storage"]["modelCachePath"],
        "uib_results_path": config["storage"]["resultsPath"],
        "uib_slurm_partition": config["slurm"]["partition"],
        "uib_slurm_time_limit": config["slurm"]["timeLimit"],
        "uib_diagnostic_image": config["libvirt"]["diagnosticImage"],
        "uib_slurm_packages": [
            "libslurm-dev=23.11.4-1.2ubuntu5",
            "slurm-client=23.11.4-1.2ubuntu5",
            "slurm-wlm-basic-plugins=23.11.4-1.2ubuntu5",
            "slurmctld=23.11.4-1.2ubuntu5",
            "slurmd=23.11.4-1.2ubuntu5",
        ],
        "uib_stack_downloads": [
            {
                "name": "enroot_4.2.1-1_amd64.deb",
                "url": (
                    "https://github.com/NVIDIA/enroot/releases/download/"
                    "v4.2.1/enroot_4.2.1-1_amd64.deb"),
                "sha256": "5955c51e88df0c5ed538d87abb5264609d4bad55fa8570745ed5d4ec66c8ab91",
            },
            {
                "name": "enroot+caps_4.2.1-1_amd64.deb",
                "url": (
                    "https://github.com/NVIDIA/enroot/releases/download/"
                    "v4.2.1/enroot+caps_4.2.1-1_amd64.deb"),
                "sha256": "ca22652dcbdd3ebb6e2c68df781ef4301647c79dd7a41ed62899c2fd0f692750",
            },
            {
                "name": "pyxis-0.24.0.tar.gz",
                "url": (
                    "https://codeload.github.com/NVIDIA/pyxis/tar.gz/"
                    "107519944221822ea1dace4db8e7234b2eaa4cd5"),
                "sha256": "32d0a56d506a3876dd59202e6a209fefae94af9803f4dd8b542e11ef2ead7907",
            },
        ],
    }


def kubernetes_ansible_values(config):
    version = config["cluster"]["version"]
    release = version.replace("+", "%2B")
    base = f"https://github.com/k3s-io/k3s/releases/download/{release}"
    return {
        "uib_k3s_version": version,
        "uib_k3s_url": f"{base}/k3s",
        "uib_k3s_checksum_url": f"{base}/sha256sum-amd64.txt",
        "uib_k3s_token": secrets.token_hex(32),
        "uib_kubernetes_pod_cidr": config["kubernetes"]["podCIDR"],
        "uib_kubernetes_service_cidr": config["kubernetes"]["serviceCIDR"],
        "uib_kubernetes_disable": config["kubernetes"]["disable"],
    }


def finalize_kubeconfig(state, management_ip):
    from .core import LibvirtError

    path = state / "kubeconfig"
    try:
        with path.open(encoding="utf-8") as source:
            config = yaml.safe_load(source)
        clusters = config["clusters"]
        if len(clusters) != 1:
            raise ValueError("expected one cluster")
        clusters[0]["cluster"]["server"] = f"https://{management_ip}:6443"
        with path.open("w", encoding="utf-8") as output:
            yaml.safe_dump(config, output, sort_keys=False)
        path.chmod(0o600)
    except (KeyError, OSError, TypeError, ValueError, yaml.YAMLError) as error:
        raise LibvirtError(
            f"invalid fetched Kubernetes configuration: {error}") from error
    return path
