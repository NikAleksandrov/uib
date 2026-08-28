# SPDX-License-Identifier: Apache-2.0

import pathlib
import re
import shutil
import sys
import tempfile

def _slurm_stage_run(args, config, config_path, ledger, missing, network_emulation, run_dir, staged_caches, staged_images, temporary):
    from .core import COMPATIBILITY, ConfigurationError, DEFAULT_RESULT_DIR, SLURM_TERMINAL_STATES
    from .state import copy_file, copy_run_provenance, copy_tree, load_ledger, owned_state_file, path_is_within, read_json, state_path, utc_now, write_json

    for directory in ('logs', 'metrics', 'scheduler', 'state', 'traces'):
        (temporary / directory).mkdir()
    copy_file(run_dir / 'ledger.json', temporary / 'state' / 'ledger.json')
    batch_path = owned_state_file(run_dir, ledger.get('batchFile', 'workload.sbatch'), 'batch file')
    copy_file(batch_path, temporary / 'scheduler' / 'workload.sbatch')
    copy_file(config_path, temporary / 'resolved-values.json')
    copy_file(COMPATIBILITY, temporary / 'compatibility-lock.json')
    staged_path = run_dir / 'infrastructure' / 'images.json'
    if staged_path.is_file() and (not staged_path.is_symlink()):
        staged_images = read_json(staged_path, 'staged image ledger')
        if staged_images.get('schemaVersion') != 1 or staged_images.get('runId') != args.run_id or (not isinstance(staged_images.get('images'), dict)):
            raise ConfigurationError(f'staged image ledger ownership mismatch: {staged_path}')
        copy_file(staged_path, temporary / 'staged-images.json')
    cache_path = run_dir / 'infrastructure' / 'caches.json'
    if cache_path.is_file() and (not cache_path.is_symlink()):
        staged_caches = read_json(cache_path, 'staged cache ledger')
        if staged_caches.get('schemaVersion') != 1 or staged_caches.get('runId') != args.run_id or (not isinstance(staged_caches.get('caches'), dict)):
            raise ConfigurationError(f'staged cache ledger ownership mismatch: {cache_path}')
        copy_file(cache_path, temporary / 'staged-caches.json')
    network_path = run_dir / 'infrastructure' / 'network.json'
    if network_path.is_file() and (not network_path.is_symlink()):
        network_emulation = read_json(network_path, 'network-emulation evidence')
        if network_emulation.get('schemaVersion') != 1 or network_emulation.get('runId') != args.run_id or network_emulation.get('interface') != config['network']['dataInterface'] or (network_emulation.get('shaping') != config['network']['shaping']) or (not isinstance(network_emulation.get('endpoints'), dict)):
            raise ConfigurationError(f'network-emulation evidence mismatch: {network_path}')
        copy_file(network_path, temporary / 'network-emulation.json')
    for name in ('render.stderr', 'sbatch.stdout', 'sbatch.stderr'):
        source = run_dir / name
        if source.is_file():
            copy_file(source, temporary / 'scheduler' / name)
        else:
            missing.append(f'run-state/{name}')
    copy_run_provenance(run_dir, ledger, temporary)
    return network_emulation, staged_caches, staged_images

