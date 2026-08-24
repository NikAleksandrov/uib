# SPDX-License-Identifier: Apache-2.0

import os
import pathlib
import re
import shlex
import subprocess


def ldd_paths(path, library_dir):
    from .core import command

    environment = os.environ.copy()
    library_path = environment.get("LD_LIBRARY_PATH")
    environment["LD_LIBRARY_PATH"] = str(library_dir)
    if library_path:
        environment["LD_LIBRARY_PATH"] += os.pathsep + library_path
    output = command("ldd", str(path), env=environment)
    paths = []
    for line in output.splitlines():
        match = re.match(r"\s*\S+\s+=>\s+(/\S+)\s+\(", line)
        if not match:
            match = re.match(r"\s*(/\S+)\s+\(", line)
        if match:
            paths.append(pathlib.Path(match.group(1)))
        elif "not found" in line:
            raise RuntimeError(f"unresolved dependency for {path}: {line.strip()}")
    return paths


def copy_dependencies(inputs, library_dir):
    from .core import PLATFORM_LIBRARIES, copy_file, digest

    copied = {}
    for binary in inputs:
        for source in ldd_paths(binary, library_dir):
            name = source.name
            if (
                name in PLATFORM_LIBRARIES
                or name in ("libuib.so", "libuib.so.0", "libuibv.so")
                or name.startswith("ld-linux-")
            ):
                continue
            real_source = source.resolve()
            real_name = real_source.name
            destination = library_dir / real_name
            source_digest = digest(real_source)
            if destination.exists() and digest(destination) != source_digest:
                raise RuntimeError(f"dependency name collision: {real_name}")
            if not destination.exists():
                copy_file(real_source, destination, 0o755)
            if name != real_name:
                link = library_dir / name
                if link.exists() and link.resolve() != destination:
                    raise RuntimeError(f"dependency link collision: {name}")
                if not link.exists():
                    link.symlink_to(real_name)
            if name not in copied:
                copied[name] = {
                    "bundledPath": f"lib/{name}",
                    "sha256": source_digest,
                    "sourcePath": str(real_source),
                }
    return [copied[name] for name in sorted(copied)]


def config_values():
    from .core import PROJECT_DIR

    config = {}
    path = PROJECT_DIR / "config.mk"
    if not path.exists():
        return config
    for line in path.read_text().splitlines():
        match = re.match(r"(HAVE_[A-Z0-9_]+|ENABLE_PROVIDER)\s*=\s*(.*)", line)
        if match:
            config[match.group(1)] = match.group(2).strip()
    return config


def configure_options():
    from .core import PROJECT_DIR

    config_status = PROJECT_DIR / "config.status"
    if not config_status.exists():
        return []
    result = subprocess.run(
        [str(config_status), "--config"],
        cwd=PROJECT_DIR,
        check=True,
        text=True,
        stdout=subprocess.PIPE,
    )
    return shlex.split(result.stdout)


def glibc_requirement(paths):
    from .core import command

    versions = set()
    for path in paths:
        output = command("readelf", "--version-info", str(path))
        versions.update(re.findall(r"GLIBC_(\d+\.\d+)", output))
    return max(versions, key=lambda value: tuple(map(int, value.split("."))))
