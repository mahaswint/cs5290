#!/usr/bin/env python3
"""Isolated, resumable factorial comparison of retention, lists, and lookup."""
import argparse
import concurrent.futures
import csv
import fcntl
import hashlib
import json
import shutil
import statistics
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
ADAPTIVE = HERE.parent / 'adaptive'
sys.path.insert(0, str(ADAPTIVE))
import experiment as shared

MODES = {'scan': (0, 0), 'lists': (1, 0), 'lookup': (0, 1), 'indexed': (1, 1)}
WORKLOADS = {'control': 0, 'scatter': 1, 'handoff': 2, 'pressure': 3, 'graph': 4, 'backend': 5, 'resident': 6}
RUNTIME = HERE / 'runtime'


def allocator_source():
    backend = (ADAPTIVE / 'buddy_backend.inc').read_text()
    baseline = (ADAPTIVE / 'allocator.c').read_text().replace('#include "buddy_backend.inc"', backend)
    return ((HERE / 'allocator.c').read_text().replace('#include "../adaptive/allocator.c"', baseline)
            .replace('#include "../adaptive/buddy_backend.inc"', backend))


def source_for(a):
    allocator = allocator_source()
    manifest = None
    if a.workload == 'graph':
        from graph.trace import make_trace
        trace, manifest = make_trace(a.graph_edges, a.seed, a.threads)
        kernel = (ADAPTIVE / 'graph/kernel.c').read_text().replace('#include "allocator.c"', allocator)
        source = (ADAPTIVE / 'graph/benchmark.c').read_text().replace('#include "kernel.c"', kernel)
        source = source.replace('#include "trace.inc"', trace)
    else:
        source = (HERE / 'benchmark.c').read_text()
        if a.workload == 'resident':
            helpers = source[:source.index('int main(void)')]
            source = (HERE / 'resident.c').read_text().replace('#include "benchmark_helpers.c"', helpers)
        source = source.replace('#include "allocator.c"', allocator)
    lists, lookup = MODES[a.mode]
    definitions = {'NR_TASKLETS': a.threads, 'POLICY': shared.POLICIES[a.policy],
                   'LISTS': lists, 'LOOKUP': lookup, 'WORKLOAD': WORKLOADS[a.workload],
                   'BURST': a.burst, 'ROUNDS': a.rounds, 'HEAP_BYTES': a.heap_kib * 1024,
                   'KEEP_PAGES': a.keep, 'MAX_KEEP': a.max_keep, 'EMPTY_BUDGET': a.budget_pages,
                   'DECAY_OPS': a.decay_ops, 'PRESSURE_PERCENT': a.pressure_percent,
                   'VALIDATE': int(a.validate), 'TRACE_SEED': a.seed,
                   'GRAPH_BLOCK_BYTES': a.graph_block_bytes}
    source = ''.join(f'#define {k} {v}\n' for k, v in definitions.items()) + source
    return source, manifest


