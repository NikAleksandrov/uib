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
kubernetes_workload_args="$kubernetes_profile_args \
--set workload.resources.cpu=$workload_cpu \
--set workload.resources.memory=$workload_memory"

case $kubernetes_profile in
kind-kubernetes)
	kubeconfig=$test_dir/kubeconfig
	;;
virtme-kubernetes)
	virtme_ssh_port_base=${SIMULATION_VIRTME_SSH_PORT_BASE:-23000}
	kubernetes_profile_args="$kubernetes_profile_args \
--set virtme.sshPortBase=$virtme_ssh_port_base"
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
	# shellcheck disable=SC2086
	"$cli" load-image --profile "$kubernetes_profile" --run-id "$name" \
		--state-dir "$state_dir" --image "$1" --allow-mutable-tags \
		$kubernetes_profile_args
}

kubernetes_down()
{
	# shellcheck disable=SC2086
	"$cli" down --profile "$kubernetes_profile" --run-id "$name" \
		--state-dir "$state_dir" $kubernetes_profile_args
}
