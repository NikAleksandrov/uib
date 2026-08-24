# SPDX-License-Identifier: Apache-2.0

import json
import shutil

from .dependencies import yaml


def command_render(args):
    from .config import resolve

    config = resolve(args)
    if args.format == "json":
        print(json.dumps(config, indent=2, sort_keys=True))
    else:
        print(yaml.safe_dump(config, sort_keys=True), end="")
    return 0


def command_doctor(args):
    from .config import find_overlay, find_profile, resolve, tools_for

    config = resolve(args)
    tools = {name: shutil.which(name) for name in tools_for(config)}
    report = {
        "schemaVersion": 1,
        "readOnly": True,
        "profile": str(find_profile(args.profile)),
        "overlays": [str(find_overlay(name)) for name in args.overlay],
        "provider": config["infrastructure"]["provider"],
        "manager": config["cluster"]["manager"],
        "frontend": config["communication"]["frontend"],
        "tools": tools,
        "missing": [name for name, path in tools.items() if not path],
    }
    print(json.dumps(report, indent=2, sort_keys=True))
    return 1 if report["missing"] else 0
