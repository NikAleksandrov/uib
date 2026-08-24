# SPDX-License-Identifier: Apache-2.0

import os


def slurm_command(variable, default):
    return os.environ.get(variable, default)
