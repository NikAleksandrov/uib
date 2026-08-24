# SPDX-License-Identifier: Apache-2.0

import subprocess


def command_slurm_status(args):
    from .core import ConfigurationError
    from .scheduler import slurm_command
    from .state import load_ledger

    unused, ledger = load_ledger(args)
    job_id = ledger.get("jobId")
    if not job_id:
        raise ConfigurationError("run ledger has no recorded Slurm job ID")
    sacct = slurm_command("SACCT_COMMAND", "sacct")
    try:
        result = subprocess.run([
            sacct, "--noheader", "--parsable2", f"--jobs={job_id}",
            "--format=JobIDRaw,State,ExitCode,Elapsed,NodeList",
        ], check=False)
    except OSError as error:
        raise ConfigurationError(f"cannot invoke sacct: {error}") from error
    return result.returncode


def command_slurm_down(args):
    from .core import ConfigurationError
    from .scheduler import slurm_command
    from .state import load_ledger, utc_now, write_json

    ledger_path, ledger = load_ledger(args)
    if ledger.get("state") == "cancellation-requested":
        return 0
    job_id = ledger.get("jobId")
    if not job_id:
        raise ConfigurationError("run ledger has no recorded Slurm job ID")
    scancel = slurm_command("SCANCEL_COMMAND", "scancel")
    try:
        result = subprocess.run([scancel, "--quiet", job_id], check=False)
    except OSError as error:
        raise ConfigurationError(f"cannot invoke scancel: {error}") from error
    if result.returncode:
        return result.returncode
    ledger["state"] = "cancellation-requested"
    ledger["cancellationRequestedAt"] = utc_now()
    write_json(ledger_path, ledger)
    return 0