def parse(out, a, started, source_hash):
    events, stats, snapshots = [], [], []
    backend = wram = None
    passed = False
    for line in (out / 'simulate.log').read_text().splitlines():
        fields = line.split(',')
        if fields[0] == 'POLICY_EVENT':
            events.append(dict(zip(['kind', 'thread', 'index', 'begin_cycle', 'end_cycle', 'cycles'],
                                   [fields[1]] + list(map(int, fields[2:])), strict=True)))
        elif fields[0] == 'POLICY_STATS':
            stats.append(dict(zip(shared.STATS, map(int, fields[1:]), strict=True)))
        elif fields[0] == 'POLICY_SNAPSHOT':
            snapshots.append(dict(zip(['phase', 'round', 'requested_bytes', 'slot_bytes', 'reserved_bytes', 'empty_bytes'],
                                       map(int, fields[1:]), strict=True)))
        elif fields[0] == 'POLICY_BACKEND':
            backend = dict(zip(['used_bytes', 'peak_bytes', 'allocations', 'frees', 'failed_attempts'], map(int, fields[1:]), strict=True))
        elif fields[0] == 'POLICY_WRAM':
            wram = dict(zip(['heap_end_address', 'cache_descriptor_bytes', 'stats_bytes'], map(int, fields[1:]), strict=True))
        elif line == 'POLICY_CHECK,PASS':
            passed = True
    assert passed and len(stats) == a.threads and backend is not None and wram is not None
    assert backend['used_bytes'] == 0 and backend['allocations'] == backend['frees']
    assert wram['heap_end_address'] <= 65536
    assert len(snapshots) == 2 + 8 * a.rounds
    for s in stats:
        assert s['successes'] == s['free_calls'] and s['live_slots'] == s['retained'] == 0
        for kind, key in [('alloc', 'alloc_calls'), ('free', 'free_calls')]:
            rows = [e for e in events if e['kind'] == kind and e['thread'] == s['thread']]
            assert len(rows) == s[key] and [e['index'] for e in rows] == list(range(s[key]))
    for filename, rows in [('events.csv', events), ('stats.csv', stats), ('snapshots.csv', snapshots)]:
        with (out / filename).open('w', newline='') as f:
            writer = csv.DictWriter(f, fieldnames=list(rows[0])); writer.writeheader(); writer.writerows(rows)
    metrics = {}
    for kind in ('alloc', 'free', 'init', 'work', 'drain', 'total'):
        values = [e['cycles'] / 350 for e in events if e['kind'] == kind]
        assert values or (kind == 'free' and not sum(s['free_calls'] for s in stats))
        metrics[kind] = {'samples': len(values), 'mean_us': statistics.mean(values) if values else None,
                         'p99_us': shared.percentile(values, .99), 'max_us': max(values) if values else None}
    windows = [e for e in events if e['kind'] == 'work']
    makespan = (max(e['end_cycle'] for e in windows) - min(e['begin_cycle'] for e in windows)) / 350
    totals = {k: sum(s[k] for s in stats) for k in shared.STATS if k != 'thread'}
    native = {}
    for line in (out / 'log.txt').read_text().splitlines():
        if ': ' in line:
            key, value = line.rsplit(': ', 1)
            if value.isdigit(): native[key] = int(value)
    owner_bytes = (a.heap_kib * 1024 // 4096) * 8 if MODES[a.mode][1] else 0
    result = {'configuration': vars(a), 'correctness_passed': True, 'frequency_mhz': 350,
              'outcome': 'oom' if totals['failures'] else 'completed', 'metrics': metrics,
              'work_makespan_us': makespan, 'totals': totals, 'backend': backend, 'wram': wram,
              'owner_table_mram_bytes': owner_bytes,
              'peak_payload_plus_owner_bytes': backend['peak_bytes'] + owner_bytes,
              'native_statistics': native, 'source_sha256': source_hash,
              'simulator_sha256': hashlib.sha256((ADAPTIVE / 'runtime/build/uPIMulator').read_bytes()).hexdigest(),
              'host_runtime_seconds': time.time() - started,
              'capacity_profile': 'WRAM address end 65536; 24 stacks x 1536 B; inherited IRAM/timing'}
    if a.workload == 'graph':
        from graph.trace import add_measurements
        add_measurements(result, events, out)
    else:
        assert len(windows) == a.threads
        expected = a.burst * a.rounds * 4
        if a.workload == 'scatter': expected += (a.burst // 2) * a.rounds * 4
        if a.workload == 'resident': expected = a.burst + (a.burst // 2) * a.rounds * 4
        result['requested_allocations_per_tasklet'] = expected
        for s in stats:
            if not s['failures']: assert s['alloc_calls'] == expected
        if a.workload == 'resident':
            spans = {e['thread']: e for e in windows}
            for kind in ('alloc', 'free'):
                values = [e['cycles']/350 for e in events if e['kind'] == kind and
                          spans[e['thread']]['begin_cycle'] <= e['begin_cycle'] and
                          e['end_cycle'] <= spans[e['thread']]['end_cycle']]
                result['work_' + kind] = dict(samples=len(values), mean_us=statistics.mean(values) if values else None,
                                             p99_us=shared.percentile(values, .99))
    temporary = out / '.summary.json.tmp'
    temporary.write_text(json.dumps(result, indent=2) + '\n')
    temporary.replace(out / 'summary.json')
    return result


def run(a):
    source, manifest = source_for(a)
    source_hash = hashlib.sha256(source.encode()).hexdigest()
    identity = hashlib.sha256(json.dumps(vars(a), sort_keys=True).encode()).hexdigest()[:10]
    name = f'{a.workload}_t{a.threads}_{a.mode}_{a.policy}-k{a.keep}_{identity}'
    out = HERE / 'results' / a.tag / name
    if (out / 'summary.json').exists():
        old = json.loads((out / 'summary.json').read_text())
        simulator_hash = hashlib.sha256((ADAPTIVE / 'runtime/build/uPIMulator').read_bytes()).hexdigest()
        if old['source_sha256'] == source_hash and old['simulator_sha256'] == simulator_hash:
            print(f'Complete: {name}', flush=True); return
    if out.exists(): out.rename(out.with_name(name + f'.superseded-{time.time_ns()}'))
    out.mkdir(parents=True)
    started, env = time.time(), shared.environment()
    compiler = shared.BASE / 'toolchain/LoCaLUT/upmem-2023.2.0-Linux-x86_64/bin/dpu-upmem-dpurte-clang'
    with (shared.BASE / '.prepare.lock').open('w') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        if not RUNTIME.exists():
            shutil.copytree(ADAPTIVE / 'runtime', RUNTIME, symlinks=True,
                            ignore=shutil.ignore_patterns('build', 'bin', '__pycache__', '*.pprof'))
        # Only SDK cache selection uses shared module state, under the common lock.
        old_runtime = shared.RUNTIME
        try:
            shared.RUNTIME = RUNTIME
            shared.compile_sdk(compiler, a.threads, env)
        finally:
            shared.RUNTIME = old_runtime
        task = RUNTIME / 'benchmark/microbenchmark/dpu/task.c'
        task.write_text(source); (out / 'task.c').write_text(source)
        if manifest: (out / 'trace.json').write_text(json.dumps(manifest, indent=2) + '\n')
        asm = RUNTIME / 'benchmark/build/microbenchmark/dpu/CMakeFiles/microbenchmark_device.dir/task.c.o'
        asm.parent.mkdir(parents=True, exist_ok=True)
        compile_command = [compiler, '-w', '-O3', '-S', '-c', task, '-o', asm]
        shared.execute(compile_command, out / 'compile.log', env=env)
        shutil.copy2(asm, out / 'task.c.o')
        exe = ADAPTIVE / 'runtime/build/uPIMulator'
        command = [exe, '--root_dirpath', RUNTIME, '--bin_dirpath', out, '--benchmark', 'microbenchmark',
                   '--num_channels', '1', '--num_ranks_per_channel', '1', '--num_dpus_per_rank', '1',
                   '--num_vm_ranks_per_channel', '1', '--num_vm_banks_per_rank', '1',
                   '--num_tasklets', a.threads, '--verbose', '0']
        (out / 'command.json').write_text(json.dumps({'configuration': vars(a), 'source_sha256': source_hash,
                                                    'compiler': list(map(str, compile_command)),
                                                    'simulator': list(map(str, command))}, indent=2) + '\n')
        shared.execute(command + ['--mode', 'link'], out / 'link.log', env=env)
    print(f'Running: {name}', flush=True)
    shared.execute(command + ['--mode', 'simulate'], out / 'simulate.log', env=env)
    result = parse(out, a, started, source_hash)
    print(f"{result['outcome']}: {name}; work {result['work_makespan_us']/1000:.2f} ms; "
          f"free mean {result['metrics']['free']['mean_us']} us", flush=True)


def options():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--mode', choices=MODES, default='indexed')
    p.add_argument('--policy', choices=shared.POLICIES, default='adaptive')
    p.add_argument('--workload', choices=WORKLOADS, default='scatter')
    p.add_argument('--threads', type=int, choices=range(1, 17), default=1)
    p.add_argument('--burst', type=int, default=128)
    p.add_argument('--rounds', type=int, choices=range(1, 17), default=3)
    p.add_argument('--heap-kib', type=int, default=32768)
    p.add_argument('--keep', type=int, choices=range(9), default=2)
    p.add_argument('--max-keep', type=int, choices=range(9), default=2)
    p.add_argument('--budget-pages', type=int, choices=range(65), default=4)
    p.add_argument('--decay-ops', type=int, default=64)
    p.add_argument('--pressure-percent', type=int, choices=range(1, 101), default=75)
    p.add_argument('--seed', type=int, default=5290)
    p.add_argument('--graph-edges', type=int, default=384)
    p.add_argument('--graph-block-bytes', type=int, choices=[64, 128, 256], default=256)
    p.add_argument('--validate', action='store_true')
    p.add_argument('--tag', default='pilot-v1')
    return p


def validate(p, a):
    for key in ('heap_kib', 'decay_ops', 'burst'):
        value = getattr(a, key)
        if value <= 0 or value & (value - 1): p.error(key + ' must be a power of two')
    if not 4 <= a.heap_kib <= 32768: p.error('heap-kib must be 4..32768')
    if not 4 <= a.burst <= 1024: p.error('burst must be 4..1024')
    if not 0 <= a.seed <= 0x3fffffff: p.error('seed must be 0..1073741823')
    if not a.tag or any(x in a.tag for x in ('/', '\\', '..')): p.error('invalid tag')
    if a.workload == 'graph' and (not 48 <= a.graph_edges <= 3072 or a.graph_edges % 3): p.error('invalid graph-edges')


if __name__ == '__main__':
    parser = options(); args = parser.parse_args(); validate(parser, args); run(args)
