# SPDX-License-Identifier: Apache-2.0

import ctypes
import importlib.metadata
import pathlib


def package_version(name):
	try:
		return importlib.metadata.version(name)
	except importlib.metadata.PackageNotFoundError:
		return None


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


def capabilities():
	import platform
	import torch

	return {
		"schemaVersion": 1,
		"python": platform.python_version(),
		"torch": torch.__version__,
		"cudaBuild": torch.version.cuda,
		"cudaAvailable": torch.cuda.is_available(),
		"cudaDeviceCount": torch.cuda.device_count(),
		"vllm": package_version("vllm"),
		"ray": package_version("ray"),
		"transformers": package_version("transformers"),
		"ncclPackage": package_version("nvidia-nccl-cu13"),
		"ncclLibrary": nccl_library_version(),
		"backends": {
			"transformers": "cpu-functional-smoke",
			"vllm": "production-gpu-serving",
		},
	}
