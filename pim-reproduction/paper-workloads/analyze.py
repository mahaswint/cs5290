#!/usr/bin/env python3
"""Require complete frozen matrices, audit inputs, and report every workload."""
import argparse
import csv
import hashlib
import importlib.util
import json
import os
from collections import defaultdict
from pathlib import Path
import run

HERE = Path(__file__).resolve().parent
LABELS = {'larson':'Larson worker', 'pim-list':'PIM-malloc linked blocks',
          'pim-array':'PIM-malloc growing arrays', 'faimgraph':'faimGraph-style updates',
          'prim-sel':'PrIM SEL control'}


def digest(data):
    return hashlib.sha256(data).hexdigest()


def key(configuration):
    return json.dumps(configuration, sort_keys=True)


def load_matrix(tag, preset):
    folder = HERE/'results'/tag
    declared = json.loads((folder/f'matrix-{preset}.json').read_text())
    expected = []
    for configuration in declared:
        a = run.options().parse_args([])
        vars(a).update(configuration, tag=tag)
        expected.append(vars(a))
    assert len({key(c) for c in expected}) == len(expected)
    found = {}
    for path in folder.glob('*/summary.json'):
        if '.superseded-' in str(path):
            continue
        result = json.loads(path.read_text())
        identity = key(result['configuration'])
        assert identity not in found, f'Duplicate: {path}'
        found[identity] = (path, result)
    assert set(found) == {key(c) for c in expected}, f'Incomplete or extra records in {tag}: {len(found)}/{len(expected)}'
    audited = []
    for configuration in expected:
        path, result = found[key(configuration)]
        a = argparse.Namespace(**configuration)
        source, manifest = run.source_for(a)
        manifest = json.loads(json.dumps(manifest))
        assert digest(source.encode()) == result['source_sha256'] == digest((path.parent/'task.c').read_bytes()), path
        assert manifest == result['trace'] == json.loads((path.parent/'trace.json').read_text()), path
        command = json.loads((path.parent/'command.json').read_text())
        assert command['configuration'] == configuration and command['source_sha256'] == result['source_sha256']
        assert result['simulator_sha256'] == digest((run.core.ADAPTIVE/'runtime/build/uPIMulator').read_bytes())
        assert result['correctness_passed'] and result['outcome'] == 'completed'
        assert result['totals']['failures'] == result['backend']['failed_attempts'] == result['backend']['used_bytes'] == 0
        assert result['totals']['alloc_calls'] == result['totals']['free_calls'] == result['totals']['successes']
        assert result['wram']['heap_end_address'] <= 65536
        events = list(csv.DictReader((path.parent/'events.csv').open()))
        for phase, count in [('total',1),('init',1),('work',a.threads),('drain',a.threads)]:
            rows = [e for e in events if e['kind']==phase]
            assert len(rows) == count and len({e['thread'] for e in rows}) == count
        for e in events:
            assert int(e['end_cycle'])-int(e['begin_cycle']) == int(e['cycles']) > 0
        windows = [e for e in events if e['kind']=='work']
        span = (max(int(e['end_cycle']) for e in windows)-min(int(e['begin_cycle']) for e in windows))/350
        assert span == result['work_makespan_us']
        result['_path'] = str(path.relative_to(HERE))
        result['_preset'] = preset
        audited.append(result)
    return audited


def rows_for(results):
    rows = []
    for d in results:
        c=d['configuration'];m=d['metrics'];w=d['work_metrics']
        snapshots=list(csv.DictReader((HERE/d['_path']).parent.joinpath('snapshots.csv').open()))
        work_end=next(s for s in snapshots if s['phase']=='1')
        rows.append(dict(preset=d['_preset'],workload=c['workload'],threads=c['threads'],seed=c['seed'],
            mode=c['mode'],policy=c['policy'],keep=c['keep'],work_ms=d['work_makespan_us']/1000,
            total_ms=m['total']['mean_us']/1000,init_ms=m['init']['mean_us']/1000,
            work_allocations=w['alloc']['samples'],work_frees=w['free']['samples'],
            work_alloc_mean_us=w['alloc']['mean_us'],work_alloc_p99_us=w['alloc']['p99_us'],
            work_free_mean_us=w['free']['mean_us'],work_free_p99_us=w['free']['p99_us'],
            all_allocations=d['totals']['alloc_calls'],all_refills=d['totals']['refills'],
            all_returns=d['totals']['returns'],all_retained_hits=d['totals']['retained_hits'],
            peak_payload_bytes=d['backend']['peak_bytes'],owner_mram_bytes=d['owner_table_mram_bytes'],
            peak_payload_plus_owner_bytes=d['peak_payload_plus_owner_bytes'],
            work_end_requested_bytes=int(work_end['requested_bytes']),work_end_slot_bytes=int(work_end['slot_bytes']),
            work_end_reserved_bytes=int(work_end['reserved_bytes']),work_end_empty_bytes=int(work_end['empty_bytes']),
            wram_heap_end=d['wram']['heap_end_address'],source_sha256=d['source_sha256'],
            trace_sha256=d['trace']['trace_sha256'],simulator_sha256=d['simulator_sha256'],summary=d['_path']))
    return rows


