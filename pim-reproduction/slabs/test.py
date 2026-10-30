#!/usr/bin/env python3
"""Sanitized native tests and optional actual DPU simulator checks for slabs."""
from pathlib import Path
import argparse
import concurrent.futures
import json
import os
import subprocess
import sys
import time
import prepare
import experiment

HERE = Path(__file__).resolve().parent


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--simulator', action='store_true')
    parser.add_argument('--jobs', type=int, choices=range(1, 5), default=2)
    a = parser.parse_args()
    prepare.prepare_backend()
    out = HERE / 'validation'; out.mkdir(exist_ok=True)
    env = os.environ | {'ASAN_OPTIONS': 'detect_leaks=0'}
    results = []
    for slab in (0, 16, 32, 64):
        for eager in (0, 1):
            exe = out / f'host-slab{slab}-eager{eager}'
            command = ['cc', '-std=gnu11', '-O1', '-g', '-fsanitize=undefined,address',
                       '-fno-sanitize-recover=all', '-fno-omit-frame-pointer',
                       '-Wno-pointer-to-int-cast', '-Wno-int-to-pointer-cast', '-Wno-int-conversion',
                       '-I'+str(HERE / 'tests/shim'), '-DNR_TASKLETS=4',
                       f'-DSLAB_BYTES={slab*1024}', f'-DEAGER_INIT={eager}',
                       str(HERE / 'tests/host_test.c'), '-o', str(exe)]
            log = out / f'{exe.name}.log'
            with log.open('w') as stream:
                subprocess.run(command, stdout=stream, stderr=subprocess.STDOUT, check=True)
                subprocess.run([str(exe)], stdout=stream, stderr=subprocess.STDOUT, check=True, env=env)
            results.append({'slab_kib': slab, 'population': 'eager' if eager else 'lazy', 'passed': True, 'log': str(log)})
            print(f'Native sanitizer/property tests passed: slab={slab}, eager={eager}', flush=True)
    if a.simulator:
        prepare.prepare_runtime()
        cases = [(slab, t, 'correctness', 8) for slab in (0, 16, 32, 64) for t in (1, 4)]
        cases += [(slab, 4, workload, 16) for slab in (0, 64) for workload in ('phases', 'sparse')]
        cases.append((64, 16, 'correctness', 8))
        def run_case(case):
            slab, t, workload, burst = case
            tag = 'slabs-validation-v1' if workload == 'correctness' else 'slabs-extra-validation-v1'
            subprocess.run([sys.executable, str(HERE / 'experiment.py'), 'run', '--slab-kib', str(slab),
                            '--threads', str(t), '--workload', workload, '--burst', str(burst), '--rounds', '1',
                            '--heap-kib', '4096', '--validate', '--tag', tag], check=True)
        with concurrent.futures.ThreadPoolExecutor(max_workers=a.jobs) as pool:
            list(pool.map(run_case, cases))
    records = []
    paths = list((HERE / 'results/slabs-validation-v1').glob('*/summary.json'))
    paths += list((HERE / 'results/slabs-extra-validation-v1').glob('*/summary.json'))
    for path in sorted(paths):
        if '.incomplete-' in path.parent.name or '.superseded-' in path.parent.name: continue
        d = json.loads(path.read_text())
        records.append({'configuration': d['configuration'], 'passed': d['correctness_passed'],
                        'outcome': d['outcome'], 'implementation_sha256': d['implementation_sha256'],
                        'summary': str(path)})
    (out / 'checks.json').write_text(json.dumps({'implementation_sha256': experiment.implementation_hash(),
                                               'native': results, 'simulator': records,
                                               'completed_at_unix': time.time()}, indent=2)+'\n')


if __name__ == '__main__': main()
