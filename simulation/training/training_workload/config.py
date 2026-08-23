# SPDX-License-Identifier: Apache-2.0

import torch.distributed as dist
import hashlib
import json
import jsonschema
import os
import torch


def load_config(path):
	from .core import ROOT

	with (ROOT / "workload.schema.json").open(encoding="utf-8") as source:
		schema = json.load(source)
	with path.open(encoding="utf-8") as source:
		config = json.load(source)
	jsonschema.Draft202012Validator(schema).validate(config)
	return config


def identity(config):
	value = {
		"schemaVersion": config["schemaVersion"],
		"model": config["model"],
		"dataset": config["dataset"],
		"parallelism": config["parallelism"],
		"training": {
			key: value for key, value in config["training"].items()
			if key != "steps"
		}
	}
	encoded = json.dumps(value, sort_keys=True, separators=(",", ":")).encode()
	return hashlib.sha256(encoded).hexdigest()


def distributed_context(strategy, requested_device):
	world_size = int(os.environ.get("WORLD_SIZE", "1"))
	rank = int(os.environ.get("RANK", "0"))
	local_rank = int(os.environ.get("LOCAL_RANK", "0"))
	use_cuda = requested_device == "cuda" or (
		requested_device == "auto" and torch.cuda.is_available()
	)
	if use_cuda and not torch.cuda.is_available():
		raise RuntimeError("CUDA requested but no CUDA device is available")
	if use_cuda:
		device = torch.device("cuda", local_rank)
		torch.cuda.set_device(device)
		backend = "nccl"
	else:
		device = torch.device("cpu")
		backend = "gloo"
	if world_size > 1 or strategy == "fsdp" or "RANK" in os.environ:
		options = {"backend": backend}
		if device.type == "cuda":
			options["device_id"] = device
		dist.init_process_group(**options)
	return rank, local_rank, world_size, backend, device
