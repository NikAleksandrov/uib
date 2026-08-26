# SPDX-License-Identifier: Apache-2.0

import json
import sys


with open(sys.argv[1], encoding="utf-8") as source:
    config = json.load(source)
assert config["cluster"] == {"manager": "slurm", "version": "24.11.5-4"}
with open(sys.argv[2], encoding="utf-8") as source:
    topology = json.load(source)
assert "kubernetesApiPort" not in topology
assert sorted(topology["guests"]) == ["controller", "worker-01", "worker-02"]
