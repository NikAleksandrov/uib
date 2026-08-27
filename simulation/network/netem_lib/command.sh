# SPDX-License-Identifier: Apache-2.0

build_command()
{
	interface=$1
	shaping_enabled=$(jq -r '.enabled' <<<"$shaping")

	if [ "$shaping_enabled" != true ]; then
		command=("$tc_command" qdisc del dev "$interface" root)
		return
	fi

	command=("$tc_command" qdisc replace dev "$interface" root netem)
	value=$(jq -r '.queueLimit // empty' <<<"$shaping")
	[ -z "$value" ] || command+=(limit "$value")

	latency=$(jq -r '.latency // empty' <<<"$shaping")
	if [ -n "$latency" ]; then
		command+=(delay "$latency")
		jitter=$(jq -r '.jitter // empty' <<<"$shaping")
		if [ -n "$jitter" ]; then
			command+=("$jitter")
			value=$(jq -r '.delayCorrelationPercent // empty' \
				<<<"$shaping")
			[ -z "$value" ] || command+=("$value%")
			value=$(jq -r '.delayDistribution // empty' <<<"$shaping")
			[ -z "$value" ] || command+=(distribution "$value")
		fi
	fi

	value=$(jq -r '.lossPercent // empty' <<<"$shaping")
	if [ -n "$value" ]; then
		loss_model=$(jq -r '.lossModel // "random"' <<<"$shaping")
		if [ "$loss_model" = gemodel ]; then
			command+=(loss gemodel "$value%")
			for parameter in lossRecoveryPercent \
				lossBadStatePercent lossGoodStatePercent; do
				value=$(jq -r ".${parameter}" <<<"$shaping")
				command+=("$value%")
			done
		else
			command+=(loss random "$value%")
			correlation=$(jq -r '.lossCorrelationPercent // empty' \
				<<<"$shaping")
			[ -z "$correlation" ] || command+=("$correlation%")
		fi
		[ "$(jq -r '.ecn // false' <<<"$shaping")" != true ] || \
			command+=(ecn)
	fi

	for impairment in duplicate corrupt; do
		value=$(jq -r ".${impairment}Percent // empty" <<<"$shaping")
		if [ -n "$value" ]; then
			command+=("$impairment" "$value%")
			correlation=$(jq -r ".${impairment}CorrelationPercent // empty" \
				<<<"$shaping")
			[ -z "$correlation" ] || command+=("$correlation%")
		fi
	done

	value=$(jq -r '.reorderPercent // empty' <<<"$shaping")
	if [ -n "$value" ]; then
		command+=(reorder "$value%")
		correlation=$(jq -r '.reorderCorrelationPercent // empty' \
			<<<"$shaping")
		[ -z "$correlation" ] || command+=("$correlation%")
		value=$(jq -r '.reorderGap // empty' <<<"$shaping")
		[ -z "$value" ] || command+=(gap "$value")
	fi

	value=$(jq -r '.bandwidth // empty' <<<"$shaping")
	if [ -n "$value" ]; then
		command+=(rate "$value")
		overhead=$(jq -r '.packetOverheadBytes // empty' <<<"$shaping")
		if [ -n "$overhead" ]; then
			command+=("$overhead")
			cell=$(jq -r '.cellSizeBytes // empty' <<<"$shaping")
			if [ -n "$cell" ]; then
				command+=("$cell")
				value=$(jq -r '.cellOverheadBytes // empty' \
					<<<"$shaping")
				[ -z "$value" ] || command+=("$value")
			fi
		fi
	fi

	value=$(jq -r '.seed // empty' <<<"$shaping")
	if [ -n "$value" ] && [ "${netem_seed_supported:-true}" = true ]; then
		command+=(seed "$value")
	fi
}

print_command()
{
	printf '%q ' "${command[@]}"
	printf '\n'
}
