# SPDX-License-Identifier: Apache-2.0

import datetime
import json
import os
import pathlib
import platform
import shutil
import subprocess
import tempfile


def prepare_inputs(args):
    from .core import PLATFORM_LIBRARIES, PROJECT_DIR, SCRIPT_DIR, command, copy_file, digest

    if not args.no_build:
        subprocess.run(["make", "all", "static"], cwd=PROJECT_DIR, check=True)
    required = [
        PROJECT_DIR / "libuib.so.0.1.0",
        PROJECT_DIR / "libuibv.so",
        PROJECT_DIR / "libuib-standalone.a",
    ]
    tools = [pathlib.Path(shutil.which(name) or "") for name in
             ("ibv_devices", "ibv_devinfo", "ibv_rc_pingpong", "rdma",
              "readelf", "sha256sum")]
    if any(not path.is_file() for path in required + tools):
        raise RuntimeError("UIB libraries and rdma-core tools must be built and installed")
    return required, tools

def prepare_staging(args):
    output = args.output.resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    archive_path = args.archive.resolve() if args.archive else None
    if archive_path and archive_path.exists() and not args.replace:
        raise RuntimeError(f"archive already exists: {archive_path}")
    work_dir = pathlib.Path(tempfile.mkdtemp(prefix="uib-runtime-", dir=output.parent))
    staging = work_dir / "uib-runtime"
    library_dir = staging / "lib"
    binary_dir = staging / "bin"
    share_dir = staging / "share/uib"
    library_dir.mkdir(parents=True)
    binary_dir.mkdir(parents=True)
    share_dir.mkdir(parents=True)
    return archive_path, binary_dir, library_dir, output, share_dir, staging, work_dir

def stage_payload(binary_dir, library_dir, required, share_dir, staging, tools):
    from .core import PLATFORM_LIBRARIES, PROJECT_DIR, SCRIPT_DIR, command, copy_file, digest

    for source in required:
        copy_file(source, library_dir / source.name,
                  0o644 if source.suffix == ".a" else 0o755)
    (library_dir / "libuib.so.0").symlink_to("libuib.so.0.1.0")
    (library_dir / "libuib.so").symlink_to("libuib.so.0")
    for name in ("libuib.so", "libuibv.so"):
        (staging / name).symlink_to(f"lib/{name}")
    for source in tools:
        copy_file(source, binary_dir / source.name, 0o755)
    copy_file(SCRIPT_DIR / "uib-preflight", binary_dir / "uib-preflight", 0o755)
    copy_file(PROJECT_DIR / "simulation/compatibility-lock.json",
              share_dir / "compatibility-lock.json", 0o644)
    for name in ("LICENSE", "NOTICE", "AUTHORS"):
        copy_file(PROJECT_DIR / name, share_dir / "licenses/uib" / name, 0o644)
    for source in sorted((PROJECT_DIR / "LICENSES").glob("*.txt")):
        copy_file(source, share_dir / "licenses/uib/LICENSES" / source.name,
                  0o644)

def collect_dependencies(binary_dir, library_dir, share_dir, staging, tools):
    from .dependencies import config_values, configure_options, copy_dependencies, glibc_requirement
    from .metadata import artifact_entries, package_metadata, source_dependency_metadata

    dynamic_inputs = [library_dir / "libuib.so", library_dir / "libuibv.so"]
    dynamic_inputs.extend(binary_dir / source.name for source in tools)
    dependencies = copy_dependencies(dynamic_inputs, library_dir)
    glibc_inputs = dynamic_inputs + [
        staging / entry["bundledPath"] for entry in dependencies
    ]
    package_sources = tools + [
        pathlib.Path(entry["sourcePath"]) for entry in dependencies
    ]
    packages = package_metadata(package_sources, share_dir)
    source_dependencies = source_dependency_metadata(share_dir)
    return dependencies, glibc_inputs, packages, source_dependencies

def create_manifest(dependencies, glibc_inputs, packages, share_dir, source_dependencies, staging):
    from .core import PLATFORM_LIBRARIES, PROJECT_DIR, SCRIPT_DIR, command, copy_file, digest
    from .metadata import artifact_entries, package_metadata, source_dependency_metadata
    from .dependencies import config_values, configure_options, copy_dependencies, glibc_requirement

    revision = os.environ.get("UIB_SOURCE_REVISION")
    if revision is None:
        revision = command("git", "rev-parse", "HEAD")
    dirty_value = os.environ.get("UIB_SOURCE_DIRTY")
    if dirty_value is None:
        dirty = bool(command(
            "git", "status", "--porcelain", "--untracked-files=no"
        ))
    elif dirty_value in ("0", "1"):
        dirty = dirty_value == "1"
    else:
        raise RuntimeError("UIB_SOURCE_DIRTY must be 0 or 1")
    built_at = datetime.datetime.now(datetime.timezone.utc).replace(
        microsecond=0).isoformat()
    if "SOURCE_DATE_EPOCH" in os.environ:
        built_at = datetime.datetime.fromtimestamp(
            int(os.environ["SOURCE_DATE_EPOCH"]), datetime.timezone.utc
        ).isoformat()
    manifest = {
        "schemaVersion": 1,
        "artifactType": "uib-runtime",
        "uib": {
            "commit": revision,
            "trackedSourceDirty": dirty,
            "capabilities": config_values(),
        },
        "build": {
            "timestamp": built_at,
            "architecture": platform.machine(),
            "compiler": command("cc", "--version").splitlines()[0],
            "configureOptions": configure_options(),
            "libc": command("ldd", "--version").splitlines()[0],
            "minimumGlibc": glibc_requirement(glibc_inputs),
        },
        "payload": artifact_entries(staging),
        "bundledDependencies": dependencies,
        "packages": packages,
        "sourceDependencies": source_dependencies,
        "platformDependencies": sorted(PLATFORM_LIBRARIES),
        "compatibilityLockSha256": digest(share_dir / "compatibility-lock.json"),
    }
    (share_dir / "runtime-manifest.json").write_text(
        json.dumps(manifest, indent=2, sort_keys=True) + "\n"
    )

def finalize_bundle(archive_path, args, output, staging):
    from .archive import replace_output, write_archive, write_checksums

    write_checksums(staging)
    replace_output(staging, output, args.replace)
    if archive_path:
        write_archive(output, archive_path)

def build(args):
    required, tools = prepare_inputs(args)
    archive_path, binary_dir, library_dir, output, share_dir, staging, work_dir = prepare_staging(args)
    try:
        stage_payload(binary_dir, library_dir, required, share_dir, staging, tools)
        dependencies, glibc_inputs, packages, source_dependencies = collect_dependencies(binary_dir, library_dir, share_dir, staging, tools)
        create_manifest(dependencies, glibc_inputs, packages, share_dir, source_dependencies, staging)
        finalize_bundle(archive_path, args, output, staging)
    finally:
        if work_dir.exists():
            shutil.rmtree(work_dir)
