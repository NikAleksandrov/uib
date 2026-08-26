# SPDX-License-Identifier: Apache-2.0

import json
import re
import sys


with open(sys.argv[1], encoding="utf-8") as source:
    topology = json.load(source)
assert topology["schemaVersion"] == 1
assert topology["runId"] == "uib-virtme-test"
assert topology["provider"] == "virtme"
assert topology["state"] == "rendered"
bridge = topology["bridge"]
assert re.fullmatch(r"uibv-[0-9a-f]{8}", bridge["name"])
assert len(bridge["name"]) <= 15
assert bridge["address"] == "198.51.100.1"
assert bridge["prefix"] == 24
assert bridge["create"][0] == [
    "ip", "link", "add", "name", bridge["name"], "type", "bridge"]
assert bridge["delete"] == [
    "ip", "link", "delete", "dev", bridge["name"], "type", "bridge"]
guests = topology["guests"]
assert sorted(guests) == ["controller", "worker-01", "worker-02"]
assert guests["controller"]["dataAddress"] == "198.51.100.10"
assert guests["worker-01"]["dataAddress"] == "198.51.100.21"
assert guests["worker-02"]["dataAddress"] == "198.51.100.22"
assert [guests[name]["sshPort"] for name in sorted(guests)] == [
    23000, 23001, 23002]
assert len({guest["dataMac"] for guest in guests.values()}) == 3
for guest in guests.values():
    command = guest["command"]
    assert command[:2] == ["vng", "--run"]
    assert command[command.index("--cpus") + 1] == "2"
    assert command[command.index("--memory") + 1] == "4096M"
    assert command[command.index("--network") + 1] == \
        "bridge=" + bridge["name"]
    assert command[command.index("--ssh") + 1] == str(guest["sshPort"])
assert topology["ownership"] == {
    "bridges": [bridge["name"]],
    "processes": [],
    "taps": [],
    "temporaryRoots": [],
}
