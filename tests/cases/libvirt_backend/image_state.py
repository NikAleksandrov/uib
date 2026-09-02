# SPDX-License-Identifier: Apache-2.0

import json
import sys


with open(sys.argv[1], encoding="utf-8") as source:
    inventory = json.load(source)
image_id = "sha256:" + "a" * 64
image = inventory["images"][image_id]
assert image["cacheKey"] == "a" * 64 + ".sqsh"
assert image["reference"] == "uib-training:test"
assert image["size"] > 0
assert len(image["checksum"]) == 64
assert "/" not in image["cacheKey"]
