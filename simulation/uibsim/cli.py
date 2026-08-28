# SPDX-License-Identifier: Apache-2.0

import argparse
import signal
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


def add_state_dir_arg(parser):
    from .core import DEFAULT_STATE_DIR

    parser.add_argument(
        "--state-dir", default=str(DEFAULT_STATE_DIR),
        help="run ledger root (default: simulation/.artifacts/runs)")


def main():
    from .core import ConfigurationError, DEFAULT_HUGGINGFACE_CACHE, LifecycleInterrupted, interrupt_lifecycle
    from .lifecycle import command_collect, command_doctor, command_down, command_kubeconfig, command_load_cache, command_load_image, command_render, command_render_workload, command_run, command_status, command_up

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
                    help="retain provider resources after create failure")
    add_state_dir_arg(up)
    up.set_defaults(handler=command_up)
    status = commands.add_parser("status", help="inspect selected infrastructure")
    add_lifecycle_args(status)
    add_state_dir_arg(status)
    status.add_argument("--kubeconfig", help="Kubernetes client configuration")
    status.set_defaults(handler=command_status)
    kubeconfig = commands.add_parser("kubeconfig", help="print provider kubeconfig")
    add_lifecycle_args(kubeconfig)
    add_state_dir_arg(kubeconfig)
    kubeconfig.set_defaults(handler=command_kubeconfig)
    load_image = commands.add_parser(
        "load-image", help="stage explicit images in selected infrastructure")
    add_lifecycle_args(load_image)
    load_image.add_argument("--image", action="append", required=True,
                            help="image reference; repeatable")
    load_image.add_argument(
        "--allow-mutable-tags", action="store_true",
        help="allow local development tags instead of immutable digests")
    add_state_dir_arg(load_image)
    load_image.set_defaults(handler=command_load_image)
    load_cache = commands.add_parser(
        "load-cache", help="stage the local Hugging Face cache")
    add_lifecycle_args(load_cache)
    load_cache.add_argument(
        "--source", default=str(DEFAULT_HUGGINGFACE_CACHE),
        help="local Hugging Face cache root")
    add_state_dir_arg(load_cache)
    load_cache.set_defaults(handler=command_load_cache)
    render_workload = commands.add_parser(
        "render-workload", help="render scheduler resources for one workload")
    add_lifecycle_args(render_workload)
    render_workload.add_argument(
        "--allow-mutable-tags", action="store_true",
        help="allow local development tags instead of immutable digests")
    render_workload.set_defaults(handler=command_render_workload)
    run = commands.add_parser(
        "run", help="render and submit one scheduler workload")
    add_lifecycle_args(run)
    add_state_dir_arg(run)
    run.add_argument(
        "--allow-mutable-tags", action="store_true",
        help="allow local development image tags instead of immutable digests")
    run.add_argument("--kubeconfig", help="Kubernetes client configuration")
    run.set_defaults(handler=command_run)
    collect = commands.add_parser(
        "collect", help="collect one recorded scheduler result")
    collect.add_argument("--run-id", required=True,
                         help="recorded scheduler run ID")
    add_state_dir_arg(collect)
    collect.add_argument(
        "--output-dir",
        help="new result bundle directory "
             "(default: simulation/.artifacts/results/RUN_ID)")
    collect.add_argument(
        "--allow-incomplete", action="store_true",
        help="collect a diagnostic snapshot before the job reaches a terminal state")
    collect.add_argument("--kubeconfig", help="Kubernetes client configuration")
    collect.set_defaults(handler=command_collect)
    down = commands.add_parser("down", help="remove selected infrastructure")
    add_lifecycle_args(down)
    add_state_dir_arg(down)
    down.add_argument("--kubeconfig", help="Kubernetes client configuration")
    down.add_argument(
        "--infrastructure", action="store_true",
        help="also remove same-run Kubernetes infrastructure")
    down.set_defaults(handler=command_down)
    args = parser.parse_args()
    previous_handlers = {}
    if args.command in {"up", "run", "collect", "down"}:
        for signum in (signal.SIGINT, signal.SIGTERM):
            previous_handlers[signum] = signal.signal(
                signum, interrupt_lifecycle)
    try:
        return args.handler(args)
    except LifecycleInterrupted as error:
        print(f"uib-sim: {error}", file=sys.stderr)
        return 128 + error.signum
    except ConfigurationError as error:
        print(f"uib-sim: {error}", file=sys.stderr)
        return 2
    finally:
        for signum, handler in previous_handlers.items():
            signal.signal(signum, handler)
