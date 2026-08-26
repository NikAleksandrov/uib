# SPDX-License-Identifier: Apache-2.0

import base64
import os
import secrets
import shlex
import time


def slurm_launch_command(command, log):
    return (
        f"nohup {command} > {shlex.quote(log)} 2>&1 < /dev/null & "
        f"echo $! > {shlex.quote(log + '.pid')}")


def slurm_environment(runtime):
    binary_path = f"{runtime}/usr/bin:{runtime}/usr/sbin:/usr/bin:/usr/sbin"
    library_path = (
        f"{runtime}/usr/lib/x86_64-linux-gnu/slurm-wlm:"
        f"{runtime}/usr/lib/x86_64-linux-gnu")
    return [
        "env",
        f"PATH={binary_path}",
        f"LD_LIBRARY_PATH={library_path}",
        "SLURM_CONF=/run/uib-slurm/slurm.conf",
        f"ENROOT_LIBRARY_PATH={runtime}/usr/lib/enroot",
        "ENROOT_SYSCONF_PATH=/run/uib-enroot/etc",
        "ENROOT_RUNTIME_PATH=/run/uib-enroot/runtime",
        "ENROOT_CACHE_PATH=/run/uib-enroot/cache",
        "ENROOT_DATA_PATH=/run/uib-enroot/data",
        "ENROOT_CONFIG_PATH=/run/uib-enroot/config",
        "ENROOT_ALLOW_SUPERUSER=yes",
        "ENROOT_LOGIN_SHELL=no",
        "XDG_CACHE_HOME=/run/uib-enroot/cache-home",
        "XDG_CONFIG_HOME=/run/uib-enroot/config-home",
        "XDG_DATA_HOME=/run/uib-enroot/data-home",
        "XDG_RUNTIME_DIR=/run/uib-enroot/runtime-home",
    ]


def slurm_command(runtime, program, arguments=None):
    return shlex.join([
        *slurm_environment(runtime), program, *(arguments or []),
    ])


def slurmd_node_record(value, guest):
    from .core import VirtmeError

    fields = dict(item.split("=", 1) for item in value.split()
                  if "=" in item)
    try:
        cpus = int(fields["CPUs"])
        memory = int(fields["RealMemory"])
    except (KeyError, ValueError) as error:
        raise VirtmeError(
            f"cannot parse slurmd hardware report from {guest['hostname']}") \
            from error
    if cpus < 1 or memory < 512:
        raise VirtmeError(
            f"invalid slurmd hardware report from {guest['hostname']}")
    return (
        f"NodeName={guest['hostname']} NodeAddr={guest['dataAddress']} "
        f"CPUs={cpus} RealMemory={memory - 256} State=UNKNOWN")


def slurm_configuration(config, topology_value, node_records, runtime):
    controller = topology_value["guests"]["controller"]
    workers = [guest["hostname"] for guest in topology_value["guests"].values()
               if guest["role"] == "worker"]
    lines = [
        f"ClusterName={topology_value['runId']}",
        (f"SlurmctldHost={controller['hostname']}"
         f"({controller['dataAddress']})"),
        "SlurmUser=root",
        "SlurmctldPort=6817",
        "SlurmdPort=6818",
        "AuthType=auth/munge",
        "CryptoType=crypto/munge",
        "StateSaveLocation=/run/uib-slurm/slurmctld",
        "SlurmdSpoolDir=/run/uib-slurm/slurmd",
        "SlurmctldLogFile=/run/uib-slurm/slurmctld.log",
        "SlurmdLogFile=/run/uib-slurm/slurmd.log",
        ("PluginDir="
         f"{runtime}/usr/lib/x86_64-linux-gnu/slurm-wlm"),
        "SwitchType=switch/none",
        "MpiDefault=none",
        "ProctrackType=proctrack/linuxproc",
        "TaskPlugin=task/affinity",
        "SelectType=select/cons_tres",
        "SelectTypeParameters=CR_Core_Memory",
        "SchedulerType=sched/backfill",
        "ReturnToService=2",
        "MinJobAge=3600",
        "AccountingStorageType=accounting_storage/none",
        "JobCompType=jobcomp/filetxt",
        "JobCompLoc=/run/uib-slurm/jobcomp.log",
        *node_records,
        (f"PartitionName={config['slurm']['partition']} "
         f"Nodes={','.join(workers)} Default=YES "
         f"MaxTime={config['slurm']['timeLimit']} State=UP"),
    ]
    return "\n".join(lines) + "\n"


