# SPDX-License-Identifier: Apache-2.0

import json
import sys


with open(sys.argv[1], encoding="utf-8") as source:
    ledger = json.load(source)
assert ledger["clusterManager"] == "kubernetes"
assert ledger["state"] == "ready"
assert ledger["kubeconfig"] == "kubeconfig"
with open(sys.argv[2], encoding="utf-8") as source:
    status = json.load(source)
assert status["state"] == "ready"
