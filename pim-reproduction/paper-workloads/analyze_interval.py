#!/usr/bin/env python3
"""Audit the paired 64/128-operation experiment and extract DRAM counters."""
import argparse
import csv
import importlib.util
import json
import os
import re
from pathlib import Path
import analyze as original
import run

HERE=Path(__file__).resolve().parent


def counters(result):
    folder=(HERE/result['_path']).parent
    values={}
    for line in (folder/'log.txt').read_text().splitlines():
        match=re.fullmatch(r'(RowBuffer|Logic)\[0_0_0\]_(\w+): (\d+)',line)
        if match:
            component,name,value=match.groups()
            values[component+'_'+name]=int(value)
    required=['RowBuffer_'+s for s in ['num_reads','num_writes','read_bytes','write_bytes','num_activations','num_precharges']]
    required+=['Logic_num_instructions']
    assert all(s in values for s in required),folder
    events=list(csv.DictReader((folder/'events.csv').open()))
    work={int(e['thread']):(int(e['begin_cycle']),int(e['end_cycle'])) for e in events if e['kind']=='work'}
    calls=[e for e in events if e['kind'] in ['alloc','free']]
    calls.sort(key=lambda e:int(e['begin_cycle']))
    sequence={t:0 for t in work};work_checks=all_checks=0
    c=result['configuration']
    for event in calls:
        t=int(event['thread']);sequence[t]+=1
        if c['policy']=='adaptive' and sequence[t]%c['decay_ops']==0:
            all_checks+=1
            if work[t][0]<=int(event['begin_cycle']) and int(event['end_cycle'])<=work[t][1]:work_checks+=1
    assert len(calls)==result['totals']['alloc_calls']+result['totals']['free_calls']
    if c['policy']=='adaptive':
        assert all_checks==sum((s['alloc_calls']+s['free_calls'])//c['decay_ops'] for s in csv_ints(folder/'stats.csv'))
    row=dict(workload=c['workload'],threads=c['threads'],seed=c['seed'],tag=c['tag'],mode=c['mode'],policy=c['policy'],
        check_interval_ops=c['decay_ops'],
        dram_read_commands=values['RowBuffer_num_reads'],dram_write_commands=values['RowBuffer_num_writes'],
        dram_access_commands=values['RowBuffer_num_reads']+values['RowBuffer_num_writes'],
        dram_read_bytes=values['RowBuffer_read_bytes'],dram_write_bytes=values['RowBuffer_write_bytes'],
        dram_read_write_bytes=values['RowBuffer_read_bytes']+values['RowBuffer_write_bytes'],
        row_activations=values['RowBuffer_num_activations'],row_precharges=values['RowBuffer_num_precharges'],
        full_execution_instructions=values['Logic_num_instructions'],
        allocator_calls=len(calls),work_allocator_calls=sum(result['work_metrics'][k]['samples'] for k in ['alloc','free']),
        expected_full_execution_periodic_checks=all_checks,expected_work_periodic_checks=work_checks,
        all_refills=result['totals']['refills'],all_returns=result['totals']['returns'],
        retained_hits=result['totals']['retained_hits'],peak_payload_bytes=result['backend']['peak_bytes'],
        owner_mram_bytes=result['owner_table_mram_bytes'],work_ms=result['work_makespan_us']/1000,
        total_ms=result['metrics']['total']['mean_us']/1000,
        source_sha256=result['source_sha256'],trace_sha256=result['trace']['trace_sha256'],summary=result['_path'])
    return row


def csv_ints(path):
    with path.open() as stream:return [{k:int(v) for k,v in row.items()} for row in csv.DictReader(stream)]


def same_input(c):
    return tuple((k,v) for k,v in sorted(c.items()) if k not in ['tag','mode','policy','decay_ops','keep'])


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--tag',default='interval128-v1')
    p.add_argument('--validation-tag',default='validation-interval128-v1')
    a=p.parse_args()
    previous=original.load_matrix('papers-v3','main')+original.load_matrix('heldout-v3','heldout')
    ablation=original.load_matrix('ablation-v3','ablation')
    updated=original.load_matrix(a.tag,'interval')
    validation=original.load_matrix(a.validation_tag,'interval-validation')
    assert len(updated)==14 and len(validation)==5
    spec=importlib.util.spec_from_file_location('paper_native_interval',HERE/'test.py')
    native=importlib.util.module_from_spec(spec);spec.loader.exec_module(native)
    evidence=json.loads((HERE/'validation/interval-128/native-evidence.json').read_text())
    assert len(evidence)==5 and {r['workload'] for r in evidence}==set(run.KINDS)
    for record in evidence:
        assert record['passed'] and record['mode']=='indexed' and record['policy']=='adaptive' and record['decay_ops']==128
        args=run.options().parse_args([])
        vars(args).update(workload=record['workload'],mode='indexed',policy='adaptive',threads=1,decay_ops=128,
            objects=64,operations=128,items=256,rounds=2,fixture=True,heap_kib=1024,validate=True)
        source,_=run.source_for(args)
        assert original.digest((native.PRELUDE+source+native.POSTLUDE).encode())==record['generated_native_source_sha256']
    all_results=previous+ablation+updated
    rows=[counters(r) for r in all_results]
    by_path={r['summary']:r for r in rows}
    paired=[]
    for result in updated:
        c=result['configuration']
        assert c['mode']=='indexed' and c['policy']=='adaptive' and c['decay_ops']==128
        group=[d for d in previous if same_input(d['configuration'])==same_input(c)]
        old=next(d for d in group if d['configuration']['mode']=='indexed' and d['configuration']['policy']=='adaptive')
        fixed=next(d for d in group if d['configuration']['mode']=='indexed' and d['configuration']['policy']=='fixed')
        assert old['configuration']['decay_ops']==fixed['configuration']['decay_ops']==64
        assert old['trace']==result['trace']==fixed['trace']
        source=(HERE/result['_path']).parent.joinpath('task.c').read_text()
        assert source.replace('#define DECAY_OPS 128\n','#define DECAY_OPS 64\n',1)==(HERE/old['_path']).parent.joinpath('task.c').read_text()
        r=by_path[result['_path']];o=by_path[old['_path']];f=by_path[fixed['_path']]
        assert (r['allocator_calls'],r['work_allocator_calls'])==(o['allocator_calls'],o['work_allocator_calls'])==(f['allocator_calls'],f['work_allocator_calls'])
        item=dict(workload=c['workload'],threads=c['threads'],seed=c['seed'],
            fixed_work_ms=f['work_ms'],adaptive64_work_ms=o['work_ms'],adaptive128_work_ms=r['work_ms'],
            work_runtime_reduction_percent=100*(o['work_ms']-r['work_ms'])/o['work_ms'],
            adaptive128_runtime_vs_fixed_percent=100*(r['work_ms']/f['work_ms']-1),
            fixed_total_ms=f['total_ms'],adaptive64_total_ms=o['total_ms'],adaptive128_total_ms=r['total_ms'],
            total_runtime_reduction_percent=100*(o['total_ms']-r['total_ms'])/o['total_ms'])
        for prefix,row in [('fixed',f),('adaptive64',o),('adaptive128',r)]:
            for field in ['dram_read_commands','dram_write_commands','dram_access_commands','dram_read_write_bytes',
                          'full_execution_instructions','all_refills','allocator_calls',
                          'expected_full_execution_periodic_checks','expected_work_periodic_checks']:
                item[prefix+'_'+field]=row[field]
        paired.append(item)
    output=HERE/'analysis';output.mkdir(exist_ok=True)
    original.write_csv(output/'memory-accesses.csv',rows)
    original.write_csv(output/'interval128-comparisons.csv',paired)
    audit=dict(new_performance_configurations=len(updated),dpu_validation_configurations=len(validation),
        native_validation_checks=len(evidence),original_performance_configurations_reaudited=len(previous)+len(ablation),
        only_generated_source_change='DECAY_OPS 64 -> 128',all_sources_traces_and_native_sources_match=True,
        all_completed_without_failures=True,all_memory_counter_logs_sha256={r['_path']:original.digest((HERE/r['_path']).parent.joinpath('log.txt').read_bytes()) for r in all_results},
        measurement_scope='DRAM read/write commands from RowBuffer counters over the full DPU execution, including final checks; excludes WRAM load/stores, instruction fetches, row activation/precharge commands and host transfers.',
        check_counts='Derived exactly from allocator-call event order and interval; periodic checks are triggered by alloc+free, not DRAM requests.',
        limitation='DECAY_OPS couples periodic sweep cadence with idle-decay time in operations; this is not an isolated overhead-only ablation.')
    (output/'interval128-audit.json').write_text(json.dumps(audit,indent=2)+'\n')
    text=['# Adaptive interval 64 versus 128','',
        'This paired experiment changes only `DECAY_OPS` from 64 to 128. It preserves traces, allocator geometry, retention caps/budget and pressure threshold. The original 64-operation results remain unchanged. Check intervals count **allocations plus frees per tasklet**, not allocations alone and not memory accesses.','',
        '**Measured outcome:** on the main seed, most allocation workloads reduce work runtime by 0.30–0.83%; four-tasklet linked-block insertion is 1.30% slower and SEL is unchanged. The second seed shows 0.44–0.60% reductions for Larson/churn. Indexed fixed-2 remains faster than adaptive 128 on Larson and churn. Total time is not uniformly improved: main-seed four-tasklet Larson and churn regress by 0.09% and 0.13%, respectively.','',
        'DRAM byte totals and backend refill counts are unchanged for most pairs. Main-seed Larson records four more read/write commands and 32 more bytes at each tasklet count, with unchanged refills. Second-seed four-tasklet Larson saves 3,504 bytes and one refill; the remaining pairs have unchanged DRAM counters. The small timing reductions are consistent with fewer controller instructions; traffic volume alone does not explain them. The periodic sweep mostly examines WRAM metadata when no empty pages need trimming.','',
        'A counter/branch still runs on every allocator call. The eight-class periodic sweep runs less often. Idle targets also decay more slowly in terms of operations; changing this parameter changes policy behavior as well as overhead.','',
        '## Work runtime','',
        'Positive reduction means 128 is faster than 64. These are paired deterministic simulator measurements at 350 MHz. Fixed uses indexed fixed-2 in every row.','',
        '| Workload | Tasklets | Seed | Fixed-2 ms | Adaptive 64 ms | Adaptive 128 ms | Runtime reduction 64→128 | 128 runtime vs fixed |',
        '| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |']
    for r in paired:
        text.append(f"| {original.LABELS[r['workload']]} | {r['threads']} | {r['seed']} | {r['fixed_work_ms']:.3f} | {r['adaptive64_work_ms']:.3f} | {r['adaptive128_work_ms']:.3f} | {r['work_runtime_reduction_percent']:+.2f}% | {r['adaptive128_runtime_vs_fixed_percent']:+.2f}% |")
    text+=['','![Runtime relative to fixed-2](analysis/interval128.png)','',
        '## Total runtime','',
        'Includes initialization, preload/warmup, work, output checks and drain; excludes compilation, host transfers and simulation wall time.','',
        '| Workload | Tasklets | Seed | Fixed-2 ms | Adaptive 64 ms | Adaptive 128 ms | Runtime reduction 64→128 |',
        '| --- | ---: | ---: | ---: | ---: | ---: | ---: |']
    for r in paired:
        text.append(f"| {original.LABELS[r['workload']]} | {r['threads']} | {r['seed']} | {r['fixed_total_ms']:.3f} | {r['adaptive64_total_ms']:.3f} | {r['adaptive128_total_ms']:.3f} | {r['total_runtime_reduction_percent']:+.2f}% |")
    text+=['','## Memory access counts','',
        '**Scope:** read + write commands counted at the simulator DRAM row buffer over the full DPU execution, including final correctness checks. These are not C-level `mram_read` calls or all CPU memory instructions. A 256 B DMA normally splits into 32 commands at the configured 8 B granularity; row boundaries/tails can create smaller commands. Row activation and precharge are reported separately in the CSV. WRAM accesses, instruction fetches and host transfers are excluded.','',
        '| Workload | Tasklets | Fixed-2 reads+writes | Adaptive 64 reads+writes | Adaptive 128 reads+writes |',
        '| --- | ---: | ---: | ---: | ---: |']
    for r in paired:
        if r['seed']!=4141:continue
        text.append(f"| {original.LABELS[r['workload']]} | {r['threads']} | {r['fixed_dram_access_commands']:,} | {r['adaptive64_dram_access_commands']:,} | {r['adaptive128_dram_access_commands']:,} |")
    for threads in [1,4]:
        group=[r for r in paired if r['seed']==4141 and r['threads']==threads]
        totals=[sum(r[prefix+'_dram_access_commands'] for r in group) for prefix in ['fixed','adaptive64','adaptive128']]
        text.append(f"| **Sum of all five workloads** | **{threads}** | **{totals[0]:,}** | **{totals[1]:,}** | **{totals[2]:,}** |")
    text+=['','Counts can differ slightly while total bytes stay identical because DMA alignment/row boundaries split commands differently. Byte counts and instructions for all 74 old/new performance configurations are retained in [memory-accesses.csv](analysis/memory-accesses.csv), including both scan controls and the ablations.','',
        '## Why fixed-2 is not a two-page live cache','',
        '`KEEP_PAGES=2` caps **completely empty** pages retained per size class. Live and partially occupied pages remain available without that two-page cap. There is also a shared four-empty-page budget per tasklet. When all existing slots are full, either policy obtains another 4 KiB page from the buddy backend, then serves multiple objects from that page.','',
        'A 256 B class page holds floor((4096−24)/256) = 15 objects. The 5,372-vertex churn sample initially allocates 5,372 such blocks, requiring 359 pages. Indexed fixed-2 records 359 refills over the entire execution, so its 3,397 timed allocations reuse the pages obtained during preload: **zero new backend page refills during timed work**. Fixed-2 is not bypassing the frontend after the first 30 objects.','',
        'For Larson at one tasklet, fixed-2 records 135 backend refills across 4,608 allocations (2.93%), not one backend allocation per object. See `all_refills` and `allocator_calls` in the CSV for every configuration. Requests larger than the frontend classes would bypass the small-object cache; this is separate from the retention limit.','',
        'Adaptive can help release idle-class pages or respond to pressure. Its current maximum is also two retained pages per class. The paper-derived traces mostly have steady demand and small heaps in use relative to the available 32 MiB, so fixed-2 can remain competitive. A claim that fixed-2 is poor needs a demonstrated workload/capacity result.','',
        '## Validation and reproduction','',
        'All 14 new performance configurations, five DPU fixture configurations and five native ASan/UBSan/oracle checks passed. Sources and traces are audited against the original runs; each adaptive pair differs only in the leading interval definition. No allocator source body or simulator executable changed. Periodic-check counts in the CSV are derived from per-tasklet allocator-call order and verified against call totals.','',
        '```sh',
        'python3 pim-reproduction/paper-workloads/test.py --decay-ops 128 --mode indexed --policy adaptive --output-tag interval-128',
        'python3 pim-reproduction/paper-workloads/sweep.py --preset interval-validation --tag validation-interval128-v1 --jobs 2',
        'python3 pim-reproduction/paper-workloads/sweep.py --preset interval --tag interval128-v1 --jobs 4',
        'pim-reproduction/.venv/bin/python pim-reproduction/paper-workloads/analyze_interval.py',
        '```','',
        '- [Paired results, counts and instruction CSV](analysis/interval128-comparisons.csv)',
        '- [Complete access counts](analysis/memory-accesses.csv)',
        '- [Audit and counter-log hashes](analysis/interval128-audit.json)',
        '']
    (HERE/'INTERVAL128.md').write_text('\n'.join(text))
    os.environ.setdefault('MPLCONFIGDIR',str(HERE/'.mplconfig'))
    import matplotlib
    matplotlib.use('Agg')
    import matplotlib.pyplot as plt
    import numpy as np
    fig,axes=plt.subplots(1,2,figsize=(12,5.7),sharex=True,sharey=True)
    for ax,t in zip(axes,[1,4]):
        group=[r for r in paired if r['seed']==4141 and r['threads']==t]
        y=np.arange(len(group))
        for i,(field,label,color) in enumerate([('adaptive64_work_ms','Adaptive, interval 64','#d48228'),('adaptive128_work_ms','Adaptive, interval 128','#2563a6')]):
            ratios=[r[field]/r['fixed_work_ms'] for r in group];positions=y+(i-.5)*.30
            ax.scatter(ratios,positions,label=label,color=color,marker='s' if i==0 else 'o')
            for value,position in zip(ratios,positions):ax.annotate(f'{value:.3f}',(value,position),xytext=(6,0),textcoords='offset points',va='center',fontsize=9,color=color)
        ax.axvline(1,color='#333333',linestyle='--',linewidth=1)
        ax.set_yticks(y,['Larson','PIM linked blocks','PIM growing arrays','faimGraph-style churn','PrIM SEL control'])
        ax.set_title(f'{t} tasklet'+('s' if t>1 else ''));ax.grid(axis='x',alpha=.2)
        ax.set_xlabel('Work runtime / indexed fixed-2 (lower is faster)')
        ax.spines[['top','right']].set_visible(False)
    ratios=[r[field]/r['fixed_work_ms'] for r in paired if r['seed']==4141 for field in ['adaptive64_work_ms','adaptive128_work_ms']]
    axes[0].set_xlim(min(.96,min(ratios)-.03),max(1.12,max(ratios)+.045));axes[0].set_ylim(4.6,-.6)
    handles,labels=axes[0].get_legend_handles_labels()
    fig.legend(handles,labels,loc='upper center',bbox_to_anchor=(.57,.94),ncol=2,frameon=False)
    fig.suptitle('Changing only the adaptive periodic interval: 64 → 128 allocator operations',y=.99,fontsize=12)
    fig.text(.5,.015,'350 MHz single-DPU simulation · seed 4141 · reference line = indexed fixed-2',ha='center',fontsize=9)
    fig.tight_layout(rect=(0,.05,1,.88));fig.savefig(output/'interval128.png',dpi=180);fig.savefig(output/'interval128.svg')
    print(json.dumps({k:v for k,v in audit.items() if k!='all_memory_counter_logs_sha256'},indent=2))


if __name__=='__main__':main()
