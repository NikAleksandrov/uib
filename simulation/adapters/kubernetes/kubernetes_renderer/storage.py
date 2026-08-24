# SPDX-License-Identifier: Apache-2.0

import copy


def storage_volume(name, storage, claim):
    volume = {"name": name}
    if claim in storage:
        volume["persistentVolumeClaim"] = {"claimName": storage[claim]}
    else:
        volume["emptyDir"] = {}
    return volume


def volumes(config):
    storage = config["storage"]
    result = [
        {"name": "workload-config", "configMap": {"name": "workload-config"}},
        storage_volume("results", storage, "resultsClaim"),
        storage_volume("model-cache", storage, "modelCacheClaim"),
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
    result = []
    cache_image = config["storage"].get("modelCacheImage")
    if cache_image:
        result.append(
            {
                "name": "model-cache",
                "image": cache_image,
                "imagePullPolicy": image_pull_policy,
                "command": ["/bin/sh", "-c"],
                "args": [
                    "mkdir -p /cache/huggingface && "
                    "cp -a /model-cache/huggingface/. /cache/huggingface/"
                ],
                "volumeMounts": [
                    {"name": "model-cache", "mountPath": "/cache"}
                ],
            }
        )
    if not config["uib"]["enabled"]:
        return result
    runtime_mount = [{"name": "uib-runtime", "mountPath": "/opt/uib"}]
    result.extend([
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
    ])
    return result
