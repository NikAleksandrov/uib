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
- [Validation requirements](#validation-requirements)
- [Architecture](#architecture)
- [Layer model](#layer-model)
- [Common configuration](#common-configuration)
- [Configuration syntax and values](#configuration-syntax-and-values)
- [Configuration examples](#configuration-examples)
- [Available deployment compositions](#available-deployment-compositions)
- [Communication compatibility comes first](#communication-compatibility-comes-first)
- [Congestion-control experiments](#congestion-control-experiments)
- [Transparent UIB injection](#transparent-uib-injection)
- [Runtime artifact](#runtime-artifact)
- [Diagnostic image](#diagnostic-image)
- [Where transparent UIB is useful](#where-transparent-uib-is-useful)
- [Kubernetes execution stack](#kubernetes-execution-stack)
- [Slurm, Pyxis, and Enroot execution stack](#slurm-pyxis-and-enroot-execution-stack)
- [Training workloads](#training-workloads)
- [Inference workloads](#inference-workloads)
- [Network and failure simulation](#network-and-failure-simulation)
- [Network emulation with netem](#network-emulation-with-netem)
- [Changing one layer](#changing-one-layer)
- [Running a simulation](#running-a-simulation)
- [Moving to real hardware](#moving-to-real-hardware)
- [Results and evidence](#results-and-evidence)
- [Simulation test reference](#simulation-test-reference)
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

## Validation requirements

The local gates and physical-cluster gates are intentionally separate. Two
processes or containers on one host can prove protocol traffic and application
compatibility, but they do not reproduce a physical multi-node GPU/RDMA
topology.

| Gate | Minimum hardware | Physical multi-node required | What it proves |
|---|---|---:|---|
| `make diagnostic-smoke-test` | CPU | No | Diagnostic tools, versions, dynamic dependencies, and capability output |
| `make diagnostic-uib-preflight-test` | CPU | No | The common UIB runtime mounts into an independently built image |
| `make diagnostic-gpu-test` | One NVIDIA GPU | No | CUDA visibility and a correctness-checked, single-rank NCCL collective |
| `make nccl-test` | One NVIDIA GPU with local MPS support | No | Two local ranks send host-staged NCCL traffic through UIB |
| `make training-uib-test` | One NVIDIA GPU with local MPS support | No | Unmodified two-rank PyTorch DDP sends gradient traffic through UIB |
| UCX verbs transport gate | CPU | No | Two local processes exchange data through UCX's verbs transport and UIB, with TCP and shared-memory transports excluded |
| libfabric verbs provider gate | CPU | No | `fi_info` and multi-process fabtests select `verbs` or `ofi_rxm;verbs` through UIB, with sockets/tcp providers excluded |
| RCCL local collective gate | At least two AMD GPUs | No | Two local RCCL ranks exchange host-staged collective traffic through UIB; this is functional, not multi-node validation |
| Multi-GPU training gate | At least two GPUs in one node | No | DDP or FSDP sharding across distinct GPUs; not node-to-node behavior |
| Two-node NCCL promotion gate | GPU on each of at least two nodes | **Yes** | Host-staged NCCL traffic through UIB across physical nodes; required for L1 |
| Two-node RCCL promotion gate | AMD GPU on each of at least two nodes | **Yes** | Host-staged RCCL traffic through UIB across physical nodes; required for AMD L1 |
| Kubeflow or Slurm distributed training | GPU on each participating node | **Yes** | Real scheduler placement, bootstrap, checkpointing, and UIB collectives |
| KubeRay or Slurm distributed inference | GPU on each participating node | **Yes** | vLLM TP/PP/DP worker traffic and serving through UIB |
| GPU-memory or DMA-BUF promotion gate | Compatible GPUs, drivers, and RDMA path on at least two nodes | **Yes** | L2 GPU-memory data path rather than host staging |
| Fabric performance, congestion, and failure qualification | Production-like GPU/RDMA nodes and fabric | **Yes** | L3 behavior and comparison with native verbs |

The physical multi-node gates are deferred until suitable hardware is
available. A one-rank NCCL initialization never counts as UIB data-plane
validation because it has no remote rank to communicate with.

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
              | Communication front end                |
              | NCCL | RCCL | MPI/UCX | libfabric      |
              +----------------------------------------+
                                 |
                                 v
              +----------------------------------------+
              | Verbs adapter and protocol             |
              | UIB libuibv.so or native libibverbs    |
              | RoCEv2/UET + selected congestion algo  |
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
             NCCL / RCCL / UCX / libfabric
                           |
                     libuibv.so
                           |
                    UIB protocol + CC
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

The communication layer has two independent selections. The front end is one
of NCCL, RCCL, UCX, or libfabric; the verbs implementation beneath it is:

- UIB through `libuibv.so`; or
- the system's native libibverbs implementation as a baseline.

NCCL and RCCL share many `NCCL_*` configuration names but remain separate
validation stacks: NVIDIA CUDA/NCCL results cannot satisfy an AMD ROCm/RCCL
gate. UCX must select a verbs transport, and libfabric must select `verbs` or
`ofi_rxm;verbs`; TCP, sockets, shared-memory, and vendor-direct fallbacks are
failures for a UIB compatibility result.

A native collective network plugin could be another adapter, but it would be
labeled separately because it would not test transparent libibverbs
substitution.

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
  congestionControl:
    algorithm: nscc
    parameters:
      maxWindowBytes: 262144
communication:
  frontend: nccl
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
    processesPerNode: 8
    tensor: 8
    pipeline: 2
    data: 1
    expert: 1
  parameters:
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

## Configuration syntax and values

Configuration is merged in this order, with later values winning:

```text
profiles/defaults.yaml
        |
        +-- profiles/<infrastructure>.yaml
        |
        +-- --overlay <layer>       (repeatable, left to right)
        |
        +-- --request <workload.yaml>
        |
        `-- --set path=value        (repeatable, typed YAML scalar)
```

Use `render` to see the complete result. Nothing is deployed by this command:

```text
simulation/bin/uib-sim render --profile kind-kubernetes
simulation/bin/uib-sim render --profile kind-kubernetes --format json
```

`--profile` accepts a checked-in name or a YAML file path. `--overlay` accepts
a checked-in layer name or a YAML file path and can be repeated. `--request`
is normally a scheduler-neutral workload. `--set` is convenient for one value;
use a small YAML overlay for a group of related values.

### Checked-in profiles and overlays

| Layer | Names |
|---|---|
| Infrastructure profiles | `kind-kubernetes`, `virtme-kind`, `virtme-kubernetes`, `virtme-slurm`, `libvirt-kubernetes`, `libvirt-slurm`, `real-kubernetes`, `real-slurm` |
| Communication | `communication/nccl`, `communication/rccl`, `communication/ucx`, `communication/libfabric` |
| Protocol and congestion | `protocol/rocev2`, `protocol/uet-nscc`, `protocol/uet-none` |
| Network condition | `network/clean`, `network/datacenter`, `network/constrained`, `network/lossy`, `network/reordered`, `network/ecn` |

A later overlay from the same layer replaces the earlier layer settings. This
makes interactive comparisons easy, but a committed request should specify
only one communication, protocol, and network-condition overlay.

### Infrastructure and execution values

| Path | Accepted values or syntax | Meaning |
|---|---|---|
| `infrastructure.provider` | `kind`, `virtme`, `libvirt`, `existing` | Node source |
| `infrastructure.kernel` | Non-empty path | Kernel selected by a virtme profile |
| `infrastructure.kubernetesContext` | Context name | Existing Kubernetes target |
| `infrastructure.slurmPartition` | Partition name | Existing Slurm target |
| `infrastructure.allowExistingMutations` | `true`, `false` | Permit explicitly requested shaping on existing hardware |
| `cluster.manager` | `kubernetes`, `slurm` | Scheduler/control plane |
| `container.runtime` | `containerd`, `enroot`, `apptainer` | OCI execution implementation |
| `container.integration` | `none`, `pyxis` | Slurm/container integration |
| `launcher.training` | `kubeflow`, `jobset`, `slurm` | Training submitter |
| `launcher.inference` | `kuberay`, `vllm-multiprocessing`, `slurm-ray` | Inference submitter |
| `nodes.controlPlane` | Integer 0-9 | Control-plane nodes; existing clusters normally use 0 |
| `nodes.workers` | Integer 1-1024 | Worker nodes available to the request |
| `nodes.cpu` | Positive integer | vCPUs per simulated worker |
| `nodes.memory`, `nodes.disk` | Integer plus `Mi`, `Gi`, or `Ti`, such as `16Gi` | Per-worker capacity |
| `nodes.gpus.vendor` | `none`, `nvidia`, `amd` | Accelerator family |
| `nodes.gpus.perWorker` | Integer 0-64 | GPUs available per worker |
| `nodes.gpus.resourceName` | Scheduler resource name | For example `nvidia.com/gpu` |

### UIB and communication values

| Path | Accepted values or syntax | Meaning |
|---|---|---|
| `uib.enabled` | `true`, `false` | UIB run or native-verbs baseline |
| `uib.bundle` | Path, archive, or immutable image reference | Runtime artifact supplied to the workload |
| `uib.protocol` | `rocev2`, `uet` | UIB wire protocol |
| `uib.transport` | `udp-sync`, `udp-io-uring`, `udp-zc-afxdp`, `tcp-devmem` | Packet-I/O backend |
| `uib.memoryMode` | `host-staged`, `gpu-direct` | Collective buffer path/result class |
| `uib.environment` | Map whose keys start with `UIB_` | Validated advanced UIB settings |
| `uib.congestionControl.algorithm` | `none`, `nscc`, or registered name matching `[a-z][a-z0-9_-]+` | Per-QP algorithm |
| `uib.congestionControl.abi` | `1` | Required congestion operations ABI |
| `uib.congestionControl.parameters` | Algorithm-specific scalar map | `nscc` currently accepts `maxWindowBytes` |
| `communication.frontend` | `nccl`, `rccl`, `ucx`, `libfabric` | Application-facing communication library |
| `communication.fallbackPolicy` | `fail` | Software/native fallback is never accepted for validation |
| `communication.transportAllowlist` | Non-empty list | Only data transports/providers that may satisfy the run |
| `communication.nccl.network`, `.rccl.network` | `IB` | Required collective data network |
| `communication.nccl.hca`, `.rccl.hca` | `=uib0` | Exact UIB device filter |
| `communication.ucx.transports` | Non-empty list, normally `rc`, `ud` | Explicit UCX verbs transports |
| `communication.ucx.device` | Device and port, normally `uib0:1` | Exact UCX device |
| `communication.libfabric.provider` | `verbs`, `ofi_rxm;verbs` | Exact OFI provider chain |

The communication overlay removes settings for other front ends from the
resolved file. This prevents stale NCCL values from appearing in an UCX run or
vice versa.

### Workload and measurement values

| Path | Accepted values or syntax | Meaning |
|---|---|---|
| `workload.kind` | `collectives`, `training`, `inference` | Workload class |
| `workload.image` | Non-empty OCI reference; digest strongly recommended | Unchanged workload image |
| `workload.entrypoint` | Absolute or image-relative executable | Unchanged command |
| `workload.arguments` | String list | Command arguments |
| `workload.model`, `.dataset` | `{id, revision}` | Dynamically cached immutable artifact |
| `workload.parallelism.nodes` | Positive integer | Participating worker nodes |
| `workload.parallelism.processesPerNode` | Positive integer | Ranks per node |
| `workload.parallelism.data`, `.tensor`, `.pipeline`, `.expert` | Positive integers | Parallel degrees |
| `workload.parameters` | Framework-specific mapping | Training, serving, or load-generator settings passed unchanged |
| `measurement.seed` | Non-negative integer | Workload random seed |
| `measurement.warmupSteps` | Non-negative integer | Excluded warmup |
| `measurement.steps` | Positive integer | Measured iterations |
| `faults[].at` | Integer plus `ms`, `s`, `m`, or `h` | Time-relative fault |
| `faults[].afterEvent` | Event name | Event-relative fault alternative |
| `faults[].action` | `add-delay`, `packet-loss`, `link-down`, `terminate-rank` | Fault type |

The rank invariant is:

```text
nodes * processesPerNode == data * tensor * pipeline * expert
```

### Combinations rejected before deployment

| Combination | Reason |
|---|---|
| Kind with Slurm | Kind is the Kubernetes backend |
| Kubernetes with Enroot or Apptainer | Kubernetes profiles use containerd/CRI |
| Slurm with containerd | Slurm profiles use Enroot/Pyxis or Apptainer |
| RCCL without `nodes.gpus.vendor: amd` | RCCL requires AMD/ROCm hardware |
| NCCL with `nodes.gpus.vendor: amd` | Select RCCL for the AMD profile |
| `gpu-direct` while the compatibility lock says unvalidated | Prevents an L2 claim from an unsupported path |
| RoCEv2 with a congestion algorithm other than `none` | RoCEv2 pending admission and feedback are not implemented yet |
| Network parameters while `shaping.enabled` is false | Prevents a condition that looks active but is not |
| Jitter without latency; distribution without jitter | These netem options depend on the preceding delay options |
| Reordering without latency | Netem needs delay for observable reordering |
| ECN without nonzero loss | Netem uses a loss model to decide which packets to mark |
| Correlation without its delay/loss/duplicate/corrupt/reorder parent | Invalid netem grammar |
| Cell size without packet overhead; cell overhead without cell size | Invalid netem rate grammar |
| Shaping an `existing` profile without `allowExistingMutations: true` | Protects real interfaces |
| Workload nodes greater than profile workers | Placement is impossible |
| Parallel degrees whose product differs from rank count | Framework topology is inconsistent |

## Configuration examples

Default Kind configuration:

```text
simulation/bin/uib-sim render --profile kind-kubernetes
```

UET/NSCC in an emulated data-center network:

```text
simulation/bin/uib-sim render --profile virtme-kubernetes \
  --overlay protocol/uet-nscc --overlay network/datacenter
```

Change only the NSCC initial window:

```text
simulation/bin/uib-sim render --profile virtme-kubernetes \
  --overlay protocol/uet-nscc \
  --set uib.congestionControl.parameters.maxWindowBytes=131072
```

Compare with congestion admission disabled in the same condition:

```text
simulation/bin/uib-sim render --profile virtme-kubernetes \
  --overlay protocol/uet-none --overlay network/constrained
```

NCCL on a libvirt/Slurm lab:

```text
simulation/bin/uib-sim render --profile libvirt-slurm \
  --overlay communication/nccl --overlay protocol/rocev2
```

RCCL on an existing AMD Kubernetes cluster:

```text
simulation/bin/uib-sim render --profile real-kubernetes \
  --overlay communication/rccl \
  --set nodes.gpus.vendor=amd \
  --set nodes.gpus.resourceName=amd.com/gpu
```

UCX over UIB/UET with loss:

```text
simulation/bin/uib-sim render --profile virtme-kubernetes \
  --overlay communication/ucx --overlay protocol/uet-nscc \
  --overlay network/lossy
```

libfabric RxM/verbs with reordering:

```text
simulation/bin/uib-sim render --profile libvirt-kubernetes \
  --overlay communication/libfabric --overlay network/reordered
```

Native-verbs baseline without changing the workload:

```text
simulation/bin/uib-sim render --profile real-slurm \
  --overlay communication/nccl --request training.yaml \
  --set uib.enabled=false
```

Custom site overlay:

```yaml
# site-a.yaml
infrastructure:
  kubernetesContext: research-a
nodes:
  workers: 8
  gpus:
    vendor: nvidia
    perWorker: 8
    resourceName: nvidia.com/gpu
network:
  topology: physical
```

```text
simulation/bin/uib-sim render --profile real-kubernetes \
  --overlay site-a.yaml --overlay communication/nccl \
  --request training.yaml
```

Custom network overlay:

```yaml
# campus-link.yaml
network:
  shaping:
    enabled: true
    bandwidth: 1Gbit
    latency: 4ms
    jitter: 500us
    delayCorrelationPercent: 20
    delayDistribution: normal
    lossPercent: 0.05
    queueLimit: 2048
    seed: 42
```

```text
simulation/bin/uib-sim render --profile virtme-slurm \
  --overlay campus-link.yaml --overlay protocol/uet-nscc
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

## Communication compatibility comes first

Compatibility is promoted one front end at a time. A direct verbs test is a
prerequisite, not evidence that a higher-level library works. Every gate pins
the front-end source or package revision, records its queried symbols, runs a
multi-process data transfer, and rejects alternative transports.

### NCCL

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

### UCX and libfabric

The UCX gate runs `ucx_info -d` and `ucx_perftest` with only the verbs-backed
transport enabled. The libfabric gate runs `fi_info -p verbs`, then fabtests
with `FI_PROVIDER=verbs` and `FI_PROVIDER=ofi_rxm;verbs`. Logs must name
`uib0`, UIB's library must appear in the process map, and any TCP, sockets,
shared-memory, or vendor-direct data path fails the gate.

These gates require more than symbol loading. UCX exercises device discovery,
memory registration, completion channels, address-vector behavior, atomics,
and error handling. The libfabric verbs provider also uses `librdmacm` for
connection management. The implementation sequence therefore inventories the
exact calls from pinned UCX/libfabric builds, adds missing semantics, tests
local namespace pairs, and finally repeats the same binaries on two physical
nodes. Until that sequence passes, UCX and libfabric support remains planned.

Use explicit selectors in a diagnostic run:

```text
UCX_TLS=rc,ud UCX_NET_DEVICES=uib0:1 \
LD_PRELOAD=/opt/uib/libuibv.so ucx_perftest <peer>

FI_PROVIDER=verbs LD_PRELOAD=/opt/uib/libuibv.so \
fi_pingpong <peer>

FI_PROVIDER='ofi_rxm;verbs' LD_PRELOAD=/opt/uib/libuibv.so \
fi_rdm_pingpong <peer>
```

The exact UCX transport list is taken from `ucx_info -d` for the pinned
release; a configuration is rejected if it accidentally retains `tcp`,
`self`, `posix`, `sysv`, or another non-verbs data transport.

### RCCL

RCCL is the AMD/ROCm collective front end. Its diagnostic image is separate
from the CUDA image and pins ROCm, RCCL, MPI, and `rccl-tests` from the current
ROCm systems source tree. The gate runs at least two ranks, sets the ordinary
RCCL `NCCL_NET=IB`, `NCCL_IB_HCA==uib0`, and debug settings, mounts the same
UIB runtime artifact, and rejects the Socket network. A PyTorch ROCm DDP smoke
test follows the lower-level `all_reduce_perf` gate.

A one-rank RCCL run only proves loading and GPU visibility. A local two-rank
run proves UIB traffic if the host can schedule both ranks. The AMD L1 claim
still requires at least two physical nodes with AMD GPUs; it cannot be inferred
from NVIDIA NCCL, CPU emulation, or a skipped ROCm test.

## Congestion-control experiments

Congestion control is below the verbs front ends and above protocol wire
encoding. This keeps NCCL, RCCL, UCX, and libfabric unchanged while allowing a
run to select an algorithm independently of the scheduler and infrastructure.

```text
NCCL / RCCL / UCX / libfabric
                |
             verbs API
                |
       +--------+---------+
       | protocol adapter |
       | RoCEv2 or UET    |
       +--------+---------+
                |
       protocol-neutral events
       send / ACK / NACK / loss / ECN
                |
       +--------+---------+
       | congestion ops   |
       | nscc | none | X  |
       +------------------+
```

UET currently uses this operations contract. `nscc` is the default and
`none` is an unsafe comparison baseline:

```text
UIB_PROTOCOL=uet UIB_UET_CC=nscc app
UIB_PROTOCOL=uet UIB_UET_CC=none app
UIB_PROTOCOL=uet UIB_UET_CC=my_algorithm app
```

The third form works after an experiment registers `my_algorithm` with
`uib_congestion_register()` before queue-pair creation. Its static operations
table supplies initialization, admission, ACK-request, send, ACK, NACK, and
inferred-loss callbacks. State must fit `UIB_CONGESTION_STATE_SIZE`, the table
must use `UIB_CONGESTION_ABI_VERSION`, and it must remain valid for the life of
the process. A preloadable experiment module may register from a constructor,
so the target application still requires no source change. The repository's
`tests/congestion_plugin.c` and `make uet-congestion-test` provide a minimal
working example of that path.

```text
cc -shared -fPIC -I/path/to/uib/include my_algorithm.c \
   -L/path/to/uib -Wl,-rpath,/opt/uib/lib -luib -o my_algorithm.so

LD_PRELOAD=/opt/experiments/my_algorithm.so:/opt/uib/libuibv.so \
UIB_PROTOCOL=uet UIB_UET_CC=my_algorithm unchanged-application
```

The module and UIB must use the same ABI version. A run stores the module hash
with its result because the algorithm name alone is not reproducible.

For an in-tree experiment, add the operations table beside
`src/congestion.c`, register it during initialization, rebuild UIB, and select
its name. Do not put protocol wire parsing in the algorithm: the UET or RoCEv2
adapter translates its ACK/NACK/ECN format into the common event structures.
This separation allows the same algorithm to be compared across protocols
where their feedback semantics are equivalent.

RoCEv2 integration is the next step and is not yet implemented. It needs a
pending-send queue so admission delays work requests instead of returning an
application-visible error, plus explicit ECN/CNP feedback translation. A
future selector such as `UIB_ROCEV2_CC=<name>` must not be documented as
working until those data-path tests pass.

The common request form is independent of UET-specific environment names:

```yaml
uib:
  protocol: uet
  congestionControl:
    algorithm: nscc
    parameters:
      maxWindowBytes: 262144
```

The adapter renders this to `UIB_UET_CC=nscc` and
`UIB_UET_NSCC_MAX_WND=262144`. Each result records the algorithm name, ABI,
resolved parameters, UIB commit, protocol, topology, offered load, ECN/loss
settings, throughput, tail latency, retransmissions, and fairness. Comparisons
must include `none` only in an isolated simulation; it is not a deployment
recommendation.

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

## Diagnostic image

The diagnostic image is the inexpensive gate that runs before a model is
downloaded or GPUs are reserved for a long workload. It contains:

- `ibv_devices`, `ibv_devinfo`, and `rdma` from rdma-core;
- `ib_read_bw`, `ib_send_bw`, `ib_write_bw`, and the remaining packaged
  perftest commands;
- all MPI-enabled NCCL tests, including all-reduce, all-gather,
  reduce-scatter, send/receive, and all-to-all;
- NVIDIA CUDA Samples `deviceQuery`;
- Open MPI, `ip`, `ping`, `ethtool`, `lspci`, `jq`, and process inspection.

That image is the NVIDIA variant. A parallel AMD variant will use a pinned
ROCm base and contain RCCL, MPI-enabled `rccl-tests`, `rocminfo`, and the same
network/UIB diagnostics. The two variants share the preflight/result contract
but are built and reported separately; neither image is allowed to satisfy the
other vendor's collective gate.

The CUDA development and runtime inputs are immutable multi-architecture
digests. NCCL is pinned to the same 2.30.4 ABI used by the training image.
NCCL-tests and CUDA Samples are pinned by full commits and archive hashes;
their licenses are included beside the binaries. Exact inputs are recorded in
`simulation/diagnostics/stack.lock.json`, and every installed package version
is recorded in `/opt/uib-diagnostics/resolved-packages.txt`.

Build and exercise it with:

```text
make diagnostic-image
make diagnostic-smoke-test
make diagnostic-uib-preflight-test
make diagnostic-gpu-test       # opt-in; needs NVIDIA CDI and one GPU
```

The source fetcher downloads each hash-verified archive once to the ignored
`simulation/.artifacts/downloads/` directory. Docker retains base images,
package layers, and compiled stages in its local cache. The ignored
`simulation/.artifacts/download-cache-inventory.json` records the large local
objects for later sessions; it is local state and must not be committed.
Override `DIAGNOSTIC_DOWNLOAD_CACHE` to use a shared CI cache.

The default preflight does not require a GPU or RDMA device. It verifies that
all tools and their dynamic libraries are present, exercises hardware-neutral
rdma-core and network inspection, and emits machine-readable capabilities:

```text
docker run --rm uib-diagnostics:test capabilities
docker run --rm uib-diagnostics:test preflight
```

Hardware gates are explicit, so an unavailable device cannot silently reduce
coverage:

```text
docker run --rm --device nvidia.com/gpu=all \
  uib-diagnostics:test preflight --require-gpu
docker run --rm --device /dev/infiniband/uverbs0 \
  uib-diagnostics:test preflight --require-rdma
```

`--require-gpu` runs `deviceQuery` and a small correctness-checked NCCL
all-reduce. It remains a single-rank CUDA gate. Cluster adapters launch the
same `all_reduce_perf` binary through MPI across nodes for the L1 gate.

Mount the common runtime artifact to validate independent composition:

```text
docker run --rm \
  -v "$PWD/simulation/.artifacts/uib-runtime:/opt/uib:ro" \
  uib-diagnostics:test preflight --uib-library-only
```

`--uib-library-only` checks hashes, dependencies, and verbs symbol versions.
`--require-uib` also opens `uib0`; it needs the network namespace, bind
address, and io_uring seccomp permissions described in
[Transparent UIB injection](#transparent-uib-injection). The scheduler must
run this full gate before a distributed workload.

## Where transparent UIB is useful

These use cases are ordered by expected practical value. Only the first two
have end-to-end repository coverage today; later entries require the stated
compatibility work.

1. **Unmodified distributed AI validation.** Exercise NCCL-based training and
   inference images on ordinary Ethernet, reproduce the exact workload in
   containers, and promote it to GPU/RDMA nodes without rebuilding the
   application. This is UIB's primary target.
2. **Hardware-independent CI for direct-verbs applications.** Catch API,
   ordering, completion, timeout, and fallback regressions in disposable jobs
   without reserving scarce RDMA lab hardware. A final real-hardware gate is
   still required for provider-specific behavior and performance.
3. **MPI, UCX, and libfabric compatibility testing.** UCX and libfabric both
   have verbs-backed transports, so they can broaden coverage to MPI, PGAS,
   RMA, and other HPC software after UIB implements their queried verbs,
   connection-management, atomic, and event semantics.
4. **Repeatable protocol, congestion, and failure experiments.** Compare
   RoCEv2 and Ultra Ethernet behavior with controlled latency, loss,
   reordering, ECN, multipath, and rank failure while preserving a normal
   verbs-facing application.
5. **User-space storage validation.** SPDK's NVMe-oF RDMA transport uses
   libibverbs and librdmacm, making it a valuable future target after UIB adds
   the required connection-management and storage workload semantics. This
   does not cover kernel NVMe-oF or other kernel-RDMA clients.
6. **Education and application development.** Let developers inspect QPs,
   memory registration, completions, collectives, and failure modes on a
   workstation before using a shared lab.

Applications tied to a vendor library such as `libmlx5`, direct device
registers, kernel RDMA, or a proprietary NCCL network plugin are not
transparent `libibverbs` substitutions. They need a separate adapter or are
out of scope. Passing a small direct-verbs test also does not imply that UCX,
libfabric, SPDK, or every MPI implementation is already compatible.

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

The implemented inference image uses the official vLLM 0.27.1 CUDA image by
immutable amd64 digest and adds pinned Ray 2.58.0. Its exact stack is recorded
in `simulation/inference/stack.lock.json`: Python 3.12.13, PyTorch
2.13.0+cu130, NCCL 2.30.7, vLLM 0.27.1, Ray 2.58.0, and Transformers 5.15.0.
Kubernetes, Slurm, a local container, and a future `uib-sim` adapter use the
same command:

```text
/opt/uib-inference/bin/inference serve --config /run/workload.json
```

The configuration selects one of two backends without changing the image:

- `transformers` is a single-replica CPU functional backend. It implements
  the OpenAI-compatible models and completions endpoints, so the default test
  can run without a GPU. It is L0 plumbing evidence, not a vLLM performance
  result.
- `vllm` executes the image's unmodified `vllm serve` command. It supports
  tensor, pipeline, and data parallelism through either local multiprocessing
  or Ray and is the production GPU backend.

Build and test the image with:

```text
make inference-image
make inference-smoke-test
make inference-gpu-test       # opt-in; needs NVIDIA CDI and a GPU
```

The mandatory smoke test runs a real local Ray task, starts the CPU backend,
checks `/health` and `/v1/models`, sends warmup plus concurrent completions,
repeats the workload through the streaming API, and records deterministic
request traces and results. The GPU test starts the real vLLM OpenAI server,
runs the same deterministic probe, and then runs the unmodified standard
`vllm bench serve` load generator with fixed random input length, output
length, concurrency, and seed. The standard benchmark JSON is saved beside
the common result.

### Small default and production models

The default profile uses the public 135-million-parameter
[`HuggingFaceTB/SmolLM2-135M-Instruct`](https://huggingface.co/HuggingFaceTB/SmolLM2-135M-Instruct)
safetensors model at full commit
`12fd25f77366fa6b3b4b768ec3050bf629380bac`. It is large enough to exercise
the real loaders and vLLM engine while remaining practical on CPU and a small
GPU. The weights are not in this repository or image. They are downloaded on
the first test and reused from `simulation/.artifacts/huggingface`.

Production testing copies the complete default JSON and changes the immutable
model ID and revision, sizing, precision, quantization, parallelism, and load
fields. `INFERENCE_GPU_CONFIG=/absolute/path/workload.json` lets the GPU target
use that external configuration. Private repositories use the standard
`HF_TOKEN` environment; credentials never belong in workload JSON or results.

### Inference configuration

`simulation/inference/workload.schema.json` rejects unknown fields and
invalid basic ranges. The entry point also rejects distributed or quantized
settings when the CPU backend is selected.

| Field | Meaning |
|---|---|
| `backend` | CPU-functional `transformers` or production `vllm` |
| `model.id`, `revision`, `servedName` | Repository, immutable commit, and API name |
| `server.host`, `port` | OpenAI-compatible listening address |
| `server.dtype`, `quantization` | Weight/activation type and optional vLLM quantizer |
| `server.maxModelLength` | Maximum prompt plus generated context |
| `server.tensorParallel`, `pipelineParallel` | Model partition sizes |
| `server.dataParallel`, `replicas` | vLLM data-parallel group and launcher replicas |
| `server.gpuMemoryUtilization` | Fraction available to weights, graphs, and KV cache |
| `server.distributedExecutor` | Local `mp`, multi-node `ray`, or automatic selection |
| `load.requests`, `concurrency`, `requestRate` | Closed- or open-loop traffic shape |
| `load.promptTokens`, `outputTokens` | Deterministic request sizes |
| `load.streaming`, `warmupRequests` | Streaming mode and unmeasured warmup |
| `load.timeoutSeconds`, `seed` | Readiness/request bound and reproducibility seed |
| `result.directory` | Shared evidence root |

The common result includes the image stack, model revision, workload identity,
parallelism, request count, token count, latency distribution, throughput,
response hashes, trace path, and injected transport environment. The trace
stores prompts and scheduled offsets so native-verbs and UIB runs can receive
identical application traffic.

### Multi-GPU and multi-node inference

On a multi-GPU node, configure TP, PP, and DP independently. Their product
must fit the exposed CDI device set:

```text
make INFERENCE_GPU_TENSOR_PARALLEL=4 inference-gpu-test
make INFERENCE_GPU_TENSOR_PARALLEL=2 \
  INFERENCE_GPU_PIPELINE_PARALLEL=2 inference-gpu-test
make INFERENCE_GPU_DATA_PARALLEL=4 inference-gpu-test
```

The local target passes those values into the same JSON-to-`vllm serve`
adapter. For multi-node use, start Ray on every node after injecting the UIB
environment, set `server.distributedExecutor=ray`, and launch the same image
and `serve --config` command on the head. KubeRay and Slurm/Pyxis are launcher
adapters around that contract; direct vLLM multiprocessing remains a
replaceable, separately labeled launcher.

Moving to physical hardware changes mounts, addresses, CDI resources, model
size, and the launcher—not image code. Mount the identical model cache and UIB
runtime artifact, inject `NCCL_NET=IB`, `NCCL_IB_HCA==uib0`, and
`NCCL_IBVERBS_LIB=/opt/uib/libuibv.so`, and keep the same workload JSON for the
UIB/native pair. The evidence gate must require NCCL `NET/IB` and reject Socket
fallback just as the training gate does.

This host validated one-GPU vLLM serving and therefore did not send distributed
inference traffic through UIB. Multi-GPU and physical multi-node inference,
Ray-based vLLM workers, and UIB transport remain explicit unvalidated gates;
one vLLM rank can initialize NCCL but cannot validate libuib traffic.

## Network and failure simulation

Configurable link features include:

- bandwidth and oversubscription;
- base latency, jitter, correlation, and distribution;
- random loss with optional correlation; state/Gilbert-Elliott burst models
  remain a planned overlay extension;
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

## Network emulation with netem

The network overlay maps directly to Linux `tc netem`; there is no separate
network simulator or invented syntax. Render the full configuration, inspect
the exact command, then apply it to an explicitly named interface:

```text
simulation/bin/uib-sim render --profile virtme-kubernetes \
  --overlay network/constrained --format json > /tmp/uib-config.json

simulation/network/netem render /tmp/uib-config.json tap-uib-0
simulation/network/netem apply  /tmp/uib-config.json tap-uib-0
simulation/network/netem show   tap-uib-0
simulation/network/netem clear  tap-uib-0
```

`render` is read-only. `apply` prints the command before invoking it. It may
replace an existing root `netem` qdisc, but refuses to replace a different
root qdisc unless `--replace-existing` is explicitly supplied. `clear` deletes
only a root qdisc whose kind is `netem`; it does not delete an unrelated qdisc.
Set `NETEM_TC=/path/to/tc` when `tc` is outside `PATH`.

The checked-in conditions are intentionally small and readable:

| Overlay | Effective condition |
|---|---|
| `network/clean` | Shaping disabled |
| `network/datacenter` | 25 Gbit/s, 100 us delay, 20 us normal jitter, 10% delay correlation, 8192-packet queue, seed 12345 |
| `network/constrained` | 100 Mbit/s, 2 ms delay, 200 us normal jitter, 256-packet queue, seed 12345 |
| `network/lossy` | 1 ms delay, 1% random loss, 25% loss correlation, 1024-packet queue, seed 12345 |
| `network/reordered` | 2 ms delay, 500 us normal jitter, 10% reordering, 25% correlation, gap 5, seed 12345 |
| `network/ecn` | 10 Gbit/s, 250 us delay, 2% loss decision model converted to ECN marks for ECN-capable packets, 512-packet queue, seed 12345 |

### Netem configuration fields

All percentages are numbers from 0 through 100. Durations accept `ns`, `us`,
`ms`, or `s`. Rates accept an integer followed by `kbit`, `Mbit`, `Gbit`, or
the corresponding case variants.

| Field under `network.shaping` | Default | Netem mapping |
|---|---:|---|
| `enabled` | Required | Apply or remove the owned netem condition |
| `bandwidth` | Unset | `rate RATE` |
| `latency` | Unset | `delay TIME` |
| `jitter` | Unset | Second `delay` argument |
| `delayCorrelationPercent` | Unset | Third `delay` argument |
| `delayDistribution` | Unset | `uniform`, `normal`, `pareto`, or `paretonormal` |
| `lossPercent` | Unset | `loss random PERCENT` |
| `lossCorrelationPercent` | Unset | Correlation for random loss |
| `duplicatePercent`, `duplicateCorrelationPercent` | Unset | Packet duplication and correlation |
| `corruptPercent`, `corruptCorrelationPercent` | Unset | Random packet corruption and correlation |
| `reorderPercent`, `reorderCorrelationPercent` | Unset | Percentage reordering and correlation |
| `reorderGap` | Unset | Send every Nth packet immediately |
| `queueLimit` | Netem default | Maximum queued packets |
| `packetOverheadBytes` | Unset | Per-packet rate overhead; may be negative |
| `cellSizeBytes`, `cellOverheadBytes` | Unset | Cell-based rate emulation |
| `ecn` | `false` | Mark ECN-capable packets instead of dropping according to the loss model |
| `seed` | Unset | Seed for reproducible random decisions |

Custom example with duplication and corruption:

```yaml
network:
  shaping:
    enabled: true
    bandwidth: 500Mbit
    latency: 5ms
    jitter: 1ms
    delayCorrelationPercent: 20
    delayDistribution: paretonormal
    lossPercent: 0.1
    duplicatePercent: 0.05
    corruptPercent: 0.01
    queueLimit: 512
    seed: 9001
```

Apply egress shaping to every endpoint-facing interface whose traffic belongs
to the run. For a two-node tap topology, apply the same or deliberately
asymmetric condition to both taps. For realistic TCP receive-side tests,
place netem on the receiver ingress path (normally an IFB or the peer egress),
because TCP Small Queues can distort sender-egress-only results. Netem timing
and rate accuracy are limited by kernel clock/timer granularity, and a seed
does not guarantee identical packet decisions across different kernel/iproute2
versions. Record both versions with the run. See the
[upstream netem manual](https://man7.org/linux/man-pages/man8/tc-netem.8.html).

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

### NCCL to RCCL

Keep the workload contract, UIB bundle, protocol, network profile, and result
schema, but select the matching vendor image and front end:

```yaml
communication:
  frontend: nccl
accelerator:
  vendor: nvidia
```

becomes:

```yaml
communication:
  frontend: rccl
accelerator:
  vendor: amd
```

CUDA and ROCm images are different artifacts, and numerical/performance
comparisons must record that hardware/software change. Training source and
checkpoint formats should remain portable where the selected framework
supports both backends.

### NCCL collectives to MPI over UCX or libfabric

A diagnostic or MPI workload can retain its image, rank layout, launcher,
UIB bundle, and network profile while changing:

```yaml
communication:
  frontend: ucx
  transportAllowlist: [rc, ud]
```

to:

```yaml
communication:
  frontend: libfabric
  provider: "ofi_rxm;verbs"
```

This swaps the communication API and therefore is not the same application
benchmark. It is useful for isolating whether a failure belongs to UIB's
verbs surface, UCX, libfabric, or the collective library. Both profiles must
assert the selected verbs transport and reject software-network fallbacks.

### Congestion algorithm without changing the workload

```yaml
uib:
  protocol: uet
  congestionControl:
    algorithm: nscc
```

can select another registered algorithm without changing the image, command,
model, dataset, ranks, or scheduler. The semantic diff records only the
algorithm and its parameters. Changing UET to RoCEv2 is a protocol comparison,
not merely an algorithm comparison, and must be labeled accordingly.

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

Configuration stays deliberately conventional: a small defaults file, one
YAML profile, an optional YAML workload request, and standard JSON Schema
validation. The thin `uib-sim` wrapper only merges, validates, and inspects
configuration; it does not replace Kind, OpenTofu, Ansible, Helm, `kubectl`,
Slurm, Pyxis, or Enroot.

```text
simulation/bin/uib-sim render --profile kind-kubernetes
simulation/bin/uib-sim doctor --profile kind-kubernetes

simulation/bin/uib-sim render \
  --profile virtme-kubernetes \
  --overlay communication/ucx \
  --overlay protocol/uet-nscc \
  --request requests/training-smoke.yaml \
  --set uib.congestionControl.parameters.maxWindowBytes=262144
```

Overlays are repeatable and correspond to replaceable layers. For example,
change only `communication/ucx` to `communication/libfabric`, or only
`protocol/uet-nscc` to `protocol/rocev2`. A site can pass a YAML file path
instead of a checked-in overlay. The rendered file is ordinary YAML that can
be inspected, versioned, and translated directly to Helm values, OpenTofu
variables, an Ansible inventory, or Slurm environment settings.

The schema rejects unknown keys and malformed units. A few cross-field checks
reject impossible rank products, incompatible scheduler/runtime choices,
RCCL without AMD GPUs, unvalidated GPU-direct, premature RoCEv2 congestion
plug-ins, and shaping existing hardware without explicit permission.
`doctor` is read-only and reports which standard tools are missing. Run
`make simulation-cli-test` to validate every checked-in profile. The wrapper
requires Python, PyYAML, and jsonschema.

Each backend will keep using its own normal configuration and state:

- Kind configuration and cluster names;
- OpenTofu state for libvirt resources;
- Ansible inventories and roles for node provisioning;
- Helm values and Kubernetes labels/namespaces;
- Slurm batch files, job IDs, Pyxis, and Enroot configuration.

`uib-sim` will delegate the planned lifecycle to those tools:

```text
simulation/bin/uib-sim doctor --profile <profile>
kind create cluster --config <rendered-kind-config>
tofu -chdir=<run-directory> apply
ansible-playbook -i <inventory> <playbook>
helm upgrade --install <release> <chart> -f <rendered-values>
sbatch <rendered-job>
```

`doctor` is read-only. It reports missing tools, kernel features, GPU and RDMA
capabilities, scheduler versions, storage, permissions, and the highest valid
validation level.

The later `up` adapter creates only infrastructure owned by the selected
profile. For existing
clusters it performs no infrastructure changes.

The later `deploy` adapter installs or detects scheduler components and stages images and UIB
artifacts.

The later `run` adapter resolves configuration, renders scheduler resources, executes
preflight gates, submits the workload, and records ownership.

The later `collect` operation can be repeated and does not delete the workload.

The later `down` operation exports final evidence and asks each standard tool
to remove only resources in the ownership
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
- communication front-end family, UIB protocol, and congestion algorithm,
  except in an explicitly named comparison;
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
7. Run rdma-core diagnostics and the selected two-rank NCCL, RCCL, UCX, or
   libfabric correctness gate.
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
- communication front end and its selected or fallback transport;
- UIB protocol, congestion algorithm, and resolved algorithm parameters;
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

## Simulation test reference

Run the smallest applicable gate first. Configuration and network-rendering
tests require no container engine or accelerator. Image tests use a container
engine, and communication tests deliberately fail when their required real
library or hardware is absent; they do not turn missing coverage into a pass.

### Configuration and network tests

| Target | Requirements | What it validates |
|---|---|---|
| `make simulation-cli-test` | Python 3, PyYAML, and `jsonschema` | Every profile, named layer replacement, schema and semantic rejection, typed `--set`, and the read-only `doctor` report |
| `make simulation-netem-test` | The CLI requirements plus `jq` | Every checked-in network condition renders to the expected `tc netem` command; unsafe interfaces and invalid combinations are rejected; it does not modify a qdisc |
| `make simulation-congestion-test` | Build dependencies, `ip`, `jq`, `tc`, `timeout`, `unshare`, and permission to create a network namespace | UET SEND, RDMA read/write, and atomics complete with actual netem conditions and the selected built-in or plugin congestion algorithm |

Run all three:

```text
make simulation-cli-test
make simulation-netem-test
make simulation-congestion-test
```

The congestion test creates a disposable network namespace, applies netem to
its loopback interface, runs `tests/loopback`, and destroys the namespace. It
never changes the host root qdisc. Its options are:

| Variable | Default | Syntax and values |
|---|---|---|
| `SIMULATION_CC_MATRIX` | `nscc:clean nscc:datacenter nscc:constrained nscc:lossy nscc:reordered none:constrained test-pass:datacenter` | Space-separated `algorithm:condition` pairs. Test algorithms are `nscc`, `none`, and the sample plugin `test-pass`; conditions are `clean`, `datacenter`, `constrained`, `lossy`, `reordered`, and `ecn` |
| `SIMULATION_CC_TIMEOUT` | `60` | Positive timeout in seconds for each matrix entry |
| `SIMULATION_CC_MAX_WINDOW` | `65536` | NSCC maximum window in bytes |
| `SIMULATION_NETNS_PRIVILEGE` | Auto-detect user namespaces, then `sudo` when present | Command prefix used only to execute `unshare --net`, for example `sudo` |
| `NETEM_TC` | `tc` or `/sbin/tc` | Exact `tc` executable used by the netem adapter |

For example:

```text
SIMULATION_CC_MATRIX='nscc:ecn none:lossy' \
SIMULATION_CC_TIMEOUT=90 make simulation-congestion-test
```

`test-pass` requires `tests/congestion_plugin.so`, which the Make target builds
automatically. Other algorithm names are rejected because this test must know
whether and how to load their plugin. Add the algorithm to the test runner when
adding a new test plugin.

### Runtime and diagnostic tests

| Target | Requirements | Command | What it validates |
|---|---|---|---|
| Runtime bundle | Normal static and shared build dependencies | `make runtime-bundle-test` | Reproducible archive, standalone archive, provider library, manifest, compatibility lock, symlinks, and library-only preflight |
| CPU diagnostic image | Docker-compatible container engine and download access on the first run | `make diagnostic-smoke-test` | Pinned tool versions, capabilities JSON, and CPU-safe preflight |
| UIB diagnostic preflight | Container engine and runtime bundle build dependencies | `make diagnostic-uib-preflight-test` | The independently built image can mount and load the UIB runtime artifact |
| GPU diagnostic | NVIDIA driver, accessible CUDA GPU, and container GPU support | `make diagnostic-gpu-test` | CUDA `deviceQuery` and a correctness-checked single-rank NCCL collective |

Runtime and diagnostic options:

| Variable | Default | Meaning |
|---|---|---|
| `RUNTIME_ARTIFACT_DIR` | `simulation/.artifacts` | Bundle and persistent download-cache root; it is ignored by Git |
| `DIAGNOSTIC_IMAGE` | `uib-diagnostics:test` | Local image name |
| `DIAGNOSTIC_CONTAINER_ENGINE` | Value of `TRAINING_CONTAINER_ENGINE` | Container command |
| `DIAGNOSTIC_DOWNLOAD_CACHE` | `$(RUNTIME_ARTIFACT_DIR)/downloads` | Persistent source cache; downloads are reused by later runs |
| `DIAGNOSTIC_GPU_DEVICE` | `nvidia.com/gpu=all` | Device selector passed to the container engine |
| `DIAGNOSTIC_SKIP_BUILD` | `0` | Direct-script option; set to `1` to reuse an existing image |
| `DIAGNOSTIC_UIB_RUNTIME` | Unset | Direct-script path to a runtime bundle mounted read-only at `/opt/uib` |

The GPU diagnostic has one rank. It validates the CUDA and NCCL installation,
not the UIB data plane.

### Communication front-end tests

| Target | Requirements | What it validates |
|---|---|---|
| `make provider-nccl-abi-test` | libibverbs development files | The provider exposes the versioned ABI and calls NCCL's required verbs entry points; no GPU is used |
| `make nccl-test` | NVIDIA GPU and driver, CUDA MPS, MPI, `nccl-tests` `all_reduce_perf`, `setpriv`, `unshare`, and `timeout` | Two local NCCL ranks use `libuibv.so`, the NCCL IB network, and UIB traffic; Socket fallback and data errors fail the test |
| `make rccl-test` | At least two accessible AMD GPU ranks, ROCm/RCCL, MPI-enabled `rccl-tests`, `rocminfo`, `setpriv`, `unshare`, and `timeout` | Two or more local RCCL ranks select the IB network through UIB without Socket fallback |
| `make ucx-libfabric-test` | `ucx_info`, `ucx_perftest`, `fi_info`, `fi_pingpong`, `fi_rdm_pingpong`, and `timeout` | Actual two-process UCX tag traffic over `rc`/`ud` and libfabric message/RDM traffic over verbs; TCP, shared-memory, and sockets fallbacks fail the test |

NCCL options:

| Variable | Default | Meaning |
|---|---|---|
| `NCCL_TESTS_BIN` | First `all_reduce_perf` in `PATH` | Exact MPI-enabled `nccl-tests` binary |
| `MPIEXEC` | `mpirun` | MPI launcher |
| `NCCL_TEST_MAX_BYTES` | `64K` | Largest collective buffer accepted by `all_reduce_perf -e` |
| `NCCL_TEST_ITERATIONS` | `3` | Measured iterations |
| `NCCL_TEST_WARMUPS` | `1` | Warm-up iterations |
| `NCCL_TEST_TIMEOUT` | `90` | Whole-test timeout in seconds |
| `NCCL_TEST_BIND_PORT` | `19700` | UIB UDP bind port |

RCCL options:

| Variable | Default | Meaning |
|---|---|---|
| `RCCL_TESTS_BIN` | First `all_reduce_perf` in `PATH` | Exact MPI-enabled `rccl-tests` binary |
| `MPIEXEC` | `mpirun` | MPI launcher |
| `RCCL_TEST_RANKS` | `2` | Rank count; values below two are rejected |
| `HIP_VISIBLE_DEVICES` | `0,1` | AMD devices visible to the ranks |
| `RCCL_TEST_MAX_BYTES` | `64K` | Largest collective buffer |
| `RCCL_TEST_ITERATIONS` | `3` | Measured iterations |
| `RCCL_TEST_WARMUPS` | `1` | Warm-up iterations |
| `RCCL_TEST_TIMEOUT` | `90` | Whole-test timeout in seconds |
| `RCCL_TEST_BIND_PORT` | `19900` | UIB UDP bind port |

UCX/libfabric options:

| Variable | Default | Meaning |
|---|---|---|
| `COMM_TEST_ITERATIONS` | `100` | UCX performance-test iterations |
| `COMM_TEST_TIMEOUT` | `30` | Timeout in seconds for each client or server |
| `COMM_TEST_BIND_PORT` | `19800` | UIB and UCX test port |

Examples:

```text
NCCL_TESTS_BIN=/opt/nccl-tests/build/all_reduce_perf \
NCCL_TEST_MAX_BYTES=1M make nccl-test

RCCL_TESTS_BIN=/opt/rccl-tests/build/all_reduce_perf \
RCCL_TEST_RANKS=4 HIP_VISIBLE_DEVICES=0,1,2,3 make rccl-test

COMM_TEST_ITERATIONS=1000 COMM_TEST_TIMEOUT=60 \
make ucx-libfabric-test
```

`nccl-test` intentionally uses two ranks on one GPU through MPS. That is real
NCCL/UIB traffic, but not multi-GPU or multi-node validation. `rccl-test`
requires AMD hardware and was not run on the NVIDIA-only development host.
The UCX/libfabric gate was not run there because those tools were unavailable.

### Training tests

| Target | Requirements | What it validates |
|---|---|---|
| `make training-smoke-test` | Container engine; image build access on first run | CPU DDP with two Gloo ranks, checkpoint/restart equivalence, and pinned NCCL library loading |
| `make training-public-test` | The smoke-test requirements and model-download access on first run | Small public Hugging Face model/dataset training on CPU; downloads persist in the ignored cache |
| `make training-gpu-test` | NVIDIA driver, accessible GPU or GPUs, and container GPU support | DDP or FSDP on CUDA plus checkpoint/restart; multiple processes use multiple visible GPUs when available |
| `make training-uib-preflight-test` | Container engine and runtime bundle dependencies | The training image can mount and load the UIB runtime without a GPU |
| `make training-uib-test` | NVIDIA GPU, CUDA MPS, MPI, container GPU support, `nvidia-cuda-mps-control`, `nvidia-smi`, and `timeout` | Two unmodified PyTorch DDP ranks send NCCL gradient traffic through UIB and reject Socket fallback |

Training options:

| Variable | Default | Values and meaning |
|---|---|---|
| `TRAINING_IMAGE` | `uib-training:test` | Local image name |
| `TRAINING_CONTAINER_ENGINE` | `docker` | Container command |
| `TRAINING_SKIP_BUILD` | `0` | Direct-script option; `1` reuses the existing image |
| `TRAINING_HF_CACHE` | `simulation/.artifacts/huggingface` | Persistent Hugging Face cache |
| `TRAINING_GPU_DEVICE` | `nvidia.com/gpu=all` | Container device selector |
| `TRAINING_GPU_PROCESSES` | `1` | Positive local CUDA process count |
| `TRAINING_GPU_STRATEGY` | `fsdp` | `ddp` or `fsdp`; other values are rejected |
| `TRAINING_UIB_RUNTIME` | `simulation/.artifacts/uib-runtime` | Runtime-bundle directory used by the UIB test |
| `TRAINING_UIB_TIMEOUT` | `180` | Two-rank UIB test timeout in seconds |
| `TRAINING_UIB_BIND_PORT` | `19720` | UIB UDP bind port |
| `TRAINING_MASTER_PORT` | `29720` | PyTorch rendezvous port |
| `MPIEXEC` | `mpirun` | MPI launcher for the UIB test |

Examples:

```text
TRAINING_HF_CACHE=/data/uib-cache/huggingface \
make training-public-test

TRAINING_GPU_PROCESSES=4 TRAINING_GPU_STRATEGY=fsdp \
make training-gpu-test

TRAINING_UIB_TIMEOUT=300 TRAINING_UIB_BIND_PORT=19820 \
make training-uib-test
```

`TRAINING_GPU_PROCESSES` must not exceed the usable visible GPU count for an
ordinary multi-GPU run. The special `training-uib-test` always uses two ranks
on one GPU through MPS and therefore does not use that variable.

### Inference tests

| Target | Requirements | What it validates |
|---|---|---|
| `make inference-smoke-test` | Container engine and model-download access on first run | CPU Transformers serving, streaming and non-streaming OpenAI-compatible probes, local Ray, and generated vLLM arguments |
| `make inference-gpu-test` | NVIDIA driver, container GPU support, and enough visible GPUs for the selected parallel product | Actual vLLM serving, deterministic request probe, and standard `vllm bench serve` result generation |

Inference options:

| Variable | Default | Meaning |
|---|---|---|
| `INFERENCE_IMAGE` | `uib-inference:test` | Local image name |
| `INFERENCE_CONTAINER_ENGINE` | Value of `TRAINING_CONTAINER_ENGINE` | Container command |
| `INFERENCE_SKIP_BUILD` | `0` | Direct-script option; `1` reuses the existing image |
| `INFERENCE_HF_CACHE` | `simulation/.artifacts/huggingface` | Persistent Hugging Face cache |
| `INFERENCE_GPU_DEVICE` | Value of `TRAINING_GPU_DEVICE` | Container device selector |
| `INFERENCE_GPU_TENSOR_PARALLEL` | `1` | Positive tensor-parallel degree |
| `INFERENCE_GPU_PIPELINE_PARALLEL` | `1` | Positive pipeline-parallel degree |
| `INFERENCE_GPU_DATA_PARALLEL` | `1` | Positive data-parallel degree |
| `INFERENCE_GPU_CONFIG` | Generated tiny GPU configuration | Direct-script path to a complete workload JSON configuration |

The three parallel degrees must be positive integers. Their product is the
required vLLM worker count, so it cannot exceed available resources. A useful
two-GPU example is:

```text
INFERENCE_GPU_TENSOR_PARALLEL=2 \
INFERENCE_GPU_PIPELINE_PARALLEL=1 \
INFERENCE_GPU_DATA_PARALLEL=1 make inference-gpu-test
```

The CPU default downloads `HuggingFaceTB/SmolLM2-135M-Instruct` only once into
the ignored cache. Larger models use a custom request or
`INFERENCE_GPU_CONFIG`; model files remain outside the repository.

### Physical multi-node promotion tests

The following gates are requirements, not local Make targets yet. They remain
explicitly pending until a suitable cluster is available.

| Gate | Minimum requirement | Reusable workload | Pass condition |
|---|---|---|---|
| Two-node NCCL | NVIDIA GPU and working network on each of two physical nodes | The same `nccl-tests` all-reduce settings | Both ranks select UIB's IB network, no Socket fallback, correct data |
| Two-node RCCL | AMD GPU and working network on each of two physical nodes | The same `rccl-tests` all-reduce settings | Both ranks select UIB's IB network, no Socket fallback, correct data |
| Kubernetes training | At least two GPU workers, Kubeflow Trainer, GPU device plugin, and UIB runtime injection | The same training image and workload configuration | Correct placement, checkpoint/restart, loss, and UIB evidence |
| Slurm training | At least two GPU nodes with Slurm, Pyxis, and Enroot | The same training image and workload configuration | Correct rank placement, checkpoint/restart, loss, and UIB evidence |
| Kubernetes inference | At least two GPU workers and KubeRay or equivalent launcher | The same inference image, model, and probe | Correct TP/PP/DP placement, responses, benchmark, and UIB evidence |
| Slurm inference | At least two GPU nodes with Slurm, Pyxis, and Enroot | The same inference image, model, and probe | Correct placement, responses, benchmark, and UIB evidence |
| GPU-direct promotion | Compatible GPUs, drivers, DMA-BUF/RDMA support, and at least two physical nodes | Any passing two-node collective | Evidence that payloads use the validated GPU-memory path, not host staging |
| Fabric qualification | Production-like switches, links, GPUs, and RDMA NICs | Simulation request plus a native-verbs baseline | Correctness and comparative metrics under congestion and faults |

These tests reuse immutable images and request files. A site overlay changes
only inventory, device selectors, interface names, addresses, storage, and
scheduler-specific placement. The application entrypoint, model revision,
dataset revision, parallelism, correctness probes, and metric definitions stay
unchanged, which makes simulation-to-hardware differences reviewable.

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

- The cluster lifecycle harness, scheduler adapters, DeepSpeed adapter, and
  Megatron-Core adapter are planned and not yet implemented.
- The diagnostic image passed its hardware-neutral, UIB runtime-injection,
  CUDA `deviceQuery`, and single-GPU NCCL all-reduce gates. Its NCCL run has
  one rank and therefore does not send UIB traffic.
- UIB's versioned NCCL loader ABI and a two-rank, host-staged NCCL all-reduce
  are covered by repository tests. The actual collective test is opt-in
  because it requires CUDA, NCCL, MPI, MPS, a GPU, and UTS namespace access.
- UCX/libfabric and RCCL compatibility gates are specified but have not yet
  passed. UIB does not currently claim transparent compatibility with them.
- The congestion-control registry and UET/NSCC adapter are implemented.
  RoCEv2 admission and feedback integration is still planned.
- Kind and CPU-only virtme/libvirt profiles do not validate real GPU
  collectives or performance.
- The training image passed CPU DDP, checkpoint/restart, public tiny-model,
  actual single-GPU NCCL initialization, single-GPU FSDP restart, and local
  two-rank NCCL DDP through UIB. The two ranks share one GPU through MPS;
  multi-GPU and physical multi-node training remain unvalidated.
- The inference image passed a CPU OpenAI-compatible service, local Ray task,
  single-GPU vLLM service, deterministic probe, and standard
  `vllm bench serve`. Distributed inference and inference through UIB remain
  unvalidated because this host has one GPU.
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
- [RCCL environment variables](https://rocm.docs.amd.com/projects/rccl/en/latest/api-reference/env-variables.html)
- [ROCm systems source tree](https://github.com/ROCm/rocm-systems)
- [UCX runtime configuration](https://openucx.readthedocs.io/en/master/running.html)
- [UCX transports](https://openucx.readthedocs.io/en/master/faq.html)
- [libfabric verbs provider](https://ofiwg.github.io/libfabric/main/man/fi_verbs.7.html)
- [libfabric provider selection](https://ofiwg.github.io/libfabric/main/man/fi_fabric.3.html)
- [SPDK NVMe-oF RDMA transport](https://spdk.io/doc/nvmf_tgt_pg.html)
- [OpenTofu](https://opentofu.org/docs/)
- [libvirt](https://libvirt.org/docs.html)
- [virtme-ng](https://github.com/arighi/virtme-ng)
- [Pyxis](https://github.com/NVIDIA/pyxis)
- [Enroot](https://github.com/NVIDIA/enroot)
