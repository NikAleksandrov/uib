# SPDX-License-Identifier: Apache-2.0

import json
import jsonschema
import pathlib
import re
import shlex


SIMULATION = pathlib.Path(__file__).resolve().parents[3]


TRAINING_SCHEMA = SIMULATION / "training" / "workload.schema.json"


INFERENCE_SCHEMA = SIMULATION / "inference" / "workload.schema.json"


NAME = re.compile(r"^[a-z](?:[a-z0-9-]{1,46}[a-z0-9])$")


DIGEST = re.compile(r"^\S+@sha256:[0-9a-f]{64}$")


IMAGE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._/@:+-]*$")


SQUASHFS = re.compile(
	r"^/[A-Za-z0-9._+@=-]+(?:/[A-Za-z0-9._+@=-]+)*/"
	r"[0-9a-f]{64}\.sqsh$"
)


class RenderError(Exception):
	pass


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
	if isinstance(reference, str) and SQUASHFS.fullmatch(reference):
		return
	if not isinstance(reference, str) or not IMAGE.fullmatch(reference):
		raise RenderError(f"invalid image reference: {reference}")
	if not DIGEST.fullmatch(reference) and not allow_mutable_tags:
		raise RenderError(
			f"image reference is not digest-pinned: {reference}; "
			"use --allow-mutable-tags only for local development images"
		)


def validate_workload_config(config, schema_path):
	parameters = config["workload"].get("parameters", {})
	workload_config = parameters.get("config")
	if not isinstance(workload_config, dict):
		raise RenderError("workload.parameters.config must contain workload JSON")
	with schema_path.open(encoding="utf-8") as source:
		schema = json.load(source)
	errors = sorted(
		jsonschema.Draft202012Validator(schema).iter_errors(workload_config),
		key=lambda error: list(error.absolute_path),
	)
	if errors:
		error = errors[0]
		path = ".".join(str(part) for part in error.absolute_path)
		raise RenderError(
			f"invalid workload.parameters.config at {path or '<root>'}: "
			f"{error.message}"
		)
	return workload_config


def validate_config(config, run_id, allow_mutable_tags):
	if not NAME.fullmatch(run_id):
		raise RenderError(
			"run ID must start with a letter, contain 3-48 lowercase "
			"letters, digits, or hyphens, and end with a letter or digit"
		)
	if config.get("cluster", {}).get("manager") != "slurm":
		raise RenderError("the Slurm adapter requires cluster.manager=slurm")
	if config.get("container", {}).get("runtime") != "enroot":
		raise RenderError("the current Slurm adapter requires Enroot")
	if config.get("container", {}).get("integration") != "pyxis":
		raise RenderError("the current Slurm adapter requires Pyxis")

	workload = config.get("workload")
	if not workload:
		raise RenderError("the Slurm adapter requires a workload")
	validate_image(workload["image"], allow_mutable_tags)
	if config["communication"]["frontend"] != "nccl":
		raise RenderError("the current workload images support only NCCL")
	collective = config["communication"]["nccl"]
	if config["communication"]["transportAllowlist"] != ["IB"]:
		raise RenderError("NCCL rendering requires transportAllowlist=[IB]")
	if config["uib"]["enabled"] and collective["hca"] != "=uib0":
		raise RenderError("UIB rendering requires the exact HCA filter =uib0")
	if config["uib"]["enabled"]:
		bundle = config["uib"].get("bundle")
		if not bundle:
			raise RenderError("uib.bundle is required when UIB is enabled")
		validate_image(bundle, allow_mutable_tags)

	storage = config["storage"]
	for name in ("modelCacheClaim", "modelCacheImage", "resultsClaim"):
		if name in storage:
			raise RenderError(f"storage.{name} is Kubernetes-only")
	for name in ("modelCachePath", "resultsPath"):
		if name not in storage:
			raise RenderError(f"storage.{name} is required for Slurm")
	if config["uib"]["enabled"]:
		if "uibRuntimePath" not in storage:
			raise RenderError("storage.uibRuntimePath is required when UIB is enabled")
		if not config["network"].get("dataInterface"):
			raise RenderError("network.dataInterface is required when UIB is enabled")

	parallel = workload["parallelism"]
	if config["uib"]["bindPort"] + parallel["processesPerNode"] - 1 > 65535:
		raise RenderError("rank-specific UIB bind ports exceed 65535")
	if config["nodes"]["cpu"] % parallel["processesPerNode"]:
		raise RenderError("nodes.cpu must be divisible by processesPerNode")
	gpus = config["nodes"]["gpus"]["perWorker"]
	if gpus and gpus % parallel["processesPerNode"]:
		raise RenderError("GPUs per worker must be divisible by processesPerNode")
	for index, fault in enumerate(config.get("faults", [])):
		if fault["action"] not in ("add-delay", "packet-loss", "link-down"):
			raise RenderError(
				f"faults[{index}] requires a scheduler-specific executor"
			)
		if "at" not in fault:
			raise RenderError(
				f"faults[{index}] event-relative execution is not implemented"
			)

	kind = workload["kind"]
	if kind == "training":
		if config["launcher"]["training"] != "slurm":
			raise RenderError("training rendering requires launcher.training=slurm")
		validate_workload_config(config, TRAINING_SCHEMA)
	elif kind == "inference":
		if config["launcher"]["inference"] not in (
			"slurm-ray", "vllm-multiprocessing"
		):
			raise RenderError("inference rendering requires a Slurm launcher")
		if parallel["nodes"] != 1 or parallel["processesPerNode"] != 1:
			raise RenderError(
				"the current Slurm inference adapter supports one task; "
				"multi-node Ray orchestration is not implemented"
			)
		validate_workload_config(config, INFERENCE_SCHEMA)
	else:
		raise RenderError(f"unsupported Slurm workload kind: {kind}")


def string_value(value):
	if isinstance(value, bool):
		return "true" if value else "false"
	return str(value)


def active_mtu(link_mtu):
	for mtu in (4096, 2048, 1024, 512, 256):
		if mtu + 64 <= link_mtu:
			return mtu
	raise RenderError("network.mtu is too small for UIB")


def slurm_memory(quantity):
	return quantity.replace("Mi", "M").replace("Gi", "G").replace("Ti", "T")


def shell_assignment(name, value):
	value = str(value)
	quoted = shlex.quote(value)
	if value.startswith("=") and quoted == value:
		quoted = f"'{value}'"
	return f"{name}={quoted}"
