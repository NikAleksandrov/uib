# SPDX-License-Identifier: Apache-2.0

import datetime
import json
import os
import pathlib
import shutil
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


def copy_file(source, destination):
    from .core import ConfigurationError

    try:
        destination.parent.mkdir(parents=True, exist_ok=True)
        with source.open("rb") as input_file, destination.open("xb") as output_file:
            shutil.copyfileobj(input_file, output_file)
    except OSError as error:
        raise ConfigurationError(
            f"cannot copy {source} to {destination}: {error}") from error


def record_run_inputs(args, run_dir):
    from .config import find_overlay, find_profile

    inputs = run_dir / "inputs"
    profile = find_profile(args.profile)
    record = {
        "profile": "inputs/profile.yaml",
        "overlays": [],
        "overrides": list(args.set),
    }
    copy_file(profile, inputs / "profile.yaml")
    if args.request:
        request = pathlib.Path(args.request).expanduser().resolve()
        copy_file(request, inputs / "request.yaml")
        record["request"] = "inputs/request.yaml"
    for index, name in enumerate(args.overlay):
        destination = inputs / "overlays" / f"{index:02d}.yaml"
        copy_file(find_overlay(name), destination)
        record["overlays"].append({
            "name": name,
            "path": str(destination.relative_to(run_dir)),
        })
    return record


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


def read_json(path, description):
    from .core import ConfigurationError

    try:
        with path.open(encoding="utf-8") as source:
            value = json.load(source)
    except (OSError, json.JSONDecodeError) as error:
        raise ConfigurationError(
            f"cannot load {description} {path}: {error}") from error
    if not isinstance(value, dict):
        raise ConfigurationError(f"{description} {path} must contain an object")
    return value


def copy_tree(source, destination):
    from .core import ConfigurationError

    try:
        destination.mkdir(parents=True)
        for entry in sorted(source.rglob("*")):
            relative = entry.relative_to(source)
            target = destination / relative
            if entry.is_symlink():
                raise ConfigurationError(
                    f"result tree contains a symbolic link: {entry}")
            if entry.is_dir():
                target.mkdir()
            elif entry.is_file():
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(entry, target)
            else:
                raise ConfigurationError(
                    f"result tree contains a special file: {entry}")
    except ConfigurationError:
        raise
    except OSError as error:
        raise ConfigurationError(
            f"cannot copy result tree {source}: {error}") from error


def path_is_within(path, parent):
    try:
        path.relative_to(parent)
        return True
    except ValueError:
        return False


def owned_state_file(run_dir, value, description):
    from .core import ConfigurationError

    if not isinstance(value, str):
        raise ConfigurationError(f"invalid {description} in run ledger")
    candidate = (run_dir / value).resolve()
    if candidate == run_dir or not path_is_within(candidate, run_dir):
        raise ConfigurationError(
            f"{description} escapes the run state directory")
    return candidate
