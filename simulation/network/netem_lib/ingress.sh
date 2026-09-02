# SPDX-License-Identifier: Apache-2.0

ingress_qdisc()
{
	"$tc_command" qdisc show dev "$1" | \
		awk '$2 == "ingress" { print $2; exit }'
}

attach_ingress()
{
	interface=$1
	ifb=$2
	replace_existing=$3
	existing=$(ingress_qdisc "$interface")

	if [ -n "$existing" ] && [ "$replace_existing" != true ]; then
		echo "refusing to replace ingress qdisc on $interface" >&2
		return 1
	fi
	if [ -n "$existing" ]; then
		"$tc_command" qdisc del dev "$interface" ingress
	fi
	"$tc_command" qdisc add dev "$interface" handle ffff: ingress
	if ! "$tc_command" filter add dev "$interface" parent ffff: \
		protocol all matchall action mirred egress redirect dev "$ifb"; then
		"$tc_command" qdisc del dev "$interface" ingress
		return 1
	fi
}

clear_ingress()
{
	if [ "$(ingress_qdisc "$1")" = ingress ]; then
		"$tc_command" qdisc del dev "$1" ingress
	fi
}

destroy_ingress()
{
	resolve_ip
	if ! "$ip_command" -j -d link show dev "$1" | \
		jq -e '.[0].linkinfo.info_kind == "ifb"' >/dev/null; then
		echo "refusing to delete non-IFB interface $1" >&2
		return 1
	fi
	clear_netem "$1"
	"$ip_command" link delete dev "$1" type ifb
}
