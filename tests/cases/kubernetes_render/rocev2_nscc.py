# SPDX-License-Identifier: Apache-2.0

import sys

import yaml


with open(sys.argv[1], encoding="utf-8") as source:
    job = list(yaml.safe_load_all(source))[3]
environment = {entry["name"]: entry for entry in job["spec"]["trainer"]["env"]}
assert environment["UIB_PROTOCOL"]["value"] == "rocev2"
assert environment["UIB_ROCEV2_CC"]["value"] == "nscc"
assert environment["UIB_ROCEV2_NSCC_MAX_WND"]["value"] == "65536"
assert "UIB_UET_CC" not in environment
