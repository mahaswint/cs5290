#!/usr/bin/env python3
"""Audit and compare isolated slab runs; never rank OOM by shorter runtime."""
import argparse
import csv
import hashlib
import json
import os
from pathlib import Path
import sys

HERE = Path(__file__).resolve().parent


def label(slab):
    return 'baseline' if slab == 0 else f'{slab} KiB slab'


def group_key(c):
    return tuple(c[k] for k in ('threads', 'workload', 'size', 'burst', 'rounds', 'heap_kib', 'population', 'validate'))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--tag', default='slabs-quick-v1')
    parser.add_argument('--no-plots', action='store_true')
    a = parser.parse_args()
    if not a.tag or any(x in a.tag for x in ('/', '\\', '..')):
        parser.error('tag must be a simple directory name')
    files = sorted(p for p in (HERE / 'results' / a.tag).glob('*/summary.json')
                   if '.incomplete-' not in p.parent.name and '.superseded-' not in p.parent.name)
    if not files: raise SystemExit('No completed runs found')
    records, rows = [], []
    for path in files:
        d = json.loads(path.read_text()); c = d['configuration']
        if not d['correctness_passed']: raise RuntimeError(f'Correctness failed: {path}')
        if hashlib.sha256((path.parent / 'task.c').read_bytes()).hexdigest() != d['source_sha256']:
            raise RuntimeError(f'Source hash mismatch: {path}')
        command = json.loads((path.parent / 'command.json').read_text())
        if command['implementation_sha256'] != d['implementation_sha256']:
            raise RuntimeError(f'Implementation hash mismatch: {path}')
        snapshots = list(csv.DictReader((path.parent / 'snapshots.csv').open()))
        tail = [int(s['reserved_bytes']) for s in snapshots if int(s['phase']) == 2] if c['workload'] == 'sparse' else []
        initial_reserved = int(snapshots[0]['reserved_bytes'])
        row = {k: c[k] for k in ('slab_kib', 'population', 'threads', 'workload', 'size', 'burst', 'rounds', 'heap_kib', 'validate')}
        row.update(label=label(c['slab_kib']), outcome=d['outcome'],
                   alloc_mean_us=d['metrics']['alloc']['mean_us'], alloc_p99_us=d['metrics']['alloc']['p99_us'],
                   free_mean_us=d['metrics']['free']['mean_us'], total_us=d['metrics']['total']['mean_us'],
                   work_us=d['work_makespan_us'], peak_reserved_bytes=d['backend']['peak_bytes'],
                   peak_requested_bytes=d['peak_requested_at_barriers_bytes'], initial_reserved_bytes=initial_reserved,
                   sparse_tail_reserved_bytes=max(tail) if tail else None,
                   successes=d['totals']['successes'], failed_requests=d['totals']['failures'],
                   failed_init_pages=d['backend']['init_failures'], backend_allocations=d['backend']['allocations'],
                   work_backend_allocations=d['backend']['allocations'] - initial_reserved // 4096,
                   slab_refills=d['totals']['slab_refills'], slab_hits=d['totals']['slab_hits'],
                   cache_hits=d['totals']['cache_hits'], direct_allocations=d['totals']['direct_allocations'],
                   heap_end_address=d['wram']['heap_end_address'], implementation_sha256=d['implementation_sha256'],
                   run=str(path.parent))
        row['cache_hit_percent'] = 100 * row['cache_hits'] / row['successes'] if row['successes'] else None
        records.append((d, row)); rows.append(row)
    if len({r['implementation_sha256'] for r in rows}) != 1:
        raise SystemExit('Mixed implementation versions: use a separate tag or rerun stale cases')
    baselines = {group_key(d['configuration']): row for d, row in records if row['slab_kib'] == 0}
    for d, row in records:
        base = baselines.get(group_key(d['configuration']))
        valid = base and base['outcome'] == row['outcome'] == 'completed' and base['successes'] == row['successes']
        row['alloc_speedup_vs_baseline'] = base['alloc_mean_us'] / row['alloc_mean_us'] if valid else None
        row['total_speedup_vs_baseline'] = base['total_us'] / row['total_us'] if valid else None
        row['work_backend_reduction_percent'] = (100 * (1-row['work_backend_allocations']/base['work_backend_allocations'])
                                                if valid and base['work_backend_allocations'] else None)
    out = HERE / 'analysis' / a.tag; out.mkdir(parents=True, exist_ok=True)
    with (out / 'comparison.csv').open('w', newline='') as stream:
        writer = csv.DictWriter(stream, fieldnames=list(rows[0])); writer.writeheader(); writer.writerows(rows)
    text = ['# Size-dependent slab results', '',
            'This experiment implements larger slabs only. It has no adaptive allocation, demand controller, or empty-slab retention.',
            'Baseline sends 2049..4096 B requests directly to the common buddy allocator. Variants split 16/32/64 KiB buddy blocks into 4 KiB slots behind a 24 B header (3/7/15 slots).',
            'The original eight size classes, buddy backend, metadata buffer, initialization mode, workload, and simulator timing are shared.',
            'Latencies are individual request issue cycles / 350 MHz. Total includes initialization, workload, and final drain. Payload checks and barriers are common.',
            'These are controlled microbenchmarks with a maintained baseline; they are not an exact Figure 15 or graph-workload reproduction.',
            'OOM runs have different completed work: speedup ratios are blank unless both cases complete the same requested trace.', '',
            '| Workload | Threads | Heap KiB | Variant | Outcome | Alloc mean / p99 us | Total ms | Work backend allocations | Cache hits % | Peak reserved KiB | Failures (request/init) |',
            '| --- | ---: | ---: | --- | --- | ---: | ---: | ---: | ---: | ---: | ---: |']
    for r in sorted(rows, key=lambda r: (r['workload'], r['threads'], r['heap_kib'], r['slab_kib'])):
        hit = f"{r['cache_hit_percent']:.1f}" if r['cache_hit_percent'] is not None else 'N/A'
        text.append(f"| {r['workload']} | {r['threads']} | {r['heap_kib']} | {r['label']} | {r['outcome']} | "
                    f"{r['alloc_mean_us']:.2f} / {r['alloc_p99_us']:.2f} | {r['total_us']/1000:.3f} | "
                    f"{r['work_backend_allocations']} | {hit} | {r['peak_reserved_bytes']/1024:.1f} | "
                    f"{r['failed_requests']}/{r['failed_init_pages']} |")
    text += ['', '## Interpretation limits', '',
             '- A cache hit means an allocation served from an existing frontend page/slab, without a backend refill. It is not a hardware data-cache hit.',
             '- Work backend allocations exclude the successful initial eager population; the raw counter includes it.',
             '- Reserved bytes include live slabs, their headers, free slots, and untouched eager pages. Backend metadata, benchmark storage, and WRAM are separate.',
             '- Sparse tails are sampled after freeing all but object indices 0,16,32,...; every variant receives the same allocation/free trace.',
             '- Peak memory and phase samples do not prove behavior for all lifetime patterns. Larger contiguous refills may fail where a 4 KiB direct allocation succeeds; no direct-allocation fallback is hidden.',
             '- Same-tasklet frees only. WRAM is capped at 64 KiB; inherited IRAM and stack bounds still need validation for a physical hardware image.',
             '- Exact parameters, counters, successful allocation counts, source hashes, and paths are in comparison.csv and per-run summaries.', '']
    if not a.no_plots:
        cache = HERE / '.cache/matplotlib'; cache.mkdir(parents=True, exist_ok=True)
        os.environ.setdefault('MPLCONFIGDIR', str(cache))
        try:
            import matplotlib
        except ModuleNotFoundError:
            python = HERE.parent / '.venv/bin/python'
            if python.exists() and Path(sys.prefix) != python.parent.parent:
                os.execv(str(python), [str(python), str(Path(__file__).resolve()), *sys.argv[1:]])
            raise SystemExit('Use the existing reproduction venv, or --no-plots')
        matplotlib.use('Agg')
        import matplotlib.pyplot as plt
        groups = {}
        for r in rows:
            key = tuple(r[k] for k in ('workload', 'size', 'burst', 'rounds', 'heap_kib', 'population', 'validate'))
            groups.setdefault(key, []).append(r)
        colors = {0: '#666666', 16: '#0072B2', 32: '#009E73', 64: '#D55E00'}
        for number, (key, group) in enumerate(sorted(groups.items())):
            fig, axes = plt.subplots(2, 3, figsize=(12, 6.8), constrained_layout=True)
            last = ('sparse_tail_reserved_bytes', 'Reserved at sparse tail (KiB)', 1/1024) if key[0] == 'sparse' else ('cache_hit_percent', 'Frontend allocation hits (%)', 1)
            panels = [('alloc_mean_us', 'Mean allocation latency (us)', 1),
                      ('alloc_p99_us', 'Allocation p99 (us)', 1), ('total_us', 'Init + work + drain (ms)', .001),
                      ('work_backend_allocations', 'Work backend allocations', 1),
                      ('peak_reserved_bytes', 'Peak reserved MRAM (KiB)', 1/1024), last]
            for slab in (0, 16, 32, 64):
                selected = sorted([r for r in group if r['slab_kib'] == slab], key=lambda r:r['threads'])
                if not selected: continue
                for ax, (metric, title, scale) in zip(axes.flat, panels):
                    valid = [r for r in selected if r[metric] is not None and
                             (r['outcome'] == 'completed' or metric in ('peak_reserved_bytes', 'sparse_tail_reserved_bytes'))]
                    ax.plot([r['threads'] for r in valid], [r[metric]*scale for r in valid], 'o-',
                            color=colors[slab], label=label(slab), linewidth=1.6, markersize=4)
                    ax.set_title(title, fontsize=10); ax.set_xlabel('Tasklets'); ax.grid(alpha=.22)
            ticks = sorted({r['threads'] for r in group})
            for ax in axes.flat:
                ax.set_ylim(bottom=0)
                ax.set_xticks(ticks)
                if len(ticks) == 1: ax.set_xlim(ticks[0]-.5, ticks[0]+.5)
            axes[0,0].legend(fontsize=8)
            fig.suptitle(f'{key[0]}: {key[1]} B requests, burst {key[2]}, rounds {key[3]}, heap {key[4]} KiB, {key[5]} init', fontsize=12)
            stem = out / f'{key[0]}-{number:02d}'
            fig.savefig(stem.with_suffix('.png'), dpi=220); fig.savefig(stem.with_suffix('.svg')); plt.close(fig)
            text.append(f'Plot: {stem.name}.png / .svg')
    (out / 'RESULTS.md').write_text('\n'.join(text)+'\n')
    print(f'Analyzed {len(rows)} configurations: {out / "RESULTS.md"}')
    print(f'CSV: {out / "comparison.csv"}')


if __name__ == '__main__': main()
