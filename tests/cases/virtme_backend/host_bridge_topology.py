# SPDX-License-Identifier: Apache-2.0

import json
import sys

with open(sys.argv[1], encoding="utf-8") as source:
    topology = json.load(source)
entries = []
for name, tap in (("worker-01", "tapw1"), ("worker-02", "tapw2")):
    entries.append({"dev": tap, "mac": topology["guests"][name]["dataMac"]})
with open(sys.argv[2], "w", encoding="utf-8") as output:
    json.dump(entries, output)
