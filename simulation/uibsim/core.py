# SPDX-License-Identifier: Apache-2.0

import pathlib


SIMULATION = pathlib.Path(__file__).resolve().parents[1]


CLI = SIMULATION / "bin" / "uib-sim"


PROFILES = SIMULATION / "profiles"


OVERLAYS = SIMULATION / "overlays"


SCHEMA = SIMULATION / "schema" / "simulation.schema.json"


COMPATIBILITY = SIMULATION / "compatibility-lock.json"


KIND_BACKEND = SIMULATION / "backends" / "kind" / "cluster"


KUBERNETES_RENDERER = SIMULATION / "adapters" / "kubernetes" / "render"


class ConfigurationError(Exception):
    pass
