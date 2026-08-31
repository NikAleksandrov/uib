# SPDX-License-Identifier: Apache-2.0

CC      ?= cc
AR      ?= ar
MPICC   ?= mpicc
PKG_CONFIG ?= pkg-config
PREFIX  ?= /usr/local
MANDIR  ?= $(PREFIX)/share/man
DOCDIR  ?= $(PREFIX)/share/doc/uib

-include config.mk

CFLAGS  ?= -O2 -g -Wall -Wextra -Wno-unused-parameter
CFLAGS  += -Werror=declaration-after-statement
CFLAGS  += -Iinclude -fPIC -pthread -D_GNU_SOURCE -MMD -MP

URING_CFLAGS ?= $(if $(wildcard ../liburing/src/include/liburing.h),-I../liburing/src/include)
URING_LIBS ?= $(if $(wildcard /usr/lib/x86_64-linux-gnu/liburing.so),-luring,-l:liburing.so.2)
MNL_CFLAGS ?= $(shell pkg-config --cflags libmnl 2>/dev/null)
MNL_LIBS ?= $(shell pkg-config --libs libmnl 2>/dev/null)
URCU_CFLAGS ?= $(shell pkg-config --cflags liburcu-bp 2>/dev/null)
URCU_LIBS ?= $(shell pkg-config --libs liburcu-bp 2>/dev/null)
HAVE_ZLIB ?= $(shell pkg-config --exists zlib 2>/dev/null && echo 1 || echo 0)
ZLIB_CFLAGS ?= $(shell pkg-config --cflags zlib 2>/dev/null)
ZLIB_LIBS ?= $(shell pkg-config --libs zlib 2>/dev/null)
ifeq ($(strip $(MNL_LIBS)),)
  $(error libmnl development files are required)
endif
ifeq ($(strip $(URCU_LIBS)),)
  $(error liburcu-bp development files are required)
endif
HAVE_IBVERBS ?= $(shell printf '#include <infiniband/verbs.h>\n' | \
	$(CC) -x c -fsyntax-only - >/dev/null 2>&1 && echo 1 || echo 0)
HAVE_IBV_REG_MR_EX ?= $(shell printf '#include <infiniband/verbs.h>\nstruct ibv_mr_init_attr attr;\nint x = IBV_REG_MR_MASK_ADDR;\n' | \
	$(CC) $(IBVERBS_CFLAGS) -x c -fsyntax-only - >/dev/null 2>&1 && \
	echo 1 || echo 0)
HAVE_MPI ?= $(shell command -v $(MPICC) >/dev/null 2>&1 && echo 1 || echo 0)
ENABLE_PROVIDER ?= $(HAVE_IBVERBS)
HAVE_VIRTME ?= $(shell command -v vng >/dev/null 2>&1 && echo 1 || echo 0)
HAVE_IBV_TOOLS ?= $(shell command -v ibv_rc_pingpong >/dev/null 2>&1 && echo 1 || echo 0)
HAVE_DMABUF ?= $(shell printf '#include <linux/dma-buf.h>\nint x=DMA_BUF_IOCTL_SYNC;\n' | \
	$(CC) -x c -fsyntax-only - >/dev/null 2>&1 && echo 1 || echo 0)
HAVE_ZEROCOPY ?= $(shell printf '#include <sys/socket.h>\n#include <linux/errqueue.h>\nint x=MSG_ZEROCOPY+SO_ZEROCOPY+SO_EE_ORIGIN_ZEROCOPY;\n' | \
	$(CC) -D_GNU_SOURCE -x c -fsyntax-only - >/dev/null 2>&1 && echo 1 || echo 0)
HAVE_URING_ZCRX ?= $(shell printf '#include <liburing.h>\nint x=IORING_OP_RECV_ZC+IORING_REGISTER_ZCRX_IFQ;\n' | \
	$(CC) $(URING_CFLAGS) -x c -fsyntax-only - >/dev/null 2>&1 && echo 1 || echo 0)

CFLAGS += $(URING_CFLAGS) $(MNL_CFLAGS) $(URCU_CFLAGS) $(IBVERBS_CFLAGS)
LDFLAGS += -pthread
LDLIBS += $(URING_LIBS) $(MNL_LIBS) $(URCU_LIBS)

ifeq ($(HAVE_ZLIB),1)
  CFLAGS += -DUIB_HAVE_ZLIB $(ZLIB_CFLAGS)
  LDLIBS += $(ZLIB_LIBS)
endif

ifeq ($(HAVE_IBV_REG_MR_EX),1)
  CFLAGS += -DUIB_HAVE_IBV_REG_MR_EX
endif

# AF_XDP backend (udp-zc-afxdp) is compiled with real libxdp glue only when
# libxdp/libbpf are available; otherwise transport_afxdp.c compiles to a stub
# whose probe() returns 0 and the dispatcher falls back to udp-io-uring.
HAVE_AFXDP ?= $(shell pkg-config --exists libxdp libbpf 2>/dev/null && echo 1 || echo 0)
ifeq ($(HAVE_AFXDP),1)
  AFXDP_CFLAGS ?= $(shell pkg-config --cflags libxdp libbpf)
  AFXDP_LIBS ?= $(shell pkg-config --libs libxdp libbpf)
  CFLAGS += -DUIB_HAVE_AFXDP $(AFXDP_CFLAGS)
  LDLIBS += $(AFXDP_LIBS)
endif

BPF_CLANG ?= clang
BPF_ARCH ?= $(shell uname -m | sed -e 's/x86_64/x86/' -e 's/aarch64/arm64/')
BPF_CFLAGS ?= -O2 -g -target bpf -D__TARGET_ARCH_$(BPF_ARCH) \
	-I/usr/include/$(shell uname -m)-linux-gnu
AFXDP_BPF_OBJ := bpf/uib_afxdp_kern.o

# devmem TCP RX (tcp-devmem C2) needs the SO_DEVMEM uapi. When present the
# tcp-devmem backend can compile its dma-buf-to-queue binding + devmem RX path.
HAVE_DEVMEM ?= $(shell printf 'int x=SO_DEVMEM_DMABUF;\n' | $(CC) \
	-D_GNU_SOURCE -include sys/socket.h -x c -fsyntax-only - \
	>/dev/null 2>&1 && echo 1 || echo 0)
ifeq ($(HAVE_DEVMEM),1)
  CFLAGS += -DUIB_HAVE_DEVMEM
endif

ifeq ($(HAVE_DMABUF),1)
  CFLAGS += -DUIB_HAVE_DMABUF
endif

ifeq ($(HAVE_ZEROCOPY),1)
  CFLAGS += -DUIB_HAVE_ZEROCOPY
endif

ifeq ($(HAVE_URING_ZCRX),1)
  CFLAGS += -DUIB_HAVE_URING_ZCRX
endif

SRCS := \
	src/bth.c \
	src/pd.c \
	src/mr.c \
	src/cq.c \
	src/utils.c \
	src/psn_index.c \
	src/qp.c \
	src/protocol.c \
	src/protocol_rocev2.c \
	src/protocol_rocev2_tx.c \
	src/protocol_rocev2_rx.c \
	src/protocol_rocev2_wire.c \
	src/protocol_dummy.c \
	src/protocol_uet.c \
	src/congestion.c \
	src/dcqcn.c \
	src/uet_rccc.c \
	src/uet_nscc.c \
	src/context.c \
	src/observability.c \
	src/transport.c \
	src/zerocopy.c \
	src/host_zc.c \
	src/uring_zcrx.c \
	src/transport_udp.c \
	src/transport_afxdp.c \
	src/transport_tcp_devmem.c \
	src/afxdp_frame.c \
	src/devmem.c \
	src/verbs.c

OBJS := $(SRCS:.c=.o)

PROV_SRCS := src/provider.c
PROV_OBJS := $(PROV_SRCS:.c=.o)
PROV_MAP := src/libuibv.map

