# SPDX-License-Identifier: Apache-2.0

import pathlib
import re


SIMULATION = pathlib.Path(__file__).resolve().parents[1]


CLI = SIMULATION / "bin" / "uib-sim"


PROFILES = SIMULATION / "profiles"


OVERLAYS = SIMULATION / "overlays"


SCHEMA = SIMULATION / "schema" / "simulation.schema.json"


COMPATIBILITY = SIMULATION / "compatibility-lock.json"


KIND_BACKEND = SIMULATION / "backends" / "kind" / "cluster"


VIRTME_BACKEND = SIMULATION / "backends" / "virtme" / "cluster"


LIBVIRT_BACKEND = SIMULATION / "backends" / "libvirt" / "cluster"


KUBERNETES_RENDERER = SIMULATION / "adapters" / "kubernetes" / "render"


SLURM_RENDERER = SIMULATION / "adapters" / "slurm" / "render"


DEFAULT_STATE_DIR = SIMULATION / ".artifacts" / "runs"


DEFAULT_RESULT_DIR = SIMULATION / ".artifacts" / "results"


DEFAULT_SLURM_IMAGE_CACHE = SIMULATION / ".artifacts" / "slurm-images"


DEFAULT_HUGGINGFACE_CACHE = SIMULATION / ".artifacts" / "huggingface"


DEFAULT_SLURM_CACHE_INVENTORY = SIMULATION / ".artifacts" / "slurm-caches"


RUN_ID = re.compile(r"^[a-z](?:[a-z0-9-]{1,46}[a-z0-9])$")


IMAGE_REFERENCE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._/@:+-]*$")


IMAGE_DIGEST = re.compile(r"^\S+@sha256:[0-9a-f]{64}$")


HEX_DIGEST = re.compile(r"^[0-9a-f]{64}$")


SLURM_TERMINAL_STATES = {
    "BOOT_FAIL",
    "CANCELLED",
    "COMPLETED",
    "DEADLINE",
    "FAILED",
    "NODE_FAIL",
    "OUT_OF_MEMORY",
    "PREEMPTED",
    "REVOKED",
    "TIMEOUT",
}


KUBERNETES_TERMINAL_STATES = {"COMPLETED", "FAILED"}


class ConfigurationError(Exception):
    pass