def wait_for_slurm(ssh, key, controller, workers, runtime):
    from .core import VirtmeError
    from .processes import remote_command
    from .topology import nodes

    try:
        timeout = float(os.environ.get("VIRTME_SLURM_WAIT", "60"))
    except ValueError as error:
        raise VirtmeError("VIRTME_SLURM_WAIT must be a number") from error
    if timeout <= 0:
        raise VirtmeError("VIRTME_SLURM_WAIT must be positive")
    deadline = time.monotonic() + timeout
    worker_count = len(workers)
    nodes = None
    while time.monotonic() < deadline:
        ping = remote_command(
            ssh, key, controller, slurm_command(runtime, "scontrol", ["ping"]),
            capture=True, check=False)
        nodes = remote_command(
            ssh, key, controller,
            slurm_command(runtime, "sinfo", [
                "--Node", "--noheader", "--format=%N|%T"]),
            capture=True, check=False)
        lines = nodes.stdout.splitlines() if nodes is not None else []
        states = [line.rpartition("|")[2].strip().lower() for line in lines]
        ready = (nodes is not None and nodes.returncode == 0 and
                 len(lines) == worker_count and
                 all(state.startswith("idle") for state in states))
        if ping is not None and ping.returncode == 0 and ready:
            return
        time.sleep(2)
    diagnostics = remote_command(
        ssh, key, controller,
        "tail -n 40 /run/uib-slurm/slurmctld-console.log "
        "/run/uib-slurm/slurmctld.log 2>/dev/null",
        capture=True, check=False)
    details = []
    if nodes is not None:
        details.extend([nodes.stdout.strip(), nodes.stderr.strip()])
    if diagnostics is not None:
        details.append(diagnostics.stdout.strip())
    for worker in workers:
        worker_log = remote_command(
            ssh, key, worker,
            "tail -n 40 /run/uib-slurm/slurmd-console.log "
            "/run/uib-slurm/slurmd.log 2>/dev/null",
            capture=True, check=False)
        if worker_log is not None:
            details.append(worker_log.stdout.strip())
    detail = "\n".join(item for item in details if item)
    suffix = f": {detail[-2000:]}" if detail else ""
    raise VirtmeError(f"the virtme Slurm cluster did not become ready{suffix}")


