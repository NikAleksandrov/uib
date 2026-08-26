# SPDX-License-Identifier: Apache-2.0

import sys

import yaml


with open(sys.argv[1], encoding="utf-8") as source:
    job = list(yaml.safe_load_all(source))[3]
environment = {entry["name"]: entry for entry in job["spec"]["trainer"]["env"]}
assert environment["UIB_PROTOCOL"]["value"] == "uet"
assert environment["UIB_BIND_PORT"]["value"] == "5432"
assert environment["UIB_UET_CC"]["value"] == "nscc"
