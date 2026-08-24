# SPDX-License-Identifier: Apache-2.0

import hashlib
import json
import math
import time
import urllib.request


def request_json(url, method="GET", payload=None, timeout=10):
	data = None
	headers = {}
	if payload is not None:
		data = json.dumps(payload).encode()
		headers["Content-Type"] = "application/json"
	request = urllib.request.Request(url, data=data, headers=headers, method=method)
	with urllib.request.urlopen(request, timeout=timeout) as response:
		return json.loads(response.read())


def request_stream(url, payload, timeout):
	data = json.dumps(payload).encode()
	request = urllib.request.Request(
		url,
		data=data,
		headers={"Content-Type": "application/json"},
		method="POST",
	)
	text = []
	usage = None
	with urllib.request.urlopen(request, timeout=timeout) as response:
		for raw_line in response:
			line = raw_line.decode().strip()
			if not line.startswith("data: ") or line == "data: [DONE]":
				continue
			chunk = json.loads(line[6:])
			if chunk.get("choices"):
				text.append(chunk["choices"][0].get("text", ""))
			if chunk.get("usage") is not None:
				usage = chunk["usage"]
	if usage is None:
		raise RuntimeError("streaming response did not include token usage")
	return {"choices": [{"text": "".join(text)}], "usage": usage}


def wait_for_server(base_url, timeout):
	deadline = time.monotonic() + timeout
	last_error = None
	while time.monotonic() < deadline:
		try:
			request = urllib.request.Request(base_url + "/health", method="GET")
			with urllib.request.urlopen(request, timeout=2) as response:
				if response.status == 200:
					return
		except (OSError, urllib.error.URLError) as error:
			last_error = error
			time.sleep(0.25)
	raise TimeoutError(f"server did not become ready: {last_error}")


def make_prompt(tokenizer, token_count, index):
	seeds = (
		"Distributed systems make progress through careful coordination. ",
		"A small language model can validate a complete serving path. ",
		"Reproducible workloads use pinned inputs and deterministic seeds. ",
	)
	encoded = tokenizer.encode(seeds[index % len(seeds)], add_special_tokens=False)
	if not encoded:
		raise RuntimeError("tokenizer produced an empty prompt")
	values = (encoded * ((token_count + len(encoded) - 1) // len(encoded)))[:token_count]
	return tokenizer.decode(values, skip_special_tokens=True)


def send_completion(base_url, config, prompt, index, measured_start, scheduled):
	delay = measured_start + scheduled - time.monotonic()
	if delay > 0:
		time.sleep(delay)
	payload = {
		"model": config["model"]["servedName"],
		"prompt": prompt,
		"max_tokens": config["load"]["outputTokens"],
		"temperature": 0,
		"stream": config["load"]["streaming"],
		"seed": config["load"]["seed"] + index,
	}
	if config["load"]["streaming"]:
		payload["stream_options"] = {"include_usage": True}
	started = time.monotonic()
	if config["load"]["streaming"]:
		response = request_stream(
			base_url + "/v1/completions",
			payload,
			config["load"]["timeoutSeconds"],
		)
	else:
		response = request_json(
			base_url + "/v1/completions",
			method="POST",
			payload=payload,
			timeout=config["load"]["timeoutSeconds"],
		)
	duration = time.monotonic() - started
	choice = response["choices"][0]
	usage = response["usage"]
	return {
		"index": index,
		"latencySeconds": duration,
		"promptTokens": usage["prompt_tokens"],
		"outputTokens": usage["completion_tokens"],
		"outputSha256": hashlib.sha256(choice["text"].encode()).hexdigest(),
	}


def percentile(values, percentage):
	ordered = sorted(values)
	position = max(
		0,
		min(len(ordered) - 1, math.ceil(len(ordered) * percentage) - 1),
	)
	return ordered[position]
