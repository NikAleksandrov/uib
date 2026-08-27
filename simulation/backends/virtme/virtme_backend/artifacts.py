# SPDX-License-Identifier: Apache-2.0

import hashlib
import os
import pathlib
import re
import shutil
import subprocess
import tarfile
import tempfile
import urllib.request


def configure_slurm_storage(config, topology_value, state):
    from .core import DEFAULT_IMAGE_CACHE, DEFAULT_MODEL_CACHE, DEFAULT_RUNTIME, VirtmeError, quantity_mib

    runtime = pathlib.Path(os.environ.get(
        "UIB_RUNTIME_SOURCE", str(DEFAULT_RUNTIME))).expanduser().resolve()
    required = [
        runtime / "bin/ibv_rc_pingpong",
        runtime / "bin/uib-preflight",
        runtime / "lib/libuibv.so",
        runtime / "share/uib/runtime-manifest.json",
    ]
    if (config["uib"]["enabled"] and
            not all(path.is_file() for path in required)):
        raise VirtmeError(
            "portable UIB runtime is missing or incomplete; run "
            "make runtime-bundle-portable-test")
    model_cache = pathlib.Path(os.environ.get(
        "VIRTME_MODEL_CACHE", str(DEFAULT_MODEL_CACHE))).expanduser().resolve()
    image_cache = pathlib.Path(os.environ.get(
        "SLURM_IMAGE_CACHE", str(DEFAULT_IMAGE_CACHE))).expanduser().resolve()
    results = state / "shared" / "results"
    enroot = state / "node-storage" / "enroot"
    try:
        image_cache.mkdir(parents=True, exist_ok=True)
        model_cache.mkdir(parents=True, exist_ok=True)
        results.mkdir(parents=True)
        enroot.mkdir(parents=True)
    except OSError as error:
        raise VirtmeError(f"cannot prepare virtme shared storage: {error}") \
            from error
    mappings = {
        "images": {
            "guest": "/var/cache/uib/images",
            "host": str(image_cache),
            "mode": "read-write",
        },
        "modelCache": {
            "guest": config["storage"]["modelCachePath"] + "/huggingface",
            "host": str(model_cache),
            "mode": "read-write",
        },
        "results": {
            "guest": config["storage"]["resultsPath"],
            "host": str(results),
            "mode": "read-write",
        },
    }
    if config["uib"]["enabled"]:
        mappings["uibRuntime"] = {
            "guest": config["storage"]["uibRuntimePath"],
            "host": str(runtime),
            "mode": "read-only",
        }
    enroot_images = {}
    topology_value["nodeStorage"] = {
        "enroot": {
            "guest": "/var/lib/uib/enroot",
            "device": "/dev/vda",
            "filesystem": "ext4",
            "images": enroot_images,
            "mode": "node-local",
            "size": config["nodes"]["disk"],
        },
    }
    for name, guest in topology_value["guests"].items():
        for mapping in mappings.values():
            option = "--rodir" if mapping["mode"] == "read-only" else "--rwdir"
            guest["command"].extend([
                option, f"{mapping['guest']}={mapping['host']}",
            ])
        if guest["role"] != "worker":
            continue
        enroot_image = enroot / f"{name}.ext4"
        enroot_images[name] = str(enroot_image)
        try:
            with enroot_image.open("xb") as output:
                output.truncate(quantity_mib(config["nodes"]["disk"]) * 1024 * 1024)
        except OSError as error:
            raise VirtmeError(
                f"cannot prepare virtme Enroot storage: {error}") from error
        try:
            formatted = subprocess.run([
                "/sbin/mkfs.ext4", "-q", "-F", "-m", "0",
                "-L", f"uib-{name}", str(enroot_image),
            ], check=False)
        except OSError as error:
            raise VirtmeError(f"cannot invoke mkfs.ext4: {error}") from error
        if formatted.returncode:
            raise VirtmeError(
                f"cannot format virtme Enroot storage: {enroot_image}")
        guest["command"].extend(["--disk", str(enroot_image)])
    topology_value["sharedStorage"] = mappings
    return runtime


