# SPDX-License-Identifier: Apache-2.0

import hashlib
import json
import os
import pathlib
import re
import shutil
import tempfile


NAME = re.compile(r"^[a-z](?:[a-z0-9-]{1,46}[a-z0-9])$")


BACKEND = pathlib.Path(__file__).resolve().parents[1]


SIMULATION = BACKEND.parents[1]


DEFAULT_CACHE = SIMULATION / ".artifacts" / "downloads" / "virtme"


DEFAULT_IMAGE_CACHE = SIMULATION / ".artifacts" / "slurm-images"


DEFAULT_MODEL_CACHE = SIMULATION / ".artifacts" / "huggingface"


DEFAULT_RUNTIME = SIMULATION / ".artifacts" / "uib-runtime"


SLURM_PACKAGES = (
    ("busybox-static", "1:1.37.0-6+b8",
     "73efc773b070ea220c5d7cb2d12206ec235c46b03a9ad133d1f6a7ca62201e41"),
    ("libb64-0d", "1.2-5+b3",
     "2eef02f6402a649d692181afe0eb26fce7b00b64092348284f7018420f8fbcde"),
    ("libjwt2", "1.17.2-1",
     "56022ab5f880e47c8f086ac4b7cf78fff4734cb035a998b6b3d5200752887803"),
    ("liblua5.1-0", "5.1.5-11",
     "e98fb0eaa8d07fd24bbaefd3b2bd356b8c6df241865780c42adb08f7ebcf8563"),
    ("libslurm-dev", "24.11.5-4",
     "41cd011854b892f4322218be21476efe485167fb3fdaa777858c4b833ccff3f8"),
    ("munge", "0.5.16-1.1~deb13u1",
     "2e26ea38f9128984d258b0f4b8e0603db7848be92629c36409563b7fae8a0e73"),
    ("patchelf", "0.18.0-1.4",
     "1d39a7973504a4a5eb4101293cca142b83055a5a0f94e1d4286ded2563289b92"),
    ("slurm-client", "24.11.5-4",
     "480591cbcf2399066f47eccee35c541e3d923ec9acba90f6e0a4b70677fba86c"),
    ("slurmctld", "24.11.5-4",
     "477ca68c0cdda9e728ea2c91fb53e59ed4410914b64001add7aa98664479acec"),
    ("slurmd", "24.11.5-4",
     "08dadcd84880a7db37f2941336aed1f552a5bcaf6efcbdac4e047d3ac870c47b"),
    ("slurm-wlm-basic-plugins", "24.11.5-4",
     "eae21933a82aa3cd3fecd4feba388e392017f8daa5ce5d23bf0b69fb64eaf4fe"),
    ("squashfs-tools", "1:4.6.1-1",
     "0e185122f5858132bbc709e26228ba28cf7f0c41f6ed26e3f55f518f2a53b59d"),
)


SLURM_VERSION = "24.11.5-4"


ENROOT_VERSION = "4.2.1"


PYXIS_VERSION = "0.24.0"


PYXIS_COMMIT = "107519944221822ea1dace4db8e7234b2eaa4cd5"


CONTAINER_ARTIFACTS = (
    (
        "enroot_4.2.1-1_amd64.deb",
        "https://github.com/NVIDIA/enroot/releases/download/"
        "v4.2.1/enroot_4.2.1-1_amd64.deb",
        "5955c51e88df0c5ed538d87abb5264609d4bad55fa8570745ed5d4ec66c8ab91",
    ),
    (
        f"pyxis-{PYXIS_VERSION}.tar.gz",
        f"https://codeload.github.com/NVIDIA/pyxis/tar.gz/{PYXIS_COMMIT}",
        "32d0a56d506a3876dd59202e6a209fefae94af9803f4dd8b542e11ef2ead7907",
    ),
)


class VirtmeError(Exception):
    pass


def verbs_active_mtu(link_mtu):
    for mtu in (4096, 2048, 1024, 512, 256):
        if mtu + 64 <= link_mtu:
            return mtu
    raise VirtmeError("network MTU is too small for UIB")