LIB_STATIC   := libuib.a
LIB_STANDALONE := libuib-standalone.a
LIB_STANDALONE_UIB = .libuib-standalone-uib-$(STANDALONE_AFXDP).a
LIB_SHARED   := libuib.so
LIB_SHARED_SONAME := libuib.so.0
LIB_SHARED_REAL := libuib.so.0.1.0
LIB_MAP      := src/libuib.map
LIB_PROVIDER := libuibv.so
STANDALONE_BUILDER := scripts/build-standalone-archive.sh
STATIC_LINK_TEST := tests/static_link_test
RUNTIME_BUNDLE_TEST := tests/run_runtime_bundle_test.sh
PORTABLE_RUNTIME_BUNDLE_TEST := tests/run_portable_runtime_bundle_test.sh
DISTRIBUTION_COMPAT_TEST := tests/run_distribution_compat_test.sh
UPSTREAM_COMPAT_TEST := scripts/run-upstream-compatibility.sh
RUNTIME_ARTIFACT_DIR ?= simulation/.artifacts
RUNTIME_IMAGE ?= uib-runtime:test
RUNTIME_CONTAINER_ENGINE ?= docker
PORTABLE_RUNTIME_BUILDER_IMAGE ?= uib-runtime-builder:test
PORTABLE_RUNTIME_CONTAINER_ENGINE ?= $(RUNTIME_CONTAINER_ENGINE)
MODEL_CACHE_IMAGE ?= uib-model-cache:test
MODEL_CACHE_CONTAINER_ENGINE ?= $(TRAINING_CONTAINER_ENGINE)
TRAINING_IMAGE ?= uib-training:test
TRAINING_CONTAINER_ENGINE ?= docker
TRAINING_GPU_DEVICE ?= nvidia.com/gpu=all
TRAINING_GPU_PROCESSES ?= 1
TRAINING_GPU_STRATEGY ?= fsdp
TRAINING_IMAGE_TEST := tests/run_training_smoke_test.sh
TRAINING_UIB_TEST := tests/run_training_uib_test.sh
INFERENCE_IMAGE ?= uib-inference:test
INFERENCE_CONTAINER_ENGINE ?= $(TRAINING_CONTAINER_ENGINE)
INFERENCE_GPU_DEVICE ?= $(TRAINING_GPU_DEVICE)
INFERENCE_GPU_TENSOR_PARALLEL ?= 1
INFERENCE_GPU_PIPELINE_PARALLEL ?= 1
INFERENCE_GPU_DATA_PARALLEL ?= 1
INFERENCE_IMAGE_TEST := tests/run_inference_smoke_test.sh
DIAGNOSTIC_IMAGE ?= uib-diagnostics:test
DIAGNOSTIC_CONTAINER_ENGINE ?= $(TRAINING_CONTAINER_ENGINE)
DIAGNOSTIC_GPU_DEVICE ?= $(TRAINING_GPU_DEVICE)
DIAGNOSTIC_DOWNLOAD_CACHE ?= $(RUNTIME_ARTIFACT_DIR)/downloads
DIAGNOSTIC_IMAGE_TEST := tests/run_diagnostic_image_test.sh

STANDALONE_AFXDP ?= 0
ifneq ($(filter $(STANDALONE_AFXDP),0 1),$(STANDALONE_AFXDP))
  $(error STANDALONE_AFXDP must be 0 or 1)
endif
STANDALONE_CFLAGS_0 := $(filter-out -DUIB_HAVE_AFXDP,$(CFLAGS))
STANDALONE_CFLAGS_1 := $(CFLAGS)
STATIC_PACKAGES := liburing libmnl liburcu-bp
ifeq ($(HAVE_ZLIB),1)
  STATIC_PACKAGES += zlib
endif
ifeq ($(STANDALONE_AFXDP),1)
  STATIC_PACKAGES += libxdp
endif
STATIC_LDLIBS = $(shell $(PKG_CONFIG) --libs --static \
	$(STATIC_PACKAGES) 2>/dev/null)
STANDALONE_OBJS_0 := $(SRCS:.c=.standalone-0.o)
STANDALONE_OBJS_1 := $(SRCS:.c=.standalone-1.o)
STANDALONE_OBJS = $(STANDALONE_OBJS_$(STANDALONE_AFXDP))
STATIC_LINK_TEST_DEP := tests/static_link_test.d
DEPS := $(OBJS:.o=.d) $(PROV_OBJS:.o=.d) \
	$(STANDALONE_OBJS_0:.o=.d) $(STANDALONE_OBJS_1:.o=.d) \
	$(STATIC_LINK_TEST_DEP)
DOC_GENERATOR := scripts/generate-docs.py
MAN_PAGE      := man/uib.7

LIB_TARGETS := $(LIB_STATIC) $(LIB_SHARED)
ifeq ($(ENABLE_PROVIDER),1)
  LIB_TARGETS += $(LIB_PROVIDER)
endif

all: $(LIB_TARGETS)

static: $(LIB_STANDALONE)

FORCE:

static-test: $(STATIC_LINK_TEST)
	./$(STATIC_LINK_TEST)
	@if readelf -d $(STATIC_LINK_TEST) 2>&1 | grep -q NEEDED; then \
		echo "static link test has dynamic dependencies" >&2; \
		readelf -d $(STATIC_LINK_TEST) >&2; \
		exit 1; \
	fi

runtime-bundle: all static
	simulation/runtime/build-bundle.py --no-build --replace \
		--output $(RUNTIME_ARTIFACT_DIR)/uib-runtime \
		--archive $(RUNTIME_ARTIFACT_DIR)/uib-runtime.tar.gz

runtime-bundle-portable:
	RUNTIME_ARTIFACT_DIR='$(abspath $(RUNTIME_ARTIFACT_DIR))' \
	PORTABLE_RUNTIME_CONTAINER_ENGINE='$(PORTABLE_RUNTIME_CONTAINER_ENGINE)' \
	PORTABLE_RUNTIME_BUILDER_IMAGE='$(PORTABLE_RUNTIME_BUILDER_IMAGE)' \
		simulation/runtime/build-portable

runtime-bundle-portable-test: runtime-bundle-portable
	RUNTIME_ARTIFACT_DIR='$(abspath $(RUNTIME_ARTIFACT_DIR))' \
		$(PORTABLE_RUNTIME_BUNDLE_TEST)

runtime-bundle-test: all static
	$(RUNTIME_BUNDLE_TEST)

distribution-compat-test:
	UIB_TEST_JOBS='$(if $(UIB_TEST_JOBS),$(UIB_TEST_JOBS),4)' \
		$(DISTRIBUTION_COMPAT_TEST)

upstream-compat-test:
	UIB_TEST_JOBS='$(if $(UIB_TEST_JOBS),$(UIB_TEST_JOBS),2)' \
		$(UPSTREAM_COMPAT_TEST)

runtime-image: runtime-bundle-portable
	$(RUNTIME_CONTAINER_ENGINE) build --progress=plain \
		-f simulation/runtime/Containerfile -t $(RUNTIME_IMAGE) \
		$(RUNTIME_ARTIFACT_DIR)

model-cache-image: inference-smoke-test
	@test -d $(RUNTIME_ARTIFACT_DIR)/huggingface/hub || { \
		echo "model cache is empty; run make inference-smoke-test first" >&2; \
		exit 1; \
	}
	$(MODEL_CACHE_CONTAINER_ENGINE) build --progress=plain \
		-f simulation/cache/Containerfile -t $(MODEL_CACHE_IMAGE) \
		$(RUNTIME_ARTIFACT_DIR)

training-image:
	$(TRAINING_CONTAINER_ENGINE) build --progress=plain \
		-f simulation/training/Containerfile -t $(TRAINING_IMAGE) \
		simulation/training

training-smoke-test:
	TRAINING_IMAGE=$(TRAINING_IMAGE) \
	TRAINING_CONTAINER_ENGINE='$(TRAINING_CONTAINER_ENGINE)' \
		$(TRAINING_IMAGE_TEST)

training-public-test:
	TRAINING_PUBLIC_TEST=1 TRAINING_IMAGE=$(TRAINING_IMAGE) \
	TRAINING_CONTAINER_ENGINE='$(TRAINING_CONTAINER_ENGINE)' \
		$(TRAINING_IMAGE_TEST)

training-production-cpu-test:
	TRAINING_PRODUCTION_CPU_TEST=1 TRAINING_IMAGE=$(TRAINING_IMAGE) \
	TRAINING_CONTAINER_ENGINE='$(TRAINING_CONTAINER_ENGINE)' \
		$(TRAINING_IMAGE_TEST)

training-gpu-test:
	TRAINING_GPU_TEST=1 \
	TRAINING_GPU_DEVICE='$(TRAINING_GPU_DEVICE)' \
	TRAINING_GPU_PROCESSES='$(TRAINING_GPU_PROCESSES)' \
	TRAINING_GPU_STRATEGY='$(TRAINING_GPU_STRATEGY)' \
	TRAINING_IMAGE=$(TRAINING_IMAGE) \
	TRAINING_CONTAINER_ENGINE='$(TRAINING_CONTAINER_ENGINE)' \
		$(TRAINING_IMAGE_TEST)

training-uib-preflight-test: runtime-bundle
	TRAINING_SKIP_BUILD=1 \
	TRAINING_UIB_RUNTIME='$(CURDIR)/$(RUNTIME_ARTIFACT_DIR)/uib-runtime' \
	TRAINING_IMAGE=$(TRAINING_IMAGE) \
	TRAINING_CONTAINER_ENGINE='$(TRAINING_CONTAINER_ENGINE)' \
		$(TRAINING_IMAGE_TEST)

training-uib-test: runtime-bundle training-image
	TRAINING_UIB_RUNTIME='$(CURDIR)/$(RUNTIME_ARTIFACT_DIR)/uib-runtime' \
	TRAINING_GPU_DEVICE='$(TRAINING_GPU_DEVICE)' \
	TRAINING_IMAGE=$(TRAINING_IMAGE) \
	TRAINING_CONTAINER_ENGINE='$(TRAINING_CONTAINER_ENGINE)' \
		$(TRAINING_UIB_TEST)

