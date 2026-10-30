#!/usr/bin/env python3
"""Declared workload families, controls, and independent mechanism ablations."""
import argparse
import concurrent.futures
import json
from pathlib import Path
import subprocess
import sys
import run


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--preset', choices=['validation', 'pilot', 'main', 'heldout', 'ablation', 'scaling', 'large', 'resident'], default='pilot')
    p.add_argument('--tag', default='pilot-v1')
    p.add_argument('--jobs', type=int, choices=range(1, 5), default=2)
    a = p.parse_args()
    if not a.tag or any(x in a.tag for x in ('/', '\\', '..')): p.error('invalid tag')
    configs = []
    if a.preset == 'validation':
        for mode in run.MODES:
            for threads in (1, 4):
                for workload in ('scatter', 'pressure', 'backend'):
                    configs.append(dict(mode=mode, threads=threads, workload=workload,
                                        burst=8, rounds=1, heap_kib=512, validate=True))
    elif a.preset in ('large', 'resident'):
        for threads in (1, 4):
            for mode in ('scan', 'indexed'):
                for policy in ('fixed', 'adaptive'):
                    configs.append(dict(mode=mode, threads=threads, policy=policy,
                                        workload='scatter' if a.preset == 'large' else 'resident',
                                        burst=512, rounds=2 if a.preset == 'large' else 3))
        if a.preset == 'resident':
            for mode in ('lists', 'lookup'):
                configs.append(dict(mode=mode, threads=1, policy='fixed', workload='resident', burst=512, rounds=3))
    elif a.preset == 'ablation':
        for threads in (1, 4):
            for mode in run.MODES:
                for policy in ('fixed', 'adaptive'):
                    configs.append(dict(mode=mode, threads=threads, policy=policy, workload='scatter', burst=128, rounds=3))
    elif a.preset == 'scaling':
        for threads in (1, 4, 8, 16):
            for burst in (16, 64, 256, 512):
                for mode in ('scan', 'indexed'):
                    configs.append(dict(mode=mode, policy='fixed', threads=threads, workload='scatter', burst=burst, rounds=2))
    else:
        seeds = [5290] if a.preset != 'heldout' else [9173, 24107]
        families = [('scatter', 128, 3, 32768), ('handoff', 32, 6, 32768),
                    ('control', 8, 3, 32768), ('backend', 8, 2, 32768), ('pressure', 16, 3, 64)]
        if a.preset == 'pilot': families = families[:2]
        if a.preset == 'heldout':
            families = [('resident', 256, 2, 32768), ('handoff', 32, 4, 32768)]
        for seed in seeds:
            for threads in (1, 4):
                for workload, burst, rounds, heap in families:
                    for mode in ('scan', 'indexed'):
                        variants = [('fixed', 2), ('adaptive', 2)]
                        if a.preset == 'main': variants += [('lazy', 0), ('fixed', 1)]
                        for policy, keep in variants:
                            configs.append(dict(mode=mode, policy=policy, keep=keep, threads=threads,
                                                workload=workload, burst=burst, rounds=rounds,
                                                heap_kib=heap * threads if workload == 'pressure' else heap,
                                                seed=seed))
                if a.preset in ('main', 'heldout'):
                    for mode in ('scan', 'indexed'):
                        for policy in ('fixed', 'adaptive'):
                            configs.append(dict(mode=mode, policy=policy, threads=threads, workload='graph',
                                                rounds=2, graph_edges=384, seed=seed))
    # Save the complete offered matrix before running any configuration.
    folder = run.HERE / 'results' / a.tag; folder.mkdir(parents=True, exist_ok=True)
    (folder / f'matrix-{a.preset}.json').write_text(json.dumps(configs, indent=2) + '\n')
    def launch(c):
        command = [sys.executable, str(run.HERE / 'run.py'), '--tag', a.tag]
        for k, v in c.items():
            command.append('--' + k.replace('_', '-'))
            if not isinstance(v, bool): command.append(str(v))
        subprocess.run(command, check=True)
    with concurrent.futures.ThreadPoolExecutor(max_workers=a.jobs) as pool:
        list(pool.map(launch, configs))
    print(f'Completed {len(configs)} declared configurations: {a.tag}', flush=True)


if __name__ == '__main__': main()
