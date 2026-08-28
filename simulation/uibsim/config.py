# SPDX-License-Identifier: Apache-2.0

import copy
import ipaddress
import json
import pathlib
import re

from .dependencies import jsonschema, yaml


def load(path):
    from .core import ConfigurationError

    try:
        with path.open(encoding="utf-8") as source:
            value = yaml.safe_load(source)
    except (OSError, yaml.YAMLError) as error:
        raise ConfigurationError(f"cannot load {path}: {error}") from error
    if not isinstance(value, dict):
        raise ConfigurationError(f"{path} must contain a mapping")
    return value


def merge(base, overlay):
    result = copy.deepcopy(base)
    for key, value in overlay.items():
        if isinstance(value, dict) and isinstance(result.get(key), dict):
            result[key] = merge(result[key], value)
        else:
            result[key] = copy.deepcopy(value)
    return result


def find_profile(name):
    from .core import ConfigurationError, PROFILES

    supplied = pathlib.Path(name)
    if supplied.is_file():
        return supplied.resolve()
    candidate = PROFILES / f"{name}.yaml"
    if candidate.is_file():
        return candidate
    raise ConfigurationError(f"unknown profile: {name}")


def find_overlay(name):
    from .core import ConfigurationError, OVERLAYS

    supplied = pathlib.Path(name)
    if supplied.is_file():
        return supplied.resolve()
    candidate = OVERLAYS / f"{name}.yaml"
    if candidate.is_file():
        return candidate
    raise ConfigurationError(f"unknown overlay: {name}")


def apply_override(config, assignment):
    from .core import ConfigurationError

    if "=" not in assignment:
        raise ConfigurationError(f"override lacks '=': {assignment}")
    dotted, raw = assignment.split("=", 1)
    keys = dotted.split(".")
    cursor = config
    for key in keys[:-1]:
        if not key or not isinstance(cursor.get(key), dict):
            raise ConfigurationError(f"unknown override path: {dotted}")
        cursor = cursor[key]
    if not keys[-1]:
        raise ConfigurationError(f"unknown override path: {dotted}")
    cursor[keys[-1]] = yaml.safe_load(raw)


def apply_overlay(config, name):
    overlay = load(find_overlay(name))

    if name.startswith("communication/"):
        config["communication"] = copy.deepcopy(overlay["communication"])
        return config
    if name.startswith("protocol/"):
        protocol = overlay["uib"]
        config["uib"]["protocol"] = protocol["protocol"]
        config["uib"]["bindPort"] = protocol["bindPort"]
        config["uib"]["congestionControl"] = copy.deepcopy(
            protocol["congestionControl"])
        config["uib"].pop("environment", None)
        if "environment" in protocol:
            config["uib"]["environment"] = copy.deepcopy(
                protocol["environment"])
        return config
    if name.startswith("network/"):
        config["network"]["shaping"] = copy.deepcopy(
            overlay["network"]["shaping"])
        if "mtu" in overlay["network"]:
            config["network"]["mtu"] = overlay["network"]["mtu"]
        config["network"].pop("endpointShaping", None)
        if "endpointShaping" in overlay["network"]:
            config["network"]["endpointShaping"] = copy.deepcopy(
                overlay["network"]["endpointShaping"])
        return config
    return merge(config, overlay)


def validate_schema(config):
    from .core import ConfigurationError, SCHEMA

    with SCHEMA.open(encoding="utf-8") as source:
        schema = json.load(source)
    errors = sorted(jsonschema.Draft202012Validator(schema).iter_errors(config),
                    key=lambda error: list(error.absolute_path))
    if not errors:
        return
    messages = []
    for error in errors:
        path = ".".join(str(part) for part in error.absolute_path)
        messages.append(f"{path or '<root>'}: {error.message}")
    raise ConfigurationError("schema validation failed:\n  " +
                             "\n  ".join(messages))


def quantity_mib(value):
    from .core import ConfigurationError

    match = re.fullmatch(r"([1-9][0-9]*)(Mi|Gi|Ti)", value)
    if not match:
        raise ConfigurationError(f"invalid memory quantity: {value}")
    factor = {"Mi": 1, "Gi": 1024, "Ti": 1024 ** 2}[match.group(2)]
    return int(match.group(1)) * factor


