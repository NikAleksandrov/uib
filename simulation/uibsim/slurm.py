# SPDX-License-Identifier: Apache-2.0

import pathlib
import re
import shutil
import sys
import tempfile


def command_slurm_run(args):
    from .config import resolve
    from .core import ConfigurationError, LifecycleInterrupted, SLURM_RENDERER, run_command
    from .scheduler import remote_path_exists, run_scheduler, scheduler_remote
    from .state import prepare_run_directory, record_run_inputs, utc_now, write_json

    config = resolve(args)
    config["runId"] = args.run_id
    run_dir = prepare_run_directory(args, config)

    ledger_path = run_dir / "ledger.json"
    ledger = {
        "schemaVersion": 1,
        "runId": args.run_id,
        "scheduler": "slurm",
        "state": "preparing",
        "createdAt": utc_now(),
        "batchFile": "workload.sbatch",
        "resolvedConfig": "resolved-simulation.json",
    }
    write_json(ledger_path, ledger)
    config_path = run_dir / "resolved-simulation.json"
    write_json(config_path, config)
    try:
        ledger["inputs"] = record_run_inputs(args, run_dir)
    except ConfigurationError as error:
        ledger["state"] = "input-record-failed"
        ledger["error"] = str(error)
        write_json(ledger_path, ledger)
        raise
    write_json(ledger_path, ledger)

    required_paths = [
        config["storage"]["modelCachePath"],
        config["storage"]["resultsPath"],
    ]
    if config["uib"]["enabled"]:
        required_paths.append(config["storage"]["uibRuntimePath"])
    missing = [path for path in required_paths
               if not remote_path_exists(args, path, "directory")]
    if missing:
        ledger["state"] = "preflight-failed"
        ledger["missingPaths"] = missing
        write_json(ledger_path, ledger)
        raise ConfigurationError(
            "Slurm shared paths do not exist: " + ", ".join(missing))

    batch_path = run_dir / "workload.sbatch"
    render_error = run_dir / "render.stderr"
    command = [str(SLURM_RENDERER), str(config_path),
               "--run-id", args.run_id]
    if args.allow_mutable_tags:
        command.append("--allow-mutable-tags")
    try:
        with batch_path.open("w", encoding="utf-8") as batch, \
                render_error.open("w", encoding="utf-8") as errors:
            rendered = run_command(
                command, stdout=batch, stderr=errors)
    except LifecycleInterrupted as error:
        ledger["state"] = "interrupted"
        ledger["interruptedStage"] = "render"
        ledger["error"] = str(error)
        write_json(ledger_path, ledger)
        raise
    except OSError as error:
        ledger["state"] = "render-failed"
        ledger["error"] = str(error)
        write_json(ledger_path, ledger)
        raise ConfigurationError(f"cannot render Slurm workload: {error}") from error
    if rendered.returncode:
        ledger["state"] = "render-failed"
        ledger["renderExitCode"] = rendered.returncode
        write_json(ledger_path, ledger)
        try:
            print(render_error.read_text(encoding="utf-8"),
                  file=sys.stderr, end="")
        except OSError:
            pass
        return rendered.returncode

    ledger["state"] = "rendered"
    write_json(ledger_path, ledger)
    try:
        remote = scheduler_remote(args)
        if remote:
            submitted = run_scheduler(
                args, "SBATCH_COMMAND", "sbatch", ["--parsable"],
                input=batch_path.read_text(encoding="utf-8"), text=True,
                capture_output=True)
            ledger["schedulerTransport"] = f"{remote['provider']}-ssh"
        else:
            submitted = run_scheduler(
                args, "SBATCH_COMMAND", "sbatch",
                ["--parsable", str(batch_path)], text=True,
                capture_output=True)
            ledger["schedulerTransport"] = "local"
    except LifecycleInterrupted as error:
        ledger["state"] = "submission-unknown"
        ledger["error"] = str(error)
        write_json(ledger_path, ledger)
        raise
    except (ConfigurationError, OSError) as error:
        ledger["state"] = "submission-not-started"
        ledger["error"] = str(error)
        write_json(ledger_path, ledger)
        raise ConfigurationError(f"cannot invoke sbatch: {error}") from error
    try:
        (run_dir / "sbatch.stdout").write_text(
            submitted.stdout, encoding="utf-8")
        (run_dir / "sbatch.stderr").write_text(
            submitted.stderr, encoding="utf-8")
    except OSError as error:
        ledger["state"] = "submission-unknown"
        ledger["error"] = f"cannot record sbatch output: {error}"
        write_json(ledger_path, ledger)
        raise ConfigurationError(
            f"sbatch returned but its output cannot be recorded: {error}") from error
    if submitted.returncode:
        ledger["state"] = "submission-failed"
        ledger["submissionExitCode"] = submitted.returncode
        write_json(ledger_path, ledger)
        if submitted.stderr:
            print(submitted.stderr, file=sys.stderr, end="")
        return submitted.returncode

    match = re.fullmatch(r"([0-9]+)(?:;[^\s;]+)?\s*", submitted.stdout)
    if not match:
        ledger["state"] = "submission-unknown"
        ledger["submissionOutput"] = submitted.stdout
        write_json(ledger_path, ledger)
        raise ConfigurationError(
            f"sbatch succeeded without a parseable job ID; inspect {run_dir}")
    ledger["state"] = "submitted"
    ledger["jobId"] = match.group(1)
    ledger["submittedAt"] = utc_now()
    write_json(ledger_path, ledger)
    print(ledger["jobId"])
    return 0


def command_slurm_status(args):
    from .core import ConfigurationError
    from .state import load_ledger

    unused, ledger = load_ledger(args, "slurm")
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


from .slurm_collection import command_slurm_collect


def command_slurm_down(args):
    from .core import ConfigurationError
    from .scheduler import run_scheduler
    from .state import load_ledger, utc_now, write_json

    ledger_path, ledger = load_ledger(args, "slurm")
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
