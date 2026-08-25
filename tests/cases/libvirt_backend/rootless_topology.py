# SPDX-License-Identifier: Apache-2.0

import json
import sys


with open(sys.argv[1], encoding="utf-8") as source:
    ledger = json.load(source)
assert ledger["schemaVersion"] == 1
assert ledger["runId"] == "uib-libvirt-test"
assert ledger["provider"] == "libvirt"
assert ledger["clusterManager"] == "slurm"
assert ledger["state"] == "ready"
