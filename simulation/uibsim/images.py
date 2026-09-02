# SPDX-License-Identifier: Apache-2.0

import hashlib
import os
import pathlib
import re
import shlex
import subprocess
import sys
import tempfile


def validate_image_reference(reference, allow_mutable_tags):
    from .core import ConfigurationError, IMAGE_DIGEST, IMAGE_REFERENCE

    if (not isinstance(reference, str) or
            not IMAGE_REFERENCE.fullmatch(reference)):
        raise ConfigurationError(f"invalid image reference: {reference}")
    if not allow_mutable_tags and not IMAGE_DIGEST.fullmatch(reference):
        raise ConfigurationError(
            f"image reference is not digest-pinned: {reference}; "
            "use --allow-mutable-tags only for local development images")


def owned_kubernetes_nodes(args, include_controller=False):
    from .config import load
    from .core import ConfigurationError
    from .state import read_json, state_path

    infrastructure = state_path(args) / "infrastructure"
    ledger_path = infrastructure / "ledger.json"
    if not ledger_path.is_file():
        raise ConfigurationError("owned Kubernetes infrastructure is not ready")
    ledger = read_json(ledger_path, "infrastructure ledger")
    provider = ledger.get("provider")
    if (ledger.get("schemaVersion") != 1 or
            ledger.get("runId") != args.run_id or
            provider not in {"libvirt", "virtme"} or
            ledger.get("clusterManager") != "kubernetes"):
        raise ConfigurationError(
            f"Kubernetes infrastructure ownership mismatch: {ledger_path}")
    if ledger.get("state") != "ready":
        raise ConfigurationError(
            f"{provider} infrastructure is not ready: {ledger.get('state')}")

    inventory_name = (
        "inventory.json" if provider == "virtme" else "inventory.yaml")
    inventory_path = infrastructure / inventory_name
    inventory = load(inventory_path)
    try:
        all_hosts = inventory["all"]
        variables = all_hosts["vars"]
        children = all_hosts["children"]
        workers = children["kubernetes_workers"]["hosts"]
        cluster = children["kubernetes_cluster"]["hosts"]
        if include_controller:
            hosts = cluster
        else:
            hosts = {name: cluster[name] for name in workers}
        user = variables["ansible_user"]
        key = pathlib.Path(
            variables["ansible_ssh_private_key_file"]).resolve()
    except (KeyError, TypeError) as error:
        raise ConfigurationError(
            f"invalid {provider} Kubernetes inventory: {inventory_path}") from error
    expected_key = (infrastructure / "id_ed25519").resolve()
    if key != expected_key or not key.is_file():
        raise ConfigurationError(
            f"{provider} Kubernetes key ownership mismatch: {key}")
    if (not isinstance(user, str) or
            not re.fullmatch(r"[a-z_][a-z0-9_-]*", user) or
            not isinstance(hosts, dict) or not hosts):
        raise ConfigurationError(
            f"invalid {provider} Kubernetes inventory: {inventory_path}")

    result = []
    for name, endpoint in sorted(hosts.items()):
        if not isinstance(endpoint, dict):
            raise ConfigurationError(
                f"invalid {provider} Kubernetes node: {name}")
        host = endpoint.get("ansible_host")
        port = endpoint.get("ansible_port")
        if (not isinstance(host, str) or not host or isinstance(port, bool) or
                not isinstance(port, int) or port < 1 or port > 65535):
            raise ConfigurationError(
                f"invalid {provider} Kubernetes endpoint: {name}")
        result.append({
            "host": host,
            "key": key,
            "name": name,
            "port": port,
            "provider": provider,
            "role": "worker" if name in workers else "controller",
            "target": f"{user}@{host}",
        })

    if provider == "virtme":
        binary = ledger.get("k3sBinary")
        if not isinstance(binary, str) or not pathlib.Path(binary).is_file():
            raise ConfigurationError("virtme k3s binary is not recorded")
    else:
        binary = "/usr/local/bin/k3s"
    return result, binary


