#!/usr/bin/env python3
"""Compile, run, and resume isolated size-dependent slab experiments."""
import argparse
import concurrent.futures
import csv
import fcntl
import hashlib
import json
import math
import os
from pathlib import Path
import shutil
import statistics
import subprocess
import sys
import time

HERE = Path(__file__).resolve().parent
BASE = HERE.parent
ROOT = BASE.parent
RUNTIME = HERE / 'runtime'
WORKLOADS = {'burst': 0, 'phases': 1, 'sparse': 2, 'correctness': 3}
STATS = ['thread', 'alloc_calls', 'successes', 'free_calls', 'cache_hits', 'refills',
         'returns', 'failures', 'direct_allocations', 'slab_hits', 'slab_refills', 'live_slots', 'peak_slots']


def execute(command, log, **kwargs):
    with Path(log).open('w') as stream:
        result = subprocess.run([str(x) for x in command], stdout=stream, stderr=subprocess.STDOUT, **kwargs)
    if result.returncode:
        tail = '\n'.join(Path(log).read_text().splitlines()[-12:])
        raise RuntimeError(f'Command failed; see {log}\n{tail}')


def environment():
    env = os.environ.copy()
    compat = BASE / 'toolchain/compat/lib/x86_64-linux-gnu'
    env['LD_LIBRARY_PATH'] = str(compat) + (':' + env['LD_LIBRARY_PATH'] if env.get('LD_LIBRARY_PATH') else '')
    env['UPMEM_NO_OS_WARNING'] = '1'
    env['GOCACHE'] = str(ROOT / '.cache/go-build')
    env['REPRO_IRAM_CACHE'] = '1'
    return env


def compile_sdk(compiler, threads, env):
    # Identical SDK assembly can be shared with the reference runner. A common
    # lock protects this cache and each runtime's own sdk/build symlink.
    cache = BASE / 'build' / f'sdk-upmem-2023.2.0-Linux-x86_64-t{threads}'
    if not (cache / 'complete.json').exists():
        cache.mkdir(parents=True, exist_ok=True)
        jobs = []
        for group in ('misc', 'stdlib', 'syslib'):
            out = cache / group / 'CMakeFiles' / f'{group}.dir'
            out.mkdir(parents=True, exist_ok=True)
            for source in sorted((RUNTIME / 'sdk' / group).glob('*.c')):
                cmd = [compiler, '-w', '-O3', '-S', f'-DNR_TASKLETS={threads}']
                for include in ('misc', 'stdlib', 'syslib'):
                    cmd += ['-I', RUNTIME / 'sdk' / include]
                cmd += ['-c', source, '-o', out / f'{source.name}.o']
                jobs.append((cmd, cache / f'{group}-{source.stem}.log'))
        with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
            list(pool.map(lambda job: execute(*job, env=env), jobs))
        (cache / 'complete.json').write_text(json.dumps({'compiler': str(compiler), 'tasklets': threads}))
    link = RUNTIME / 'sdk/build'
    if link.is_symlink(): link.unlink()
    elif link.exists(): raise RuntimeError(f'Unexpected directory: {link}')
    link.symlink_to(cache, target_is_directory=True)


def run_name(a):
    return (f'slab{a.slab_kib}_t{a.threads}_{a.workload}_s{a.size}_b{a.burst}_r{a.rounds}'
            f'_h{a.heap_kib}_{a.population}_v{int(a.validate)}')


def implementation_hash():
    digest = hashlib.sha256()
    for name in ('allocator.c', 'buddy_backend.inc', 'benchmark.c', 'prepare.py'):
        digest.update(name.encode()); digest.update((HERE / name).read_bytes())
    return digest.hexdigest()


def percentile(values, p):
    return sorted(values)[max(0, math.ceil(len(values) * p) - 1)] if values else None


