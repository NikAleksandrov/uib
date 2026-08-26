# SPDX-License-Identifier: Apache-2.0

import json
import sys


with open(sys.argv[1], encoding="utf-8") as source:
    topology = json.load(source)
for guest in topology["guests"].values():
    command = guest["command"]
    assert command[2] == "/boot/vmlinuz-test"
    assert command.count("--append") == 2
    assert "panic=1" in command
    assert "iommu=pt" in command
