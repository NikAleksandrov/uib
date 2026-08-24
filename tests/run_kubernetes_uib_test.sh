#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
set -eu

project_dir=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
cli=$project_dir/simulation/bin/uib-sim
name=${SIMULATION_KUBERNETES_UIB_NAME:-uib-data-$$}
namespace=${SIMULATION_KUBERNETES_UIB_NAMESPACE:-uib-data-$$}
protocols=${SIMULATION_KUBERNETES_UIB_PROTOCOLS:-rocev2 uet}
iterations=${SIMULATION_KUBERNETES_UIB_ITERATIONS:-64}
wait_time=${SIMULATION_KUBERNETES_UIB_WAIT:-180s}
test_timeout=${SIMULATION_KUBERNETES_UIB_TIMEOUT:-180s}
diagnostic_image=${SIMULATION_KUBERNETES_UIB_DIAGNOSTIC_IMAGE:-uib-diagnostics:test}
runtime_image=${SIMULATION_KUBERNETES_UIB_RUNTIME_IMAGE:-uib-runtime:test}
kind_command=${KIND_COMMAND:-kind}
kubectl_command=${KUBECTL_COMMAND:-kubectl}
test_dir=$(mktemp -d "${TMPDIR:-/tmp}/uib-kubernetes-data.XXXXXX")
kubeconfig=$test_dir/kubeconfig
created=0

cleanup()
{
	if [ "$created" = 1 ]; then
		if [ -s "$kubeconfig" ]; then
			"$kubectl_command" --kubeconfig "$kubeconfig" delete namespace \
				"$namespace" --ignore-not-found --wait=false \
				>/dev/null 2>&1 || true
		fi
		"$cli" down --profile kind-kubernetes --run-id "$name" \
			>/dev/null 2>&1 || true
	fi
	rm -rf -- "$test_dir"
}
trap cleanup EXIT INT TERM