inference-image:
	$(INFERENCE_CONTAINER_ENGINE) build --progress=plain \
		-f simulation/inference/Containerfile -t $(INFERENCE_IMAGE) \
		simulation/inference

inference-smoke-test:
	INFERENCE_IMAGE=$(INFERENCE_IMAGE) \
	INFERENCE_CONTAINER_ENGINE='$(INFERENCE_CONTAINER_ENGINE)' \
		$(INFERENCE_IMAGE_TEST)

inference-production-cpu-test:
	INFERENCE_PRODUCTION_CPU_TEST=1 INFERENCE_IMAGE=$(INFERENCE_IMAGE) \
	INFERENCE_CONTAINER_ENGINE='$(INFERENCE_CONTAINER_ENGINE)' \
		$(INFERENCE_IMAGE_TEST)

inference-gpu-test:
	INFERENCE_GPU_TEST=1 \
	INFERENCE_GPU_DEVICE='$(INFERENCE_GPU_DEVICE)' \
	INFERENCE_GPU_TENSOR_PARALLEL='$(INFERENCE_GPU_TENSOR_PARALLEL)' \
	INFERENCE_GPU_PIPELINE_PARALLEL='$(INFERENCE_GPU_PIPELINE_PARALLEL)' \
	INFERENCE_GPU_DATA_PARALLEL='$(INFERENCE_GPU_DATA_PARALLEL)' \
	INFERENCE_IMAGE=$(INFERENCE_IMAGE) \
	INFERENCE_CONTAINER_ENGINE='$(INFERENCE_CONTAINER_ENGINE)' \
		$(INFERENCE_IMAGE_TEST)

diagnostic-sources:
	DIAGNOSTIC_DOWNLOAD_CACHE='$(CURDIR)/$(DIAGNOSTIC_DOWNLOAD_CACHE)' \
		simulation/diagnostics/fetch-sources

diagnostic-image: diagnostic-sources
	$(DIAGNOSTIC_CONTAINER_ENGINE) build --progress=plain \
		--build-context \
			diagnostic-sources='$(CURDIR)/$(DIAGNOSTIC_DOWNLOAD_CACHE)' \
		-f simulation/diagnostics/Containerfile -t $(DIAGNOSTIC_IMAGE) \
		simulation/diagnostics

diagnostic-smoke-test:
	DIAGNOSTIC_IMAGE=$(DIAGNOSTIC_IMAGE) \
	DIAGNOSTIC_DOWNLOAD_CACHE='$(CURDIR)/$(DIAGNOSTIC_DOWNLOAD_CACHE)' \
	DIAGNOSTIC_CONTAINER_ENGINE='$(DIAGNOSTIC_CONTAINER_ENGINE)' \
		$(DIAGNOSTIC_IMAGE_TEST)

diagnostic-uib-preflight-test: runtime-bundle diagnostic-image
	DIAGNOSTIC_SKIP_BUILD=1 \
	DIAGNOSTIC_UIB_RUNTIME='$(CURDIR)/$(RUNTIME_ARTIFACT_DIR)/uib-runtime' \
	DIAGNOSTIC_IMAGE=$(DIAGNOSTIC_IMAGE) \
	DIAGNOSTIC_CONTAINER_ENGINE='$(DIAGNOSTIC_CONTAINER_ENGINE)' \
		$(DIAGNOSTIC_IMAGE_TEST)

diagnostic-gpu-test: diagnostic-image
	DIAGNOSTIC_SKIP_BUILD=1 DIAGNOSTIC_GPU_TEST=1 \
	DIAGNOSTIC_GPU_DEVICE='$(DIAGNOSTIC_GPU_DEVICE)' \
	DIAGNOSTIC_IMAGE=$(DIAGNOSTIC_IMAGE) \
	DIAGNOSTIC_CONTAINER_ENGINE='$(DIAGNOSTIC_CONTAINER_ENGINE)' \
		$(DIAGNOSTIC_IMAGE_TEST)

simulation-cli-test:
	./tests/run_simulation_cli_test.sh

simulation-netem-test:
	./tests/run_netem_config_test.sh

simulation-fault-schedule-test:
	./tests/run_fault_schedule_test.sh

simulation-rank-fault-test:
	./tests/run_rank_schedule_test.sh

simulation-fault-e2e-test: all $(TEST_BIN)
	./tests/run_fault_schedule_e2e_test.sh

simulation-kind-test:
	./tests/run_kind_backend_test.sh

simulation-virtme-backend-test:
	./tests/run_virtme_backend_test.sh

simulation-lifecycle-interruption-test:
	./tests/run_lifecycle_interruption_test.sh

simulation-workload-interruption-test:
	./tests/run_workload_interruption_test.sh

simulation-virtme-kubernetes-e2e-test:
	./tests/run_virtme_kubernetes_e2e_test.sh

simulation-virtme-slurm-e2e-test: runtime-bundle-portable-test
	./tests/run_virtme_slurm_e2e_test.sh

simulation-virtme-netem-e2e-test: runtime-bundle-portable-test
	VIRTME_NETWORK_CONDITION=datacenter \
		./tests/run_virtme_slurm_e2e_test.sh

simulation-virtme-training-e2e-test: runtime-bundle-portable-test
	@$(TRAINING_CONTAINER_ENGINE) image inspect $(TRAINING_IMAGE) >/dev/null 2>&1 || \
		$(MAKE) training-image
	VIRTME_SLURM_WORKLOAD=training \
	VIRTME_SLURM_IMAGE='$(TRAINING_IMAGE)' \
	CONTAINER_ENGINE_COMMAND='$(TRAINING_CONTAINER_ENGINE)' \
		./tests/run_virtme_slurm_e2e_test.sh

simulation-virtme-inference-e2e-test: runtime-bundle-portable-test inference-smoke-test
	VIRTME_SLURM_WORKLOAD=inference \
	VIRTME_SLURM_IMAGE='$(INFERENCE_IMAGE)' \
	CONTAINER_ENGINE_COMMAND='$(INFERENCE_CONTAINER_ENGINE)' \
		./tests/run_virtme_slurm_e2e_test.sh

simulation-kind-e2e-test:
	./tests/run_kind_e2e_test.sh

simulation-kubernetes-render-test:
	./tests/run_kubernetes_render_test.sh

simulation-kubernetes-lifecycle-test:
	./tests/run_kubernetes_lifecycle_test.sh

simulation-transport-evidence-test:
	./tests/run_transport_evidence_test.sh

simulation-provider-counter-evidence-test:
	./tests/run_provider_counter_evidence_test.sh

simulation-workload-metrics-test:
	./tests/run_workload_metrics_test.sh

simulation-slurm-render-test:
	./tests/run_slurm_render_test.sh

simulation-slurm-lifecycle-test:
	./tests/run_slurm_lifecycle_test.sh

simulation-slurm-collect-test:
	./tests/run_slurm_collect_test.sh

simulation-libvirt-test:
	./tests/run_libvirt_backend_test.sh

simulation-libvirt-e2e-test: runtime-bundle-portable-test
	./tests/run_libvirt_slurm_e2e_test.sh

simulation-libvirt-netem-e2e-test: runtime-bundle-portable-test
	LIBVIRT_NETWORK_CONDITION="$${LIBVIRT_NETWORK_CONDITION:-datacenter}" \
		./tests/run_libvirt_slurm_e2e_test.sh

simulation-libvirt-training-e2e-test: runtime-bundle-portable-test
	@$(TRAINING_CONTAINER_ENGINE) image inspect $(TRAINING_IMAGE) >/dev/null 2>&1 || \
		$(MAKE) training-image
	LIBVIRT_E2E_WORKLOAD=training LIBVIRT_E2E_IMAGE='$(TRAINING_IMAGE)' \
	LIBVIRT_DISK=32Gi CONTAINER_ENGINE_COMMAND='$(TRAINING_CONTAINER_ENGINE)' \
		./tests/run_libvirt_slurm_e2e_test.sh

simulation-libvirt-inference-e2e-test: runtime-bundle-portable-test
	@$(INFERENCE_CONTAINER_ENGINE) image inspect $(INFERENCE_IMAGE) >/dev/null 2>&1 || \
		$(MAKE) inference-image
	LIBVIRT_E2E_WORKLOAD=inference LIBVIRT_E2E_IMAGE='$(INFERENCE_IMAGE)' \
	LIBVIRT_MEMORY=8Gi LIBVIRT_DISK=64Gi \
	CONTAINER_ENGINE_COMMAND='$(INFERENCE_CONTAINER_ENGINE)' \
		./tests/run_libvirt_slurm_e2e_test.sh

simulation-kubeflow-e2e-test:
	./tests/run_kubeflow_e2e_test.sh

simulation-virtme-kubeflow-e2e-test: runtime-image training-image
	SIMULATION_KUBERNETES_PROFILE=virtme-kubernetes \
	SIMULATION_KUBERNETES_CPU=2 \
	SIMULATION_KUBERNETES_MEMORY=2Gi \
	KUBECTL_COMMAND='$(CURDIR)/simulation/.artifacts/tools/bin/kubectl' \
	HELM_COMMAND='$(CURDIR)/simulation/.artifacts/tools/bin/helm' \
		./tests/run_kubeflow_e2e_test.sh

