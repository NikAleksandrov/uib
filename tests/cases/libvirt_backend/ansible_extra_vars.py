# SPDX-License-Identifier: Apache-2.0

import grp
import json
import pwd
import sys


with open(sys.argv[1], encoding="utf-8") as source:
    values = json.load(source)
assert values["run_id"] == "uib-libvirt-test"
assert values["management_cidr"] == "192.0.2.0/24"
assert values["data_cidr"] == "198.51.100.0/24"
assert values["nodes"]["controller"]["management_ip"] == "192.0.2.10"
assert values["nodes"]["controller"]["data_ip"] == "198.51.100.10"
assert values["nodes"]["worker-01"]["management_ip"] == "192.0.2.21"
assert values["nodes"]["worker-02"]["data_ip"] == "198.51.100.22"
assert values["nodes"]["worker-01"]["cpu"] == 2
assert values["nodes"]["worker-01"]["memory_mib"] == 4096
assert values["nodes"]["worker-01"]["disk_bytes"] == 16 * 1024 ** 3
macs = {
    node[key]
    for node in values["nodes"].values()
    for key in ("management_mac", "data_mac")
}
assert len(macs) == 6
assert all(mac.startswith("52:54:00:") for mac in macs)
account = pwd.getpwuid(int(values["volume_owner"]))
try:
    group = grp.getgrnam(account.pw_name).gr_gid
except KeyError:
    group = account.pw_gid
assert values["volume_group"] == str(group)
