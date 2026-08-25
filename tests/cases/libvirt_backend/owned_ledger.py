# SPDX-License-Identifier: Apache-2.0

import json
import sys


with open(sys.argv[1], encoding="utf-8") as source:
    summary = json.load(source)
assert summary["schedulerEvidenceSource"] == "scontrol"
assert summary["schedulerState"] == "COMPLETED"
assert summary["workloadExitCode"] == 0
assert summary["missingArtifacts"] == []
with open(sys.argv[2], encoding="utf-8") as source:
    images = json.load(source)
assert images["stagedImages"] == "staged-images.json"
assert images["stagedCaches"] == "staged-caches.json"