def sha256_file(path):
    from .core import VirtmeError

    digest = hashlib.sha256()
    try:
        with path.open("rb") as source:
            for block in iter(lambda: source.read(1024 * 1024), b""):
                digest.update(block)
    except OSError as error:
        raise VirtmeError(f"cannot hash {path}: {error}") from error
    return digest.hexdigest()


def acquire_k3s(config, cache):
    from .core import VirtmeError

    override = os.environ.get("K3S_BINARY")
    if override:
        path = pathlib.Path(override).expanduser().resolve()
        if not path.is_file() or not os.access(path, os.X_OK):
            raise VirtmeError(f"K3S_BINARY is not executable: {path}")
        return path
    version = config["cluster"]["version"]
    encoded = urllib.parse.quote(version, safe="")
    base = f"https://github.com/k3s-io/k3s/releases/download/{encoded}"
    destination = cache / f"k3s-{version.replace('+', '_')}"
    checksum_path = pathlib.Path(str(destination) + ".sha256")
    try:
        if destination.is_file() and checksum_path.is_file():
            expected = checksum_path.read_text(encoding="utf-8").strip()
            if re.fullmatch(r"[0-9a-f]{64}", expected):
                if sha256_file(destination) == expected:
                    return destination
        cache.mkdir(parents=True, exist_ok=True)
        with urllib.request.urlopen(
                f"{base}/sha256sum-amd64.txt", timeout=60) as response:
            checksums = response.read().decode("utf-8")
        expected = next(
            line.split()[0] for line in checksums.splitlines()
            if len(line.split()) >= 2 and line.split()[-1] == "k3s")
        if not re.fullmatch(r"[0-9a-f]{64}", expected):
            raise ValueError("invalid checksum")
        temporary = pathlib.Path(str(destination) + ".part")
        temporary.unlink(missing_ok=True)
        with urllib.request.urlopen(f"{base}/k3s", timeout=60) as response, \
                temporary.open("xb") as output:
            shutil.copyfileobj(response, output)
        if sha256_file(temporary) != expected:
            temporary.unlink(missing_ok=True)
            raise VirtmeError("downloaded k3s binary failed SHA-256 validation")
        temporary.chmod(0o755)
        temporary.replace(destination)
        checksum_path.write_text(expected + "\n", encoding="utf-8")
    except (OSError, StopIteration, urllib.error.URLError,
            UnicodeError, ValueError) as error:
        raise VirtmeError(f"cannot acquire pinned k3s binary: {error}") from error
    return destination


def debian_package(path):
    from .core import VirtmeError, executable

    dpkg_deb = executable("DPKG_DEB_COMMAND", "dpkg-deb")
    try:
        result = subprocess.run([
            dpkg_deb, "--field", str(path), "Package", "Version",
            "Architecture",
        ], text=True, capture_output=True, timeout=30, check=False)
    except (OSError, subprocess.TimeoutExpired) as error:
        raise VirtmeError(f"cannot inspect Debian package {path}: {error}") from error
    fields = {}
    for line in result.stdout.splitlines():
        name, separator, value = line.partition(": ")
        if separator:
            fields[name] = value
    if result.returncode or set(fields) != {"Package", "Version", "Architecture"}:
        raise VirtmeError(f"invalid Debian package: {path}")
    return fields["Package"], fields["Version"], fields["Architecture"]


def validate_slurm_packages(directory, verify_hashes):
    from .core import SLURM_PACKAGES, VirtmeError

    packages = {}
    try:
        candidates = sorted(directory.glob("*.deb"))
    except OSError as error:
        raise VirtmeError(f"cannot inspect Slurm package cache: {error}") from error
    for path in candidates:
        name, version, architecture = debian_package(path)
        if name in packages:
            raise VirtmeError(f"duplicate cached Slurm package: {name}")
        packages[name] = (path, version, architecture)
    expected = {name: (version, digest)
                for name, version, digest in SLURM_PACKAGES}
    if set(packages) != set(expected):
        return None
    for name, (path, version, architecture) in packages.items():
        expected_version, digest = expected[name]
        if version != expected_version or architecture not in {"amd64", "all"}:
            return None
        if verify_hashes and sha256_file(path) != digest:
            return None
    return [packages[name][0] for name, unused_version, unused_digest
            in SLURM_PACKAGES]