def write_csv(path, rows):
    with path.open('w',newline='') as stream:
        writer=csv.DictWriter(stream,fieldnames=list(rows[0]));writer.writeheader();writer.writerows(rows)


def comparisons(rows, preset):
    groups=defaultdict(list)
    for r in rows:
        if r['preset']==preset:groups[(r['workload'],r['threads'],r['seed'])].append(r)
    answer=[]
    for (workload,threads,seed), group in groups.items():
        assert len({r['trace_sha256'] for r in group})==1
        assert len({(r['all_allocations'],r['work_allocations'],r['work_frees']) for r in group})==1
        scans=[r for r in group if r['mode']=='scan']
        scan2=next(r for r in scans if r['keep']==2)
        best=min(scans,key=lambda r:r['work_ms']);best_total=min(scans,key=lambda r:r['total_ms'])
        fixed=next(r for r in group if r['mode']=='indexed' and r['policy']=='fixed')
        adaptive=next(r for r in group if r['mode']=='indexed' and r['policy']=='adaptive')
        answer.append(dict(workload=workload,threads=threads,seed=seed,
            best_scan_keep=best['keep'],scan_work_ms=best['work_ms'],indexed_fixed_work_ms=fixed['work_ms'],
            indexed_adaptive_work_ms=adaptive['work_ms'],fixed_work_speedup=best['work_ms']/fixed['work_ms'],
            adaptive_work_speedup=best['work_ms']/adaptive['work_ms'],
            adaptive_work_vs_scan2=scan2['work_ms']/adaptive['work_ms'],
            adaptive_vs_indexed_fixed=fixed['work_ms']/adaptive['work_ms'],
            total_baseline_keep=best_total['keep'],scan_total_ms=best_total['total_ms'],
            indexed_fixed_total_ms=fixed['total_ms'],indexed_adaptive_total_ms=adaptive['total_ms'],
            fixed_total_speedup=best_total['total_ms']/fixed['total_ms'],
            adaptive_total_speedup=best_total['total_ms']/adaptive['total_ms']))
    return answer


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--main-tag',default='papers-v3')
    p.add_argument('--heldout-tag',default='heldout-v3')
    p.add_argument('--ablation-tag',default='ablation-v3')
    p.add_argument('--validation-tag',default='validation-v3')
    p.add_argument('--gate-tag',default='gate-v3')
    a=p.parse_args()
    main_runs=load_matrix(a.main_tag,'main')
    heldout=load_matrix(a.heldout_tag,'heldout')
    ablation=load_matrix(a.ablation_tag,'ablation')
    validation=load_matrix(a.validation_tag,'validation')
    native=json.loads((HERE/'validation/checks.json').read_text())
    assert len(native)==40 and all(r['passed'] for r in native)
    evidence=json.loads((HERE/'validation/native-evidence.json').read_text())
    spec=importlib.util.spec_from_file_location('paper_native',HERE/'test.py')
    harness=importlib.util.module_from_spec(spec);spec.loader.exec_module(harness)
    expected_native={(w,m,p) for w in run.KINDS for m in run.core.MODES for p in ['fixed','adaptive']}
    assert len(evidence)==40
    assert {(r['workload'],r['mode'],r['policy']) for r in native}=={(r['workload'],r['mode'],r['policy']) for r in evidence}==expected_native
    for r in evidence:
        c=run.options().parse_args([])
        vars(c).update(workload=r['workload'],mode=r['mode'],policy=r['policy'],threads=1,
            objects=64,operations=128,items=256,rounds=2,fixture=True,heap_kib=1024,validate=True)
        source,_=run.source_for(c)
        assert digest((harness.PRELUDE+source+harness.POSTLUDE).encode())==r['generated_native_source_sha256']
    rng=json.loads((HERE/'validation/larson-rng.json').read_text())
    generator=run.trace.LRan2(rng['seed'])
    assert rng['matched'] and rng['reference_output']==[generator.next() for _ in range(rng['samples'])]
    gate_paths=list((HERE/'results'/a.gate_tag).glob('*/summary.json'))
    assert len(gate_paths)==1
    gate=json.loads(gate_paths[0].read_text())
    same=next(d for d in main_runs if d['source_sha256']==gate['source_sha256'])
    assert gate['correctness_passed'] and gate['work_makespan_us']==same['work_makespan_us']
    assert gate['metrics']==same['metrics']
    rows=rows_for(main_runs+heldout+ablation)
    main_comp=comparisons(rows,'main');held_comp=comparisons(rows,'heldout')
    output=HERE/'analysis';output.mkdir(exist_ok=True)
    write_csv(output/'all-runs.csv',rows)
    write_csv(output/'main-comparisons.csv',main_comp)
    write_csv(output/'heldout-comparisons.csv',held_comp)
    audit=dict(performance_configurations=len(rows),dpu_validation_configurations=len(validation),
        native_sanitizer_checks=len(native),native_source_hashes_checked=True,source_hashes_checked=True,trace_and_dataset_hashes_checked=True,
        upstream_c_rng_outputs_matched=rng['samples'],production_gate_matches_main=True,
        complete_declared_matrices=True,all_successful=True,
        simulator_sha256=main_runs[0]['simulator_sha256'],
        allocator_sha256=main_runs[0]['trace']['allocator_sha256'],
        max_wram_heap_end=max(d['wram']['heap_end_address'] for d in main_runs+heldout+ablation+validation),
        result_paths=[d['_path'] for d in main_runs+heldout+ablation+validation],
        excluded_development_tags=['papers-v1','papers-v2','validation-v1','validation-v2','validation-final'],
        limitation='Deterministic single-DPU simulator; scaled kernel ports, no claim of original published speedups.')
    (output/'audit.json').write_text(json.dumps(audit,indent=2)+'\n')
    text=['# Paper-derived workload results','',
        'All timings are simulated single-DPU milliseconds at 350 MHz. These are scaled workload ports; see [SOURCES.md](SOURCES.md) for fidelity limits. Speedups below 1 are regressions. No allocator threshold was changed for this experiment.','',
        '## Main comparison','',
        'The baseline is the **faster of scan fixed-1 and scan fixed-2**, selected separately for work and total. Fixed indexed uses keep=2. Adaptive indexed is the complete combined idea. All variants receive identical inputs and complete all offered work.','',
        '| Workload | Tasklets | Best scan work ms | Indexed fixed ms | Indexed adaptive ms | Fixed work speedup | Adaptive work speedup | Adaptive total speedup |',
        '| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |']
    for r in main_comp:
        text.append(f"| {LABELS[r['workload']]} | {r['threads']} | {r['scan_work_ms']:.3f} | {r['indexed_fixed_work_ms']:.3f} | {r['indexed_adaptive_work_ms']:.3f} | {r['fixed_work_speedup']:.3f}x | {r['adaptive_work_speedup']:.3f}x | {r['adaptive_total_speedup']:.3f}x |")
    best=max(main_comp,key=lambda r:r['adaptive_work_speedup'])
    text+=['',
        f"The strongest combined result is **{best['adaptive_work_speedup']:.2f}x work / {best['adaptive_total_speedup']:.2f}x total** for {LABELS[best['workload']]} at {best['threads']} tasklet(s). Larson replacement and graph churn are the useful follow-up workloads: their persistent live sets expose long page scans and scattered reusable holes. The fixed-retention ablation below attributes the gain to lists and ownership lookup. Fixed indexed retention is faster than adaptive on Larson and churn; these results do not establish an additional speed benefit from adaptation on those workloads.",
        '', '![All workload speedups](analysis/speedups.png)','',
        'Work includes application operations, MRAM trace reads, payload DMA and synchronization. It excludes preload/warmup and final checking/destruction. Total includes those phases and ownership-map initialization; host transfers and simulation wall time are excluded. Larson adds a fixed pool per tasklet; graph and selection keep total input fixed. These tasklet comparisons therefore mix weak and strong scaling.','',
        '## Second seed (9173)','',
        'Settings and input sizes are unchanged. Only scan fixed-2 is tested in this matrix. This measures sensitivity to a different deterministic operation ordering and graph sample, not timing variance.','',
        '| Workload | Tasklets | Scan work ms | Indexed fixed ms | Indexed adaptive ms | Fixed speedup | Adaptive speedup | Adaptive total speedup |',
        '| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |']
    for r in held_comp:
        text.append(f"| {LABELS[r['workload']]} | {r['threads']} | {r['scan_work_ms']:.3f} | {r['indexed_fixed_work_ms']:.3f} | {r['indexed_adaptive_work_ms']:.3f} | {r['fixed_work_speedup']:.3f}x | {r['adaptive_work_speedup']:.3f}x | {r['adaptive_total_speedup']:.3f}x |")
    text+=['','## Mechanism ablation','',
        'One tasklet, seed 4141, fixed-2 retention in every row. This separates lists from lookup and holds the retention controller constant.','',
        '| Workload | Mode | Work ms | Speedup over scan | Timed alloc mean / p99 µs | Timed free mean / p99 µs | All-run refills |',
        '| --- | --- | ---: | ---: | ---: | ---: | ---: |']
    for workload in ['larson','faimgraph']:
        group=[r for r in rows if r['preset']=='ablation' and r['workload']==workload]
        scan=next(r for r in group if r['mode']=='scan')
        assert len({(r['trace_sha256'],r['all_allocations'],r['work_allocations'],r['work_frees']) for r in group})==1
        for r in group:
            text.append(f"| {LABELS[workload]} | {r['mode']} | {r['work_ms']:.3f} | {scan['work_ms']/r['work_ms']:.3f}x | {r['work_alloc_mean_us']:.2f} / {r['work_alloc_p99_us']:.2f} | {r['work_free_mean_us']:.2f} / {r['work_free_p99_us']:.2f} | {r['all_refills']} |")
    text+=['','## Memory and input accounting','',
        'All main runs have a 32 MiB payload heap. Lookup/indexed modes additionally consume 64 KiB MRAM for the owner map; this is extra physical memory. List descriptors add 24 B per tasklet/class. The table gives allocator payload high-water marks, excluding preallocated application tables/traces; those arrays are identical across variants. Sum-of-tasklet peak slot counters are not a simultaneous global peak.','',
        '| Workload | Tasklets | Scan fixed-2 payload peak KiB | Indexed fixed payload peak KiB | Indexed adaptive payload peak KiB | Timed alloc / free calls |',
        '| --- | ---: | ---: | ---: | ---: | ---: |']
    for c in main_comp:
        group=[r for r in rows if r['preset']=='main' and r['workload']==c['workload'] and r['threads']==c['threads']]
        scan=next(r for r in group if r['mode']=='scan' and r['keep']==2)
        fixed=next(r for r in group if r['mode']=='indexed' and r['policy']=='fixed')
        adaptive=next(r for r in group if r['mode']=='indexed' and r['policy']=='adaptive')
        text.append(f"| {LABELS[c['workload']]} | {c['threads']} | {scan['peak_payload_bytes']/1024:.1f} | {fixed['peak_payload_bytes']/1024:.1f} | {adaptive['peak_payload_bytes']/1024:.1f} | {adaptive['work_allocations']} / {adaptive['work_frees']} |")
    text+=['','Graph inputs and actual final degree-driven allocation sizes (main seed):','']
    for d in main_runs:
        c=d['configuration'];tr=d['trace']
        if c['threads']!=1 or c['mode']!='scan' or c['keep']!=2 or 'expected_degrees' not in tr:continue
        if c['workload']=='pim-array':
            sizes=sorted({max(64,1<<((4*degree-1).bit_length())) for degree in tr['expected_degrees'] if degree})
            shape=f"final array capacities {', '.join(map(str,sizes))} B (growth also traverses smaller powers of two)"
        else:shape='256 B adjacency blocks'
        singles=sum(degree==1 for degree in tr['expected_degrees'])
        text.append(f"- {LABELS[c['workload']]}: {tr['sampled_undirected_edges']} undirected edges, {tr['vertices']} vertices, maximum degree {max(tr['expected_degrees'])}, {singles} degree-one vertices; {shape}.")
    text+=['','## Verification and limits','',
        f"The analyzer verified all **{len(rows)} declared performance runs**, **{len(validation)} DPU correctness runs** and **{len(native)} native ASan/UBSan/oracle checks**. Every performance run completed without allocation failures and ended with zero live payload, balanced calls and complete buddy coalescing. Exact generated sources, trace/dataset manifests and simulator hashes match the saved artifacts. Maximum reported WRAM heap endpoint: {audit['max_wram_heap_end']} / 65,536 B. The separate production-size gate also passed.",
        '',
        'Graphs have independent degree/checksum references on the DPU; native fixtures additionally enforce exact neighbor membership and uniqueness. Larson checks live byte counts and payload tags; SEL checks every output element. Native interval oracles detect live-allocation overlap and wrong-size frees. Performance runs disable extra invariant traversals inside the kernel. Workload assertions and DMA remain included.',
        '',
        'These are deterministic simulator samples, so repeated identical runs would not provide independent statistical confidence intervals. Second-seed results test input sensitivity. Same-owner frees exclude inter-tasklet object migration; no original concurrent Larson or full GPU faimGraph claim is supported. The reduced graph does not reproduce full-dataset allocation ranges. Only SEL is ported from PrIM. Worst-case stack depth, IRAM feasibility, physical hardware and multi-DPU transfers remain outside this evaluation.',
        '',
        'The 3,072-edge reservoir sample is sparse: most vertices have one neighbor, and the largest degree is below one 62-neighbor block. This makes deletion/reinsertion replace many individual adjacency blocks across hundreds of allocator pages. That behavior strongly exercises page discovery and ownership lookup; it does not establish the same speedup for a full, denser graph. Multi-block adjacency is checked separately by the star fixture.',
        '',
        'Development attempts with compiler-duplicated phase labels are excluded by explicit tag selection. Final runs use dedicated phase functions and require exact probe counts as well as successful simulator exit. Earlier results are retained for audit.',
        '',
        '## Artifacts','',
        '- [Complete timings, latencies and memory CSV](analysis/all-runs.csv)',
        '- [Main speedup comparisons](analysis/main-comparisons.csv)',
        '- [Second-seed comparisons](analysis/heldout-comparisons.csv)',
        '- [Audit and exact accepted paths](analysis/audit.json)',
        '- [Source mapping and limitations](SOURCES.md)',
        '- [Reproduction commands](README.md)',
        '']
    (HERE/'RESULTS.md').write_text('\n'.join(text))
    os.environ.setdefault('MPLCONFIGDIR',str(HERE/'.mplconfig'))
    import matplotlib
    matplotlib.use('Agg')
    import matplotlib.pyplot as plt
    import numpy as np
    plt.rcParams.update({'font.size':10,'axes.spines.top':False,'axes.spines.right':False})
    fig,axes=plt.subplots(1,2,figsize=(12,5.8),sharey=True,sharex=True)
    colors=['#2563a6','#d48228']
    for ax,threads in zip(axes,[1,4]):
        group=[r for r in main_comp if r['threads']==threads]
        y=np.arange(len(group))
        for i,(field,label) in enumerate([('fixed_work_speedup','Indexed + fixed'),('adaptive_work_speedup','Indexed + adaptive')]):
            positions=y+(i-.5)*.30
            values=[r[field] for r in group]
            ax.scatter(values,positions,label=label,color=colors[i],marker='o' if i==0 else 's',s=45,zorder=3)
            for value,position in zip(values,positions):
                ax.annotate(f'{value:.2f}×',(value,position),xytext=(7,0),textcoords='offset points',
                    va='center',fontsize=9,color=colors[i])
        ax.axvline(1,color='#333333',linewidth=1,linestyle='--')
        ax.set_yticks(y,['Larson worker','PIM linked blocks','PIM growing arrays','faimGraph-style updates','PrIM SEL control'])
        ax.set_title(f'{threads} tasklet'+('s' if threads>1 else ''))
        ax.set_xscale('log');ax.grid(axis='x',alpha=.17);ax.set_axisbelow(True)
        ax.set_xlabel('Work speedup (log scale)')
    ymax=max(r[k] for r in main_comp for k in ['fixed_work_speedup','adaptive_work_speedup'])
    axes[0].set_xlim(.70,max(2,ymax*1.65));axes[0].set_ylim(4.6,-.6)
    ticks=[v for v in [1,2,4,8,16,32,64] if v<=max(2,ymax*1.65)]
    axes[0].set_xticks(ticks,[f'{v}×' for v in ticks]);axes[0].minorticks_off();axes[1].minorticks_off()
    handles,labels=axes[0].get_legend_handles_labels()
    fig.legend(handles,labels,loc='upper center',bbox_to_anchor=(.57,.94),ncol=2,frameon=False)
    fig.suptitle('Paper-derived DPU workload ports · comparison with faster scan fixed-1 / fixed-2',fontsize=12,y=.99)
    fig.text(.5,.015,'350 MHz simulation · identical operations within each comparison · values < 1 are slower',ha='center',fontsize=9)
    fig.tight_layout(rect=(0,.05,1,.88))
    fig.savefig(output/'speedups.png',dpi=180)
    fig.savefig(output/'speedups.svg')
    print(json.dumps(audit,indent=2))


if __name__=='__main__':main()