def validate_shaping(shaping, path):
    from .core import ConfigurationError

    impairments = set(shaping) - {"enabled"}
    if not shaping["enabled"] and impairments:
        raise ConfigurationError(
            f"{path} parameters require enabled=true")
    if "jitter" in shaping and "latency" not in shaping:
        raise ConfigurationError(f"{path} jitter requires latency")
    if "delayDistribution" in shaping and "jitter" not in shaping:
        raise ConfigurationError(f"{path} delay distribution requires jitter")
    if shaping.get("reorderPercent", 0) and "latency" not in shaping:
        raise ConfigurationError(f"{path} packet reordering requires latency")
    if shaping.get("ecn", False) and not shaping.get("lossPercent", 0):
        raise ConfigurationError(f"{path} netem ECN requires a nonzero loss model")
    loss_model = shaping.get("lossModel", "random")
    burst_parameters = {
        "lossRecoveryPercent",
        "lossBadStatePercent",
        "lossGoodStatePercent",
    }
    configured_burst = burst_parameters & set(shaping)
    if loss_model == "gemodel":
        if not shaping.get("lossPercent", 0):
            raise ConfigurationError(
                f"{path} netem gemodel requires a nonzero lossPercent")
        missing = burst_parameters - configured_burst
        if missing:
            raise ConfigurationError(
                f"{path} netem gemodel requires {sorted(missing)[0]}")
        if "lossCorrelationPercent" in shaping:
            raise ConfigurationError(
                f"{path} lossCorrelationPercent applies only to random loss")
    elif configured_burst:
        raise ConfigurationError(
            f"{path} burst-loss parameters require lossModel=gemodel")
    correlation_requirements = {
        "delayCorrelationPercent": "jitter",
        "lossCorrelationPercent": "lossPercent",
        "duplicateCorrelationPercent": "duplicatePercent",
        "corruptCorrelationPercent": "corruptPercent",
        "reorderCorrelationPercent": "reorderPercent",
    }
    for correlation, parent in correlation_requirements.items():
        if correlation in shaping and parent not in shaping:
            raise ConfigurationError(f"{path}.{correlation} requires {parent}")
    if "cellSizeBytes" in shaping and "packetOverheadBytes" not in shaping:
        raise ConfigurationError(
            f"{path}.cellSizeBytes requires packetOverheadBytes")
    if "cellOverheadBytes" in shaping and "cellSizeBytes" not in shaping:
        raise ConfigurationError(
            f"{path}.cellOverheadBytes requires cellSizeBytes")


def validate_unsigned_parameter(parameters, name, maximum=(1 << 64) - 1):
    from .core import ConfigurationError

    value = parameters.get(name)

    if value is None:
        return
    if (isinstance(value, bool) or not isinstance(value, int) or
            value < 1 or value > maximum):
        raise ConfigurationError(
            f"congestion-control {name} must be an integer from 1 to {maximum}")


def validate_congestion(config):
    from .core import ConfigurationError

    uib = config["uib"]
    algorithm = uib["congestionControl"]["algorithm"]
    parameters = uib["congestionControl"]["parameters"]
    allowed = {
        "nscc": {"maxWindowBytes"},
        "dcqcn": {
            "initialRateBytesPerSecond",
            "minimumRateBytesPerSecond",
            "maximumRateBytesPerSecond",
            "additiveIncreaseBytesPerSecond",
            "recoveryIntervalNs",
            "alphaGainQ16",
        },
        "rccc": {
            "maxWindowBytes",
            "creditQuantumBytes",
            "linkRateBytesPerSecond",
            "baseRttNs",
        },
    }
    names = allowed.get(algorithm, set())
    unknown = set(parameters) - names

    if unknown:
        raise ConfigurationError(
            f"unknown {algorithm} parameter: {sorted(unknown)[0]}")
    for name in names:
        validate_unsigned_parameter(parameters, name)
    if algorithm == "nscc":
        validate_unsigned_parameter(parameters, "maxWindowBytes", 4294967295)
    elif algorithm == "dcqcn":
        experimental = uib.get("environment", {}).get(
            "UIB_UET_CC_EXPERIMENTAL", "")
        if uib["protocol"] == "uet" and str(experimental).lower() != "on":
            raise ConfigurationError(
                "DCQCN with UET requires UIB_UET_CC_EXPERIMENTAL=on")
        minimum = parameters.get("minimumRateBytesPerSecond", 12500000)
        initial = parameters.get("initialRateBytesPerSecond", 12500000000)
        maximum = parameters.get("maximumRateBytesPerSecond", 12500000000)
        alpha = parameters.get("alphaGainQ16", 256)
        if minimum > initial or initial > maximum:
            raise ConfigurationError(
                "DCQCN rates require minimum <= initial <= maximum")
        if alpha > 65536:
            raise ConfigurationError(
                "DCQCN alphaGainQ16 must be an integer from 1 to 65536")
    elif algorithm == "rccc":
        if uib["protocol"] != "uet":
            raise ConfigurationError("RCCC is available only with UET")
    elif parameters:
        raise ConfigurationError(
            "registered congestion-control algorithms do not accept "
            "parameters through the common schema")


