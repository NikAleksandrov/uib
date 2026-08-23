# SPDX-License-Identifier: Apache-2.0

import gzip
import os
import shutil
import tarfile


def write_checksums(root):
    from .core import digest

    checksum_path = root / "share/uib/SHA256SUMS"
    paths = [
        path for path in sorted(root.rglob("*"))
        if path.is_file() and not path.is_symlink() and path != checksum_path
    ]
    checksum_path.write_text("".join(
        f"{digest(path)}  {path.relative_to(root)}\n" for path in paths
    ))


def write_archive(root, archive_path):
    archive_path.parent.mkdir(parents=True, exist_ok=True)
    timestamp = int(os.environ.get("SOURCE_DATE_EPOCH", "0"))
    with archive_path.open("wb") as output:
        with gzip.GzipFile(filename="", fileobj=output, mode="wb",
                           mtime=timestamp) as compressed:
            with tarfile.open(fileobj=compressed, mode="w",
                              format=tarfile.PAX_FORMAT) as archive:
                for path in sorted(root.rglob("*")):
                    name = f"uib-runtime/{path.relative_to(root)}"
                    info = archive.gettarinfo(path, arcname=name)
                    info.uid = 0
                    info.gid = 0
                    info.uname = "root"
                    info.gname = "root"
                    info.mtime = timestamp
                    if path.is_file():
                        with path.open("rb") as source:
                            archive.addfile(info, source)
                    else:
                        archive.addfile(info)


def replace_output(staging, output, replace):
    from .core import PROJECT_DIR

    if output.exists():
        if not replace:
            raise RuntimeError(f"output already exists: {output}")
        marker = output / "share/uib/runtime-manifest.json"
        artifact_root = PROJECT_DIR / "simulation/.artifacts"
        if not marker.exists() and artifact_root not in output.parents:
            raise RuntimeError(f"refusing to replace unrecognized output: {output}")
        shutil.rmtree(output)
    output.parent.mkdir(parents=True, exist_ok=True)
    staging.rename(output)
