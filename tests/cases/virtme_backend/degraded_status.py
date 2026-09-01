# SPDX-License-Identifier: Apache-2.0

import json
import sys


with open(sys.argv[1], encoding="utf-8") as source:
    status = json.load(source)
assert status["state"] == "degraded"
assert status["recordedGuests"] == 3
assert status["liveGuests"] == 2