def validate_infrastructure(config):
    from .core import COMPATIBILITY, ConfigurationError
    provider = config["infrastructure"]["provider"]
    manager = config["cluster"]["manager"]
    runtime = config["container"]["runtime"]
    frontend = config["communication"]["frontend"]
    gpu = config["nodes"]["gpus"]
    uib = config["uib"]
    storage = config["storage"]
    endpoint_shaping = config["network"].get("endpointShaping", {})
    expected_targets = {
        f"worker-{index:02d}/data"
        for index in range(1, config["nodes"]["workers"] + 1)
    }
    if provider == "libvirt" and "libvirt" not in config:
        raise ConfigurationError("libvirt requires provider settings")
    if provider != "libvirt" and "libvirt" in config:
        raise ConfigurationError(
            "libvirt settings require infrastructure.provider=libvirt")
    if provider == "virtme" and "virtme" not in config:
        raise ConfigurationError("virtme requires provider settings")
    if provider != "virtme" and "virtme" in config:
        raise ConfigurationError(
            "virtme settings require infrastructure.provider=virtme")
    if provider == "virtme":
        if config["nodes"]["controlPlane"] != 1:
            raise ConfigurationError(
                "virtme currently requires one control plane")
        ssh_port_base = config["virtme"]["sshPortBase"]
        last_port = ssh_port_base + config["nodes"]["workers"]
        if config["cluster"]["manager"] == "kubernetes":
            last_port += 1
        if last_port > 65535:
            raise ConfigurationError(
                "virtme forwarded port range exceeds 65535")
        if (config["virtme"]["mode"] == "multi-guest" and
                not config["network"].get("dataInterface")):
            raise ConfigurationError(
                "multi-guest virtme requires network.dataInterface")
        if config["virtme"]["mode"] == "multi-guest":
            expected_interface = (
                "eth1" if config["virtme"]["networkMode"] == "socket"
                else "eth0")
            if config["network"]["dataInterface"] != expected_interface:
                raise ConfigurationError(
                    f"virtme {config['virtme']['networkMode']} networking "
                    f"requires network.dataInterface={expected_interface}")
    if provider == "libvirt" and config["nodes"]["controlPlane"] != 1:
        raise ConfigurationError("libvirt currently requires one control plane")
    if manager == "kubernetes":
        if "kubernetes" in config:
            kubernetes = config["kubernetes"]
            if kubernetes["distribution"] != "k3s":
                raise ConfigurationError(
                    "only the k3s Kubernetes distribution is implemented")
            networks = {
                "management": config["network"]["managementCIDR"],
                "data": config["network"]["dataCIDR"],
                "pods": kubernetes["podCIDR"],
                "services": kubernetes["serviceCIDR"],
            }
            try:
                parsed = {name: ipaddress.ip_network(value, strict=True)
                          for name, value in networks.items()}
            except ValueError as error:
                raise ConfigurationError(
                    f"invalid Kubernetes network: {error}") from error
            for name, network in parsed.items():
                for other_name, other in parsed.items():
                    if name < other_name and network.overlaps(other):
                        raise ConfigurationError(
                            f"Kubernetes {name} and {other_name} networks "
                            "overlap")
        elif (provider == "libvirt" or
              (provider == "virtme" and
               config["virtme"]["mode"] == "multi-guest")):
            raise ConfigurationError(
                f"{provider} Kubernetes requires distribution settings")
        if (provider == "libvirt" and not re.fullmatch(
                r"v[0-9]+\.[0-9]+\.[0-9]+\+k3s[0-9]+",
                config["cluster"]["version"])):
            raise ConfigurationError(
                "libvirt k3s requires an exact vX.Y.Z+k3sN version")
        if (provider == "libvirt" and
                config["libvirt"]["networkMode"] != "bridge"):
            raise ConfigurationError(
                "libvirt Kubernetes currently requires bridge networking")
    elif "kubernetes" in config:
        raise ConfigurationError(
            "Kubernetes settings require cluster.manager=kubernetes")
    if provider == "libvirt":
        ssh_port_base = config["libvirt"].get("sshPortBase", 22000)
        if ssh_port_base + config["nodes"]["workers"] > 65535:
            raise ConfigurationError(
                "libvirt SSH port range exceeds 65535")
        usable_memory = quantity_mib(config["nodes"]["memory"]) - 512
        if usable_memory < 1:
            raise ConfigurationError(
                "libvirt node memory must exceed the 512Mi system reserve")
        requested_memory = config.get("slurm", {}).get("memory")
        if (requested_memory and
                quantity_mib(requested_memory) > usable_memory):
            raise ConfigurationError(
                "slurm.memory exceeds libvirt scheduler memory after the "
                "512Mi system reserve")
    if (provider == "libvirt" and
            config["libvirt"]["networkMode"] == "rootless" and
            config["libvirt"]["uri"] != "qemu:///session"):
        raise ConfigurationError(
            "libvirt rootless mode requires qemu:///session")
    if (provider == "libvirt" and manager == "slurm" and
            config["cluster"]["version"] != "23.11.4"):
        raise ConfigurationError(
            "libvirt Slurm provisioning requires version 23.11.4")