simulation-kubernetes-uib-test: runtime-image diagnostic-image
	SIMULATION_KUBERNETES_UIB_RUNTIME_IMAGE=$(RUNTIME_IMAGE) \
	SIMULATION_KUBERNETES_UIB_DIAGNOSTIC_IMAGE=$(DIAGNOSTIC_IMAGE) \
		./tests/run_kubernetes_uib_test.sh

simulation-kuberay-e2e-test: simulation-kubernetes-uib-test \
	runtime-image model-cache-image
	SIMULATION_KUBERAY_RUNTIME_IMAGE=$(RUNTIME_IMAGE) \
	SIMULATION_KUBERAY_INFERENCE_IMAGE=$(INFERENCE_IMAGE) \
	SIMULATION_KUBERAY_MODEL_CACHE_IMAGE=$(MODEL_CACHE_IMAGE) \
		./tests/run_kuberay_e2e_test.sh

VIRTME_KUBERAY_ENV = SIMULATION_KUBERNETES_PROFILE=virtme-kubernetes \
	SIMULATION_KUBERNETES_CPU=2 \
	SIMULATION_KUBERNETES_MEMORY=3Gi \
	SIMULATION_KUBERNETES_WORKLOAD_MEMORY=2Gi \
	SIMULATION_VIRTME_CONTROLLER_MEMORY=2Gi \
	SIMULATION_VIRTME_WORKER_MEMORY=3Gi \
	VIRTME_SSH_WAIT=180

simulation-virtme-kuberay-e2e-test:
	@set +e; \
	$(VIRTME_KUBERAY_ENV) ./tests/run_kuberay_e2e_test.sh \
		--preflight-only; status=$$?; \
	set -e; \
	if test "$$status" = 77; then exit 0; fi; \
	if test "$$status" -ne 0; then exit "$$status"; fi; \
	$(MAKE) runtime-image inference-image model-cache-image; \
	$(VIRTME_KUBERAY_ENV) \
	SIMULATION_KUBERAY_RUNTIME_IMAGE=$(RUNTIME_IMAGE) \
	SIMULATION_KUBERAY_INFERENCE_IMAGE=$(INFERENCE_IMAGE) \
	SIMULATION_KUBERAY_MODEL_CACHE_IMAGE=$(MODEL_CACHE_IMAGE) \
	KUBECTL_COMMAND='$(CURDIR)/simulation/.artifacts/tools/bin/kubectl' \
	HELM_COMMAND='$(CURDIR)/simulation/.artifacts/tools/bin/helm' \
		./tests/run_kuberay_e2e_test.sh

docs:
	python3 $(DOC_GENERATOR)

license-check:
	./scripts/check-licenses.sh

docs-check:
	python3 $(DOC_GENERATOR) --check

examples: all
	$(MAKE) -C examples HAVE_IBVERBS=$(HAVE_IBVERBS) \
		IBVERBS_CFLAGS='$(IBVERBS_CFLAGS)' IBVERBS_LIBS='$(IBVERBS_LIBS)'

shared-abi-test: $(LIB_SHARED)
	./tests/run_shared_abi_test.sh

$(LIB_STATIC): $(OBJS)
	$(AR) rcs $@ $^

$(LIB_STANDALONE_UIB): $(STANDALONE_OBJS)
	$(AR) rcs $@ $^

$(LIB_STANDALONE): $(LIB_STANDALONE_UIB) $(STANDALONE_BUILDER) Makefile FORCE
	@$(PKG_CONFIG) --exists $(STATIC_PACKAGES) || { \
		echo "static development packages required: $(STATIC_PACKAGES)" >&2; \
		exit 1; \
	}
	$(STANDALONE_BUILDER) $@ $(LIB_STANDALONE_UIB) $(AR) $(CC) \
		$(STATIC_LDLIBS)

$(STATIC_LINK_TEST): tests/static_link_test.c $(LIB_STANDALONE) Makefile
	$(CC) $(CFLAGS) -static -o $@ $< $(LIB_STANDALONE)

$(LIB_SHARED): $(LIB_SHARED_SONAME)
	ln -sfn $(LIB_SHARED_SONAME) $@

$(LIB_SHARED_SONAME): $(LIB_SHARED_REAL)
	ln -sfn $(LIB_SHARED_REAL) $@

$(LIB_SHARED_REAL): $(OBJS) $(LIB_MAP)
	$(CC) -shared -Wl,-soname,$(LIB_SHARED_SONAME) -Wl,--version-script=$(LIB_MAP) -o $@ $(OBJS) $(LDFLAGS) $(LDLIBS)

$(LIB_PROVIDER): $(PROV_OBJS) $(LIB_SHARED) $(PROV_MAP)
	$(CC) -shared -o $@ $(PROV_OBJS) -L. -Wl,-rpath,'$$ORIGIN' \
		-Wl,--version-script=$(PROV_MAP) -luib -ldl $(LDFLAGS) $(LDLIBS)

src/%.o: src/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

src/%.standalone-0.o: src/%.c
	$(CC) $(STANDALONE_CFLAGS_0) -c -o $@ $<

src/%.standalone-1.o: src/%.c
	$(CC) $(STANDALONE_CFLAGS_1) -c -o $@ $<

TEST_BIN    := tests/loopback
FRAME_TEST  := tests/afxdp_frame_test
DMABUF_TEST := tests/dmabuf_mr_test
BTH_TEST    := tests/bth_test
RXE_TEST    := tests/rxe_interop
PROTOCOL_TEST := tests/protocol_test
UET_TEST     := tests/uet_test
NSCC_TEST    := tests/nscc_test
CONGESTION_TEST := tests/congestion_test
ROCEV2_ECN_TEST := tests/rocev2_ecn_test
CONGESTION_PLUGIN := tests/congestion_plugin.so
ZEROCOPY_TEST := tests/zerocopy_test
PROVIDER_DMABUF_TEST := tests/provider_dmabuf_test
PROVIDER_NCCL_ABI_TEST := tests/provider_nccl_abi_test
MPI_COMPAT_TEST := tests/mpi_compat_test
EXTERNAL_DMABUF_TEST := tests/external_dmabuf_test
CQ_EVENT_TEST := tests/cq_event_test
URING_ASYNC_TEST := tests/uring_async_test
PTR_RING_TEST := tests/ptr_ring_test
PROVIDER_CQ_EVENT_TEST := tests/provider_cq_event_test
REORDER_TEST := tests/reorder_test
UET_CONCURRENT_POST_TEST := tests/uet_concurrent_post_test
TRANSPORT_TEST := tests/transport_test
QP_SCALE_TEST := tests/qp_scale_test
QP_LOOKUP_BENCH := tests/qp_lookup_bench
MR_SCALE_TEST := tests/mr_scale_test
MR_LIFETIME_TEST := tests/mr_lifetime_test
PROVIDER_DISPATCH_BENCH := tests/provider_dispatch_bench
PROVIDER_WR_BENCH := tests/provider_wr_bench
PSN_INDEX_TEST := tests/psn_index_test
HOST_ZC_TEST := tests/host_zc_test
HOST_ZC_E2E_TEST := tests/host_zc_e2e_test
HOST_ZC_BENCH := tests/host_zc_bench
ifeq ($(HAVE_IBVERBS),1)
SCALING_PROVIDER_BENCH := $(PROVIDER_DISPATCH_BENCH)
SCALING_PROVIDER_WR_BENCH := $(PROVIDER_WR_BENCH)
NCCL_PROVIDER_TEST := $(PROVIDER_NCCL_ABI_TEST)
endif

DEPS += $(TEST_BIN).d $(FRAME_TEST).d $(DMABUF_TEST).d \
	$(BTH_TEST).d $(RXE_TEST).d $(PROTOCOL_TEST).d $(UET_TEST).d \
	$(NSCC_TEST).d $(CONGESTION_TEST).d tests/congestion_plugin.d
DEPS += $(ROCEV2_ECN_TEST).d

DEPS += $(ZEROCOPY_TEST).d $(PROVIDER_DMABUF_TEST).d \
	$(PROVIDER_NCCL_ABI_TEST).d $(EXTERNAL_DMABUF_TEST).d
DEPS += $(MPI_COMPAT_TEST).d

DEPS += $(CQ_EVENT_TEST).d $(URING_ASYNC_TEST).d $(PTR_RING_TEST).d
DEPS += $(PROVIDER_CQ_EVENT_TEST).d
DEPS += $(REORDER_TEST).d
DEPS += $(UET_CONCURRENT_POST_TEST).d
DEPS += $(TRANSPORT_TEST).d
DEPS += $(QP_SCALE_TEST).d
DEPS += $(QP_LOOKUP_BENCH).d
DEPS += $(MR_SCALE_TEST).d
DEPS += $(MR_LIFETIME_TEST).d
DEPS += $(PROVIDER_DISPATCH_BENCH).d
DEPS += $(PROVIDER_WR_BENCH).d
DEPS += $(PSN_INDEX_TEST).d
DEPS += $(HOST_ZC_TEST).d
DEPS += $(HOST_ZC_E2E_TEST).d
DEPS += $(HOST_ZC_BENCH).d

