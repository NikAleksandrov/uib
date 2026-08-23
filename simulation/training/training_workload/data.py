# SPDX-License-Identifier: Apache-2.0

from transformers import AutoModelForCausalLM, AutoTokenizer, GPT2Config
import os
import torch


def load_public_tokens(config, rank, world_size):
	from datasets import load_dataset

	model_config = config["model"]
	dataset_config = config["dataset"]
	if model_config["source"] != "huggingface":
		raise ValueError("a Hugging Face dataset requires a Hugging Face model")
	tokenizer = AutoTokenizer.from_pretrained(
		model_config["id"],
		revision=model_config["revision"],
		trust_remote_code=False,
	)
	if tokenizer.pad_token_id is None:
		tokenizer.pad_token = tokenizer.eos_token
	dataset = load_dataset(
		dataset_config["id"],
		dataset_config.get("name"),
		split=dataset_config["split"],
		revision=dataset_config["revision"],
		streaming=True,
	)
	sequence_length = config["training"]["sequenceLength"]
	micro_batch = config["training"]["microBatchSize"]
	accumulation = accumulation_steps(config, world_size)
	required = config["training"]["steps"] * accumulation * micro_batch
	tokens = []
	for index, row in enumerate(dataset):
		if index % world_size != rank:
			continue
		encoded = tokenizer(
			row[dataset_config["textField"]],
			padding="max_length",
			truncation=True,
			max_length=sequence_length,
			return_tensors="pt",
		)["input_ids"][0]
		tokens.append(encoded)
		if len(tokens) >= required:
			break
	if len(tokens) < required:
		raise RuntimeError(
			f"dataset yielded {len(tokens)} examples; {required} are required"
		)
	return torch.stack(tokens)


def accumulation_steps(config, world_size):
	training = config["training"]
	denominator = training["microBatchSize"] * world_size
	if training["globalBatchSize"] % denominator:
		raise ValueError(
			"globalBatchSize must be divisible by microBatchSize * WORLD_SIZE"
		)
	return training["globalBatchSize"] // denominator


def generated_batch(config, rank, world_size, step, micro_step, vocab_size):
	training = config["training"]
	accumulation = accumulation_steps(config, world_size)
	sequence = step * accumulation + micro_step
	generator = torch.Generator(device="cpu")
	generator.manual_seed(training["seed"] + sequence * world_size + rank)
	return torch.randint(
		0,
		vocab_size,
		(training["microBatchSize"], training["sequenceLength"]),
		generator=generator,
	)


def public_batch(tokens, config, step, micro_step):
	training = config["training"]
	accumulation = accumulation_steps(config, int(os.environ.get("WORLD_SIZE", "1")))
	start = (step * accumulation + micro_step) * training["microBatchSize"]
	return tokens[start:start + training["microBatchSize"]]