def _slurm_collect_workload(args, job_id, missing, remote, results_root, status, temporary, workload_results, workload_status):
    from .core import COMPATIBILITY, ConfigurationError, DEFAULT_RESULT_DIR, SLURM_TERMINAL_STATES
    from .state import copy_file, copy_run_provenance, copy_tree, load_ledger, owned_state_file, path_is_within, read_json, state_path, utc_now, write_json
    from .scheduler import copy_remote_path, remote_path_exists, run_scheduler, scheduler_remote

    collected_workload = workload_results
    if remote:
        if remote_path_exists(args, str(workload_results), 'directory'):
            checked = run_scheduler(args, '', 'find', [str(workload_results), '-type', 'l', '-print', '-quit'], text=True, capture_output=True)
            if checked.returncode or checked.stdout.strip():
                raise ConfigurationError('remote workload results contain a symbolic link')
            staged_workload = temporary / '.remote-workload'
            copy_remote_path(args, str(workload_results), staged_workload, recursive=True)
            copy_tree(staged_workload, temporary / 'workload')
            shutil.rmtree(staged_workload)
            collected_workload = temporary / 'workload'
        else:
            collected_workload = None
    elif workload_results.is_symlink():
        raise ConfigurationError(f'workload result directory is a symbolic link: {workload_results}')
    if collected_workload and collected_workload.is_dir():
        status_path = collected_workload / 'status.json'
        if status_path.is_file() and (not status_path.is_symlink()):
            workload_status = read_json(status_path, 'workload status')
            if workload_status.get('runId') != args.run_id or workload_status.get('scheduler') != 'slurm' or str(workload_status.get('jobId')) != job_id or (not isinstance(workload_status.get('exitCode'), int)):
                raise ConfigurationError(f'workload status ownership mismatch: {status_path}')
            if 'expectedFaultOutcome' in workload_status and (not isinstance(workload_status['expectedFaultOutcome'], bool)):
                raise ConfigurationError(f'invalid fault outcome in {status_path}')
        if not remote:
            copy_tree(workload_results, temporary / 'workload')
    else:
        missing.append(f'workload/{args.run_id}')
    for extension in ('out', 'err'):
        source = results_root / f'{args.run_id}-{job_id}.{extension}'
        if remote and remote_path_exists(args, str(source), 'file'):
            copy_remote_path(args, str(source), temporary / 'logs' / f'batch.{extension}')
        elif not remote and source.is_file() and (not source.is_symlink()):
            copy_file(source, temporary / 'logs' / f'batch.{extension}')
        else:
            missing.append(f'logs/batch.{extension}')
    (temporary / 'scheduler' / 'status.psv').write_text(status['normalized'], encoding='utf-8')
    write_json(temporary / 'scheduler' / 'status-source.json', {'schemaVersion': 1, 'source': status['source']})
    (temporary / 'scheduler' / 'sacct.stderr').write_text(status['sacct'].stderr, encoding='utf-8')
    if status['source'] == 'sacct':
        (temporary / 'scheduler' / 'sacct.psv').write_text(status['sacct'].stdout, encoding='utf-8')
    else:
        (temporary / 'scheduler' / 'scontrol.txt').write_text(status['scontrol'].stdout, encoding='utf-8')
        (temporary / 'scheduler' / 'scontrol.stderr').write_text(status['scontrol'].stderr, encoding='utf-8')
    return collected_workload, workload_status