def validate_scheduler(config):
    from .core import COMPATIBILITY, ConfigurationError
    provider = config["infrastructure"]["provider"]
    manager = config["cluster"]["manager"]
    runtime = config["container"]["runtime"]
    frontend = config["communication"]["frontend"]
    gpu = config["nodes"]["gpus"]
    uib = config["uib"]
    storage = config["storage"]
    endpoint_shaping = config["network"].get("endpointShaping", {})
    expected_targets = {
        f"worker-{index:02d}/data"
        for index in range(1, config["nodes"]["workers"] + 1)
    }
    if provider == "kind" and manager != "kubernetes":
        raise ConfigurationError("Kind requires Kubernetes")
    if manager == "kubernetes" and runtime != "containerd":
        raise ConfigurationError("Kubernetes requires containerd")
    if manager == "slurm" and runtime == "containerd":
        raise ConfigurationError("Slurm requires Enroot or Apptainer")
    if manager == "slurm" and "slurm" not in config:
        raise ConfigurationError("Slurm requires scheduler settings")
    if manager != "slurm" and "slurm" in config:
        raise ConfigurationError("Slurm settings require cluster.manager=slurm")
    if config["container"]["integration"] == "pyxis":
        if manager != "slurm" or runtime != "enroot":
            raise ConfigurationError("Pyxis requires Slurm with Enroot")
    kubernetes_storage = {"modelCacheClaim", "modelCacheImage", "resultsClaim"}
    slurm_storage = {"uibRuntimePath", "modelCachePath", "resultsPath"}
    if manager == "kubernetes" and slurm_storage & set(storage):
        raise ConfigurationError("host storage paths require Slurm")
    if manager == "slurm":
        if kubernetes_storage & set(storage):
            raise ConfigurationError(
                "Kubernetes storage settings cannot be used with Slurm")
        for name in ("modelCachePath", "resultsPath"):
            if name not in storage:
                raise ConfigurationError(f"Slurm requires storage.{name}")
        if uib["enabled"] and "uibRuntimePath" not in storage:
            raise ConfigurationError("Slurm UIB runs require storage.uibRuntimePath")
        if uib["enabled"] and not config["network"].get("dataInterface"):
            raise ConfigurationError("Slurm UIB runs require network.dataInterface")
    if provider == "kind" and endpoint_shaping:
        raise ConfigurationError(
            "per-worker shaping is not implemented for Kind")
    if provider == "kind" and config["network"]["mtu"] != 1500:
        raise ConfigurationError(
            "custom link MTUs are not implemented for Kind")
    expected_targets = {
        f"worker-{index:02d}/data"
        for index in range(1, config["nodes"]["workers"] + 1)
    }
    unknown_targets = set(endpoint_shaping) - expected_targets
    if unknown_targets:
        raise ConfigurationError(
            "network.endpointShaping has unknown target: "
            f"{sorted(unknown_targets)[0]}")
    for target, shaping in endpoint_shaping.items():
        validate_shaping(shaping, f"network.endpointShaping.{target}")



