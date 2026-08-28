# SPDX-License-Identifier: Apache-2.0

import json
import os
import sys


state = sys.argv[1]
with open(os.path.join(state, "ledger.json"), encoding="utf-8") as source:
    ledger = json.load(source)
assert ledger["state"] == "ready"
assert ledger["provider"] == "virtme"
assert ledger["kubeconfig"] == "kubeconfig"
assert ledger["k3sBinary"].endswith("/fakes/k3s")
assert ledger["networkEvidence"] == "network.json"
with open(os.path.join(state, "topology.json"), encoding="utf-8") as source:
    topology = json.load(source)
assert len(topology["ownership"]["processes"]) == 4
assert all(item["pid"] > 1 and item["startTime"]
           for item in topology["ownership"]["processes"])
assert topology["ownership"]["processes"][-1]["node"] == \
    "kubernetes-api-forward"
storage = topology["nodeStorage"]["k3s"]
assert storage["guest"] == "/var/lib/rancher/k3s"
assert storage["device"] == "/dev/vda"
assert storage["filesystem"] == "ext4"
assert storage["size"] == "32Gi"
assert sorted(storage["images"]) == ["worker-01", "worker-02"]
assert all(os.path.isfile(path) for path in storage["images"].values())
with open(os.path.join(state, "network.json"), encoding="utf-8") as source:
    network = json.load(source)
assert network["scope"] == "worker-data-egress"
assert network["interface"] == "eth1"
assert network["mtu"] == 1280
assert network["activeMtu"] == 1024
assert network["shaping"]["enabled"] is True
assert len(network["endpoints"]) == 2
endpoints = {
    endpoint["target"]: endpoint
    for endpoint in network["endpoints"].values()
}
assert endpoints["worker-01/data"]["shaping"]["latency"] == "100us"
assert endpoints["worker-02/data"]["shaping"]["latency"] == "5ms"
for endpoint in endpoints.values():
    assert endpoint["mtu"] == 1280
    assert "qdisc netem " in endpoint["qdisc"]
    assert "Sent 4096 bytes" in endpoint["qdisc"]
    assert endpoint["seedApplied"] is True
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
assert status["kubernetesApiForward"] is True
