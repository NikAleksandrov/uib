# SPDX-License-Identifier: Apache-2.0

import hashlib
import json


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


def collect_fault_evidence(workload, destination):
    from .core import ConfigurationError

    lines = []
    files = 0
    applied_targets = set()
    recovered_targets = set()
    if workload and workload.is_dir():
        for path in sorted(workload.glob("faults-*.jsonl")):
            if path.is_symlink() or not path.is_file():
                raise ConfigurationError(
                    f"invalid fault evidence file: {path}")
            files += 1
            try:
                content = path.read_text(encoding="utf-8").splitlines()
            except OSError as error:
                raise ConfigurationError(
                    f"cannot read fault evidence {path}: {error}") from error
            for number, line in enumerate(content, 1):
                try:
                    record = json.loads(line)
                except json.JSONDecodeError as error:
                    raise ConfigurationError(
                        f"invalid fault evidence {path}:{number}: {error}") \
                        from error
                if (not isinstance(record, dict) or
                        record.get("schemaVersion") != 1 or
                        not isinstance(record.get("phase"), str) or
                        not isinstance(record.get("target"), str) or
                        not isinstance(record.get("result"), str)):
                    raise ConfigurationError(
                        f"incomplete fault evidence {path}:{number}")
                if record["phase"] == "apply":
                    applied_targets.add(record["target"])
                elif record["phase"] == "recover":
                    recovered_targets.add(record["target"])
                record["sourceFile"] = path.name
                lines.append(json.dumps(record, sort_keys=True))
    destination.write_text("\n".join(lines) + ("\n" if lines else ""),
                           encoding="utf-8")
    return files, len(lines), applied_targets, recovered_targets
