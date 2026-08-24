# SPDX-License-Identifier: Apache-2.0

import hashlib


def file_manifest(root):
    from .core import ConfigurationError

    files = []
    for path in sorted(root.rglob("*")):
        if not path.is_file() or path.name == "manifest.json":
            continue
        digest = hashlib.sha256()
        try:
            with path.open("rb") as source:
                for block in iter(lambda: source.read(1024 * 1024), b""):
                    digest.update(block)
            size = path.stat().st_size
        except OSError as error:
            raise ConfigurationError(f"cannot hash result {path}: {error}") from error
        files.append({
            "path": str(path.relative_to(root)),
            "sha256": digest.hexdigest(),
            "size": size,
        })
    return {"schemaVersion": 1, "algorithm": "sha256", "files": files}
