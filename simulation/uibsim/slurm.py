# SPDX-License-Identifier: Apache-2.0

import re


def command_slurm_status(args):
    from .core import ConfigurationError
    from .state import load_ledger

    unused, ledger = load_ledger(args)
    job_id = ledger.get("jobId")
    if not job_id:
        raise ConfigurationError("run ledger has no recorded Slurm job ID")
    status = query_slurm_status(args, job_id)
    print(status["normalized"], end="")
    return 0


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


def exact_scontrol_record(output, job_id):
    from .core import ConfigurationError

    fields = dict(re.findall(
        r"(?:^|\s)([A-Za-z][A-Za-z0-9]*)=(\S*)", output))
    if fields.get("JobId") != job_id:
        raise ConfigurationError(
            f"scontrol returned no exact job record for {job_id}")
    required = ("JobState", "ExitCode", "RunTime", "NodeList")
    if any(name not in fields for name in required):
        raise ConfigurationError(
            f"scontrol returned an incomplete job record for {job_id}")
    return {
        "jobId": job_id,
        "state": fields["JobState"].split(maxsplit=1)[0].rstrip("+"),
        "exitCode": fields["ExitCode"],
        "elapsed": fields["RunTime"],
        "nodeList": fields["NodeList"],
    }


def normalized_slurm_record(record):
    return "|".join((
        record["jobId"], record["state"], record["exitCode"],
        record["elapsed"], record["nodeList"],
    )) + "\n"


def query_slurm_status(args, job_id):
    from .core import ConfigurationError
    from .scheduler import run_scheduler

    accounting = run_scheduler(args, "SACCT_COMMAND", "sacct", [
        "--noheader", "--parsable2", f"--jobs={job_id}",
        "--format=JobIDRaw,State,ExitCode,Elapsed,NodeList",
    ], text=True, capture_output=True)
    if accounting.returncode == 0:
        try:
            record = exact_slurm_record(accounting.stdout, job_id)
            return {
                "source": "sacct",
                "record": record,
                "normalized": normalized_slurm_record(record),
                "sacct": accounting,
                "scontrol": None,
            }
        except ConfigurationError:
            pass

    control = run_scheduler(args, "SCONTROL_COMMAND", "scontrol", [
        "--oneliner", "show", "job", job_id,
    ], text=True, capture_output=True)
    if control.returncode == 0:
        try:
            record = exact_scontrol_record(control.stdout, job_id)
            return {
                "source": "scontrol",
                "record": record,
                "normalized": normalized_slurm_record(record),
                "sacct": accounting,
                "scontrol": control,
            }
        except ConfigurationError:
            pass

    details = []
    if accounting.stderr.strip():
        details.append(f"sacct: {accounting.stderr.strip()}")
    if control.stderr.strip():
        details.append(f"scontrol: {control.stderr.strip()}")
    if control.stdout.strip():
        details.append(f"scontrol output: {control.stdout.strip()}")
    suffix = "; ".join(details) or "no exact scheduler record returned"
    raise ConfigurationError(
        f"cannot query Slurm job {job_id}: {suffix}")


def command_slurm_down(args):
    from .core import ConfigurationError
    from .scheduler import run_scheduler
    from .state import load_ledger, utc_now, write_json

    ledger_path, ledger = load_ledger(args)
    if ledger.get("state") == "cancellation-requested":
        return 0
    job_id = ledger.get("jobId")
    if not job_id:
        raise ConfigurationError("run ledger has no recorded Slurm job ID")
    try:
        result = run_scheduler(
            args, "SCANCEL_COMMAND", "scancel", ["--quiet", job_id])
    except ConfigurationError as error:
        raise ConfigurationError(f"cannot invoke scancel: {error}") from error
    if result.returncode:
        return result.returncode
    ledger["state"] = "cancellation-requested"
    ledger["cancellationRequestedAt"] = utc_now()
    write_json(ledger_path, ledger)
    return 0
