#!/usr/bin/env python3
"""Verify all declared final matrices and write a concise, reproducible report."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import run

HERE = Path(__file__).resolve().parent
TAGS = ['combined-v2', 'resident-v2', 'large-v2', 'ablation-v2', 'heldout-v2']


def records(tag):
    return [json.loads(p.read_text()) for p in sorted((HERE / 'results' / tag).glob('*/summary.json'))
            if '.superseded-' not in p.parent.name and '.incomplete-' not in p.parent.name]


def pick(data, workload, threads, mode, policy, keep=2):
    selected = [d for d in data if all(d['configuration'][k] == v for k, v in
                dict(workload=workload, threads=threads, mode=mode, policy=policy, keep=keep).items())]
    assert len(selected) == 1, (workload, threads, mode, policy, keep, len(selected))
    return selected[0]


def main():
    all_data, audit = {}, {}
    for tag in TAGS:
        data = records(tag); all_data[tag] = data
        expected = set()
        for matrix in (HERE / 'results' / tag).glob('matrix-*.json'):
            for c in json.loads(matrix.read_text()):
                full = vars(run.options().parse_args([])) | {'tag': tag} | c
                expected.add(json.dumps(full, sort_keys=True))
        actual = {json.dumps(d['configuration'], sort_keys=True) for d in data}
        assert expected and expected == actual, (tag, 'missing/extra configurations', len(expected), len(actual))
        for d in data:
            c = argparse.Namespace(**d['configuration']); source, _ = run.source_for(c)
            assert hashlib.sha256(source.encode()).hexdigest() == d['source_sha256']
            assert d['correctness_passed'] and d['wram']['heap_end_address'] <= 65536
            assert d['backend']['used_bytes'] == 0 and d['backend']['allocations'] == d['backend']['frees']
        audit[tag] = dict(runs=len(data), completed=sum(d['outcome']=='completed' for d in data),
                          oom=sum(d['outcome']=='oom' for d in data), declared_matrix_complete=True,
                          current_generated_source_matches=True)
    native = json.loads((HERE / 'validation/checks.json').read_text())
    assert len(native) == 16 and all(d['native_passed'] for d in native)
    validation_tags = ['validation-final', 'validation-resident-v2', 'validation-16-v2', 'validation-zero-free']
    validation = [d for tag in validation_tags for d in records(tag)]
    assert len(validation) == 27 and all(d['correctness_passed'] for d in validation)
    audit['validation'] = dict(native_mode_policy_combinations=16, simulator_cases=27,
                               maximum_tested_tasklets=max(d['configuration']['threads'] for d in validation))
    audit['maximum_wram_end_address'] = max(d['wram']['heap_end_address'] for data in all_data.values() for d in data + validation)
    main_data, resident = all_data['combined-v2'], all_data['resident-v2']
    examples = [('Resident pool, 1 tasklet', resident, 'resident', 1),
                ('Resident pool, 4 tasklets', resident, 'resident', 4),
                ('Class handoff, 4 tasklets', main_data, 'handoff', 4),
                ('Small pool, 4 tasklets', main_data, 'control', 4),
                ('Gowalla sample, 4 tasklets', main_data, 'graph', 4),
                ('Backend only, 4 tasklets', main_data, 'backend', 4)]
    rows = []
    for name, data, workload, threads in examples:
        baseline = pick(data, workload, threads, 'scan', 'fixed')
        fixed = pick(data, workload, threads, 'indexed', 'fixed')
        combined = pick(data, workload, threads, 'indexed', 'adaptive')
        assert all(d['outcome']=='completed' for d in (baseline, fixed, combined))
        assert len({d['totals']['successes'] for d in (baseline, fixed, combined)}) == 1
        rows.append(dict(name=name, baseline_ms=baseline['work_makespan_us']/1000,
                         combined_ms=combined['work_makespan_us']/1000,
                         fixed_indexed_speedup=baseline['work_makespan_us']/fixed['work_makespan_us'],
                         combined_speedup=baseline['work_makespan_us']/combined['work_makespan_us'],
                         total_speedup=baseline['metrics']['total']['mean_us']/combined['metrics']['total']['mean_us']))
    audit['headline_comparisons'] = rows
    lines = ['# Combined allocator: measured outcome', '',
             'Implemented adaptive empty-page retention with partial/full/empty page lists and direct MRAM '
             'ownership lookup. The scan control includes the maintained allocator unchanged; fixed and adaptive '
             'share the same four-empty-page budget per tasklet. Original Figure 15 results and earlier experiments '
             'are preserved.', '',
             'The best workload is a persistent object pool with scattered replacement across many pages. '
             'Changing size-class demand is the useful workload for adaptation itself. Small pools and this '
             'small graph sample do not justify enabling the extra mechanisms for speed.', '',
             '## Representative measurements', '',
             'All rows below compare the same completed offered work against **scan + fixed-2**. '
             'The detailed main report also includes fixed-1 and immediate-return controls.', '',
             '| Workload | Scan fixed-2 work (ms) | Indexed adaptive work (ms) | Work speedup | Total speedup |',
             '| --- | ---: | ---: | ---: | ---: |']
    for r in rows:
        lines.append(f"| {r['name']} | {r['baseline_ms']:.2f} | {r['combined_ms']:.2f} | "
                     f"{r['combined_speedup']:.3f}x | {r['total_speedup']:.3f}x |")
    lines += ['', '**Values below 1 are regressions.** Total includes initialization and final drain; '
              'for the resident pool it also includes preload and destruction. Graph work includes its '
              'payload traversals/checks and excludes preload/teardown. These are simulated single-DPU '
              'measurements at 350 MHz, not hardware throughput.', '',
              '![Measured speedups](headline-speedups.png)', '',
              '## Which workloads to use', '',
              '- **Persistent pool / scattered replacement:** 512 live 256 B objects per tasklet; replace '
              'half per round for 12 rounds. This preserves a substantial live set, creates reusable holes '
              'throughout many pages, and avoids repeated complete teardown during work. It is a synthetic '
              'model of an object store or graph block pool, not a captured application trace.',
              '- **Class handoff:** six bursts of 32 objects in each of the 256 B, 1024 B, 64 B, and 512 B '
              'classes. Cold classes compete with newly hot classes for the same retention budget. '
              'Compare indexed adaptive directly with indexed fixed-2 to isolate adaptation.',
              '- **Pressure as a capacity experiment:** report success/failure and retained memory, rather '
              'than ranking incomplete runs by runtime. The single-tasklet case demonstrates reclamation; '
              'the concurrent near-full-heap case exposes the current owner-local limit.',
              '- **Keep small pools, backend-only requests, and the Gowalla sample as controls.** '
              'They expose metadata/controller overhead and keep the conclusions appropriately narrow.', '',
              '## Isolating the mechanisms', '']
    baseline = pick(resident, 'resident', 1, 'scan', 'fixed')
    lines += ['Resident pool, one tasklet, fixed-2 retention throughout:', '',
              '| Page management | Work (ms) | Speedup over scans |', '| --- | ---: | ---: |']
    for mode in ('scan', 'lists', 'lookup', 'indexed'):
        d = pick(resident, 'resident', 1, mode, 'fixed')
        lines.append(f"| {mode} | {d['work_makespan_us']/1000:.2f} | {baseline['work_makespan_us']/d['work_makespan_us']:.3f}x |")
    hf = pick(main_data, 'handoff', 4, 'indexed', 'fixed')
    ha = pick(main_data, 'handoff', 4, 'indexed', 'adaptive')
    lines += ['', 'The partial-page lists deliver most of the persistent-pool gain. Direct lookup improves '
              'the combined static configuration further. Adaptive retention adds overhead on a stable pool; '
              'indexed fixed retention is the faster choice for that workload.', '',
              f"For class handoff at four tasklets, indexed adaptive takes {ha['work_makespan_us']/1000:.2f} ms "
              f"versus {hf['work_makespan_us']/1000:.2f} ms for indexed fixed-2 "
              f"({hf['work_makespan_us']/ha['work_makespan_us']:.3f}x), with "
              f"{ha['totals']['refills']} rather than {hf['totals']['refills']} refills. "
              'This comparison isolates the benefit of retention adaptation.', '',
              '## Capacity outcomes and limitation', '',
              'Pressure uses 16 objects per burst, three rounds per class, and a 64 KiB payload heap '
              'per active tasklet. Fixed/adaptive retain at most four empty pages per tasklet. '
              'All offered requests and all allocation failures are counted.', '',
              '| Tasklets | Indexed policy | Outcome | Failed requests |', '| ---: | --- | --- | ---: |']
    for threads in (1, 4):
        for policy, keep in [('lazy', 0), ('fixed', 1), ('fixed', 2), ('adaptive', 2)]:
            d = pick(main_data, 'pressure', threads, 'indexed', policy, keep)
            lines.append(f"| {threads} | {policy if policy!='fixed' else 'fixed-'+str(keep)} | {d['outcome']} | {d['totals']['failures']} |")
    lines += ['', 'At four tasklets the adaptive allocator still fails near capacity. It only drains its '
              'own empty pages, and periodic maintenance cannot reclaim another inactive tasklet’s retained '
              'pages. Indexed lookup does not solve that ownership/synchronization problem. No speedup is '
              'claimed for those failed runs.', '', '## Held-out checks', '',
              'Allocator settings were frozen before testing seeds **9173 and 24107**. Held-out pools use '
              '256 objects and eight replacement rounds; handoff uses four bursts per class; graph inputs '
              'are independently reservoir-sampled with those seeds. This checks additional orderings and '
              'samples, not statistical timing noise in the deterministic simulator.', '']
    heldout_report = json.loads((HERE / 'analysis/heldout-v2/audit.json').read_text())
    for workload in ('resident', 'handoff', 'graph'):
        speeds = [r['speedup'] for r in heldout_report['comparisons'] if r['workload']==workload]
        assert len(speeds) == 4
        lines.append(f"- {workload}: {min(speeds):.3f}x–{max(speeds):.3f}x work speedup over the tested fixed-2 scan control.")
    lines += ['', 'No retention threshold was tuned to these results. The larger pool and resident lifecycle '
              'were chosen explicitly to test the page-count and reuse hypothesis; all negative controls are '
              'included in the main report.', '', '## Cost and verification', '',
              'The dense ownership table costs **64 KiB MRAM for a 32 MiB payload heap**, or 8 B per '
              '4096 B payload page. Its initialization and updates are timed. Comparisons keep payload '
              'capacity equal and report the additional metadata separately; this is extra physical MRAM '
              'consumption. The list heads/patterns add 24 B WRAM per tasklet/class, or 3072 B at 16 tasklets.', '',
              f"Verified **{sum(len(v) for v in all_data.values())} declared performance configurations**, "
              'including failures, with exact generated-source hash checks; **16 native sanitizer/property '
              'combinations**; and **27 simulator validation cases**, including 16-tasklet concurrency, '
              'resident pools, and allocation failure before any free. Payloads, non-overlap, page states, '
              'ownership, large-block free, OOM recovery, and complete coalescing are covered. '
              f"Maximum reported WRAM heap endpoint: {audit['maximum_wram_end_address']} / 65536. "
              'Inherited IRAM and worst-case stack-depth checks remain outside this simulator validation.', '',
              '## Reproduce', '', '```bash',
              'python3 pim-reproduction/combined/test.py',
              'python3 pim-reproduction/combined/sweep.py --preset main --tag combined-v2 --jobs 2',
              'python3 pim-reproduction/combined/sweep.py --preset resident --tag resident-v2 --jobs 2',
              'python3 pim-reproduction/combined/sweep.py --preset heldout --tag heldout-v2 --jobs 2',
              'pim-reproduction/.venv/bin/python pim-reproduction/combined/analyze.py --tag combined-v2',
              'pim-reproduction/.venv/bin/python pim-reproduction/combined/analyze.py --tag resident-v2',
              'pim-reproduction/.venv/bin/python pim-reproduction/combined/analyze.py --tag heldout-v2',
              'pim-reproduction/.venv/bin/python pim-reproduction/combined/summarize.py', '```', '',
              'Detailed tables, p99 latencies, memory accounting, exact sources, binary hashes, event CSVs, '
              'and declared matrices are retained under `analysis/` and `results/`. See [README](README.md) '
              'for all modes, workload definitions, and validation commands.', '']
    (HERE / 'SUMMARY.md').write_text('\n'.join(lines))
    (HERE / 'verification.json').write_text(json.dumps(audit, indent=2)+'\n')
    os.environ.setdefault('MPLCONFIGDIR', str(HERE / '.mplconfig'))
    import matplotlib
    matplotlib.use('Agg')
    import matplotlib.pyplot as plt
    import numpy as np
    x = np.arange(len(rows)); width=.36
    fig, ax = plt.subplots(figsize=(11.5, 5.4))
    bars1 = ax.bar(x-width/2, [r['fixed_indexed_speedup'] for r in rows], width, label='Indexed + fixed-2', color='#217b9b')
    bars2 = ax.bar(x+width/2, [r['combined_speedup'] for r in rows], width, label='Indexed + adaptive (combined)', color='#dc813e')
    ax.bar_label(bars1, fmt='%.2fx', padding=3, fontsize=9)
    ax.bar_label(bars2, fmt='%.2fx', padding=3, fontsize=9)
    ax.axhline(1, color='#333333', lw=1)
    ax.set_xticks(x, [r['name'].replace(', ', '\n') for r in rows], fontsize=9)
    ax.set_ylabel('Work speedup over scan + fixed-2 (higher is better)')
    ax.set_title('Indexed pages help sustained churn; adaptation helps changing demand')
    ax.legend(frameon=False, loc='upper right'); ax.grid(axis='y', alpha=.2); ax.set_axisbelow(True)
    ax.set_ylim(0, max(r['fixed_indexed_speedup'] for r in rows)*1.25)
    fig.text(.06,.015,'Identical completed traces · Single-DPU simulation · Failed capacity cases reported separately',fontsize=9,color='#555555')
    fig.tight_layout(rect=[0,.045,1,1])
    for suffix in ('png','svg'): fig.savefig(HERE / f'headline-speedups.{suffix}', dpi=160)
    plt.close(fig)
    print(f"Verified {sum(len(v) for v in all_data.values())} performance configurations; report: {HERE/'SUMMARY.md'}")


if __name__ == '__main__': main()
