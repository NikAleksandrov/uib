# SPDX-License-Identifier: Apache-2.0

import json
import sys


with open(sys.argv[1], encoding="utf-8") as source:
    ledger = json.load(source)
cache = ledger["caches"]["huggingface"]
assert cache["path"] == "/var/cache/uib/models/huggingface"
assert cache["sourceKey"] == "huggingface"
assert cache["archiveSize"] > 0
assert len(cache["archiveSha256"]) == 64
with open(sys.argv[2], encoding="utf-8") as source:
    inventory = json.load(source)
assert inventory["caches"]["huggingface"]["archiveSha256"] == \
    cache["archiveSha256"]
assert "/" not in cache["sourceKey"]
