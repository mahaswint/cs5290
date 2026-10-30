#!/usr/bin/env python3
"""Report every outcome and compare only identical, completed offered traces."""
import argparse
import csv
import hashlib
import json
import os
from collections import defaultdict
from pathlib import Path

HERE = Path(__file__).resolve().parent


def label(c):
    return c['mode'] + '/' + (f"fixed-{c['keep']}" if c['policy'] == 'fixed' else c['policy'])


def number(value):
    return 'N/A' if value is None else f'{value:.2f}'


def group(c):
    return tuple((k, c[k]) for k in ('workload', 'threads', 'burst', 'rounds', 'heap_kib',
                                    'budget_pages', 'max_keep', 'decay_ops', 'pressure_percent',
                                    'seed', 'graph_edges', 'graph_block_bytes', 'validate'))


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--tag', default='combined-v2')
    a = p.parse_args()
    paths = sorted(p for p in (HERE / 'results' / a.tag).glob('*/summary.json')
                   if '.superseded-' not in p.parent.name and '.incomplete-' not in p.parent.name)
    if not paths: p.error('No completed summaries')
    groups, rows, digests = defaultdict(list), [], []
    for path in paths:
        d = json.loads(path.read_text()); c = d['configuration']
        assert d['correctness_passed']
        assert hashlib.sha256((path.parent / 'task.c').read_bytes()).hexdigest() == d['source_sha256']
        am = d.get('graph', {}).get('update_alloc', d.get('work_alloc', d['metrics']['alloc']))
        fm = d.get('graph', {}).get('update_free', d.get('work_free', d['metrics']['free']))
        row = dict(workload=c['workload'], threads=c['threads'], burst=c['burst'], rounds=c['rounds'],
                   heap_kib=c['heap_kib'], seed=c['seed'], variant=label(c), outcome=d['outcome'],
                   work_ms=d['work_makespan_us']/1000, total_ms=d['metrics']['total']['mean_us']/1000,
                   init_ms=d['metrics']['init']['mean_us']/1000,
                   alloc_mean_us=am['mean_us'], alloc_p99_us=am['p99_us'],
                   free_mean_us=fm['mean_us'], free_p99_us=fm['p99_us'],
                   peak_payload_kib=d['backend']['peak_bytes']/1024,
                   owner_table_kib=d['owner_table_mram_bytes']/1024,
                   peak_payload_plus_owner_kib=d['peak_payload_plus_owner_bytes']/1024,
                   refills=d['totals']['refills'], failures=d['totals']['failures'],
                   idle_trims=d['totals']['idle_trims'], pressure_trims=d['totals']['pressure_trims'],
                   emergency_trims=d['totals']['emergency_trims'],
                   wram_end=d['wram']['heap_end_address'],
                   instructions=d['native_statistics'].get('Logic[0_0_0]_num_instructions'),
                   source_sha256=d['source_sha256'], path=str(path.parent))
        rows.append(row); groups[group(c)].append((d, row))
        digests.append(dict(path=str(path), summary_sha256=hashlib.sha256(path.read_bytes()).hexdigest()))
    out = HERE / 'analysis' / a.tag; out.mkdir(parents=True, exist_ok=True)
    with (out / 'comparison.csv').open('w', newline='') as f:
        w = csv.DictWriter(f, fieldnames=list(rows[0])); w.writeheader(); w.writerows(rows)
    lines = ['# Combined retention and indexed page management', '',
             'Single-DPU simulated measurements at 350 MHz. Scan uses the maintained allocator verbatim. '
             'Lists and lookup preserve its page/slot geometry and same-tasklet free contract. '
             'All offered configurations, including regressions and allocation failures, are reported.', '',
             'Work time includes payload checks, pointer transfers, barriers and snapshots; graph work time '
             'includes update validation traversals and excludes preload/teardown. Total includes initialization '
             'and final drain (graph and resident: also preload/teardown). Allocation/free means for graph/resident use work windows. '
             'Do not rank OOM runs by runtime against completed runs.', '',
             'Peak payload and owner-table bytes are shown separately. The owner table uses 8 B per 4 KiB '
             'payload page: 64 KiB for a 32 MiB heap, outside the equal-capacity payload heap. '
             'Additional metadata is charged, not hidden. Common buddy metadata, benchmark storage, and WRAM '
             'are separate. WRAM endpoint checks are simulation capacity checks, not complete hardware validation.', '']
    comparisons = []
    for key, members in sorted(groups.items()):
        settings = dict(key); workload, threads = settings['workload'], settings['threads']
        lines += [f"## {workload}, {threads} tasklets, burst {settings['burst']}, heap {settings['heap_kib']} KiB, seed {settings['seed']}", '',
                  '| Variant | Outcome | Work / total (ms) | Alloc / free mean (us) | Peak payload + map (KiB) | Refills | Failures |',
                  '| --- | --- | ---: | ---: | ---: | ---: | ---: |']
        for d, r in sorted(members, key=lambda pair: pair[1]['variant']):
            lines.append(f"| {r['variant']} | {r['outcome']} | {r['work_ms']:.2f} / {r['total_ms']:.2f} | "
                         f"{number(r['alloc_mean_us'])} / {number(r['free_mean_us'])} | "
                         f"{r['peak_payload_kib']:.0f} + {r['owner_table_kib']:.2f} | {r['refills']} | {r['failures']} |")
        good = {r['variant']: (d, r) for d, r in members if r['outcome'] == 'completed'}
        scan_controls = [(d, r) for d, r in members if r['outcome'] == 'completed' and
                         d['configuration']['mode'] == 'scan' and d['configuration']['policy'] != 'adaptive']
        combined = good.get('indexed/adaptive')
        if combined and scan_controls:
            bd, br = min(scan_controls, key=lambda pair: pair[1]['work_ms'])
            cd, cr = combined
            # Successful offered operations must match before computing a speedup.
            assert bd['totals']['alloc_calls'] == cd['totals']['alloc_calls']
            assert bd['totals']['successes'] == cd['totals']['successes']
            if workload == 'graph': assert bd['graph']['trace_sha256'] == cd['graph']['trace_sha256']
            ratio = br['work_ms'] / cr['work_ms']
            comparison = dict(settings, baseline=br['variant'], speedup=ratio,
                              baseline_work_ms=br['work_ms'], combined_work_ms=cr['work_ms'],
                              baseline_total_ms=br['total_ms'], combined_total_ms=cr['total_ms'],
                              baseline_peak_kib=br['peak_payload_plus_owner_kib'],
                              combined_peak_kib=cr['peak_payload_plus_owner_kib'])
            if 'indexed/fixed-2' in good:
                comparison['adaptive_vs_indexed_fixed2_speedup'] = good['indexed/fixed-2'][1]['work_ms']/cr['work_ms']
            comparisons.append(comparison)
            lines += ['', f"Combined vs fastest successful non-adaptive scan control ({br['variant']}): "
                      f"**{ratio:.3f}x** work speedup. Values below 1 are regressions."]
            if 'adaptive_vs_indexed_fixed2_speedup' in comparison:
                lines += [f"Adaptive vs indexed fixed-2: {comparison['adaptive_vs_indexed_fixed2_speedup']:.3f}x."]
        lines += ['']
    lines += ['## Interpretation limits', '',
              'Scatter, resident, handoff, pressure, control, and backend are explicit synthetic workload families, '
              'chosen to isolate mechanisms. Scatter preserves half a live set while refilling scattered holes; '
              'resident keeps a pool live across rounds and replaces half its objects; '
              'handoff shifts repeated bursts between classes; pressure constrains the payload heap. '
              'The stable small-live-set control and uncached backend control expose overhead. '
              'Graph uses the existing reservoir-sampled Gowalla edges and a synthetic batch schedule. '
              'These are neither full-dataset throughput results nor recorded production traces.', '',
              'Simulator executions are deterministic; seeds vary operation order or graph samples, not timing noise. '
              'No measured speedup is a guarantee for other workloads. The original Figure 15 results are unchanged.', '']
    (out / 'RESULTS.md').write_text('\n'.join(lines))
    (out / 'audit.json').write_text(json.dumps(dict(runs=digests, comparisons=comparisons), indent=2) + '\n')
    if comparisons:
        try:
            os.environ.setdefault('MPLCONFIGDIR', str(HERE / '.mplconfig'))
            import matplotlib
            matplotlib.use('Agg')
            import matplotlib.pyplot as plt
            import numpy as np
        except ImportError:
            print('Tables written; use the existing pim-reproduction/.venv Python for plots.')
        else:
            fig, ax = plt.subplots(figsize=(max(9, len(comparisons)*.75), 4.8))
            x = np.arange(len(comparisons))
            ax.bar(x, [c['speedup'] for c in comparisons], color=['#167d9a' if c['speedup'] >= 1 else '#c95b48' for c in comparisons])
            ax.axhline(1, color='#333333', lw=1)
            ax.set_xticks(x, [f"{c['workload']}\nt{c['threads']}, seed {c['seed']}" for c in comparisons], rotation=35, ha='right')
            ax.set_ylabel('Work speedup over fastest non-adaptive scan control')
            ax.set_title('Combined adaptive retention + indexed pages (below 1 = regression)')
            ax.grid(axis='y', alpha=.2); ax.set_axisbelow(True); fig.tight_layout()
            for suffix in ('png', 'svg'): fig.savefig(out / f'work-speedups.{suffix}', dpi=160)
            plt.close(fig)
    print(f'{len(rows)} runs, {len(comparisons)} matched comparisons: {out}')


if __name__ == '__main__': main()
