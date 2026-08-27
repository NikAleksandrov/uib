# SPDX-License-Identifier: Apache-2.0

import hashlib
import json
import re


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


def transport_evidence(config, roots):
    suffixes = {".err", ".json", ".jsonl", ".log", ".out", ".txt"}
    maximum_file = 4 * 1024 * 1024
    maximum_total = 32 * 1024 * 1024
    fragments = []
    files = []
    skipped = []
    total = 0
    for label, root in roots:
        if not root.is_dir():
            continue
        for path in sorted(root.rglob("*")):
            if (not path.is_file() or path.is_symlink() or
                    path.suffix.lower() not in suffixes):
                continue
            try:
                size = path.stat().st_size
            except OSError:
                skipped.append(f"{label}/{path.relative_to(root)}")
                continue
            name = f"{label}/{path.relative_to(root)}"
            if size > maximum_file or total + size > maximum_total:
                skipped.append(name)
                continue
            try:
                value = path.read_text(encoding="utf-8", errors="replace")
            except OSError:
                skipped.append(name)
                continue
            fragments.append(value)
            files.append(name)
            total += size
    content = "\n".join(fragments)
    frontend = config["communication"]["frontend"]
    parallel = config["workload"]["parallelism"]
    ranks = parallel["nodes"] * parallel["processesPerNode"]
    matches = []
    forbidden = []

    if not config["uib"]["enabled"]:
        classification = "native-baseline"
    elif ranks < 2:
        classification = "not-exercised-single-rank"
    elif re.search(r'"distributedBackend"\s*:\s*"gloo"',
                   content, re.IGNORECASE):
        classification = "not-exercised-non-rdma-backend"
    else:
        rules = {
            "nccl": (
                {
                    "uib-library":
                        r"(?:NCCL_IBVERBS_LIB|ncclIbverbsLibrary)[^\n]*"
                        r"(?:libuibv\.so|libuibverbs\.so)",
                    "ib-data-path":
                        r"(?:Using network IB|Initialized NET plugin IB|"
                        r"via NET/IB/[0-9]+)",
                },
                {"socket-data-path":
                    r"(?:Using network Socket|via NET/Socket/[0-9]+)"},
            ),
            "rccl": (
                {
                    "uib-library":
                        r"(?:NCCL_IBVERBS_LIB|ncclIbverbsLibrary)[^\n]*"
                        r"(?:libuibv\.so|libuibverbs\.so)",
                    "ib-data-path":
                        r"(?:Using network IB|Initialized NET plugin IB|"
                        r"via NET/IB/[0-9]+)",
                },
                {"socket-data-path":
                    r"(?:Using network Socket|via NET/Socket/[0-9]+)"},
            ),
            "ucx": (
                {"uib-device": r"(?:ud|rc)_verbs/uib0:1"},
                {"software-transport":
                    r"(?:^|[\s,])(?:tcp|posix|sysv|cma)(?:/|[\s,])"},
            ),
            "libfabric": (
                {
                    "uib-device": r"uib0-dgram",
                    "verbs-rxd-provider": r"verbs;ofi_rxd",
                },
                {"software-provider":
                    r"(?:provider|prov_name)[^\n]*(?:sockets|tcp)"},
            ),
        }
        required_rules, forbidden_rules = rules[frontend]
        matches = [name for name, pattern in required_rules.items()
                   if re.search(pattern, content, re.IGNORECASE | re.MULTILINE)]
        forbidden = [name for name, pattern in forbidden_rules.items()
                     if re.search(pattern, content,
                                  re.IGNORECASE | re.MULTILINE)]
        if forbidden:
            classification = "fallback-detected"
        elif len(matches) == len(required_rules):
            classification = "confirmed"
        else:
            classification = "insufficient"

    return {
        "schemaVersion": 1,
        "classification": classification,
        "frontend": frontend,
        "requestedRanks": ranks,
        "checkedFiles": files,
        "skippedFiles": skipped,
        "positiveMatches": matches,
        "forbiddenMatches": forbidden,
        "bytesInspected": total,
        "promotionEligible": False,
    }


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
