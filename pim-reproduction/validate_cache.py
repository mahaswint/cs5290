#!/usr/bin/env python3
"""Replay saved kernels and verify host optimizations preserve simulated timestamps."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import time

BASE = Path(__file__).resolve().parent
for threads in (1, 16):
    previous = BASE / f'results/fig15/sw_t{threads}_s32_n128'
    target = BASE / f'validation/host-caches-sw{threads}-32'
    if (target / 'validation.json').exists():
        continue
    target.mkdir(parents=True, exist_ok=False)
    for name in ('addresses.txt', 'values.txt', 'atomic.bin', 'iram.bin', 'wram.bin', 'mram.bin'):
        shutil.copy2(previous / name, target / name)
    command = json.loads((previous / 'command.json').read_text())
    command[command.index('--bin_dirpath') + 1] = str(target)
    started = time.time()
    with (target / 'simulate.log').open('w') as output:
        subprocess.run(command + ['--mode', 'simulate'], stdout=output, stderr=subprocess.STDOUT,
                       env=os.environ | {'REPRO_IRAM_CACHE': '1'}, check=True)
    expected = [x for x in (previous / 'simulate.log').read_text().splitlines() if x.startswith('REPRO_ALLOC,')]
    actual = [x for x in (target / 'simulate.log').read_text().splitlines() if x.startswith('REPRO_ALLOC,')]
    if expected != actual:
        raise RuntimeError('Host caches changed simulated allocation timestamps')
    prior_stats = dict(line.rsplit(': ', 1) for line in (previous / 'log.txt').read_text().splitlines() if ': ' in line)
    stats = dict(line.rsplit(': ', 1) for line in (target / 'log.txt').read_text().splitlines() if ': ' in line)
    if stats != prior_stats:
        raise RuntimeError('Host caches changed native simulator statistics')
    result = {'identical_allocation_timestamps': True, 'identical_native_statistics': True,
              'samples': len(actual), 'host_runtime_seconds': time.time() - started}
    (target / 'validation.json').write_text(json.dumps(result, indent=2) + '\n')
    print(f'{threads} threads: {result}', flush=True)
