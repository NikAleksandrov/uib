# SPDX-License-Identifier: Apache-2.0
# shellcheck shell=sh

kubernetes_profile=${SIMULATION_KUBERNETES_PROFILE:-kind-kubernetes}
kubernetes_workers=${SIMULATION_KUBERNETES_WORKERS:-2}
kubernetes_cpu=${SIMULATION_KUBERNETES_CPU:-2}
kubernetes_memory=${SIMULATION_KUBERNETES_MEMORY:-2Gi}
kubernetes_disk=${SIMULATION_KUBERNETES_DISK:-32Gi}
kubernetes_profile_args="--set nodes.workers=$kubernetes_workers \
--set nodes.cpu=$kubernetes_cpu --set nodes.memory=$kubernetes_memory \
--set nodes.disk=$kubernetes_disk"

case $kubernetes_profile in
kind-kubernetes)
	kubeconfig=$test_dir/kubeconfig
	;;
virtme-kubernetes|libvirt-kubernetes)
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
