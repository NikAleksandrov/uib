# SPDX-License-Identifier: Apache-2.0

import json
import sys

with open(sys.argv[1], encoding="utf-8") as source:
    evidence = json.load(source)
with open(sys.argv[2], encoding="utf-8") as source:
    ownership = json.load(source)
assert evidence["scope"] == "worker-data-host-tap-ingress"
assert evidence["emulationPoint"] == "host-tap-ingress"
assert evidence["oversubscription"] == {"enabled": False}
assert evidence["endpoints"]["worker-01"]["tap"] == "vnet11"
assert "qdisc netem " in evidence["endpoints"]["worker-01"]["qdisc"]
assert ownership["endpoints"]["worker-01"]["ifb"].startswith("ui")