$(TEST_BIN): tests/loopback.c $(LIB_SHARED)
	$(CC) $(CFLAGS) -o $@ $< -L. -Wl,-rpath,'$$ORIGIN/..' -luib $(LDFLAGS) $(LDLIBS)

$(FRAME_TEST): tests/afxdp_frame_test.c $(LIB_STATIC)
	$(CC) $(CFLAGS) -o $@ $< $(LIB_STATIC) $(LDFLAGS) $(LDLIBS)

$(DMABUF_TEST): tests/dmabuf_mr_test.c $(LIB_SHARED)
	$(CC) $(CFLAGS) -o $@ $< -L. -Wl,-rpath,'$$ORIGIN/..' -luib $(LDFLAGS) $(LDLIBS)

$(BTH_TEST): tests/bth_test.c $(LIB_STATIC)
	$(CC) $(CFLAGS) -o $@ $< $(LIB_STATIC) $(LDFLAGS) $(LDLIBS)

$(RXE_TEST): tests/rxe_interop.c
	$(CC) $(CFLAGS) -o $@ $< $(IBVERBS_LIBS) -libverbs

$(PROTOCOL_TEST): tests/protocol_test.c $(LIB_STATIC)
	$(CC) $(CFLAGS) -o $@ $< $(LIB_STATIC) $(LDFLAGS) $(LDLIBS)

$(UET_TEST): tests/uet_test.c $(LIB_SHARED)
	$(CC) $(CFLAGS) -o $@ $< -L. -Wl,-rpath,'$$ORIGIN/..' -luib $(LDFLAGS) $(LDLIBS)

$(NSCC_TEST): tests/nscc_test.c $(LIB_SHARED)
	$(CC) $(CFLAGS) -o $@ $< -L. -Wl,-rpath,'$$ORIGIN/..' -luib $(LDFLAGS) $(LDLIBS)

$(CONGESTION_TEST): tests/congestion_test.c $(LIB_SHARED)
	$(CC) $(CFLAGS) -o $@ $< -L. -Wl,-rpath,'$$ORIGIN/..' -luib $(LDFLAGS) $(LDLIBS)

$(CONGESTION_PLUGIN): tests/congestion_plugin.c $(LIB_SHARED)
	$(CC) $(CFLAGS) -shared -o $@ $< -L. \
		-Wl,-rpath,'$$ORIGIN/..' -luib $(LDFLAGS) $(LDLIBS)

$(ZEROCOPY_TEST): tests/zerocopy_test.c $(LIB_SHARED)
	$(CC) $(CFLAGS) -o $@ $< -L. -Wl,-rpath,'$$ORIGIN/..' -luib $(LDFLAGS) $(LDLIBS)

$(PROVIDER_DMABUF_TEST): tests/provider_dmabuf_test.c $(LIB_PROVIDER)
	$(CC) $(CFLAGS) -o $@ $< $(IBVERBS_LIBS) -libverbs

$(PROVIDER_NCCL_ABI_TEST): tests/provider_nccl_abi_test.c $(LIB_PROVIDER)
	$(CC) $(CFLAGS) -o $@ $< -ldl

$(PROVIDER_CQ_EVENT_TEST): tests/provider_cq_event_test.c $(LIB_PROVIDER)
	$(CC) $(CFLAGS) -o $@ $< $(IBVERBS_LIBS) -libverbs

$(EXTERNAL_DMABUF_TEST): tests/external_dmabuf_test.c $(LIB_SHARED)
	$(CC) $(CFLAGS) -o $@ $< -L. -Wl,-rpath,'$$ORIGIN/..' -luib $(LDFLAGS) $(LDLIBS)

$(CQ_EVENT_TEST): tests/cq_event_test.c $(LIB_SHARED)
	$(CC) $(CFLAGS) -o $@ $< -L. -Wl,-rpath,'$$ORIGIN/..' -luib $(LDFLAGS) $(LDLIBS)

$(URING_ASYNC_TEST): tests/uring_async_test.c $(LIB_SHARED)
	$(CC) $(CFLAGS) -o $@ $< -L. -Wl,-rpath,'$$ORIGIN/..' -luib $(LDFLAGS) $(LDLIBS)

$(PTR_RING_TEST): tests/ptr_ring_test.c src/ptr_ring.h
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS) -pthread

$(TRANSPORT_TEST): tests/transport_test.c $(LIB_SHARED)
	$(CC) $(CFLAGS) -o $@ $< -L. -Wl,-rpath,'$$ORIGIN/..' -luib $(LDFLAGS) $(LDLIBS)

$(QP_SCALE_TEST): tests/qp_scale_test.c $(LIB_SHARED)
	$(CC) $(CFLAGS) -o $@ $< -L. -Wl,-rpath,'$$ORIGIN/..' -luib $(LDFLAGS) $(LDLIBS)

$(QP_LOOKUP_BENCH): tests/qp_lookup_bench.c $(LIB_SHARED)
	$(CC) $(CFLAGS) -o $@ $< -L. -Wl,-rpath,'$$ORIGIN/..' -luib $(LDFLAGS) $(LDLIBS)

$(MR_SCALE_TEST): tests/mr_scale_test.c $(LIB_SHARED)
	$(CC) $(CFLAGS) -o $@ $< -L. -Wl,-rpath,'$$ORIGIN/..' -luib $(LDFLAGS) $(LDLIBS)

$(MR_LIFETIME_TEST): tests/mr_lifetime_test.c $(LIB_SHARED)
	$(CC) $(CFLAGS) -o $@ $< -L. -Wl,-rpath,'$$ORIGIN/..' -luib $(LDFLAGS) $(LDLIBS)

$(PROVIDER_DISPATCH_BENCH): tests/provider_dispatch_bench.c $(LIB_PROVIDER)
	$(CC) $(CFLAGS) -o $@ $< -L. -Wl,-rpath,'$$ORIGIN/..' -luibv \
		$(IBVERBS_LIBS) -libverbs

$(PROVIDER_WR_BENCH): tests/provider_wr_bench.c $(LIB_PROVIDER)
	$(CC) $(CFLAGS) -o $@ $< -L. -Wl,-rpath,'$$ORIGIN/..' -luibv \
		$(IBVERBS_LIBS) -libverbs

ifeq ($(HAVE_MPI),1)
$(MPI_COMPAT_TEST): tests/mpi_compat_test.c
	$(MPICC) $(CFLAGS) -o $@ $<
endif

$(PSN_INDEX_TEST): tests/psn_index_test.c $(LIB_SHARED)
	$(CC) $(CFLAGS) -o $@ $< -L. -Wl,-rpath,'$$ORIGIN/..' -luib $(LDFLAGS) $(LDLIBS)

$(ROCEV2_ECN_TEST): tests/rocev2_ecn_test.c $(LIB_SHARED)
	$(CC) $(CFLAGS) -o $@ $< -L. -Wl,-rpath,'$$ORIGIN/..' -luib \
		$(LDFLAGS) $(LDLIBS)

$(HOST_ZC_TEST): tests/host_zc_test.c $(LIB_STATIC)
	$(CC) $(CFLAGS) -o $@ $< $(LIB_STATIC) $(LDFLAGS) $(LDLIBS)

$(HOST_ZC_E2E_TEST): tests/host_zc_e2e_test.c $(LIB_SHARED)
	$(CC) $(CFLAGS) -o $@ $< -L. -Wl,-rpath,'$$ORIGIN/..' -luib $(LDFLAGS) $(LDLIBS)

$(HOST_ZC_BENCH): tests/host_zc_bench.c $(LIB_SHARED)
	$(CC) $(CFLAGS) -o $@ $< -L. -Wl,-rpath,'$$ORIGIN/..' -luib $(LDFLAGS) $(LDLIBS)

host-zc-benchmark: $(HOST_ZC_BENCH)
	./$(HOST_ZC_BENCH)

host-zc-virtme-test:
	./tests/run_host_zc_virtme.sh

$(REORDER_TEST): tests/reorder_test.c $(LIB_STATIC)
	$(CC) $(CFLAGS) -o $@ $< $(LIB_STATIC) $(LDFLAGS) $(LDLIBS)

$(UET_CONCURRENT_POST_TEST): tests/uet_concurrent_post_test.c $(LIB_SHARED)
	$(CC) $(CFLAGS) -o $@ $< -L. -Wl,-rpath,'$$ORIGIN/..' -luib $(LDFLAGS) $(LDLIBS)

ifeq ($(HAVE_IBVERBS),1)
rxe-test-build: $(RXE_TEST)

rxe-test: all rxe-test-build
	./tests/run_rxe_interop.sh

rxe-tools-test: all
	./tests/run_rxe_tools.sh

