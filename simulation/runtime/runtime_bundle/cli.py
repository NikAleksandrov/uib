# SPDX-License-Identifier: Apache-2.0

import argparse
import pathlib


def parse_args():
    parser = argparse.ArgumentParser(description="Build the UIB runtime bundle")
    parser.add_argument("--output", required=True, type=pathlib.Path)
    parser.add_argument("--archive", type=pathlib.Path)
    parser.add_argument("--replace", action="store_true")
    parser.add_argument("--no-build", action="store_true")
    return parser.parse_args()
