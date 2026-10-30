#!/usr/bin/env python3
"""Run the same sampled dynamic graph and varying batches across all policies."""
import argparse
import concurrent.futures
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
import experiment


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    experiment.add_options(parser)
    parser.set_defaults(workload='graph', rounds=2, burst=1, tag='gowalla-v1')
    parser.add_argument('--jobs', type=int, choices=range(1, 5), default=2)
    parser.add_argument('--thread-counts', type=int, nargs='+', default=[1, 4])
    parser.add_argument('--block-sizes', type=int, nargs='+', choices=[64, 128, 256], default=[64, 256])
    a = parser.parse_args()
    if a.workload != 'graph': parser.error('This runner supports the graph workload only')
    if any(not 1 <= n <= 16 for n in a.thread_counts): parser.error('tasklets must be 1..16')
    if not 4 <= a.heap_kib <= 32768 or a.heap_kib & (a.heap_kib - 1):
        parser.error('heap-kib must be a power of two in 4..32768')
    if not 1 <= a.rounds <= 64 or a.decay_ops < 1 or a.decay_ops & (a.decay_ops - 1):
        parser.error('rounds must be 1..64 and decay-ops a power of two')
    if not a.tag or any(x in a.tag for x in ('/', '\\', '..')): parser.error('invalid tag')
    if not 48 <= a.graph_edges <= 3072 or a.graph_edges % 3: parser.error('invalid graph-edges')
    configs = []
    for threads in a.thread_counts:
        for block in a.block_sizes:
            for policy, keep in [('eager', 1), ('lazy', 1), ('fixed', 1), ('fixed', 2), ('adaptive', 1)]:
                c = argparse.Namespace(**vars(a))
                c.threads, c.graph_block_bytes, c.policy, c.keep = threads, block, policy, keep
                del c.jobs, c.thread_counts, c.block_sizes
                configs.append(c)
    with concurrent.futures.ThreadPoolExecutor(max_workers=a.jobs) as pool:
        list(pool.map(experiment.run, configs))
    print(f'Finished {len(configs)} graph configurations; tag {a.tag}', flush=True)


if __name__ == '__main__': main()
