# SPDX-License-Identifier: Apache-2.0

import hashlib
import os
import pathlib
import re
import shutil
import subprocess
import tempfile
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


def debian_package(path):
    from .core import VirtmeError, executable

    dpkg_deb = executable("DPKG_DEB_COMMAND", "dpkg-deb")
    try:
        result = subprocess.run([
            dpkg_deb, "--field", str(path), "Package", "Version",
            "Architecture",
        ], text=True, capture_output=True, timeout=30, check=False)
    except (OSError, subprocess.TimeoutExpired) as error:
        raise VirtmeError(f"cannot inspect Debian package {path}: {error}") from error
    fields = {}
    for line in result.stdout.splitlines():
        name, separator, value = line.partition(": ")
        if separator:
            fields[name] = value
    if result.returncode or set(fields) != {"Package", "Version", "Architecture"}:
        raise VirtmeError(f"invalid Debian package: {path}")
    return fields["Package"], fields["Version"], fields["Architecture"]


def validate_slurm_packages(directory, verify_hashes):
    from .core import SLURM_PACKAGES, VirtmeError

    packages = {}
    try:
        candidates = sorted(directory.glob("*.deb"))
    except OSError as error:
        raise VirtmeError(f"cannot inspect Slurm package cache: {error}") from error
    for path in candidates:
        name, version, architecture = debian_package(path)
        if name in packages:
            raise VirtmeError(f"duplicate cached Slurm package: {name}")
        packages[name] = (path, version, architecture)
    expected = {name: (version, digest)
                for name, version, digest in SLURM_PACKAGES}
    if set(packages) != set(expected):
        return None
    for name, (path, version, architecture) in packages.items():
        expected_version, digest = expected[name]
        if version != expected_version or architecture != "amd64":
            return None
        if verify_hashes and sha256_file(path) != digest:
            return None
    return [packages[name][0] for name, unused_version, unused_digest
            in SLURM_PACKAGES]


def acquire_slurm_packages(cache):
    from .core import SLURM_PACKAGES, SLURM_VERSION, VirtmeError, executable, write_json

    override = os.environ.get("VIRTME_SLURM_PACKAGE_DIR")
    if override:
        directory = pathlib.Path(override).expanduser().resolve()
        packages = validate_slurm_packages(directory, False)
        if not packages:
            raise VirtmeError(
                f"VIRTME_SLURM_PACKAGE_DIR has an incompatible package set: "
                f"{directory}")
        return packages

    directory = cache / f"slurm-{SLURM_VERSION}"
    packages = validate_slurm_packages(directory, True)
    if packages:
        return packages
    try:
        cache.mkdir(parents=True, exist_ok=True)
        temporary = pathlib.Path(tempfile.mkdtemp(
            prefix=f".slurm-{SLURM_VERSION}.", dir=cache))
    except OSError as error:
        raise VirtmeError(f"cannot create Slurm package cache: {error}") from error
    apt_get = executable("APT_GET_COMMAND", "apt-get")
    specifications = [f"{name}={version}"
                      for name, version, unused_digest in SLURM_PACKAGES]
    try:
        result = subprocess.run(
            [apt_get, "download", *specifications], cwd=temporary,
            timeout=300, check=False)
        if result.returncode:
            raise VirtmeError("cannot download the pinned Slurm package set")
        packages = validate_slurm_packages(temporary, True)
        if not packages:
            raise VirtmeError(
                "downloaded Slurm package set failed metadata or SHA-256 "
                "validation")
        directory.mkdir(parents=True, exist_ok=True)
        for stale in directory.glob("*.deb"):
            stale.unlink()
        installed = []
        for path in packages:
            destination = directory / path.name
            path.replace(destination)
            installed.append(destination)
        manifest = {
            "schemaVersion": 1,
            "slurmVersion": SLURM_VERSION,
            "packages": [
                {"name": name, "version": version, "sha256": digest}
                for name, version, digest in SLURM_PACKAGES
            ],
        }
        write_json(directory / "manifest.json", manifest)
        return installed
    finally:
        shutil.rmtree(temporary, ignore_errors=True)


def extract_slurm_runtime(packages, cache):
    from .core import SLURM_VERSION, VirtmeError, executable

    runtime = cache / f"slurm-{SLURM_VERSION}-runtime"
    required = [
        runtime / "usr/bin/srun",
        runtime / "usr/bin/scontrol",
        runtime / "usr/sbin/munged",
        runtime / "usr/sbin/slurmctld-wlm",
        runtime / "usr/sbin/slurmd-wlm",
        runtime / "usr/sbin/slurmstepd-wlm",
        runtime / "usr/lib/x86_64-linux-gnu/slurm-wlm/libslurmfull.so",
    ]
    commands = {
        "slurmctld": "slurmctld-wlm",
        "slurmd": "slurmd-wlm",
        "slurmstepd": "slurmstepd-wlm",
    }
    if all(path.is_file() for path in required):
        for name, target in commands.items():
            link = runtime / "usr/sbin" / name
            if not link.exists():
                link.symlink_to(target)
        return runtime
    try:
        temporary = pathlib.Path(tempfile.mkdtemp(
            prefix=f".slurm-runtime-{SLURM_VERSION}.", dir=cache))
    except OSError as error:
        raise VirtmeError(f"cannot create Slurm runtime cache: {error}") from error
    dpkg_deb = executable("DPKG_DEB_COMMAND", "dpkg-deb")
    try:
        for package in packages:
            result = subprocess.run(
                [dpkg_deb, "--extract", str(package), str(temporary)],
                timeout=60, check=False)
            if result.returncode:
                raise VirtmeError(f"cannot extract Slurm package: {package}")
        if runtime.exists():
            shutil.rmtree(runtime)
        temporary.replace(runtime)
        if not all(path.is_file() for path in required):
            raise VirtmeError("extracted Slurm runtime is incomplete")
        for name, target in commands.items():
            (runtime / "usr/sbin" / name).symlink_to(target)
        return runtime
    finally:
        if temporary.exists():
            shutil.rmtree(temporary, ignore_errors=True)
