# SPDX-License-Identifier: Apache-2.0

import json
import sys


with open(sys.argv[1], encoding="utf-8") as source:
    evidence = json.load(source)
assert evidence["runId"] == "uib-libvirt-test"
assert evidence["scope"] == "worker-data-egress"
assert evidence["interface"] == "eth1"
assert evidence["mtu"] == 1500
assert evidence["activeMtu"] == 1024
assert evidence["shaping"] == {"enabled": False}
assert sorted(evidence["endpoints"]) == ["worker-01", "worker-02"]
assert all(endpoint["mtu"] == 1500
           for endpoint in evidence["endpoints"].values())
assert all(endpoint["seedApplied"] is None
           for endpoint in evidence["endpoints"].values())
