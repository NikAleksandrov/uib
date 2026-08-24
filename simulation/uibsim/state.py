# SPDX-License-Identifier: Apache-2.0

import datetime
import json
import os
import pathlib
import tempfile


def utc_now():
    return datetime.datetime.now(datetime.timezone.utc).isoformat()


def write_json(path, value):
    from .core import ConfigurationError

    temporary = None
    try:
        with tempfile.NamedTemporaryFile(
                mode="w", encoding="utf-8", dir=path.parent,
                prefix=f".{path.name}.", delete=False) as output:
            temporary = pathlib.Path(output.name)
            json.dump(value, output, indent=2, sort_keys=True)
            output.write("\n")
            output.flush()
            os.fsync(output.fileno())
        temporary.replace(path)
    except OSError as error:
        if temporary is not None:
            temporary.unlink(missing_ok=True)
        raise ConfigurationError(f"cannot write {path}: {error}") from error


def state_path(args):
    from .core import ConfigurationError, RUN_ID

    if not RUN_ID.fullmatch(args.run_id):
        raise ConfigurationError("invalid run ID")
    return pathlib.Path(args.state_dir).expanduser().resolve() / args.run_id


def load_ledger(args):
    from .core import ConfigurationError

    path = state_path(args) / "ledger.json"
    try:
        with path.open(encoding="utf-8") as source:
            ledger = json.load(source)
    except (OSError, json.JSONDecodeError) as error:
        raise ConfigurationError(f"cannot load run ledger {path}: {error}") from error
    if ledger.get("schemaVersion") != 1:
        raise ConfigurationError(f"unsupported run ledger: {path}")
    if ledger.get("runId") != args.run_id or ledger.get("scheduler") != "slurm":
        raise ConfigurationError(f"run ledger ownership mismatch: {path}")
    return path, ledger
