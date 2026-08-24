# SPDX-License-Identifier: Apache-2.0

import copy
import json
import pathlib

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
        config["uib"]["congestionControl"] = copy.deepcopy(
            protocol["congestionControl"])
        return config
    if name.startswith("network/"):
        config["network"]["shaping"] = copy.deepcopy(
            overlay["network"]["shaping"])
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


def validate_scheduler(config):
    from .core import COMPATIBILITY, ConfigurationError
    provider = config["infrastructure"]["provider"]
    manager = config["cluster"]["manager"]
    runtime = config["container"]["runtime"]
    frontend = config["communication"]["frontend"]
    gpu = config["nodes"]["gpus"]
    uib = config["uib"]
    if provider == "kind" and manager != "kubernetes":
        raise ConfigurationError("Kind requires Kubernetes")
    if manager == "kubernetes" and runtime != "containerd":
        raise ConfigurationError("Kubernetes requires containerd")
    if manager == "slurm" and runtime == "containerd":
        raise ConfigurationError("Slurm requires Enroot or Apptainer")



def validate_communication(config):
    from .core import COMPATIBILITY, ConfigurationError
    provider = config["infrastructure"]["provider"]
    manager = config["cluster"]["manager"]
    runtime = config["container"]["runtime"]
    frontend = config["communication"]["frontend"]
    gpu = config["nodes"]["gpus"]
    uib = config["uib"]
    if frontend == "rccl" and gpu["vendor"] != "amd":
        raise ConfigurationError("RCCL requires AMD GPUs")
    if frontend == "nccl" and gpu["vendor"] == "amd":
        raise ConfigurationError("NCCL cannot use the AMD GPU profile")
    if uib["memoryMode"] == "gpu-direct":
        with COMPATIBILITY.open(encoding="utf-8") as source:
            lock = json.load(source)
        if lock["limitations"]["gpuDirect"] != "validated":
            raise ConfigurationError("GPU-direct is not validated")
    cc = uib["congestionControl"]
    if uib["protocol"] == "rocev2" and cc["algorithm"] != "none":
        raise ConfigurationError("RoCEv2 congestion plug-ins are not implemented")
    if provider == "existing" and config["network"]["shaping"]["enabled"]:
        if not config["infrastructure"]["allowExistingMutations"]:
            raise ConfigurationError(
                "shaping an existing cluster requires allowExistingMutations=true")
    shaping = config["network"]["shaping"]
    impairments = set(shaping) - {"enabled"}
    if not shaping["enabled"] and impairments:
        raise ConfigurationError("network shaping parameters require enabled=true")
    if "jitter" in shaping and "latency" not in shaping:
        raise ConfigurationError("network jitter requires latency")
    if "delayDistribution" in shaping and "jitter" not in shaping:
        raise ConfigurationError("delay distribution requires jitter")
    if shaping.get("reorderPercent", 0) and "latency" not in shaping:
        raise ConfigurationError("packet reordering requires latency")
    if shaping.get("ecn", False) and not shaping.get("lossPercent", 0):
        raise ConfigurationError("netem ECN requires a nonzero loss model")
    correlation_requirements = {
        "delayCorrelationPercent": "jitter",
        "lossCorrelationPercent": "lossPercent",
        "duplicateCorrelationPercent": "duplicatePercent",
        "corruptCorrelationPercent": "corruptPercent",
        "reorderCorrelationPercent": "reorderPercent",
    }
    for correlation, parent in correlation_requirements.items():
        if correlation in shaping and parent not in shaping:
            raise ConfigurationError(f"{correlation} requires {parent}")
    if "cellSizeBytes" in shaping and "packetOverheadBytes" not in shaping:
        raise ConfigurationError("cellSizeBytes requires packetOverheadBytes")
    if "cellOverheadBytes" in shaping and "cellSizeBytes" not in shaping:
        raise ConfigurationError("cellOverheadBytes requires cellSizeBytes")



def validate_workload(config):
    from .core import COMPATIBILITY, ConfigurationError
    provider = config["infrastructure"]["provider"]
    manager = config["cluster"]["manager"]
    runtime = config["container"]["runtime"]
    frontend = config["communication"]["frontend"]
    gpu = config["nodes"]["gpus"]
    uib = config["uib"]
    workload = config.get("workload")
    if workload:
        parallel = workload["parallelism"]
        ranks = parallel["nodes"] * parallel["processesPerNode"]
        degrees = (parallel["data"] * parallel["tensor"] *
                   parallel["pipeline"] * parallel["expert"])
        if parallel["nodes"] > config["nodes"]["workers"]:
            raise ConfigurationError("workload nodes exceed profile workers")
        if ranks != degrees:
            raise ConfigurationError(
                "parallel degrees must multiply to the total rank count")



def validate_semantics(config):
    validate_scheduler(config)
    validate_communication(config)
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
    tools.update({
        "nccl": {"all_reduce_perf", "mpirun"},
        "rccl": {"all_reduce_perf", "mpirun", "rocminfo"},
        "ucx": {"ucx_info", "ucx_perftest"},
        "libfabric": {"fi_info", "fi_pingpong", "fi_rdm_pingpong"},
    }[frontend])
    return sorted(tools)
