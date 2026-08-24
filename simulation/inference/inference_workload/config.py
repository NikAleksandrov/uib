# SPDX-License-Identifier: Apache-2.0

import hashlib
import json
import jsonschema
import os


def load_config(path):
	from .core import ROOT

	with (ROOT / "workload.schema.json").open(encoding="utf-8") as source:
		schema = json.load(source)
	with path.open(encoding="utf-8") as source:
		config = json.load(source)
	jsonschema.Draft202012Validator(schema).validate(config)
	validate_config(config)
	return config


def validate_config(config):
	server = config["server"]
	if config["backend"] == "transformers":
		if any(server[key] != 1 for key in (
			"tensorParallel", "pipelineParallel", "dataParallel", "replicas"
		)):
			raise ValueError("the CPU smoke backend supports one local replica")
		if server["distributedExecutor"] is not None:
			raise ValueError("the CPU smoke backend has no distributed executor")
		if server["quantization"] is not None:
			raise ValueError("the CPU smoke backend does not support quantization")
		if server["dtype"] not in ("auto", "float32"):
			raise ValueError("the CPU smoke backend requires float32 or auto")
	if config["load"]["promptTokens"] + config["load"]["outputTokens"] \
			> server["maxModelLength"]:
		raise ValueError("prompt and output tokens exceed maxModelLength")


def workload_identity(config):
	value = {key: item for key, item in config.items() if key != "result"}
	encoded = json.dumps(value, sort_keys=True, separators=(",", ":")).encode()
	return hashlib.sha256(encoded).hexdigest()


def transport_evidence():
	preload = os.environ.get("LD_PRELOAD", "")
	verbs = os.environ.get("NCCL_IBVERBS_LIB", "")
	return {
		"uibEnabled": "libuibv.so" in preload or "libuibv.so" in verbs,
		"ncclNetwork": os.environ.get("NCCL_NET"),
		"ncclIbverbsLibrary": verbs or None,
		"ldPreloadContainsUib": "libuibv.so" in preload,
		"uibProtocol": os.environ.get("UIB_PROTOCOL"),
		"uibTransportMode": os.environ.get("UIB_TRANSPORT_MODE"),
	}


def vllm_arguments(config):
	model = config["model"]
	server = config["server"]
	arguments = [
		"vllm", "serve", model["id"],
		"--revision", model["revision"],
		"--served-model-name", model["servedName"],
		"--host", server["host"],
		"--port", str(server["port"]),
		"--dtype", server["dtype"],
		"--max-model-len", str(server["maxModelLength"]),
		"--tensor-parallel-size", str(server["tensorParallel"]),
		"--pipeline-parallel-size", str(server["pipelineParallel"]),
		"--data-parallel-size", str(server["dataParallel"]),
		"--gpu-memory-utilization", str(server["gpuMemoryUtilization"]),
		"--seed", str(server["seed"]),
	]
	if server["distributedExecutor"] is not None:
		arguments.extend([
			"--distributed-executor-backend",
			server["distributedExecutor"],
		])
	if server["quantization"] is not None:
		arguments.extend(["--quantization", server["quantization"]])
	return arguments
