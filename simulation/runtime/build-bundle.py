#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0

import pathlib
import sys


sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

from runtime_bundle.builder import build
from runtime_bundle.cli import parse_args


if __name__ == "__main__":
    build(parse_args())
