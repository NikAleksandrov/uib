# SPDX-License-Identifier: Apache-2.0

import copy
import json
import shlex
import yaml


def ray_pod_template(config, role, image_pull_policy):
    from .core import labels, resources
    from .environment import environment
    from .storage import init_containers, volume_mounts, volumes

    run_id = config["runId"]
    pod_labels = {**labels(run_id), "uib.dev/ray-role": role}
    pod_spec = {
        "restartPolicy": "Never",
        "containers": [
            {
                "name": f"ray-{role}",
                "image": config["workload"]["image"],
                "imagePullPolicy": image_pull_policy,
                "env": environment(config),
                "resources": resources(config),
                "volumeMounts": volume_mounts(config),
            }
        ],
        "volumes": volumes(config),
    }
    if config["workload"]["parallelism"]["nodes"] > 1:
        pod_spec["affinity"] = {
            "podAntiAffinity": {
                "requiredDuringSchedulingIgnoredDuringExecution": [
                    {
                        "labelSelector": {
                            "matchLabels": {"uib.dev/run-id": run_id}
                        },
                        "topologyKey": "kubernetes.io/hostname",
                    }
                ]
            }
        }
    initializers = init_containers(config, image_pull_policy)
    if initializers:
        pod_spec["initContainers"] = initializers
    return {
        "metadata": {"labels": pod_labels},
        "spec": pod_spec,
    }


def render_inference(config, run_id, allow_mutable_tags):
    from .core import LiteralString, labels, validate_config

    validate_config(config, run_id, allow_mutable_tags)
    config = copy.deepcopy(config)
    config["runId"] = run_id
    workload = config["workload"]
    inference_config = copy.deepcopy(workload["parameters"]["config"])
    inference_config["runId"] = run_id
    resource_labels = labels(run_id)
    image_pull_policy = "IfNotPresent"
    config_path = "/etc/uib-workload/workload.json"
    executable = shlex.quote(workload["entrypoint"])
    quoted_config = shlex.quote(config_path)
    lifecycle = (
        "set -eu; "
        f"{executable} serve --config {quoted_config} & server=$!; "
        "cleanup() { kill \"$server\" >/dev/null 2>&1 || true; }; "
        "trap cleanup EXIT INT TERM; "
        f"{executable} probe --config {quoted_config}; "
        "kill \"$server\" >/dev/null 2>&1 || true; "
        "wait \"$server\" || true; trap - EXIT INT TERM"
    )
    entrypoint = LiteralString(shlex.join(["/bin/sh", "-c", lifecycle]))
    ray_cluster = {
        "rayVersion": "2.58.0",
        "headGroupSpec": {
            "rayStartParams": {"dashboard-host": "0.0.0.0"},
            "template": ray_pod_template(config, "head", image_pull_policy),
        },
    }
    worker_count = workload["parallelism"]["nodes"] - 1
    if worker_count:
        ray_cluster["workerGroupSpecs"] = [
            {
                "groupName": "worker",
                "replicas": worker_count,
                "minReplicas": worker_count,
                "maxReplicas": worker_count,
                "rayStartParams": {},
                "template": ray_pod_template(
                    config, "worker", image_pull_policy
                ),
            }
        ]
    documents = [
        {
            "apiVersion": "v1",
            "kind": "Namespace",
            "metadata": {"name": run_id, "labels": resource_labels},
        },
        {
            "apiVersion": "v1",
            "kind": "ConfigMap",
            "metadata": {
                "name": "workload-config",
                "namespace": run_id,
                "labels": resource_labels,
            },
            "data": {
                "workload.json": LiteralString(
                    json.dumps(inference_config, indent=2, sort_keys=True) + "\n"
                )
            },
        },
        {
            "apiVersion": "ray.io/v1",
            "kind": "RayJob",
            "metadata": {
                "name": run_id,
                "namespace": run_id,
                "labels": resource_labels,
            },
            "spec": {
                "entrypoint": entrypoint,
                "shutdownAfterJobFinishes": True,
                "ttlSecondsAfterFinished": 60,
                "rayClusterSpec": ray_cluster,
            },
        },
    ]
    print(
        yaml.safe_dump_all(
            documents, explicit_start=True, sort_keys=False, width=100
        ),
        end="",
    )
