# SPDX-License-Identifier: Apache-2.0

import ipaddress
import json
import sys


with open(sys.argv[1], encoding="utf-8") as source:
    values = json.load(source)
assert values["network_mode"] == "rootless"
assert ipaddress.ip_address(values["multicast_address"]).is_multicast
assert 30000 <= values["multicast_port"] < 50000
assert values["nodes"]["controller"]["ssh_port"] == 31000
assert values["nodes"]["worker-01"]["ssh_port"] == 31001
