# SPDX-License-Identifier: Apache-2.0

import json
import sys


with open(sys.argv[1], encoding="utf-8") as source:
    status = json.load(source)
assert status == {
    "provider": "libvirt",
    "runId": "uib-libvirt-test",
    "state": "ready",
}
