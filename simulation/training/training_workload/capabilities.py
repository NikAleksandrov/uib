# SPDX-License-Identifier: Apache-2.0

import ctypes
import importlib.metadata
import pathlib
import platform
import torch


def package_version(name):
	try:
		return importlib.metadata.version(name)
	except importlib.metadata.PackageNotFoundError:
		return None


def capabilities():
	nccl_build = None
	if torch.cuda.is_available():
		try:
			nccl_build = ".".join(
				str(value) for value in torch.cuda.nccl.version()
			)
		except (AttributeError, TypeError):
			pass
	return {
		"schemaVersion": 1,
		"python": platform.python_version(),
		"torch": torch.__version__,
		"transformers": package_version("transformers"),
		"datasets": package_version("datasets"),
		"cudaBuild": torch.version.cuda,
		"cudaAvailable": torch.cuda.is_available(),
		"cudaDeviceCount": torch.cuda.device_count(),
		"ncclPackage": package_version("nvidia-nccl-cu13"),
		"ncclLibrary": nccl_library_version(),
		"pytorchNcclBuild": nccl_build,
		"strategies": {
			"ddp": "available",
			"fsdp": "available-with-cuda"
		},
		"optionalAdapters": {
			"deepspeed": package_version("deepspeed"),
			"megatron-core": package_version("megatron-core")
		}
	}


def nccl_library_version():
	try:
		import nvidia.nccl

		path = pathlib.Path(nvidia.nccl.__path__[0]) / "lib/libnccl.so.2"
		library = ctypes.CDLL(str(path))
		version = ctypes.c_int()
		if library.ncclGetVersion(ctypes.byref(version)) != 0:
			return None
		value = version.value
		return f"{value // 10000}.{value % 10000 // 100}.{value % 100}"
	except (ImportError, OSError):
		return None
