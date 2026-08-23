# SPDX-License-Identifier: Apache-2.0

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
