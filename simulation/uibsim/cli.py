# SPDX-License-Identifier: Apache-2.0

import argparse
import sys


def add_config_args(parser):
    parser.add_argument("--profile", required=True,
                        help="profile name or YAML file")
    parser.add_argument("--request", help="workload request overlay")
    parser.add_argument("--overlay", action="append", default=[],
                        help="named layer overlay or YAML file; repeatable")
    parser.add_argument("--set", action="append", default=[],
                        metavar="PATH=VALUE", help="override an existing value")


def main():
    from .core import ConfigurationError
    from .lifecycle import command_doctor, command_render

    parser = argparse.ArgumentParser(
        description="resolve and inspect UIB simulation configuration")
    commands = parser.add_subparsers(dest="command", required=True)
    render = commands.add_parser("render", help="merge and validate YAML")
    add_config_args(render)
    render.add_argument("--format", choices=("yaml", "json"), default="yaml")
    render.set_defaults(handler=command_render)
    doctor = commands.add_parser("doctor", help="report required standard tools")
    add_config_args(doctor)
    doctor.set_defaults(handler=command_doctor)
    args = parser.parse_args()
    try:
        return args.handler(args)
    except ConfigurationError as error:
        print(f"uib-sim: {error}", file=sys.stderr)
        return 2
