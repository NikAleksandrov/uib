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


LIBVIRT_BACKEND = SIMULATION / "backends" / "libvirt" / "cluster"


KUBERNETES_RENDERER = SIMULATION / "adapters" / "kubernetes" / "render"


SLURM_RENDERER = SIMULATION / "adapters" / "slurm" / "render"


DEFAULT_STATE_DIR = SIMULATION / ".artifacts" / "runs"


DEFAULT_RESULT_DIR = SIMULATION / ".artifacts" / "results"


RUN_ID = re.compile(r"^[a-z](?:[a-z0-9-]{1,46}[a-z0-9])$")


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


class ConfigurationError(Exception):
    pass
