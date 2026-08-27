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
- [virtme Slurm lifecycle](#virtme-slurm-lifecycle)
- [Libvirt Slurm lifecycle](#libvirt-slurm-lifecycle)
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
| `make simulation-kubernetes-uib-test` | CPU | No | Unchanged verbs programs exchange actual RC SEND/RECV and RDMA Write traffic through UIB between two Kind workers |
| `make simulation-virtme-slurm-e2e-test` | CPU with KVM or QEMU emulation | No | Real controller/two-worker Slurm/Pyxis/Enroot lifecycle and 32-message, 2051-byte RC traffic through UIB both directly and inside Pyxis, with exact cleanup |
| `make simulation-virtme-netem-e2e-test` | CPU with KVM or QEMU emulation | No | The same virtme Slurm UIB gate with a named netem condition active on both worker data interfaces and validated post-traffic evidence |
| `make simulation-virtme-training-e2e-test` | CPU with KVM or QEMU emulation | No | The same cross-worker UIB gate plus real two-rank CPU DDP training through Slurm/Pyxis/Enroot; Gloo training is not UIB collective traffic |
| `make simulation-virtme-inference-e2e-test` | CPU with KVM or QEMU emulation | No | The same cross-worker UIB gate plus offline SmolLM2 inference from the persistent cache; implemented, but its local resource gate is not yet recorded as passing |
| `make simulation-libvirt-e2e-test` | CPU with KVM | No | Disposable Slurm/Pyxis/Enroot controller/two-worker lifecycle, actual cross-VM RC payload traffic through UIB, and remote `run`/`status`/`collect`/`down` |
| `make simulation-libvirt-netem-e2e-test` | CPU with KVM | No | The same cross-VM UIB gate while a selected netem condition is active on both workers, with collected qdisc and version evidence |
| `make simulation-libvirt-training-e2e-test` | CPU with KVM | No | The same UIB gate plus real two-rank CPU DDP training in a staged local OCI filesystem through Slurm/Pyxis/Enroot; Gloo training is not UIB collective traffic |
| `make simulation-libvirt-inference-e2e-test` | CPU with KVM | No | The same UIB gate plus real offline SmolLM2 serving from a staged image and dynamic model cache through Slurm/Pyxis/Enroot; one serving task has no collective traffic |
| `make nccl-test` | One NVIDIA GPU with local MPS support | No | Two local ranks send host-staged NCCL traffic through UIB |
| `make training-uib-test` | One NVIDIA GPU with local MPS support | No | Unmodified two-rank PyTorch DDP sends gradient traffic through UIB |
| UCX verbs transport gate | CPU | No | Two local processes exchange tag, stream, UCP put/get, fetch-add, and compare-swap traffic through `ud_verbs/uib0:1`, with TCP and shared-memory transports excluded |
| libfabric verbs provider gate | CPU | No | `fi_info` discovers `uib0-dgram`, then a two-process data-integrity fabtest uses `verbs;ofi_rxd`; sockets/tcp providers are excluded |
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
gate. UCX must select a verbs transport. The currently validated libfabric
chain is `verbs;ofi_rxd`, which layers an RDM endpoint over UIB's verbs UD
endpoint. TCP, sockets, shared-memory, and vendor-direct fallbacks are
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
libvirt:
  uri: qemu:///system
  pool: default
  networkMode: bridge
  baseImage: https://cloud-images.ubuntu.com/releases/noble/release-20260814/ubuntu-24.04-server-cloudimg-amd64.img
  baseImageSha256: 6e40c07ae715f744f84af0bec76415cc1987dd115b4b8de437818561f01a3733
  sshUser: uib
  diagnosticImage: docker.io/library/ubuntu@sha256:2edbbc5dc405e9612ba3584ce95480277e3eb374407b5505fe26f17df77c7dbc
cluster:
  manager: slurm
  version: "23.11.4"
container:
  runtime: enroot
  integration: pyxis
launcher:
  training: slurm
  inference: slurm-ray
nodes:
  controlPlane: 1
  workers: 4
  cpu: 16
  memory: 64Gi
  disk: 100Gi
network:
  dataInterface: eth1
```

The checked-in profiles pin every downloaded image and software version.
Change node capacity freely, but update a URL and its SHA-256 digest together.
`rootless` mode is described under [Libvirt Slurm lifecycle](#libvirt-slurm-lifecycle).

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
| virtme network | `virtme-bridge` (optional; the profiles default to rootless socket networking) |
| Communication | `communication/nccl`, `communication/rccl`, `communication/ucx`, `communication/libfabric` |
| Protocol and congestion | `protocol/rocev2`, `protocol/rocev2-nscc-experimental`, `protocol/uet-nscc`, `protocol/uet-none` |
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
| `infrastructure.allowExistingMutations` | `true`, `false` | Permit explicitly requested shaping on existing hardware |
| `virtme.mode` | `kind`, `multi-guest` | One guest containing Kind, or one guest per scheduler node |
| `virtme.networkMode` | `socket`, `bridge` | Rootless QEMU socket Ethernet (default), or a host Linux bridge |
| `virtme.bridgePrefix` | 2-6 lowercase letters/digits, beginning with a letter | Bridge-name prefix used only in `bridge` mode; the deterministic run-owned name remains within Linux's 15-character limit |
| `virtme.sshPortBase` | Integer 1024-65534; default `23000` | Controller SSH-forward port; worker ports are consecutive |
| `virtme.extraKernelArguments` | List of non-empty strings | Extra `--append` arguments added separately to every generated virtme-ng command |
| `libvirt.uri` | Libvirt connection URI | `qemu:///system` for bridge mode or `qemu:///session` for passwordless rootless mode |
| `libvirt.pool` | Existing active storage-pool name | Pool in which run-owned overlays and cloud-init volumes are created |
| `libvirt.networkMode` | `bridge`, `rootless` | Managed libvirt networks, or passt management plus an unprivileged multicast data plane |
| `libvirt.sshPortBase` | Integer 1024-65534; default `22000` | First localhost SSH-forward port in rootless mode; the controller uses the base and workers use consecutive ports |
| `libvirt.baseImage` | HTTPS URL | Pinned Ubuntu cloud image downloaded once into the ignored cache |
| `libvirt.baseImageSha256` | 64 lowercase hexadecimal digits | Required digest checked before the cached base image is used |
| `libvirt.sshUser` | Linux user name | Cloud-init administrator used only through the run-owned SSH key |
| `libvirt.diagnosticImage` | OCI reference with an `@sha256:` digest | Image used by the Pyxis/Enroot provisioning preflight |
| `cluster.manager` | `kubernetes`, `slurm` | Scheduler/control plane |
| `cluster.version` | Non-empty version | Kubernetes or Slurm version selected by the adapter |
| `cluster.nodeImage` | OCI reference with an `@sha256:` digest | Reproducible Kind node image |
| `kubernetes.distribution` | `k3s` | Multi-VM Kubernetes provisioner; Kind and existing clusters do not use this setting |
| `kubernetes.podCIDR` | IPv4 or IPv6 CIDR | Pod overlay network; it must not overlap management, data, or service networks |
| `kubernetes.serviceCIDR` | IPv4 or IPv6 CIDR | ClusterIP network; it must not overlap management, data, or pod networks |
| `kubernetes.disable` | Any unique subset of `traefik`, `servicelb`, `local-storage`, `metrics-server` | Optional bundled k3s components to omit; the libvirt profile disables only Traefik and ServiceLB |
| `container.runtime` | `containerd`, `enroot`, `apptainer` | OCI execution implementation |
| `container.integration` | `none`, `pyxis` | Slurm/container integration |
| `launcher.training` | `kubeflow`, `jobset`, `slurm` | Training submitter |
| `launcher.inference` | `kuberay`, `vllm-multiprocessing`, `slurm-ray` | Inference submitter |
| `slurm.partition` | Letters, digits, `.`, `_`, or `-` | Required target partition |
| `slurm.account`, `.qos`, `.reservation`, `.constraint` | Optional Slurm name | Site allocation or placement selection |
| `slurm.memory` | Integer plus `Mi`, `Gi`, or `Ti`, such as `12Gi` | Optional workload memory request per node; omit it to use the partition or site default. For libvirt it cannot exceed `nodes.memory - 512Mi` |
| `slurm.timeLimit` | `[days-]HH:MM:SS`, with valid clock fields | Batch time limit |
| `slurm.rendezvousPort` | Integer 1-65535 | PyTorch rendezvous port; default profiles use 29500 |
| `slurm.exclusive` | `true`, `false` | Request whole nodes for the job |
| `nodes.controlPlane` | Integer 0-9 | Control-plane nodes; existing clusters normally use 0 |
| `nodes.workers` | Integer 1-1024 | Worker nodes available to the request |
| `nodes.cpu` | Positive integer | vCPUs per simulated worker |
| `nodes.memory`, `nodes.disk` | Integer plus `Mi`, `Gi`, or `Ti`, such as `16Gi` | Per-worker capacity |
| `nodes.gpus.vendor` | `none`, `nvidia`, `amd` | Accelerator family |
| `nodes.gpus.perWorker` | Integer 0-64 | GPUs available per worker |
| `nodes.gpus.resourceName` | Scheduler resource name | For example `nvidia.com/gpu` |
| `network.dataInterface` | Linux interface name, at most 15 characters | Slurm bind interface or the Kubernetes node/CNI data interface |

### UIB and communication values

| Path | Accepted values or syntax | Meaning |
|---|---|---|
| `uib.enabled` | `true`, `false` | UIB run or native-verbs baseline |
| `uib.bundle` | Digest-qualified OCI image; local tags need explicit development opt-in | Runtime identity; Kubernetes pulls it and Slurm mounts the corresponding extracted tree |
| `uib.protocol` | `rocev2`, `uet` | UIB wire protocol |
| `uib.bindPort` | Integer 1-65535; 4791 for RoCEv2, 5432 for UET | Per-pod UIB service port selected by the protocol overlay |
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
| `communication.nccl.hca`, `.rccl.hca` | `=uib0` for UIB; an explicit site device for a native baseline | Exact device filter; UIB rendering rejects any value other than `=uib0` |
| `communication.nccl.environment`, `.rccl.environment` | Scalar map with `NCCL_` or `RCCL_` names | Advanced collective settings; transport-selection variables are adapter-owned |
| `communication.ucx.transports` | Non-empty list, normally `rc`, `ud` | Explicit UCX verbs transports |
| `communication.ucx.device` | Device and port, normally `uib0:1` | Exact UCX device |
| `communication.libfabric.provider` | `verbs`, `verbs;ofi_rxd` | Exact OFI provider chain; the local RDM gate uses `verbs;ofi_rxd` |

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
| `storage.modelCacheClaim` | Existing Kubernetes PVC name | Optional shared Hugging Face/model cache mounted at `/cache` |
| `storage.resultsClaim` | Existing Kubernetes PVC name | Optional shared checkpoint and result storage mounted at `/results` |
| `storage.uibRuntimePath` | Absolute path without spaces, commas, or colons | Slurm-visible extracted UIB runtime mounted read-only at `/opt/uib` |
| `storage.modelCachePath` | Absolute path without spaces, commas, or colons | Slurm-visible persistent cache mounted at `/cache` |
| `storage.resultsPath` | Absolute path without spaces, commas, or colons | Slurm-visible result root; it must exist before `sbatch` |
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
| Pyxis without Slurm and Enroot | Pyxis is the Enroot SPANK integration used by this adapter |
| Kubernetes with host storage paths | Kubernetes uses claims, cache images, or `emptyDir` volumes |
| `kubernetes.*` with Slurm | Kubernetes distribution and CIDR settings belong only to a Kubernetes manager |
| libvirt Kubernetes without `distribution: k3s` or an exact `vX.Y.Z+k3sN` version | The implemented multi-VM provisioner is pinned k3s; unsupported distributions never fall through silently |
| Overlapping Kubernetes pod, service, management, or data CIDRs | Nodes need four unambiguous address domains |
| Slurm with Kubernetes claims or cache images | Slurm uses shared host paths visible on every allocated node |
| Slurm without model-cache/result paths, or UIB without a runtime path/data interface | Pyxis cannot provide the common mount and address contract |
| `libvirt.networkMode: rootless` without `qemu:///session` | Rootless mode is deliberately scoped to a per-user libvirt daemon |
| `libvirt.sshPortBase + nodes.workers` above 65535 | The controller and every worker need a valid consecutive forwarding port |
| `virtme.sshPortBase + nodes.workers` above 65535 | Every persistent guest needs a valid, distinct SSH-forward port |
| Multi-guest virtme without `network.dataInterface` | Guest provisioning and UIB binding need an explicit data NIC |
| virtme `socket` with an interface other than `eth1`, or `bridge` with an interface other than `eth0` | TCP SSH creates a management NIC; the selected data backend has a fixed, validated device order |
| virtme `bridge` without permission for `ip link` and QEMU bridge attachment | The optional host bridge/tap path needs network-administration permission and QEMU bridge authorization |
| `libvirt.networkMode: bridge` without permission to create libvirt networks | Managed bridge/tap networking needs the normal system-libvirt network privileges |
| libvirt Kubernetes with `networkMode: rootless` | Per-guest passt management networks cannot form the required Kubernetes control plane; use bridge mode |
| Rootless mode for network-performance conclusions | Passt and multicast tunnels are functional transports, not an accurate model of physical fabric performance |
| RCCL without `nodes.gpus.vendor: amd` | RCCL requires AMD/ROCm hardware |
| NCCL with `nodes.gpus.vendor: amd` | Select RCCL for the AMD profile |
| `gpu-direct` while the compatibility lock says unvalidated | Prevents an L2 claim from an unsupported path |
| Congestion parameters with an algorithm other than `nscc` | Only NSCC consumes `maxWindowBytes`; registered algorithms currently receive no configuration map |
| Unknown NSCC parameters or a non-integer/out-of-range `maxWindowBytes` | Prevents a setting from being accepted but ignored |
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

Experimental NSCC comparison with RoCEv2:

```text
simulation/bin/uib-sim render --profile virtme-kubernetes \
  --overlay protocol/rocev2-nscc-experimental --overlay network/datacenter
```

NSCC was engineered for Ultra Ethernet. This overlay exists only to exercise
the protocol-neutral plug-in ABI and enable controlled cross-protocol research;
it is not a recommended RoCEv2 configuration. RoCEv2 defaults to `none`.

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

libfabric RxD/verbs with reordering:

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
| `libvirt-kubernetes` | OpenTofu/libvirt VMs | Kubernetes | containerd | Kubeflow/KubeRay | Planned |
| `libvirt-slurm` | OpenTofu/libvirt VMs | Slurm | Pyxis/Enroot | `sbatch`/`srun` | L0 verified; L1 requires physical multi-node traffic |
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

`make ucx-libfabric-test` is an actual two-process data-path gate. With UCX
1.18.1 it runs tag, stream, UCP put/get, fetch-add, and compare-swap tests
through `ud_verbs/uib0:1`. It separately pins the lower-level UCT API to
`rc_verbs/uib0:1` and runs active-message latency, RDMA write, RDMA read,
64-bit fetch-add, and 64-bit compare-swap.
With libfabric 2.1.0 it discovers `uib0-dgram`, layers the standard RxD utility
provider over verbs, and runs untagged ping-pong, tagged ping-pong, and tagged
bandwidth tests with data-integrity checking at 1, 64, and 1024 bytes. It also
runs counter-completed RMA write/read and 64-bit sum/compare-swap tests. The
test requires the expected provider names in the logs and rejects TCP,
sockets, shared-memory, and vendor-direct fallbacks.

The gate added the verbs semantics those front ends exercised: address
handles, UD send/receive and GRH handling, basic shared receive queues,
`sq_sig_all`, bounded inline sends, QP capability queries, ECE rejection, and
clean rejection of EFA-specific queries. This is local CPU compatibility
evidence. The UCP atomic tests use UCX protocol behavior over UD. Direct UCT
RC active messages, RMA, fetch-add, and compare-swap are also validated.
Direct libfabric DGRAM coverage, the blocking `fi_rdm_cntr_pingpong` variant,
and physical-node repetition remain separate promotion gates.

`make mpi-compat-test` runs an unchanged two-rank MPI program over both front
ends. The UCX case pins Open MPI's `pml/ucx` to `ud_verbs/uib0:1`; the OFI case
pins `pml/cm` plus `mtl/ofi` to `verbs;ofi_rxd`. Both exchange verified 1,
2051, and 65536-byte messages and run an all-reduce. Provider logs must prove
the requested UIB path and fail on a software fallback.

Both front ends inspect `/dev/infiniband/uib0` before opening the user-space
device. It must be a readable and writable character device. UIB maps
`/dev/null` there as a discovery anchor; data does not flow through that file.
The local wrapper, Kubernetes renderer, and Slurm/Pyxis renderer create the
appropriate mapping automatically. See the
[transparent-verbs guide](transparent-verbs.md#character-device-anchor-for-ucx-and-libfabric)
for direct-container and host commands.

The selectors used by the gate are:

```text
UCX_TLS=ud UCX_NET_DEVICES=uib0:1 \
LD_PRELOAD=/opt/uib/lib/libuibv.so ucx_perftest <peer>

UCX_RC_VERBS_FLUSH_MODE=fc \
LD_PRELOAD=/opt/uib/lib/libuibv.so \
ucx_perftest -x rc_verbs -d uib0:1 <peer>

FI_PROVIDER='verbs;ofi_rxd' FI_VERBS_DGRAM_USE_NAME_SERVER=0 \
LD_PRELOAD=/opt/uib/lib/libuibv.so \
fi_rdm_pingpong -p 'verbs;ofi_rxd' -d uib0-dgram -b <peer>
```

RxD is used because UIB currently exposes a verbs DGRAM endpoint and does not
provide the IPoIB plus `librdmacm` path required by the verbs MSG/RxM chain.
The exact UCX transport is taken from `ucx_info -d`; a result fails if it
selects `tcp`, `self`, `posix`, `sysv`, `cma`, or another non-verbs data path.

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
       | nscc (UET)       |
       | none | plug-in X |
       +------------------+
```

RoCEv2 and UET use this operations contract. UET defaults to `nscc`; RoCEv2
defaults to `none` to preserve its original behavior. `none` is an unsafe
comparison baseline for congestion experiments:

```text
UIB_PROTOCOL=uet UIB_UET_CC=nscc app
UIB_PROTOCOL=uet UIB_UET_CC=none app
UIB_PROTOCOL=uet UIB_UET_CC=my_algorithm app
# Experimental only: NSCC was not designed for RoCEv2.
UIB_PROTOCOL=rocev2 UIB_ROCEV2_CC=nscc unchanged-application
UIB_PROTOCOL=rocev2 UIB_ROCEV2_CC=my_algorithm app
```

The third form works after an experiment registers `my_algorithm` with
`uib_congestion_register()` before queue-pair creation. Its static operations
table supplies initialization, admission, ACK-request, send, ACK, NACK, and
inferred-loss callbacks. State must fit `UIB_CONGESTION_STATE_SIZE`, the table
must use `UIB_CONGESTION_ABI_VERSION`, and it must remain valid for the life of
the process. A preloadable experiment module may register from a constructor,
so the target application still requires no source change. The repository's
`tests/congestion_plugin.c`, `make uet-congestion-test`, and
`make rocev2-congestion-test` provide a minimal working example of that path.

```text
cc -shared -fPIC -I/path/to/uib/include my_algorithm.c \
   -L/path/to/uib -Wl,-rpath,/opt/uib/lib -luib -o my_algorithm.so

LD_PRELOAD=/opt/experiments/my_algorithm.so:/opt/uib/lib/libuibv.so \
UIB_PROTOCOL=rocev2 UIB_ROCEV2_CC=my_algorithm unchanged-application
```

The module and UIB must use the same ABI version. A run stores the module hash
with its result because the algorithm name alone is not reproducible.

For an in-tree experiment, add the operations table beside
`src/congestion.c`, register it during initialization, rebuild UIB, and select
its name. Do not put protocol wire parsing in the algorithm: the UET or RoCEv2
adapter translates its ACK/NACK/ECN format into the common event structures.
This separation allows the same algorithm to be compared across protocols
where their feedback semantics are equivalent.

The RoCEv2 adapter retains congestion-limited RC packets in PSN order and
drains them as acknowledgements reopen the window. It requests intermediate
ACKs when the selected algorithm needs them and translates cumulative ACKs,
read responses, retry loss, and received CNP packets into the common events.
UIB recognizes CNP on the wire; the current UDP receive path does not yet
synthesize CNP from an IP ECN mark, so physical ECN/CNP interoperation remains
a hardware promotion test. The shared ABI does not imply that every algorithm
is suitable for every protocol: NSCC remains a UET algorithm, and its RoCEv2
selector is experimental only.

The common request form is independent of UET-specific environment names:

```yaml
uib:
  protocol: rocev2
  congestionControl:
    # Experimental only: NSCC was designed for Ultra Ethernet.
    algorithm: nscc
    parameters:
      maxWindowBytes: 262144
```

The adapter renders the experimental example to `UIB_ROCEV2_CC=nscc` and
`UIB_ROCEV2_NSCC_MAX_WND=262144`; UET uses the corresponding `UIB_UET_*`
names. `maxWindowBytes` is valid only with `algorithm: nscc`. It must be an
integer from 1 through 4294967295, and the runtime rejects a value smaller than
the active path MTU plus wire overhead. Other algorithms require an empty
`parameters` map. Each result records the resolved algorithm, parameters,
protocol, workload, and network condition.

## Transparent UIB injection

The workload source remains unchanged. The adapter mounts a checksummed UIB
runtime bundle at `/opt/uib` and sets the required environment:

```text
NCCL_IBVERBS_LIB=/opt/uib/lib/libuibv.so
NCCL_NET=IB
NCCL_IB_DISABLE=0
NCCL_IB_HCA==uib0
NCCL_DEBUG=INFO
LD_PRELOAD=/opt/uib/lib/libuibv.so
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

UCX and libfabric additionally require `/dev/infiniband/uib0` to exist as a
readable and writable character device for their pre-open checks. The
Kubernetes adapter mounts a `/dev/null` `hostPath` at that path, and the Slurm
adapter adds `/dev/null:/dev/infiniband/uib0` to the Pyxis mounts. This anchor
is configuration metadata only; all verbs and data operations still go
through `libuibv.so` and UIB.

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
make runtime-bundle-portable
make runtime-bundle-portable-test
```

The native command builds against the development host. The portable command
builds the same tracked source in a digest-pinned Ubuntu 22.04 container and is
the artifact used by `make runtime-image`, Kubernetes, and other container
workloads. Both commands create these ignored local artifacts:

```text
simulation/.artifacts/
|-- uib-runtime/
|   |-- bin/
|   |   |-- ibv_devices
|   |   |-- ibv_devinfo
|   |   |-- ibv_rc_pingpong
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

The portable standard `ibv_rc_pingpong` binary is used unchanged by the Slurm
VM traffic gate. The abbreviated tree omits other bundled diagnostic tools,
dependency libraries, and their versioned symlinks. `runtime-manifest.json`
records the UIB commit,
tracked-source state, compiler, libc, minimum required glibc symbol version,
detected build capabilities, dependency source paths, sizes, and hashes. The
compatibility lock records validated NCCL/CUDA combinations and explicitly
labels unvalidated collectives and GPU-direct operation. On Debian-family
builders, the artifact also records package versions and includes available
copyright files for bundled runtime and static-library dependencies.

The portable builder pins and verifies libmnl 1.0.5 and liburing 2.9 source
archives because Ubuntu 22.04 does not provide the required static libmnl
archive or a new enough liburing API. Their revisions, archive hashes, source
URLs, and copied licenses appear under `sourceDependencies` in the manifest.
The current portable artifact is built with glibc 2.35 and requires no symbols
newer than GLIBC 2.34. It enables the transparent provider, UDP transport,
socket zero-copy, and static archive, but disables AF_XDP, TCP devmem,
DMA-BUF, and io_uring ZCRX. Use a separately qualified native artifact for
those host/kernel-specific features.

The bundle intentionally does not carry glibc or the dynamic loader. Injecting
a private glibc into an arbitrary application process is unsafe. Build on the
oldest supported userspace, and run preflight in the target image or node;
`ldd` rejects a target whose libc cannot satisfy the recorded libraries. The
portable build automates this boundary; do not use a native bundle in a
container merely because both were built on the same machine.

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
  env NCCL_IBVERBS_LIB=/opt/uib/lib/libuibv.so \
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
3. **MPI, UCX, and libfabric compatibility testing.** The validated local UCX,
   libfabric RxD/RDM, and two-rank Open MPI paths broaden CPU compatibility
   coverage. PGAS, connection-managed endpoints, and physical-node runs remain
   distinct promotion targets.
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
simulation/bin/uib-sim render --profile kind-kubernetes --format json \
    > /tmp/uib-kind.json
simulation/backends/kind/cluster render /tmp/uib-kind.json
simulation/bin/uib-sim up --profile kind-kubernetes --run-id uib-lab \
    --kubeconfig /tmp/uib-lab.kubeconfig
simulation/bin/uib-sim status --profile kind-kubernetes --run-id uib-lab
simulation/bin/uib-sim load-image --profile kind-kubernetes \
    --run-id uib-lab \
    --image registry.example/uib-training@sha256:<digest>
simulation/bin/uib-sim down --profile kind-kubernetes --run-id uib-lab
```

The adapter renders ordinary Kind v1alpha4 YAML with one control-plane and the
configured worker count. Every node uses the digest-pinned image from
`cluster.nodeImage`. The default profile pins Kind v0.31.0 and Kubernetes
v1.35.0; use a matching Kind executable through `KIND_COMMAND` when the host
has another version. `up`, `status`, `kubeconfig`, and `down` invoke the real
Kind CLI and always require an explicit, validated cluster name. The rendered
YAML can be passed directly to `kind create cluster` when debugging. The
top-level `uib-sim` lifecycle commands currently delegate only the Kind
profile; other providers fail explicitly as unimplemented.

`load-image` delegates to `kind load docker-image` and requires digest-pinned
references by default. Local development tags require the explicit
`--allow-mutable-tags` option. KubeRay installation and pod network-namespace
shaping are later deployment phases; the current Kind adapter does not claim
those steps. The Kubernetes UIB gate preserves ignored evidence, but generic
lifecycle commands do not yet collect arbitrary workload results.

Kind nodes share one host kernel and compute capacity. Results are L0 unless a
separate GPU capability has been proven.

### Kubeflow training renderer

`render-workload` turns the same resolved configuration used by every backend
into ordinary Kubeflow Trainer v2 resources. The checked-in request is a
generated, four-step CPU DDP workload and therefore needs no model or dataset
download:

```text
simulation/bin/uib-sim render-workload --profile kind-kubernetes \
    --request simulation/requests/training-smoke.yaml \
    --run-id uib-smoke --allow-mutable-tags > /tmp/uib-smoke.yaml
```

The output contains a run-owned namespace, workload ConfigMap,
`TrainingRuntime`, and `TrainJob`. The adapter maps Kubeflow's standard rank
variables to the unchanged workload entry point.

The runtime uses one pod per requested node and required pod anti-affinity on
`kubernetes.io/hostname`. When UIB is enabled, its first init container copies
the runtime image from `/uib-runtime` into a shared `/opt/uib` `emptyDir`; a
second init container executes `uib-preflight --library-only` inside the
training image. The trainer receives its pod IP through the downward API as
`UIB_BIND_IP`, the protocol overlay's `UIB_BIND_PORT`, and the explicit
`NCCL_NET=IB`, `NCCL_IB_HCA==uib0`, and `NCCL_IBVERBS_LIB` selection. NCCL
logging is enabled so evidence can be preserved for the planned Kubernetes
collector to reject socket or native-provider fallback.

Local image tags are accepted only with `--allow-mutable-tags`. The renderer
otherwise requires immutable digests for both images:

```text
simulation/bin/uib-sim render-workload --profile kind-kubernetes \
    --request simulation/requests/training-smoke.yaml \
    --run-id uib-smoke \
    --set workload.image=registry.example/training@sha256:<digest> \
    --set uib.bundle=registry.example/uib-runtime@sha256:<digest>
```

Images use the normal node cache. The generated smoke model needs no external
artifacts; public-model and durable-result runs name existing site claims:

```text
simulation/bin/uib-sim render-workload --profile real-kubernetes \
    --request simulation/requests/training-smoke.yaml \
    --run-id uib-training \
    --set storage.modelCacheClaim=shared-model-cache \
    --set storage.resultsClaim=uib-results \
    --set workload.image=registry.example/training@sha256:<digest> \
    --set uib.bundle=registry.example/uib-runtime@sha256:<digest>
```

Multi-node jobs need site-provided shared claims, normally `ReadWriteMany`.
The adapter does not create or delete them, and each run writes below its ID.

The native-verbs comparison keeps the image and workload identical and only
turns off UIB injection while selecting an explicit physical HCA:

```text
simulation/bin/uib-sim render-workload --profile real-kubernetes \
    --request simulation/requests/training-smoke.yaml \
    --run-id native-smoke \
    --set uib.enabled=false \
    --set communication.nccl.hca==mlx5_0 \
    --set workload.image=registry.example/training@sha256:<digest>
```

Change the UIB protocol without changing the scheduler or workload:

```text
simulation/bin/uib-sim render-workload --profile kind-kubernetes \
    --request simulation/requests/training-smoke.yaml \
    --overlay protocol/uet-nscc --run-id uib-uet \
    --allow-mutable-tags
```

The protocol overlay changes the port to 5432 and adds `UIB_UET_CC=nscc`.
`uib.environment` and the collective environment accept advanced variables,
but the renderer owns bind, protocol, verbs-library, HCA, and network-selection
variables and rejects attempts to override them through those maps.

Current compatibility boundaries are intentional:

| Request | Result |
|---|---|
| `launcher.training: kubeflow`, `workload.kind: training` | Supported render path |
| `launcher.inference: kuberay`, `workload.kind: inference` | Supported RayJob render path; the one-task CPU lifecycle is validated |
| NCCL with `transportAllowlist: [IB]` | Supported communication shape |
| UIB enabled without `uib.bundle` or with HCA other than `=uib0` | Rejected |
| Mutable workload or runtime images without explicit opt-in | Rejected |
| More than one process per pod | Rejected until UIB derives a unique bind port for each local rank |
| RCCL, UCX, or libfabric training front end | Rejected until a corresponding workload image and launcher pass their compatibility gate |
| CPU smoke request | L0 scheduler/rendering coverage; PyTorch uses Gloo and does not exercise UIB traffic |

Kubeflow Trainer v2.2.1 and API `trainer.kubeflow.org/v1alpha1` are pinned in
the compatibility lock.

### Real Kind UIB data-plane gate

The shortest cluster test that actually sends traffic through UIB is:

```text
make simulation-kubernetes-uib-test
```

It builds the current UIB runtime bundle and diagnostic image, creates one
control-plane and two Kind workers, and starts a server and client pod on
different workers. The image supplies the distribution's unmodified
`ibv_rc_pingpong` and perftest `ib_write_bw`; the pod injects `libuibv.so` with
`LD_PRELOAD` and selects `-d uib0`. No source patch, wrapper binary, synthetic
verbs implementation, or application rebuild is involved.

The gate tests RC SEND/RECV and RDMA Write over RoCEv2 and UET between distinct
Pod IPs and workers, requires payload evidence, and cleans up the namespace and
cluster on success or failure.

The normal path has no required configuration. The one common override is:

```text
SIMULATION_KUBERNETES_UIB_PROTOCOLS=uet \
make simulation-kubernetes-uib-test
```

Supported options are deliberately small:

| Variable | Default | Syntax and meaning |
|---|---|---|
| `SIMULATION_KUBERNETES_UIB_PROTOCOLS` | `rocev2 uet` | Space-separated subset of `rocev2` and `uet`; any other or empty value is rejected. The port is derived and cannot conflict with the protocol. |
| `SIMULATION_KUBERNETES_UIB_ITERATIONS` | `64` | Positive exchange count passed to both unchanged verbs programs. |
| `SIMULATION_KUBERNETES_UIB_NAME` | Unique `uib-data-<pid>` | Disposable Kind cluster name. |
| `SIMULATION_KUBERNETES_UIB_NAMESPACE` | Unique `uib-data-<pid>` | Disposable test namespace. |
| `SIMULATION_KUBERNETES_UIB_WAIT` | `180s` | Kind readiness duration. |
| `SIMULATION_KUBERNETES_UIB_TIMEOUT` | `180s` | Per-pod and cleanup duration. |
| `SIMULATION_KUBERNETES_UIB_RESULTS` | `simulation/.artifacts/results` | Ignored persistent result root. Each unique cluster name gets one subdirectory; an existing directory is never overwritten. |
| `RUNTIME_IMAGE` | `uib-runtime:test` | Local runtime init image built and loaded into Kind. |
| `DIAGNOSTIC_IMAGE` | `uib-diagnostics:test` | Local standard-tool image built and loaded into Kind. |
| `KIND_COMMAND`, `KUBECTL_COMMAND` | `kind`, `kubectl` | Exact client executables. |

For a longer single-protocol run:

```text
SIMULATION_KUBERNETES_UIB_PROTOCOLS=rocev2 \
SIMULATION_KUBERNETES_UIB_ITERATIONS=4096 \
make simulation-kubernetes-uib-test
```

For locally named images:

```text
RUNTIME_IMAGE=my-uib-runtime:test \
DIAGNOSTIC_IMAGE=my-uib-diagnostics:test \
make simulation-kubernetes-uib-test
```

This is genuine UIB data-plane validation and is the required local gate
before treating a scheduler smoke test as useful. It remains L0: both Kind
workers share one physical host and kernel, the program uses host memory, and
no GPU collective runs. It does not replace the two-physical-node NCCL or RCCL
promotion gate required for L1.

Every run prints its ignored evidence directory, containing the request,
summary, versions, runtime manifest, topology, preflight, and verbs logs.

### Real Kind and Kubeflow L0 gate

The opt-in gate installs that exact operator and executes the generated
two-node request rather than only inspecting YAML:

```text
make simulation-kubeflow-e2e-test
```

It installs the pinned operator in a disposable two-worker Kind cluster and
runs the generated TrainJob. Passing requires distinct worker placement, UIB
preflight in both pods, CPU/Gloo world size two, four steps, and cleanup.

The validated local tool set is Kind v0.31.0, kubectl v1.35.0, Helm v3.21.3,
Kubernetes v1.35.0, and Kubeflow Trainer v2.2.1. Required host resources are a
working Kind container engine, enough disk for the approximately 9 GB training
image in each node, and enough CPU and memory for two training pods. The first
run needs network access for the pinned node image, operator chart, and
operator images. Those remain in the normal Docker/containerd and Helm caches;
the training/runtime images are reused from the host cache. The generated
workload downloads no model or dataset.

| Variable | Default | Syntax and meaning |
|---|---|---|
| `SIMULATION_KUBEFLOW_E2E_NAME` | Unique `uib-kf-<pid>` | Disposable Kind cluster name |
| `SIMULATION_KUBEFLOW_E2E_RUN_ID` | Unique `uib-smoke-<pid>` | Run namespace and TrainJob name |
| `SIMULATION_KUBEFLOW_TRAINER_VERSION` | `2.2.1` | OCI chart version; another value is unvalidated and changes the compatibility claim |
| `SIMULATION_KUBEFLOW_OPERATOR_TIMEOUT` | `10m` | Helm install/readiness timeout |
| `SIMULATION_KUBEFLOW_WORKLOAD_TIMEOUT` | `10m` | TrainJob and namespace-deletion timeout |
| `SIMULATION_KUBEFLOW_TRAINING_IMAGE` | `uib-training:test` | Existing host image loaded into Kind |
| `SIMULATION_KUBEFLOW_RUNTIME_IMAGE` | `uib-runtime:test` | Existing host UIB runtime image loaded into Kind |
| `KIND_COMMAND` | `kind` | Exact Kind executable |
| `KUBECTL_COMMAND` | `kubectl` | Exact kubectl executable |
| `HELM_COMMAND` | `helm` | Exact Helm executable |

Changing either local image is useful for development but does not create an
immutable promotion result. Published-cluster runs use image digests through
the renderer and pull from a registry instead of this local Kind staging path.
This gate needs two Kubernetes workers but not two physical machines or GPUs:
all nodes share one host and kernel. PyTorch deliberately uses Gloo on CPU, so
the passing UIB preflight proves library deployment only. It does not open
`uib0`, send UIB traffic, validate NCCL, or qualify as L1.

### KubeRay inference renderer

The inference request uses the same configuration and image/storage policy as
training:

```text
simulation/bin/uib-sim render-workload --profile kind-kubernetes \
    --request simulation/requests/inference-smoke.yaml \
    --run-id inference-smoke --allow-mutable-tags \
    > /tmp/inference-smoke.yaml
```

`workload.kind: inference` selects a `ray.io/v1` `RayJob`; there is no second
KubeRay configuration format. It mounts the configured cache, results, and UIB
runtime and uses the common communication settings.

The checked-in CPU request has one Ray head and no worker, so it is not
distributed inference. A two-node shape changes only common fields:

```text
simulation/bin/uib-sim render-workload --profile kind-kubernetes \
    --request /path/to/vllm-ray.yaml \
    --run-id inference-ray --allow-mutable-tags \
    --set workload.parallelism.nodes=2 \
    --set workload.parallelism.data=2
```

The external request retains the checked-in shape but selects `backend: vllm`,
`server.distributedExecutor: ray`, matching TP/PP/DP degrees, and NVIDIA GPU
resources. The rendering then adds one fixed-size worker group and required
cross-node pod anti-affinity. The renderer rejects an idle multi-node CPU
shape, mismatched parallel degrees, GPU-less vLLM, and more than one process
per pod because those ranks would share a UIB bind port.

KubeRay v1.6.2 and `ray.io/v1` are pinned in the compatibility lock. Run the
real local gate with:

```text
make simulation-kuberay-e2e-test
```

The target first runs the actual two-worker UIB verbs gate, builds the portable
runtime and inference images, downloads the pinned SmolLM2 revision once into
the ignored cache, creates a disposable one-worker Kind cluster, installs the
operator, and submits the standard inference request. It requires a
Docker-compatible engine, Kind, kubectl, Helm, network access for uncached
images/charts, about 10 GB of image storage, and four CPU cores plus 8 GiB for
the Ray head. A successful run checks Ray readiness, all UIB init preflights,
four requests, 16 output tokens, the Transformers backend, and complete
namespace/operator/cluster cleanup.

`storage.modelCacheImage` stages the ignored host cache through a disposable
local image and forces Hugging Face offline mode in the Ray Pod. It cannot be
combined with `storage.modelCacheClaim`. The local image is convenient for
Kind only; on a real cluster, omit it and set `storage.modelCacheClaim` and
`storage.resultsClaim` to existing PVCs. This preserves dynamically downloaded
model weights between runs and keeps them out of this repository.

Evidence is retained under `simulation/.artifacts/results/<cluster-name>` on
both success and failure. It includes the rendered workload, RayJob, Pod
states/descriptions, init and submitter logs, and operator logs. Model and
result storage are `emptyDir` volumes in the local CPU gate, so the JSON result
is validated from the submitter log before cleanup.

This is an L0 scheduler/runtime validation. The one-head Transformers workload
sets and reports the UIB/NCCL environment, but it has no communicating second
rank and therefore sends no UIB data-plane traffic. Actual UIB traffic is
validated by the required two-worker verbs gate. Multi-node inference over UIB
still requires GPU workers and an NCCL/vLLM validation run.

KubeRay gate options:

| Variable | Default | Syntax and meaning |
|---|---|---|
| `SIMULATION_KUBERAY_E2E_NAME` | `uib-ray-<pid>` | Owned Kind cluster and evidence-directory name; use a unique lowercase DNS label |
| `SIMULATION_KUBERAY_E2E_RUN_ID` | `uib-infer-<pid>` | Namespace and RayJob name; use a unique lowercase DNS label |
| `SIMULATION_KUBERAY_OPERATOR_VERSION` | `1.6.2` | Helm chart version; changing it produces an unqualified compatibility run |
| `SIMULATION_KUBERAY_OPERATOR_TIMEOUT` | `10m` | Helm duration for operator installation |
| `SIMULATION_KUBERAY_PROVISIONING_TIMEOUT` | `1800` | Positive seconds allowed after RayJob creation for image unpacking and RayCluster readiness |
| `SIMULATION_KUBERAY_WORKLOAD_TIMEOUT` | `900` | Positive seconds allowed after the deployment enters `Running` |
| `SIMULATION_KUBERAY_INFERENCE_IMAGE` | `uib-inference:test` | Existing host image loaded into Kind |
| `SIMULATION_KUBERAY_RUNTIME_IMAGE` | `uib-runtime:test` | Portable UIB init image loaded into Kind |
| `SIMULATION_KUBERAY_MODEL_CACHE_IMAGE` | `uib-model-cache:test` | Disposable image built from the ignored Hugging Face cache |
| `SIMULATION_KUBERAY_RESULTS` | `simulation/.artifacts/results` | Ignored persistent root for success and failure evidence |
| `KIND_COMMAND`, `KUBECTL_COMMAND`, `HELM_COMMAND` | Standard command names | Exact client executables used by the gate |

Timeouts are separate intentionally: large local images may take minutes to
unpack before the Ray head exists, while a running inference workload should
have its own shorter correctness deadline. Reusing an evidence name is
rejected so an earlier run is never overwritten.

Examples:

```text
# Give a slow local container store more time without weakening job execution.
SIMULATION_KUBERAY_PROVISIONING_TIMEOUT=2400 \
    make simulation-kuberay-e2e-test

# Keep evidence in another ignored or external directory.
SIMULATION_KUBERAY_RESULTS=/var/tmp/uib-results \
    make simulation-kuberay-e2e-test

# Real-cluster storage: use PVCs, never the local cache image at the same time.
simulation/bin/uib-sim render-workload --profile real-kubernetes \
    --request simulation/requests/inference-smoke.yaml \
    --run-id inference-real \
    --set storage.modelCacheClaim=llm-model-cache \
    --set storage.resultsClaim=llm-results
```

### virtme with Kind

`virtme-kind` is the one-guest composition for a selected Linux kernel with
Kind inside that guest. The existing `make virtme-test` gate proves one-shot
guest execution; the persistent Kind lifecycle adapter remains pending. This
mode is intended for io_uring, DMA-BUF, ZCRX, namespace, and kernel-version
compatibility tests.

Example overlay:

```yaml
infrastructure:
  provider: virtme
  kernel: /boot/vmlinuz-test
virtme:
  mode: kind
  networkMode: socket
  sshPortBase: 23000
  extraKernelArguments: [panic=1]
cluster:
  manager: kubernetes
nodes:
  cpu: 8
  memory: 16Gi
```

### Multiple virtme Kubernetes nodes

`virtme-kubernetes` creates one guest for the control plane and one for each
configured worker. The default `socket` network is a rootless QEMU Ethernet
segment shared by those guests. It was validated with two simultaneously
running guests and actual ICMP traffic between their distinct data addresses.
The backend also creates a run-owned SSH key, forwards a separate loopback SSH
port to every guest, records PID start times, and writes an Ansible inventory.
Kernel, CPU, memory, addresses, and extra kernel arguments remain configuration
values rather than script edits.

The exact `cluster.version` k3s binary is downloaded once, verified against the
release checksum, and retained in the ignored virtme download cache. Because
virtme uses `virtme-init` rather than systemd as PID 1, the backend starts the
k3s server and agents directly instead of pretending the libvirt systemd role
is reusable. It waits for every node to become Ready and verifies the worker
data addresses before reporting success.

```text
simulation/bin/uib-sim render --profile virtme-kubernetes --format json \
  > /tmp/uib-virtme.json
simulation/backends/virtme/cluster render /tmp/uib-virtme.json \
  --name uib-virtme-lab > /tmp/uib-virtme-topology.json

simulation/bin/uib-sim up --profile virtme-kubernetes \
  --run-id uib-virtme-lab
simulation/bin/uib-sim status --profile virtme-kubernetes \
  --run-id uib-virtme-lab
simulation/bin/uib-sim kubeconfig --profile virtme-kubernetes \
  --run-id uib-virtme-lab
simulation/bin/uib-sim down --profile virtme-kubernetes \
  --run-id uib-virtme-lab
```

The controller API is exposed through a run-owned SSH forward on
`virtme.sshPortBase + nodes.workers + 1`; it does not add another guest NIC or
another configuration value. `down` signals only guest and forward PIDs whose
recorded Linux start time still matches, preventing PID-reuse cleanup errors.

Use the optional bridge layer when host tap visibility or attachment to another
host network matters:

```text
simulation/bin/uib-sim render --profile virtme-kubernetes \
  --overlay virtme-bridge
simulation/bin/uib-sim up --profile virtme-kubernetes \
  --overlay virtme-bridge --run-id uib-virtme-bridge
```

`virtme-bridge` changes `virtme.networkMode` to `bridge` and the data device to
`eth0`. The backend creates one deterministically named Linux bridge, attaches
each QEMU data NIC through virtme-ng, and deletes only that recorded bridge on
failure or `down`. This mode requires permission to run the emitted `ip link`
commands and a QEMU bridge-helper policy that permits the bridge. The rootless
default uses `eth1`, creates no host taps or bridge, and is the easier choice
for functional UIB and guest-side netem tests. Neither mode represents physical
fabric performance.

The installed virtme-ng 1.35 passes the one-shot tests, persistent TCP SSH with
a run-owned key, simultaneous two-guest socket networking, pinned k3s node
join, host kubeconfig access, and complete teardown. The lifecycle does not
fall back to Kind or libvirt.

### OpenTofu and libvirt Kubernetes

This is the realistic local Kubernetes lab. It uses the same OpenTofu VM and
network module as the Slurm profile, then selects the Kubernetes Ansible
playbook:

1. OpenTofu creates management and data networks, volumes, cloud-init data,
   one control-plane VM, and worker VMs.
2. Ansible installs the exact `cluster.version` k3s binary after checking its
   release checksum, starts the server, and joins the workers. The default is
   [k3s v1.35.5+k3s1](https://github.com/k3s-io/k3s/releases/tag/v1.35.5%2Bk3s1).
3. k3s supplies Kubernetes and containerd. Flannel uses `eth1`, so worker
   netem conditions affect the pod overlay rather than an unused interface.
4. The backend waits for every node, verifies worker data addresses, records
   qdisc/kernel/tool evidence, and fetches a host-usable kubeconfig.
5. The normal Helm/Kubeflow/KubeRay layer deploys the same rendered workload
   used by Kind and real clusters.
6. `tofu destroy` removes only resources in the run's OpenTofu state.

The default profile is intentionally small:

```yaml
cluster:
  manager: kubernetes
  version: v1.35.5+k3s1
kubernetes:
  distribution: k3s
  podCIDR: 10.42.0.0/16
  serviceCIDR: 10.43.0.0/16
  disable: [traefik, servicelb]
network:
  dataInterface: eth1
```

Change node sizing, worker count, Kubernetes CIDRs, disabled bundled
components, protocol, and network condition through `--set` or overlays. The
four network CIDRs must not overlap. Only k3s is accepted by this adapter; an
unsupported distribution is rejected instead of being treated as k3s.

```text
simulation/bin/uib-sim up --profile libvirt-kubernetes \
  --run-id uib-k8s-lab --overlay network/datacenter
simulation/bin/uib-sim status --profile libvirt-kubernetes \
  --run-id uib-k8s-lab --overlay network/datacenter
simulation/bin/uib-sim kubeconfig --profile libvirt-kubernetes \
  --run-id uib-k8s-lab --overlay network/datacenter \
  > /tmp/uib-k8s-lab.kubeconfig
KUBECONFIG=/tmp/uib-k8s-lab.kubeconfig kubectl get nodes
simulation/bin/uib-sim down --profile libvirt-kubernetes \
  --run-id uib-k8s-lab --overlay network/datacenter
```

This profile requires `libvirt.networkMode: bridge`; rootless passt management
networks isolate the guests from the controller endpoint. Its real VM
lifecycle therefore needs a host allowed to use the configured system libvirt
URI.
The repository test covers configuration, playbook selection, inventory,
secret handling, evidence, kubeconfig rewriting, status, and exact cleanup
with mock standard-tool boundaries; it is not a substitute for that VM gate.

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

`render-workload` dispatches on `cluster.manager`; the same command that emits
Kubernetes objects emits an executable batch file for a Slurm profile:

```text
simulation/bin/uib-sim render-workload --profile libvirt-slurm \
    --request simulation/requests/training-smoke.yaml \
    --run-id uib-slurm-smoke --allow-mutable-tags \
    > uib-slurm-smoke.sbatch
sbatch uib-slurm-smoke.sbatch
```

The local tag opt-in is only for development. A reproducible or real-cluster
run sets digest-qualified `workload.image` and `uib.bundle` references and
omits `--allow-mutable-tags`.

The generated file uses Slurm resource directives and Pyxis's documented
`srun` switches. Its relevant shape is:

```sh
#!/bin/sh
#SBATCH --nodes=4
#SBATCH --ntasks=32
#SBATCH --ntasks-per-node=8
#SBATCH --gpus-per-node=8
#SBATCH --gpus-per-task=1
#SBATCH --cpus-per-task=16
#SBATCH --mem=1T
#SBATCH --time=01:00:00

set -eu

export NCCL_IBVERBS_LIB=/opt/uib/lib/libuibv.so
export NCCL_NET=IB
export NCCL_IB_DISABLE=0
export NCCL_IB_HCA='=uib0'
export LD_LIBRARY_PATH=/opt/uib/lib
export UIB_PROTOCOL=rocev2
export UIB_TRANSPORT_MODE=udp-io-uring

srun \
    --container-image=registry.example/uib-llm@sha256:<digest> \
    --container-mounts=/shared/uib:/opt/uib:ro,/models:/cache,/results/run:/results \
    --container-workdir=/results \
    --no-container-mount-home \
    --no-container-entrypoint \
    /bin/sh -c '<rank setup and preflight>; exec "$@"' \
    uib-slurm /opt/uib-training/bin/train \
    --config /etc/uib-workload/workload.json
```

The implementation follows the [official Pyxis command-line contract](https://github.com/NVIDIA/pyxis#usage)
and standard [Slurm batch options](https://slurm.schedmd.com/sbatch.html).
It does not implement a private container runtime or scheduler protocol.
The disposable VM contract uses the exact Ubuntu 24.04 Slurm package
`23.11.4-1.2ubuntu5`, Pyxis 0.24.0, and Enroot 4.2.1. That combination passed
an actual controller/worker lifecycle in the rootless libvirt lab described
below. Existing clusters may use another compatible version, but must record
it in their site evidence.

Before the application starts, every rank:

1. maps `SLURM_PROCID`, `SLURM_NTASKS`, and `SLURM_LOCALID` to `RANK`,
   `WORLD_SIZE`, and `LOCAL_RANK`;
2. derives `UIB_BIND_IP` from `network.dataInterface` inside its container;
3. derives a collision-free port as `uib.bindPort + LOCAL_RANK`;
4. runs `/opt/uib/bin/uib-preflight --library-only`; and
5. executes the request's unchanged entry point and arguments.

The renderer rejects a port range above 65535 and requires `nodes.cpu` to be
divisible by `processesPerNode`, so its `--cpus-per-task` value is exact.
Slurm provides normal GPU visibility from the allocation; when
`nodes.gpus.perWorker` is nonzero, the adapter emits both the per-node
allocation and a derived `--gpus-per-task` binding. GPU count, like CPU count,
must divide evenly across `processesPerNode`.

The following paths must already exist and be visible under the same absolute
name on every compute node:

| Setting | Container path | Access and purpose |
|---|---|---|
| `storage.uibRuntimePath` | `/opt/uib` | Read-only extracted portable runtime corresponding to `uib.bundle` |
| `storage.modelCachePath` | `/cache` | Read-write persistent model/dataset cache; downloads survive later runs |
| `storage.resultsPath/<run-id>` | `/results` | New run-owned directory for checkpoints, results, and scheduler evidence |

The result root itself must exist before submission because Slurm opens the
`%x-%j.out` and `%x-%j.err` files before the script body runs. The adapter
refuses to reuse an existing `<run-id>` directory. It embeds the resolved
simulation and unchanged workload JSON in the batch file, then writes both,
the allocated node list, `scontrol show job`, and a final status JSON to the
run directory.

Stage the runtime once from the portable artifact, not by installing it over
the host's libibverbs:

```text
make runtime-bundle-portable
mkdir -p /shared/uib/runtime /shared/uib/models /shared/uib/results
tar -xzf simulation/.artifacts/uib-runtime.tar.gz \
    -C /shared/uib/runtime --strip-components=1
```

The exact portable archive name is printed by its build target. On a shared
cluster an administrator normally owns the runtime directory while users own
their cache and result roots.

### Staging an image in a disposable Slurm lab

Pyxis accepts either an OCI reference or an absolute SquashFS path. Disposable
libvirt VMs and virtme guests cannot use a host-local development tag directly,
so `load-image` converts an image once and returns the path visible on every
lab worker:

```text
run_id=uib-slurm-training

simulation/bin/uib-sim up --profile libvirt-slurm \
    --run-id "$run_id"

training_image=$(simulation/bin/uib-sim load-image \
    --profile libvirt-slurm --run-id "$run_id" \
    --image uib-training:test --allow-mutable-tags)

simulation/bin/uib-sim run --profile libvirt-slurm \
    --request simulation/requests/training-smoke.yaml \
    --run-id "$run_id" --allow-mutable-tags \
    --set "workload.image=$training_image"
```

Use the same commands with `--profile virtme-slurm` for the virtme lab.

`up` must complete before `load-image`. The selected Docker-compatible engine
is inspected first and pulls the requested reference only when it is absent.
References are digest-qualified by default; `--allow-mutable-tags` is required
for a development tag and cannot be used for a reproducibility or promotion
claim. `--image` is repeatable and prints one staged path per input in the same
order. The returned absolute path is content-addressed and is the only
SquashFS path shape accepted by the Slurm renderer; arbitrary host or guest
paths are rejected.

On first use, the adapter exports the image without running it, feeds the tar
stream directly to `mksquashfs` so ownership metadata is retained, verifies
and caches the image by ID, and records its provenance.

A fresh lab hashes the complete host artifact before streaming it to the
controller, where the staging helper validates the SquashFS again. The ignored
cache defaults to `simulation/.artifacts/slurm-images`; `inventory.json`
records every retained artifact without embedding a machine-specific absolute
path. `SLURM_IMAGE_CACHE` selects another host cache directory, and
`CONTAINER_ENGINE_COMMAND` selects one exact Docker-compatible executable.
Neither value accepts a shell pipeline or embedded arguments. An incomplete,
symbolic-link, malformed-checksum, or checksum-mismatched cache entry is a
hard failure rather than an implicit replacement.

Inference additionally needs the dynamically downloaded model cache. Register
it after `up` and before `run`; the default source is the same ignored cache
used by the container smoke tests:

```text
simulation/bin/uib-sim load-cache --profile libvirt-slurm \
    --run-id "$run_id"

# Or select an existing cache explicitly.
simulation/bin/uib-sim load-cache --profile libvirt-slurm \
    --run-id "$run_id" --source /srv/huggingface-cache

# The command is identical for a virtme lab.
simulation/bin/uib-sim load-cache --profile virtme-slurm \
    --run-id "$run_id"
```

`load-cache` currently has one intentionally fixed contract: a Hugging Face
cache root is transferred to `/var/cache/uib/models/huggingface`, which the
workload sees as `HF_HOME=/cache/huggingface`. The source must be a nonempty
directory containing only directories, regular files, and symbolic links that
resolve inside that root. This preserves the normal Hugging Face blob/snapshot
layout while rejecting sockets, devices, broken links, and escaping links.

Libvirt transfers a temporary archive, verifies it before atomic installation,
and retains the original ignored cache. A different existing guest cache is a
hard failure. `TAR_COMMAND` selects the local `tar` executable. Virtme instead
maps `VIRTME_MODEL_CACHE` directly into every guest, so the files are neither
copied nor downloaded again. Its `--source` must identify that same directory;
select a different cache by setting `VIRTME_MODEL_CACHE` before `up`.
`SLURM_CACHE_INVENTORY` selects the ignored inventory directory, which defaults
to `simulation/.artifacts/slurm-caches`. Its `inventory.json` and the run-owned
`staged-caches.json` contain only the cache kind, source directory name,
archive size, digest, guest path, and timestamps—not model contents or an
absolute host path. The downloaded source remains in its normal ignored cache
for later runs; no model or dataset is committed.

The collected result bundle includes `staged-images.json` and points to it
from `images.json`; a staged model cache similarly adds `staged-caches.json`.
The workload image filesystem is unchanged; Pyxis is
already invoked with `--no-container-entrypoint`, and the request supplies the
same explicit entry point and arguments used on a real cluster. OCI runtime
configuration is therefore intentionally not part of the local SquashFS
contract.

On real Slurm hardware, push the same image to the site registry, replace only
the infrastructure-specific path, and omit the mutable-tag opt-in. Populate
the configured site model cache with the same pinned Hugging Face revision.
`load-cache` records or transfers cache provenance for disposable libvirt and
virtme labs; an existing site cache needs no staging command:

```text
simulation/bin/uib-sim run --profile real-slurm \
    --request simulation/requests/training-smoke.yaml \
    --run-id uib-physical-training \
    --set workload.image=registry.example/training@sha256:<digest> \
    --set uib.bundle=registry.example/uib-runtime@sha256:<digest>
```

The request, training code, generated or downloaded data selection, rank
layout, result schema, and application command do not change. Only the image
delivery form and site paths change. A local CPU run remains L0 because its
DDP backend is Gloo. Promotion to L1 requires the same request changed to CUDA
on at least two physical GPU nodes, NCCL transport evidence through UIB, and a
matching native-verbs baseline.

Multiple local ranks are a configuration change, not a different request
format:

```text
simulation/bin/uib-sim render-workload --profile real-slurm \
    --request training.yaml --run-id uib-eight-ranks \
    --set workload.parallelism.processesPerNode=8 \
    --set workload.parallelism.data=16 \
    --set nodes.cpu=128 \
    --set nodes.gpus.perWorker=8 \
    --set workload.image=registry.example/training@sha256:<digest> \
    --set uib.bundle=registry.example/uib-runtime@sha256:<digest> \
    > uib-eight-ranks.sbatch
```

Here a two-node profile resolves to 16 ranks and ports `4791` through `4798`
on each node. The parallel degrees must still multiply to 16.

The UET/NSCC and native-verbs comparisons change only communication layers:

```text
# UET with NSCC
simulation/bin/uib-sim render-workload --profile real-slurm \
    --request training.yaml --overlay protocol/uet-nscc \
    --run-id uib-uet > uib-uet.sbatch

# Same image and workload through a named physical HCA
simulation/bin/uib-sim render-workload --profile real-slurm \
    --request training.yaml --run-id native-verbs \
    --set uib.enabled=false --set communication.nccl.hca==mlx5_0 \
    > native-verbs.sbatch
```

The native file has no `/opt/uib` mount, UIB preflight, `NCCL_IBVERBS_LIB`, or
`UIB_*` variables. Results from it are comparable only when image, workload,
rank topology, seeds, and all non-transport settings match.

The implemented inference renderer runs the one-task CPU or single-GPU
serve/probe lifecycle under one Pyxis job step and uses the same persistent
cache. Multi-node Slurm-Ray startup is deliberately rejected for now; it will
be promoted only with real Ray head/worker lifecycle and UIB traffic tests.

### Run-owned submit, status, and cancel

`run` performs the safe standard lifecycle around the same rendered batch
file. It validates the shared directories on the submission node, retains the
resolved configuration and renderer diagnostics, calls `sbatch --parsable`,
and records the returned numeric job ID before reporting success:

```text
simulation/bin/uib-sim run --profile real-slurm \
    --request training.yaml --run-id uib-training
```

The command prints only the job ID on success. Its default run ledger is
`simulation/.artifacts/runs/<run-id>/`, which is ignored by Git. Use an
explicit private location when invoking the CLI from outside the repository:

```text
simulation/bin/uib-sim run --profile real-slurm \
    --request training.yaml --run-id uib-training \
    --state-dir /shared/uib/run-ledgers
```

Inspect accounting without modifying the ledger:

```text
simulation/bin/uib-sim status --profile real-slurm \
    --run-id uib-training --state-dir /shared/uib/run-ledgers
```

This prefers `sacct` for the exact recorded job and reports its `JobIDRaw`,
state, exit code, elapsed time, and node list. A disposable libvirt cluster has
no accounting database, so it falls back to the standard `scontrol show job`
record and emits the same pipe-separated fields. It never searches by job
name or user. Request cancellation with:

```text
simulation/bin/uib-sim down --profile real-slurm \
    --run-id uib-training --state-dir /shared/uib/run-ledgers
```

`down` passes `--quiet` and only the recorded numeric ID to `scancel`, then
records `cancellation-requested`. This treats an already completed job as
clean and makes repeated `down` calls idempotent. It does not delete the
ledger, scheduler output, shared cache, or workload results. It refuses a
missing, malformed,
unparseable, or ownership-mismatched ledger, and never discovers jobs by user,
name, partition, or wildcard.

Run IDs are exclusive. Render, preflight, and submission failures remain in
the ledger; inspect its captured stdout/stderr when submission is ambiguous.

### Collecting a Slurm result bundle

After the job reaches a terminal scheduler state, collect its evidence before
requesting cleanup:

```text
simulation/bin/uib-sim collect --run-id uib-training \
    --state-dir /shared/uib/run-ledgers \
    --output-dir /shared/uib/collected/uib-training
```

`collect` intentionally takes no profile, request, overlay, or `--set`
arguments. It reads the exact inputs and numeric job ID captured by `run`,
then asks Slurm only for that exact ID. It prefers durable `sacct` accounting
and uses `scontrol` only when accounting is unavailable. This prevents a later
command-line change from relabeling the submitted workload. The default output
is the new ignored directory
`simulation/.artifacts/results/<run-id>`.

| Option | Value | Meaning |
|---|---|---|
| `--run-id` | The exact safe ID passed to `run` | Selects one owned ledger and result directory |
| `--state-dir` | Directory; default `simulation/.artifacts/runs` | Selects the private ledger root |
| `--output-dir` | A new directory; default `simulation/.artifacts/results/<run-id>` | Selects the complete exported bundle, not merely its parent |
| `--allow-incomplete` | Flag; default off | Allows a diagnostic snapshot while scheduler state is nonterminal |

Without `--allow-incomplete`, collection requires a terminal Slurm state. It
rejects ownership mismatches, escaping paths, symbolic links, malformed job
records, and existing output directories. Export is non-destructive and
includes inputs, scheduler records, logs, workload output, topology, metadata,
summaries, and a SHA-256 manifest; missing artifacts are explicit.

The collector deliberately sets `validationLevel` to `not-assigned`,
`uibTrafficEvidence` to `not-evaluated`, and `promotionEligible` to false.
Neither an exit code of zero nor UIB preflight proves that application payloads
used UIB; assign a validation level only after checking transport-selection,
correctness, and network evidence.

Normal users need no command overrides. The fake lifecycle gate uses these
test seams, each of which must name one executable rather than a shell command:

| Variable | Default | Purpose |
|---|---|---|
| `SBATCH_COMMAND` | `sbatch` | Submit the generated batch file |
| `SACCT_COMMAND` | `sacct` | Read accounting for the recorded job ID |
| `SCONTROL_COMMAND` | `scontrol` | Read exact job state when accounting is unavailable |
| `SCANCEL_COMMAND` | `scancel` | Cancel the recorded job ID |
| `SSH_COMMAND` | `ssh` | Reach an owned libvirt or virtme controller |
| `SCP_COMMAND` | `scp` | Copy its run-owned result files |

## virtme Slurm lifecycle

`virtme-slurm` creates one controller and two compute kernels by default. It
uses the same run-owned SSH, QEMU socket Ethernet, optional bridge, process
identity, and cleanup contracts as virtme Kubernetes. The application-facing
commands are standard `sbatch`, `srun`, `sinfo`, and `scontrol` commands.

The backend downloads the exact Debian 13 Slurm 24.11.5 and Munge 0.5.16
packages, Enroot 4.2.1, and Pyxis 0.24.0 once. Every artifact is checked against
a repository-pinned SHA-256 digest. It extracts the packages and builds Pyxis
against the pinned Slurm headers in the ignored cache instead of installing
anything on the host. This is necessary because unprivileged virtme root
cannot modify the host account database and `virtme-init`, rather than systemd,
is PID 1. Each cluster receives its own Munge key, configuration, sockets,
logs, spool, and Enroot state under `/run`.

During `up`, the backend discovers each worker's actual CPU and memory values,
starts Munge and the Slurm daemons directly, waits for every worker to become
idle, runs one ordinary Slurm task per worker, and verifies the worker data
path. It then runs an unchanged `ibv_rc_pingpong` binary between the two
workers through UIB, first directly and then inside a cached generated
BusyBox/glibc squashfs through standard Pyxis/Enroot options. Both transfers
send 32 2051-byte RC messages, so they exercise the multi-packet path rather
than only loading the library. The container run also executes the portable
runtime preflight. `status` checks both guest ownership and `slurmctld`.

The portable UIB runtime is shared read-only at `storage.uibRuntimePath`. The
Hugging Face cache is mapped below `storage.modelCachePath`, results use
`storage.resultsPath`, and the content-addressed SquashFS cache is shared at
`/var/cache/uib/images`. Each worker receives a sparse ext4 image mounted at
`/var/lib/uib/enroot` for Enroot's writable data. `nodes.disk` controls its
maximum size; the backend records every image and removes it during `down` or
failed `up`. This avoids placing expanded containers in the guest's small
memory-backed root filesystem. Standard pinned Enroot configuration supplies
the normal device mounts inside containers.

Host storage can be selected with `UIB_RUNTIME_SOURCE`,
`VIRTME_MODEL_CACHE`, and `SLURM_IMAGE_CACHE`.

```text
simulation/bin/uib-sim up --profile virtme-slurm \
    --run-id uib-virtme-slurm

image=$(simulation/bin/uib-sim load-image --profile virtme-slurm \
    --run-id uib-virtme-slurm \
    --image docker.io/library/ubuntu@sha256:2edbbc5dc405e9612ba3584ce95480277e3eb374407b5505fe26f17df77c7dbc)

simulation/bin/uib-sim run --profile virtme-slurm \
    --request simulation/requests/slurm-lifecycle-smoke.yaml \
    --run-id uib-virtme-slurm --allow-mutable-tags \
    --set "workload.image=$image"
simulation/bin/uib-sim status --profile virtme-slurm \
    --run-id uib-virtme-slurm
simulation/bin/uib-sim collect --run-id uib-virtme-slurm \
    --output-dir simulation/.artifacts/results/uib-virtme-slurm
simulation/bin/uib-sim down --profile virtme-slurm \
    --run-id uib-virtme-slurm
```

The workload gates use the same lifecycle and accept a small set of overrides:

| Variable | Values or default | Purpose |
|---|---|---|
| `VIRTME_SLURM_WORKLOAD` | `lifecycle` | Select `lifecycle`, `training`, or `inference` when invoking the test script directly |
| `VIRTME_SLURM_IMAGE` | Workload-specific local tag | Select the training or inference OCI image |
| `VIRTME_SLURM_MEMORY` | `2Gi`, `4Gi`, or `8Gi` by workload | Memory per controller and worker guest |
| `VIRTME_SLURM_DISK` | `32Gi`, or `64Gi` for inference | Maximum sparse Enroot disk size per worker |
| `VIRTME_SLURM_CACHE_SOURCE` | Ignored Hugging Face cache | Select the inference cache; it must also be `VIRTME_MODEL_CACHE` at `up` |
| `VIRTME_SLURM_JOB_WAIT` | Workload-specific seconds | Bound job completion waiting |
| `VIRTME_SLURM_RESULT_DIR` | Run-owned ignored directory | Select collected evidence output |

The Make targets set the workload and image automatically:

```text
make simulation-virtme-training-e2e-test
make simulation-virtme-inference-e2e-test

# Conserve host memory by reducing each of the three guests.
VIRTME_SLURM_MEMORY=4Gi make simulation-virtme-inference-e2e-test

# Reuse a cache on another filesystem.
VIRTME_MODEL_CACHE=/srv/uib-huggingface \
VIRTME_SLURM_CACHE_SOURCE=/srv/uib-huggingface \
    make simulation-virtme-inference-e2e-test
```

Training was validated with `4Gi` per guest. Inference defaults to `8Gi` per
guest and may work with less, but a lower value is a local tuning choice rather
than a recorded compatibility result. All three modes first run unchanged
cross-worker UIB traffic, so a passing workload cannot silently replace the
UIB transport gate with Gloo or a single local process.

This is a real multi-kernel Slurm/Pyxis/Enroot UIB L0 gate with optional
guest-side network shaping and a complete remote
`load-image`/`run`/`status`/`collect` lifecycle. Neither virtme mode is GPU or
physical-fabric evidence.

## Libvirt Slurm lifecycle

`libvirt-slurm` creates a disposable Slurm controller and one or more workers.
`uib-sim up` verifies and caches the cloud image, creates an ephemeral SSH key,
runs the pinned OpenTofu module, generates a private Ansible inventory, stages
the portable UIB runtime once, and provisions standard Slurm, Munge, NFS,
Enroot, and Pyxis components. The application-facing stack remains ordinary
`sbatch`, `srun`, and Pyxis; no private scheduler or container protocol is
introduced.

Bridge mode is the closer local model of a conventional cluster. It creates
run-owned management and isolated data networks through system libvirt. The
complete lifecycle uses the same public commands as an existing Slurm site:

```text
simulation/bin/uib-sim up --profile libvirt-slurm \
    --run-id uib-slurm-lab
simulation/bin/uib-sim run --profile libvirt-slurm \
    --request simulation/requests/slurm-lifecycle-smoke.yaml \
    --run-id uib-slurm-lab --allow-mutable-tags
simulation/bin/uib-sim status --profile libvirt-slurm \
    --run-id uib-slurm-lab
simulation/bin/uib-sim collect --run-id uib-slurm-lab \
    --output-dir simulation/.artifacts/results/uib-slurm-lab
simulation/bin/uib-sim down --profile libvirt-slurm \
    --run-id uib-slurm-lab
```

Before `run`, `status` reports infrastructure state. After a job has been
recorded, it reports that exact Slurm job. `run`, `status`, `collect`, and the
job-cancellation part of `down` automatically use the controller endpoint and
ephemeral key from the owned inventory. The host therefore needs `ssh` and
`scp`, but no local Slurm client. The rendered batch is piped to ordinary
`sbatch --parsable`; result paths and `%x-%j` logs are copied back before the
VMs are destroyed. Always collect a libvirt run before `down`, because the
controller and its shared result storage are disposable.

The named `libvirt.pool` must already be active. The caller also needs normal
permission to use the configured URI and create networks. Each run receives
its own OpenTofu state, overlays, cloud-init volume, key, networks, domains,
inventory, and lifecycle log under
`simulation/.artifacts/runs/<run-id>/infrastructure/`.

Rootless mode provides a passwordless functional path for development. It
uses `qemu:///session`, passt management interfaces with deterministic
loopback SSH forwards, and an unprivileged multicast tunnel for the private
data plane. The repository gate starts a bounded per-user daemon when needed,
creates an exact temporary storage pool, provisions one controller and two
worker VMs by default, runs the gates, destroys the OpenTofu state, and removes
only that pool:

```text
make simulation-libvirt-e2e-test
```

The gate verifies controller and worker health, NFSv4, the private data
network, ordinary Slurm tasks, and digest-pinned Pyxis/Enroot execution. Its
UIB check places one unchanged `ibv_rc_pingpong` rank on each worker and
exchanges 32 correctness-checked 2051-byte multi-packet RC messages through
UIB/RoCEv2. It also runs the common UIB preflight in both containers, collects
rank and scheduler evidence, and removes the exact job and infrastructure.

This is actual cross-VM UIB traffic, but remains CPU L0 evidence. It is not an
NCCL collective, a GPU path, or a performance model. Passt and multicast
networking are unsuitable for throughput or latency claims; use bridge mode or
physical hardware for those measurements.

The disposable lab limits its NFS export to run-owned worker addresses and
uses `no_root_squash` for Enroot. Do not apply that role or the rootless
multicast tunnel to an existing multi-user cluster; use the site's secured
shared storage and segmented network there.

Infrastructure and Slurm jobs have separate ownership ledgers under one run
ID. `down` cancels only a recorded numeric job ID, then destroys the owned
OpenTofu state. The disposable controller keeps completed `scontrol` records
for collection; production profiles use site `sacct` accounting.

### Existing Slurm

The renderer accepts partition, account, QoS, reservation, constraint, shared
paths, and data-interface configuration. `run`, `status`, and `down` provide
local submission-node lifecycle delegation, and `collect` exports the recorded
job's evidence. Remote login and file transfer remain later work; run the CLI
on a submission host that can see the ledger and shared result paths.

```yaml
infrastructure:
  provider: existing
cluster:
  manager: slurm
container:
  runtime: enroot
  integration: pyxis
slurm:
  partition: gpu
  account: llm-validation
  qos: normal
  constraint: h100
  timeLimit: "01:00:00"
  rendezvousPort: 29500
  exclusive: false
nodes:
  workers: 4
  cpu: 128
  memory: 1Ti
  disk: 1Ti
  gpus:
    vendor: nvidia
    perWorker: 8
    resourceName: gpu
storage:
  uibRuntimePath: /shared/uib/runtime
  modelCachePath: /shared/uib/models
  resultsPath: /shared/uib/results
network:
  dataInterface: bond-rdma
  shaping:
    enabled: false
```

The lifecycle records the returned job ID and cancels only that job during
cleanup. It never changes Slurm configuration, partitions, Pyxis, Enroot,
shared caches, or unrelated jobs.

## Training workloads

The implemented training image deliberately has no scheduler API. Kubernetes
and Slurm adapters, plus a local shell, all launch the same executable:

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

The CPU smoke target checks two-rank DDP, checkpoint/restart equivalence, and
the pinned NCCL ABI. Set `TRAINING_SKIP_BUILD=1` to reuse an existing image or
`TRAINING_IMAGE=registry/image@sha256:<digest>` to test a published image.

The opt-in GPU target uses standard CDI, initializes NCCL, and checks FSDP
checkpoint/restart. Set `TRAINING_GPU_DEVICE` to another CDI device name when
necessary. One GPU exercises `NO_SHARD` and therefore does not validate
multi-GPU sharding or network traffic.

On a multi-GPU node, select the local process count explicitly. The default
strategy is FSDP; DDP uses the same gate and checkpoint comparison:

```text
make TRAINING_GPU_PROCESSES=8 training-gpu-test
make TRAINING_GPU_PROCESSES=8 TRAINING_GPU_STRATEGY=ddp training-gpu-test
```

The result records the actual world size and requires the restarted model
checksum to match the uninterrupted run. Multi-GPU support remains a
hardware-required gate until it passes on a suitable host.

The UIB preflight target mounts the common runtime artifact read-only at
`/opt/uib`, validates it, and runs the CPU workload gates. It proves image
composition but does not send UIB traffic.

The opt-in `training-uib-test` runs two unchanged training ranks through NCCL
and UIB, using NVIDIA MPS when one local GPU must be shared. It selects
`NCCL_NET=IB`, `uib0`, and `NCCL_IBVERBS_LIB=/opt/uib/lib/libuibv.so`, requires
NCCL's IB transport evidence, and rejects Socket fallback.

Configure the local gate with `TRAINING_CONTAINER_ENGINE`, `MPIEXEC`,
`TRAINING_UIB_BIND_PORT`, `TRAINING_MASTER_PORT`, `TRAINING_UIB_TIMEOUT`, and
`TRAINING_GPU_DEVICE`. It uses relaxed namespaces and seccomp only for its
disposable local containers. For example:

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

The current Kubeflow renderer uses the same image, executable, JSON ConfigMap,
and environment. Its L0 smoke manifest uses `emptyDir` storage; real profiles
can select existing shared cache and result claims without changing the
workload. This makes changing Kubernetes to Slurm a launcher change rather
than a training-code change.

Native verbs and UIB also use the same image digest and command. The native
baseline has no UIB variables. The UIB variant adds only the `/opt/uib` mount
and the environment documented in
[Transparent UIB injection](#transparent-uib-injection), including
`NCCL_IBVERBS_LIB=/opt/uib/lib/libuibv.so`. The result records whether those
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
Kubernetes, Slurm, and a local container use the
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

The smoke target checks Ray plus the CPU backend's health, model, completion,
and streaming paths. The GPU target applies the same deterministic probe to a
real vLLM server and saves the unmodified `vllm bench serve` result beside the
common evidence.

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
`NCCL_IBVERBS_LIB=/opt/uib/lib/libuibv.so`, and keep the same workload JSON for the
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

`tc netem` is the default impairment mechanism. The virtme and libvirt Slurm
backends apply it to each disposable worker's data-interface egress. Kind and
host-side libvirt tap placement remain backend extensions. A real-cluster
profile normally disables shaping and records physical NIC and switch counters
instead.

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

The virtme and libvirt Slurm lifecycles apply the same adapter automatically
before their actual cross-worker UIB traffic gates:

```text
make simulation-virtme-netem-e2e-test

VIRTME_NETWORK_CONDITION=constrained \
make simulation-virtme-netem-e2e-test

make simulation-libvirt-netem-e2e-test

LIBVIRT_NETWORK_CONDITION=constrained \
make simulation-libvirt-netem-e2e-test
```

Both targets default to `datacenter`; accepted values are `clean`,
`datacenter`, `constrained`, `lossy`, `reordered`, and `ecn`. The condition is
applied symmetrically to the egress of every worker data interface. Because
the guests and their interfaces are run-owned, each lifecycle explicitly
replaces their initial root qdisc. After the cross-worker UIB gate, it requires
the requested `netem` state and nonzero qdisc traffic counters, then records
the printed apply command, `tc -s` output, kernel release, iproute2 version,
and whether a requested seed was accepted for every worker. Older
netem releases without `seed` support omit that option, warn, and record
`seedApplied: false`; they are functional but not packet-decision reproducible.
The Slurm `collect` path validates and exports the record as
`network-emulation.json`; the generated manifest covers it.
The same network overlay must be supplied to `up` and `run`, and `uib-sim`
rejects a workload whose resolved network differs from its existing libvirt
lab. The provided end-to-end wrapper handles that invariant automatically.

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
| `seed` | Unset | Seed for reproducible random decisions when the target netem supports it; otherwise evidence records `seedApplied: false` |

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
  transportAllowlist: ["verbs;ofi_rxd"]
  libfabric:
    provider: "verbs;ofi_rxd"
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

Apptainer is a planned adapter for Slurm sites without Pyxis:

```yaml
container:
  runtime: enroot
  integration: pyxis
```

will eventually be replaceable with:

```yaml
container:
  runtime: apptainer
```

The adapter must still resolve the same OCI digest, mount `/opt/uib` and the
same data paths, and run the same command.

The current batch renderer accepts only `runtime: enroot` with
`integration: pyxis` and rejects Apptainer, preventing an unsupported runtime
from looking runnable.

## Running a simulation

Configuration stays deliberately conventional: a small defaults file, one
YAML profile, an optional YAML workload request, and standard JSON Schema
validation. The thin `uib-sim` wrapper merges and validates configuration and
delegates the implemented Kind and libvirt Kubernetes/Slurm infrastructure
lifecycles. It does not replace Kind, OpenTofu, Ansible, Helm, `kubectl`,
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
`protocol/uet-nscc` to `protocol/rocev2-nscc-experimental`. The latter is an
explicitly experimental cross-protocol comparison, not a RoCEv2 deployment
recommendation. A site can pass a YAML file path instead of a checked-in
overlay. The rendered file is ordinary YAML that can be inspected, versioned,
and translated directly to Helm values, OpenTofu variables, an Ansible
inventory, or Slurm environment settings.

The schema rejects unknown keys and malformed units. A few cross-field checks
reject impossible rank products, incompatible scheduler/runtime choices,
RCCL without AMD GPUs, unvalidated GPU-direct, invalid congestion parameters,
and shaping existing hardware without explicit permission.
`doctor` is read-only and reports which standard tools are missing. Run
`make simulation-cli-test` to validate every checked-in profile. The wrapper
requires Python, PyYAML, and jsonschema.

Each backend keeps using its own normal configuration and state:

- Kind configuration and cluster names;
- OpenTofu state for libvirt resources;
- Ansible inventories and roles for node provisioning;
- Helm values and Kubernetes labels/namespaces;
- Slurm batch files, job IDs, Pyxis, and Enroot configuration.

`uib-sim` delegates Kind and libvirt Kubernetes/Slurm infrastructure now. The
remaining adapters follow the same standard-tool pattern:

```text
simulation/bin/uib-sim doctor --profile <profile>
kind create cluster --config <rendered-kind-config>
tofu -chdir=<run-directory> apply
ansible-playbook -i <inventory> <playbook>
helm upgrade --install <release> <chart> -f <rendered-values>
sbatch <rendered-job>
```

`doctor` is read-only. It reports the selected profile, provider, manager,
communication front end, and presence of required command-line tools. The
libvirt `up` path performs image and node provisioning plus manager-specific
checks inside its disposable guests. The Slurm path also checks shared
storage, Pyxis, Enroot, and the UIB runtime. Kernel, GPU/RDMA, and
physical-fabric qualification remain separate promotion gates.

Kind `up` creates only the explicit `--run-id` cluster. Libvirt `up` creates
only resources recorded in that run's private OpenTofu state, then provisions
the selected Kubernetes or Slurm manager through its generated inventory. For
existing clusters the adapter performs no infrastructure changes. The general
`deploy` operation is not implemented yet.

The Slurm `run` adapter resolves configuration, records its exact inputs,
checks shared paths, renders the batch file, submits it, and records ownership.
Its `collect` adapter exports one exact recorded job without deleting the
workload; use a new output directory for each repeat snapshot. A Kubernetes
`run` adapter and deeper Slurm capability preflights remain planned.

Kind `down` removes the named Kind cluster; Slurm `down` requests cancellation
of the recorded job. Automatic export from `down` and a common Kubernetes
collector remain planned; invoke Slurm `collect` explicitly before cleanup.

### Fast local example

```text
uib-sim doctor --profile kind-kubernetes
uib-sim up --profile kind-kubernetes --run-id uib-lab
uib-sim status --profile kind-kubernetes --run-id uib-lab
uib-sim kubeconfig --profile kind-kubernetes --run-id uib-lab
uib-sim down --profile kind-kubernetes --run-id uib-lab
```

These lifecycle operations are implemented for Kind and libvirt Kubernetes.
Slurm additionally
implements `run`, accounting-oriented `status`, evidence-only `collect`, and
recorded-job `down`. `deploy`, Kubernetes collection, and Kubernetes workload
submission remain planned.

### Planned selected-kernel lifecycle

```text
uib-sim up --profile virtme-kind \
    --set infrastructure.kernel=/boot/vmlinuz-test
uib-sim run --request collectives-smoke.yaml
uib-sim down
```

### Disposable NVIDIA-style Slurm infrastructure

```text
simulation/bin/uib-sim up --profile libvirt-slurm \
    --run-id uib-slurm-smoke
simulation/bin/uib-sim status --profile libvirt-slurm \
    --run-id uib-slurm-smoke
simulation/bin/uib-sim down --profile libvirt-slurm \
    --run-id uib-slurm-smoke
```

This provisions and validates the VM cluster automatically. It does not yet
make the local Slurm `run` adapter submit remotely through the guest
controller; see [Libvirt Slurm lifecycle](#libvirt-slurm-lifecycle). Use the
`real-slurm` profile on a normal login node for current workload submission.

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

Planned Kubernetes promotion:

```text
uib-sim doctor --profile real-kubernetes
uib-sim run --profile real-kubernetes --request common-training.yaml
uib-sim down
```

Slurm promotion:

```text
simulation/bin/uib-sim doctor --profile real-slurm \
    --request common-training.yaml
simulation/bin/uib-sim run --profile real-slurm \
    --request common-training.yaml --run-id uib-real-training \
    --state-dir /shared/uib/run-ledgers
simulation/bin/uib-sim status --profile real-slurm \
    --run-id uib-real-training --state-dir /shared/uib/run-ledgers
simulation/bin/uib-sim collect --run-id uib-real-training \
    --state-dir /shared/uib/run-ledgers \
    --output-dir /shared/uib/collected/uib-real-training
```

Request cancellation, if needed, with the same profile, run ID, and state
directory passed to `down`; it uses only the ledger's numeric job ID.

Named scale overlays such as `tiny`, `two-node`, `rack`, and `production` can
change node count and parallel degrees while retaining application code,
framework, communication backend, model architecture, checkpoint format, and
metric definitions.

## Results and evidence

The implemented Slurm collector produces:

```text
results/<run-id>/
  profile.yaml
  request.yaml
  overlays/
  request-provenance.json
  resolved-values.json
  compatibility-lock.json
  versions.json
  topology.json
  images.json
  preflight.json
  network-emulation.json      # when infrastructure supplied validated evidence
  manifest.json
  scheduler/
    workload.sbatch
    status.psv
    status-source.json
    sacct.stderr
    sacct.psv                 # when sacct supplied the record
    scontrol.txt              # when scontrol supplied the record
    scontrol.stderr           # when scontrol supplied the record
    render.stderr
    sbatch.stdout
    sbatch.stderr
  state/
    ledger.json
  logs/
    batch.out
    batch.err
  workload/
    status.json
    nodes.txt
    slurm-job.txt
    ... workload-owned checkpoints, metrics, and traces ...
  metrics/
  traces/
  faults.jsonl
  summary.json
  summary.md
```

`request.yaml` is absent only when `run` had no request file; the exact profile,
ordered overlays, typed `--set` expressions, and fully resolved result still
remain. Empty common `metrics/` and `traces/` directories reserve the portable
schema while workload-native files remain unchanged under `workload/`.
`manifest.json` records the size and SHA-256 digest of every regular file other
than the manifest itself. Files absent because a batch failed early are named
in `summary.json.missingArtifacts`.

The long-term common summary states:

- UIB or native verbs;
- communication front end and its selected or fallback transport;
- UIB protocol, congestion algorithm, and resolved algorithm parameters;
- host-staged or GPU-direct memory;
- L0, L1, L2, or L3 validation level;
- exact image and library digests;
- actual rank placement;
- skipped gates and reasons;
- correctness and performance outcomes.

The current collector fills identity, scheduler outcome, requested and
allocated topology, images, selected UIB layers, workload exit status,
validated libvirt qdisc evidence when available, and missing artifacts. It
does not infer transport use, correctness, performance, or a validation level
from logs; those promotion checks remain explicit so a single-process or
socket-fallback run cannot be mislabeled as UIB validation.

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
| `make simulation-congestion-test` | Build dependencies, `ip`, `jq`, `tc`, `timeout`, `unshare`, and permission to create a network namespace | RoCEv2 and UET SEND and RDMA read/write complete with actual netem conditions and built-in or plugin congestion control; the UET cases also cover atomics |
| `make simulation-kind-test` | Python 3 and PyYAML; no container daemon | Digest-pinned standard Kind YAML, worker count, safe named create/status/kubeconfig/delete delegation using a fake Kind executable |
| `make simulation-virtme-backend-test` | Python 3, PyYAML, and `jsonschema`; no guest boot or elevated network permission | Rootless socket and optional bridge rendering, IPv4/MAC/SSH allocation, exact virtme-ng launch vectors, run-owned key/inventory creation, PID/start-time ownership, CLI `up/status/down`, bridge create/delete cleanup through fake standard commands, and invalid combination rejection |
| `make simulation-virtme-kubernetes-e2e-test` | virtme-ng 1.35, SSH, kubectl, QEMU with KVM or TCG, about 4 GiB memory for the two reduced-size guests, and download access for the pinned k3s binary on first use | A real controller and worker under separate virtme kernels, rootless socket data traffic, pinned k3s/containerd startup without systemd PID 1, both nodes Ready, host API access through an owned SSH forward, cached download reuse, and exact process cleanup |
| `make simulation-virtme-slurm-e2e-test` | virtme-ng 1.35, SSH/SCP, QEMU with KVM or TCG, `jq`, `tc`, Slurm/Pyxis build tools, a Docker-compatible engine for the portable runtime and workload image, about 6 GiB memory, and download access on the first run | A real controller/two-worker Slurm/Pyxis/Enroot lifecycle, direct and containerized cross-worker UIB traffic, automatic pinned-image staging, remote two-rank `run`/`status`/`collect`, clean network evidence, portable-runtime preflight, and exact cleanup |
| `make simulation-virtme-netem-e2e-test` | The virtme Slurm gate requirements | The same complete workload lifecycle while the selected condition is active on both worker data interfaces; exact qdisc/kernel/iproute2 evidence and cleanup are validated |
| `make simulation-libvirt-test` | Python 3, PyYAML, `jsonschema`, POSIX shell tools, and no libvirt daemon | Deterministic bridge/rootless VM rendering; Slurm and bridge-only Kubernetes playbook selection; pinned k3s inputs, non-logged join token, kubeconfig export, cached-image verification, worker network evidence, Slurm remote lifecycle, exclusive ownership, and exact-state cleanup with fake standard commands |
| `make simulation-kind-e2e-test` | Kind, an accessible Docker-compatible runtime, `timeout`, network access for the pinned node image on its first run, and resources for the requested nodes | A real disposable Kind control plane and workers become ready, expose kubeconfig, optionally load images, report the exact node count, and are absent after cleanup |
| `make simulation-libvirt-e2e-test` | A Docker-compatible engine for the portable runtime, OpenTofu 1.12.3, libvirt provider 0.9.8, Ansible Core, libvirt/QEMU/KVM, `virsh`, SSH/SCP, passt, NFS-capable Ubuntu guests, roughly 12 GiB memory for the default three VMs, and download access on the first run | Actual controller/two-worker VMs, private data networking, NFSv4, exact Slurm 23.11.4 packages, Munge, Enroot 4.2.1, Pyxis 0.24.0, ordinary worker tasks, cross-VM standard RC payload through UIB, remote batch submission and status, two-rank pinned OCI execution with portable UIB preflight, result collection, state-based destruction, and domain/pool absence; no NCCL or GPU claim |
| `make simulation-libvirt-netem-e2e-test` | The libvirt gate requirements; guest iproute2 and `jq` are installed automatically | The selected checked-in condition is active on both worker data interfaces before actual cross-VM UIB traffic, exact qdisc/kernel/iproute2 evidence is collected, the Slurm lifecycle passes, and the VMs are removed; rootless timing and throughput are not physical-fabric measurements |
| `make simulation-libvirt-training-e2e-test` | The libvirt gate requirements, the local training image or image-build access, 32 GiB guest disks, roughly 12 GiB host memory, and about 3 GiB ignored host cache space for the current image | Everything in the libvirt gate, content-addressed SquashFS creation or verified cache reuse, real two-worker CPU DDP training through Slurm/Pyxis/Enroot, collected training and image ledgers, and complete cleanup; the workload uses Gloo, while the separate provisioning gate supplies actual UIB traffic |
| `make simulation-libvirt-inference-e2e-test` | The libvirt gate requirements, the local inference image or build access, the downloaded SmolLM2 cache, 8 GiB per VM, 64 GiB guest disks, and about 8 GiB ignored host image-cache space | Everything in the libvirt gate, bounded-memory SquashFS staging, verified Hugging Face cache transfer, real offline four-request CPU serving through Slurm/Pyxis/Enroot, request trace and artifact ledgers, and cleanup; the one-task workload itself has no UIB traffic |
| `make simulation-kubernetes-render-test` | Python 3, PyYAML, and `jsonschema`; no cluster or container engine | Kubeflow Trainer v2.2.1 and KubeRay v1.6.2 resource shapes, ownership labels, unchanged workload JSON, UIB init and environment injection, protocol switching, native baseline, fixed Ray worker shape, digest enforcement, and rejection of unsafe combinations |
| `make simulation-slurm-render-test` | Python 3, PyYAML, `jsonschema`, and a POSIX shell; no Slurm installation or container engine | Training and one-task inference batch syntax, exact Slurm resources, Pyxis image/mount/environment arguments, unchanged workload JSON, rank-specific UIB address/ports, UIB preflight, RoCEv2/UET congestion settings, native baseline, immutable-image enforcement, protected result directories, and execution against fake `scontrol`/`srun` commands |
| `make simulation-slurm-lifecycle-test` | The Slurm renderer requirements; no Slurm installation or container engine | Exclusive ignored run ledgers, shared-path preflight, exact `sbatch --parsable` submission, recorded numeric job ownership, read-only `sacct` status and `scontrol` fallback including an empty pending node list, exact-ID `scancel`, retained failures, and rejection of result/ledger reuse or ambiguous submission output using fake standard commands |
| `make simulation-slurm-collect-test` | The Slurm lifecycle requirements; no Slurm installation, container engine, or network access | Exact-ID `sacct` and `scontrol` evidence, terminal/incomplete policy, captured source inputs, scheduler/log/workload export, ownership validation, symlink rejection, non-overwrite behavior, honest nonpromotion labels, and every SHA-256 manifest entry using fake standard commands |
| `make runtime-bundle-portable-test` | A Docker-compatible engine, network access for the pinned Ubuntu image and verified source archives on first use, and normal UIB build resources | Reproducible old-userspace build, GLIBC ceiling, portable feature set, provider/static artifacts, checksums, preflight, source dependency metadata, and copied licenses |
| `make simulation-kubernetes-uib-test` | Kind, kubectl, a Docker-compatible engine, build dependencies, and CPU/memory for two workers; network access is needed only for uncached pinned images and diagnostic sources | Actual RoCEv2 and UET RC SEND/RECV and RDMA Write traffic from unchanged standard verbs binaries between distinct Pod IPs and Kind workers, plus runtime injection and complete cleanup; no GPU or collective claim |
| `make simulation-kubeflow-e2e-test` | The pinned Kind/kubectl/Helm clients, a Docker-compatible engine, cached training/runtime images, network access for the operator on first use, and two workers' CPU/memory | Actual Trainer installation, admission, two-node torchrun rendezvous, distinct placement, UIB preflight, CPU DDP result, and complete run/operator/cluster cleanup; no UIB traffic |
| `make simulation-kuberay-e2e-test` | The pinned Kind/kubectl/Helm clients, a Docker-compatible engine, roughly 10 GB of image storage, four CPU cores and 8 GiB for the Ray head; network access for uncached images, source archives, chart, and model revision | Required actual two-worker UIB verbs traffic, portable-runtime compatibility, KubeRay v1.6.2 admission and Ray 2.58.0 readiness, offline cached SmolLM2 inference, four-result correctness, retained evidence, and complete cleanup; the single-rank inference portion itself sends no UIB traffic |

Run the ten hardware-neutral gates:

```text
make simulation-cli-test
make simulation-netem-test
make simulation-congestion-test
make simulation-kind-test
make simulation-virtme-backend-test
make simulation-libvirt-test
make simulation-kubernetes-render-test
make simulation-slurm-render-test
make simulation-slurm-lifecycle-test
make simulation-slurm-collect-test
```

The mocked Kind adapter test does not create a cluster. The opt-in real gate is:

```text
make simulation-kind-e2e-test
```

It creates an explicitly named disposable cluster and always attempts cleanup
after `up`, including failure paths. The successful local gate used one
control-plane and one worker. The digest-pinned node image remains in the
container runtime's normal cache for later runs.

| Kind option | Default | Syntax and meaning |
|---|---|---|
| `cluster.nodeImage` | Pinned in `kind-kubernetes.yaml` | Digest-qualified node image written to every Kind node |
| `--run-id` | Required by `uib-sim` lifecycle commands | Lowercase DNS label, at most 63 characters; becomes the only Kind cluster an operation may inspect or delete |
| Backend `--name` | Required when invoking the backend directly | Same value and validation as `--run-id` |
| `--wait` | `120s` | Positive integer plus `s`, `m`, or `h` passed to `kind create cluster` |
| `--kubeconfig` | Kind default | Explicit output path for the new cluster's kubeconfig |
| `--retain` | Off | Preserve Kind nodes when cluster creation fails |
| `KIND_COMMAND` | `kind` | Exact Kind executable; the default profile is validated with v0.31.0 |
| `load-image --image` | Required; repeatable | Host image passed to standard `kind load docker-image` |
| `--allow-mutable-tags` | Off | Permit a local development tag such as `uib-training:test`; incompatible with promotion or reproducibility claims |

Real Kind gate options:

| Variable | Default | Syntax and meaning |
|---|---|---|
| `SIMULATION_KIND_E2E_NAME` | Unique `uib-e2e-<pid>` | Explicit cluster name owned by the test |
| `SIMULATION_KIND_E2E_WORKERS` | `1` | Positive worker count |
| `SIMULATION_KIND_E2E_WAIT` | `180s` | Kind readiness duration |
| `SIMULATION_KIND_E2E_TIMEOUT` | `600` | Positive whole-test timeout in seconds |
| `SIMULATION_KIND_E2E_IMAGES` | Unset | Whitespace-separated image references loaded after readiness |
| `SIMULATION_KIND_E2E_ALLOW_MUTABLE_TAGS` | `0` | `0` requires digests; `1` permits local tags |

Examples:

```text
SIMULATION_KIND_E2E_WORKERS=2 make simulation-kind-e2e-test

SIMULATION_KIND_E2E_IMAGES='registry.example/training@sha256:<digest>' \
make simulation-kind-e2e-test

SIMULATION_KIND_E2E_IMAGES='uib-training:test uib-inference:test' \
SIMULATION_KIND_E2E_ALLOW_MUTABLE_TAGS=1 \
make simulation-kind-e2e-test
```

### virtme Kubernetes test

Run the real reduced-size controller/worker gate with:

```text
make simulation-virtme-kubernetes-e2e-test
```

The test always attempts `down`, including after a failed `up`. It retains the
verified k3s binary and checksum sidecar under the ignored download cache, but
does not retain QEMU processes or the SSH API forward.

| Variable | Default | Syntax and meaning |
|---|---|---|
| `KUBECTL_COMMAND` | `kubectl` | kubectl executable used only for the host-facing node check |
| `SIMULATION_STATE_DIR` | `simulation/.artifacts/runs` | Ignored or external run-ledger directory |
| `VIRTME_DOWNLOAD_CACHE` | `simulation/.artifacts/downloads/virtme` | Reusable verified k3s download cache |
| `VIRTME_SSH_WAIT` | `90` | Positive seconds allowed for each guest SSH endpoint |
| `VIRTME_KUBERNETES_WAIT` | `300` | Positive seconds allowed for the API and node join |

The test fixes one worker, two vCPUs, and 2 GiB per guest to keep the local gate
small. Use normal `uib-sim up` with `--set nodes.*` for larger experiments.

### virtme Slurm test

Run the real controller/two-worker scheduler gate with:

```text
make simulation-virtme-slurm-e2e-test
```

Run the same gate with the default `datacenter` condition using:

```text
make simulation-virtme-netem-e2e-test

VIRTME_NETWORK_CONDITION=lossy \
make simulation-virtme-netem-e2e-test
```

The test requires an amd64 Debian 13-compatible userspace, `vng`, KVM or QEMU
emulation, `ssh`, `ssh-keygen`, `apt-get`, `dpkg-deb`, `make`, a C compiler,
and a Docker-compatible engine for the portable runtime build. The first run
needs network access to the configured signed Debian repositories, the pinned
NVIDIA release artifacts, and the portable-runtime builder image. Later runs
reuse the verified downloads, extracted runtime, compiled Pyxis plugin,
generated smoke image, and portable UIB runtime. The gate exercises Munge
authentication, controller health, per-worker registration, multi-node
`srun`, direct and Pyxis-contained UIB traffic, portable-runtime preflight,
automatic pinned-image staging, remote workload submission/status/collection,
and exact process cleanup.

| Variable | Default | Syntax and meaning |
|---|---|---|
| `SIMULATION_STATE_DIR` | `simulation/.artifacts/runs` | Ignored or external run-ledger directory |
| `VIRTME_DOWNLOAD_CACHE` | `simulation/.artifacts/downloads/virtme` | Reusable verified package, source, runtime, plugin, and smoke-image cache |
| `UIB_RUNTIME_SOURCE` | `simulation/.artifacts/uib-runtime` | Host path to the complete portable runtime shared read-only with every guest |
| `VIRTME_MODEL_CACHE` | `simulation/.artifacts/huggingface` | Host path shared read-write at `storage.modelCachePath` in every guest |
| `VIRTME_NETWORK_CONDITION` | Unset; the netem target sets `datacenter` | Empty, `clean`, `datacenter`, `constrained`, `lossy`, `reordered`, or `ecn`; the selected overlay applies to every worker data-interface egress |
| `VIRTME_SLURM_JOB_WAIT` | `120` | Positive seconds allowed for the end-to-end workload job to reach a terminal state |
| `SLURM_IMAGE_CACHE` | `simulation/.artifacts/slurm-images` | Reusable verified host SquashFS cache shared with the virtme guests |
| `PORTABLE_RUNTIME_CONTAINER_ENGINE` | `docker` | Docker-compatible engine used to build and verify the portable runtime dependency |
| `VIRTME_SSH_WAIT` | `90` | Positive seconds allowed for each guest SSH endpoint |
| `VIRTME_SLURM_WAIT` | `60` | Positive seconds allowed for controller and worker readiness |
| `APT_GET_COMMAND` | `apt-get` | Debian package downloader used only on a cold cache |
| `DPKG_DEB_COMMAND` | `dpkg-deb` | Package metadata checker and extractor |
| `VIRTME_SLURM_PACKAGE_DIR` | Unset | Development-only directory containing the exact package names and versions; repository hashes are not enforced for this explicit override |

The test fixes two workers, two vCPUs, and 2 GiB per guest. Override normal
profile values for larger experiments. The explicit package-directory override
is intended for local package development and is not promotion evidence.

### Libvirt VM tests

The structural libvirt gate uses fake executables and does not contact a
daemon:

```text
make simulation-libvirt-test
```

The opt-in end-to-end gate defaults to a passwordless per-user daemon,
rootless networking, one controller, and two workers:

```text
make simulation-libvirt-e2e-test
```

Run the cross-worker UIB gate with the default datacenter-like condition:

```text
make simulation-libvirt-netem-e2e-test
```

Run the real CPU training variant with:

```text
make simulation-libvirt-training-e2e-test
```

It builds `uib-training:test` only when that image is absent, uses a 32 GiB
guest disk, stages the image into the lab, and runs the unchanged generated
four-step request on two workers. No model or dataset is stored in the
repository or downloaded for this default workload.

Run the real CPU inference variant with:

```text
make simulation-libvirt-inference-e2e-test
```

The normal inference smoke test downloads the pinned SmolLM2 revision into the
ignored Hugging Face cache once. The libvirt gate stages that cache and the
same production-capable vLLM image, then uses its CPU Transformers backend for
four offline requests. The target uses 8 GiB per VM and 64 GiB guest disks.
The first image conversion can take tens of minutes; its verified SquashFS is
retained in the ignored host image cache. Conversion intentionally uses one
compressor and at most 512 MiB of SquashFS working memory so it cannot starve
the controller VM while staging the production-capable image.

The gate builds the verified portable UIB runtime rather than a host-native
bundle, because the diagnostic container may have an older glibc than the
development host. On its first run it downloads the digest-verified cloud
image, the pinned OpenTofu provider, Enroot and Pyxis sources, Ubuntu packages
in the guests, and the diagnostic OCI image. Host downloads are retained under
the ignored `simulation/.artifacts` cache. OCI and package-manager caches
follow their normal host or guest behavior; model and dataset files are never
committed.

| Variable | Default | Syntax and meaning |
|---|---|---|
| `LIBVIRT_URI` | `qemu:///session` | Exact libvirt URI; rootless mode requires the session URI |
| `LIBVIRT_NETWORK_MODE` | `rootless` | `rootless` for passt plus multicast, or `bridge` for managed networks |
| `LIBVIRT_NETWORK_CONDITION` | Unset; the netem target sets `datacenter` | Empty for the profile setting, or `clean`, `datacenter`, `constrained`, `lossy`, `reordered`, or `ecn`; applied to every worker data-interface egress |
| `LIBVIRT_POOL` | Unique run ID | Active pool name; the gate creates and later removes a missing session pool |
| `LIBVIRT_POOL_DIR` | Ignored run-specific path | Directory backing a session pool created by the gate; not used for a pre-existing pool |
| `LIBVIRT_WORKERS` | `2` | Positive worker count; two or more place the UIB traffic ranks on distinct VMs |
| `LIBVIRT_CPUS` | `2` | Positive vCPU count per VM |
| `LIBVIRT_MEMORY` | `4Gi` | Per-VM memory with `Mi`, `Gi`, or `Ti` suffix |
| `LIBVIRT_DISK` | `16Gi`; training sets `32Gi`, inference sets `64Gi` | Per-VM overlay capacity with `Mi`, `Gi`, or `Ti` suffix; first-use image conversion needs room for the expanded root filesystem and SquashFS |
| `LIBVIRT_E2E_RUN_ID` | Unique `uib-libvirt-e2e-<pid>` | Safe run ID used for all owned resources and state |
| `LIBVIRT_E2E_RESULT_DIR` | `simulation/.artifacts/results/<run-id>` | New host directory that receives collected scheduler and workload evidence |
| `LIBVIRT_E2E_WORKLOAD` | `lifecycle` | `lifecycle`, `training`, or `inference`; dedicated targets select the latter modes |
| `LIBVIRT_E2E_IMAGE` | Mode-specific local image | Existing local training or inference OCI image to stage; ignored in lifecycle mode |
| `LIBVIRT_E2E_CACHE_SOURCE` | Ignored `simulation/.artifacts/huggingface` | Existing Hugging Face cache root used only in inference mode |
| `LIBVIRT_E2E_JOB_TIMEOUT` | `180` for lifecycle, `900` for training, `1200` for inference | Integer of at least two seconds; bounds only the submitted workload, not provisioning or cleanup |
| `LIBVIRT_SSH_PORT_BASE` | PID-derived value from 30000 through 49999 | First localhost SSH-forward port for the disposable rootless gate; workers use consecutive ports |
| `SIMULATION_LIBVIRT_STATE_DIR` | `simulation/.artifacts/runs` | Private lifecycle-ledger root |
| `CONTAINER_ENGINE_COMMAND` | `docker` | Exact Docker-compatible executable used to inspect, create, export, and remove the stopped staging container |
| `SLURM_IMAGE_CACHE` | `simulation/.artifacts/slurm-images` | Ignored host directory containing verified SquashFS files, checksum sidecars, and `inventory.json` |
| `SLURM_CACHE_INVENTORY` | `simulation/.artifacts/slurm-caches` | Ignored directory containing model-cache transfer metadata, never model contents or an absolute source path |
| `TAR_COMMAND` | `tar` | Exact local executable used to archive the validated Hugging Face cache |
| `TOFU_COMMAND` | `tofu` | Exact OpenTofu executable; the module requires version 1.12.3 |
| `ANSIBLE_PLAYBOOK_COMMAND` | `ansible-playbook` | Exact Ansible playbook executable |
| `LIBVIRTD_COMMAND` | Discovered `libvirtd` | Exact daemon executable used only when a session daemon is unavailable |
| `ANSIBLE_LOCAL_TEMP` | Ignored artifacts path | Controller-side Ansible temporary directory |

For a larger rootless functional lab:

```text
LIBVIRT_WORKERS=3 LIBVIRT_CPUS=4 LIBVIRT_MEMORY=8Gi \
make simulation-libvirt-e2e-test
```

Reuse a cache on a larger filesystem and allow a slower cold CPU launch:

```text
SLURM_IMAGE_CACHE=/srv/uib-sim-cache \
LIBVIRT_E2E_JOB_TIMEOUT=1200 \
make simulation-libvirt-training-e2e-test
```

`LIBVIRT_E2E_IMAGE` has an effect only with training or inference.
`LIBVIRT_E2E_CACHE_SOURCE` has an effect only with inference. Do not combine
the local mutable-tag workflow
with promotion results; use a registry digest on real hardware. Do not point
`SLURM_IMAGE_CACHE` at a shared directory whose contents can be modified by
untrusted users, because a cache integrity failure deliberately stops the run.

`LIBVIRT_WORKERS=1` is a resource-saving fallback. It still runs two processes
through UIB, but both ranks share one worker VM and therefore cannot satisfy
the cross-VM traffic claim.

For bridge mode, supply an active system pool and suitable libvirt network
permissions. The gate never creates or removes a system pool:

```text
LIBVIRT_URI=qemu:///system LIBVIRT_NETWORK_MODE=bridge \
LIBVIRT_POOL=default make simulation-libvirt-e2e-test
```

Do not combine `rootless` with `qemu:///system`. Do not use the rootless gate
for performance measurements. A netem result describes the configured guest
egress qdisc and functional UIB behavior, not physical switch congestion or
line-rate accuracy. Command override variables name one executable, not a
shell command with arguments. The gate's interrupt trap destroys only the
run's OpenTofu state and removes a pool only when the gate created it.
The lab configures a sticky node-local `/run/enroot` root so unattended Slurm
jobs do not depend on a transient interactive-login directory.

The congestion test creates a disposable network namespace, applies netem to
its loopback interface, runs `tests/loopback`, and destroys the namespace. It
never changes the host root qdisc. Its options are:

| Variable | Default | Syntax and values |
|---|---|---|
| `SIMULATION_CC_MATRIX` | Experimental RoCEv2 clean/datacenter/lossy NSCC, constrained `none`, and plugin cases; UET clean/datacenter/constrained/lossy/reordered NSCC, constrained `none`, and plugin cases | Space-separated `protocol:algorithm:condition` entries. Protocol is `rocev2` or `uet`; test algorithms are `nscc`, `none`, and `test-pass`; RoCEv2+NSCC entries are experimental only; condition is any checked-in network overlay such as `clean`, `datacenter`, `constrained`, `lossy`, `reordered`, or `ecn` |
| `SIMULATION_CC_TIMEOUT` | `60` | Positive timeout in seconds for each matrix entry |
| `SIMULATION_CC_MAX_WINDOW` | `65536` | NSCC maximum window in bytes |
| `SIMULATION_NETNS_PRIVILEGE` | Unset | Optional command prefix used only to execute `unshare --net` when root and unprivileged user namespaces are unavailable |
| `NETEM_TC` | `tc` or `/sbin/tc` | Exact `tc` executable used by the netem adapter |

For example:

```text
SIMULATION_CC_MATRIX='rocev2:nscc:ecn uet:none:lossy' \
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
| `RUNTIME_CONTAINER_ENGINE` | `docker` | Container engine used for the runtime init image |
| `PORTABLE_RUNTIME_CONTAINER_ENGINE` | `RUNTIME_CONTAINER_ENGINE` | Exact engine executable used for the pinned portable build |
| `PORTABLE_RUNTIME_BUILDER_IMAGE` | `uib-runtime-builder:test` | Local builder image name |
| `RUNTIME_IMAGE` | `uib-runtime:test` | Portable UIB init image name |
| `MODEL_CACHE_IMAGE` | `uib-model-cache:test` | Local image built from the ignored Hugging Face cache; for disposable simulations only |
| `MODEL_CACHE_CONTAINER_ENGINE` | `TRAINING_CONTAINER_ENGINE` | Container engine used to build the local model-cache image |
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
| `make ucx-libfabric-test` | UCX 1.18.1-compatible `ucx_info`/`ucx_perftest`, libfabric/fabtests 2.1.0-compatible tools, `timeout`, and permission to expose the character-device anchor | Actual two-process UCX tag, stream, UCP RMA, and UCP atomic traffic over `ud_verbs/uib0:1`; direct UCT active-message, write, read, fetch-add, and compare-swap traffic over `rc_verbs/uib0:1`; and data-checked libfabric untagged/tagged RDM, counter-completed RMA write/read, and 64-bit sum/compare-swap traffic over `verbs;ofi_rxd`. TCP, shared-memory, sockets, and native-verbs fallbacks fail the test |
| `make mpi-compat-test` | Open MPI 5.0.7-compatible `mpicc`/`mpirun` built with `pml/ucx` and `mtl/ofi`, the UCX/libfabric requirements above, and permission to expose the character-device anchor | The same two-rank MPI binary exchanges 1, 2051, and 65536-byte verified messages and runs an all-reduce through UIB, once through `pml/ucx` and once through `mtl/ofi`; fallback fails the test |

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
| `COMM_TEST_WARMUPS` | `10` | UCX warm-up iterations per operation |
| `COMM_TEST_TIMEOUT` | `30` | Timeout in seconds for each client or server |
| `COMM_TEST_BIND_PORT` | `19800` | UIB and UCX test port |
| `COMM_TEST_MESSAGE_SIZES` | `l:1,64,1024` | Fabtests `-S` size expression; the default checks three payload sizes |
| `COMM_TEST_UCX_TESTS` | `tag_lat stream_lat ucp_put_lat ucp_get ucp_fadd ucp_cswap` | Space-separated `ucx_perftest -t` matrix; each test must select `ud_verbs/uib0:1` |
| `COMM_TEST_UCT_TESTS` | `am_lat put_lat get fadd cswap` | Space-separated direct UCT matrix pinned to `rc_verbs/uib0:1`; `get` uses bcopy and the other tests use short layout |
| `COMM_TEST_LIBFABRIC_TESTS` | `fi_rdm_pingpong fi_rdm_tagged_pingpong fi_rdm_tagged_bw` | Space-separated fabtests matrix pinned to `verbs;ofi_rxd` and `uib0-dgram`; entries must accept the common iteration, size, verification, and out-of-band options |
| `COMM_TEST_LIBFABRIC_RMA_OPS` | `write read` | Space-separated `fi_rma_bw` operations; each uses `-t counter` and data-integrity checking; empty disables this matrix |
| `COMM_TEST_LIBFABRIC_ATOMIC_OPS` | `sum cswap` | Space-separated `fi_rdm_atomic` operations; empty disables this matrix |
| `COMM_TEST_LIBFABRIC_ATOMIC_DATATYPE` | `uint64` | Fabtests datatype shared by the configured atomic operations |

MPI options:

| Variable | Default | Meaning |
|---|---|---|
| `MPIEXEC` | `mpirun` | MPI launcher executable |
| `MPI_TEST_FRONTENDS` | `ucx ofi` | Space-separated matrix; valid entries are `ucx` and `ofi`; empty is rejected because it would test no UIB path |
| `MPI_TEST_TIMEOUT` | `45` | Timeout in seconds for each two-rank run |
| `MPI_TEST_BIND_PORT` | `19820` | UIB UDP bind port shared by rank-specific loopback addresses |

Examples:

```text
NCCL_TESTS_BIN=/opt/nccl-tests/build/all_reduce_perf \
NCCL_TEST_MAX_BYTES=1M make nccl-test

RCCL_TESTS_BIN=/opt/rccl-tests/build/all_reduce_perf \
RCCL_TEST_RANKS=4 HIP_VISIBLE_DEVICES=0,1,2,3 make rccl-test

COMM_TEST_ITERATIONS=1000 COMM_TEST_TIMEOUT=60 \
COMM_TEST_MESSAGE_SIZES=l:1,256,4096 \
make ucx-libfabric-test

COMM_TEST_UCX_TESTS= COMM_TEST_UCT_TESTS= \
COMM_TEST_LIBFABRIC_TESTS= \
COMM_TEST_LIBFABRIC_RMA_OPS=write \
COMM_TEST_LIBFABRIC_ATOMIC_OPS=cswap \
make ucx-libfabric-test

MPI_TEST_FRONTENDS=ucx MPI_TEST_TIMEOUT=60 make mpi-compat-test
```

`nccl-test` intentionally uses two ranks on one GPU through MPS. That is real
NCCL/UIB traffic, but not multi-GPU or multi-node validation. `rccl-test`
requires AMD hardware and was not run on the NVIDIA-only development host.
The UCX/libfabric and MPI gates passed locally; physical-node repetition
remains separate promotion work.

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
- The promoted virtme lifecycle may stop only recorded PIDs and delete
  recorded taps, bridges, overlays, and temporary roots. Its current renderer
  performs no host mutation.
- libvirt cleanup uses the run's OpenTofu state and `tofu destroy`.
- existing Kubernetes cleanup deletes only run-owned namespaced resources.
- Slurm cleanup cancels only recorded job IDs.
- shared Kubernetes operators, Slurm configuration, Pyxis, Enroot, GPU/RDMA
  operators, partitions, caches, and unrelated jobs are never removed.

Cleanup must remain safe after interruption during every lifecycle stage.

## Limitations

- The Kind backend passed a real one-control-plane, one-worker lifecycle test,
  including readiness, kubeconfig export, node-count validation, and cleanup.
  Kubeflow training resources render and pass structural tests. Operator
  installation, server-side CRD/admission validation, and an actual two-worker
  CPU TrainJob passed. The run uses Gloo and validates UIB packaging but not
  its data plane. A disposable libvirt controller/two-worker lab passed actual
  Slurm, NFS, cross-VM standard RC payload through UIB, Enroot, Pyxis,
  pinned-container, UIB library-preflight, and exact cleanup gates. The UIB
  gate uses a 2051-byte payload to exercise multi-packet RC transfers. The
  public lifecycle also passed remote controller submission, exact-job status,
  two-rank Pyxis execution with UIB preflight, result collection, and cleanup.
  That lifecycle request is an orchestration probe; the preceding unchanged
  verbs gate is the actual UIB payload evidence. A named netem condition also
  passed on both worker data interfaces with qdisc/version collection while
  that cross-VM payload gate was active. The libvirt Kubernetes lifecycle,
  pinned k3s/containerd provisioning, dedicated data interface, kubeconfig,
  evidence, and cleanup paths pass mock lifecycle tests, but could not run
  against the password-protected host libvirt daemon. GPU/NCCL Kubernetes
  execution, host-side tap shaping, DeepSpeed, and Megatron-Core remain
  planned.
- The diagnostic image passed its hardware-neutral, UIB runtime-injection,
  CUDA `deviceQuery`, and single-GPU NCCL all-reduce gates. Its NCCL run has
  one rank and therefore does not send UIB traffic.
- UIB's versioned NCCL loader ABI and a two-rank, host-staged NCCL all-reduce
  are covered by repository tests. The actual collective test is opt-in
  because it requires CUDA, NCCL, MPI, MPS, a GPU, and UTS namespace access.
- UCX tag, stream, UCP RMA, and UCP atomic tests over `ud_verbs`; direct UCT
  active-message, RDMA-write, RDMA-read, fetch-add, and compare-swap tests over
  `rc_verbs`; and libfabric untagged/tagged RDM, counter-completed RMA
  write/read, and 64-bit sum/compare-swap traffic over `verbs;ofi_rxd` passed
  the local CPU gate. Open MPI also passed verified point-to-point and
  collective traffic through each front end. This does not claim direct
  DGRAM, MSG/RxM, or physical-node qualification. RCCL remains unvalidated
  until AMD hardware is available.
- The common congestion-control registry is implemented for RoCEv2 and UET.
  The local matrix covers NSCC, `none`, a registered plug-in, constrained
  pending-send admission, deterministic retry loss, and named netem profiles.
  NSCC is native to UET and its RoCEv2 cases are experimental ABI research,
  not a RoCEv2 recommendation. Physical ECN/CNP behavior remains a hardware
  promotion gate.
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
- [Kubeflow Trainer runtime guide](https://trainer.kubeflow.org/en/latest/operator-guides/runtime/)
- [Kubeflow Trainer v2.2.1](https://github.com/kubeflow/trainer/releases/tag/v2.2.1)
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
