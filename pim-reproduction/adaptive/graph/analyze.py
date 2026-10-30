#!/usr/bin/env python3
"""Audit and plot measured small-object Gowalla graph updates."""
import argparse
import csv
import hashlib
import json
import os
from pathlib import Path
import sys

HERE = Path(__file__).resolve().parent
ADAPTIVE = HERE.parent
LABELS = ['eager', 'lazy', 'fixed-1', 'fixed-2', 'adaptive']
COLORS = dict(zip(LABELS, ['#777777', '#d95f02', '#7570b3', '#1b9e77', '#1261a0']))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--tag', default='gowalla-v1')
    parser.add_argument('--no-plots', action='store_true')
    a = parser.parse_args()
    files = sorted(p for p in (ADAPTIVE / 'results' / a.tag).glob('*/summary.json')
                   if '.superseded-' not in p.parent.name and '.incomplete-' not in p.parent.name)
    if not files: raise SystemExit('No completed graph runs for this tag')
    rows, records = [], []
    for path in files:
        s = json.loads(path.read_text()); c, g = s['configuration'], s['graph']
        assert s['correctness_passed'] and c['workload'] == 'graph'
        for name, expected in [('task.c', s['source_sha256']), ('trace.inc', g['trace_sha256'])]:
            assert hashlib.sha256((path.parent / name).read_bytes()).hexdigest() == expected, path
        label = f"fixed-{c['keep']}" if c['policy'] == 'fixed' else c['policy']
        rows.append({'policy': label, 'threads': c['threads'], 'block_bytes': c['graph_block_bytes'],
                     'heap_kib': c['heap_kib'], 'graph_edges': c['graph_edges'], 'seed': c['graph_seed'],
                     'rounds': c['rounds'], 'outcome': s['outcome'],
                     'update_allocations': g['update_alloc']['samples'],
                     'update_alloc_mean_us': g['update_alloc']['mean_us'],
                     'update_alloc_p99_us': g['update_alloc']['p99_us'],
                     'update_free_mean_us': g['update_free']['mean_us'],
                     'update_time_ms': s['work_makespan_us'] / 1000,
                     'total_time_ms': s['metrics']['total']['mean_us'] / 1000,
                     'frontend_hit_percent_all_requests': 100 * g['frontend_hit_fraction_all_requests'],
                     'peak_reserved_kib': s['backend']['peak_bytes'] / 1024,
                     'all_allocations': s['totals']['alloc_calls'], 'refills': s['totals']['refills'],
                     'retained_hits': s['totals']['retained_hits'], 'failures': s['totals']['failures'],
                     'pressure_trims': s['totals']['pressure_trims'],
                     'emergency_trims': s['totals']['emergency_trims'],
                     'idle_trims': s['totals']['idle_trims'],
                     'heap_end_address': s['wram']['heap_end_address'],
                     'implementation_sha256': s['implementation_sha256'],
                     'trace_sha256': g['trace_sha256'], 'run': str(path.parent)})
        records.append((rows[-1], s))
    assert len({r['implementation_sha256'] for r in rows}) == 1, 'Mixed implementations'
    for key in ('heap_kib', 'graph_edges', 'seed', 'rounds'):
        assert len({r[key] for r in rows}) == 1, f'Different {key}; analyze these campaigns under separate tags'
    for threads in {r['threads'] for r in rows}:
        assert len({r['trace_sha256'] for r in rows if r['threads'] == threads}) == 1, 'Different edge traces'
    for threads, block in {(r['threads'], r['block_bytes']) for r in rows}:
        complete = [s for r, s in records if r['threads'] == threads and r['block_bytes'] == block
                    and r['outcome'] == 'completed']
        counts = {(s['totals']['alloc_calls'], s['totals']['free_calls'],
                   tuple((p['allocations'], p['frees'], p['attempted_insert_arcs'])
                         for p in s['graph']['phases'])) for s in complete}
        assert len(counts) <= 1, 'Completed policies performed different graph/allocator work'
    out = ADAPTIVE / 'analysis' / a.tag; out.mkdir(parents=True, exist_ok=True)
    with (out / 'comparison.csv').open('w', newline='') as stream:
        writer = csv.DictWriter(stream, fieldnames=list(rows[0])); writer.writeheader(); writer.writerows(rows)
    text = ['# Gowalla small-object dynamic graph updates', '',
            'A reduced UPMEM adjacency-list implementation using real PrIM/SNAP Gowalla friendship edges. '
            'The changing update batches are synthetic, not recorded social-network arrival timestamps. '
            'This is a workload port/extension, not an exact Figure 17 reproduction or execution of CUDA faimGraph.', '',
            'Each peak batch uses the sampled 1:2 new/existing edge split. Both directions of each undirected edge '
            'are stored. Each batch inserts edges, traverses/validates adjacency lists, deletes inserted edges in '
            'reverse order, and checks restoration. Low phases perform four adjacency read passes; high/rebound one. '
            'Batch sizes follow low/high/low-again/rebound for each cycle.', '',
            'All policies use the same input edges, seed, ownership, payload work, and allocator code. '
            'A 64 B graph block is an additional object-size variant; the paper uses 256 B. '
            'Allocator pages remain 4096 B for both variants.', '',
            'Update time and allocation latency exclude graph preload/final teardown. Frontend hit percentages '
            'and refill counters include preload; they count allocator requests, not ordinary MRAM reads/writes. '
            'Peak reserved memory is measured under the backend lock. OOM runs are not ranked by speed.', '',
            'Update time spans the earliest update-phase start to the latest finish across tasklets. '
            'Individual phase windows can overlap slightly at barrier release, so their duration sum is '
            'retained as a diagnostic rather than used as overall elapsed time.', '',
            'The empirical nearest-rank p99 can be below the mean when fewer than 1% of requests are extremely '
            'expensive page refills. Raw samples are retained rather than removing those outliers.', '',
            '| Tasklets | Object (B) | Policy | Outcome | Update alloc mean / p99 (us) | Update time (ms) | Frontend hits (%) | Peak reserved (KiB) |',
            '| ---: | ---: | --- | --- | ---: | ---: | ---: | ---: |']
    for r in sorted(rows, key=lambda r: (r['threads'], r['block_bytes'], LABELS.index(r['policy']))):
        text.append(f"| {r['threads']} | {r['block_bytes']} | {r['policy']} | {r['outcome']} | "
                    f"{r['update_alloc_mean_us']:.2f} / {r['update_alloc_p99_us']:.2f} | "
                    f"{r['update_time_ms']:.2f} | {r['frontend_hit_percent_all_requests']:.2f} | {r['peak_reserved_kib']:.0f} |")
    text += ['', '## Interpretation of this run', '']
    for threads, block in sorted({(r['threads'], r['block_bytes']) for r in rows}):
        group = {r['policy']: r for r in rows if r['threads'] == threads and r['block_bytes'] == block}
        if not all(name in group and group[name]['outcome'] == 'completed'
                   for name in ('adaptive', 'fixed-2', 'eager')): continue
        adaptive, fixed, eager = (group[name] for name in ('adaptive', 'fixed-2', 'eager'))
        fixed_delta = 100 * (adaptive['update_time_ms'] / fixed['update_time_ms'] - 1)
        eager_delta = 100 * (adaptive['update_time_ms'] / eager['update_time_ms'] - 1)
        text.append(f"- {threads} tasklets, {block} B objects: adaptive update time is "
                    f"{abs(fixed_delta):.2f}% {'higher' if fixed_delta >= 0 else 'lower'} than fixed-2, "
                    f"and {abs(eager_delta):.2f}% {'higher' if eager_delta >= 0 else 'lower'} than eager. "
                    f"Peak reservation is {adaptive['peak_reserved_kib']:.0f} KiB for adaptive and "
                    f"{fixed['peak_reserved_kib']:.0f} KiB for fixed-2.")
    text += ['', 'High frontend hit rates support the suitability of this workload for small-object thread caching. '
             'They do not by themselves demonstrate that adaptive retention improves on fixed retention. '
             'The graph kernel uses one size class at a time, so it does not exercise migration of demand between '
             'allocator size classes. The present controller checks idleness in allocator-operation counts; '
             'elapsed graph-traversal time alone does not make a class decay. Block-size comparisons also change '
             'graph payload transfer size and adjacency capacity, so their runtime difference is not solely an allocator effect.',
             f"Pressure trims: {sum(r['pressure_trims'] for r in rows)}; emergency trims: "
             f"{sum(r['emergency_trims'] for r in rows)}. A separate constrained-heap experiment is needed to evaluate reclamation.", '']
    if not a.no_plots:
        os.environ.setdefault('MPLCONFIGDIR', str(ADAPTIVE / '.cache/matplotlib'))
        try:
            import matplotlib
        except ModuleNotFoundError:
            python = ADAPTIVE.parent / '.venv/bin/python'
            os.execv(str(python), [str(python), str(Path(__file__).resolve()), *sys.argv[1:]])
        matplotlib.use('Agg')
        import matplotlib.pyplot as plt
        plt.rcParams.update({'font.size': 10, 'axes.spines.top': False, 'axes.spines.right': False,
                             'svg.fonttype': 'none'})
        for block in sorted({r['block_bytes'] for r in rows}):
            selected = [r for r in rows if r['block_bytes'] == block]
            fig, axes = plt.subplots(2, 3, figsize=(12, 6.7), constrained_layout=True)
            panels = [('update_alloc_mean_us', 'Update allocation mean (us)'),
                      ('update_alloc_p99_us', 'Update allocation p99 (us)'),
                      ('update_time_ms', 'Graph update time (ms)'),
                      ('frontend_hit_percent_all_requests', 'Frontend hits incl. preload (%)'),
                      ('peak_reserved_kib', 'Peak reserved MRAM (KiB)'),
                      ('refills', 'Backend page refills incl. preload')]
            for name in LABELS:
                points = sorted([r for r in selected if r['policy'] == name], key=lambda r: r['threads'])
                for ax, (metric, title) in zip(axes.flat, panels):
                    valid = points if metric in ('peak_reserved_kib', 'refills') else [r for r in points if r['outcome'] == 'completed']
                    ax.plot([r['threads'] for r in valid], [r[metric] for r in valid],
                            'o-', color=COLORS[name], label=name, linewidth=1.7, markersize=4)
                    ax.set_title(title, fontsize=10); ax.set_xlabel('Tasklets'); ax.grid(alpha=.2)
                    ax.set_xticks(sorted({r['threads'] for r in selected}))
            for ax in axes.flat: ax.set_ylim(bottom=0)
            axes[1, 0].set_ylim(0, 100); axes[0, 0].legend(fontsize=8)
            fig.suptitle(f'Gowalla dynamic updates: {block} B graph objects, fixed 4 KiB allocator pages')
            for suffix in ('.png', '.svg'): fig.savefig(out / f'comparison-{block}B{suffix}', dpi=220)
            plt.close(fig)
        for threads in sorted({r['threads'] for r in rows}):
            for block in sorted({r['block_bytes'] for r in rows}):
                group = [(r, s) for r, s in records if r['threads'] == threads and r['block_bytes'] == block]
                if not group: continue
                fig, slots = plt.subplots(4, 1, figsize=(10, 8.5), constrained_layout=True,
                                          gridspec_kw={'height_ratios': [1, 1, .14, 1]})
                axes = [slots[0], slots[1], slots[3]]
                slots[2].axis('off')
                sample = group[0][1]['graph']['phases']
                indexes = list(range(len(sample)))
                axes[0].bar(indexes, [p['attempted_insert_arcs'] for p in sample], color='#97b3c9')
                axes[0].set_ylabel('Inserted adjacency arcs'); axes[0].set_title('Identical low / high / low / rebound batches')
                for r, s in sorted(group, key=lambda item: LABELS.index(item[0]['policy'])):
                    if r['outcome'] != 'completed': continue
                    phases = s['graph']['phases']; name = r['policy']
                    axes[1].plot(indexes, [p['alloc_mean_us'] for p in phases], 'o-', color=COLORS[name], label=name)
                    snapshots = list(csv.DictReader((Path(r['run']) / 'snapshots.csv').open()))
                    live = [p for p in snapshots if int(p['phase']) in (1, 3, 5, 7)]
                    axes[2].plot(indexes, [int(p['reserved_bytes']) / 1024 for p in live], 'o-', color=COLORS[name], label=name)
                axes[1].set_ylabel('Allocation mean (us)'); axes[1].set_title('Allocator requests during graph updates')
                axes[2].set_ylabel('Reserved MRAM (KiB)'); axes[2].set_title('Memory after each insertion batch')
                for ax in axes:
                    ax.set_xticks(indexes, [p['phase'] for p in sample], rotation=20)
                    ax.set_ylim(bottom=0); ax.grid(axis='y', alpha=.2)
                axes[2].set_xlabel('Successive workload phases (closed-loop; no external arrival clock)')
                handles, names = axes[1].get_legend_handles_labels()
                slots[2].legend(handles, names, ncol=5, fontsize=8, loc='center', frameon=False)
                fig.suptitle(f'Gowalla changing demand: {threads} tasklets, {block} B graph blocks')
                for suffix in ('.png', '.svg'): fig.savefig(out / f'phases-t{threads}-{block}B{suffix}', dpi=220)
                plt.close(fig)
        max_threads = max(r['threads'] for r in rows)
        blocks = sorted({r['block_bytes'] for r in rows})
        fig, axes = plt.subplots(2, len(blocks), figsize=(6 * len(blocks), 6.5),
                                 squeeze=False, constrained_layout=True)
        phase_colors = ['#e5eef5', '#f5d7ce', '#e5eef5', '#f4e6c1']
        phase_short_names = ['L', 'H', 'L2', 'R']
        max_latency = 1
        for column, block in enumerate(blocks):
            for row, name in enumerate(('fixed-2', 'adaptive')):
                ax = axes[row, column]
                selected = [(r, s) for r, s in records if r['threads'] == max_threads
                            and r['block_bytes'] == block and r['policy'] == name and r['outcome'] == 'completed']
                if not selected: continue
                r, s = selected[0]; phases = s['graph']['phases']; origin = phases[0]['begin_cycle']
                samples = list(csv.DictReader((Path(r['run']) / 'events.csv').open()))
                samples = [p for p in samples if p['kind'] == 'alloc'
                           and int(p['begin_cycle']) >= origin and int(p['end_cycle']) <= phases[-1]['end_cycle']]
                for p in phases:
                    begin, end = (p['begin_cycle'] - origin) / 350000, (p['end_cycle'] - origin) / 350000
                    ax.axvspan(begin, end, color=phase_colors[p['index'] % 4], alpha=.7, linewidth=0)
                    if p['index'] < 4:
                        ax.text((begin + end) / 2, .98, phase_short_names[p['index'] % 4], ha='center', va='top',
                                transform=ax.get_xaxis_transform(), fontsize=8)
                ax.scatter([(int(p['begin_cycle']) - origin) / 350000 for p in samples],
                           [int(p['cycles']) / 350 for p in samples], s=5, alpha=.65,
                           color=COLORS[name], rasterized=True)
                max_latency = max(max_latency, max(int(p['cycles']) / 350 for p in samples))
                ax.set_yscale('log'); ax.set_title(f'{name}, {block} B objects')
                ax.set_xlabel('Elapsed simulated time since update start (ms)')
                ax.set_ylabel('Individual allocation latency (us, log scale)'); ax.grid(axis='y', alpha=.2)
        for ax in axes.flat: ax.set_ylim(1, 1.5 * max_latency)
        fig.suptitle(f'Allocation latency over execution: {max_threads} tasklets (phase timing differs by policy)\n'
                     'L = low; H = high; L2 = low-again; R = rebound', fontsize=12)
        for suffix in ('.png', '.svg'): fig.savefig(out / f'latency-over-time{suffix}', dpi=220)
        plt.close(fig)
        text += ['', 'Plots: comparison-64B / comparison-256B, phases-t<tasklets>-<object>B, '
                 'and latency-over-time (PNG/SVG). Time plots show each policy\'s own simulated phase boundaries.']
    text += ['', 'Per-request cycles, graph phase durations, memory snapshots, generated input with original vertex IDs, '
             'source hashes, and binaries are retained in every run folder. The inherited simulator/IRAM limitations '
             'and reconstructed baseline repairs in the parent README still apply.', '']
    (out / 'RESULTS.md').write_text('\n'.join(text))
    (out / 'audit.json').write_text(json.dumps({'configurations': len(rows),
        'completed': sum(r['outcome'] == 'completed' for r in rows),
        'failed_allocator_requests': sum(r['failures'] for r in rows),
        'source_and_trace_hashes_verified': True,
        'completed_policies_have_equal_graph_and_allocator_work': True,
        'max_wram_heap_end_address': max(r['heap_end_address'] for r in rows),
        'implementation_sha256': rows[0]['implementation_sha256']}, indent=2) + '\n')
    print(f'Analyzed {len(rows)} graph configurations: {out / "RESULTS.md"}')


if __name__ == '__main__': main()
