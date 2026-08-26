# SPDX-License-Identifier: Apache-2.0

import json
import os
import sys


state = sys.argv[1]
with open(os.path.join(state, "ledger.json"), encoding="utf-8") as source:
    ledger = json.load(source)
assert ledger["state"] == "ready"
assert ledger["provider"] == "virtme"
with open(os.path.join(state, "topology.json"), encoding="utf-8") as source:
    topology = json.load(source)
assert len(topology["ownership"]["processes"]) == 3
assert all(item["pid"] > 1 and item["startTime"]
           for item in topology["ownership"]["processes"])
with open(os.path.join(state, "inventory.json"), encoding="utf-8") as source:
    inventory = json.load(source)
assert inventory["all"]["vars"]["ansible_user"] == "root"
assert sorted(inventory["all"]["children"]["kubernetes_cluster"]["hosts"]) == [
    "controller", "worker-01", "worker-02"]
with open(sys.argv[2], encoding="utf-8") as source:
    status = json.load(source)
assert status["state"] == "ready"
assert status["guests"] == status["liveGuests"] == 3
assert status["recordedGuests"] == 3
