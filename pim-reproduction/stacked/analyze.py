#!/usr/bin/env python3
"""Require complete matrices and compare slabs with the indexed baseline."""
import argparse
import csv
import hashlib
import json
import os
import re
from pathlib import Path
import run

HERE=Path(__file__).resolve().parent
def digest(value):return hashlib.sha256(value).hexdigest()
def identity(c):return json.dumps(c,sort_keys=True)


def load(tag,preset):
    folder=HERE/'results'/tag
    configs=[]
    for c in json.loads((folder/f'matrix-{preset}.json').read_text()):
        a=run.options().parse_args([]);vars(a).update(c,tag=tag);configs.append(vars(a))
    found={}
    for path in folder.glob('*/summary.json'):
        if '.superseded-' in str(path):continue
        d=json.loads(path.read_text());key=identity(d['configuration']);assert key not in found
        found[key]=(path,d)
    assert set(found)=={identity(c) for c in configs},f'Incomplete matrix {tag}: {len(found)}/{len(configs)}'
    results=[]
    for c in configs:
        path,d=found[identity(c)];source,trace=run.source_for(argparse.Namespace(**c))
        assert digest(source.encode())==d['source_sha256']==digest((path.parent/'task.c').read_bytes())
        assert trace==d['trace']==json.loads((path.parent/'trace.json').read_text())
        assert d['simulator_sha256']==digest((run.core.ADAPTIVE/'runtime/build/uPIMulator').read_bytes())
        assert d['correctness_passed'] and d['outcome']=='completed' and not d['totals']['failures'] and not d['backend']['used_bytes']
        assert d['totals']['successes']==d['totals']['free_calls']==d['totals']['alloc_calls']
        assert d['backend']['allocations']==d['backend']['frees'] and d['wram']['heap_end_address']<=65536
        events=list(csv.DictReader((path.parent/'events.csv').open()))
        for phase,n in [('total',1),('init',1),('work',c['threads']),('drain',c['threads'])]:
            phase_rows=[e for e in events if e['kind']==phase]
            assert len(phase_rows)==len({e['thread'] for e in phase_rows})==n
        d['_folder']=path.parent;results.append(d)
    return results


def row(d):
    c=d['configuration'];folder=d['_folder'];counts={}
    for line in (folder/'log.txt').read_text().splitlines():
        m=re.fullmatch(r'(RowBuffer|Logic)\[0_0_0\]_(\w+): (\d+)',line)
        if m:counts[m[1]+'_'+m[2]]=int(m[3])
    snapshots=list(csv.DictReader((folder/'snapshots.csv').open()))
    end=next(s for s in snapshots if s['phase']=='1')
    return dict(workload=c['workload'],threads=c['threads'],size=c['size'],policy=c['policy'],slab_kib=c['slab_kib'],
        objects=c['objects'],operations=c['operations'],rounds=c['rounds'],budget_bytes=c['budget_pages']*4096,
        decay_ops=c['decay_ops'],work_ms=d['work_makespan_us']/1000,total_ms=d['metrics']['total']['mean_us']/1000,
        alloc_calls=d['totals']['alloc_calls'],backend_calls=d['backend']['allocations'],frontend_refills=d['totals']['refills'],
        peak_payload_kib=d['backend']['peak_bytes']/1024,owner_mram_kib=d['owner_table_mram_bytes']/1024,
        work_end_requested_kib=int(end['requested_bytes'])/1024,work_end_reserved_kib=int(end['reserved_bytes'])/1024,
        work_end_empty_kib=int(end['empty_bytes'])/1024,wram_heap_end=d['wram']['heap_end_address'],
        work_alloc_mean_us=d['work_metrics']['alloc']['mean_us'],work_alloc_p99_us=d['work_metrics']['alloc']['p99_us'],
        work_free_mean_us=d['work_metrics']['free']['mean_us'],work_free_p99_us=d['work_metrics']['free']['p99_us'],
        dram_read_commands=counts['RowBuffer_num_reads'],dram_write_commands=counts['RowBuffer_num_writes'],
        dram_read_write_bytes=counts['RowBuffer_read_bytes']+counts['RowBuffer_write_bytes'],
        instructions=counts['Logic_num_instructions'],source_sha256=d['source_sha256'],trace_sha256=d['trace']['trace_sha256'],
        summary=str((folder/'summary.json').relative_to(HERE)))