def node_ssh_command(node):
    from .scheduler import command_executable

    return [
        command_executable("SSH_COMMAND", "ssh"),
        "-o", "BatchMode=yes",
        "-o", "IdentitiesOnly=yes",
        "-o", "StrictHostKeyChecking=no",
        "-o", "UserKnownHostsFile=/dev/null",
        "-o", "LogLevel=ERROR",
        "-o", "ConnectTimeout=10",
        "-i", str(node["key"]),
        "-p", str(node["port"]),
        node["target"],
    ]


def inspect_local_image(engine, reference):
    from .core import ConfigurationError, HEX_DIGEST, IMAGE_PLATFORM, run_command

    try:
        inspected = run_command([
            engine, "image", "inspect",
            "--format={{.Id}}\t{{.Os}}/{{.Architecture}}", reference,
        ], text=True, capture_output=True)
        if inspected.returncode:
            pulled = run_command(
                [engine, "pull", reference], text=True, capture_output=True)
            if pulled.returncode:
                if pulled.stderr:
                    print(pulled.stderr, file=sys.stderr, end="")
                raise ConfigurationError(f"cannot pull image: {reference}")
            inspected = run_command([
                engine, "image", "inspect",
                "--format={{.Id}}\t{{.Os}}/{{.Architecture}}", reference,
            ], text=True, capture_output=True)
    except OSError as error:
        raise ConfigurationError(
            f"cannot invoke container engine: {error}") from error
    image = inspected.stdout.strip().split("\t", 1)
    image_id = image[0] if image else ""
    platform = image[1] if len(image) == 2 else ""
    if (inspected.returncode or not image_id.startswith("sha256:") or
            not HEX_DIGEST.fullmatch(image_id[7:]) or
            not IMAGE_PLATFORM.fullmatch(platform)):
        if inspected.stderr:
            print(inspected.stderr, file=sys.stderr, end="")
        raise ConfigurationError(f"cannot inspect local image: {reference}")
    return image_id, platform


def record_kubernetes_image(args, reference, image_id, platform, nodes):
    from .core import ConfigurationError
    from .state import read_json, state_path, utc_now, write_json

    path = state_path(args) / "infrastructure" / "kubernetes-images.json"
    if path.is_file():
        record = read_json(path, "Kubernetes image ledger")
        if (record.get("schemaVersion") != 1 or
                record.get("runId") != args.run_id):
            raise ConfigurationError(
                f"Kubernetes image ledger ownership mismatch: {path}")
    else:
        record = {"schemaVersion": 1, "runId": args.run_id, "images": {}}
    images = record.get("images")
    if not isinstance(images, dict):
        raise ConfigurationError(f"invalid Kubernetes image ledger: {path}")
    images[reference] = {
        "imageId": image_id,
        "nodes": [node["name"] for node in nodes],
        "platform": platform,
        "stagedAt": utc_now(),
    }
    write_json(path, record)