def validate_communication(config):
    from .core import COMPATIBILITY, ConfigurationError
    provider = config["infrastructure"]["provider"]
    manager = config["cluster"]["manager"]
    runtime = config["container"]["runtime"]
    frontend = config["communication"]["frontend"]
    gpu = config["nodes"]["gpus"]
    uib = config["uib"]
    storage = config["storage"]
    endpoint_shaping = config["network"].get("endpointShaping", {})
    expected_targets = {
        f"worker-{index:02d}/data"
        for index in range(1, config["nodes"]["workers"] + 1)
    }
    if frontend == "rccl" and gpu["vendor"] != "amd":
        raise ConfigurationError("RCCL requires AMD GPUs")
    if frontend == "nccl" and gpu["vendor"] == "amd":
        raise ConfigurationError("NCCL cannot use the AMD GPU profile")
    if uib["memoryMode"] == "gpu-direct":
        with COMPATIBILITY.open(encoding="utf-8") as source:
            lock = json.load(source)
        if lock["limitations"]["gpuDirect"] != "validated":
            raise ConfigurationError("GPU-direct is not validated")
    validate_congestion(config)



def validate_network(config):
    from .core import COMPATIBILITY, ConfigurationError
    provider = config["infrastructure"]["provider"]
    manager = config["cluster"]["manager"]
    runtime = config["container"]["runtime"]
    frontend = config["communication"]["frontend"]
    gpu = config["nodes"]["gpus"]
    uib = config["uib"]
    storage = config["storage"]
    endpoint_shaping = config["network"].get("endpointShaping", {})
    expected_targets = {
        f"worker-{index:02d}/data"
        for index in range(1, config["nodes"]["workers"] + 1)
    }
    shaping_values = [config["network"]["shaping"],
                      *config["network"].get("endpointShaping", {}).values()]
    if provider == "existing" and any(
            shaping["enabled"] for shaping in shaping_values):
        if not config["infrastructure"]["allowExistingMutations"]:
            raise ConfigurationError(
                "shaping an existing cluster requires allowExistingMutations=true")
    validate_shaping(config["network"]["shaping"], "network.shaping")
    endpoint_shaping = config["network"].get("endpointShaping", {})



def validate_faults(config):
    from .core import COMPATIBILITY, ConfigurationError
    provider = config["infrastructure"]["provider"]
    manager = config["cluster"]["manager"]
    runtime = config["container"]["runtime"]
    frontend = config["communication"]["frontend"]
    gpu = config["nodes"]["gpus"]
    uib = config["uib"]
    storage = config["storage"]
    endpoint_shaping = config["network"].get("endpointShaping", {})
    expected_targets = {
        f"worker-{index:02d}/data"
        for index in range(1, config["nodes"]["workers"] + 1)
    }
    action_fields = {
        "add-delay": {"target", "latency", "duration"},
        "packet-loss": {"target", "percent", "duration"},
        "link-down": {"target", "duration"},
        "partition": {"target", "duration"},
        "terminate-rank": {"rank"},
    }
    optional_fields = {"at", "afterEvent", "action"}
    for index, fault in enumerate(config.get("faults", [])):
        action = fault["action"]
        required = action_fields[action]
        missing = required - set(fault)
        if missing:
            raise ConfigurationError(
                f"faults[{index}] {action} requires {sorted(missing)[0]}")
        unexpected = set(fault) - required - optional_fields
        if unexpected:
            raise ConfigurationError(
                f"faults[{index}] {action} does not accept "
                f"{sorted(unexpected)[0]}")
        if "duration" in fault and re.fullmatch(
                r"0+(?:ms|s|m|h)", fault["duration"]):
            raise ConfigurationError(
                f"faults[{index}] duration must be nonzero")
        if "latency" in fault and re.fullmatch(
                r"0+(?:ns|us|ms|s)", fault["latency"]):
            raise ConfigurationError(
                f"faults[{index}] latency must be nonzero")
        if action == "terminate-rank":
            workload = config.get("workload")
            parallel = workload["parallelism"] if workload else None
            if (parallel and fault["rank"] >=
                    (parallel["nodes"] * parallel["processesPerNode"])):
                raise ConfigurationError(
                    f"faults[{index}] rank is outside the workload")
        if "percent" in fault and fault["percent"] <= 0:
            raise ConfigurationError(
                f"faults[{index}] percent must be greater than zero")