def acquire_slurm_packages(cache):
    from .core import SLURM_PACKAGES, SLURM_VERSION, VirtmeError, executable, write_json

    override = os.environ.get("VIRTME_SLURM_PACKAGE_DIR")
    if override:
        directory = pathlib.Path(override).expanduser().resolve()
        packages = validate_slurm_packages(directory, False)
        if not packages:
            raise VirtmeError(
                f"VIRTME_SLURM_PACKAGE_DIR has an incompatible package set: "
                f"{directory}")
        return packages

    directory = cache / f"slurm-{SLURM_VERSION}"
    packages = validate_slurm_packages(directory, True)
    if packages:
        return packages
    try:
        cache.mkdir(parents=True, exist_ok=True)
        temporary = pathlib.Path(tempfile.mkdtemp(
            prefix=f".slurm-{SLURM_VERSION}.", dir=cache))
    except OSError as error:
        raise VirtmeError(f"cannot create Slurm package cache: {error}") from error
    apt_get = executable("APT_GET_COMMAND", "apt-get")
    specifications = [f"{name}={version}"
                      for name, version, unused_digest in SLURM_PACKAGES]
    try:
        result = subprocess.run(
            [apt_get, "download", *specifications], cwd=temporary,
            timeout=300, check=False)
        if result.returncode:
            raise VirtmeError("cannot download the pinned Slurm package set")
        packages = validate_slurm_packages(temporary, True)
        if not packages:
            raise VirtmeError(
                "downloaded Slurm package set failed metadata or SHA-256 "
                "validation")
        directory.mkdir(parents=True, exist_ok=True)
        for stale in directory.glob("*.deb"):
            stale.unlink()
        installed = []
        for path in packages:
            destination = directory / path.name
            path.replace(destination)
            installed.append(destination)
        manifest = {
            "schemaVersion": 1,
            "slurmVersion": SLURM_VERSION,
            "packages": [
                {"name": name, "version": version, "sha256": digest}
                for name, version, digest in SLURM_PACKAGES
            ],
        }
        write_json(directory / "manifest.json", manifest)
        return installed
    finally:
        shutil.rmtree(temporary, ignore_errors=True)


def patch_slurm_runtime(runtime):
    from .core import VirtmeError

    patchelf = runtime / "usr/bin/patchelf"
    runpath = (
        f"{runtime}/usr/lib/x86_64-linux-gnu/slurm-wlm:"
        f"{runtime}/usr/lib/x86_64-linux-gnu")
    for name in ("slurmctld-wlm", "slurmd-wlm", "slurmstepd-wlm"):
        binary = runtime / "usr/sbin" / name
        result = subprocess.run(
            [str(patchelf), "--print-rpath", str(binary)],
            text=True, capture_output=True, timeout=30, check=False)
        if result.returncode:
            raise VirtmeError(f"cannot inspect the {name} runtime search path")
        if result.stdout.strip() == runpath:
            continue
        result = subprocess.run(
            [str(patchelf), "--set-rpath", runpath, str(binary)],
            timeout=30, check=False)
        if result.returncode:
            raise VirtmeError(f"cannot set the {name} runtime search path")


