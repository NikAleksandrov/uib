<!-- SPDX-License-Identifier: Apache-2.0 -->

# LLM cluster simulation and real-hardware validation

The goal is to run standard distributed LLM training and inference software
through UIB without modifying the application. The same workload image,
entry point, model, data, parallelism, NCCL settings, and measurements should
move from a laptop simulation to a real GPU/RDMA cluster by changing a small
infrastructure profile.

The design supports two real-world cluster styles:

- Kubernetes with Helm, Kubeflow Trainer, KubeRay, and vLLM.
- Slurm with `sbatch`, `srun`, Pyxis, and Enroot.

Both styles use the same OCI workload images and scheduler-neutral workload
configuration.

## Contents

- [What the harness validates](#what-the-harness-validates)
- [Architecture](#architecture)
- [Layer model](#layer-model)
- [Common configuration](#common-configuration)
- [Available deployment compositions](#available-deployment-compositions)
- [NCCL compatibility comes first](#nccl-compatibility-comes-first)
- [Transparent UIB injection](#transparent-uib-injection)
- [Runtime artifact](#runtime-artifact)
- [Kubernetes execution stack](#kubernetes-execution-stack)
- [Slurm, Pyxis, and Enroot execution stack](#slurm-pyxis-and-enroot-execution-stack)
- [Training workloads](#training-workloads)
- [Inference workloads](#inference-workloads)
- [Network and failure simulation](#network-and-failure-simulation)
- [Changing one layer](#changing-one-layer)
- [Running a simulation](#running-a-simulation)
- [Moving to real hardware](#moving-to-real-hardware)
- [Results and evidence](#results-and-evidence)
- [Cleanup and safety](#cleanup-and-safety)
- [Limitations](#limitations)

## What the harness validates

A successful application exit is not enough. A validation run must prove:

1. NCCL loaded UIB's `libuibv.so` with the expected libibverbs ABI.
2. NCCL selected its `IB` network and the synthetic `uib0` device.
3. Collective data did not silently fall back to NCCL sockets.
4. Every requested rank ran on the expected node and GPU.
5. The selected UIB protocol, transport, memory mode, and network model were
   actually active.
6. Workload correctness checks passed.
7. The run captured enough configuration, version, topology, and metric data
   to reproduce it.

Results are classified by capability:

| Level | Meaning |
|---|---|
| L0 | CPU-only orchestration, deployment, network, cleanup, and ordinary verbs validation |
| L1 | A real multi-node GPU workload uses host-staged NCCL traffic through UIB |
| L2 | A real multi-node GPU workload uses a validated UIB GPU-memory or CUDA DMA-BUF path |
| L3 | Production-like scale, physical GPU/RDMA hardware, failures, congestion, and native-verbs comparison |

Kind or CPU-only VMs can provide valuable L0 coverage, but they are not NCCL
or GPU performance simulators.

## Architecture

The architecture is layered so infrastructure, scheduler, container runtime,
launcher, transport, networking, and metrics can change independently.

```text
                         simulation request
                    workload + profile overlays
                                 |
                                 v
                 +-------------------------------+
                 | uib-sim                       |
                 | schema, resolve, diff, own,   |
                 | deploy, collect, clean        |
                 +-------------------------------+
                                 |
               +-----------------+-----------------+
               |                                   |
               v                                   v
     +-------------------------+         +-------------------------+
     | Kubernetes adapter      |         | Slurm adapter           |
     | Helm                    |         | sbatch / srun           |
     | Kubeflow Trainer        |         | Pyxis                   |
     | KubeRay / RayService    |         | Enroot                  |
     +-------------------------+         +-------------------------+
               |                                   |
               +-----------------+-----------------+
                                 |
                                 v
              +----------------------------------------+
              | Common workload contract               |
              | OCI digest, command, args, env, ranks, |
              | model, data, checkpoints, measurements |
              +----------------------------------------+
                                 |
                                 v
              +----------------------------------------+
              | Communication adapter                  |
              | UIB libuibv.so or native libibverbs    |
              | NCCL IB selection and fallback proof   |
              +----------------------------------------+
                                 |
                                 v
              +----------------------------------------+
              | Network and storage                    |
              | CNI / tap / bridge / physical fabric   |
              | netem or real links; PVC or shared FS  |
              +----------------------------------------+
                                 |
                                 v
      +---------------+---------------+---------------+---------------+
      | Kind          | virtme-ng     | libvirt VMs  | real nodes    |
      | local and CI  | kernel tests  | local lab    | GPU and RDMA  |
      +---------------+---------------+---------------+---------------+
                                 |
                                 v
              +----------------------------------------+
              | Common evidence                        |
              | logs, metrics, traces, versions,       |
              | resolved config, faults, ownership     |
              +----------------------------------------+
```

The scheduler-specific paths converge at the container boundary:

```text
                           one workload request
                                    |
                  +-----------------+-----------------+
                  |                                   |
                  v                                   v
        Kubernetes production style         NVIDIA/HPC production style
        ---------------------------         ----------------------------
        Helm values                         resolved values
             |                                   |
        Kubeflow TrainJob                  sbatch allocation
        or KubeRay RayService                   |
             |                              srun + Pyxis
        containerd / OCI                        |
             |                              Enroot / OCI
             +-----------------+----------------+
                               |
                    same image and entry point
                               |
                    NCCL -> libuibv.so -> UIB
```

## Layer model

### Infrastructure

The infrastructure layer creates or discovers nodes. It returns an inventory
and an explicit ownership ledger.

| Implementation | Best use |
|---|---|
| Kind | Fast local and CI Kubernetes lifecycle tests |
| virtme-ng | Disposable kernel, namespace, and transport tests |
| OpenTofu with libvirt | Realistic multi-node local Kubernetes or Slurm lab |
| Existing nodes | Real Kubernetes or Slurm GPU/RDMA validation |

Infrastructure does not know which model is being trained or served.

### Node provisioning

Ansible is the default mutable provisioner for virtme and libvirt nodes. It
installs one of these node stacks:

- containerd, kubeadm or k3s, CNI, and Helm;
- Munge, Slurm, Enroot, and Pyxis;
- shared base dependencies such as UIB, CUDA diagnostics, iproute2, and
  monitoring tools.

An immutable prebuilt image or an administrator-managed real cluster can
replace Ansible if it produces the same capability report.

### Cluster manager

The cluster manager owns job submission and cancellation:

- Kubernetes accepts rendered Helm and custom resources.
- Slurm accepts rendered `sbatch` files and launches job steps with `srun`.

The cluster manager does not change model or benchmark parameters.

### Container runtime

- Kubernetes normally runs OCI images with containerd through CRI.
- Slurm normally lets Pyxis ask Enroot to import and run the same OCI image.
- Apptainer can be added as a Slurm-site alternative.

The result records both the original OCI digest and any locally converted
Enroot or Apptainer artifact.

### Workload launcher

Training launchers may be Kubeflow Trainer, Slurm, or a debugging JobSet.
Inference launchers may be KubeRay, Slurm-managed Ray, or direct multi-node
vLLM. Each launcher consumes the same resolved workload contract.

### Communication

The communication layer selects either:

- UIB through `libuibv.so`; or
- the system's native libibverbs implementation as a baseline.

A future NCCL network plugin could be another adapter, but it would be labeled
separately because it would not test transparent libibverbs substitution.

### Network

The network adapter maps a logical topology onto pod interfaces, VM taps,
bridges, or physical interfaces. It applies `tc` qdiscs for synthetic links and
collects physical counters on real fabrics.

### Storage

PVCs, NFS, parallel filesystems, object storage, and node-local caches are
interchangeable behind model, dataset, checkpoint, and result mount contracts.
All variants retain immutable model and dataset revisions.

### Evidence

Prometheus, DCGM exporter, Kubernetes events, Slurm accounting, and direct
process sampling can be used together or separately. Raw evidence is mapped
into a common result schema rather than discarded.

## Common configuration

The harness accepts a scheduler-neutral request plus a deployment profile.
Both are validated with JSON Schema. Unknown keys and impossible parallelism
must fail before infrastructure is created.

### Workload request example

```yaml
workload:
  kind: training
  image: registry.example/uib-llm@sha256:<digest>
  entrypoint: /opt/workload/bin/train
  model:
    id: example/model
    revision: <immutable-revision>
  dataset:
    id: example/dataset
    revision: <immutable-revision>
  parallelism:
    nodes: 4
    processesPerNode: 8
    data: 4
    tensor: 4
    pipeline: 2
    expert: 1
  training:
    sequenceLength: 4096
    globalBatchSize: 512
    microBatchSize: 2
    precision: bf16
    steps: 200
    warmupSteps: 20
uib:
  enabled: true
  bundle: registry.example/uib-runtime@sha256:<digest>
  protocol: rocev2
  transport: udp-io-uring
  memoryMode: host-staged
nccl:
  network: IB
  hca: "=uib0"
  fallbackPolicy: fail
measurement:
  seed: 12345
```

The values above illustrate the shape of the request. Schema validation must
enforce the selected framework's exact DP, TP, PP, EP, node, and process
relationships.

### Infrastructure profile example

```yaml
infrastructure:
  provider: libvirt
  controlPlanes: 1
  workers: 4
  worker:
    cpu: 16
    memory: 64Gi
    disk: 100Gi
cluster:
  manager: kubernetes
  version: <pinned-version>
container:
  runtime: containerd
launcher:
  training: kubeflow
  inference: kuberay
network:
  managementCIDR: 192.0.2.0/24
  dataCIDR: 198.51.100.0/24
```

### Network profile example

```yaml
network:
  topology: leaf-spine
  oversubscription: "2:1"
  mtu: 4096
  shaping:
    enabled: true
    bandwidth: 25Gbit
    latency: 100us
    jitter: 10us
    jitterDistribution: normal
    loss:
      model: random
      percent: 0.01
    reorderPercent: 0
    queueLimit: 8192
    seed: 12345
```

### Inference request example

```yaml
workload:
  kind: inference
  image: registry.example/uib-vllm@sha256:<digest>
  entrypoint: /opt/workload/bin/serve
  model:
    id: example/model
    revision: <immutable-revision>
  parallelism:
    nodes: 2
    tensor: 8
    pipeline: 2
    data: 1
  inference:
    maxModelLength: 32768
    gpuMemoryUtilization: 0.9
    replicas: 1
    streaming: true
load:
  mode: open-loop
  requestRate: 20
  concurrency: 128
  promptLength:
    distribution: normal
    mean: 2048
    deviation: 256
  outputLength:
    distribution: fixed
    value: 256
  warmupRequests: 100
  duration: 10m
  seed: 12345
```

### Fault schedule example

```yaml
faults:
  - at: 2m
    action: add-delay
    target: worker-2/data
    latency: 2ms
    duration: 30s
  - at: 4m
    action: packet-loss
    target: rack-b/uplink
    percent: 1
    duration: 15s
  - afterEvent: checkpoint-complete
    action: terminate-rank
    rank: 7
```

## Available deployment compositions

| Profile | Infrastructure | Manager | Runtime | Launcher | Normal level |
|---|---|---|---|---|---|
| `kind-kubernetes` | Kind | Kubernetes | containerd | Kubeflow/KubeRay | L0 |
| `virtme-kind` | One virtme guest with Kind | Kubernetes | containerd | Kubeflow/KubeRay | L0 |
| `virtme-kubernetes` | Multiple virtme guests | k3s or kubeadm | containerd | Kubeflow/KubeRay | L0 |
| `virtme-slurm` | Multiple virtme guests | Slurm | Pyxis/Enroot | `sbatch`/`srun` | L0 |
| `libvirt-kubernetes` | OpenTofu/libvirt VMs | Kubernetes | containerd | Kubeflow/KubeRay | L0 or L1 with passthrough |
| `libvirt-slurm` | OpenTofu/libvirt VMs | Slurm | Pyxis/Enroot | `sbatch`/`srun` | L0 or L1 with passthrough |
| `real-kubernetes` | Existing hardware | Kubernetes | site CRI | Kubeflow/KubeRay | L1-L3 |
| `real-slurm` | Existing hardware | Slurm | Pyxis/Enroot | `sbatch`/`srun` | L1-L3 |

## NCCL compatibility comes first

Current NCCL can open a verbs library explicitly and resolve versioned symbols
with `dlvsym()`. UIB therefore needs more than an ordinary unversioned
`LD_PRELOAD` library. UIB now provides the versioned loader ABI and its
automated `provider-nccl-abi-test`. The opt-in `nccl-test` additionally runs
an unchanged, two-rank `nccl-tests` all-reduce through UIB and rejects Socket
fallback.

The compatibility sequence is:

1. export core calls with `IBVERBS_1.1` symbol versions (implemented);
2. version `ibv_reg_mr_iova2()` and `ibv_reg_dmabuf_mr()` appropriately
   (implemented);
3. implement the mandatory calls
   `ibv_get_async_event()`, `ibv_ack_async_event()`, `ibv_fork_init()`, and
   `ibv_event_type_str()` (implemented);
4. deliberately reject absent optional ECE and port-speed calls
   (implemented);
5. test verbs operations reached through `struct ibv_context_ops`
   (implemented);
6. test current NCCL's exact symbol contract automatically (implemented);
7. run unmodified NCCL tests and reject Socket fallback (implemented for the
   local host-staged profile).

UIB async events need a real eventfd-backed queue. A stub must not leave NCCL
polling an invalid descriptor or busy-looping.

Initial GPU validation should use host staging. GPU-direct validation remains
a distinct later capability because it requires a complete non-mappable GPU
DMA-BUF TX/RX path.

See the [NCCL verbs loader](https://github.com/NVIDIA/nccl/blob/master/src/misc/ibvsymbols.cc)
and [NCCL environment documentation](https://docs.nvidia.com/deeplearning/nccl/user-guide/docs/env.html).

## Transparent UIB injection

The workload source remains unchanged. The adapter mounts a checksummed UIB
runtime bundle at `/opt/uib` and sets the required environment:

```text
NCCL_IBVERBS_LIB=/opt/uib/libuibv.so
NCCL_NET=IB
NCCL_IB_DISABLE=0
NCCL_IB_HCA==uib0
NCCL_DEBUG=INFO
LD_PRELOAD=/opt/uib/libuibv.so
LD_LIBRARY_PATH=/opt/uib/lib
UIB_BIND_IP=<rank data-plane address>
UIB_PROTOCOL=rocev2
UIB_TRANSPORT_MODE=udp-io-uring
```

The precise HCA filter syntax is validated against the pinned NCCL release.

Three distribution methods are supported:

1. Kubernetes init container copies the UIB bundle into a shared `emptyDir`.
2. Pyxis bind-mounts the same bundle into an Enroot container.
3. A derived OCI image contains the bundle. Both UIB and native runs use the
   same derived image; only environment selection differs.

No method overwrites the host's system libibverbs installation.

Native baseline:

```yaml
uib:
  enabled: false
nccl:
  network: IB
  fallbackPolicy: fail
```

UIB host-staged run:

```yaml
uib:
  enabled: true
  memoryMode: host-staged
nccl:
  network: IB
  environment:
    NCCL_NET_GDR_LEVEL: LOC
```

GPU-direct mode is accepted only after the requested UIB transport passes the
GPU DMA-BUF capability gate.

## Runtime artifact

Every backend consumes one generated filesystem tree. It contains
`libuibv.so`, `libuib.so`, the standalone static archive, rdma-core discovery
tools, non-glibc dynamic dependencies, a compatibility lock, a build manifest,
checksums, and `uib-preflight`. Build and validate it with:

```text
make runtime-bundle-test
make runtime-bundle
```

The second command creates these ignored local artifacts:

```text
simulation/.artifacts/
|-- uib-runtime/
|   |-- bin/
|   |   |-- ibv_devices
|   |   |-- ibv_devinfo
|   |   `-- uib-preflight
|   |-- lib/
|   |   |-- libuib.so
|   |   |-- libuibv.so
|   |   `-- libuib-standalone.a
|   `-- share/uib/
|       |-- compatibility-lock.json
|       |-- runtime-manifest.json
|       `-- SHA256SUMS
`-- uib-runtime.tar.gz
```

The abbreviated tree omits bundled diagnostic tools, dependency libraries,
and their versioned symlinks. `runtime-manifest.json` records the UIB commit,
tracked-source state, compiler, libc, minimum required glibc symbol version,
detected build capabilities, dependency source paths, sizes, and hashes. The
compatibility lock records validated NCCL/CUDA combinations and explicitly
labels unvalidated collectives and GPU-direct operation. On Debian-family
builders, the artifact also records package versions and includes available
copyright files for bundled runtime and static-library dependencies.

The bundle intentionally does not carry glibc or the dynamic loader. Injecting
a private glibc into an arbitrary application process is unsafe. Build on the
oldest supported userspace, and run preflight in the target image or node;
`ldd` rejects a target whose libc cannot satisfy the recorded libraries.

### OCI and Kubernetes init container

Build the init image from the exact generated tree. The Containerfile defaults
to the tested Debian 13 slim digest. Override it only with another digest whose
glibc satisfies the manifest:

```text
docker build -f simulation/runtime/Containerfile \
  --build-arg BASE_IMAGE='registry.example/base@sha256:<digest>' \
  -t registry.example/uib-runtime:<uib-commit> \
  simulation/.artifacts
```

The base also needs a POSIX shell, glibc's `ldd`, `grep`, and `env`. Artifact
signing belongs to the later publishing pipeline; local bundles are protected
by `SHA256SUMS` but are not presented as signed releases.

The init container can use the scheduler's default seccomp profile because it
runs `uib-preflight --library-only`. A workload opening `uib0` must allow
`io_uring_setup`, `io_uring_enter`, and `io_uring_register`; UIB's UDP worker
uses io_uring even when synchronous transmit is selected. Use a site-managed
`Localhost` seccomp profile containing those calls. `Unconfined` is acceptable
for a disposable local simulation but should not be the production default.
The full preflight was tested in the pinned image with Docker's seccomp filter
disabled; Docker's default filter rejected `io_uring_setup` on the test host.

The image stores the tree at `/uib-runtime`. A Kubernetes adapter copies it to
an `emptyDir`; the application container only mounts the result:

```yaml
volumes:
  - name: uib-runtime
    emptyDir: {}
initContainers:
  - name: install-uib
    image: registry.example/uib-runtime@sha256:<digest>
    command: ["/bin/sh", "-ec"]
    args:
      - cp -a /uib-runtime/. /opt/uib/ &&
        /opt/uib/bin/uib-preflight --library-only
    volumeMounts:
      - name: uib-runtime
        mountPath: /opt/uib
containers:
  - name: workload
    volumeMounts:
      - name: uib-runtime
        mountPath: /opt/uib
        readOnly: true
```

The workload image, command, model, and framework remain identical for native
verbs and UIB. Only this mount and the environment in
[Transparent UIB injection](#transparent-uib-injection) change.

### Slurm, Pyxis, and Enroot bundle

Publish `uib-runtime.tar.gz` alongside its SHA-256 digest. Extract it once to a
run-owned or administrator-managed path, verify it, and bind-mount it without
altering the host libibverbs installation:

```text
tar -xzf uib-runtime.tar.gz -C /shared/uib/<uib-commit>
/shared/uib/<uib-commit>/uib-runtime/bin/uib-preflight

srun \
  --container-image=registry.example/uib-llm@sha256:<digest> \
  --container-mounts=/shared/uib/<uib-commit>/uib-runtime:/opt/uib:ro \
  env NCCL_IBVERBS_LIB=/opt/uib/libuibv.so \
      LD_PRELOAD=/opt/uib/libuibv.so \
      LD_LIBRARY_PATH=/opt/uib/lib \
      /opt/workload/bin/train
```

The scheduler adapter records the bundle checksum and manifest with the job.
Kubernetes and Slurm runs are comparable only when those values match. The
same tree may also be copied into a derived workload image when an init
container or shared path is undesirable.

### Static native consumers

Applications using UIB's native API can avoid runtime libraries entirely with
`libuib-standalone.a`. `make static-test` creates a fully static executable and
rejects any ELF dynamic dependency. See the
[standalone build instructions](../README.md#standalone-static-library).

## Kubernetes execution stack

### Kind

Kind provides the quickest Kubernetes lifecycle:

```text
uib-sim doctor --profile kind-kubernetes
uib-sim up --profile kind-kubernetes
uib-sim deploy --profile kind-kubernetes
uib-sim run --request training-smoke.yaml
uib-sim collect
uib-sim down
```

The planned adapter creates a named cluster, loads pinned images, installs
Kubeflow Trainer and KubeRay, spreads ranks across worker containers, shapes
pod network namespaces, exports logs, and deletes only the named cluster.

Kind nodes share one host kernel and compute capacity. Results are L0 unless a
separate GPU capability has been proven.

### virtme with Kind

`virtme-kind` launches a selected Linux kernel, starts Kind inside that guest,
and runs the same Helm chart. This is useful for testing io_uring, DMA-BUF,
ZCRX, namespaces, and kernel-version compatibility.

Example overlay:

```yaml
infrastructure:
  provider: virtme
  mode: kind
  kernel: /boot/vmlinuz-test
  cpu: 8
  memory: 16Gi
cluster:
  manager: kubernetes
```

### Multiple virtme Kubernetes nodes

`virtme-kubernetes` starts a control-plane guest and worker guests connected
through explicitly owned bridge and tap devices. Each guest gets a writable
overlay and a management and data-plane address. A pinned k3s deployment is
the initial lightweight choice; kubeadm can replace it through the manager
adapter.

### OpenTofu and libvirt Kubernetes

This is the main realistic local Kubernetes lab:

1. OpenTofu creates management and data networks, volumes, cloud-init data,
   one control-plane VM, and worker VMs.
2. Ansible installs containerd, kubeadm, the CNI, Helm, Kubeflow Trainer, and
   KubeRay.
3. Helm deploys the common workload.
4. Link shaping occurs on dedicated data-plane taps.
5. `tofu destroy` removes the explicitly recorded VM resources.

### Existing Kubernetes

The real-cluster adapter accepts an explicit kubeconfig context, creates one
run namespace, detects GPUs and RDMA resources, and submits the same chart.
It does not install or upgrade shared operators unless an administrator
explicitly enables that action.

Example site overlay:

```yaml
infrastructure:
  provider: existing
cluster:
  manager: kubernetes
  kubeContext: production-gpu-lab
kubernetes:
  namespacePrefix: uib-validation
  gpuResource: nvidia.com/gpu
  nodeSelector:
    accelerator: h100
  dataPlaneNetwork: rdma-network
storage:
  modelClass: shared-model-cache
  checkpointClass: parallel-filesystem
network:
  shaping:
    enabled: false
```

## Slurm, Pyxis, and Enroot execution stack

The Slurm path mirrors NVIDIA-oriented production clusters.

### Cluster components

- `slurmctld` controller;
- optional `slurmdbd` accounting;
- Munge authentication;
- `slurmd` compute nodes;
- cgroup v2 isolation;
- GPU GRES definitions;
- Enroot on compute nodes;
- Pyxis SPANK integration;
- shared model, checkpoint, and result paths;
- distinct management and UIB data-plane interfaces.

### Rendered batch job

The adapter renders a standard batch file from the common request. A simplified
shape is:

```sh
#!/bin/sh
#SBATCH --nodes=4
#SBATCH --ntasks-per-node=8
#SBATCH --gpus-per-node=8

set -eu

export NCCL_IBVERBS_LIB=/opt/uib/libuibv.so
export NCCL_NET=IB
export NCCL_IB_DISABLE=0
export NCCL_IB_HCA='=uib0'
export LD_PRELOAD=/opt/uib/libuibv.so
export LD_LIBRARY_PATH=/opt/uib/lib
export UIB_PROTOCOL=rocev2
export UIB_TRANSPORT_MODE=udp-io-uring

srun \
    --container-image=registry.example/uib-llm@sha256:<digest> \
    --container-mounts=/shared/uib:/opt/uib:ro,/models:/models:ro,/results:/results \
    /opt/workload/bin/train --config /run/resolved-workload.yaml
```

The real renderer supplies rank-specific `UIB_BIND_IP`, scheduler-standard
GPU visibility, rendezvous settings, exact Pyxis flags, and site storage
mounts. The rendered file is always retained with the results.

### virtme Slurm

`virtme-slurm` starts controller and compute guests, installs Munge, Slurm,
Enroot, and Pyxis with Ansible, and shapes the compute data-plane taps. It is
an L0 kernel and lifecycle test unless GPU passthrough is proven.

### OpenTofu and libvirt Slurm

`libvirt-slurm` creates a realistic disposable Slurm lab. OpenTofu owns the
VMs and networks; Ansible owns node configuration. The workload still runs
through ordinary `sbatch`, `srun`, Pyxis, and Enroot.

Expected lifecycle:

```text
tofu apply
ansible-playbook site-slurm.yaml
uib-sim deploy --profile libvirt-slurm
uib-sim run --request training-smoke.yaml
uib-sim collect
uib-sim down
tofu destroy
```

### Existing Slurm

The adapter accepts login host, partition, account, QoS, reservation,
constraints, shared paths, and image registry configuration. It performs
read-only discovery, uploads the rendered request, submits the job, records
its job ID, and collects results.

```yaml
infrastructure:
  provider: existing
cluster:
  manager: slurm
container:
  runtime: enroot
  integration: pyxis
slurm:
  loginHost: login.cluster.example
  partition: gpu
  account: llm-validation
  qos: normal
  constraint: h100
  gpusPerNode: 8
storage:
  modelPath: /models
  checkpointPath: /checkpoints/uib-validation
  resultPath: /results/uib-validation
network:
  dataInterface: bond-rdma
  shaping:
    enabled: false
```

On existing clusters, cleanup cancels only recorded job IDs. It never changes
Slurm configuration, partitions, Pyxis, Enroot, shared caches, or unrelated
jobs.

## Training workloads

The implemented training image deliberately has no scheduler API. Kubernetes,
Slurm, a shell, and a future `uib-sim` adapter all launch the same executable:

```text
/opt/uib-training/bin/train --config /run/workload.json
```

The image is pinned by `simulation/training/stack.lock.json`. It uses Python
3.12, CUDA 13.2.1, PyTorch 2.12.1, NCCL 2.30.4, Transformers 5.15.0, and
Datasets 5.0.1. PyTorch's CUDA 13.2 wheel normally requests NCCL 2.29.7; the
image deliberately replaces it with the UIB-validated 2.30.4 ABI so
`NCCL_IBVERBS_LIB` is available. The build records every resolved Python
package in `/opt/uib-training/resolved-packages.txt`, and the runtime result
records the versions it actually loaded.

Build it and run the mandatory offline CPU checks with:

```text
make training-image
make training-smoke-test
make training-gpu-test       # opt-in; needs NVIDIA CDI and a GPU
make training-uib-preflight-test
make training-uib-test       # opt-in; actual two-rank traffic through UIB
```

The smoke target runs an unmodified two-rank `torchrun` DDP job over Gloo,
runs an uninterrupted single-rank job, stops another job at step two, resumes
it through step four, and requires the resumed and uninterrupted model
checksums to match. It also loads `libnccl.so.2` and requires
`ncclGetVersion()` to report 2.30.4. Set `TRAINING_SKIP_BUILD=1` to reuse an
existing image or `TRAINING_IMAGE=registry/image@sha256:<digest>` to test a
published image.

The opt-in GPU target selects `nvidia.com/gpu=all` through the standard CDI
interface, initializes a real NCCL process, runs the FSDP code path, writes a
full model/optimizer checkpoint at step two, and resumes through step four.
Set `TRAINING_GPU_DEVICE` to another CDI device name when necessary. A
single-GPU FSDP run uses PyTorch's `NO_SHARD` mode, so it validates CUDA, NCCL,
FSDP wrapping, and FSDP checkpoint conversion but not multi-GPU sharding or
network traffic.

On a multi-GPU node, select the local process count explicitly. The default
strategy is FSDP; DDP uses the same gate and checkpoint comparison:

```text
make TRAINING_GPU_PROCESSES=8 training-gpu-test
make TRAINING_GPU_PROCESSES=8 TRAINING_GPU_STRATEGY=ddp training-gpu-test
```

The test exposes the CDI device set, maps each `LOCAL_RANK` to one CUDA device,
sets its generated global batch to two samples per rank, and requires the
restarted model checksum to equal the uninterrupted run. The result records
the actual world size. This machine has one GPU, so the checked-in result class
records single-GPU validation; multi-GPU execution must pass on a suitable
host before that limitation is removed.

The UIB preflight target rebuilds the common runtime artifact, mounts that
exact tree read-only at `/opt/uib` in the training image, validates checksums,
ELF dependencies, symbol versions, and the synthetic verbs library, and then
runs the CPU workload gates. It proves that the two independently built image
layers compose, but it does not send UIB traffic.

The opt-in `training-uib-test` is the data-plane gate. It starts two instances
of the unchanged training entry point with distinct ranks and hostnames, uses
an isolated NVIDIA MPS daemon so the local test can share one physical GPU,
disables NCCL P2P and shared-memory paths, mounts the runtime artifact at
`/opt/uib`, and selects `NCCL_NET=IB`, `uib0`, and
`NCCL_IBVERBS_LIB=/opt/uib/libuibv.so`. It requires NCCL's log to contain the
IB plugin, `Using network IB`, and `via NET/IB/<device>`; any Socket network
selection fails the test. It also requires the training evidence to report a
two-rank CUDA DDP run through NCCL with UIB enabled.

The local container gate uses host networking, the host PID and IPC
namespaces, and an unconfined container seccomp profile for UIB's io_uring
operations. These settings are confined to disposable test containers and
are not a production deployment recommendation. Configure the executable and
ports with `TRAINING_CONTAINER_ENGINE`, `MPIEXEC`, `TRAINING_UIB_BIND_PORT`,
`TRAINING_MASTER_PORT`, and `TRAINING_UIB_TIMEOUT`; select the exposed CDI
device with `TRAINING_GPU_DEVICE`. For example:

```text
make TRAINING_CONTAINER_ENGINE='sudo docker' \
  TRAINING_GPU_DEVICE=nvidia.com/gpu=all training-uib-test
```

This is genuine two-rank UIB traffic and a useful local compatibility gate,
but two container hostnames on one machine are not two physical nodes and do
not qualify as L1. Promotion still requires the identical image, command, and
environment on at least two GPU-equipped nodes.

### CPU-sized data and production-sized data

The default container command uses `configs/tiny-public.json`: the roughly
5 MB `sshleifer/tiny-gpt2` model and the TinyStories validation split. Both
are public and pinned to full repository commits. Neither weights nor dataset
rows are in this repository or image. They are fetched when that profile is
first run and reused from the mounted Hugging Face cache:

```text
make training-public-test

# Equivalent direct run; later runs reuse this ignored host cache.
docker run --rm \
  -e HF_HOME=/cache/huggingface \
  -v "$PWD/simulation/.artifacts/huggingface:/cache/huggingface" \
  -v "$PWD/simulation/.artifacts/training-results:/results" \
  uib-training:test
```

`configs/smoke.json` constructs a tiny GPT-2 architecture and deterministic
token stream in memory. It is the network-independent CI profile; it does not
pretend to validate model or dataset distribution. The public profile is the
CPU end-to-end loading check.

Large GPU tests change only `model.id`, `model.revision`, `dataset.id`,
`dataset.revision`, and sizing fields in an external JSON file. Always use a
full immutable revision, mount the site model cache, and keep the configuration
with the result. For example:

```json
{
  "model": {
    "source": "huggingface",
    "id": "organization/production-causal-lm",
    "revision": "<full-model-commit>"
  },
  "dataset": {
    "source": "huggingface",
    "id": "organization/production-corpus",
    "name": "sample-10BT",
    "revision": "<full-dataset-commit>",
    "split": "train",
    "textField": "text"
  }
}
```

Copy the complete tiny-public configuration before replacing these objects;
the fragment above is intentionally not a complete workload. Private or gated
repositories use the standard `HF_TOKEN` environment without putting a token
in configuration or results.

### Configuration contract

`workload.schema.json` rejects unknown fields and invalid combinations before
opening a model. The important fields are:

| Field | Meaning |
|---|---|
| `runId` | Safe result-directory identifier |
| `model.source` | `generated` for offline CI or `huggingface` for a real repository |
| `model.id`, `model.revision` | Repository and immutable commit; required together |
| `model.vocabSize`, `hiddenSize`, `layers`, `heads` | Generated smoke-model shape |
| `dataset.source` | `generated` or streaming `huggingface` |
| `dataset.id`, `name`, `revision`, `split`, `textField` | External dataset selection |
| `parallelism.strategy` | Validated `ddp`, or CUDA-gated `fsdp` |
| `parallelism.tensor`, `pipeline` | Must remain 1 until their standard adapter passes |
| `training.steps`, `sequenceLength` | Run length and tokens per sample |
| `training.microBatchSize`, `globalBatchSize` | Per-rank microbatch and global batch; their ratio determines accumulation |
| `training.learningRate`, `weightDecay` | AdamW rate and decay |
| `training.adamBeta1`, `adamBeta2`, `adamEpsilon` | AdamW numerical parameters |
| `training.warmupSteps`, `gradientClipNorm` | Linear warmup and optional global gradient clipping |
| `training.gradientCheckpointing` | Trade compute for activation memory through the model's standard support |
| `training.device`, `precision`, `seed` | Explicit `cpu`/`cuda`/`auto`, `fp32`/`bf16`, and deterministic seed |
| `checkpoint.directory`, `interval`, `resume` | Shared output, cadence, and optional restart file |
| `result.directory` | Shared evidence root |

`globalBatchSize` must be divisible by `microBatchSize * WORLD_SIZE`. Changing
the requested total step count is allowed when resuming; changing the model,
data, parallelism, batch semantics, precision, learning rate, or seed changes
the workload identity and the checkpoint is rejected. Restarts currently
require the same `WORLD_SIZE`; each rank's Python, CPU Torch, and CUDA random
state is recorded so an interrupted run matches an uninterrupted run.
Both checked-in profiles select `device=cpu`; GPU profiles explicitly replace
it with `cuda`, so merely exposing a GPU does not change the default test.

### Launching the same workload

Local two-rank CPU launch:

```text
docker run --rm -v "$PWD/results:/results" \
  --entrypoint torchrun uib-training:test \
  --standalone --nproc-per-node=2 --no-python \
  /opt/uib-training/bin/train \
  --config /opt/uib-training/configs/smoke.json
```

Slurm/Pyxis launches one process per allocated GPU. Its adapter supplies
`MASTER_ADDR`, `MASTER_PORT`, `RANK`, `LOCAL_RANK`, and `WORLD_SIZE`; it does
not rewrite the workload:

```text
srun --ntasks-per-node=8 --gpus-per-task=1 \
  --container-image=registry/uib-training@sha256:<digest> \
  --container-mounts=/shared/run:/run,/shared/results:/results \
  /opt/uib-training/bin/train --config /run/workload.json
```

Kubeflow Trainer uses the same image, executable, JSON ConfigMap, cache PVC,
checkpoint PVC, and result PVC. Its launcher supplies the equivalent rank
environment. This makes changing Kubernetes to Slurm a launcher change rather
than a training-code change.

Native verbs and UIB also use the same image digest and command. The native
baseline has no UIB variables. The UIB variant adds only the `/opt/uib` mount
and the environment documented in
[Transparent UIB injection](#transparent-uib-injection), including
`NCCL_IBVERBS_LIB=/opt/uib/libuibv.so`. The result records whether those
variables selected UIB; the later cluster evidence gate must additionally
prove NCCL chose `NET/IB` and `uib0` rather than sockets.

Each successful run writes `<result.directory>/<runId>/result.json` with the
workload identity, world size, device, strategy, starting and completed steps,
loss, model checksum, throughput, checkpoint path, resolved framework/CUDA/NCCL
versions, and transport-selection environment. Checkpoints are written through
a temporary file and atomically renamed.

### Training adapter status

The intended common workload shapes remain:

- PyTorch DDP;
- FSDP;
- DeepSpeed ZeRO;
- tensor and pipeline parallel Megatron-style training;
- MoE expert parallelism after all-to-all is validated.

Communication gates cover all-reduce, reduce-scatter, all-gather, send/receive,
and all-to-all before the equivalent LLM profile is enabled.

DDP, checkpoint/restart, the generated CPU workload, the public tiny CPU
workload, single-GPU FSDP checkpoint/restart, and two-rank CUDA DDP traffic
through UIB are executable today. The local UIB gate shares one GPU with MPS;
FSDP is gated to CUDA, and multi-GPU sharding remains unvalidated. DeepSpeed
0.19.5 and Megatron-Core 0.19.0 are pinned as replaceable adapters but are not
installed or reported as supported yet. Tensor and pipeline degrees therefore
remain 1. This explicit gate avoids turning a package import into an invalid
compatibility claim.

Important training parameters include model and dataset revisions, sequence
length, token budget, global and microbatch sizes, gradient accumulation,
DP/TP/PP/EP degrees, ZeRO stage, precision, optimizer, checkpointing, steps,
warmup, and seed.

## Inference workloads

The primary inference stack is vLLM. Kubernetes uses KubeRay; Slurm starts a
Ray head and workers inside one allocation. Direct vLLM multiprocessing is a
replaceable, separately labeled launcher.

Supported shapes include:

- tensor-parallel serving;
- pipeline-parallel serving;
- multiple data-parallel replicas;
- later prefill/decode disaggregation;
- later MoE serving after all-to-all validation.

Use the OpenAI-compatible API and a standard load generator such as
`vllm bench serve`. Save the generated request trace so UIB and native runs
receive identical prompt lengths, output lengths, arrival times, concurrency,
streaming behavior, warmup, and duration.

## Network and failure simulation

Configurable link features include:

- bandwidth and oversubscription;
- base latency, jitter, correlation, and distribution;
- random or burst loss;
- reordering, duplication, and corruption;
- queue capacity and MTU;
- ECN where available;
- deterministic random seed;
- timed link degradation and recovery.

`tc netem` is the default impairment mechanism. Kind applies qdiscs inside
workload network namespaces. virtme and libvirt apply them to explicit tap or
bridge data interfaces. A real-cluster profile normally disables shaping and
records physical NIC and switch counters instead.

Faults can also terminate a pod, Slurm rank, virtme guest, or VM. Each fault is
recorded with planned time, actual time, target, result, and recovery time.

## Changing one layer

### Kind to libvirt without changing Kubernetes software

Keep:

```yaml
cluster:
  manager: kubernetes
launcher:
  training: kubeflow
```

Change only:

```yaml
infrastructure:
  provider: kind
```

to:

```yaml
infrastructure:
  provider: libvirt
```

The Helm chart, operator resources, image, workload request, and UIB
configuration remain unchanged.

### Kubernetes to Slurm without changing the workload

Kubernetes profile:

```yaml
cluster:
  manager: kubernetes
container:
  runtime: containerd
launcher:
  training: kubeflow
```

Slurm profile:

```yaml
cluster:
  manager: slurm
container:
  runtime: enroot
  integration: pyxis
launcher:
  training: slurm
```

The common workload request, OCI digest, entry point, UIB bundle, model, data,
parallelism, and measurements do not change.

### Kubeflow to a debugging launcher

Change:

```yaml
launcher:
  training: kubeflow
```

to:

```yaml
launcher:
  training: jobset
```

This changes scheduling mechanics only. It is useful for diagnosing an
operator issue without creating a different training benchmark.

### KubeRay to direct vLLM multiprocessing

```yaml
launcher:
  inference: kuberay
```

can become:

```yaml
launcher:
  inference: vllm-multiprocessing
```

The result must label the launcher because Ray scheduling and direct
multiprocessing are not performance-identical.

### UIB to native verbs

```yaml
uib:
  enabled: true
```

becomes:

```yaml
uib:
  enabled: false
```

All workload and infrastructure fields remain unchanged, producing a valid
baseline comparison.

### Enroot to Apptainer

On a Slurm site without Pyxis:

```yaml
container:
  runtime: enroot
  integration: pyxis
```

may become:

```yaml
container:
  runtime: apptainer
```

The adapter must still resolve the same OCI digest, mount `/opt/uib` and the
same data paths, and run the same command.

## Running a simulation

The planned CLI lifecycle is:

```text
uib-sim doctor --profile <profile>
uib-sim up --profile <profile>
uib-sim deploy --profile <profile>
uib-sim run --profile <profile> --request <request.yaml>
uib-sim collect
uib-sim status
uib-sim down
```

`doctor` is read-only. It reports missing tools, kernel features, GPU and RDMA
capabilities, scheduler versions, storage, permissions, and the highest valid
validation level.

`up` creates only infrastructure owned by the selected profile. For existing
clusters it performs no infrastructure changes.

`deploy` installs or detects scheduler adapters and stages images and UIB
artifacts.

`run` resolves configuration, renders scheduler resources, executes
preflight gates, submits the workload, and records ownership.

`collect` can be repeated and does not delete the workload.

`down` exports final evidence and removes only resources in the ownership
ledger. `--keep-on-failure` preserves resources and prints the exact cleanup
command.

### Fast local example

```text
uib-sim doctor --profile kind-kubernetes
uib-sim up --profile kind-kubernetes
uib-sim run --profile kind-kubernetes --request training-smoke.yaml
uib-sim collect
uib-sim down
```

### Selected-kernel example

```text
uib-sim up --profile virtme-kind \
    --set infrastructure.kernel=/boot/vmlinuz-test
uib-sim run --request collectives-smoke.yaml
uib-sim down
```

### Realistic local NVIDIA-style example

```text
uib-sim up --profile libvirt-slurm
uib-sim run --request training-smoke.yaml
uib-sim run --request inference-smoke.yaml
uib-sim collect
uib-sim down
```

## Moving to real hardware

Promotion must require only a small site overlay.

Fields that remain invariant:

- OCI image digest and entry point;
- model and dataset revisions;
- numerical settings and checkpoint format;
- rank layout and parallelism unless deliberately scaled;
- NCCL and UIB configuration;
- request trace or training seed;
- metric definitions and correctness thresholds.

Fields normally changed by a site profile:

- `infrastructure.provider`;
- Kubernetes context or Slurm partition/account;
- node selectors, constraints, reservations, and GPU resource names;
- storage classes or shared paths;
- data-plane interface selection;
- registry credentials;
- synthetic network shaping, normally disabled on physical fabric.

### Promotion procedure

1. Pass L0 lifecycle and configuration tests locally.
2. Pass UIB's versioned ABI and actual host-staged NCCL collective tests.
3. Publish immutable workload and UIB artifacts to the site's registry.
4. Write a site overlay containing only infrastructure, scheduler, storage,
   placement, and network-discovery values.
5. Run `doctor` against the real cluster in read-only mode.
6. Render a semantic diff against the simulation profile. Changes to invariant
   fields require explicit approval.
7. Run rdma-core diagnostics and a two-rank NCCL correctness job.
8. Run a small LLM smoke request.
9. Run the unchanged production-shape request at the intended scale.
10. Repeat with UIB disabled for the native-verbs baseline.
11. Collect and compare result bundles.
12. Remove only the namespace or recorded Slurm jobs.

Kubernetes promotion:

```text
uib-sim doctor --profile real-kubernetes
uib-sim run --profile real-kubernetes --request common-training.yaml
uib-sim down
```

Slurm promotion:

```text
uib-sim doctor --profile real-slurm
uib-sim run --profile real-slurm --request common-training.yaml
uib-sim down
```

Named scale overlays such as `tiny`, `two-node`, `rack`, and `production` can
change node count and parallel degrees while retaining application code,
framework, communication backend, model architecture, checkpoint format, and
metric definitions.

## Results and evidence

Each run produces:

```text
results/<run-id>/
  request.yaml
  resolved-values.yaml
  versions.json
  topology.json
  images.json
  preflight.json
  scheduler/
  logs/
  metrics/
  traces/
  faults.jsonl
  summary.json
  summary.md
```

The summary states:

- UIB or native verbs;
- NCCL IB or socket network;
- host-staged or GPU-direct memory;
- L0, L1, L2, or L3 validation level;
- exact image and library digests;
- actual rank placement;
- skipped gates and reasons;
- correctness and performance outcomes.

Training metrics include tokens per second, step time, collective time,
checkpoint time, loss, GPU use, host use, network counters, retries, and fault
recovery.

Inference metrics include request and token throughput, time to first token,
inter-token latency, end-to-end percentiles, queueing, KV-cache use, errors,
GPU use, network counters, and recovery.

## Cleanup and safety

Every resource receives a run ID and is written to an ownership ledger as soon
as it is created.

- Kind deletes only the recorded cluster name.
- virtme stops only recorded PIDs and deletes recorded taps, bridges, overlays,
  and temporary roots.
- libvirt cleanup uses the run's OpenTofu state and `tofu destroy`.
- existing Kubernetes cleanup deletes only run-owned namespaced resources.
- Slurm cleanup cancels only recorded job IDs.
- shared Kubernetes operators, Slurm configuration, Pyxis, Enroot, GPU/RDMA
  operators, partitions, caches, and unrelated jobs are never removed.

Cleanup must remain safe after interruption during every lifecycle stage.

## Limitations

- The cluster lifecycle harness, scheduler adapters, inference image,
  DeepSpeed adapter, and Megatron-Core adapter are planned and not yet
  implemented.
- UIB's versioned NCCL loader ABI and a two-rank, host-staged NCCL all-reduce
  are covered by repository tests. The actual collective test is opt-in
  because it requires CUDA, NCCL, MPI, MPS, a GPU, and UTS namespace access.
- Kind and CPU-only virtme/libvirt profiles do not validate real GPU
  collectives or performance.
- The training image passed CPU DDP, checkpoint/restart, public tiny-model,
  actual single-GPU NCCL initialization, single-GPU FSDP restart, and local
  two-rank NCCL DDP through UIB. The two ranks share one GPU through MPS;
  multi-GPU and physical multi-node training remain unvalidated.
- Host-staged NCCL and GPU-direct NCCL are materially different and are never
  combined in one result class.
- virtme is intended for kernel and functional validation, not accurate L2
  performance modeling.
- Real physical-fabric behavior cannot be completely reproduced with netem;
  final L3 claims require real GPU/RDMA hardware.

## Further reading

- [Transparent verbs operation](transparent-verbs.md)
- [Generated environment-variable reference](environment-variables.md)
- [DMA-BUF and zero-copy](dmabuf-zerocopy.md)
- [Kind configuration](https://kind.sigs.k8s.io/docs/user/configuration/)
- [Kubeflow Trainer](https://www.kubeflow.org/docs/components/trainer/overview/)
- [Ray on Kubernetes](https://docs.ray.io/en/latest/cluster/kubernetes/index.html)
- [vLLM distributed serving](https://docs.vllm.ai/en/stable/serving/parallelism_scaling/)
- [NCCL tests](https://github.com/NVIDIA/nccl-tests)
- [OpenTofu](https://opentofu.org/docs/)
- [libvirt](https://libvirt.org/docs.html)
- [virtme-ng](https://github.com/arighi/virtme-ng)
- [Pyxis](https://github.com/NVIDIA/pyxis)
- [Enroot](https://github.com/NVIDIA/enroot)
