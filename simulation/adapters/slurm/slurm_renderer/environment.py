# SPDX-License-Identifier: Apache-2.0

def environment(config):
	from .core import RenderError, active_mtu, string_value

	uib = config["uib"]
	collective = config["communication"]["nccl"]
	values = {
		"GLOO_SOCKET_IFNAME": config["network"]["dataInterface"],
		"HF_HOME": "/cache/huggingface",
		"MASTER_PORT": string_value(config["slurm"].get("rendezvousPort", 29500)),
		"NCCL_DEBUG": "INFO",
		"NCCL_DEBUG_SUBSYS": "INIT,NET",
		"NCCL_IB_DISABLE": "0",
		"NCCL_IB_HCA": collective["hca"],
		"NCCL_NET": collective["network"],
	}
	protected_environment = {
		"GLOO_SOCKET_IFNAME",
		"NCCL_IBVERBS_LIB",
		"NCCL_IB_DISABLE",
		"NCCL_IB_HCA",
		"NCCL_NET",
	}
	for name, value in collective.get("environment", {}).items():
		if name in protected_environment:
			raise RenderError(
				f"{name} is controlled by the resolved communication layer"
			)
		values[name] = string_value(value)

	if uib["enabled"]:
		values.update({
			"NCCL_IBVERBS_LIB": "/opt/uib/lib/libuibv.so",
			"SLURM_UIB_BIND_PORT_BASE": string_value(uib["bindPort"]),
			"SLURM_UIB_DATA_INTERFACE": config["network"]["dataInterface"],
			"SLURM_UIB_NETWORK_MTU": string_value(config["network"]["mtu"]),
			"UIB_ACTIVE_MTU": string_value(
				active_mtu(config["network"]["mtu"])),
			"UIB_PROTOCOL": uib["protocol"],
			"UIB_STATS_OUTPUT": "stderr",
			"UIB_TRANSPORT_MODE": uib["transport"],
			"UIB_ZEROCOPY": "off",
		})
		cc = uib["congestionControl"]
		cc_prefix = ("UIB_UET" if uib["protocol"] == "uet" else
			     "UIB_ROCEV2")
		values[f"{cc_prefix}_CC"] = cc["algorithm"]
		maximum = cc["parameters"].get("maxWindowBytes")
		if maximum is not None:
			values[f"{cc_prefix}_NSCC_MAX_WND"] = string_value(maximum)
		protected_uib = {
			"UIB_ACTIVE_MTU",
			"UIB_BIND_IP",
			"UIB_BIND_PORT",
			"UIB_PROTOCOL",
			"UIB_STATS_OUTPUT",
			"UIB_TRANSPORT_MODE",
			"UIB_UET_CC",
			"UIB_UET_NSCC_MAX_WND",
			"UIB_ROCEV2_CC",
			"UIB_ROCEV2_NSCC_MAX_WND",
			"UIB_ZEROCOPY",
		}
		for name, value in uib.get("environment", {}).items():
			if name in protected_uib:
				raise RenderError(
					f"{name} is controlled by the resolved UIB layer"
				)
			values[name] = string_value(value)
	return values


def container_mounts(config):
	storage = config["storage"]
	mounts = [f"{storage['modelCachePath']}:/cache"]

	if config["uib"]["enabled"]:
		mounts.insert(0, f"{storage['uibRuntimePath']}:/opt/uib:ro")
		if config["communication"]["frontend"] in ("ucx", "libfabric"):
			mounts.append("/dev/null:/dev/infiniband/uib0")
	return mounts


def directives(config, run_id):
	from .core import slurm_memory

	slurm = config["slurm"]
	parallel = config["workload"]["parallelism"]
	nodes = config["nodes"]
	result = [
		f"#SBATCH --job-name={run_id}",
		f"#SBATCH --partition={slurm['partition']}",
		f"#SBATCH --nodes={parallel['nodes']}",
		f"#SBATCH --ntasks={parallel['nodes'] * parallel['processesPerNode']}",
		f"#SBATCH --ntasks-per-node={parallel['processesPerNode']}",
		f"#SBATCH --cpus-per-task={nodes['cpu'] // parallel['processesPerNode']}",
		f"#SBATCH --time={slurm['timeLimit']}",
		f"#SBATCH --output={config['storage']['resultsPath']}/%x-%j.out",
		f"#SBATCH --error={config['storage']['resultsPath']}/%x-%j.err",
	]
	if "memory" in slurm:
		result.insert(7, f"#SBATCH --mem={slurm_memory(slurm['memory'])}")
	for key, option in (
		("account", "account"),
		("qos", "qos"),
		("reservation", "reservation"),
		("constraint", "constraint"),
	):
		if key in slurm:
			result.append(f"#SBATCH --{option}={slurm[key]}")
	if nodes["gpus"]["perWorker"]:
		result.append(f"#SBATCH --gpus-per-node={nodes['gpus']['perWorker']}")
		result.append(
			"#SBATCH --gpus-per-task="
			f"{nodes['gpus']['perWorker'] // parallel['processesPerNode']}"
		)
	if slurm.get("exclusive", False):
		result.append("#SBATCH --exclusive")
	return result