def _slurm_collect_evidence(collected_workload, config, missing, remote, scheduler, staged_caches, staged_images, temporary, workload_results):
    from .core import COMPATIBILITY, ConfigurationError, DEFAULT_RESULT_DIR, SLURM_TERMINAL_STATES
    from .evidence import collect_fault_evidence, collect_workload_metrics, file_manifest, provider_counter_evidence, transport_evidence
    from .state import copy_file, copy_run_provenance, copy_tree, load_ledger, owned_state_file, path_is_within, read_json, state_path, utc_now, write_json

    fault_files, fault_records, applied_fault_targets, recovered_fault_targets = collect_fault_evidence(collected_workload, temporary / 'faults.jsonl')
    expected_fault_targets = {fault['target'] for fault in config.get('faults', []) if fault['action'] in ('add-delay', 'packet-loss', 'link-down', 'partition')}
    if expected_fault_targets - (applied_fault_targets & recovered_fault_targets):
        missing.append('fault-evidence/completed-targets')
    expected_rank_targets = {f"rank-{fault['rank']}" for fault in config.get('faults', []) if fault['action'] == 'terminate-rank'}
    if expected_rank_targets - applied_fault_targets:
        missing.append('fault-evidence/terminated-ranks')
    nodes = []
    nodes_path = temporary / 'workload' / 'nodes.txt' if remote else workload_results / 'nodes.txt'
    if nodes_path.is_file() and (not nodes_path.is_symlink()):
        nodes = [line for line in nodes_path.read_text(encoding='utf-8').splitlines() if line]
    host_probes = []
    probe_path = collected_workload / 'host-probes.psv' if collected_workload else None
    if probe_path and probe_path.is_file() and (not probe_path.is_symlink()):
        for number, line in enumerate(probe_path.read_text(encoding='utf-8').splitlines(), 1):
            fields = line.split('|')
            if len(fields) != 4 or not all(fields):
                raise ConfigurationError(f'invalid Slurm host probe at line {number}')
            host_probes.append({'name': fields[0], 'operatingSystem': fields[1], 'kernelVersion': fields[2], 'architecture': fields[3]})
    else:
        missing.append('host-probes/slurm-nodes')
    tools_path = collected_workload / 'scheduler-tools.txt' if collected_workload else None
    scheduler_tools = None
    if tools_path and tools_path.is_file() and (not tools_path.is_symlink()):
        scheduler_tools = 'workload/scheduler-tools.txt'
    else:
        missing.append('host-probes/slurm-tools')
    write_json(temporary / 'host-probes.json', {'schemaVersion': 1, 'scheduler': 'slurm', 'schedulerTools': scheduler_tools, 'nodes': host_probes})
    parallel = config['workload']['parallelism']
    write_json(temporary / 'topology.json', {'schemaVersion': 1, 'requestedNodes': parallel['nodes'], 'processesPerNode': parallel['processesPerNode'], 'requestedRanks': parallel['nodes'] * parallel['processesPerNode'], 'allocatedNodes': nodes, 'schedulerNodeList': scheduler['nodeList']})
    write_json(temporary / 'images.json', {'schemaVersion': 1, 'workload': config['workload']['image'], 'uibRuntime': config['uib'].get('bundle'), 'stagedImages': 'staged-images.json' if staged_images is not None else None, 'stagedCaches': 'staged-caches.json' if staged_caches is not None else None})
    write_json(temporary / 'versions.json', {'schemaVersion': 1, 'clusterKind': config['cluster']['manager'], 'configuredClusterVersion': config['cluster']['version'], 'compatibilityLock': 'compatibility-lock.json'})
    write_json(temporary / 'preflight.json', {'schemaVersion': 1, 'uibEnabled': config['uib']['enabled'], 'runtimePreflight': 'invoked-by-batch-script' if config['uib']['enabled'] else 'not-applicable', 'verifiedByCollector': False, 'evidence': 'logs/batch.out and logs/batch.err'})
    transport = transport_evidence(config, [('logs', temporary / 'logs'), ('workload', temporary / 'workload')])
    write_json(temporary / 'transport-evidence.json', transport)
    counters = provider_counter_evidence(config, [('logs', temporary / 'logs'), ('workload', temporary / 'workload')])
    write_json(temporary / 'provider-counters.json', counters)
    metrics = collect_workload_metrics(config, [('logs', temporary / 'logs'), ('workload', temporary / 'workload')], temporary / 'metrics' / 'workload.jsonl')
    return counters, fault_files, fault_records, host_probes, metrics, transport

def _slurm_finalize_bundle(args, config, counters, fault_files, fault_records, host_probes, job_id, metrics, missing, network_emulation, output, scheduler, status, temporary, terminal, transport, workload_status):
    from .evidence import collect_fault_evidence, collect_workload_metrics, file_manifest, provider_counter_evidence, transport_evidence
    from .state import copy_file, copy_run_provenance, copy_tree, load_ledger, owned_state_file, path_is_within, read_json, state_path, utc_now, write_json

    summary = {'schemaVersion': 1, 'runId': args.run_id, 'collectedAt': utc_now(), 'completeSchedulerSnapshot': terminal, 'scheduler': 'slurm', 'schedulerEvidenceSource': status['source'], 'jobId': job_id, 'schedulerState': scheduler['state'], 'schedulerExitCode': scheduler['exitCode'], 'workloadExitCode': workload_status.get('exitCode') if workload_status else None, 'underlyingWorkloadExitCode': workload_status.get('workloadExitCode') if workload_status else None, 'expectedFaultOutcome': workload_status.get('expectedFaultOutcome', False) if workload_status else False, 'workloadStatusRecorded': workload_status is not None, 'uibEnabled': config['uib']['enabled'], 'protocol': config['uib']['protocol'] if config['uib']['enabled'] else None, 'congestionControl': config['uib']['congestionControl'] if config['uib']['enabled'] else None, 'memoryMode': config['uib']['memoryMode'] if config['uib']['enabled'] else None, 'communicationFrontend': config['communication']['frontend'], 'validationLevel': 'not-assigned', 'uibTrafficEvidence': transport['classification'], 'uibCounterEvidence': counters['classification'], 'transportEvidence': 'transport-evidence.json', 'providerCounters': 'provider-counters.json', 'workloadMetrics': 'metrics/workload.jsonl', 'workloadMetricRecords': len(metrics), 'hostProbes': 'host-probes.json', 'hostProbeNodes': len(host_probes), 'networkEmulation': 'network-emulation.json' if network_emulation is not None else None, 'faultEvidenceFiles': fault_files, 'faultEvidenceRecords': fault_records, 'promotionEligible': False, 'missingArtifacts': missing}
    write_json(temporary / 'summary.json', summary)
    summary_text = f"# UIB simulation result: {args.run_id}\n\nSlurm job `{job_id}` was `{scheduler['state']}` with scheduler exit code `{scheduler['exitCode']}`. The bundle is {('a terminal' if terminal else 'an incomplete diagnostic')} snapshot.\n\nCollection alone does not prove that application traffic used UIB. Inspect the preflight, transport logs, correctness output, and network evidence before assigning a validation level or promoting this result.\n"
    (temporary / 'summary.md').write_text(summary_text, encoding='utf-8')
    write_json(temporary / 'manifest.json', file_manifest(temporary))
    temporary.rename(output)

