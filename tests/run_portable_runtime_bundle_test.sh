#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
set -eu

project_dir=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
artifact_dir=${RUNTIME_ARTIFACT_DIR:-$project_dir/simulation/.artifacts}
bundle=$artifact_dir/uib-runtime
manifest=$bundle/share/uib/runtime-manifest.json

"$bundle/bin/uib-preflight" --library-only
test -s "$artifact_dir/uib-runtime.tar.gz"

python3 - "$bundle" "$manifest" <<'PY'
import json
import pathlib
import sys


bundle = pathlib.Path(sys.argv[1])
manifest = json.loads(pathlib.Path(sys.argv[2]).read_text())
assert manifest["artifactType"] == "uib-runtime"
assert tuple(map(int, manifest["build"]["minimumGlibc"].split("."))) <= (2, 35)
assert "Ubuntu" in manifest["build"]["libc"]
assert "2.35" in manifest["build"]["libc"]
capabilities = manifest["uib"]["capabilities"]
assert capabilities["ENABLE_PROVIDER"] == "1"
assert capabilities["HAVE_AFXDP"] == "0"
assert capabilities["HAVE_DEVMEM"] == "0"
assert capabilities["HAVE_DMABUF"] == "0"
assert capabilities["HAVE_URING_ZCRX"] == "0"
sources = {item["name"]: item for item in manifest["sourceDependencies"]}
assert sources["libmnl"]["version"] == "1.0.5"
assert sources["liburing"]["version"] == "2.9"
for source in sources.values():
    assert len(source["sha256"]) == 64
    assert (bundle / source["license"]).is_file()
PY

echo "portable UIB runtime bundle test: PASS"
