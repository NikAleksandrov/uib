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


def add_lifecycle_args(parser):
    add_config_args(parser)
    parser.add_argument("--run-id", required=True,
                        help="explicit provider-owned resource name")


def main():
    from .core import ConfigurationError
    from .lifecycle import command_doctor, command_down, command_kubeconfig, command_render, command_status, command_up

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
    up = commands.add_parser("up", help="create selected infrastructure")
    add_lifecycle_args(up)
    up.add_argument("--wait", default="120s", help="provider readiness timeout")
    up.add_argument("--kubeconfig", help="write a Kind kubeconfig to this path")
    up.add_argument("--retain", action="store_true",
                    help="retain Kind nodes after create failure")
    up.set_defaults(handler=command_up)
    status = commands.add_parser("status", help="inspect selected infrastructure")
    add_lifecycle_args(status)
    status.set_defaults(handler=command_status)
    kubeconfig = commands.add_parser("kubeconfig", help="print provider kubeconfig")
    add_lifecycle_args(kubeconfig)
    kubeconfig.set_defaults(handler=command_kubeconfig)
    down = commands.add_parser("down", help="remove selected infrastructure")
    add_lifecycle_args(down)
    down.set_defaults(handler=command_down)
    args = parser.parse_args()
    try:
        return args.handler(args)
    except ConfigurationError as error:
        print(f"uib-sim: {error}", file=sys.stderr)
        return 2
