#!/usr/bin/env python3
"""Native property tests with sanitizers, optionally actual DPU simulator checks."""
from pathlib import Path
import argparse
import concurrent.futures
import json
import os
import subprocess
import sys
import time
import prepare

HERE = Path(__file__).resolve().parent


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--simulator', action='store_true')
    parser.add_argument('--jobs', type=int, choices=range(1, 5), default=2)
    a = parser.parse_args()
    prepare.prepare_backend()
    out = HERE / 'validation'; out.mkdir(exist_ok=True)
    env = os.environ | {'ASAN_OPTIONS': 'detect_leaks=0'}  # LSan unavailable under this host's ptrace.
    results = []
    for policy, number, keep in [('eager', 0, 1), ('lazy', 1, 1), ('fixed', 2, 1), ('fixed-2', 2, 2), ('adaptive', 3, 1)]:
        exe = out / f'host-{policy}'
        command = ['cc', '-std=gnu11', '-O1', '-g', '-fsanitize=undefined,address',
                   '-fno-sanitize-recover=all', '-fno-omit-frame-pointer',
                   '-Wno-pointer-to-int-cast', '-Wno-int-to-pointer-cast', '-Wno-int-conversion',
                   '-I'+str(HERE / 'tests/shim'), '-DNR_TASKLETS=4', f'-DPOLICY={number}', f'-DKEEP_PAGES={keep}',
                   str(HERE / 'tests/host_test.c'), '-o', str(exe)]
        with (out / f'host-{policy}.log').open('w') as log:
            subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=True)
            subprocess.run([str(exe)], stdout=log, stderr=subprocess.STDOUT, check=True, env=env)
        results.append({'policy': policy, 'native_passed': True})
        print(f'Native sanitizer/property tests passed: {policy}', flush=True)
    if a.simulator:
        prepare.prepare_runtime()
        cases = [(p, t) for p in ('eager', 'lazy', 'fixed', 'adaptive') for t in (1, 4)]
        def run_case(case):
            p, t = case
            command = [sys.executable, str(HERE / 'experiment.py'), 'run', '--policy', p,
                       '--threads', str(t), '--workload', 'correctness', '--burst', '4',
                       '--rounds', '1', '--heap-kib', '512', '--validate', '--tag', 'validation-v1']
            subprocess.run(command, check=True)
        with concurrent.futures.ThreadPoolExecutor(max_workers=a.jobs) as pool:
            list(pool.map(run_case, cases))
    simulator_records = []
    for path in sorted((HERE / 'results/validation-v1').glob('*/summary.json')):
        if '.incomplete-' in path.parent.name or '.superseded-' in path.parent.name:
            continue
        record = json.loads(path.read_text())
        simulator_records.append({'configuration': record['configuration'],
                                  'passed': record['correctness_passed'], 'outcome': record['outcome'],
                                  'summary': str(path)})
    (out / 'checks.json').write_text(json.dumps({'native': results, 'simulator_requested': a.simulator,
                                               'preserved_simulator_checks': simulator_records,
                                               'completed_at_unix': time.time()}, indent=2)+'\n')


if __name__ == '__main__': main()
