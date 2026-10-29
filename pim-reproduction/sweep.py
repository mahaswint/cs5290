#!/usr/bin/env python3
"""Resume the software-only Figure 15 sweep, retaining every raw run."""
from pathlib import Path
import argparse
import concurrent.futures
import json
import subprocess
import sys
import time

base = Path(__file__).resolve().parent
configs = [(allocator, threads, size) for threads in (1, 16)
           for allocator in ('sw', 'buddy') for size in (32, 256, 4096)]
parser = argparse.ArgumentParser()
parser.add_argument('--jobs', type=int, choices=range(1, 5), default=3)
args = parser.parse_args()


def run_configuration(config):
    allocator, threads, size = config
    run = base / 'results/fig15' / f'{allocator}_t{threads}_s{size}_n128'
    if (run / 'summary.json').exists():
        print(f'Complete: {allocator}, {threads} threads, {size} B', flush=True)
        return
    if run.exists():
        retained = run.with_name(run.name + f'.incomplete-{int(time.time())}')
        run.rename(retained)
        print(f'Retained incomplete attempt at {retained}', flush=True)
    print(f'Running: {allocator}, {threads} threads, {size} B', flush=True)
    subprocess.run([sys.executable, str(base / 'run.py'), '--allocator', allocator,
                    '--threads', str(threads), '--size', str(size)], check=True)


with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
    list(pool.map(run_configuration, configs))
print('All 12 configurations completed.', flush=True)
