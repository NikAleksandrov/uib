# SPDX-License-Identifier: Apache-2.0

import json
import pathlib
import re
import shutil
import sys
import tempfile

def record_kubernetes_output(args, config, arguments, output, required=True):
    from .core import ConfigurationError
    from .faults import run_kubectl

    result = run_kubectl(args, config, arguments,
                         text=True, capture_output=True)
    output.write_text(result.stdout, encoding="utf-8")
    error = output.with_suffix(output.suffix + ".stderr")
    error.write_text(result.stderr, encoding="utf-8")
    if result.returncode and required:
        detail = result.stderr.strip() or result.stdout.strip()
        raise ConfigurationError(
            f"kubectl {' '.join(arguments[:3])} failed: {detail}")
    return result


def _kubernetes_stage_run(config_path, ledger, missing, run_dir, temporary):
    from .core import COMPATIBILITY, ConfigurationError, DEFAULT_RESULT_DIR
    from .state import copy_file, copy_run_provenance, load_ledger, owned_state_file, path_is_within, read_json, state_path, utc_now, write_json

    for directory in ('logs', 'metrics', 'scheduler', 'state', 'traces'):
        (temporary / directory).mkdir()
    copy_file(run_dir / 'ledger.json', temporary / 'state' / 'ledger.json')
    manifest = owned_state_file(run_dir, ledger.get('manifest', 'workload.yaml'), 'manifest')
    copy_file(manifest, temporary / 'scheduler' / 'workload.yaml')
    copy_file(config_path, temporary / 'resolved-values.json')
    copy_file(COMPATIBILITY, temporary / 'compatibility-lock.json')
    for name in ('render.stderr', 'kubectl-apply.stdout', 'kubectl-apply.stderr', 'fault-scheduler.stdout', 'fault-scheduler.stderr'):
        source = run_dir / name
        if source.is_file() and (not source.is_symlink()):
            copy_file(source, temporary / 'scheduler' / name)
        else:
            missing.append(f'run-state/{name}')
    copy_run_provenance(run_dir, ledger, temporary)

def _kubernetes_collect_scheduler(args, config, missing, status, temporary):
    from .state import copy_file, copy_run_provenance, load_ledger, owned_state_file, path_is_within, read_json, state_path, utc_now, write_json

    write_json(temporary / 'scheduler' / 'workload.json', status['object'])
    write_json(temporary / 'scheduler' / 'pods.json', {'apiVersion': 'v1', 'kind': 'List', 'items': status['pods']})
    record_kubernetes_output(args, config, ['get', 'namespace', args.run_id, '-o', 'yaml'], temporary / 'scheduler' / 'namespace.yaml')
    events = record_kubernetes_output(args, config, ['get', 'events', '--namespace', args.run_id, '--sort-by=.metadata.creationTimestamp', '-o', 'yaml'], temporary / 'scheduler' / 'events.yaml', required=False)
    if events.returncode:
        missing.append('scheduler/events.yaml')
    version_result = record_kubernetes_output(args, config, ['version', '-o', 'json'], temporary / 'scheduler' / 'kubectl-version.json', required=False)
    nodes_result = record_kubernetes_output(args, config, ['get', 'nodes', '-o', 'json'], temporary / 'scheduler' / 'nodes.json', required=False)
    probes = []
    kubernetes_versions = None
    if not version_result.returncode:
        try:
            kubernetes_versions = json.loads(version_result.stdout)
        except json.JSONDecodeError:
            missing.append('host-probes/kubernetes-versions')
    else:
        missing.append('host-probes/kubernetes-versions')
    if not nodes_result.returncode:
        try:
            node_items = json.loads(nodes_result.stdout).get('items', [])
        except json.JSONDecodeError:
            node_items = []
            missing.append('host-probes/kubernetes-nodes')
        for node in node_items:
            metadata = node.get('metadata', {})
            info = node.get('status', {}).get('nodeInfo', {})
            probes.append({'name': metadata.get('name'), 'architecture': info.get('architecture'), 'operatingSystem': info.get('operatingSystem'), 'osImage': info.get('osImage'), 'kernelVersion': info.get('kernelVersion'), 'containerRuntimeVersion': info.get('containerRuntimeVersion'), 'kubeletVersion': info.get('kubeletVersion')})
    else:
        missing.append('host-probes/kubernetes-nodes')
    write_json(temporary / 'host-probes.json', {'schemaVersion': 1, 'scheduler': 'kubernetes', 'versions': kubernetes_versions, 'nodes': probes})
    return probes

