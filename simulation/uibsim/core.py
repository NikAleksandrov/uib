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


KUBERNETES_RENDERER = SIMULATION / "adapters" / "kubernetes" / "render"


SLURM_RENDERER = SIMULATION / "adapters" / "slurm" / "render"


DEFAULT_STATE_DIR = SIMULATION / ".artifacts" / "runs"


RUN_ID = re.compile(r"^[a-z](?:[a-z0-9-]{1,46}[a-z0-9])$")


class ConfigurationError(Exception):
    pass
