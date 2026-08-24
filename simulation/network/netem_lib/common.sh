# SPDX-License-Identifier: Apache-2.0

usage()
{
	cat >&2 <<'EOF'
usage: netem render CONFIG.json INTERFACE
       netem apply CONFIG.json INTERFACE [--replace-existing]
       netem clear INTERFACE
       netem show INTERFACE
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
