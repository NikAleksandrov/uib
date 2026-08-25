# SPDX-License-Identifier: Apache-2.0

def environment(config):
	from .core import RenderError, string_value

	uib = config["uib"]
	collective = config["communication"]["nccl"]
	values = {
		"HF_HOME": "/cache/huggingface",
		"MASTER_PORT": string_value(config["slurm"].get("rendezvousPort", 29500)),
		"NCCL_DEBUG": "INFO",
		"NCCL_DEBUG_SUBSYS": "INIT,NET",
		"NCCL_IB_DISABLE": "0",
		"NCCL_IB_HCA": collective["hca"],
		"NCCL_NET": collective["network"],
	}
	protected_nccl = {
		"NCCL_IBVERBS_LIB",
		"NCCL_IB_DISABLE",
		"NCCL_IB_HCA",
		"NCCL_NET",
	}
	for name, value in collective.get("environment", {}).items():
		if name in protected_nccl:
			raise RenderError(
				f"{name} is controlled by the resolved communication layer"
			)
		values[name] = string_value(value)

	if uib["enabled"]:
		values.update({
			"NCCL_IBVERBS_LIB": "/opt/uib/lib/libuibv.so",
			"SLURM_UIB_BIND_PORT_BASE": string_value(uib["bindPort"]),
			"SLURM_UIB_DATA_INTERFACE": config["network"]["dataInterface"],
			"UIB_PROTOCOL": uib["protocol"],
			"UIB_TRANSPORT_MODE": uib["transport"],
			"UIB_ZEROCOPY": "off",
		})
		if uib["protocol"] == "uet":
			cc = uib["congestionControl"]
			values["UIB_UET_CC"] = cc["algorithm"]
			maximum = cc["parameters"].get("maxWindowBytes")
			if maximum is not None:
				values["UIB_UET_NSCC_MAX_WND"] = string_value(maximum)
		protected_uib = {
			"UIB_BIND_IP",
			"UIB_BIND_PORT",
			"UIB_PROTOCOL",
			"UIB_TRANSPORT_MODE",
			"UIB_UET_CC",
			"UIB_UET_NSCC_MAX_WND",
			"UIB_ZEROCOPY",
		}
		for name, value in uib.get("environment", {}).items():
			if name in protected_uib:
				raise RenderError(
					f"{name} is controlled by the resolved UIB layer"
				)
			values[name] = string_value(value)
	return values


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