def extract_slurm_runtime(packages, cache):
    from .core import SLURM_VERSION, VirtmeError, executable

    runtime = cache / f"slurm-{SLURM_VERSION}-runtime"
    required = [
        runtime / "usr/bin/busybox",
        runtime / "usr/bin/mksquashfs",
        runtime / "usr/bin/srun",
        runtime / "usr/bin/scontrol",
        runtime / "usr/include/slurm/spank.h",
        runtime / "usr/bin/patchelf",
        runtime / "usr/sbin/munged",
        runtime / "usr/sbin/slurmctld-wlm",
        runtime / "usr/sbin/slurmd-wlm",
        runtime / "usr/sbin/slurmstepd-wlm",
        runtime / "usr/lib/x86_64-linux-gnu/slurm-wlm/libslurmfull.so",
    ]
    commands = {
        "slurmctld": "slurmctld-wlm",
        "slurmd": "slurmd-wlm",
        "slurmstepd": "slurmstepd-wlm",
    }
    if all(path.is_file() for path in required):
        patch_slurm_runtime(runtime)
        for name, target in commands.items():
            link = runtime / "usr/sbin" / name
            if not link.exists():
                link.symlink_to(target)
        return runtime
    try:
        temporary = pathlib.Path(tempfile.mkdtemp(
            prefix=f".slurm-runtime-{SLURM_VERSION}.", dir=cache))
    except OSError as error:
        raise VirtmeError(f"cannot create Slurm runtime cache: {error}") from error
    dpkg_deb = executable("DPKG_DEB_COMMAND", "dpkg-deb")
    try:
        for package in packages:
            result = subprocess.run(
                [dpkg_deb, "--extract", str(package), str(temporary)],
                timeout=60, check=False)
            if result.returncode:
                raise VirtmeError(f"cannot extract Slurm package: {package}")
        if runtime.exists():
            shutil.rmtree(runtime)
        temporary.replace(runtime)
        if not all(path.is_file() for path in required):
            raise VirtmeError("extracted Slurm runtime is incomplete")
        patch_slurm_runtime(runtime)
        for name, target in commands.items():
            (runtime / "usr/sbin" / name).symlink_to(target)
        return runtime
    finally:
        if temporary.exists():
            shutil.rmtree(temporary, ignore_errors=True)


def acquire_container_artifacts(cache):
    from .core import CONTAINER_ARTIFACTS, VirtmeError

    directory = cache / "container-stack"
    try:
        directory.mkdir(parents=True, exist_ok=True)
    except OSError as error:
        raise VirtmeError(f"cannot create container artifact cache: {error}") \
            from error
    artifacts = []
    for name, url, digest in CONTAINER_ARTIFACTS:
        destination = directory / name
        if destination.is_file() and sha256_file(destination) == digest:
            artifacts.append(destination)
            continue
        temporary = directory / f".{name}.part"
        temporary.unlink(missing_ok=True)
        try:
            with urllib.request.urlopen(url, timeout=60) as response, \
                    temporary.open("xb") as output:
                shutil.copyfileobj(response, output)
        except (OSError, urllib.error.URLError) as error:
            temporary.unlink(missing_ok=True)
            raise VirtmeError(
                f"cannot download pinned container artifact {name}: {error}") \
                from error
        if sha256_file(temporary) != digest:
            temporary.unlink(missing_ok=True)
            raise VirtmeError(
                f"downloaded container artifact failed SHA-256: {name}")
        temporary.replace(destination)
        artifacts.append(destination)
    return artifacts


def prepare_container_runtime(runtime, cache):
    from .core import PYXIS_COMMIT, PYXIS_VERSION, VirtmeError, executable

    enroot, pyxis = acquire_container_artifacts(cache)
    enroot_binary = runtime / "usr/bin/enroot"
    if not enroot_binary.is_file():
        dpkg_deb = executable("DPKG_DEB_COMMAND", "dpkg-deb")
        result = subprocess.run(
            [dpkg_deb, "--extract", str(enroot), str(runtime)],
            timeout=60, check=False)
        if result.returncode or not enroot_binary.is_file():
            raise VirtmeError("cannot extract the pinned Enroot runtime")

    plugin = (runtime / "usr/lib/x86_64-linux-gnu/slurm-wlm" /
              "spank_pyxis.so")
    if plugin.is_file():
        return runtime
    try:
        build = pathlib.Path(tempfile.mkdtemp(
            prefix=f".pyxis-{PYXIS_VERSION}.", dir=cache))
        with tarfile.open(pyxis, "r:gz") as archive:
            archive.extractall(build, filter="data")
        source = build / f"pyxis-{PYXIS_COMMIT}"
        make = executable("MAKE_COMMAND", "make")
        environment = os.environ.copy()
        environment["CPPFLAGS"] = f"-I{runtime}/usr/include"
        result = subprocess.run(
            [make, "-j", str(max(1, os.cpu_count() or 1))], cwd=source,
            env=environment, timeout=180, check=False)
        if result.returncode:
            raise VirtmeError("cannot build the pinned Pyxis plugin")
        plugin.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source / "spank_pyxis.so", plugin)
    except (OSError, tarfile.TarError) as error:
        raise VirtmeError(f"cannot prepare the Pyxis runtime: {error}") from error
    finally:
        if "build" in locals():
            shutil.rmtree(build, ignore_errors=True)
    return runtime


