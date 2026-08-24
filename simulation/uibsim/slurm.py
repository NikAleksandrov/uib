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


def exact_slurm_record(output, job_id):
    from .core import ConfigurationError

    for line in output.splitlines():
        fields = line.split("|")
        if len(fields) >= 5 and fields[0] == job_id:
            state = fields[1].split(maxsplit=1)[0].rstrip("+")
            return {
                "jobId": fields[0],
                "state": state,
                "exitCode": fields[2],
                "elapsed": fields[3],
                "nodeList": fields[4],
            }
    raise ConfigurationError(
        f"sacct returned no exact accounting row for job {job_id}")


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
