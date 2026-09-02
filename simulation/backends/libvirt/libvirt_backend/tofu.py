# SPDX-License-Identifier: Apache-2.0

import hashlib
import os
import pathlib
import shutil
import urllib.request


def tofu_values(config, run_id, base_image, public_key):
    from .core import multicast_endpoint
    from .topology import node_values, qemu_identity

    multicast_address, multicast_port = multicast_endpoint(run_id)
    volume_owner, volume_group = qemu_identity(config)

    return {
        "run_id": run_id,
        "libvirt_uri": config["libvirt"]["uri"],
        "pool": config["libvirt"]["pool"],
        "base_image": str(pathlib.Path(base_image).resolve()),
        "volume_owner": volume_owner,
        "volume_group": volume_group,
        "ssh_user": config["libvirt"]["sshUser"],
        "ssh_public_key": public_key.strip(),
        "network_mode": config["libvirt"]["networkMode"],
        "management_cidr": config["network"]["managementCIDR"],
        "data_cidr": config["network"]["dataCIDR"],
        "multicast_address": multicast_address,
        "multicast_port": multicast_port,
        "mtu": config["network"]["mtu"],
        "nodes": node_values(config, run_id),
    }


def cache_path(config, cache):
    from .core import LibvirtError, SHA256

    digest = config["libvirt"]["baseImageSha256"]
    if not SHA256.fullmatch(digest):
        raise LibvirtError("invalid libvirt base-image SHA-256")
    return cache / f"ubuntu-noble-{digest[:16]}.img"


def sha256(path):
    from .core import LibvirtError

    digest = hashlib.sha256()
    try:
        with path.open("rb") as source:
            for block in iter(lambda: source.read(1024 * 1024), b""):
                digest.update(block)
    except OSError as error:
        raise LibvirtError(f"cannot hash {path}: {error}") from error
    return digest.hexdigest()


def acquire_base_image(config, cache):
    from .core import LibvirtError

    destination = cache_path(config, cache)
    expected = config["libvirt"]["baseImageSha256"]
    if destination.is_file() and sha256(destination) == expected:
        return destination
    if destination.exists():
        raise LibvirtError(f"cached base image has the wrong digest: {destination}")
    cache.mkdir(parents=True, exist_ok=True)
    temporary = destination.with_suffix(".part")
    temporary.unlink(missing_ok=True)
    try:
        with urllib.request.urlopen(
                config["libvirt"]["baseImage"], timeout=60) as response, \
                temporary.open("xb") as output:
            shutil.copyfileobj(response, output)
    except (OSError, urllib.error.URLError) as error:
        temporary.unlink(missing_ok=True)
        raise LibvirtError(f"cannot download libvirt base image: {error}") from error
    if sha256(temporary) != expected:
        temporary.unlink(missing_ok=True)
        raise LibvirtError("downloaded libvirt base image failed SHA-256 validation")
    temporary.replace(destination)
    return destination


def tofu_environment(cache):
    environment = os.environ.copy()
    plugin_cache = cache / "opentofu-plugins"
    plugin_cache.mkdir(parents=True, exist_ok=True)
    environment["TF_PLUGIN_CACHE_DIR"] = str(plugin_cache)
    environment["TF_IN_AUTOMATION"] = "1"
    return environment


def destroy(state, cache, log):
    from .core import executable, run_logged

    tofu = executable("TOFU_COMMAND", "tofu")
    module = state / "opentofu"
    variables = state / "terraform.tfvars.json"
    run_logged([
        tofu, f"-chdir={module}", "destroy", "-auto-approve",
        f"-var-file={variables}",
    ], log, tofu_environment(cache))
