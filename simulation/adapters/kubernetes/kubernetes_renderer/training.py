# SPDX-License-Identifier: Apache-2.0

def trainer_launch(workload, run_id):
    script = (
        'exec torchrun --nnodes="$PET_NNODES" '
        '--nproc-per-node="$PET_NPROC_PER_NODE" '
        '--node-rank="$PET_NODE_RANK" --rdzv-backend=c10d '
        '--rdzv-endpoint="$PET_MASTER_ADDR:$PET_MASTER_PORT" '
        f'--rdzv-id={run_id} --no-python "$@"'
    )
    return ["/bin/sh", "-c"], [
        script,
        "uib-kubeflow",
        workload["entrypoint"],
        *workload.get("arguments", []),
    ]
