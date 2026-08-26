# SPDX-License-Identifier: Apache-2.0

import argparse
import sys


def main():
    from .core import VirtmeError
    from .lifecycle import command_render

    parser = argparse.ArgumentParser(
        description="render an owned multi-guest virtme topology")
    commands = parser.add_subparsers(dest="command", required=True)
    render = commands.add_parser(
        "render", help="render guest and bridge commands")
    render.add_argument("config")
    render.add_argument("--name", required=True)
    render.set_defaults(handler=command_render)
    args = parser.parse_args()
    try:
        result = args.handler(args)
        return result if result is not None else 0
    except (OSError, VirtmeError) as error:
        print(f"uib-virtme: {error}", file=sys.stderr)
        return 2