def stage_kubernetes_image(args, reference, allow_mutable_tags,
                           include_controller=False):
    from .core import ConfigurationError, run_command
    from .scheduler import command_executable
    from .state import state_path

    validate_image_reference(reference, allow_mutable_tags)
    nodes, binary = owned_kubernetes_nodes(args, include_controller)
    engine = command_executable("CONTAINER_ENGINE_COMMAND", "docker")
    image_id, platform = inspect_local_image(engine, reference)
    staging = state_path(args) / "infrastructure"
    archive = None
    try:
        with tempfile.NamedTemporaryFile(
                prefix="uib-kubernetes-image-", suffix=".tar",
                dir=staging, delete=False) as output:
            archive = pathlib.Path(output.name)
            exported = run_command(
                [engine, "image", "save", reference], stdout=output,
                stderr=subprocess.PIPE, text=True)
        if exported.returncode:
            if exported.stderr:
                print(exported.stderr, file=sys.stderr, end="")
            raise ConfigurationError(f"cannot export image: {reference}")
        for node in nodes:
            remote = [
                binary, "ctr", "images", "import",
                "--local", "--platform", platform,
            ]
            if node["provider"] == "virtme":
                snapshotter = (
                    "native" if node["role"] == "controller"
                    else "overlayfs")
                remote.extend(["--snapshotter", snapshotter])
            remote.extend(["--sync-fs", "-"])
            if node["provider"] == "libvirt":
                remote = ["sudo", "-n", *remote]
            with archive.open("rb") as source:
                imported = run_command(
                    [*node_ssh_command(node), shlex.join(remote)], stdin=source)
            if imported.returncode:
                raise ConfigurationError(
                    f"cannot import image on Kubernetes node {node['name']}: "
                    f"{reference}")
    except OSError as error:
        raise ConfigurationError(
            f"cannot stage Kubernetes image {reference}: {error}") from error
    finally:
        if archive is not None:
            archive.unlink(missing_ok=True)
    record_kubernetes_image(args, reference, image_id, platform, nodes)


def sha256_file(path):
    from .core import ConfigurationError

    digest = hashlib.sha256()
    try:
        with path.open("rb") as source:
            for block in iter(lambda: source.read(1024 * 1024), b""):
                digest.update(block)
    except OSError as error:
        raise ConfigurationError(f"cannot hash cached image {path}: {error}") from error
    return digest.hexdigest()


def cached_slurm_image(path):
    from .core import ConfigurationError, HEX_DIGEST

    checksum = path.with_suffix(path.suffix + ".sha256")
    if path.is_symlink() or checksum.is_symlink():
        raise ConfigurationError(f"unsafe cached Slurm image: {path}")
    if not path.exists() and not checksum.exists():
        return False
    if not path.is_file() or not checksum.is_file():
        raise ConfigurationError(
            f"incomplete or unsafe cached Slurm image: {path}")
    try:
        expected = checksum.read_text(encoding="utf-8").strip()
    except OSError as error:
        raise ConfigurationError(
            f"cannot read cached image checksum {checksum}: {error}") from error
    if not HEX_DIGEST.fullmatch(expected):
        raise ConfigurationError(f"invalid cached image checksum: {checksum}")
    if sha256_file(path) != expected:
        raise ConfigurationError(f"cached Slurm image failed checksum: {path}")
    return True


def run_image_helper(args, mode, digest, **kwargs):
    from .core import ConfigurationError
    from .scheduler import run_scheduler, scheduler_remote

    remote = scheduler_remote(args)
    if remote and remote["provider"] == "virtme":
        program = "/run/uib-stage-image"
        arguments = [mode, digest]
    else:
        program = "sudo"
        arguments = [
            "-n", "/usr/local/libexec/uib-stage-image", mode, digest]
    result = run_scheduler(
        args, "", program, arguments, text=True, capture_output=True,
        **kwargs)
    remote_path = f"/var/cache/uib/images/{digest}.sqsh"
    if result.returncode == 0 and result.stdout.strip() != remote_path:
        raise ConfigurationError(
            f"image staging helper returned an invalid path: {result.stdout.strip()}")
    return result, remote_path


def install_cached_slurm_image(args, digest, cache):
    from .core import ConfigurationError

    try:
        with cache.open("rb") as source:
            result, remote_path = run_image_helper(
                args, "install", digest, stdin=source)
    except OSError as error:
        raise ConfigurationError(f"cannot read cached Slurm image: {error}") from error
    if result.returncode:
        if result.stderr:
            print(result.stderr, file=sys.stderr, end="")
        raise ConfigurationError("cannot install cached Slurm image")
    return remote_path