require_command()
{
	case $1 in
	*/*) [ -x "$1" ] ;;
	*) command -v "$1" >/dev/null 2>&1 ;;
	esac
}

case $iterations in
''|*[!0-9]*|0)
	echo "SIMULATION_KUBERNETES_UIB_ITERATIONS must be a positive integer" >&2
	exit 2
	;;
esac

protocol_count=0
for protocol in $protocols; do
	protocol_count=$((protocol_count + 1))
	case $protocol in
	rocev2|uet) ;;
	*)
		echo "unsupported UIB protocol in test list: $protocol" >&2
		exit 2
		;;
	esac
done
if [ "$protocol_count" -eq 0 ]; then
	echo "SIMULATION_KUBERNETES_UIB_PROTOCOLS cannot be empty" >&2
	exit 2
fi

for command_name in "$kind_command" "$kubectl_command"; do
	if ! require_command "$command_name"; then
		echo "required Kubernetes UIB command not found: $command_name" >&2
		exit 1
	fi
done

created=1
"$cli" up --profile kind-kubernetes --run-id "$name" \
	--kubeconfig "$kubeconfig" --wait "$wait_time"
"$cli" load-image --profile kind-kubernetes --run-id "$name" \
	--image "$diagnostic_image" --image "$runtime_image" \
	--allow-mutable-tags
"$kubectl_command" --kubeconfig "$kubeconfig" create namespace "$namespace"

for protocol in $protocols; do
	case $protocol in
	rocev2) port=4791 ;;
	uet) port=5432 ;;
	esac
	for operation in send-receive rdma-write; do
		case $operation in
		send-receive)
			program_args='        - ibv_rc_pingpong
        - -d
        - uib0
        - -g
        - "0"
        - -m
        - "1024"
        - -s
        - "2051"
        - -n
        - "'"$iterations"'"
        - -c'
			result_pattern='bytes in'
			;;
		rdma-write)
			program_args='        - ib_write_bw
        - -d
        - uib0
        - -F
        - -s
        - "4096"
        - -n
        - "'"$iterations"'"'
			result_pattern='RDMA_Write BW Test'
			;;
		esac
		server=uib-$protocol-$operation-server
		client=uib-$protocol-$operation-client
		manifest=$test_dir/$protocol-$operation.yaml
		cat > "$manifest" <<EOF
apiVersion: v1
kind: Pod
metadata:
  name: $server
  namespace: $namespace
  labels:
    app.kubernetes.io/name: uib-data-plane-test
    uib.dev/protocol: $protocol
    uib.dev/role: server
spec:
  restartPolicy: Never
  nodeSelector:
    kubernetes.io/hostname: $name-worker
  securityContext:
    seccompProfile:
      type: Unconfined
  initContainers:
    - name: uib-runtime
      image: $runtime_image
      imagePullPolicy: IfNotPresent
      command: [/bin/sh, -ec]
      args: [cp -a /uib-runtime/. /opt/uib/]
      volumeMounts:
        - name: uib-runtime
          mountPath: /opt/uib
    - name: uib-runtime-check
      image: $diagnostic_image
      imagePullPolicy: IfNotPresent
      command: [/usr/local/bin/diagnose]
      args: [preflight, --require-uib]
      env:
        - name: UIB_PROTOCOL
          value: $protocol
        - name: UIB_BIND_IP
          valueFrom:
            fieldRef:
              fieldPath: status.podIP
        - name: UIB_BIND_PORT
          value: "$port"
        - name: UIB_TRANSPORT_MODE
          value: udp-sync
        - name: UIB_ZEROCOPY
          value: "off"
      volumeMounts:
        - name: uib-runtime
          mountPath: /opt/uib
  containers:
    - name: verbs
      image: $diagnostic_image
      imagePullPolicy: IfNotPresent
      command: [/usr/local/bin/diagnose]
      args:
        - exec
        - timeout
        - -k
        - "1"
        - "$test_timeout"
$program_args
      env:
        - name: LD_PRELOAD
          value: /opt/uib/lib/libuibv.so
        - name: LD_LIBRARY_PATH
          value: /opt/uib/lib
        - name: UIB_PROTOCOL
          value: $protocol
        - name: UIB_BIND_IP
          valueFrom:
            fieldRef:
              fieldPath: status.podIP
        - name: UIB_BIND_PORT
          value: "$port"
        - name: UIB_TRANSPORT_MODE
          value: udp-sync
        - name: UIB_ZEROCOPY
          value: "off"
      volumeMounts:
        - name: uib-runtime
          mountPath: /opt/uib
  volumes:
    - name: uib-runtime
      emptyDir: {}
EOF
		"$kubectl_command" --kubeconfig "$kubeconfig" apply -f "$manifest"
		"$kubectl_command" --kubeconfig "$kubeconfig" wait \
			--namespace "$namespace" --for=condition=Ready "pod/$server" \
			--timeout "$test_timeout"
		server_ip=$("$kubectl_command" --kubeconfig "$kubeconfig" get pod \
			--namespace "$namespace" "$server" \
			-o jsonpath='{.status.podIP}')

		python3 - "$manifest" "$client" "$server_ip" "$name-worker2" <<'PY'
import sys

import yaml


path, client, server_ip, node = sys.argv[1:]
with open(path, encoding="utf-8") as source:
    pod = yaml.safe_load(source)
pod["metadata"]["name"] = client
pod["metadata"]["labels"]["uib.dev/role"] = "client"
pod["spec"]["nodeSelector"]["kubernetes.io/hostname"] = node
pod["spec"]["containers"][0]["args"].append(server_ip)
with open(path, "w", encoding="utf-8") as output:
    yaml.safe_dump(pod, output, sort_keys=False)
PY
		"$kubectl_command" --kubeconfig "$kubeconfig" apply -f "$manifest"
		"$kubectl_command" --kubeconfig "$kubeconfig" wait \
			--namespace "$namespace" \
			--for=jsonpath='{.status.phase}'=Succeeded "pod/$client" \
			--timeout "$test_timeout"
		"$kubectl_command" --kubeconfig "$kubeconfig" wait \
			--namespace "$namespace" \
			--for=jsonpath='{.status.phase}'=Succeeded "pod/$server" \
			--timeout "$test_timeout"

		for pod in "$server" "$client"; do
			"$kubectl_command" --kubeconfig "$kubeconfig" logs \
				--namespace "$namespace" "$pod" \
				--container uib-runtime-check \
				| grep -F 'UIB runtime preflight: PASS' >/dev/null
			"$kubectl_command" --kubeconfig "$kubeconfig" logs \
				--namespace "$namespace" "$pod" --container verbs \
				> "$test_dir/$pod.log"
			grep -F "$result_pattern" "$test_dir/$pod.log" >/dev/null
		done

		"$kubectl_command" --kubeconfig "$kubeconfig" get pod \
			--namespace "$namespace" "$server" "$client" -o json \
			> "$test_dir/$protocol-$operation-pods.json"
		python3 - "$test_dir/$protocol-$operation-pods.json" "$name" <<'PY'
import json
import sys


with open(sys.argv[1], encoding="utf-8") as source:
    pods = json.load(source)["items"]
nodes = {pod["spec"]["nodeName"] for pod in pods}
expected = {f"{sys.argv[2]}-worker", f"{sys.argv[2]}-worker2"}
assert nodes == expected, f"UIB peers used {nodes}, expected {expected}"
assert all(pod["status"]["phase"] == "Succeeded" for pod in pods)
assert len({pod["status"]["podIP"] for pod in pods}) == 2
PY
		cat "$test_dir/$client.log"
		"$kubectl_command" --kubeconfig "$kubeconfig" delete pod \
			--namespace "$namespace" "$server" "$client" \
			--wait --timeout "$test_timeout"
		echo "Kubernetes $protocol $operation UIB test: PASS"
	done
done

"$kubectl_command" --kubeconfig "$kubeconfig" delete namespace "$namespace" \
	--wait --timeout "$test_timeout"
"$cli" down --profile kind-kubernetes --run-id "$name"
created=0

echo "real Kubernetes UIB data-plane test: PASS"
