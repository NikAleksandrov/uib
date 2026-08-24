# SPDX-License-Identifier: Apache-2.0

import copy
import json
import yaml


def render(config, run_id, allow_mutable_tags):
    from .core import LiteralString, labels, resources, validate_config
    from .environment import environment
    from .storage import init_containers, volume_mounts, volumes
    from .training import trainer_launch

    validate_config(config, run_id, allow_mutable_tags)
    workload = config["workload"]
    training_config = copy.deepcopy(workload["parameters"]["config"])
    training_config["runId"] = run_id
    resource_labels = labels(run_id)
    runtime_name = f"{run_id}-torch"
    image_pull_policy = "IfNotPresent"
    trainer_command, trainer_args = trainer_launch(workload, run_id)
    pod_labels = copy.deepcopy(resource_labels)
    pod_labels["trainer.kubeflow.org/trainjob-ancestor-step"] = "trainer"

    pod_spec = {
        "restartPolicy": "Never",
        "affinity": {
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
        },
        "containers": [
            {
                "name": "node",
                "image": workload["image"],
                "imagePullPolicy": image_pull_policy,
                "volumeMounts": volume_mounts(config),
            }
        ],
        "volumes": volumes(config),
    }
    initializers = init_containers(config, image_pull_policy)
    if initializers:
        pod_spec["initContainers"] = initializers

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
                    json.dumps(training_config, indent=2, sort_keys=True) + "\n"
                )
            },
        },
        {
            "apiVersion": "trainer.kubeflow.org/v1alpha1",
            "kind": "TrainingRuntime",
            "metadata": {
                "name": runtime_name,
                "namespace": run_id,
                "labels": {
                    **resource_labels,
                    "trainer.kubeflow.org/framework": "torch",
                },
            },
            "spec": {
                "mlPolicy": {
                    "numNodes": workload["parallelism"]["nodes"],
                    "torch": {},
                },
                "template": {
                    "metadata": {"labels": resource_labels},
                    "spec": {
                        "replicatedJobs": [
                            {
                                "name": "node",
                                "template": {
                                    "metadata": {
                                        "labels": {
                                            "trainer.kubeflow.org/trainjob-ancestor-step": "trainer"
                                        }
                                    },
                                    "spec": {
                                        "template": {
                                            "metadata": {"labels": pod_labels},
                                            "spec": pod_spec,
                                        }
                                    },
                                },
                            }
                        ]
                    },
                },
            },
        },
        {
            "apiVersion": "trainer.kubeflow.org/v1alpha1",
            "kind": "TrainJob",
            "metadata": {
                "name": run_id,
                "namespace": run_id,
                "labels": resource_labels,
            },
            "spec": {
                "runtimeRef": {
                    "apiGroup": "trainer.kubeflow.org",
                    "kind": "TrainingRuntime",
                    "name": runtime_name,
                },
                "trainer": {
                    "image": workload["image"],
                    "command": trainer_command,
                    "args": trainer_args,
                    "env": environment(config),
                    "numNodes": workload["parallelism"]["nodes"],
                    "numProcPerNode": workload["parallelism"]["processesPerNode"],
                    "resourcesPerNode": resources(config),
                },
            },
        },
    ]
    print(
        yaml.safe_dump_all(
            documents, explicit_start=True, sort_keys=False, width=100
        ),
        end="",
    )