def import_local_slurm_image(args, engine, reference, digest):
    from .core import ConfigurationError

    try:
        created = subprocess.run(
            [engine, "create", reference], text=True, capture_output=True,
            check=False)
    except OSError as error:
        raise ConfigurationError(
            f"cannot invoke container engine: {error}") from error
    container = created.stdout.strip()
    if created.returncode or not re.fullmatch(r"[0-9a-f]{64}", container):
        if created.stderr:
            print(created.stderr, file=sys.stderr, end="")
        raise ConfigurationError(f"cannot create image export container: {reference}")

    try:
        export_error = tempfile.TemporaryFile(mode="w+", encoding="utf-8")
    except OSError as error:
        raise ConfigurationError(
            f"cannot create image export diagnostic file: {error}") from error
    exporter = None
    export_status = None
    imported = None
    remote_path = None
    operation_error = None
    try:
        exporter = subprocess.Popen(
            [engine, "export", container], stdout=subprocess.PIPE,
            stderr=export_error)
        imported, remote_path = run_image_helper(
            args, "import", digest, stdin=exporter.stdout)
        exporter.stdout.close()
        export_status = exporter.wait()
    except (ConfigurationError, OSError) as error:
        if exporter and exporter.poll() is None:
            exporter.terminate()
            exporter.wait()
        operation_error = error
    finally:
        try:
            removed = subprocess.run(
                [engine, "rm", container], text=True, capture_output=True,
                check=False)
        except OSError as error:
            removed = None
            removal_error = error
        else:
            removal_error = None
        export_error.seek(0)
        export_diagnostics = export_error.read()
        export_error.close()

    if removal_error is not None:
        raise ConfigurationError(
            f"cannot invoke container engine to remove {container}: "
            f"{removal_error}") from removal_error
    if removed.returncode:
        if removed.stderr:
            print(removed.stderr, file=sys.stderr, end="")
        raise ConfigurationError(
            f"cannot remove image export container: {container}")
    if operation_error is not None:
        if export_diagnostics:
            print(export_diagnostics, file=sys.stderr, end="")
        if isinstance(operation_error, ConfigurationError):
            raise operation_error
        raise ConfigurationError(
            f"cannot export local image: {operation_error}") from operation_error
    if export_status or imported.returncode:
        if export_diagnostics:
            print(export_diagnostics, file=sys.stderr, end="")
        if imported.stderr:
            print(imported.stderr, file=sys.stderr, end="")
        raise ConfigurationError(f"cannot stage local image: {reference}")
    return remote_path


def cache_remote_slurm_image(args, remote_path, cache):
    from .core import ConfigurationError
    from .scheduler import copy_remote_path, scheduler_remote

    checksum_path = cache.with_suffix(cache.suffix + ".sha256")
    try:
        remote = scheduler_remote(args)
        if not remote or remote["provider"] != "virtme":
            copy_remote_path(args, remote_path, cache)
        elif not cache.is_file() or cache.is_symlink():
            raise ConfigurationError(
                f"virtme image cache did not produce a regular file: {cache}")
        checksum = sha256_file(cache)
        checksum_path.write_text(checksum + "\n", encoding="utf-8")
    except (ConfigurationError, OSError):
        cache.unlink(missing_ok=True)
        checksum_path.unlink(missing_ok=True)
        raise


def record_staged_image(args, reference, image_id, remote_path, cache):
    from .core import ConfigurationError
    from .state import read_json, state_path, utc_now, write_json

    path = state_path(args) / "infrastructure" / "images.json"
    if path.is_file():
        record = read_json(path, "staged image ledger")
        if record.get("schemaVersion") != 1 or record.get("runId") != args.run_id:
            raise ConfigurationError(f"staged image ledger ownership mismatch: {path}")
    else:
        record = {"schemaVersion": 1, "runId": args.run_id, "images": {}}
    images = record.get("images")
    if not isinstance(images, dict):
        raise ConfigurationError(f"invalid staged image ledger: {path}")
    images[reference] = {
        "imageId": image_id,
        "path": remote_path,
        "hostCacheKey": cache.name,
        "stagedAt": utc_now(),
    }
    write_json(path, record)


