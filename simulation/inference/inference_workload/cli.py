# SPDX-License-Identifier: Apache-2.0

import argparse
import json
import pathlib


def main():
	from .capabilities import capabilities
	from .config import load_config, vllm_arguments
	from .probe import run_probe
	from .server import serve

	parser = argparse.ArgumentParser()
	parser.add_argument("--print-capabilities", action="store_true")
	subparsers = parser.add_subparsers(dest="command")
	for name in ("serve", "probe", "render-vllm"):
		command = subparsers.add_parser(name)
		command.add_argument("--config", required=True, type=pathlib.Path)
	args = parser.parse_args()
	if args.print_capabilities:
		print(json.dumps(capabilities(), indent=2, sort_keys=True))
		return
	if args.command is None:
		parser.error("a command is required")
	config = load_config(args.config)
	if args.command == "serve":
		serve(config)
	elif args.command == "probe":
		run_probe(config)
	else:
		print(json.dumps(vllm_arguments(config)))