uib-tools-test: all
	./tests/run_uib_tools.sh

uet-tools-test: all
	UIB_TEST_PROTOCOL=uet UIB_TEST_PORT=5432 ./tests/run_uib_tools.sh

simulation-mtu-test: all
	./tests/run_simulation_mtu_test.sh

provider-dmabuf-test: all $(PROVIDER_DMABUF_TEST)
	LD_PRELOAD=$(CURDIR)/$(LIB_PROVIDER) UIB_BIND_PORT=19101 \
		UIB_UDP_MODE=socket \
		./$(PROVIDER_DMABUF_TEST)

provider-cq-event-test: all $(PROVIDER_CQ_EVENT_TEST)
	UIB_BIND_PORT=19610 LD_PRELOAD=$(CURDIR)/$(LIB_PROVIDER) \
		./$(PROVIDER_CQ_EVENT_TEST)

provider-nccl-abi-test: all $(PROVIDER_NCCL_ABI_TEST)
	UIB_BIND_PORT=19620 UIB_TRANSPORT_MODE=udp-sync UIB_ZEROCOPY=off \
		./$(PROVIDER_NCCL_ABI_TEST)

nccl-test: all $(PROVIDER_CQ_EVENT_TEST) $(PROVIDER_NCCL_ABI_TEST)
	$(MAKE) provider-cq-event-test
	$(MAKE) provider-nccl-abi-test
	./tests/run_nccl_test.sh

rccl-test: all $(PROVIDER_CQ_EVENT_TEST) $(PROVIDER_NCCL_ABI_TEST)
	$(MAKE) provider-cq-event-test
	$(MAKE) provider-nccl-abi-test
	./tests/run_rccl_test.sh

ucx-libfabric-test: all
	./simulation/runtime/with-device-anchor \
		./tests/run_ucx_libfabric_test.sh
ifeq ($(HAVE_MPI),1)
mpi-compat-test: all $(MPI_COMPAT_TEST)
	./simulation/runtime/with-device-anchor \
		./tests/run_mpi_compat_test.sh
else
mpi-compat-test:
	@echo "SKIP $@: MPI development files are unavailable"
endif
else
rxe-test-build rxe-test rxe-tools-test uib-tools-test uet-tools-test \
	simulation-mtu-test \
provider-dmabuf-test provider-cq-event-test provider-nccl-abi-test nccl-test \
rccl-test ucx-libfabric-test mpi-compat-test:
	@echo "SKIP $@: libibverbs development files are unavailable"
endif

uet-loss-test: all
	UIB_TEST_PROTOCOL=uet UIB_TEST_PORT=5432 UIB_TEST_DROP_TX=1 \
		UIB_TEST_ITERS=8 \
		./tests/run_uib_tools.sh

rocev2-loss-test: all $(TEST_BIN)
	UIB_TEST_DROP_TX=1 ./$(TEST_BIN)

rocev2-ecn-test: all $(ROCEV2_ECN_TEST)
	./$(ROCEV2_ECN_TEST)
	UIB_URING_RX_MODE=batch ./$(ROCEV2_ECN_TEST)

rocev2-congestion-test: all $(TEST_BIN) $(CONGESTION_PLUGIN)
	LD_PRELOAD=$(CURDIR)/$(CONGESTION_PLUGIN) UIB_PROTOCOL=rocev2 \
		UIB_ROCEV2_CC=test-pass CONGESTION_PLUGIN_TEST=1 ./$(TEST_BIN)
	LD_PRELOAD=$(CURDIR)/$(CONGESTION_PLUGIN) UIB_PROTOCOL=rocev2 \
		UIB_ROCEV2_CC=test-pass CONGESTION_PLUGIN_TEST=1 \
		UIB_TEST_DROP_TX=1 ./$(TEST_BIN)
	@echo "RoCEv2 with NSCC is experimental cross-protocol coverage only"
	UIB_PROTOCOL=rocev2 UIB_ROCEV2_CC=nscc \
		UIB_ROCEV2_NSCC_MAX_WND=5000 ./$(TEST_BIN)
	@if UIB_PROTOCOL=rocev2 UIB_ROCEV2_CC=nscc \
		UIB_ROCEV2_NSCC_MAX_WND=1 ./$(TEST_BIN) >/dev/null 2>&1; then \
		echo "invalid RoCEv2 NSCC window was accepted" >&2; \
		exit 1; \
	fi

uet-congestion-test: all $(TEST_BIN) $(NSCC_TEST) $(CONGESTION_TEST) \
	$(CONGESTION_PLUGIN)
	./$(CONGESTION_TEST)
	./$(NSCC_TEST)
	UIB_PROTOCOL=uet UIB_UET_NSCC_MAX_WND=5000 ./$(TEST_BIN)
	UIB_PROTOCOL=uet UIB_UET_CC=none ./$(TEST_BIN)
	LD_PRELOAD=$(CURDIR)/$(CONGESTION_PLUGIN) UIB_PROTOCOL=uet \
		UIB_UET_CC=test-pass ./$(TEST_BIN)

simulation-congestion-test: all $(TEST_BIN) $(CONGESTION_PLUGIN)
	./tests/run_simulation_congestion_test.sh

uet-multipath-test: all $(TEST_BIN)
	UIB_PROTOCOL=uet UIB_UET_MULTIPATH=oblivious ./$(TEST_BIN)
	UIB_PROTOCOL=uet UIB_UET_MULTIPATH=path-aware UIB_TEST_DROP_TX=1 \
		./$(TEST_BIN)

dmabuf-e2e-test: examples
	./examples/direct_traffic rocev2 dmabuf
	./examples/direct_traffic uet dmabuf
	./examples/direct_traffic dummy dmabuf
	./examples/stress_bench --protocol rocev2 --memory dmabuf --iterations 64 --size 16384 --window 16
	./examples/stress_bench --protocol uet --memory dmabuf --iterations 64 --size 16384 --window 16
	./examples/stress_bench --protocol dummy --memory dmabuf --iterations 64 --size 16384 --window 16

zerocopy-e2e-test: examples
	UIB_UDP_MODE=socket UIB_ZEROCOPY=off ./examples/stress_bench --protocol rocev2 --iterations 64 --size 16384 --window 16
	UIB_UDP_MODE=socket UIB_ZEROCOPY=required ./examples/stress_bench --protocol rocev2 --iterations 64 --size 16384 --window 16
	UIB_TRANSPORT_MODE=tcp-devmem UIB_ZEROCOPY=off ./examples/stress_bench --protocol uet --iterations 64 --size 16384 --window 16
	UIB_TRANSPORT_MODE=tcp-devmem UIB_ZEROCOPY=required ./examples/stress_bench --protocol uet --iterations 64 --size 16384 --window 16

benchmark-all: examples
	./examples/benchmark_matrix.sh

benchmark-local: benchmark-all

benchmark-congestion: examples
	@output=$${UIB_BENCH_OUTPUT:-/tmp/uib-congestion-benchmark-$$(date +%Y%m%d-%H%M%S)}; \
		./examples/congestion_benchmark_matrix.sh "$$output"

benchmark-report: examples
	@output=$${UIB_BENCH_OUTPUT:-/tmp/uib-benchmark-report-$$(date +%Y%m%d-%H%M%S)}; \
		status=0; \
		./examples/benchmark_matrix.sh "$$output" || status=$$?; \
		./examples/benchmark_report.sh "$$output"; \
		exit $$status

benchmark-doc: examples
	./examples/update_benchmark_reference.sh

benchmark-smoke: examples
	UIB_BENCH_ITERATIONS=64 UIB_BENCH_SIZE=16384 UIB_BENCH_WINDOW=16 \
		UIB_CC_BENCH_MATRIX='rocev2:dcqcn:clean uet:rccc:drops' \
		UIB_CC_BENCH_ITERATIONS=16 \
		./examples/benchmark_matrix.sh /tmp/uib-benchmark-smoke

dummy-reorder-test: examples
	UIB_TRANSPORT_MODE=udp-io-uring UIB_ZEROCOPY=off \
		./examples/stress_bench --protocol dummy --iterations 4096 \
		--size 1024 --window 64

ib-write-bw-test: examples
	./tests/run_ib_write_bw_matrix.sh

scaling-benchmark: examples $(QP_LOOKUP_BENCH) $(MR_SCALE_TEST) \
		$(SCALING_PROVIDER_BENCH) $(SCALING_PROVIDER_WR_BENCH)
	./$(QP_LOOKUP_BENCH)
	./$(MR_SCALE_TEST)
	$(if $(SCALING_PROVIDER_BENCH),./$(PROVIDER_DISPATCH_BENCH))
	$(if $(SCALING_PROVIDER_WR_BENCH),./$(PROVIDER_WR_BENCH))
	UIB_TRANSPORT_MODE=udp-sync ./examples/stress_bench --protocol rocev2 \
		--iterations $${UIB_SCALE_TRAFFIC_ITERS:-30000} --size 1024 --window 64
	UIB_TRANSPORT_MODE=udp-sync ./examples/stress_bench --protocol rocev2 \
		--iterations $${UIB_SCALE_TRAFFIC_ITERS:-30000} --size 1024 --window 128
	UIB_TRANSPORT_MODE=udp-sync ./examples/stress_bench --protocol uet \
		--iterations $${UIB_SCALE_TRAFFIC_ITERS:-30000} --size 1024 --window 64
	UIB_TRANSPORT_MODE=udp-sync ./examples/stress_bench --protocol uet \
		--iterations $${UIB_SCALE_TRAFFIC_ITERS:-30000} --size 1024 --window 128

