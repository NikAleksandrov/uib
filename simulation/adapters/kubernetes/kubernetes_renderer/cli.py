# SPDX-License-Identifier: Apache-2.0

import argparse
import sys


def main():
    from .core import RenderError, load_config
    from .renderer import render

    parser = argparse.ArgumentParser(
        description="render a Kubernetes workload from resolved UIB JSON"
    )
    parser.add_argument("config", help="resolved uib-sim JSON configuration")
    parser.add_argument("--run-id", required=True, help="run namespace and ownership ID")
    parser.add_argument(
        "--allow-mutable-tags",
        action="store_true",
        help="allow local development image tags instead of immutable digests",
    )
    args = parser.parse_args()
    try:
        render(load_config(args.config), args.run_id, args.allow_mutable_tags)
    except RenderError as error:
        print(f"kubernetes renderer: {error}", file=sys.stderr)
        return 2
    return 0