def create_container_smoke_image(runtime, cache):
    from .core import VirtmeError

    image = cache / "virtme-pyxis-smoke-v4.sqsh"
    checksum = pathlib.Path(str(image) + ".sha256")
    try:
        expected = checksum.read_text(encoding="utf-8").strip()
    except OSError:
        expected = ""
    if (image.is_file() and re.fullmatch(r"[0-9a-f]{64}", expected) and
            sha256_file(image) == expected):
        return image
    try:
        rootfs = pathlib.Path(tempfile.mkdtemp(
            prefix=".pyxis-rootfs.", dir=cache))
        (rootfs / "bin").mkdir()
        (rootfs / "lib/x86_64-linux-gnu").mkdir(parents=True)
        (rootfs / "lib64").mkdir()
        (rootfs / "etc").mkdir()
        (rootfs / "proc").mkdir()
        (rootfs / "sys").mkdir()
        (rootfs / "dev").mkdir()
        (rootfs / "run").mkdir()
        (rootfs / "tmp").mkdir(mode=0o1777)
        busybox = runtime / "usr/bin/busybox"
        shutil.copy2(busybox, rootfs / "bin/busybox")
        applets = subprocess.run(
            [str(busybox), "--list"], text=True, capture_output=True,
            timeout=30, check=False)
        if applets.returncode:
            raise VirtmeError("cannot inspect the pinned BusyBox runtime")
        for name in applets.stdout.splitlines():
            link = rootfs / "bin" / name
            if name and name != "busybox" and not link.exists():
                link.symlink_to("busybox")
        libc = pathlib.Path("/lib/x86_64-linux-gnu/libc.so.6")
        loader = pathlib.Path("/lib64/ld-linux-x86-64.so.2")
        if not libc.is_file() or not loader.is_file():
            raise VirtmeError("the amd64 glibc runtime is unavailable")
        shutil.copy2(libc, rootfs / "lib/x86_64-linux-gnu/libc.so.6")
        shutil.copy2(loader, rootfs / "lib64/ld-linux-x86-64.so.2")
        (rootfs / "etc/passwd").write_text(
            "root:x:0:0:root:/root:/bin/sh\n", encoding="utf-8")
        (rootfs / "etc/group").write_text("root:x:0:\n", encoding="utf-8")
        temporary = pathlib.Path(str(image) + ".part")
        temporary.unlink(missing_ok=True)
        mksquashfs = runtime / "usr/bin/mksquashfs"
        result = subprocess.run([
            str(mksquashfs), str(rootfs), str(temporary), "-noappend",
            "-comp", "gzip", "-no-progress",
        ], stdout=subprocess.DEVNULL, timeout=60, check=False)
        if result.returncode:
            raise VirtmeError("cannot build the local Pyxis smoke image")
        temporary.replace(image)
        checksum.write_text(sha256_file(image) + "\n", encoding="utf-8")
        return image
    except OSError as error:
        raise VirtmeError(f"cannot prepare the Pyxis smoke image: {error}") \
            from error
    finally:
        if "rootfs" in locals():
            shutil.rmtree(rootfs, ignore_errors=True)
