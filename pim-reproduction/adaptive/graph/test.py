#!/usr/bin/env python3
"""Validate the graph kernel against exact ordered host adjacency lists."""
import json
import os
from pathlib import Path
import subprocess

HERE = Path(__file__).resolve().parent


def main():
    out = HERE / 'validation'; out.mkdir(exist_ok=True)
    env = os.environ | {'ASAN_OPTIONS': 'detect_leaks=0'}
    records = []
    for block in (64, 256):
        for policy, number, keep in [('eager', 0, 1), ('lazy', 1, 1), ('fixed-1', 2, 1),
                                      ('fixed-2', 2, 2), ('adaptive', 3, 1)]:
            name = f'{policy}-{block}B'; exe = out / name
            command = ['cc', '-std=gnu11', '-O1', '-g', '-fsanitize=undefined,address',
                       '-fno-sanitize-recover=all', '-fno-omit-frame-pointer',
                       '-Wno-pointer-to-int-cast', '-Wno-int-to-pointer-cast', '-Wno-int-conversion',
                       '-I' + str(HERE.parent / 'tests/shim'), '-I' + str(HERE.parent),
                       '-DNR_TASKLETS=4', f'-DPOLICY={number}', f'-DKEEP_PAGES={keep}',
                       f'-DGRAPH_BLOCK_BYTES={block}', str(HERE / 'host_test.c'), '-o', str(exe)]
            with (out / f'{name}.log').open('w') as log:
                subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=True)
                subprocess.run([str(exe)], stdout=log, stderr=subprocess.STDOUT, check=True, env=env)
            records.append({'policy': policy, 'block_bytes': block, 'passed': True})
            print(f'Graph sanitizer/reference test passed: {name}', flush=True)
    (out / 'checks.json').write_text(json.dumps(records, indent=2) + '\n')


if __name__ == '__main__': main()
