# SPDX-License-Identifier: Apache-2.0

import argparse
import os
import signal
import sys


def main():
    from .core import DEFAULT_CACHE, VirtmeError, interrupt_lifecycle
    from .lifecycle import command_down, command_kubeconfig, command_render, command_status, command_up

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
    up.add_argument(
        "--cache-dir",
        default=os.environ.get("VIRTME_DOWNLOAD_CACHE", str(DEFAULT_CACHE)))
    up.add_argument("--retain", action="store_true")
    up.set_defaults(handler=command_up)
    status = commands.add_parser("status", help="show owned virtme lab state")
    status.add_argument("--name", required=True)
    status.add_argument("--state-dir", required=True)
    status.set_defaults(handler=command_status)
    kubeconfig = commands.add_parser(
        "kubeconfig", help="print the owned Kubernetes configuration")
    kubeconfig.add_argument("--name", required=True)
    kubeconfig.add_argument("--state-dir", required=True)
    kubeconfig.set_defaults(handler=command_kubeconfig)
    down = commands.add_parser("down", help="destroy owned virtme guests")
    down.add_argument("--name", required=True)
    down.add_argument("--state-dir", required=True)
    down.set_defaults(handler=command_down)
    args = parser.parse_args()
    previous_handlers = {}
    if args.command in {"up", "down"}:
        for signum in (signal.SIGINT, signal.SIGTERM):
            previous_handlers[signum] = signal.signal(
                signum, interrupt_lifecycle)
    try:
        result = args.handler(args)
        return result if result is not None else 0
    except (OSError, VirtmeError) as error:
        print(f"uib-virtme: {error}", file=sys.stderr)
        return 2
    finally:
        for signum, handler in previous_handlers.items():
            signal.signal(signum, handler)
