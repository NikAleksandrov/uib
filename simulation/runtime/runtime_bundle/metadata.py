# SPDX-License-Identifier: Apache-2.0

import json
import os
import pathlib
import shutil
import subprocess


def artifact_entries(root):
    from .core import digest

    entries = []
    for path in sorted(root.rglob("*")):
        if path.is_file() and not path.is_symlink():
            entries.append({
                "path": str(path.relative_to(root)),
                "bytes": path.stat().st_size,
                "sha256": digest(path),
            })
    return entries


def dpkg_owner(path):
    resolved = str(path.resolve())
    candidates = [resolved]
    if resolved.startswith("/usr/"):
        candidates.append(resolved[4:])
    for candidate in candidates:
        result = subprocess.run(
            ["dpkg-query", "-S", candidate],
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
        )
        if result.returncode == 0:
            return result.stdout.split(": ", 1)[0].splitlines()[0]
    return None


def package_metadata(paths, share_dir):
    from .core import STATIC_DEVELOPMENT_PACKAGES, copy_file

    if not shutil.which("dpkg-query"):
        return []
    packages = {package for path in paths if (package := dpkg_owner(path))}
    packages.update(STATIC_DEVELOPMENT_PACKAGES)
    metadata = []
    for package in sorted(packages):
        result = subprocess.run(
            ["dpkg-query", "-W", "-f=${Version}", package],
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
        )
        if result.returncode != 0:
            continue
        base_name = package.split(":", 1)[0]
        copyright_path = pathlib.Path("/usr/share/doc") / base_name / "copyright"
        entry = {"name": package, "version": result.stdout}
        if copyright_path.exists():
            destination = share_dir / "licenses/debian" / base_name / "copyright"
            copy_file(copyright_path.resolve(), destination, 0o644)
            entry["copyright"] = str(destination.relative_to(share_dir.parent.parent))
        metadata.append(entry)
    return metadata


def source_dependency_metadata(share_dir):
    from .core import copy_file

    raw = os.environ.get("UIB_SOURCE_DEPENDENCIES", "[]")
    try:
        dependencies = json.loads(raw)
    except json.JSONDecodeError as error:
        raise RuntimeError("UIB_SOURCE_DEPENDENCIES must be valid JSON") from error
    if not isinstance(dependencies, list):
        raise RuntimeError("UIB_SOURCE_DEPENDENCIES must contain a JSON list")
    result = []
    for dependency in dependencies:
        if not isinstance(dependency, dict):
            raise RuntimeError("source dependency entry must be an object")
        entry = dict(dependency)
        license_value = entry.pop("license", None)
        if license_value:
            source = pathlib.Path(license_value)
            if not source.is_file():
                raise RuntimeError(f"source dependency license not found: {source}")
            destination = share_dir / "licenses/source" / entry["name"] / source.name
            copy_file(source, destination, 0o644)
            entry["license"] = str(destination.relative_to(share_dir.parent.parent))
        result.append(entry)
    return result
