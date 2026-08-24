# SPDX-License-Identifier: Apache-2.0

import copy


def volumes(config):
    result = [
        {"name": "workload-config", "configMap": {"name": "workload-config"}},
        {"name": "results", "emptyDir": {}},
        {"name": "model-cache", "emptyDir": {}},
    ]
    if config["uib"]["enabled"]:
        result.append({"name": "uib-runtime", "emptyDir": {}})
    return result


def volume_mounts(config):
    result = [
        {
            "name": "workload-config",
            "mountPath": "/etc/uib-workload",
            "readOnly": True,
        },
        {"name": "results", "mountPath": "/results"},
        {"name": "model-cache", "mountPath": "/cache"},
    ]
    if config["uib"]["enabled"]:
        result.append({"name": "uib-runtime", "mountPath": "/opt/uib"})
    return result


def init_containers(config, image_pull_policy):
    if not config["uib"]["enabled"]:
        return []
    runtime_mount = [{"name": "uib-runtime", "mountPath": "/opt/uib"}]
    return [
        {
            "name": "uib-runtime",
            "image": config["uib"]["bundle"],
            "imagePullPolicy": image_pull_policy,
            "command": ["/bin/sh", "-c"],
            "args": ["cp -a /uib-runtime/. /opt/uib/"],
            "volumeMounts": copy.deepcopy(runtime_mount),
        },
        {
            "name": "uib-runtime-check",
            "image": config["workload"]["image"],
            "imagePullPolicy": image_pull_policy,
            "command": ["/opt/uib/bin/uib-preflight"],
            "args": ["--library-only"],
            "volumeMounts": copy.deepcopy(runtime_mount),
        },
    ]
