# SPDX-License-Identifier: Apache-2.0

import json
import sys

with open(sys.argv[1], encoding="utf-8") as source:
    evidence = json.load(source)
with open(sys.argv[2], encoding="utf-8") as source:
    topology = json.load(source)
assert evidence["scope"] == "worker-data-host-tap-ingress"
assert {endpoint["tap"] for endpoint in evidence["endpoints"].values()} == {
    "tapw1", "tapw2"}
assert topology["ownership"]["taps"] == ["tapw1", "tapw2"]
assert len(topology["ownership"]["ifbs"]) == 2
