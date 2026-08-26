# SPDX-License-Identifier: Apache-2.0

import hashlib
import os
import pathlib
import re
import shutil
import urllib.request


def sha256_file(path):
    from .core import VirtmeError

    digest = hashlib.sha256()
    try:
        with path.open("rb") as source:
            for block in iter(lambda: source.read(1024 * 1024), b""):
                digest.update(block)
    except OSError as error:
        raise VirtmeError(f"cannot hash {path}: {error}") from error
    return digest.hexdigest()


def acquire_k3s(config, cache):
    from .core import VirtmeError

    override = os.environ.get("K3S_BINARY")
    if override:
        path = pathlib.Path(override).expanduser().resolve()
        if not path.is_file() or not os.access(path, os.X_OK):
            raise VirtmeError(f"K3S_BINARY is not executable: {path}")
        return path
    version = config["cluster"]["version"]
    encoded = urllib.parse.quote(version, safe="")
    base = f"https://github.com/k3s-io/k3s/releases/download/{encoded}"
    destination = cache / f"k3s-{version.replace('+', '_')}"
    checksum_path = pathlib.Path(str(destination) + ".sha256")
    try:
        if destination.is_file() and checksum_path.is_file():
            expected = checksum_path.read_text(encoding="utf-8").strip()
            if re.fullmatch(r"[0-9a-f]{64}", expected):
                if sha256_file(destination) == expected:
                    return destination
        cache.mkdir(parents=True, exist_ok=True)
        with urllib.request.urlopen(
                f"{base}/sha256sum-amd64.txt", timeout=60) as response:
            checksums = response.read().decode("utf-8")
        expected = next(
            line.split()[0] for line in checksums.splitlines()
            if len(line.split()) >= 2 and line.split()[-1] == "k3s")
        if not re.fullmatch(r"[0-9a-f]{64}", expected):
            raise ValueError("invalid checksum")
        temporary = pathlib.Path(str(destination) + ".part")
        temporary.unlink(missing_ok=True)
        with urllib.request.urlopen(f"{base}/k3s", timeout=60) as response, \
                temporary.open("xb") as output:
            shutil.copyfileobj(response, output)
        if sha256_file(temporary) != expected:
            temporary.unlink(missing_ok=True)
            raise VirtmeError("downloaded k3s binary failed SHA-256 validation")
        temporary.chmod(0o755)
        temporary.replace(destination)
        checksum_path.write_text(expected + "\n", encoding="utf-8")
    except (OSError, StopIteration, urllib.error.URLError,
            UnicodeError, ValueError) as error:
        raise VirtmeError(f"cannot acquire pinned k3s binary: {error}") from error
    return destination
