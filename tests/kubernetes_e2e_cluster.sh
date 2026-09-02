# SPDX-License-Identifier: Apache-2.0
# shellcheck shell=sh
# shellcheck disable=SC2034,SC2154

kubernetes_profile=${SIMULATION_KUBERNETES_PROFILE:-kind-kubernetes}
kubernetes_workers=${SIMULATION_KUBERNETES_WORKERS:-2}
kubernetes_cpu=${SIMULATION_KUBERNETES_CPU:-4}
kubernetes_memory=${SIMULATION_KUBERNETES_MEMORY:-8Gi}
kubernetes_disk=${SIMULATION_KUBERNETES_DISK:-32Gi}
workload_cpu=${SIMULATION_KUBERNETES_WORKLOAD_CPU:-$kubernetes_cpu}
workload_memory=${SIMULATION_KUBERNETES_WORKLOAD_MEMORY:-$kubernetes_memory}
kubernetes_profile_args="--set nodes.workers=$kubernetes_workers \
--set nodes.cpu=$kubernetes_cpu --set nodes.memory=$kubernetes_memory \
--set nodes.disk=$kubernetes_disk"
if test -n "${SIMULATION_KUBERNETES_DISABLE:-}"; then
	kubernetes_profile_args="$kubernetes_profile_args \
--set kubernetes.disable=$SIMULATION_KUBERNETES_DISABLE"
fi
kubernetes_workload_args="$kubernetes_profile_args \
--set workload.resources.cpu=$workload_cpu \
--set workload.resources.memory=$workload_memory"

quantity_mib()
{
	case $1 in
	*Mi) echo "${1%Mi}" ;;
	*Gi) echo "$((${1%Gi} * 1024))" ;;
	*Ti) echo "$((${1%Ti} * 1024 * 1024))" ;;
	*) return 1 ;;
	esac
}

kubernetes_host_memory_preflight()
{
	if test "$kubernetes_profile" != virtme-kubernetes; then
		return 0
	fi
	controller_memory=${SIMULATION_VIRTME_CONTROLLER_MEMORY:-$kubernetes_memory}
	worker_memory=${SIMULATION_VIRTME_WORKER_MEMORY:-$kubernetes_memory}
	reserve_gib=${SIMULATION_HOST_MEMORY_RESERVE_GIB:-12}
	available_kib=$(awk '/^MemAvailable:/ { print $2 }' /proc/meminfo)
	controller_mib=$(quantity_mib "$controller_memory")
	worker_mib=$(quantity_mib "$worker_memory")

	case $reserve_gib in
	''|*[!0-9]*)
		echo "SIMULATION_HOST_MEMORY_RESERVE_GIB must be a non-negative integer" >&2
		exit 2
		;;
	esac
	required_mib=$((controller_mib +
			kubernetes_workers * worker_mib + reserve_gib * 1024))
	if test "$available_kib" -lt "$((required_mib * 1024))"; then
		echo "SKIP virtme Kubernetes E2E: ${required_mib}Mi available memory required, $((available_kib / 1024))Mi available" >&2
		return 77
	fi
}

case $kubernetes_profile in
kind-kubernetes)
	kubeconfig=$test_dir/kubeconfig
	;;
virtme-kubernetes)
	virtme_ssh_port_base=${SIMULATION_VIRTME_SSH_PORT_BASE:-23000}
	kubernetes_profile_args="$kubernetes_profile_args \
--set virtme.sshPortBase=$virtme_ssh_port_base"
	if test -n "${SIMULATION_VIRTME_CONTROLLER_MEMORY:-}"; then
		kubernetes_profile_args="$kubernetes_profile_args \
--set virtme.controllerMemory=$SIMULATION_VIRTME_CONTROLLER_MEMORY"
	fi
	if test -n "${SIMULATION_VIRTME_WORKER_MEMORY:-}"; then
		kubernetes_profile_args="$kubernetes_profile_args \
--set virtme.workerMemory=$SIMULATION_VIRTME_WORKER_MEMORY"
	fi
	kubeconfig=$state_dir/$name/infrastructure/kubeconfig
	;;
libvirt-kubernetes)
	kubeconfig=$state_dir/$name/infrastructure/kubeconfig
	;;
*)
	echo "operator E2E profile must be kind-kubernetes, virtme-kubernetes, or libvirt-kubernetes" >&2
	exit 2
	;;
esac

kubernetes_up()
{
	if test "$kubernetes_profile" = kind-kubernetes; then
		# shellcheck disable=SC2086
		"$cli" up --profile "$kubernetes_profile" --run-id "$name" \
			--state-dir "$state_dir" --kubeconfig "$kubeconfig" \
			--wait 300s $kubernetes_profile_args
	else
		# shellcheck disable=SC2086
		"$cli" up --profile "$kubernetes_profile" --run-id "$name" \
			--state-dir "$state_dir" $kubernetes_profile_args
	fi
}

kubernetes_load_image()
{
	controller_arg=
	if test "${2:-}" = include-controller; then
		controller_arg=--include-controller
	fi
	# shellcheck disable=SC2086
	"$cli" load-image --profile "$kubernetes_profile" --run-id "$name" \
		--state-dir "$state_dir" --image "$1" --allow-mutable-tags \
		$controller_arg $kubernetes_profile_args
}

kubernetes_down()
{
	# shellcheck disable=SC2086
	"$cli" down --profile "$kubernetes_profile" --run-id "$name" \
		--state-dir "$state_dir" $kubernetes_profile_args
}