def command_slurm_collect(args):
    from .slurm import query_slurm_status
    from .core import COMPATIBILITY, ConfigurationError, DEFAULT_RESULT_DIR, SLURM_TERMINAL_STATES
    from .evidence import collect_fault_evidence, collect_workload_metrics, file_manifest, provider_counter_evidence, transport_evidence
    from .scheduler import copy_remote_path, remote_path_exists, run_scheduler, scheduler_remote
    from .state import copy_file, copy_run_provenance, copy_tree, load_ledger, owned_state_file, path_is_within, read_json, state_path, utc_now, write_json

    run_dir = state_path(args)
    unused, ledger = load_ledger(args, "slurm")
    job_id = ledger.get("jobId")
    if not isinstance(job_id, str) or not job_id.isdecimal():
        raise ConfigurationError("run ledger has no recorded numeric Slurm job ID")
    config_path = owned_state_file(
        run_dir, ledger.get("resolvedConfig", "resolved-simulation.json"),
        "resolved configuration")
    config = read_json(config_path, "resolved configuration")
    if config.get("runId") != args.run_id:
        raise ConfigurationError("resolved configuration ownership mismatch")

    status = query_slurm_status(args, job_id)
    scheduler = status["record"]
    terminal = scheduler["state"] in SLURM_TERMINAL_STATES
    if not terminal and not args.allow_incomplete:
        raise ConfigurationError(
            f"Slurm job {job_id} is {scheduler['state']}; "
            "use --allow-incomplete only for a diagnostic snapshot")

    results_root = pathlib.Path(config["storage"]["resultsPath"]).resolve()
    workload_results = results_root / args.run_id
    remote = scheduler_remote(args)
    output = (pathlib.Path(args.output_dir).expanduser().resolve()
              if args.output_dir else
              (DEFAULT_RESULT_DIR / args.run_id).resolve())
    if output.exists():
        raise ConfigurationError(f"result bundle already exists: {output}")
    if path_is_within(output, workload_results):
        raise ConfigurationError("result bundle cannot be inside workload results")
    if path_is_within(output, run_dir):
        raise ConfigurationError("result bundle cannot be inside run state")
    try:
        output.parent.mkdir(parents=True, exist_ok=True)
        temporary = pathlib.Path(tempfile.mkdtemp(
            prefix=f".{args.run_id}.", dir=output.parent))
    except OSError as error:
        raise ConfigurationError(f"cannot create result bundle: {error}") from error

    missing = []
    workload_status = None
    staged_images = None
    staged_caches = None
    network_emulation = None
    try:
        network_emulation, staged_caches, staged_images = _slurm_stage_run(args, config, config_path, ledger, missing, network_emulation, run_dir, staged_caches, staged_images, temporary)
        collected_workload, workload_status = _slurm_collect_workload(args, job_id, missing, remote, results_root, status, temporary, workload_results, workload_status)
        counters, fault_files, fault_records, host_probes, metrics, transport = _slurm_collect_evidence(collected_workload, config, missing, remote, scheduler, staged_caches, staged_images, temporary, workload_results)
        _slurm_finalize_bundle(args, config, counters, fault_files, fault_records, host_probes, job_id, metrics, missing, network_emulation, output, scheduler, status, temporary, terminal, transport, workload_status)
    except (ConfigurationError, OSError) as error:
        shutil.rmtree(temporary, ignore_errors=True)
        if isinstance(error, ConfigurationError):
            raise
        raise ConfigurationError(f'cannot collect result bundle: {error}') from error
    print(output)
    return 0