def _kubernetes_collect_logs(args, config, missing, status, temporary):
    from .core import COMPATIBILITY, ConfigurationError, DEFAULT_RESULT_DIR
    from .state import copy_file, copy_run_provenance, load_ledger, owned_state_file, path_is_within, read_json, state_path, utc_now, write_json

    preflight_pods = 0
    passed_preflights = 0
    for pod in status['pods']:
        metadata = pod.get('metadata', {})
        pod_name = metadata.get('name')
        if not isinstance(pod_name, str) or not re.fullmatch('[a-z0-9](?:[-a-z0-9.]*[a-z0-9])?', pod_name):
            raise ConfigurationError('Kubernetes pod has an invalid name')
        spec = pod.get('spec', {})
        containers = [*spec.get('initContainers', []), *spec.get('containers', [])]
        for container in containers:
            container_name = container.get('name')
            if not isinstance(container_name, str) or not re.fullmatch('[a-z0-9](?:[-a-z0-9.]*[a-z0-9])?', container_name):
                raise ConfigurationError(f'pod {pod_name} has an invalid container name')
            log_path = temporary / 'logs' / f'{pod_name}.{container_name}.log'
            log = record_kubernetes_output(args, config, ['logs', '--namespace', args.run_id, pod_name, '--container', container_name], log_path, required=False)
            if log.returncode:
                missing.append(f'logs/{log_path.name}')
            if container_name == 'uib-runtime-check':
                preflight_pods += 1
                if not log.returncode and 'UIB runtime preflight: PASS' in log.stdout:
                    passed_preflights += 1
    allocated_nodes = sorted({pod.get('spec', {}).get('nodeName') for pod in status['pods'] if pod.get('spec', {}).get('nodeName')})
    parallel = config['workload']['parallelism']
    write_json(temporary / 'topology.json', {'schemaVersion': 1, 'requestedNodes': parallel['nodes'], 'processesPerNode': parallel['processesPerNode'], 'requestedRanks': parallel['nodes'] * parallel['processesPerNode'], 'allocatedNodes': allocated_nodes, 'podCount': len(status['pods'])})
    write_json(temporary / 'images.json', {'schemaVersion': 1, 'workload': config['workload']['image'], 'uibRuntime': config['uib'].get('bundle')})
    write_json(temporary / 'versions.json', {'schemaVersion': 1, 'clusterKind': 'kubernetes', 'configuredClusterVersion': config['cluster']['version'], 'compatibilityLock': 'compatibility-lock.json'})
    verified = config['uib']['enabled'] and preflight_pods == len(status['pods']) and (passed_preflights == preflight_pods) and (preflight_pods > 0)
    write_json(temporary / 'preflight.json', {'schemaVersion': 1, 'uibEnabled': config['uib']['enabled'], 'runtimePreflight': 'pod-init-container' if config['uib']['enabled'] else 'not-applicable', 'expectedPods': len(status['pods']), 'recordedPods': preflight_pods, 'passedPods': passed_preflights, 'verifiedByCollector': verified, 'evidence': 'logs/*.uib-runtime-check.log'})
    return verified

def _kubernetes_collect_evidence(config, missing, probes, run_dir, status, temporary):
    from .evidence import collect_fault_evidence, collect_workload_metrics, file_manifest, normalized_result, provider_counter_evidence, transport_evidence
    from .state import copy_file, copy_run_provenance, load_ledger, owned_state_file, path_is_within, read_json, state_path, utc_now, write_json

    transport = transport_evidence(config, [('logs', temporary / 'logs')])
    write_json(temporary / 'transport-evidence.json', transport)
    counters = provider_counter_evidence(config, [('logs', temporary / 'logs')])
    write_json(temporary / 'provider-counters.json', counters)
    metrics = collect_workload_metrics(config, [('logs', temporary / 'logs')], temporary / 'metrics' / 'workload.jsonl')
    fault_files, fault_records, applied_fault_targets, recovered_fault_targets = collect_fault_evidence(run_dir, temporary / 'faults.jsonl')
    expected_recovery = {fault['target'] for fault in config.get('faults', []) if fault['action'] in {'checkpoint-storage-delay', 'delay-worker'}}
    expected_termination = {f"rank-{fault['rank']}" for fault in config.get('faults', []) if fault['action'] == 'terminate-rank'}
    if expected_recovery - (applied_fault_targets & recovered_fault_targets):
        missing.append('fault-evidence/completed-targets')
    if expected_termination - applied_fault_targets:
        missing.append('fault-evidence/terminated-ranks')
    write_json(temporary / 'normalized-result.json', normalized_result(config, 'kubernetes', status['state'], status['terminal'], metrics, transport, counters, len(probes), missing))
    return counters, fault_files, fault_records, metrics, transport