def provision_slurm(config, topology_value, ssh, key, runtime, smoke_image,
                    state):
    from .core import VirtmeError
    from .processes import remote_command, remote_write

    guests = topology_value["guests"]
    prepare = (
        "set -eu; install -d -m 0700 /run/uib-slurm; "
        "install -d -m 0755 /run/uib-slurm/slurmctld "
        "/run/uib-slurm/slurmd /run/munge; "
        "install -d -m 0700 /run/uib-enroot /run/uib-enroot/cache "
        "/run/uib-enroot/config /run/uib-enroot/data "
        "/run/uib-enroot/runtime /run/uib-enroot/cache-home "
        "/run/uib-enroot/config-home /run/uib-enroot/data-home "
        "/run/uib-enroot/runtime-home; "
        "install -d -m 0755 /run/uib-enroot/etc "
        "/run/uib-enroot/etc/hooks.d /run/uib-enroot/etc/mounts.d "
        "/run/uib-enroot/etc/environ.d; "
        "if test -e /usr/lib/enroot; then "
        f"test \"$(readlink -f /usr/lib/enroot)\" = {shlex.quote(str(runtime))}"
        "/usr/lib/enroot; else "
        f"ln -s {shlex.quote(str(runtime))}/usr/lib/enroot /usr/lib/enroot; fi; "
        "if test -e /etc/enroot; then "
        "test \"$(readlink -f /etc/enroot)\" = /run/uib-enroot/etc; else "
        "ln -s /run/uib-enroot/etc /etc/enroot; fi")
    for guest in guests.values():
        remote_command(ssh, key, guest, prepare)

    key_path = state / "munge.key"
    try:
        key_path.write_bytes(secrets.token_bytes(1024))
        key_path.chmod(0o600)
        munge_key = key_path.read_bytes()
    except OSError as error:
        raise VirtmeError(f"cannot create the run-owned Munge key: {error}") from error
    encoded_key = base64.b64encode(munge_key).decode()
    for guest in guests.values():
        remote_command(ssh, key, guest, (
            f"printf %s {shlex.quote(encoded_key)} | base64 --decode > "
            "/run/uib-slurm/munge.key && chmod 0400 "
            "/run/uib-slurm/munge.key && "
            f"{runtime}/usr/sbin/munged --force "
            "--key-file=/run/uib-slurm/munge.key "
            "--pid-file=/run/munge/munged.pid "
            "--socket=/run/munge/munge.socket.2 "
            "--seed-file=/run/uib-slurm/munged.seed "
            "--log-file=/run/uib-slurm/munged.log"))
        remote_command(ssh, key, guest, (
            f"{runtime}/usr/bin/munge --no-input | "
            f"{runtime}/usr/bin/unmunge"))

    node_records = []
    for guest in guests.values():
        if guest["role"] != "worker":
            continue
        report = remote_command(
            ssh, key, guest,
            slurm_command(runtime, f"{runtime}/usr/sbin/slurmd", ["-C"]),
            capture=True)
        node_records.append(slurmd_node_record(report.stdout, guest))
    slurm_conf = slurm_configuration(
        config, topology_value, node_records, runtime)
    for guest in guests.values():
        remote_write(
            ssh, key, guest, "/run/uib-slurm/slurm.conf", slurm_conf)
        remote_write(
            ssh, key, guest, "/run/uib-slurm/cgroup.conf",
            "CgroupPlugin=autodetect\nIgnoreSystemd=yes\n")
        remote_write(
            ssh, key, guest, "/run/uib-slurm/plugstack.conf",
            ("required "
             f"{runtime}/usr/lib/x86_64-linux-gnu/slurm-wlm/"
             "spank_pyxis.so\n"))

    controller = guests["controller"]
    remote_command(
        ssh, key, controller,
        slurm_launch_command(
            slurm_command(
                runtime, f"{runtime}/usr/sbin/slurmctld", ["-D"]),
            "/run/uib-slurm/slurmctld-console.log"))
    for guest in guests.values():
        if guest["role"] == "worker":
            remote_command(
                ssh, key, guest,
                slurm_launch_command(
                    slurm_command(
                        runtime, f"{runtime}/usr/sbin/slurmd", [
                            "-D", "-d", f"{runtime}/usr/sbin/slurmstepd",
                        ]),
                    "/run/uib-slurm/slurmd-console.log"))
    workers = [guest for guest in guests.values() if guest["role"] == "worker"]
    worker_count = len(workers)
    wait_for_slurm(ssh, key, controller, workers, runtime)
    remote_command(ssh, key, controller, slurm_command(runtime, "srun", [
        f"--nodes={worker_count}", f"--ntasks={worker_count}",
        "--ntasks-per-node=1", "/bin/hostname",
    ]), timeout=120)
    if len(workers) > 1:
        remote_command(ssh, key, workers[0], shlex.join([
            "ping", "-c", "3", "-W", "2", workers[1]["dataAddress"],
        ]), timeout=30)
    container_command = slurm_command(runtime, "srun", [
        "--nodes=1", "--ntasks=1", f"--container-image={smoke_image}",
        "--no-container-mount-home", "--no-container-entrypoint",
        "/bin/true",
    ])
    result = remote_command(
        ssh, key, controller,
        f"timeout 60s {container_command} > /run/uib-slurm/pyxis.log 2>&1",
        check=False, timeout=70)
    if result is None or result.returncode:
        diagnostics = []
        for guest in [controller, *workers]:
            log = remote_command(
                ssh, key, guest,
                "tail -n 80 /run/uib-slurm/pyxis.log "
                "/run/uib-slurm/slurmd.log 2>/dev/null",
                capture=True, check=False)
            if log is not None and log.stdout.strip():
                diagnostics.append(log.stdout.strip())
        detail = "\n".join(diagnostics)
        suffix = f": {detail[-4000:]}" if detail else ""
        raise VirtmeError(f"the Pyxis/Enroot smoke test failed{suffix}")