def record_slurm_image_cache(cache_root, reference, image_id, cache):
    from .core import ConfigurationError
    from .state import read_json, utc_now, write_json

    inventory_path = cache_root / "inventory.json"
    if inventory_path.is_file():
        inventory = read_json(inventory_path, "Slurm image cache inventory")
        if inventory.get("schemaVersion") != 1:
            raise ConfigurationError(
                f"unsupported Slurm image cache inventory: {inventory_path}")
    else:
        inventory = {"schemaVersion": 1, "images": {}}
    images = inventory.get("images")
    if not isinstance(images, dict):
        raise ConfigurationError(
            f"invalid Slurm image cache inventory: {inventory_path}")
    checksum_path = cache.with_suffix(cache.suffix + ".sha256")
    try:
        checksum = checksum_path.read_text(encoding="utf-8").strip()
        size = cache.stat().st_size
    except OSError as error:
        raise ConfigurationError(
            f"cannot record cached Slurm image: {error}") from error
    images[image_id] = {
        "cacheKey": cache.name,
        "checksum": checksum,
        "reference": reference,
        "size": size,
        "verifiedAt": utc_now(),
    }
    write_json(inventory_path, inventory)


def stage_slurm_image(args, reference, allow_mutable_tags):
    from .core import ConfigurationError, DEFAULT_SLURM_IMAGE_CACHE, HEX_DIGEST
    from .scheduler import command_executable, scheduler_remote

    validate_image_reference(reference, allow_mutable_tags)
    remote = scheduler_remote(args)
    if not remote:
        raise ConfigurationError("owned Slurm infrastructure is not ready")

    engine = command_executable("CONTAINER_ENGINE_COMMAND", "docker")
    try:
        inspected = subprocess.run(
            [engine, "image", "inspect", "--format={{.Id}}", reference],
            text=True, capture_output=True, check=False)
    except OSError as error:
        raise ConfigurationError(
            f"cannot invoke container engine: {error}") from error
    if inspected.returncode:
        try:
            pulled = subprocess.run(
                [engine, "pull", reference], text=True, capture_output=True,
                check=False)
            inspected = subprocess.run(
                [engine, "image", "inspect", "--format={{.Id}}", reference],
                text=True, capture_output=True, check=False)
        except OSError as error:
            raise ConfigurationError(
                f"cannot pull container image: {error}") from error
        if pulled.returncode:
            if pulled.stderr:
                print(pulled.stderr, file=sys.stderr, end="")
            raise ConfigurationError(f"cannot pull image: {reference}")
    image_id = inspected.stdout.strip()
    if (inspected.returncode or not image_id.startswith("sha256:") or
            not HEX_DIGEST.fullmatch(image_id[7:])):
        if inspected.stderr:
            print(inspected.stderr, file=sys.stderr, end="")
        raise ConfigurationError(f"cannot inspect local image: {reference}")
    digest = image_id[7:]

    checked, remote_path = run_image_helper(args, "check", digest)
    cache_root = pathlib.Path(os.environ.get(
        "SLURM_IMAGE_CACHE", DEFAULT_SLURM_IMAGE_CACHE)).expanduser().resolve()
    cache = cache_root / f"{digest}.sqsh"
    if checked.returncode not in (0, 3):
        if checked.stderr:
            print(checked.stderr, file=sys.stderr, end="")
        raise ConfigurationError("cannot inspect the controller image cache")
    try:
        cache_root.mkdir(parents=True, exist_ok=True)
    except OSError as error:
        raise ConfigurationError(
            f"cannot create Slurm image cache: {error}") from error
    cached = cached_slurm_image(cache)
    if checked.returncode == 3:
        if cached:
            remote_path = install_cached_slurm_image(args, digest, cache)
        else:
            remote_path = import_local_slurm_image(
                args, engine, reference, digest)
            cache_remote_slurm_image(args, remote_path, cache)
    elif not cached:
        cache_remote_slurm_image(args, remote_path, cache)
    record_slurm_image_cache(cache_root, reference, image_id, cache)
    record_staged_image(args, reference, image_id, remote_path, cache)
    return remote_path


