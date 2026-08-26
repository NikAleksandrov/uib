# SPDX-License-Identifier: Apache-2.0

import json
import sys


with open(sys.argv[1], encoding="utf-8") as source:
    ledger = json.load(source)
image = ledger["images"]["uib-training:test"]
assert image["imageId"] == "sha256:" + "a" * 64
assert image["path"] == "/var/cache/uib/images/" + "a" * 64 + ".sqsh"
assert image["hostCacheKey"] == "a" * 64 + ".sqsh"
