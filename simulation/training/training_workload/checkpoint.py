# SPDX-License-Identifier: Apache-2.0

import torch.distributed as dist
import pathlib
import random
import tempfile
import torch


def checkpoint_path(config, step):
	directory = pathlib.Path(config["checkpoint"]["directory"])
	return directory / f"step-{step:08d}.pt"


def rank_rng_states(world_size):
	state = {
		"python": random.getstate(),
		"torch": torch.get_rng_state(),
		"cuda": torch.cuda.get_rng_state() if torch.cuda.is_available() else None,
	}
	if world_size == 1:
		return [state]
	states = [None] * world_size
	dist.all_gather_object(states, state)
	return states


def save_checkpoint(model, optimizer, config, step, rank, world_size):
	from .config import identity
	from .model import distributed_state

	if world_size > 1:
		dist.barrier()
	rng_states = rank_rng_states(world_size)
	model_state, optimizer_state = distributed_state(model, optimizer)
	if rank == 0:
		path = checkpoint_path(config, step)
		path.parent.mkdir(parents=True, exist_ok=True)
		state = {
			"schemaVersion": 2,
			"workloadIdentity": identity(config),
			"worldSize": world_size,
			"step": step,
			"model": model_state,
			"optimizer": optimizer_state,
			"rankRngStates": rng_states,
		}
		with tempfile.NamedTemporaryFile(dir=path.parent, delete=False) as output:
			temporary = pathlib.Path(output.name)
		try:
			torch.save(state, temporary)
			temporary.replace(path)
		finally:
			temporary.unlink(missing_ok=True)
	if world_size > 1:
		dist.barrier()


def resume_checkpoint(model, optimizer, config, device, rank, world_size):
	from .config import identity

	resume = config["checkpoint"].get("resume")
	if not resume:
		return 0
	state = torch.load(resume, map_location=device, weights_only=True)
	if state.get("schemaVersion") != 2:
		raise RuntimeError("unsupported checkpoint schema")
	if state.get("workloadIdentity") != identity(config):
		raise RuntimeError("checkpoint workload identity does not match configuration")
	if state.get("worldSize") != world_size:
		raise RuntimeError("checkpoint world size does not match WORLD_SIZE")
	from torch.distributed.checkpoint.state_dict import (
		StateDictOptions,
		set_state_dict,
	)

	set_state_dict(
		model,
		optimizer,
		model_state_dict=state["model"],
		optim_state_dict=state["optimizer"],
		options=StateDictOptions(full_state_dict=True, cpu_offload=True),
	)
	rng_state = state["rankRngStates"][rank]
	random.setstate(rng_state["python"])
	torch.set_rng_state(rng_state["torch"].cpu())
	if rng_state["cuda"] is not None:
		torch.cuda.set_rng_state(rng_state["cuda"].cpu(), device=device)
	return int(state["step"])
