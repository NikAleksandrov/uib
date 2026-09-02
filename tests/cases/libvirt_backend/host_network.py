# SPDX-License-Identifier: Apache-2.0

import json
import sys

with open(sys.argv[1], encoding="utf-8") as source:
    evidence = json.load(source)
assert evidence["oversubscription"] == {
    "enabled": True,
    "bandwidth": "10Gbit",
    "queueLimit": 4096,
}
ifbs = {endpoint["ifb"] for endpoint in evidence["endpoints"].values()}
assert len(ifbs) == 1
