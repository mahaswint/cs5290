#!/usr/bin/env python3
"""Summarize successful contribution runs; keep OOM outcomes visible."""
import argparse
import csv
import hashlib
import json
import os
from pathlib import Path
import sys

HERE = Path(__file__).resolve().parent


def label(c):
    return f"fixed-{c['keep']}" if c['policy'] == 'fixed' else c['policy']


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--tag', default='comparison-v1')
    parser.add_argument('--no-plots', action='store_true')
    a = parser.parse_args()
    folder = HERE / 'results' / a.tag
    files = sorted(p for p in folder.glob('*/summary.json') if
                   '.incomplete-' not in p.parent.name and '.superseded-' not in p.parent.name)
    if not files: raise SystemExit(f'No completed runs in {folder}')
    rows, records = [], []
    for path in files:
        s = json.loads(path.read_text()); c = s['configuration']; records.append(s)
        if not s['correctness_passed']: raise RuntimeError(f'Failed correctness gate: {path}')
        if hashlib.sha256((path.parent / 'task.c').read_bytes()).hexdigest() != s['source_sha256']:
            raise RuntimeError(f'Source hash mismatch: {path}')
        row = {k: c[k] for k in ('policy', 'threads', 'workload', 'burst', 'rounds', 'heap_kib',
                                  'keep', 'max_keep', 'budget_pages', 'decay_ops', 'pressure_percent', 'validate')}
        row.update({'label': label(c), 'outcome': s['outcome'],
                    'alloc_mean_us': s['metrics']['alloc']['mean_us'], 'alloc_p99_us': s['metrics']['alloc']['p99_us'],
                    'free_mean_us': s['metrics']['free']['mean_us'], 'free_p99_us': s['metrics']['free']['p99_us'],
                    'init_us': s['metrics']['init']['mean_us'], 'work_us': s['work_makespan_us'],
                    'total_us': s['metrics']['total']['mean_us'], 'drain_mean_us': s['metrics']['drain']['mean_us'],
                    'peak_reserved_bytes': s['backend']['peak_bytes'],
                    'peak_requested_at_barriers_bytes': s['peak_requested_at_barriers_bytes'],
                    'max_reserved_live_barrier_ratio': s['max_reserved_at_live_barriers_ratio'],
                    'backend_allocations': s['backend']['allocations'], 'backend_frees': s['backend']['frees'],
                    'failures': s['totals']['failures'], 'grows': s['totals']['grows'],
                    'decays': s['totals']['decays'], 'idle_trims': s['totals']['idle_trims'],
                    'pressure_trims': s['totals']['pressure_trims'],
                    'heap_end_address': s['wram']['heap_end_address'],
                    'implementation_sha256': s['implementation_sha256'], 'run': str(path.parent)})
        rows.append(row)
    if len({r['implementation_sha256'] for r in rows}) > 1:
        raise SystemExit('Mixed implementation versions in this tag; rerun the sweep to refresh stale runs.')
    out = HERE / 'analysis' / a.tag; out.mkdir(parents=True, exist_ok=True)
    with (out / 'comparison.csv').open('w', newline='') as stream:
        writer = csv.DictWriter(stream, fieldnames=list(rows[0])); writer.writeheader(); writer.writerows(rows)
    text = ['# Empty-page retention experiment results', '',
            'Simulated request latencies at 350 MHz. These are maintained-baseline experiments, separate from Figure 15.',
            'Work time includes identical benchmark payload checks, memory snapshots, and barriers. Total time includes initialization and final empty-page drain.',
            'OOM configurations have different completed work and must not be ranked by runtime against successful configurations.', '',
            '| Workload | Threads | Policy | Outcome | Alloc mean / p99 (us) | Free mean (us) | Total (ms) | Peak reserved (KiB) | Failed requests/init pages |',
            '| --- | ---: | --- | --- | ---: | ---: | ---: | ---: | ---: |']
    for r in sorted(rows, key=lambda x: (x['workload'], x['threads'], x['label'])):
        free_mean = f"{r['free_mean_us']:.2f}" if r['free_mean_us'] is not None else 'N/A'
        text.append(f"| {r['workload']} | {r['threads']} | {r['label']} | {r['outcome']} | "
                    f"{r['alloc_mean_us']:.2f} / {r['alloc_p99_us']:.2f} | {free_mean} | "
                    f"{r['total_us']/1000:.2f} | {r['peak_reserved_bytes']/1024:.1f} | {r['failures']} |")
    text += ['', 'Detailed controller counters, memory ratios at common barriers, all configuration parameters, and exact run paths are in comparison.csv.',
             'Peak reserved memory is measured under the backend lock. Peak requested bytes and reserved/live ratios are sampled at quiescent allocation/free barriers; zero-live ratios are omitted.',
             'The WRAM address limit is 65,536 with 24 x 1,536 B stack reservations. This is a simulation capacity check, not a complete real-hardware or IRAM deployment validation.', '']
    if not a.no_plots:
        cache = HERE / '.cache/matplotlib'
        cache.mkdir(parents=True, exist_ok=True)
        os.environ.setdefault('MPLCONFIGDIR', str(cache))
        try:
            import matplotlib
        except ModuleNotFoundError:
            python = HERE.parent / '.venv/bin/python'
            if python.exists() and Path(sys.prefix) != python.parent.parent:
                os.execv(str(python), [str(python), str(Path(__file__).resolve()), *sys.argv[1:]])
            raise SystemExit('Matplotlib unavailable; use the reproduction .venv or pass --no-plots')
        matplotlib.use('Agg')
        import matplotlib.pyplot as plt
        groups = {}
        for r in rows:
            heap_rule = r['heap_kib'] / r['threads'] if r['workload'] == 'pressure' else r['heap_kib']
            key = tuple(r[k] for k in ('workload', 'burst', 'rounds', 'max_keep', 'budget_pages',
                                      'decay_ops', 'pressure_percent', 'validate')) + (heap_rule,)
            groups.setdefault(key, []).append(r)
        colors = {'eager': '#777777', 'lazy': '#d95f02', 'fixed-1': '#7570b3',
                  'fixed-2': '#1b9e77', 'adaptive': '#1261a0'}
        for number, (key, group) in enumerate(sorted(groups.items())):
            fig, axes = plt.subplots(2, 3, figsize=(12, 6.7), constrained_layout=True)
            panels = [('alloc_mean_us', 'Mean allocation latency (us)', 1),
                      ('alloc_p99_us', 'Allocation p99 (us)', 1), ('free_mean_us', 'Mean free latency (us)', 1),
                      ('total_us', 'Init + workload + drain (ms)', .001),
                      ('peak_reserved_bytes', 'Peak reserved MRAM (KiB)', 1/1024),
                      ('failures', 'Failed requests / init pages', 1)]
            for name in ('eager', 'lazy', 'fixed-1', 'fixed-2', 'adaptive'):
                selected = sorted([r for r in group if r['label'] == name], key=lambda r: r['threads'])
                if not selected:
                    continue
                for ax, (metric, title, scale) in zip(axes.flat, panels):
                    valid = selected if metric in ('peak_reserved_bytes', 'failures') else [r for r in selected if r['outcome'] == 'completed']
                    ax.plot([r['threads'] for r in valid], [r[metric]*scale for r in valid],
                            'o-', label=name, color=colors[name], linewidth=1.6, markersize=4)
                    ax.set_title(title, fontsize=10); ax.set_xlabel('Tasklets'); ax.grid(alpha=.22)
            ticks = sorted({r['threads'] for r in group})
            for ax in axes.flat:
                ax.set_xticks(ticks)
                ax.set_ylim(bottom=0)
                if len(ticks) == 1:
                    ax.set_xlim(ticks[0] - .5, ticks[0] + .5)
            if not any(r['failures'] for r in group):
                axes[1, 2].set_ylim(0, 1)
            axes[0, 0].legend(fontsize=8)
            fig.suptitle(f'{key[0]}: burst {key[1]}, rounds {key[2]}, empty budget {key[4]} pages/tasklet', fontsize=12)
            stem = out / f'{key[0]}-{number:02d}'
            fig.savefig(stem.with_suffix('.png'), dpi=220); fig.savefig(stem.with_suffix('.svg')); plt.close(fig)
            text += [f'Plot: {stem.name}.png / .svg', '']
    (out / 'RESULTS.md').write_text('\n'.join(text))
    print(f'Analyzed {len(rows)} configurations: {out / "RESULTS.md"}')
    print(f'CSV: {out / "comparison.csv"}')


if __name__ == '__main__': main()