def load_config(path):
    try:
        with pathlib.Path(path).open(encoding="utf-8") as source:
            config = json.load(source)
    except (OSError, json.JSONDecodeError) as error:
        raise VirtmeError(f"cannot load {path}: {error}") from error
    if config.get("infrastructure", {}).get("provider") != "virtme":
        raise VirtmeError("configuration does not select the virtme provider")
    if config.get("nodes", {}).get("controlPlane") != 1:
        raise VirtmeError("virtme requires exactly one control-plane guest")
    if "virtme" not in config:
        raise VirtmeError("configuration has no virtme settings")
    if config["virtme"]["mode"] != "multi-guest":
        raise VirtmeError("the topology renderer requires multi-guest mode")
    if config.get("cluster", {}).get("manager") == "kubernetes":
        if config.get("kubernetes", {}).get("distribution") != "k3s":
            raise VirtmeError("virtme Kubernetes requires k3s")
        if not re.fullmatch(
                r"v[0-9]+\.[0-9]+\.[0-9]+\+k3s[0-9]+",
                config["cluster"].get("version", "")):
            raise VirtmeError("virtme k3s requires an exact release version")
    if (config.get("cluster", {}).get("manager") == "slurm" and
            config["cluster"].get("version") != SLURM_VERSION):
        raise VirtmeError(
            f"virtme Slurm requires the pinned {SLURM_VERSION} package set")
    network_mode = config["virtme"]["networkMode"]
    if network_mode == "bridge" and "bridgePrefix" not in config["virtme"]:
        raise VirtmeError("virtme bridge networking requires bridgePrefix")
    expected_interface = "eth1" if network_mode == "socket" else "eth0"
    if config.get("network", {}).get("dataInterface") != expected_interface:
        raise VirtmeError(
            f"virtme {network_mode} networking requires dataInterface "
            f"{expected_interface}")
    return config


def validate_name(name):
    if not NAME.fullmatch(name):
        raise VirtmeError(
            "run ID must contain 3-48 lowercase letters, digits, or hyphens")


def quantity_mib(value):
    match = re.fullmatch(r"([1-9][0-9]*)(Mi|Gi|Ti)", value)
    if not match:
        raise VirtmeError(f"invalid node memory: {value}")
    factor = {"Mi": 1, "Gi": 1024, "Ti": 1024 ** 2}[match.group(2)]
    return int(match.group(1)) * factor


def mac_address(run_id, node):
    digest = hashlib.sha256(f"{run_id}:{node}:data".encode()).digest()
    return "52:54:00:" + ":".join(f"{byte:02x}" for byte in digest[:3])


def socket_endpoint(run_id):
    digest = hashlib.sha256(f"{run_id}:virtme-data".encode()).digest()
    address = f"239.255.{digest[0]}.{digest[1]}"
    port = 30000 + int.from_bytes(digest[2:4], "big") % 20000
    return address, port


def bridge_name(config, run_id):
    suffix = hashlib.sha256(run_id.encode()).hexdigest()[:8]
    return f"{config['virtme']['bridgePrefix']}-{suffix}"


def write_json(path, value):
    temporary = None
    try:
        path.parent.mkdir(parents=True, exist_ok=True)
        with tempfile.NamedTemporaryFile(
                mode="w", encoding="utf-8", dir=path.parent,
                prefix=f".{path.name}.", delete=False) as output:
            temporary = pathlib.Path(output.name)
            json.dump(value, output, indent=2, sort_keys=True)
            output.write("\n")
        temporary.replace(path)
    except OSError as error:
        if temporary:
            temporary.unlink(missing_ok=True)
        raise VirtmeError(f"cannot write {path}: {error}") from error


def read_json(path, description):
    try:
        with path.open(encoding="utf-8") as source:
            value = json.load(source)
    except (OSError, json.JSONDecodeError) as error:
        raise VirtmeError(f"cannot load {description} {path}: {error}") from error
    if not isinstance(value, dict):
        raise VirtmeError(f"{description} {path} must contain an object")
    return value


def executable(variable, default):
    command = os.environ.get(variable, default)
    if os.path.sep in command:
        resolved = command if os.access(command, os.X_OK) else None
    else:
        resolved = shutil.which(command)
    if not resolved:
        raise VirtmeError(f"command not found: {command}")
    return resolved
