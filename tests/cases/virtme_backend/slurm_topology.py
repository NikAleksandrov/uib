# SPDX-License-Identifier: Apache-2.0

import json
import re
import sys


with open(sys.argv[1], encoding="utf-8") as source:
    topology = json.load(source)
network = topology["network"]
assert network["type"] == "linux-bridge"
assert re.fullmatch(r"uibv-[0-9a-f]{8}", network["name"])
assert len(network["name"]) <= 15
assert network["address"] == "198.51.100.1"
assert network["dataInterface"] == "eth0"
assert network["create"][0] == [
    "ip", "link", "add", "name", network["name"], "type", "bridge"]
assert network["delete"] == [
    "ip", "link", "delete", "dev", network["name"], "type", "bridge"]
for guest in topology["guests"].values():
    command = guest["command"]
    assert command[command.index("--network") + 1] == \
        "bridge=" + network["name"]
    assert command[command.index("--net-mac-address") + 1] == \
        guest["dataMac"]
    assert not any(item.startswith("--qemu-opts=") for item in command)
assert topology["ownership"]["bridges"] == [network["name"]]
