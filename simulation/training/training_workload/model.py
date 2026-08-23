# SPDX-License-Identifier: Apache-2.0

from transformers import AutoModelForCausalLM, AutoTokenizer, GPT2Config
from torch.nn.parallel import DistributedDataParallel
import torch


def build_model(config):
	model_config = config["model"]
	if model_config["source"] == "huggingface":
		model = AutoModelForCausalLM.from_pretrained(
			model_config["id"],
			revision=model_config["revision"],
			trust_remote_code=False,
		)
		model.config.loss_type = "ForCausalLM"
		model.loss_type = "ForCausalLM"
		return model
	gpt2 = GPT2Config(
		vocab_size=model_config.get("vocabSize", 128),
		n_positions=config["training"]["sequenceLength"],
		n_ctx=config["training"]["sequenceLength"],
		n_embd=model_config.get("hiddenSize", 32),
		n_layer=model_config.get("layers", 1),
		n_head=model_config.get("heads", 2),
		bos_token_id=0,
		eos_token_id=1,
		loss_type="ForCausalLM",
	)
	model = AutoModelForCausalLM.from_config(gpt2)
	model.loss_type = "ForCausalLM"
	return model


def unwrap(model):
	return model.module if isinstance(model, DistributedDataParallel) else model


def is_fsdp(model):
	from torch.distributed.fsdp import FullyShardedDataParallel

	return isinstance(model, FullyShardedDataParallel)


def distributed_state(model, optimizer):
	from torch.distributed.checkpoint.state_dict import (
		StateDictOptions,
		get_state_dict,
	)

	return get_state_dict(
		model,
		optimizer,
		options=StateDictOptions(full_state_dict=True, cpu_offload=True),
	)


def model_checksum(model, device):
	checksum = torch.zeros(1, dtype=torch.float64, device=device)
	with torch.no_grad():
		for parameter in unwrap(model).parameters():
			checksum += parameter.detach().double().sum()
	return checksum.item()