def validate_huggingface_cache(source):
    from .core import ConfigurationError
    from .state import path_is_within

    source = source.expanduser()
    if source.is_symlink() or not source.is_dir():
        raise ConfigurationError(
            f"Hugging Face cache is not a directory: {source}")
    try:
        root = source.resolve(strict=True)
        entries = sorted(root.rglob("*"))
    except OSError as error:
        raise ConfigurationError(
            f"cannot inspect Hugging Face cache {source}: {error}") from error
    if not entries:
        raise ConfigurationError(f"Hugging Face cache is empty: {source}")
    for entry in entries:
        if entry.is_symlink():
            try:
                target = entry.resolve(strict=True)
            except OSError as error:
                raise ConfigurationError(
                    f"broken cache symbolic link: {entry}") from error
            if not path_is_within(target, root):
                raise ConfigurationError(
                    f"cache symbolic link escapes its root: {entry}")
        elif not entry.is_dir() and not entry.is_file():
            raise ConfigurationError(
                f"Hugging Face cache contains a special file: {entry}")
    return root


def create_cache_archive(source):
    from .core import ConfigurationError
    from .scheduler import command_executable

    tar = command_executable("TAR_COMMAND", "tar")
    try:
        descriptor, name = tempfile.mkstemp(
            prefix="uib-huggingface-", suffix=".tar")
        os.close(descriptor)
    except OSError as error:
        raise ConfigurationError(
            f"cannot create Hugging Face cache archive: {error}") from error
    archive = pathlib.Path(name)
    try:
        created = subprocess.run([
            tar, "--create", f"--file={archive}",
            f"--directory={source}", ".",
        ], text=True, capture_output=True, check=False)
    except OSError as error:
        archive.unlink(missing_ok=True)
        raise ConfigurationError(f"cannot invoke tar: {error}") from error
    if created.returncode:
        archive.unlink(missing_ok=True)
        if created.stderr:
            print(created.stderr, file=sys.stderr, end="")
        raise ConfigurationError("cannot archive the Hugging Face cache")
    return archive, sha256_file(archive)


def run_cache_helper(args, mode, digest, **kwargs):
    from .core import ConfigurationError
    from .scheduler import run_scheduler, scheduler_remote

    remote = scheduler_remote(args)
    if remote and remote["provider"] == "virtme":
        program = "/run/uib-stage-huggingface-cache"
    else:
        program = "/usr/local/libexec/uib-stage-huggingface-cache"
    arguments = [mode, digest]
    result = run_scheduler(
        args, "", program, arguments, text=True, capture_output=True,
        **kwargs)
    remote_path = "/var/cache/uib/models/huggingface"
    if result.returncode == 0 and result.stdout.strip() != remote_path:
        raise ConfigurationError(
            "cache staging helper returned an invalid path: "
            f"{result.stdout.strip()}")
    return result, remote_path


def record_staged_cache(args, source, digest, size, remote_path):
    from .core import ConfigurationError
    from .state import read_json, state_path, utc_now, write_json

    path = state_path(args) / "infrastructure" / "caches.json"
    if path.is_file():
        record = read_json(path, "staged cache ledger")
        if record.get("schemaVersion") != 1 or record.get("runId") != args.run_id:
            raise ConfigurationError(
                f"staged cache ledger ownership mismatch: {path}")
    else:
        record = {"schemaVersion": 1, "runId": args.run_id, "caches": {}}
    caches = record.get("caches")
    if not isinstance(caches, dict):
        raise ConfigurationError(f"invalid staged cache ledger: {path}")
    caches["huggingface"] = {
        "archiveSha256": digest,
        "archiveSize": size,
        "path": remote_path,
        "sourceKey": source.name,
        "stagedAt": utc_now(),
    }
    write_json(path, record)