context-scaling-benchmark: examples
	./examples/context_scaling_benchmark.sh

qp-scaling-benchmark: examples
	./examples/qp_scaling_benchmark.sh

single-qp-cpu-benchmark: examples
	./examples/single_qp_cpu_benchmark.sh

scaling-test: examples $(QP_SCALE_TEST) $(QP_LOOKUP_BENCH) $(MR_SCALE_TEST) \
		$(SCALING_PROVIDER_BENCH) $(SCALING_PROVIDER_WR_BENCH)
	./$(QP_SCALE_TEST)
	UIB_SCALE_THREADS=4 UIB_SCALE_LOOKUPS=10000 ./$(QP_LOOKUP_BENCH)
	UIB_MR_SCALE_LOOKUPS=10000 ./$(MR_SCALE_TEST)
	$(if $(SCALING_PROVIDER_BENCH),UIB_PROVIDER_CONTEXTS=8 \
		UIB_PROVIDER_LOOKUPS=10000 ./$(PROVIDER_DISPATCH_BENCH))
	$(if $(SCALING_PROVIDER_WR_BENCH),UIB_PROVIDER_WR_ITERS=1000 \
		./$(PROVIDER_WR_BENCH))
	UIB_TRANSPORT_MODE=udp-sync ./examples/stress_bench --protocol rocev2 \
		--iterations 256 --size 1024 --window 128
	UIB_TRANSPORT_MODE=udp-sync ./examples/stress_bench --protocol uet \
		--iterations 256 --size 1024 --window 128

gpu-dmabuf-test: all $(EXTERNAL_DMABUF_TEST)
	./$(EXTERNAL_DMABUF_TEST)

external-dmabuf-test: all $(EXTERNAL_DMABUF_TEST)
	./tests/run_external_dmabuf.sh

optional-features-test:
	./tests/run_optional_features.sh

remote-benchmark-test: examples
	./tests/run_remote_benchmark.sh

namespace-benchmark-test: examples
	./tests/run_namespace_benchmark.sh

examples-test: examples
	./examples/direct_traffic rocev2
	./examples/direct_traffic uet
	./examples/direct_traffic dummy
	UIB_TRANSPORT_MODE=udp-sync UIB_UDP_WORKERS=1 \
		UIB_QP_SCALE_QPS=2 UIB_BENCH_ITERATIONS=16 \
		UIB_BENCH_SIZE=4096 UIB_BENCH_WINDOW=8 UIB_BENCH_MTU=1024 \
		./examples/qp_scaling_bench
	UIB_TRANSPORT_MODE=udp-sync UIB_UDP_FLOWS=2 UIB_TX_WORKERS=2 \
		UIB_TX_WORKER_AFFINITY=off UIB_UDP_WORKER_AFFINITY=off \
		UIB_QP_SCALE_QPS=1 UIB_BENCH_ITERATIONS=32 \
		UIB_BENCH_SIZE=60000 UIB_BENCH_WINDOW=16 UIB_BENCH_MTU=8192 \
		./examples/qp_scaling_bench
	UIB_TRANSPORT_MODE=udp-sync UIB_UDP_WORKERS=1 \
		UIB_TEST_DROP_TX=1 UIB_QP_SCALE_QPS=2 \
		UIB_BENCH_ITERATIONS=16 UIB_BENCH_SIZE=4096 \
		UIB_BENCH_WINDOW=8 UIB_BENCH_MTU=1024 \
		./examples/qp_scaling_bench
	UIB_TRANSPORT_MODE=udp-sync UIB_UDP_WORKERS=1 \
		UIB_QP_SCALE_QPS=2 UIB_BENCH_PROTOCOL=rocev2 \
		UIB_BENCH_ITERATIONS=16 UIB_BENCH_SIZE=4096 \
		UIB_BENCH_WINDOW=8 UIB_BENCH_MTU=1024 \
		./examples/qp_scaling_bench
ifeq ($(HAVE_IBVERBS),1)
	UIB_BIND_PORT=19100 LD_PRELOAD=$(CURDIR)/$(LIB_PROVIDER) \
		./examples/verbs_traffic rocev2
	UIB_BIND_PORT=19100 LD_PRELOAD=$(CURDIR)/$(LIB_PROVIDER) \
		./examples/verbs_traffic uet
	UIB_BIND_PORT=19100 LD_PRELOAD=$(CURDIR)/$(LIB_PROVIDER) \
		./examples/verbs_traffic dummy
endif

examples-stress-test: examples
	./examples/stress_bench --protocol rocev2 --iterations 256 --size 1024 --window 32
	./examples/stress_bench --protocol uet --iterations 256 --size 1024 --window 32
	./examples/stress_bench --protocol dummy --iterations 256 --size 1024 --window 32

sync-transport-test: examples $(TEST_BIN)
	UIB_TRANSPORT_MODE=udp-sync ./$(TEST_BIN)
	UIB_TRANSPORT_MODE=udp-sync ./examples/stress_bench \
		--protocol rocev2 --iterations 256 --size 1024 --window 32
	UIB_TRANSPORT_MODE=udp-sync ./examples/stress_bench \
		--protocol uet --iterations 256 --size 1024 --window 32
	UIB_TRANSPORT_MODE=udp-sync ./examples/stress_bench \
		--protocol dummy --iterations 256 --size 1024 --window 32

uet-test: all $(TEST_BIN) $(UET_TEST)
	./$(UET_TEST)
	./$(NSCC_TEST)
	UIB_PROTOCOL=uet ./$(TEST_BIN)
	UIB_PROTOCOL=uet UIB_UET_OPTIMIZED=off ./$(TEST_BIN)
	UIB_PROTOCOL=uet UIB_UET_OPTIMIZED=small ./$(TEST_BIN)
	UIB_PROTOCOL=uet UIB_UET_OPTIMIZED=medium ./$(TEST_BIN)
	UIB_PROTOCOL=uet UIB_TRANSPORT_MODE=tcp-devmem ./$(TEST_BIN)

uet-error-test: all $(TEST_BIN)
	UIB_PROTOCOL=uet UIB_TRANSPORT_MODE=udp-sync UIB_UET_CC=rccc \
		UIB_UET_ERROR_TEST=1 ./$(TEST_BIN)
	UIB_PROTOCOL=uet UIB_TRANSPORT_MODE=udp-sync UIB_UET_CC=rccc \
		UIB_UET_ERROR_ONLY=1 UIB_TEST_DROP_TX=1 ./$(TEST_BIN)

test: shared-abi-test license-check examples $(TEST_BIN) $(FRAME_TEST) $(DMABUF_TEST) $(BTH_TEST) \
	$(PROTOCOL_TEST) $(UET_TEST) $(NSCC_TEST) $(CONGESTION_TEST) \
	$(ROCEV2_ECN_TEST) \
	$(ZEROCOPY_TEST) \
	$(CQ_EVENT_TEST) $(URING_ASYNC_TEST) $(PTR_RING_TEST) $(REORDER_TEST) \
	$(UET_CONCURRENT_POST_TEST) \
	$(TRANSPORT_TEST) $(QP_SCALE_TEST) $(PSN_INDEX_TEST) $(HOST_ZC_TEST) \
	$(HOST_ZC_E2E_TEST) $(MR_LIFETIME_TEST) $(NCCL_PROVIDER_TEST)
	./$(BTH_TEST)
	./$(FRAME_TEST)
	./$(DMABUF_TEST)
	./$(PROTOCOL_TEST)
	./$(UET_TEST)
	./$(NSCC_TEST)
	./$(CONGESTION_TEST)
	./$(ROCEV2_ECN_TEST)
	UIB_URING_RX_MODE=batch ./$(ROCEV2_ECN_TEST)
	./$(ZEROCOPY_TEST)
	./$(CQ_EVENT_TEST)
	./$(URING_ASYNC_TEST)
	./$(PTR_RING_TEST)
	./$(REORDER_TEST)
	./$(UET_CONCURRENT_POST_TEST)
	UIB_TEST_PROTOCOL=rocev2 ./$(UET_CONCURRENT_POST_TEST)
	./$(TRANSPORT_TEST)
	./$(QP_SCALE_TEST)
	./$(PSN_INDEX_TEST)
	./$(HOST_ZC_TEST)
	./$(HOST_ZC_E2E_TEST)
	./$(MR_LIFETIME_TEST)
	$(if $(NCCL_PROVIDER_TEST),UIB_BIND_PORT=19620 \
		UIB_TRANSPORT_MODE=udp-sync UIB_ZEROCOPY=off \
		./$(PROVIDER_NCCL_ABI_TEST))
	./tests/run_tcp_shutdown.sh
	UIB_TRANSPORT_MODE=tcp-devmem ./$(TEST_BIN)
	UIB_TRANSPORT_MODE=udp-sync ./$(TEST_BIN)
	UIB_TRANSPORT_MODE=udp-io-uring ./$(TEST_BIN)
	./tests/run_stats_output_test.sh

