# SPDX-License-Identifier: Apache-2.0

import json
import sys


with open(sys.argv[1], encoding="utf-8") as source:
    topology = json.load(source)
assert topology["schemaVersion"] == 1
assert topology["runId"] == "uib-virtme-test"
assert topology["provider"] == "virtme"
assert topology["state"] == "rendered"
network = topology["network"]
assert network["type"] == "qemu-socket"
assert network["multicastAddress"].startswith("239.255.")
assert 30000 <= network["multicastPort"] < 50000
assert network["dataInterface"] == "eth1"
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
    assert command[command.index("--ssh") + 1] == str(guest["sshPort"])
    assert "--ssh-tcp" in command
    qemu = next(item for item in command if item.startswith("--qemu-opts="))
    assert "-netdev socket,id=data,mcast=" in qemu
    assert guest["dataMac"] in qemu
assert topology["ownership"] == {
    "bridges": [],
    "processes": [],
    "taps": [],
    "temporaryRoots": [],
}
