# SPDX-License-Identifier: Apache-2.0

import shlex
import subprocess


def run_network_command(command):
    from .core import VirtmeError, executable

    resolved = executable("IP_COMMAND", "ip")
    actual = [resolved, *command[1:]]
    try:
        result = subprocess.run(actual, check=False)
    except OSError as error:
        raise VirtmeError(f"cannot execute {resolved}: {error}") from error
    if result.returncode:
        raise VirtmeError(
            f"network command failed with status {result.returncode}: "
            f"{shlex.join(command)}")


def create_network(network):
    from .core import VirtmeError

    if network["type"] != "linux-bridge":
        return False
    commands = network["create"]
    run_network_command(commands[0])
    try:
        for command in commands[1:]:
            run_network_command(command)
    except VirtmeError:
        try:
            delete_network(network)
        except VirtmeError:
            pass
        raise
    return True


def delete_network(network):
    if network["type"] == "linux-bridge":
        run_network_command(network["delete"])