guest-test: all rxe-test-build
	$(MAKE) test
	$(MAKE) rxe-test
	$(MAKE) rxe-tools-test
	$(MAKE) uib-tools-test
	$(MAKE) uet-test
	$(MAKE) uet-tools-test
	$(MAKE) uet-loss-test
	$(MAKE) rocev2-loss-test
	$(MAKE) rocev2-congestion-test
	$(MAKE) uet-congestion-test
	$(MAKE) uet-multipath-test
	$(MAKE) dummy-reorder-test
	$(MAKE) examples-test
	$(MAKE) examples-stress-test
	$(MAKE) sync-transport-test
	$(MAKE) scaling-test
	$(MAKE) dmabuf-e2e-test
	$(MAKE) zerocopy-e2e-test
	$(MAKE) benchmark-smoke
	$(MAKE) ib-write-bw-test
	$(MAKE) provider-dmabuf-test
	$(MAKE) provider-cq-event-test
	$(MAKE) provider-nccl-abi-test
	$(MAKE) optional-features-test
	$(MAKE) remote-benchmark-test
	UIB_NAMESPACE_REQUIRED=1 $(MAKE) namespace-benchmark-test
	$(MAKE) gpu-dmabuf-test
	$(MAKE) external-dmabuf-test

virtme-test:
	./tests/run_virtme_tests.sh

test-all: virtme-test

configure-test: configure
	./configure
	$(MAKE) all

test-all: configure-test

install: $(LIB_TARGETS) $(MAN_PAGE)
	install -d $(DESTDIR)$(PREFIX)/lib
	install -d $(DESTDIR)$(PREFIX)/include/uib
	install -d $(DESTDIR)$(MANDIR)/man7
	install -d $(DESTDIR)$(DOCDIR)
	install -d $(DESTDIR)$(DOCDIR)/LICENSES
	install -m 644 $(LIB_STATIC) $(DESTDIR)$(PREFIX)/lib/
	install -m 755 $(LIB_SHARED_REAL) $(DESTDIR)$(PREFIX)/lib/
	ln -sfn $(LIB_SHARED_REAL) $(DESTDIR)$(PREFIX)/lib/$(LIB_SHARED_SONAME)
	ln -sfn $(LIB_SHARED_SONAME) $(DESTDIR)$(PREFIX)/lib/$(LIB_SHARED)
ifeq ($(ENABLE_PROVIDER),1)
	install -m 755 $(LIB_PROVIDER) $(DESTDIR)$(PREFIX)/lib/
endif
	install -m 644 include/uib/*.h $(DESTDIR)$(PREFIX)/include/uib/
	install -m 644 $(MAN_PAGE) $(DESTDIR)$(MANDIR)/man7/
	install -m 644 LICENSE NOTICE AUTHORS $(DESTDIR)$(DOCDIR)/
	install -m 644 LICENSES/*.txt $(DESTDIR)$(DOCDIR)/LICENSES/
	install -m 644 LICENSES/README.md $(DESTDIR)$(DOCDIR)/LICENSES/

install-static: $(LIB_STANDALONE)
	install -d $(DESTDIR)$(PREFIX)/lib
	install -m 644 $(LIB_STANDALONE) $(DESTDIR)$(PREFIX)/lib/

$(AFXDP_BPF_OBJ): bpf/uib_afxdp_kern.c
	$(BPF_CLANG) $(BPF_CFLAGS) -c -o $@ $<

afxdp-program: $(AFXDP_BPF_OBJ)

clean:
	$(MAKE) -C examples clean
	rm -f $(OBJS) $(PROV_OBJS) $(STANDALONE_OBJS_0) \
		$(STANDALONE_OBJS_1) $(DEPS) \
		$(LIB_STATIC) $(LIB_SHARED) $(LIB_SHARED_SONAME) $(LIB_SHARED_REAL) $(LIB_PROVIDER) \
		.libuib-standalone-uib-0.a .libuib-standalone-uib-1.a \
		$(LIB_STANDALONE) $(STATIC_LINK_TEST) \
		$(TEST_BIN) $(FRAME_TEST) $(DMABUF_TEST) \
		$(BTH_TEST) $(RXE_TEST) $(PROTOCOL_TEST) \
		$(UET_TEST) $(NSCC_TEST) $(CONGESTION_TEST) \
		$(ROCEV2_ECN_TEST) \
		$(CONGESTION_PLUGIN) $(ZEROCOPY_TEST) \
		$(PROVIDER_DMABUF_TEST) $(PROVIDER_NCCL_ABI_TEST) \
		$(EXTERNAL_DMABUF_TEST) $(MPI_COMPAT_TEST) \
		$(CQ_EVENT_TEST) $(URING_ASYNC_TEST) $(PROVIDER_CQ_EVENT_TEST) \
		$(PTR_RING_TEST) \
		$(REORDER_TEST) $(UET_CONCURRENT_POST_TEST) \
		$(TRANSPORT_TEST) $(QP_SCALE_TEST) \
		$(QP_LOOKUP_BENCH) $(MR_SCALE_TEST) $(MR_LIFETIME_TEST) \
		$(PROVIDER_DISPATCH_BENCH) \
		$(PROVIDER_WR_BENCH) $(PSN_INDEX_TEST) $(HOST_ZC_TEST) \
		$(HOST_ZC_E2E_TEST) $(HOST_ZC_BENCH) \
		$(AFXDP_BPF_OBJ)

.PHONY: shared-abi-test all license-check static static-test runtime-bundle runtime-bundle-portable \
	runtime-bundle-portable-test \
	runtime-bundle-test distribution-compat-test upstream-compat-test \
	runtime-image model-cache-image \
	training-image training-smoke-test FORCE docs \
	training-public-test training-production-cpu-test training-gpu-test \
	training-uib-preflight-test \
	training-uib-test \
	inference-image inference-smoke-test inference-production-cpu-test \
	inference-gpu-test \
	diagnostic-sources diagnostic-image diagnostic-smoke-test \
	diagnostic-uib-preflight-test diagnostic-gpu-test \
	simulation-cli-test simulation-netem-test simulation-fault-schedule-test \
	simulation-rank-fault-test \
	simulation-fault-e2e-test simulation-congestion-test \
	simulation-kind-test simulation-kind-e2e-test \
	simulation-virtme-backend-test \
	simulation-lifecycle-interruption-test \
	simulation-workload-interruption-test \
	simulation-virtme-kubernetes-e2e-test \
	simulation-virtme-kubeflow-e2e-test \
	simulation-virtme-kuberay-e2e-test \
	simulation-virtme-slurm-e2e-test \
	simulation-virtme-training-e2e-test \
	simulation-virtme-inference-e2e-test \
	simulation-virtme-netem-e2e-test \
	simulation-kubernetes-render-test simulation-slurm-render-test \
	simulation-kubernetes-lifecycle-test \
	simulation-transport-evidence-test \
	simulation-provider-counter-evidence-test \
	simulation-workload-metrics-test \
	simulation-slurm-lifecycle-test simulation-slurm-collect-test \
	simulation-libvirt-test \
	simulation-libvirt-e2e-test \
	simulation-libvirt-netem-e2e-test \
	simulation-libvirt-training-e2e-test \
	simulation-libvirt-inference-e2e-test \
	simulation-kubeflow-e2e-test \
	simulation-kubernetes-uib-test \
	simulation-kuberay-e2e-test \
	docs-check examples clean install install-static test afxdp-program \
	rxe-test-build \
	rxe-test rxe-tools-test \
	uib-tools-test uet-test uet-error-test uet-tools-test simulation-mtu-test \
	uet-loss-test rocev2-loss-test \
	rocev2-ecn-test \
	uet-congestion-test uet-multipath-test examples-test \
	examples-stress-test sync-transport-test dmabuf-e2e-test \
	zerocopy-e2e-test \
	benchmark-all benchmark-local benchmark-congestion benchmark-report benchmark-doc \
	benchmark-smoke ib-write-bw-test dummy-reorder-test \
	scaling-benchmark context-scaling-benchmark qp-scaling-benchmark \
	single-qp-cpu-benchmark \
	scaling-test \
	provider-dmabuf-test provider-cq-event-test provider-nccl-abi-test \
	nccl-test rccl-test ucx-libfabric-test mpi-compat-test \
	gpu-dmabuf-test external-dmabuf-test remote-benchmark-test \
	namespace-benchmark-test guest-test \
	optional-features-test virtme-test configure-test test-all \
	host-zc-benchmark host-zc-virtme-test


-include $(DEPS)
