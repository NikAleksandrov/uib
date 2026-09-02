# SPDX-License-Identifier: Apache-2.0

import os
import pathlib
import re
import signal
import subprocess


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


IMAGE_PLATFORM = re.compile(
    r"^[a-z0-9]+/[a-z0-9_]+(?:/[A-Za-z0-9._-]+)?$")


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


class LifecycleInterrupted(ConfigurationError):
    def __init__(self, signum):
        super().__init__(f"interrupted by {signal.Signals(signum).name}")
        self.signum = signum


def interrupt_lifecycle(signum, unused_frame):
    raise LifecycleInterrupted(signum)


def terminate_command(process):
    if process.poll() is not None:
        return
    try:
        os.killpg(process.pid, signal.SIGTERM)
    except ProcessLookupError:
        return
    try:
        process.wait(timeout=30)
    except subprocess.TimeoutExpired:
        try:
            os.killpg(process.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        process.wait()


def run_command(command, **kwargs):
    input_value = kwargs.pop("input", None)
    timeout = kwargs.pop("timeout", None)
    text = kwargs.pop("text", False)
    capture = kwargs.pop("capture_output", False)
    kwargs.pop("check", None)
    if capture:
        if "stdout" in kwargs or "stderr" in kwargs:
            raise ValueError("capture_output conflicts with stdout or stderr")
        kwargs["stdout"] = subprocess.PIPE
        kwargs["stderr"] = subprocess.PIPE
    if input_value is not None:
        if "stdin" in kwargs:
            raise ValueError("input conflicts with stdin")
        kwargs["stdin"] = subprocess.PIPE
    process = subprocess.Popen(
        command, text=text, start_new_session=True, **kwargs)
    try:
        stdout, stderr = process.communicate(
            input=input_value, timeout=timeout)
    except LifecycleInterrupted:
        terminate_command(process)
        raise
    except subprocess.TimeoutExpired:
        terminate_command(process)
        raise
    return subprocess.CompletedProcess(
        command, process.returncode, stdout, stderr)


def run_backend(command):
    try:
        process = subprocess.Popen(command, start_new_session=True)
    except OSError as error:
        raise ConfigurationError(
            f"cannot invoke {pathlib.Path(command[0]).name}: {error}") from error
    try:
        return process.wait()
    except LifecycleInterrupted:
        try:
            os.killpg(process.pid, signal.SIGTERM)
        except ProcessLookupError:
            pass
        try:
            process.wait(timeout=300)
        except subprocess.TimeoutExpired:
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            process.wait()
        raise
