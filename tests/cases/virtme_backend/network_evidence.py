# SPDX-License-Identifier: Apache-2.0

import json
import sys


with open(sys.argv[1], encoding="utf-8") as source:
    images = json.load(source)["images"]
assert images["uib-training:test"]["imageId"] == "sha256:" + "a" * 64
assert images["uib-training:test"]["nodes"] == ["worker-01", "worker-02"]
assert images["uib-training:test"]["platform"] == "linux/amd64"
assert images["uib-submitter:test"]["imageId"] == "sha256:" + "a" * 64
assert images["uib-submitter:test"]["nodes"] == [
    "controller", "worker-01", "worker-02"]
assert images["uib-submitter:test"]["platform"] == "linux/amd64"
