# SPDX-License-Identifier: Apache-2.0

import json
import os
import sys


with open(sys.argv[1], encoding="utf-8") as source:
    assert json.load(source)["state"] == "destroyed"
with open(sys.argv[2], encoding="utf-8") as source:
    topology = json.load(source)
assert not any(os.path.exists(path) for path in
               topology["nodeStorage"]["k3s"]["images"].values())