def parse_run(out, a, started):
    events, stats, snapshots = [], [], []
    backend = wram = None
    passed = False
    for line in (out / 'simulate.log').read_text().splitlines():
        if line.startswith('POLICY_EVENT,'):
            f = line.split(',')
            events.append(dict(zip(['kind', 'thread', 'index', 'begin_cycle', 'end_cycle', 'cycles'],
                                   [f[1]] + [int(x) for x in f[2:]])))
        elif line.startswith('POLICY_STATS,'):
            stats.append(dict(zip(STATS, map(int, line.split(',')[1:]), strict=True)))
        elif line.startswith('POLICY_SNAPSHOT,'):
            snapshots.append(dict(zip(['phase', 'round', 'requested_bytes', 'slot_bytes', 'reserved_bytes', 'empty_bytes'],
                                       map(int, line.split(',')[1:]), strict=True)))
        elif line.startswith('POLICY_BACKEND,'):
            backend = dict(zip(['used_bytes', 'peak_bytes', 'allocations', 'frees', 'failed_attempts', 'init_failures'],
                               map(int, line.split(',')[1:]), strict=True))
        elif line.startswith('POLICY_WRAM,'):
            wram = dict(zip(['heap_end_address', 'cache_descriptor_bytes', 'stats_bytes'],
                            map(int, line.split(',')[1:]), strict=True))
        elif line == 'POLICY_CHECK,PASS': passed = True
    if not passed or len(stats) != a.threads or backend is None or wram is None:
        raise RuntimeError(f'Missing terminal correctness/statistics records: {out}')
    assert backend['used_bytes'] == 0 and wram['heap_end_address'] <= 65536
    assert backend['allocations'] == backend['frees']
    phases = 4 if a.workload in ('phases', 'correctness') else 1
    assert len(snapshots) == 2 + (3 if a.workload == 'sparse' else 2) * phases * a.rounds
    per_round = sum(max(1, a.burst // d) for d in (8, 1, 4, 2)) if a.workload == 'phases' else a.burst * phases
    for s in stats:
        assert s['successes'] == s['free_calls'] and s['live_slots'] == 0
        if not s['failures']:
            assert s['alloc_calls'] == per_round * a.rounds
        for kind, key in [('alloc', 'alloc_calls'), ('free', 'free_calls')]:
            rows = [e for e in events if e['kind'] == kind and e['thread'] == s['thread']]
            assert len(rows) == s[key] and [e['index'] for e in rows] == list(range(s[key]))
    for filename, rows in [('events.csv', events), ('stats.csv', stats), ('snapshots.csv', snapshots)]:
        with (out / filename).open('w', newline='') as f:
            writer = csv.DictWriter(f, fieldnames=list(rows[0]))
            writer.writeheader(); writer.writerows(rows)
    metrics = {}
    for kind in ('alloc', 'free', 'init', 'work', 'drain', 'total'):
        values = [e['cycles'] / 350 for e in events if e['kind'] == kind]
        if not values and not (kind == 'free' and not sum(s['free_calls'] for s in stats)):
            raise RuntimeError(f'Missing {kind} probes')
        metrics[kind] = {'samples': len(values), 'mean_us': statistics.mean(values) if values else None,
                         'p99_us': percentile(values, .99), 'max_us': max(values) if values else None}
    work = [e for e in events if e['kind'] == 'work']
    makespan = (max(e['end_cycle'] for e in work) - min(e['begin_cycle'] for e in work)) / 350
    totals = {k: sum(s[k] for s in stats) for k in STATS if k != 'thread'}
    result = {'configuration': vars(a), 'correctness_passed': passed,
              'implementation_sha256': json.loads((out / 'command.json').read_text())['implementation_sha256'],
              'outcome': 'oom' if totals['failures'] or backend['init_failures'] else 'completed',
              'frequency_mhz': 350, 'metrics': metrics, 'work_makespan_us': makespan,
              'successful_allocations_per_second': totals['successes'] / (makespan / 1e6),
              'totals': totals, 'backend': backend, 'wram': wram,
              'peak_requested_at_barriers_bytes': max(s['requested_bytes'] for s in snapshots),
              'max_reserved_at_live_barriers_ratio': max(
                  (s['reserved_bytes'] / s['requested_bytes'] for s in snapshots if s['requested_bytes']), default=None),
              'host_runtime_seconds': time.time() - started,
              'experiment': 'Size-dependent slabs only; no adaptive allocation or empty-span retention',
              'capacity_profile': 'WRAM end 65536; 24 stacks x 1536 B; inherited IRAM/timing defaults',
              'source_sha256': hashlib.sha256((out / 'task.c').read_bytes()).hexdigest(),
              'simulator_sha256': hashlib.sha256((RUNTIME / 'build/uPIMulator').read_bytes()).hexdigest()}
    (out / 'summary.json').write_text(json.dumps(result, indent=2) + '\n')
    return result


def run(a):
    if not RUNTIME.exists():
        raise RuntimeError('Run python3 pim-reproduction/slabs/prepare.py first')
    out = HERE / 'results' / a.tag / run_name(a)
    if (out / 'summary.json').exists():
        previous = json.loads((out / 'summary.json').read_text())
        if previous.get('implementation_sha256') == implementation_hash():
            print(f'Complete: {out.name}', flush=True)
            return
        out.rename(out.with_name(out.name + f'.superseded-{time.time_ns()}'))
    if out.exists():
        out.rename(out.with_name(out.name + f'.incomplete-{time.time_ns()}'))
    out.mkdir(parents=True)
    started, env = time.time(), environment()
    compiler = BASE / 'toolchain/LoCaLUT/upmem-2023.2.0-Linux-x86_64/bin/dpu-upmem-dpurte-clang'
    with (BASE / '.prepare.lock').open('w') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        compile_sdk(compiler, a.threads, env)
        # Flatten includes so each result contains its exact complete C source.
        backend = (HERE / 'buddy_backend.inc').read_text()
        allocator = (HERE / 'allocator.c').read_text().replace('#include "buddy_backend.inc"', backend)
        source = (HERE / 'benchmark.c').read_text().replace('#include "allocator.c"', allocator)
        definitions = {'NR_TASKLETS': a.threads, 'SLAB_BYTES': a.slab_kib * 1024,
                       'EAGER_INIT': int(a.population == 'eager'), 'WORKLOAD': WORKLOADS[a.workload],
                       'BURST': a.burst, 'ROUNDS': a.rounds, 'SIZE_BYTES': a.size,
                       'HEAP_BYTES': a.heap_kib * 1024, 'VALIDATE': int(a.validate)}
        source = ''.join(f'#define {key} {value}\n' for key, value in definitions.items()) + source
        task = RUNTIME / 'benchmark/microbenchmark/dpu/task.c'
        task.write_text(source); (out / 'task.c').write_text(source)
        asm = RUNTIME / 'benchmark/build/microbenchmark/dpu/CMakeFiles/microbenchmark_device.dir/task.c.o'
        asm.parent.mkdir(parents=True, exist_ok=True)
        compile_command = [compiler, '-w', '-O3', '-S', '-c', task, '-o', asm]
        execute(compile_command, out / 'compile.log', env=env)
        shutil.copy2(asm, out / 'task.c.o')
        exe = RUNTIME / 'build/uPIMulator'; exe.parent.mkdir(exist_ok=True)
        fingerprint = hashlib.sha256()
        for go_file in sorted((RUNTIME / 'src').rglob('*.go')):
            fingerprint.update(str(go_file.relative_to(RUNTIME)).encode())
            fingerprint.update(go_file.read_bytes())
        stamp = exe.parent / 'source-hash.txt'
        if not exe.exists() or not stamp.exists() or stamp.read_text() != fingerprint.hexdigest():
            execute(['go', 'build', '-C', RUNTIME / 'src', '-buildvcs=false', '-o', exe], out / 'go-build.log', env=env)
            stamp.write_text(fingerprint.hexdigest())
        command = [exe, '--root_dirpath', RUNTIME, '--bin_dirpath', out, '--benchmark', 'microbenchmark',
                   '--num_channels', '1', '--num_ranks_per_channel', '1', '--num_dpus_per_rank', '1',
                   '--num_vm_ranks_per_channel', '1', '--num_vm_banks_per_rank', '1',
                   '--num_tasklets', a.threads, '--verbose', '0']
        (out / 'command.json').write_text(json.dumps({'compiler': list(map(str, compile_command)),
                                                    'simulator': list(map(str, command)),
                                                    'implementation_sha256': implementation_hash(),
                                                    'configuration': vars(a)}, indent=2))
        execute(command + ['--mode', 'link'], out / 'link.log', env=env)
    print(f'Running {out.name}', flush=True)
    execute(command + ['--mode', 'simulate'], out / 'simulate.log', env=env)
    result = parse_run(out, a, started)
    print(f"{result['outcome']}: {out.name}; alloc mean {result['metrics']['alloc']['mean_us']:.2f} us", flush=True)


def add_options(parser):
    parser.add_argument('--slab-kib', type=int, choices=[0, 16, 32, 64], default=16,
                        help='0 keeps 2049..4096 B requests on the original direct buddy path')
    parser.add_argument('--population', choices=['eager', 'lazy'], default='eager')
    parser.add_argument('--threads', type=int, choices=range(1, 17), default=1)
    parser.add_argument('--workload', choices=WORKLOADS, default='burst')
    parser.add_argument('--size', type=int, choices=range(16, 32769), default=4096)
    parser.add_argument('--burst', type=int, choices=range(1, 257), default=32)
    parser.add_argument('--rounds', type=int, choices=range(1, 33), default=4)
    parser.add_argument('--heap-kib', type=int, default=32768)
    parser.add_argument('--validate', action='store_true')
    parser.add_argument('--tag', default='slabs-quick-v1')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='action', required=True)
    single = sub.add_parser('run'); add_options(single)
    sweep = sub.add_parser('sweep'); add_options(sweep)
    sweep.add_argument('--preset', choices=['quick', 'full'], default='quick')
    sweep.add_argument('--jobs', type=int, choices=range(1, 5), default=2)
    a = parser.parse_args()
    if not 4 <= a.heap_kib <= 32768 or a.heap_kib & (a.heap_kib - 1):
        parser.error('heap-kib must be a power of two in 4..32768')
    if a.size % 8: parser.error('size must be a multiple of 8 for the payload DMA checks')
    if not a.tag or any(x in a.tag for x in ('/', '\\', '..')):
        parser.error('tag must be a simple directory name')
    if a.action == 'run': run(a); return
    configs = []
    threads = [1, 4] if a.preset == 'quick' else [1, 2, 4, 8, 16]
    workloads = ['burst'] if a.preset == 'quick' else ['burst', 'phases', 'sparse']
    for t in threads:
        for workload in workloads:
            for slab_kib in (0, 16, 32, 64):
                config = argparse.Namespace(**vars(a))
                config.slab_kib, config.threads, config.workload = slab_kib, t, workload
                if a.preset == 'quick': config.burst, config.rounds = 32, 2
                del config.preset, config.jobs
                configs.append(config)
    with concurrent.futures.ThreadPoolExecutor(max_workers=a.jobs) as pool:
        list(pool.map(run, configs))
    print(f'Finished {len(configs)} configurations; analyze tag {a.tag}', flush=True)


if __name__ == '__main__': main()
