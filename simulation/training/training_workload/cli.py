# SPDX-License-Identifier: Apache-2.0

import argparse
import json
import pathlib


def main():
	from .capabilities import capabilities
	from .config import load_config
	from .runner import train

	parser = argparse.ArgumentParser()
	parser.add_argument("--config", type=pathlib.Path)
	parser.add_argument("--print-capabilities", action="store_true")
	args = parser.parse_args()
	if args.print_capabilities:
		print(json.dumps(capabilities(), indent=2, sort_keys=True))
		return
	if args.config is None:
		parser.error("--config is required")
	train(load_config(args.config))
