# SPDX-License-Identifier: Apache-2.0

import copy
import json
import jsonschema
import pathlib
import re
import yaml


SIMULATION = pathlib.Path(__file__).resolve().parents[3]


TRAINING_SCHEMA = SIMULATION / "training" / "workload.schema.json"


INFERENCE_SCHEMA = SIMULATION / "inference" / "workload.schema.json"


NAME = re.compile(r"^[a-z](?:[a-z0-9-]{1,46}[a-z0-9])$")


DIGEST = re.compile(r"^\S+@sha256:[0-9a-f]{64}$")


IMAGE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._/@:+-]*$")


class RenderError(Exception):
    pass


class LiteralString(str):
    pass


def literal_string(dumper, value):
    return dumper.represent_scalar("tag:yaml.org,2002:str", value, style="|")


yaml.SafeDumper.add_representer(LiteralString, literal_string)


def load_config(path):
    try:
        with pathlib.Path(path).open(encoding="utf-8") as source:
            config = json.load(source)
    except (OSError, json.JSONDecodeError) as error:
        raise RenderError(f"cannot load {path}: {error}") from error
    if not isinstance(config, dict):
        raise RenderError("resolved configuration must be an object")
    return config


def validate_image(reference, allow_mutable_tags):
    if not isinstance(reference, str) or not IMAGE.fullmatch(reference):
        raise RenderError(f"invalid image reference: {reference}")
    if not DIGEST.fullmatch(reference) and not allow_mutable_tags:
        raise RenderError(
            f"image reference is not digest-pinned: {reference}; "
            "use --allow-mutable-tags only for local development images"
        )


def validate_common(config, run_id, allow_mutable_tags):
    if not NAME.fullmatch(run_id):
        raise RenderError(
            "run ID must start with a letter, contain 3-48 lowercase "
            "letters, digits, or hyphens, and end with a letter or digit"
        )
    if config.get("cluster", {}).get("manager") != "kubernetes":
        raise RenderError("the Kubernetes adapter requires cluster.manager=kubernetes")

    workload = config.get("workload")
    if not workload:
        raise RenderError("the Kubernetes adapter requires a workload")
    validate_image(workload["image"], allow_mutable_tags)
    uib = config["uib"]
    if uib["enabled"]:
        bundle = uib.get("bundle")
        if not bundle:
            raise RenderError("uib.bundle is required when UIB is enabled")
        validate_image(bundle, allow_mutable_tags)


def validate_hca(config):
    collective = config["communication"][config["communication"]["frontend"]]
    if config["uib"]["enabled"] and collective["hca"] != "=uib0":
        raise RenderError("UIB rendering requires the exact HCA filter =uib0")


def validate_config(config, run_id, allow_mutable_tags):
    validate_common(config, run_id, allow_mutable_tags)

    workload = config.get("workload")
    kind = workload.get("kind")
    if kind == "inference":
        validate_inference_config(config)
        return
    if kind != "training":
        raise RenderError(f"unsupported Kubernetes workload kind: {kind}")
    if config.get("launcher", {}).get("training") != "kubeflow":
        raise RenderError("training rendering requires launcher.training=kubeflow")
    parallelism = workload["parallelism"]
    if parallelism["processesPerNode"] != 1:
        raise RenderError(
            "Kubeflow UIB rendering currently requires processesPerNode=1; "
            "multiple local ranks need rank-aware UIB bind ports"
        )
    if config["communication"]["frontend"] != "nccl":
        raise RenderError("the current training image supports only NCCL")
    validate_hca(config)
    if config["communication"]["transportAllowlist"] != ["IB"]:
        raise RenderError("NCCL rendering requires transportAllowlist=[IB]")

    parameters = workload.get("parameters", {})
    training = parameters.get("config")
    if not isinstance(training, dict):
        raise RenderError("workload.parameters.config must contain training JSON")
    with TRAINING_SCHEMA.open(encoding="utf-8") as source:
        schema = json.load(source)
    errors = sorted(
        jsonschema.Draft202012Validator(schema).iter_errors(training),
        key=lambda error: list(error.absolute_path),
    )
    if errors:
        error = errors[0]
        path = ".".join(str(part) for part in error.absolute_path)
        raise RenderError(
            f"invalid workload.parameters.config at {path or '<root>'}: "
            f"{error.message}"
        )


def validate_inference_config(config):
    if config.get("launcher", {}).get("inference") != "kuberay":
        raise RenderError("inference rendering requires launcher.inference=kuberay")
    workload = config["workload"]
    if workload["parallelism"]["processesPerNode"] != 1:
        raise RenderError(
            "KubeRay UIB rendering currently requires processesPerNode=1; "
            "multiple local ranks need rank-aware UIB bind ports"
        )
    if config["communication"]["frontend"] != "nccl":
        raise RenderError("the current inference image supports only NCCL")
    validate_hca(config)
    if config["communication"]["transportAllowlist"] != ["IB"]:
        raise RenderError("NCCL rendering requires transportAllowlist=[IB]")
    parameters = workload.get("parameters", {})
    inference = parameters.get("config")
    if not isinstance(inference, dict):
        raise RenderError("workload.parameters.config must contain inference JSON")
    with INFERENCE_SCHEMA.open(encoding="utf-8") as source:
        schema = json.load(source)
    errors = sorted(
        jsonschema.Draft202012Validator(schema).iter_errors(inference),
        key=lambda error: list(error.absolute_path),
    )
    if errors:
        error = errors[0]
        path = ".".join(str(part) for part in error.absolute_path)
        raise RenderError(
            f"invalid workload.parameters.config at {path or '<root>'}: "
            f"{error.message}"
        )
    server = inference["server"]
    nodes = workload["parallelism"]["nodes"]
    if inference["backend"] == "transformers" and nodes != 1:
        raise RenderError("the CPU transformers inference request requires one node")
    if inference["backend"] == "vllm":
        ranks = nodes * workload["parallelism"]["processesPerNode"]
        degrees = (
            server["tensorParallel"]
            * server["pipelineParallel"]
            * server["dataParallel"]
        )
        if ranks != degrees:
            raise RenderError(
                "vLLM TP, PP, and DP degrees must multiply to total ranks"
            )
        if nodes > 1 and server["distributedExecutor"] != "ray":
            raise RenderError("multi-node vLLM requires distributedExecutor=ray")
        gpus = config["nodes"]["gpus"]
        if gpus["vendor"] != "nvidia" or gpus["perWorker"] < 1:
            raise RenderError("the current vLLM image requires NVIDIA GPU resources")


def string_value(value):
    if isinstance(value, bool):
        return "true" if value else "false"
    return str(value)


def labels(run_id):
    return {
        "app.kubernetes.io/instance": run_id,
        "app.kubernetes.io/name": "uib-simulation",
        "uib.dev/run-id": run_id,
    }


def resources(config):
    nodes = config["nodes"]
    values = {
        "cpu": string_value(nodes["cpu"]),
        "memory": nodes["memory"],
    }
    gpu = nodes["gpus"]
    if gpu["vendor"] != "none":
        resource_name = gpu.get("resourceName")
        if not resource_name:
            raise RenderError("GPU workloads require nodes.gpus.resourceName")
        values[resource_name] = string_value(gpu["perWorker"])
    return {"limits": copy.deepcopy(values), "requests": values}