def write_csv(path,rows):
    with path.open('w',newline='') as stream:
        w=csv.DictWriter(stream,fieldnames=list(rows[0]));w.writeheader();w.writerows(rows)


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--tag',default='stacked-v1');p.add_argument('--validation-tag',default='validation-v1');a=p.parse_args()
    performance=load(a.tag,'main');validation=load(a.validation_tag,'validation')
    assert len(performance)==28 and len(validation)==14
    checks=json.loads((HERE/'validation/checks.json').read_text());assert len(checks)==16 and all(c['passed'] for c in checks)
    native=(HERE/'host_test.c').read_text().replace('#include "allocator.c"',run.allocator_source())
    for c in checks:
        definitions=dict(NR_TASKLETS=4,SLAB_BYTES=c['slab_kib']*1024,POLICY=run.core.shared.POLICIES[c['policy']],
            LISTS=1,LOOKUP=1,KEEP_PAGES=2,MAX_KEEP=2,EMPTY_BUDGET=c['budget_pages'],DECAY_OPS=128,PRESSURE_PERCENT=75)
        source=''.join(f'#define {k} {v}\n' for k,v in definitions.items())+native
        assert digest(source.encode())==c['source_sha256']
    origin=json.loads((HERE/'origin_manifest.json').read_text())
    assert origin['base_sha256']==digest((HERE.parent/'combined/allocator.c').read_bytes())
    rows=[row(d) for d in performance]
    def group(r):return tuple(r[k] for k in ['workload','threads','size','policy','objects','operations','rounds','budget_bytes','decay_ops'])
    for r in rows:
        base=next(s for s in rows if s['slab_kib']==0 and group(s)==group(r))
        assert r['trace_sha256']==base['trace_sha256'] and r['alloc_calls']==base['alloc_calls']
        r['work_speedup']=base['work_ms']/r['work_ms'];r['total_speedup']=base['total_ms']/r['total_ms']
    output=HERE/'analysis';output.mkdir(exist_ok=True);write_csv(output/'comparisons.csv',rows)
    audit=dict(performance_configurations=len(performance),dpu_validation_configurations=len(validation),native_checks=len(checks),
        all_successful=True,all_source_trace_and_simulator_hashes_match=True,byte_budgets_equal_within_comparisons=True,
        base_source_unchanged=True,max_wram_heap_end=max(d['wram']['heap_end_address'] for d in performance+validation),
        simulator_sha256=performance[0]['simulator_sha256'],
        paths=[str((d['_folder']/'summary.json').relative_to(HERE)) for d in performance+validation],
        excluded_prototypes=['gate-v1','gate-v2'],
        limitation='Synthetic single-DPU simulator workloads; memory endpoints checked, worst-case stacks/IRAM/physical deployment not validated.')
    (output/'audit.json').write_text(json.dumps(audit,indent=2)+'\n')
    text=['# Slabs + indexed + fixed/adaptive: results','',
        'All comparisons already use indexed lists and ownership lookup. The added mechanism is the 2049–4096 B slab class; fixed and adaptive are separate retention policies. These are synthetic 350 MHz single-DPU simulations, not full paper applications or hardware measurements.','',
        'Every row uses a 32 MiB payload heap, an additional 64 KiB MRAM owner map, and the same **256 KiB empty-byte budget per tasklet**. The adaptive interval is 128 alloc/free operations. A fixed/adaptive class target counts spans, while the budget is charged in physical bytes. This is an explicit larger byte budget than the earlier 16 KiB experiment; only like-budget controls below are used for ratios.','',
        '## Dense 4 KiB bursts','',
        'Each tasklet allocates and frees 32 objects for four rounds. Speedup is relative to **indexed without slabs using the same retention policy**. Values below 1 are regressions. All offered requests succeed.','',
        '| Slab KiB | Policy | Tasklets | Work ms | Work speedup | Total speedup | Backend allocations | Peak payload KiB | End empty KiB |',
        '| ---: | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |']
    for r in rows:
        if r['workload']!='burst' or r['size']!=4096:continue
        text.append(f"| {r['slab_kib']} | {r['policy']} | {r['threads']} | {r['work_ms']:.3f} | {r['work_speedup']:.3f}x | {r['total_speedup']:.3f}x | {r['backend_calls']} | {r['peak_payload_kib']:.1f} | {r['work_end_empty_kib']:.1f} |")
    text+=['','![Dense comparison](analysis/dense.png)','',
        '## Persistent, sparse and bypass controls','',
        'The four-tasklet persistent pool preloads 64 live 4 KiB objects per tasklet and performs 128 random victim replacements. Sparse retains indices 0 and 16 from 32 objects, then drains empty spans before its work-end snapshot; its reserved bytes therefore expose live-span pinning. The 256 B and 8192 B controls use two burst rounds and stay outside the added class.','',
        '| Workload | Size B | Slab KiB | Policy | Tasklets | Work ms | Work speedup | Total speedup | Work-end requested / reserved KiB |',
        '| --- | ---: | ---: | --- | ---: | ---: | ---: | ---: | ---: |']
    for r in rows:
        if r['workload']=='burst' and r['size']==4096:continue
        text.append(f"| {r['workload']} | {r['size']} | {r['slab_kib']} | {r['policy']} | {r['threads']} | {r['work_ms']:.3f} | {r['work_speedup']:.3f}x | {r['total_speedup']:.3f}x | {r['work_end_requested_kib']:.1f} / {r['work_end_reserved_kib']:.1f} |")
    text+=['','## Correctness, costs and limits','',
        f"All **{len(checks)} native sanitizer/property cases**, **{len(validation)} DPU validation cases** and **{len(performance)} performance configurations** passed. Sources, traces and simulator hashes match the saved artifacts. DPU validation includes 16 tasklets, all geometries/policies, mixed boundaries, lower budgets, persistent reuse and sparse survivors. Maximum WRAM endpoint: {audit['max_wram_heap_end']} / 65,536 B.",
        '',
        'Native tests check all payload bytes, independent live intervals, every registered/cleared owner leaf, retention units, wrong-owner/interior/double frees, exact capacity and complete coalescing. A 64 KiB heap holds 16 direct objects, or 12/14/15 slab objects because of header/tail overhead. A fragmentation case has enough aggregate free memory but no contiguous span; allocation fails nonfatally. No speedup is claimed for these capacity failures.',
        '',
        'Sparse survivors pin 128 KiB with two 64 KiB slabs versus 8 KiB for two direct 4 KiB objects after empty spans are drained. Retaining empty spans additionally consumes memory between dense bursts; the table and CSV report it rather than hiding the allocation/reclamation tradeoff. The ninth class costs 56 B extra WRAM per tasklet. The ownership table is the same size in all variants and its initialization/updates are timed.',
        '',
        'Work is concurrent makespan including payload sentinels and application DMA. Total additionally includes allocator initialization, preload, final output checks and drain. Passive counters in the CSV record full-execution DRAM read/write commands/bytes and instructions, including final accounting; they exclude WRAM accesses and host transfers. One DMA may split into several DRAM commands. Timings are deterministic samples, with no independent confidence intervals claimed.',
        '',
        'The returned object alignment is 8 B; slabs are relative to the actual buddy payload base. Frees must come from the allocating tasklet and accept bases, not interior pointers. Geometry is static for a run; no live object moves. Physical hardware throughput, full-application speedups, worst-case stack depth and IRAM fit remain unverified.',
        '',
        '- [Complete timings, latencies, bytes and memory CSV](analysis/comparisons.csv)',
        '- [Audit and exact run paths](analysis/audit.json)',
        '- [Implementation, accounting and reproduction](README.md)',
        '']
    (HERE/'RESULTS.md').write_text('\n'.join(text))
    os.environ.setdefault('MPLCONFIGDIR',str(HERE/'.mplconfig'))
    import matplotlib
    matplotlib.use('Agg')
    import matplotlib.pyplot as plt
    import numpy as np
    fig,axes=plt.subplots(1,2,figsize=(11,4.8),sharey=True)
    for ax,threads in zip(axes,[1,4]):
        x=np.arange(4)
        for i,(policy,color) in enumerate([('fixed','#2563a6'),('adaptive','#d48228')]):
            group=[next(r for r in rows if r['workload']=='burst' and r['size']==4096 and r['threads']==threads and r['policy']==policy and r['slab_kib']==slab) for slab in [0,16,32,64]]
            bars=ax.bar(x+(i-.5)*.34,[r['work_speedup'] for r in group],.34,label=policy,color=color)
            ax.bar_label(bars,fmt='%.2f',fontsize=9,padding=3)
        ax.set_xticks(x,['Disabled','16 KiB','32 KiB','64 KiB']);ax.axhline(1,color='#444444',linestyle='--',linewidth=1)
        ax.set_title(f'{threads} tasklet'+('s' if threads>1 else ''));ax.grid(axis='y',alpha=.2);ax.set_axisbelow(True)
        ax.spines[['top','right']].set_visible(False)
    axes[0].set_ylabel('Work speedup vs indexed without slabs')
    ymax=max(r['work_speedup'] for r in rows if r['workload']=='burst' and r['size']==4096)
    axes[0].set_ylim(0,ymax*1.22);axes[1].legend(frameon=False)
    fig.suptitle('Adding 4 KiB object slabs to the indexed allocator',fontsize=13)
    fig.text(.5,.015,'32 objects × 4 rounds/tasklet · equal 256 KiB empty-byte budgets · 350 MHz simulation',ha='center',fontsize=9)
    fig.tight_layout(rect=(0,.05,1,.94));fig.savefig(output/'dense.png',dpi=180);fig.savefig(output/'dense.svg')
    print(json.dumps({k:v for k,v in audit.items() if k!='paths'},indent=2))


if __name__=='__main__':main()
