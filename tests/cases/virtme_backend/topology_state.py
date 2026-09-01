# SPDX-License-Identifier: Apache-2.0

import json
import os
import signal
import sys
import time


with open(sys.argv[1], encoding="utf-8") as source:
    processes = json.load(source)["ownership"]["processes"]
worker = next(item for item in processes
              if item["node"].endswith("-worker-02"))
os.killpg(worker["pid"], signal.SIGTERM)
deadline = time.monotonic() + 5
while time.monotonic() < deadline:
    try:
        with open(f"/proc/{worker['pid']}/stat", encoding="utf-8") as source:
            fields = source.read().rpartition(") ")[2].split()
    except OSError:
        break
    if len(fields) <= 19 or fields[19] != worker["startTime"]:
        break
    time.sleep(0.05)
else:
    raise SystemExit("terminated virtme worker is still running")