def validate_workload(config):
    from .core import COMPATIBILITY, ConfigurationError
    provider = config["infrastructure"]["provider"]
    manager = config["cluster"]["manager"]
    runtime = config["container"]["runtime"]
    frontend = config["communication"]["frontend"]
    gpu = config["nodes"]["gpus"]
    uib = config["uib"]
    storage = config["storage"]
    endpoint_shaping = config["network"].get("endpointShaping", {})
    expected_targets = {
        f"worker-{index:02d}/data"
        for index in range(1, config["nodes"]["workers"] + 1)
    }
    workload = config.get("workload")
    if workload:
        parallel = workload["parallelism"]
        ranks = parallel["nodes"] * parallel["processesPerNode"]
        degrees = (parallel["data"] * parallel["tensor"] *
                   parallel["pipeline"] * parallel["expert"])
        if parallel["nodes"] > config["nodes"]["workers"]:
            raise ConfigurationError("workload nodes exceed profile workers")
        resources = workload.get("resources", config["nodes"])
        if resources["cpu"] > config["nodes"]["cpu"]:
            raise ConfigurationError(
                "workload.resources.cpu exceeds node CPU capacity")
        if quantity_mib(resources["memory"]) > quantity_mib(
                config["nodes"]["memory"]):
            raise ConfigurationError(
                "workload.resources.memory exceeds node memory capacity")
        if resources["cpu"] < parallel["processesPerNode"]:
            raise ConfigurationError(
                "workload resources provide less than one CPU per process")
        if resources["cpu"] % parallel["processesPerNode"]:
            raise ConfigurationError(
                "workload CPU resources must divide evenly among processes")
        if ranks != degrees:
            raise ConfigurationError(
                "parallel degrees must multiply to the total rank count")



def validate_semantics(config):
    validate_infrastructure(config)
    validate_scheduler(config)
    validate_communication(config)
    validate_network(config)
    validate_faults(config)
    validate_workload(config)


def resolve(args):
    from .core import PROFILES

    config = merge(load(PROFILES / "defaults.yaml"),
                   load(find_profile(args.profile)))
    for overlay in args.overlay:
        config = apply_overlay(config, overlay)
    if args.request:
        config = merge(config, load(pathlib.Path(args.request)))
    for assignment in args.set:
        apply_override(config, assignment)

    frontend = config["communication"]["frontend"]
    for name in ("nccl", "rccl", "ucx", "libfabric"):
        if name != frontend:
            config["communication"].pop(name, None)
    validate_schema(config)
    validate_semantics(config)
    return config


def tools_for(config):
    provider = config["infrastructure"]["provider"]
    manager = config["cluster"]["manager"]
    frontend = config["communication"]["frontend"]
    tools = {"python3"}
    tools.update({
        "kind": {"docker", "kind", "kubectl"},
        "virtme": {"vng"},
        "libvirt": {"ansible-playbook", "tofu", "virsh"},
        "existing": {"kubectl"} if manager == "kubernetes" else {"srun"},
    }[provider])
    if provider == "virtme" and config["virtme"]["mode"] == "multi-guest":
        tools.update({"ip", "ssh", "ssh-keygen"})
        if manager == "slurm":
            tools.update({"apt-get", "cc", "dpkg-deb", "make"})
    if manager == "slurm" and provider in {"libvirt", "virtme"}:
        tools.update({"scp", "ssh"})
    elif manager == "slurm":
        tools.update({"sacct", "sbatch", "scancel", "scontrol", "srun"})
    tools.update({
        "nccl": {"all_reduce_perf", "mpirun"},
        "rccl": {"all_reduce_perf", "mpirun", "rocminfo"},
        "ucx": {"ucx_info", "ucx_perftest"},
        "libfabric": {"fi_info", "fi_rdm_pingpong"},
    }[frontend])
    return sorted(tools)
