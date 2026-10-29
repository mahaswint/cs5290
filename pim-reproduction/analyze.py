#!/usr/bin/env python3
"""Compare actual measurements with printed Figure 15 values; export figures."""
import csv
import json
import math
import os
from pathlib import Path

BASE = Path(__file__).resolve().parent
os.environ.setdefault('MPLCONFIGDIR', str(BASE / '.cache/matplotlib'))
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
import numpy as np


def main():
    output = BASE / 'analysis'
    output.mkdir(exist_ok=True)
    with (BASE / 'reference/figure15_published.csv').open() as stream:
        published = list(csv.DictReader(stream))
    rows = []
    for item in published:
        threads, size = int(item['threads']), int(item['size_bytes'])
        path = BASE / 'results/fig15' / f"{item['allocator']}_t{threads}_s{size}_n128" / 'summary.json'
        if not path.exists():
            raise SystemExit(f'Measurement is not complete: {path.parent.name}')
        actual = json.loads(path.read_text())
        with (path.parent / 'allocations.csv').open() as stream:
            samples = list(csv.DictReader(stream))
        batch_cycles = max(int(r['end_cycle']) for r in samples) - min(int(r['begin_cycle']) for r in samples)
        batch_latency = batch_cycles / actual['requests_per_thread'] / actual['frequency_mhz']
        reference = float(item['average_latency_us'])
        rows.append({'threads': threads, 'size_bytes': size, 'allocator': item['allocator'],
                     'paper_latency_us': reference, 'measured_latency_us': batch_latency,
                     'mean_individual_request_us': actual['average_latency_us'],
                     'difference_percent': 100 * (batch_latency / reference - 1),
                     'samples': actual['samples'], 'average_cycles': actual['average_cycles']})
    with (output / 'figure15_comparison.csv').open('w', newline='') as stream:
        writer = csv.DictWriter(stream, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)

    plt.rcParams.update({'font.family': 'DejaVu Sans', 'font.size': 10,
                         'axes.spines.top': False, 'axes.spines.right': False,
                         'svg.fonttype': 'none', 'savefig.facecolor': 'white'})
    fig, axes = plt.subplots(1, 2, figsize=(10, 4.1), layout='constrained')
    x = np.arange(3)
    sizes = (32, 256, 4096)
    colors = {'buddy': '#343c4a', 'sw': '#007b83'}
    labels = {'buddy': 'Simple buddy allocator', 'sw': 'PIM-malloc-SW'}
    for ax, threads in zip(axes, (1, 16)):
        for allocator, offset in [('buddy', -0.19), ('sw', 0.19)]:
            subset = [next(r for r in rows if r['threads'] == threads and r['size_bytes'] == size and r['allocator'] == allocator) for size in sizes]
            values = [r['measured_latency_us'] for r in subset]
            ax.bar(x + offset, values, width=0.34, color=colors[allocator], label=labels[allocator], zorder=3)
            ax.scatter(x + offset, [r['paper_latency_us'] for r in subset], marker='_', s=250,
                       color='#d34831', linewidths=2, zorder=5,
                       label='Paper (rounded labels)' if allocator == 'buddy' else None)
            for position, value in zip(x + offset, values):
                ax.annotate(f'{value:,.2f}' if value < 100 else f'{value:,.0f}',
                            (position, value), xytext=(0, 7), textcoords='offset points',
                            ha='center', va='bottom', fontsize=9, rotation=0)
        ax.set_title(f'{threads} thread' + ('s' if threads != 1 else ''), weight='bold')
        ax.set_xticks(x, ['32 B', '256 B', '4 KB'])
        ax.set_xlabel('Allocation size')
        ax.set_ylabel('Batch time / 128 requests (µs, log scale)')
        ax.set_yscale('log')
        ax.set_ylim(2 if threads == 1 else 20, 1800 if threads == 1 else 45000)
        ax.grid(axis='y', which='major', alpha=0.25, zorder=0)
    handles, names = axes[0].get_legend_handles_labels()
    fig.legend(handles, names, loc='outside lower center', ncol=3, frameon=False)
    fig.suptitle('Figure 15 reproduction · software allocators only', fontsize=13, weight='bold')
    fig.savefig(output / 'figure15_reproduction.png', dpi=240)
    fig.savefig(output / 'figure15_reproduction.svg')
    plt.close(fig)

    # The request mean answers a different question from concurrent batch completion.
    fig, axes = plt.subplots(1, 2, figsize=(10, 4.1), layout='constrained')
    for ax, threads in zip(axes, (1, 16)):
        for allocator, offset in [('buddy', -0.19), ('sw', 0.19)]:
            values = [next(r['mean_individual_request_us'] for r in rows
                           if r['threads'] == threads and r['size_bytes'] == size and r['allocator'] == allocator)
                      for size in sizes]
            ax.bar(x + offset, values, width=0.34, color=colors[allocator], label=labels[allocator], zorder=3)
            for position, value in zip(x + offset, values):
                ax.annotate(f'{value:,.2f}' if value < 100 else f'{value:,.0f}',
                            (position, value), xytext=(0, 7), textcoords='offset points',
                            ha='center', va='bottom', fontsize=9)
        ax.set_title(f'{threads} thread' + ('s' if threads != 1 else ''), weight='bold')
        ax.set_xticks(x, ['32 B', '256 B', '4 KB'])
        ax.set_xlabel('Allocation size')
        ax.set_ylabel('Mean individual allocation latency (µs)')
        ax.set_yscale('log')
        ax.set_ylim(2 if threads == 1 else 10, 1800 if threads == 1 else 45000)
        ax.grid(axis='y', which='major', alpha=0.25, zorder=0)
    handles, names = axes[0].get_legend_handles_labels()
    fig.legend(handles, names, loc='outside lower center', ncol=2, frameon=False)
    fig.suptitle('Measured per-request latency · software allocators only', fontsize=13, weight='bold')
    fig.savefig(output / 'mean_request_latency.png', dpi=240)
    fig.savefig(output / 'mean_request_latency.svg')
    plt.close(fig)

    table = ['# Figure 15 reproduction results', '',
             'Paper values are rounded labels transcribed from Figure 15. Measured values come from local uPIMulator runs, not digitized bars. The comparison uses allocation-batch completion time divided by 128 requests per thread; the arithmetic mean of individual request latencies is retained separately in the CSV.', '',
             '| Threads | Size | Buddy: paper / measured (µs) | SW: paper / measured (µs) | Measured speedup |',
             '| --- | --- | ---: | ---: | ---: |']
    speedups = []
    for threads in (1, 16):
        for size in sizes:
            buddy = next(r for r in rows if r['threads'] == threads and r['size_bytes'] == size and r['allocator'] == 'buddy')
            sw = next(r for r in rows if r['threads'] == threads and r['size_bytes'] == size and r['allocator'] == 'sw')
            speedup = buddy['measured_latency_us'] / sw['measured_latency_us']
            speedups.append(speedup)
            table.append(f"| {threads} | {size} B | {buddy['paper_latency_us']:g} / {buddy['measured_latency_us']:.2f} | {sw['paper_latency_us']:g} / {sw['measured_latency_us']:.2f} | {speedup:.2f}× |")
    geometric = math.exp(sum(math.log(x) for x in speedups) / len(speedups))
    arithmetic = sum(speedups) / len(speedups)
    maximum_error = max(abs(r['difference_percent']) for r in rows)
    table += ['', f'Arithmetic mean of the six speedup ratios: **{arithmetic:.2f}×**.',
              f'Geometric mean speedup over these six configurations: **{geometric:.2f}×**.',
              f'Maximum absolute deviation from a printed paper label: **{maximum_error:.2f}%**.', '',
              'This geometric mean is defined over the six listed microbenchmark configurations. It does not aggregate workloads or include hardware-cache results.', '',
              'Capacity audit (29 October 2026): the inherited authors\' simulator permits 256 KiB WRAM, while physical UPMEM has 64 KiB. Its fixed 24 × 4 KiB stack reservations place the 16-thread SW image\'s heap start above the physical limit. The figures above remain measurements of that released simulation configuration, not a demonstrated hardware-valid memory layout. See `../research/WRAM_AND_SLAB_CONSTRAINTS.md`; original measurements and simulator settings are preserved.', '',
              'Each configuration contains 128 requests per thread. Initialization is excluded; lock waiting is included. Cycles are converted at 350 MHz. The paper labels its metric average latency but does not explicitly state its multithread aggregation formula; using batch completion time / 128 is our interpretation, supported by the comparison, not confirmed author metadata. See ../README.md for harness and host VM configuration differences.', '']
    (output / 'RESULTS.md').write_text('\n'.join(table))
    print('\n'.join(table))


if __name__ == '__main__':
    main()