def _kubernetes_finalize_bundle(args, config, counters, fault_files, fault_records, metrics, missing, output, probes, status, temporary, transport, verified):
    from .evidence import collect_fault_evidence, collect_workload_metrics, file_manifest, normalized_result, provider_counter_evidence, transport_evidence
    from .state import copy_file, copy_run_provenance, load_ledger, owned_state_file, path_is_within, read_json, state_path, utc_now, write_json

    summary = {'schemaVersion': 1, 'runId': args.run_id, 'collectedAt': utc_now(), 'completeSchedulerSnapshot': status['terminal'], 'scheduler': 'kubernetes', 'schedulerState': status['state'], 'namespace': args.run_id, 'resource': f"{status['resource']}/{args.run_id}", 'podPhases': status['podPhases'], 'workloadStatusRecorded': True, 'uibEnabled': config['uib']['enabled'], 'uibPreflightVerified': verified, 'protocol': config['uib']['protocol'] if config['uib']['enabled'] else None, 'congestionControl': config['uib']['congestionControl'] if config['uib']['enabled'] else None, 'memoryMode': config['uib']['memoryMode'] if config['uib']['enabled'] else None, 'communicationFrontend': config['communication']['frontend'], 'validationLevel': 'not-assigned', 'uibTrafficEvidence': transport['classification'], 'uibCounterEvidence': counters['classification'], 'transportEvidence': 'transport-evidence.json', 'providerCounters': 'provider-counters.json', 'workloadMetrics': 'metrics/workload.jsonl', 'normalizedResult': 'normalized-result.json', 'workloadMetricRecords': len(metrics), 'hostProbes': 'host-probes.json', 'hostProbeNodes': len(probes), 'faultEvidenceFiles': fault_files, 'faultEvidenceRecords': fault_records, 'promotionEligible': False, 'missingArtifacts': missing}
    write_json(temporary / 'summary.json', summary)
    (temporary / 'summary.md').write_text(f"# UIB simulation result: {args.run_id}\n\nKubernetes {status['resource']}/{args.run_id} was `{status['state']}`. This is {('a terminal' if status['terminal'] else 'an incomplete diagnostic')} snapshot.\n\nCollection verifies runtime preflight logs but does not by itself prove that application traffic used UIB. Inspect transport, correctness, and network evidence before promotion.\n", encoding='utf-8')
    write_json(temporary / 'manifest.json', file_manifest(temporary))
    temporary.rename(output)

def command_kubernetes_collect(args):
    from .kubernetes import query_kubernetes_status
    from .core import COMPATIBILITY, ConfigurationError, DEFAULT_RESULT_DIR
    from .evidence import collect_fault_evidence, collect_workload_metrics, file_manifest, normalized_result, provider_counter_evidence, transport_evidence
    from .state import copy_file, copy_run_provenance, load_ledger, owned_state_file, path_is_within, read_json, state_path, utc_now, write_json

    run_dir = state_path(args)
    unused, ledger = load_ledger(args, "kubernetes")
    config_path = owned_state_file(
        run_dir, ledger.get("resolvedConfig", "resolved-simulation.json"),
        "resolved configuration")
    config = read_json(config_path, "resolved configuration")
    if config.get("runId") != args.run_id:
        raise ConfigurationError("resolved configuration ownership mismatch")
    status = query_kubernetes_status(
        args, config, ledger.get("resourceKind"))
    if not status["terminal"] and not args.allow_incomplete:
        raise ConfigurationError(
            f"Kubernetes {status['resource']}/{args.run_id} is "
            f"{status['state']}; use --allow-incomplete only for a "
            "diagnostic snapshot")

    output = (pathlib.Path(args.output_dir).expanduser().resolve()
              if args.output_dir else
              (DEFAULT_RESULT_DIR / args.run_id).resolve())
    if output.exists():
        raise ConfigurationError(f"result bundle already exists: {output}")
    if path_is_within(output, run_dir):
        raise ConfigurationError("result bundle cannot be inside run state")
    try:
        output.parent.mkdir(parents=True, exist_ok=True)
        temporary = pathlib.Path(tempfile.mkdtemp(
            prefix=f".{args.run_id}.", dir=output.parent))
    except OSError as error:
        raise ConfigurationError(f"cannot create result bundle: {error}") from error

    missing = []
    try:
        _kubernetes_stage_run(config_path, ledger, missing, run_dir, temporary)
        probes = _kubernetes_collect_scheduler(args, config, missing, status, temporary)
        verified = _kubernetes_collect_logs(args, config, missing, status, temporary)
        counters, fault_files, fault_records, metrics, transport = _kubernetes_collect_evidence(config, missing, probes, run_dir, status, temporary)
        _kubernetes_finalize_bundle(args, config, counters, fault_files, fault_records, metrics, missing, output, probes, status, temporary, transport, verified)
    except (ConfigurationError, OSError) as error:
        shutil.rmtree(temporary, ignore_errors=True)
        if isinstance(error, ConfigurationError):
            raise
        raise ConfigurationError(f'cannot collect result bundle: {error}') from error
    print(output)
    return 0
