# SPDX-License-Identifier: Apache-2.0

import json


def command_render(args):
    from .core import load_config, validate_name
    from .topology import topology

    validate_name(args.name)
    config = load_config(args.config)
    print(json.dumps(topology(config, args.name), indent=2, sort_keys=True))
