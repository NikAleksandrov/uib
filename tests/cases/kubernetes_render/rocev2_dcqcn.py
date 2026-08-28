# SPDX-License-Identifier: Apache-2.0

import sys

import yaml


with open(sys.argv[1], encoding="utf-8") as source:
    job = list(yaml.safe_load_all(source))[3]
environment = {entry["name"]: entry for entry in job["spec"]["trainer"]["env"]}
assert environment["UIB_ROCEV2_CC"]["value"] == "dcqcn"
assert environment["UIB_DCQCN_INITIAL_RATE"]["value"] == "1000000000"
assert environment["UIB_DCQCN_ALPHA_GAIN"]["value"] == "512"
