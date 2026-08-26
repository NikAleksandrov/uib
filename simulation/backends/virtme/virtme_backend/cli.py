# SPDX-License-Identifier: Apache-2.0

import argparse
import sys


def main():
    from .core import VirtmeError
    from .lifecycle import command_down, command_render, command_status, command_up

    parser = argparse.ArgumentParser(
        description="render and invoke an owned multi-guest virtme lab")
    commands = parser.add_subparsers(dest="command", required=True)
    render = commands.add_parser(
        "render", help="render guest and network commands")
    render.add_argument("config")
    render.add_argument("--name", required=True)
    render.set_defaults(handler=command_render)
    up = commands.add_parser("up", help="create an owned virtme lab")
    up.add_argument("config")
    up.add_argument("--name", required=True)
    up.add_argument("--state-dir", required=True)
    up.add_argument("--retain", action="store_true")
    up.set_defaults(handler=command_up)
    status = commands.add_parser("status", help="show owned virtme lab state")
    status.add_argument("--name", required=True)
    status.add_argument("--state-dir", required=True)
    status.set_defaults(handler=command_status)
    down = commands.add_parser("down", help="destroy owned virtme guests")
    down.add_argument("--name", required=True)
    down.add_argument("--state-dir", required=True)
    down.set_defaults(handler=command_down)
    args = parser.parse_args()
    try:
        result = args.handler(args)
        return result if result is not None else 0
    except (OSError, VirtmeError) as error:
        print(f"uib-virtme: {error}", file=sys.stderr)
        return 2
