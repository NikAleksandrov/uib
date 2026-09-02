# SPDX-License-Identifier: Apache-2.0

usage()
{
	cat >&2 <<'EOF'
usage: netem render CONFIG.json INTERFACE [--target TARGET]
       netem apply CONFIG.json INTERFACE [--target TARGET] [--replace-existing]
       netem clear INTERFACE
       netem show INTERFACE
       netem render-ingress CONFIG.json INTERFACE IFB [--target TARGET|--oversubscription]
       netem apply-ingress CONFIG.json INTERFACE IFB [--target TARGET|--oversubscription] [--replace-existing]
       netem attach-ingress INTERFACE IFB [--replace-existing]
       netem clear-ingress INTERFACE
       netem destroy-ingress IFB
       netem show-ingress INTERFACE IFB
EOF
}

valid_interface()
{
	case $1 in
	''|*[!a-zA-Z0-9_.:@-]*) return 1 ;;
	*) return 0 ;;
	esac
}

root_qdisc()
{
	"$tc_command" qdisc show dev "$1" | \
		awk '$4 == "root" { print $2; exit }'
}

clear_netem()
{
	interface=$1

	if [ "$(root_qdisc "$interface")" = netem ]; then
		"$tc_command" qdisc del dev "$interface" root
	fi
}

resolve_ip()
{
	ip_command=${NETEM_IP:-}
	if [ -z "$ip_command" ]; then
		ip_command=$(command -v ip || true)
	fi
	if [ -z "$ip_command" ] && [ -x /sbin/ip ]; then
		ip_command=/sbin/ip
	fi
	if [ -z "$ip_command" ]; then
		echo "ip was not found; set NETEM_IP to its path" >&2
		exit 1
	fi
}
