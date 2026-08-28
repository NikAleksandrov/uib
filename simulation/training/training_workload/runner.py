# SPDX-License-Identifier: Apache-2.0

from torch.nn.parallel import DistributedDataParallel
import contextlib
import torch.distributed as dist
import json
import math
import os
import pathlib
import random
import time
import torch


def transport_evidence(backend):
	preload = os.environ.get("LD_PRELOAD", "")
	verbs = os.environ.get("NCCL_IBVERBS_LIB", "")
	return {
		"distributedBackend": backend,
		"uibEnabled": "libuibv.so" in preload or "libuibv.so" in verbs,
		"ncclNetwork": os.environ.get("NCCL_NET"),
		"ncclIbverbsLibrary": verbs or None,
		"ldPreloadContainsUib": "libuibv.so" in preload,
		"uibProtocol": os.environ.get("UIB_PROTOCOL"),
		"uibTransportMode": os.environ.get("UIB_TRANSPORT_MODE"),
	}


def result_path(config):
	directory = pathlib.Path(config["result"]["directory"]) / config["runId"]
	directory.mkdir(parents=True, exist_ok=True)
	return directory / "result.json"


def train(config):
	from .capabilities import capabilities
	from .checkpoint import checkpoint_path, resume_checkpoint, save_checkpoint
	from .config import distributed_context, identity
	from .data import accumulation_steps, generated_batch, load_public_tokens, public_batch
	from .model import build_model, is_fsdp, model_checksum, unwrap

	training = config["training"]
	torch.set_num_threads(int(os.environ.get("UIB_TORCH_THREADS", "1")))
	random.seed(training["seed"])
	torch.manual_seed(training["seed"])
	rank, local_rank, world_size, backend, device = distributed_context(
		config["parallelism"]["strategy"], training["device"]
	)
	accumulation = accumulation_steps(config, world_size)
	if config["parallelism"]["strategy"] == "fsdp" and device.type != "cuda":
		raise RuntimeError("FSDP is capability-gated to CUDA in this image")
	model = build_model(config).to(device)
	if training["gradientCheckpointing"]:
		model.gradient_checkpointing_enable()
		model.config.use_cache = False
	if world_size > 1 and config["parallelism"]["strategy"] == "ddp":
		device_ids = [local_rank] if device.type == "cuda" else None
		model = DistributedDataParallel(model, device_ids=device_ids)
	elif config["parallelism"]["strategy"] == "fsdp":
		from torch.distributed.fsdp import FullyShardedDataParallel
		model = FullyShardedDataParallel(model, device_id=device)
	optimizer = torch.optim.AdamW(
		model.parameters(),
		lr=training["learningRate"],
		betas=(training["adamBeta1"], training["adamBeta2"]),
		eps=training["adamEpsilon"],
		weight_decay=training["weightDecay"],
	)
	start_step = resume_checkpoint(
		model, optimizer, config, device, rank, world_size
	)
	if start_step >= training["steps"]:
		raise ValueError("checkpoint step must be less than training.steps")
	if training["warmupSteps"] > training["steps"]:
		raise ValueError("warmupSteps must not exceed training.steps")
	public_tokens = None
	if config["dataset"]["source"] == "huggingface":
		public_tokens = load_public_tokens(config, rank, world_size)
	precision = training["precision"]
	if precision == "bf16" and device.type == "cuda" and not torch.cuda.is_bf16_supported():
		raise RuntimeError("bf16 requested but the CUDA device does not support it")
	autocast = torch.autocast if precision == "bf16" else None
	losses = []
	started = time.monotonic()
	model.train()
	for step in range(start_step, training["steps"]):
		warmup = training["warmupSteps"]
		factor = min((step + 1) / warmup, 1.0) if warmup else 1.0
		for group in optimizer.param_groups:
			group["lr"] = training["learningRate"] * factor
		optimizer.zero_grad(set_to_none=True)
		step_loss = torch.zeros(1, device=device)
		for micro_step in range(accumulation):
			if public_tokens is None:
				vocab_size = unwrap(model).config.vocab_size
				batch = generated_batch(
					config, rank, world_size, step, micro_step, vocab_size
				)
			else:
				batch = public_batch(public_tokens, config, step, micro_step)
			batch = batch.to(device)
			synchronize = micro_step == accumulation - 1
			no_sync = model.no_sync if hasattr(model, "no_sync") and not synchronize \
				else contextlib.nullcontext
			amp = autocast(device_type=device.type, dtype=torch.bfloat16) \
				if autocast else contextlib.nullcontext()
			with no_sync(), amp:
				loss = model(input_ids=batch, labels=batch).loss / accumulation
			loss.backward()
			step_loss += loss.detach()
		clip_norm = training["gradientClipNorm"]
		if clip_norm is not None:
			if is_fsdp(model):
				model.clip_grad_norm_(clip_norm)
			else:
				torch.nn.utils.clip_grad_norm_(model.parameters(), clip_norm)
		optimizer.step()
		if world_size > 1:
			dist.all_reduce(step_loss, op=dist.ReduceOp.SUM)
			step_loss /= world_size
		value = step_loss.item()
		if not math.isfinite(value):
			raise RuntimeError(f"non-finite loss at step {step + 1}")
		losses.append(value)
		completed = step + 1
		if completed % config["checkpoint"]["interval"] == 0 \
				or completed == training["steps"]:
			save_checkpoint(model, optimizer, config, completed, rank, world_size)
	duration = time.monotonic() - started
	checksum = model_checksum(model, device)
	if is_fsdp(model) and world_size > 1:
		value = torch.tensor(checksum, dtype=torch.float64, device=device)
		dist.all_reduce(value, op=dist.ReduceOp.SUM)
		checksum = value.item()
	elif world_size > 1:
		minimum = torch.tensor(checksum, dtype=torch.float64, device=device)
		maximum = minimum.clone()
		dist.all_reduce(minimum, op=dist.ReduceOp.MIN)
		dist.all_reduce(maximum, op=dist.ReduceOp.MAX)
		if not torch.allclose(minimum, maximum, rtol=0, atol=1e-7):
			raise RuntimeError("model checksum differs between ranks")
	if rank == 0:
		result = {
			"schemaVersion": 1,
			"runId": config["runId"],
			"status": "passed",
			"workloadIdentity": identity(config),
			"worldSize": world_size,
			"device": device.type,
			"strategy": config["parallelism"]["strategy"],
			"trainingShape": {
				"sequenceLength": training["sequenceLength"],
				"microBatchSize": training["microBatchSize"],
				"globalBatchSize": training["globalBatchSize"],
				"gradientAccumulationSteps": accumulation,
				"gradientCheckpointing": training["gradientCheckpointing"],
			},
			"completedSteps": training["steps"],
			"resumedFromStep": start_step,
			"loss": {"first": losses[0], "last": losses[-1]},
			"modelChecksum": checksum,
			"durationSeconds": duration,
			"samplesPerSecond": (
				(training["steps"] - start_step) * training["globalBatchSize"] / duration
			),
			"checkpoint": str(checkpoint_path(config, training["steps"])),
			"stack": capabilities(),
			"transport": transport_evidence(backend),
		}
		path = result_path(config)
		path.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n")
		print(json.dumps(result, sort_keys=True))
	if dist.is_initialized():
		dist.barrier()
		dist.destroy_process_group()
