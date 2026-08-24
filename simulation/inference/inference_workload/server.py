# SPDX-License-Identifier: Apache-2.0

import json
import os
import threading
import time


def serve_transformers(config):
	import torch
	import uvicorn
	from fastapi import FastAPI, HTTPException
	from fastapi.responses import StreamingResponse
	from transformers import AutoModelForCausalLM, AutoTokenizer

	model_config = config["model"]
	server = config["server"]
	torch.manual_seed(server["seed"])
	tokenizer = AutoTokenizer.from_pretrained(
		model_config["id"],
		revision=model_config["revision"],
		trust_remote_code=False,
	)
	model = AutoModelForCausalLM.from_pretrained(
		model_config["id"],
		revision=model_config["revision"],
		trust_remote_code=False,
	).eval()
	lock = threading.Lock()
	app = FastAPI()

	@app.get("/health")
	def health():
		return {"status": "ok", "backend": "transformers"}

	@app.get("/v1/models")
	def models():
		return {
			"object": "list",
			"data": [{
				"id": model_config["servedName"],
				"object": "model",
				"owned_by": "uib-smoke",
			}],
		}

	@app.post("/v1/completions")
	def completions(request: dict):
		if request.get("model") != model_config["servedName"]:
			raise HTTPException(status_code=404, detail="unknown model")
		prompt = request.get("prompt")
		if not isinstance(prompt, str) or not prompt:
			raise HTTPException(status_code=400, detail="prompt must be a string")
		max_tokens = int(request.get("max_tokens", 16))
		if max_tokens < 1:
			raise HTTPException(status_code=400, detail="max_tokens must be positive")
		inputs = tokenizer(prompt, return_tensors="pt")
		if inputs.input_ids.shape[1] + max_tokens > server["maxModelLength"]:
			raise HTTPException(status_code=400, detail="request exceeds maxModelLength")
		with lock, torch.inference_mode():
			output = model.generate(
				**inputs,
				max_new_tokens=max_tokens,
				do_sample=False,
				pad_token_id=tokenizer.eos_token_id,
			)
		completion = output[0, inputs.input_ids.shape[1]:]
		text = tokenizer.decode(completion, skip_special_tokens=True)
		response = {
			"id": "cmpl-uib-smoke",
			"object": "text_completion",
			"created": int(time.time()),
			"model": model_config["servedName"],
			"choices": [{
				"index": 0,
				"text": text,
				"finish_reason": "length",
			}],
			"usage": {
				"prompt_tokens": int(inputs.input_ids.shape[1]),
				"completion_tokens": int(completion.shape[0]),
				"total_tokens": int(inputs.input_ids.shape[1] + completion.shape[0]),
			},
		}
		if not request.get("stream", False):
			return response

		def events():
			yield "data: " + json.dumps(response) + "\n\n"
			yield "data: [DONE]\n\n"

		return StreamingResponse(events(), media_type="text/event-stream")

	uvicorn.run(app, host=server["host"], port=server["port"], log_level="info")


def serve(config):
	from .config import vllm_arguments

	if config["backend"] == "vllm":
		arguments = vllm_arguments(config)
		os.execvp(arguments[0], arguments)
	serve_transformers(config)


def server_url(config):
	configured = os.environ.get("INFERENCE_BASE_URL")
	if configured:
		return configured.rstrip("/")
	host = config["server"]["host"]
	if host in ("0.0.0.0", "::"):
		host = "127.0.0.1"
	return f"http://{host}:{config['server']['port']}"
