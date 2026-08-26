# SPDX-License-Identifier: Apache-2.0

import importlib.machinery
import importlib.util
import sys


loader = importlib.machinery.SourceFileLoader("uib_kubernetes_render", sys.argv[1])
spec = importlib.util.spec_from_loader(loader.name, loader)
renderer = importlib.util.module_from_spec(spec)
loader.exec_module(renderer)
config = {
    "communication": {"frontend": "ucx"},
    "storage": {},
    "uib": {"enabled": True},
}
volumes = {item["name"]: item for item in renderer.volumes(config)}
mounts = {item["name"]: item for item in renderer.volume_mounts(config)}
assert volumes["uib-device-anchor"]["hostPath"] == {
    "path": "/dev/null",
    "type": "CharDevice",
}
assert mounts["uib-device-anchor"] == {
    "name": "uib-device-anchor",
    "mountPath": "/dev/infiniband/uib0",
}
config["uib"]["enabled"] = False
assert not renderer.needs_device_anchor(config)
