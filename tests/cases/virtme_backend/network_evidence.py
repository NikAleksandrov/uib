# SPDX-License-Identifier: Apache-2.0

import json
import sys


with open(sys.argv[1], encoding="utf-8") as source:
    entry = json.load(source)["images"]["uib-training:test"]
assert entry["imageId"] == "sha256:" + "a" * 64
assert entry["nodes"] == ["worker-01", "worker-02"]
