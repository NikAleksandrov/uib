# SPDX-License-Identifier: Apache-2.0

import concurrent.futures
import json
import pathlib
import statistics
import time


def run_probe(config):
	from .capabilities import capabilities
	from .client import make_prompt, percentile, request_json, send_completion, wait_for_server
	from .config import transport_evidence, workload_identity
	from .server import server_url

	from transformers import AutoTokenizer

	base_url = server_url(config)
	wait_for_server(base_url, config["load"]["timeoutSeconds"])
	models = request_json(base_url + "/v1/models")
	if config["model"]["servedName"] not in {
		item["id"] for item in models["data"]
	}:
		raise RuntimeError("served model is absent from /v1/models")
	tokenizer = AutoTokenizer.from_pretrained(
		config["model"]["id"],
		revision=config["model"]["revision"],
		trust_remote_code=False,
	)
	load = config["load"]
	prompts = [
		make_prompt(tokenizer, load["promptTokens"], index)
		for index in range(load["requests"])
	]
	for index in range(load["warmupRequests"]):
		send_completion(base_url, config, prompts[index % len(prompts)], -1, 0, 0)
	rate = load["requestRate"]
	schedule = [index / rate if rate is not None else 0 for index in range(len(prompts))]
	result_directory = pathlib.Path(config["result"]["directory"]) / config["runId"]
	result_directory.mkdir(parents=True, exist_ok=True)
	trace = {
		"schemaVersion": 1,
		"seed": load["seed"],
		"requests": [
			{
				"index": index,
				"scheduledOffsetSeconds": schedule[index],
				"prompt": prompt,
				"maximumOutputTokens": load["outputTokens"],
			}
			for index, prompt in enumerate(prompts)
		],
	}
	(result_directory / "request-trace.json").write_text(
		json.dumps(trace, indent=2, sort_keys=True) + "\n"
	)
	started = time.monotonic()
	with concurrent.futures.ThreadPoolExecutor(
		max_workers=load["concurrency"]
	) as executor:
		futures = [
			executor.submit(
				send_completion,
				base_url,
				config,
				prompt,
				index,
				started,
				schedule[index],
			)
			for index, prompt in enumerate(prompts)
		]
		responses = [future.result() for future in futures]
	duration = time.monotonic() - started
	latencies = [item["latencySeconds"] for item in responses]
	output_tokens = sum(item["outputTokens"] for item in responses)
	result = {
		"schemaVersion": 1,
		"runId": config["runId"],
		"status": "passed",
		"workloadIdentity": workload_identity(config),
		"backend": config["backend"],
		"model": config["model"],
		"parallelism": {
			key: config["server"][key] for key in (
				"tensorParallel", "pipelineParallel", "dataParallel", "replicas"
			)
		},
		"load": {
			"configuredRequests": load["requests"],
			"concurrency": load["concurrency"],
			"requestRate": load["requestRate"],
			"promptTokens": load["promptTokens"],
			"maximumOutputTokens": load["outputTokens"],
			"streaming": load["streaming"],
			"warmupRequests": load["warmupRequests"],
		},
		"requests": len(responses),
		"outputTokens": output_tokens,
		"durationSeconds": duration,
		"requestsPerSecond": len(responses) / duration,
		"outputTokensPerSecond": output_tokens / duration,
		"latencySeconds": {
			"minimum": min(latencies),
			"median": statistics.median(latencies),
			"p95": percentile(latencies, 0.95),
			"maximum": max(latencies),
		},
		"responses": sorted(responses, key=lambda item: item["index"]),
		"requestTrace": str(result_directory / "request-trace.json"),
		"stack": capabilities(),
		"transport": transport_evidence(),
	}
	path = result_directory / "result.json"
	path.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n")
	print(json.dumps(result, sort_keys=True))
