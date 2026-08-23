#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
set -eu

rank=${OMPI_COMM_WORLD_RANK:-${PMI_RANK:-${PMIX_RANK:-}}}
if [ -z "$rank" ]; then
	echo "cannot determine the training rank" >&2
	exit 1
fi
if [ "$rank" -gt 253 ]; then
	echo "local UIB training supports at most 254 ranks" >&2
	exit 1
fi

engine=${TRAINING_CONTAINER_ENGINE:-docker}
image=${TRAINING_IMAGE:-uib-training:test}
runtime=${TRAINING_UIB_RUNTIME:?TRAINING_UIB_RUNTIME is required}
config=${TRAINING_UIB_CONFIG:?TRAINING_UIB_CONFIG is required}
results=${TRAINING_UIB_RESULTS:?TRAINING_UIB_RESULTS is required}
mps_pipe=${TRAINING_MPS_PIPE:?TRAINING_MPS_PIPE is required}
mps_log=${TRAINING_MPS_LOG:?TRAINING_MPS_LOG is required}
gpu_device=${TRAINING_GPU_DEVICE:-nvidia.com/gpu=all}
bind_port=${TRAINING_UIB_BIND_PORT:-19720}
master_port=${TRAINING_MASTER_PORT:-29720}
bind_ip=127.0.0.$((rank + 1))

container()
{
	# The make variable may include a privilege helper before the engine.
	# shellcheck disable=SC2086
	$engine "$@"
}

exec_container()
{
	container run --rm --user "$(id -u):$(id -g)" \
		--hostname "uib-training-rank-$rank" \
		--network host --ipc host --pid host \
		--security-opt seccomp=unconfined \
		--device "$gpu_device" \
		-v "$runtime:/opt/uib:ro" \
		-v "$config:/run/workload.json:ro" \
		-v "$results:/results" \
		-v "$mps_pipe:/run/uib-mps/pipe" \
		-v "$mps_log:/run/uib-mps/log" \
		-e HOME=/tmp -e USER=uib -e LOGNAME=uib \
		-e LD_LIBRARY_PATH=/opt/uib/lib \
		-e RANK="$rank" -e LOCAL_RANK=0 -e WORLD_SIZE=2 \
		-e MASTER_ADDR=127.0.0.1 -e MASTER_PORT="$master_port" \
		-e CUDA_VISIBLE_DEVICES=0 \
		-e CUDA_MPS_PIPE_DIRECTORY=/run/uib-mps/pipe \
		-e CUDA_MPS_LOG_DIRECTORY=/run/uib-mps/log \
		-e NCCL_MULTI_RANK_GPU_ENABLE=1 \
		-e NCCL_MAX_CTAS=1 -e NCCL_MIN_NCHANNELS=1 \
		-e NCCL_MAX_NCHANNELS=1 -e NCCL_NVLS_ENABLE=0 \
		-e NCCL_P2P_DISABLE=1 -e NCCL_SHM_DISABLE=1 \
		-e NCCL_NET_GDR_LEVEL=LOC \
		-e NCCL_IBVERBS_LIB=/opt/uib/libuibv.so \
		-e NCCL_NET=IB -e NCCL_IB_DISABLE=0 -e NCCL_IB_HCA==uib0 \
		-e NCCL_SOCKET_IFNAME=lo -e NCCL_DEBUG=INFO \
		-e NCCL_DEBUG_SUBSYS=INIT,NET \
		-e UIB_BIND_IP="$bind_ip" -e UIB_BIND_PORT="$bind_port" \
		-e UIB_PROTOCOL=rocev2 -e UIB_TRANSPORT_MODE=udp-sync \
		-e UIB_ZEROCOPY=off \
		--entrypoint /opt/uib-training/bin/train "$image" \
		--config /run/workload.json
}

exec_container