def record_slurm_cache_inventory(source, digest, size):
    from .core import ConfigurationError, DEFAULT_SLURM_CACHE_INVENTORY
    from .state import read_json, utc_now, write_json

    root = pathlib.Path(os.environ.get(
        "SLURM_CACHE_INVENTORY", DEFAULT_SLURM_CACHE_INVENTORY)
    ).expanduser().resolve()
    try:
        root.mkdir(parents=True, exist_ok=True)
    except OSError as error:
        raise ConfigurationError(
            f"cannot create Slurm cache inventory: {error}") from error
    path = root / "inventory.json"
    if path.is_file():
        inventory = read_json(path, "Slurm cache inventory")
        if inventory.get("schemaVersion") != 1:
            raise ConfigurationError(
                f"unsupported Slurm cache inventory: {path}")
    else:
        inventory = {"schemaVersion": 1, "caches": {}}
    caches = inventory.get("caches")
    if not isinstance(caches, dict):
        raise ConfigurationError(f"invalid Slurm cache inventory: {path}")
    caches["huggingface"] = {
        "archiveSha256": digest,
        "archiveSize": size,
        "sourceKey": source.name,
        "verifiedAt": utc_now(),
    }
    write_json(path, inventory)


def stage_huggingface_cache(args, source):
    from .core import ConfigurationError
    from .scheduler import scheduler_remote
    from .state import read_json, state_path

    remote = scheduler_remote(args)
    if not remote:
        raise ConfigurationError("remote Slurm infrastructure is not ready")
    source = validate_huggingface_cache(source)
    archive, digest = create_cache_archive(source)
    try:
        if remote["provider"] == "virtme":
            topology = read_json(
                state_path(args) / "infrastructure" / "topology.json",
                "virtme topology")
            try:
                mapping = topology["sharedStorage"]["modelCache"]
                mounted = pathlib.Path(mapping["host"]).resolve(strict=True)
                remote_path = mapping["guest"]
            except (KeyError, OSError, TypeError) as error:
                raise ConfigurationError(
                    "invalid virtme model-cache mapping") from error
            if mounted != source or remote_path != "/var/cache/uib/models/huggingface":
                raise ConfigurationError(
                    "virtme cache source differs from VIRTME_MODEL_CACHE; "
                    "select the source before up")
            try:
                size = archive.stat().st_size
            except OSError as error:
                raise ConfigurationError(
                    f"cannot inspect Hugging Face cache archive: {error}") from error
            record_slurm_cache_inventory(source, digest, size)
            record_staged_cache(args, source, digest, size, remote_path)
            return remote_path
        checked, remote_path = run_cache_helper(args, "check", digest)
        if checked.returncode == 3:
            try:
                with archive.open("rb") as content:
                    installed, remote_path = run_cache_helper(
                        args, "install", digest, stdin=content)
            except OSError as error:
                raise ConfigurationError(
                    f"cannot read Hugging Face cache archive: {error}") from error
            if installed.returncode:
                if installed.stderr:
                    print(installed.stderr, file=sys.stderr, end="")
                raise ConfigurationError("cannot stage Hugging Face cache")
        elif checked.returncode:
            if checked.stderr:
                print(checked.stderr, file=sys.stderr, end="")
            raise ConfigurationError(
                "controller has a different Hugging Face cache")
        try:
            size = archive.stat().st_size
        except OSError as error:
            raise ConfigurationError(
                f"cannot inspect Hugging Face cache archive: {error}") from error
        record_slurm_cache_inventory(source, digest, size)
        record_staged_cache(args, source, digest, size, remote_path)
        return remote_path
    finally:
        archive.unlink(missing_ok=True)
