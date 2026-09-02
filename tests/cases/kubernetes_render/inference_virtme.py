# SPDX-License-Identifier: Apache-2.0

import sys

import yaml


with open(sys.argv[1], encoding="utf-8") as source:
    job = list(yaml.safe_load_all(source))[2]
spec = job["spec"]
submitter_spec = spec["submitterPodTemplate"]["spec"]
submitter = submitter_spec["containers"][0]
assert submitter["name"] == "ray-job-submitter"
assert submitter["image"] == (
    "docker.io/rayproject/ray:2.58.0-py312-cpu@sha256:"
    "c3c9573c5c6bfe4127885f79622d6a32064d34cafc7d156ec728aab8657be250"
)
assert submitter["imagePullPolicy"] == "IfNotPresent"
assert submitter["resources"]["requests"] == {
    "cpu": "50m", "memory": "64Mi"}
assert submitter["resources"]["limits"] == {
    "cpu": "100m", "memory": "128Mi"}
assert submitter_spec["restartPolicy"] == "Never"
assert submitter_spec["nodeSelector"] == {
    "node-role.kubernetes.io/control-plane": "true"}
assert submitter_spec["tolerations"] == [{
    "key": "node-role.kubernetes.io/control-plane",
    "operator": "Exists",
    "effect": "NoSchedule",
}]
params = spec["rayClusterSpec"]["headGroupSpec"]["rayStartParams"]
assert params == {
    "dashboard-host": "0.0.0.0",
    "memory": "805306368",
    "object-store-memory": "134217728",
}
