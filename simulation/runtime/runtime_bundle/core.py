# SPDX-License-Identifier: Apache-2.0

import hashlib
import pathlib
import shutil
import subprocess


SCRIPT_DIR = pathlib.Path(__file__).resolve().parents[1]


PROJECT_DIR = SCRIPT_DIR.parent.parent


PLATFORM_LIBRARIES = {
    "ld-linux-x86-64.so.2",
    "libc.so.6",
    "libdl.so.2",
    "libm.so.6",
    "libpthread.so.0",
    "librt.so.1",
}


STATIC_DEVELOPMENT_PACKAGES = (
    "libmnl-dev",
    "liburcu-dev",
    "liburing-dev",
    "zlib1g-dev",
)


def command(*args, env=None):
    return subprocess.run(
        args,
        cwd=PROJECT_DIR,
        env=env,
        check=True,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
    ).stdout.strip()


def digest(path):
    value = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            value.update(chunk)
    return value.hexdigest()


def copy_file(source, destination, mode=None):
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source, destination)
    if mode is not None:
        destination.chmod(mode)
