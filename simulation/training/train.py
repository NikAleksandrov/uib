#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0

import pathlib
import sys


sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

from training_workload.cli import main


if __name__ == "__main__":
	try:
		main()
	except Exception as error:
		print(f"training workload: {error}", file=sys.stderr)
		raise
