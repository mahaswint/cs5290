#!/usr/bin/env python3
"""Audit all stages and separate incremental pinning gain from earlier ideas."""
import argparse
import csv
import hashlib
import json
import re
from collections import Counter
import run

HERE=run.HERE
original=run.module('pinned_original_analysis',HERE.parent/'paper-workloads/analyze.py')
original.HERE=HERE;original.run=run
native=run.module('pinned_native_tests',HERE/'test.py')
LABELS={'larson':'Larson 8–4095 B','cloud-cache':'CloudPhysics write cache','larson-wide':'Larson 8–8191 B',
        'graph-churn':'Gowalla graph churn','prim-sel':'PrIM SEL control'}
SHORT={'baseline':'Scan fixed-2','indexed-adaptive64':'Indexed adaptive-64','indexed-fixed2':'Indexed fixed-2',
       'slab-adaptive64':'+ slab, adaptive','slab-fixed2':'+ slab, fixed-2',
       'slab-pinned-adaptive64':'+ slab + pin, adaptive','slab-pinned-fixed2':'+ slab + pin, fixed-2'}


def request_profile(result):
    a=argparse.Namespace(**result['configuration'])
    if a.workload=='cloud-cache':
        tr=result['trace']
        return dict(all_sizes={int(k):v for k,v in tr['request_sizes'].items()},
                    work_sizes={int(k):v for k,v in tr['work_request_sizes'].items()},
                    all_frees=sum(c['allocations'] for c in tr['counts']),
                    work_frees=sum(c['work_frees'] for c in tr['counts']))
    if a.workload=='prim-sel':return dict(all_sizes={},work_sizes={},all_frees=0,work_frees=0)
    a.workload={'larson-wide':'larson','graph-churn':'faimgraph'}.get(a.workload,a.workload)
    header,_=run.paper.trace.make_trace(a)
    body=re.search(r'paper_ops\[\d+\] = \{\n(.*?)\n\};',header,re.S).group(1)
    ops=[tuple(map(int,re.findall(r'(\d+)U',line))) for line in body.splitlines()]
    def values(name):return list(map(int,re.findall(r'(\d+)U',re.search(name+r'\[\d+\] = \{([^}]*)\}',header).group(1))))
    offsets=values('paper_offsets');starts=values('paper_work_starts')
    all_sizes=Counter();work_sizes=Counter();degrees=Counter();all_frees=work_frees=0
    for t in range(a.threads):
        for i in range(offsets[t],offsets[t+1]):
            kind,index,value,tag=ops[i];timed=i>=starts[t];size=freed=0
            if a.workload=='larson':
                if kind in [0,2]:size=value
                freed=int(kind==2)
            elif kind==3:
                assert index%a.threads==t
                if degrees[index]%62==0:size=256
                degrees[index]+=1
            else:
                assert kind==4 and degrees[index]>0
                freed=int(degrees[index]%62==1);degrees[index]-=1
            if size:
                all_sizes[size]+=1
                if timed:work_sizes[size]+=1
            all_frees+=freed
            if timed:work_frees+=freed
    all_frees+=a.threads*a.objects if a.workload=='larson' else sum((d+61)//62 for d in degrees.values())
    return dict(all_sizes=dict(sorted(all_sizes.items())),work_sizes=dict(sorted(work_sizes.items())),
                all_frees=all_frees,work_frees=work_frees)


def counters(folder):
    values={}
    for line in (folder/'log.txt').read_text().splitlines():
        m=re.fullmatch(r'(RowBuffer|Logic)\[0_0_0\]_(\w+): (\d+)',line)
        if m:values[m[1]+'_'+m[2]]=int(m[3])
    return dict(dram_reads=values['RowBuffer_num_reads'],dram_writes=values['RowBuffer_num_writes'],
        dram_commands=values['RowBuffer_num_reads']+values['RowBuffer_num_writes'],
        dram_read_bytes=values['RowBuffer_read_bytes'],dram_write_bytes=values['RowBuffer_write_bytes'],
        dram_bytes=values['RowBuffer_read_bytes']+values['RowBuffer_write_bytes'],
        instructions=values['Logic_num_instructions'])


def audit_native():
    records=json.loads((HERE/'validation/checks.json').read_text())
    assert len(records)==47 and Counter(r['kind'] for r in records)==dict(backend=6,layered=6,application=35)
    body=(HERE/'backend_test.c').read_text().replace('#include "reference_backend.inc"',native.renamed(run.BACKEND,'Ref'))
    body=body.replace('#include "pinned_backend.inc"',native.renamed(run.PINNED,'Pin'))
    for r in records:
        assert r['passed'];path=HERE/'validation'/(r['name']+'.c')
        if r['kind']=='application':
            source,_=run.source_for(argparse.Namespace(**r['configuration']))
            source=native.native.PRELUDE+source+native.native.POSTLUDE
        elif r['kind']=='backend':
            source=f"#define BUDDY_PIN_LEVELS {r['pin_levels']}\n#define TEST_BUFFER {r['buffer_bytes']}\n"+body
        else:
            slab,policy,budget=r['slab_kib'],r['policy'],r['budget_pages']
            a=run.normalize(run.options().parse_args(['--variant','slab-pinned-'+('fixed2' if policy=='fixed' else 'adaptive64')]))
            a.slab_kib=slab
            source=(HERE.parent/'stacked/host_test.c').read_text().replace('#include "allocator.c"',run.allocator_source(a))
            definitions=dict(NR_TASKLETS=4,SLAB_BYTES=slab*1024,POLICY=run.core.shared.POLICIES[policy],
                BUDDY_PIN_LEVELS=11,LISTS=1,LOOKUP=1,KEEP_PAGES=2,MAX_KEEP=2,EMPTY_BUDGET=budget,
                DECAY_OPS=64,PRESSURE_PERCENT=75)
            source=''.join(f'#define {k} {v}\n' for k,v in definitions.items())+source
        assert original.digest(source.encode())==r['source_sha256']==original.digest(path.read_bytes())
        assert original.digest(path.with_suffix('.log').read_bytes())==r['log_sha256']
    return records


def main():
    results=original.load_matrix('pinned-v1','main')
    held=original.load_matrix('heldout-pinned-v1','heldout')
    validation=original.load_matrix('validation-pinned-v1','validation')
    assert len(results)==35 and len(held)==8 and len(validation)==35
    evidence=audit_native()
    origin=json.loads((HERE/'origin_manifest.json').read_text())
    assert original.digest(run.BACKEND.encode())==origin['base_sha256']
    all_results=results+held;rows=original.rows_for(all_results);profiles={}
    for d,r in zip(all_results,rows):
        c=d['configuration'];r.update(counters((HERE/d['_path']).parent))
        r.update(variant=c['variant'],pin_levels=c['pin_levels'],pinned_metadata_bytes=d['trace']['pinned_metadata_bytes'],
            slab_kib=c['slab_kib'],decay_ops=c['decay_ops'],budget_bytes=c['budget_pages']*4096,
            backend_allocations=d['backend']['allocations'],backend_frees=d['backend']['frees'],
            allocator_sha256=d['trace']['allocator_sha256'],backend_sha256=d['trace']['backend_sha256'])
        profile=request_profile(d)
        assert sum(profile['all_sizes'].values())==r['all_allocations']==profile['all_frees']
        assert sum(profile['work_sizes'].values())==r['work_allocations']
        assert profile['work_frees']==r['work_frees']
        if c['workload']=='cloud-cache':assert r['work_end_requested_bytes']==d['trace']['final_requested_bytes']
        r['work_slab_eligible_allocations']=sum(n for s,n in profile['work_sizes'].items() if 2049<=s<=4096)
        r['work_direct_bypass_allocations']=sum(n for s,n in profile['work_sizes'].items() if s>4096)
        profile_key=(r['preset'],r['workload'])
        assert profiles.setdefault(profile_key,profile)==profile
        assert c['decay_ops']==64 and c['budget_pages']==64
    by_path={d['_path']:d for d in all_results}
    feature_fields=['allocator_sha256','backend_sha256','pin_levels','pinned_metadata_bytes']
    for preset,workload in profiles:
        group=[r for r in rows if (r['preset'],r['workload'])==(preset,workload)]
        assert len(group)==(7 if preset=='main' else 4)
        cleaned=[{k:v for k,v in by_path[r['summary']]['trace'].items() if k not in feature_fields} for r in group]
        assert all(t==cleaned[0] for t in cleaned)
    pairs=[]
    for r in rows:
        if not r['pin_levels']:continue
        unpinned=r['variant'].replace('slab-pinned-','slab-')
        b=next(b for b in rows if (b['preset'],b['workload'],b['variant'])==(r['preset'],r['workload'],unpinned))
        cs=[{k:v for k,v in by_path[x['summary']]['configuration'].items() if k not in ['variant','pin_levels']} for x in [b,r]]
        assert cs[0]==cs[1]
        p=dict(preset=r['preset'],workload=r['workload'],policy=r['policy'],
            incremental_work_speedup=b['work_ms']/r['work_ms'],incremental_total_speedup=b['total_ms']/r['total_ms'],
            dram_byte_reduction_percent=100*(1-r['dram_bytes']/b['dram_bytes']),
            wram_endpoint_difference=r['wram_heap_end']-b['wram_heap_end'])
        for prefix,x in [('unpinned',b),('pinned',r)]:
            for field in ['work_ms','total_ms','dram_commands','dram_bytes','instructions','backend_allocations',
                          'peak_payload_bytes','work_end_reserved_bytes','work_end_empty_bytes','wram_heap_end','summary']:
                p[prefix+'_'+field]=x[field]
        pairs.append(p)
    output=HERE/'analysis';output.mkdir(exist_ok=True)
    original.write_csv(output/'all-runs.csv',rows);original.write_csv(output/'incremental-pinning.csv',pairs)
    (output/'request-profiles.json').write_text(json.dumps([dict(preset=k[0],workload=k[1],**v) for k,v in profiles.items()],indent=2)+'\n')
    audit=dict(main_configurations=len(results),heldout_configurations=len(held),dpu_validation_configurations=len(validation),
        native_sanitizer_cases=len(evidence),complete_declared_matrices=True,same_offered_work_in_all_comparisons=True,
        source_trace_simulator_native_hashes_match=True,original_backend_unchanged=True,
        all_completed_without_failures=True,pin_levels=11,pinned_prefix_bytes=528,
        max_wram_heap_end=max(d['wram']['heap_end_address'] for d in all_results+validation),
        simulator_sha256=results[0]['simulator_sha256'],
        result_paths=[d['_path'] for d in all_results+validation],
        counter_log_sha256={d['_path']:original.digest((HERE/d['_path']).parent.joinpath('log.txt').read_bytes()) for d in all_results})
    (output/'audit.json').write_text(json.dumps(audit,indent=2)+'\n')
    write_report(rows,pairs,audit)
    print(json.dumps({k:v for k,v in audit.items() if k not in ['result_paths','counter_log_sha256']},indent=2))


def write_report(rows,pairs,audit):
    main=[r for r in rows if r['preset']=='main']
    text=['# Full allocator stack with pinned buddy metadata','',
        'All requested stages were run on five cases at **four tasklets**, using the same offered work within each case. These are deterministic 350 MHz single-DPU workload kernels/ports, not hardware or complete original applications. [README.md](README.md) defines provenance, workload changes and the WRAM/MRAM consistency design.','',
        '**Baseline:** maintained scan fixed-2, without indexing, slabs or pinning. All stages use a 32 MiB payload heap and 256 KiB empty-byte budgets/tasklet. Indexed stages add 64 KiB MRAM ownership lookup. Slabs use 64 KiB spans for 2049–4096 B objects. Every adaptive stage checks every **64 allocation plus free operations**, with maximum two retained empty spans/class. Pinning keeps the aligned 528 B upper metadata prefix in shared WRAM; lower metadata remains streamed.','',
        'Work includes application/trace DMA and synchronization, excluding warmup/preload and final checking/drain. Total includes initialization, warmup/preload, work, output checking and drain, ending before passive final accounting. Compilation, simulation wall time, storage/network service time and host transfers are excluded.','',
        '## Work runtime: every requested stage','',
        'Milliseconds; lower is faster. Abbreviations: IA/IF = indexed adaptive-64/fixed-2; SA/SF = indexed + slab with that policy; PA/PF = indexed + slab + pinned metadata with that policy.','',
        '| Workload | Baseline | IA | IF | SA | SF | PA | PF |',
        '| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |']
    for w in run.WORKLOADS:
        group={r['variant']:r for r in main if r['workload']==w}
        text.append('| '+LABELS[w]+' | '+' | '.join(f"{group[v]['work_ms']:.3f}" for v in run.VARIANTS)+' |')
    text+=['','## Work speedup relative to scan fixed-2','',
        'Values below 1 are regressions. This table measures the complete progression; the following incremental table isolates pinning.','',
        '| Workload | IA | IF | SA | SF | PA | PF |',
        '| --- | ---: | ---: | ---: | ---: | ---: | ---: |']
    for w in run.WORKLOADS:
        g={r['variant']:r for r in main if r['workload']==w};b=g['baseline']['work_ms']
        text.append('| '+LABELS[w]+' | '+' | '.join(f"{b/g[v]['work_ms']:.3f}x" for v in run.VARIANTS if v!='baseline')+' |')
    text+=['','![Full comparison](analysis/full-stack.png)','', '## Incremental gain from pinned metadata','',
        'Each comparison changes only the pinned backend and its required WRAM. Slab size, indexing, retention policy, interval, inputs and heap/budget are fixed. DRAM bytes are full-execution counts; WRAM loads/stores are not counted.','',
        '| Workload | Policy | Slab work ms | Slab + pin ms | Pin work speedup | Pin total speedup | DRAM byte reduction | Extra WRAM endpoint B |',
        '| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: |']
    for p in pairs:
        if p['preset']!='main':continue
        text.append(f"| {LABELS[p['workload']]} | {p['policy']} | {p['unpinned_work_ms']:.3f} | {p['pinned_work_ms']:.3f} | {p['incremental_work_speedup']:.3f}x | {p['incremental_total_speedup']:.3f}x | {p['dram_byte_reduction_percent']:+.2f}% | {p['wram_endpoint_difference']} |")
    text+=['','![Incremental pinning gain](analysis/incremental-pinning.png)','',
        'Pinning can only accelerate the buddy accesses that remain after frontend reuse. A slab benefit, an indexed lookup benefit and an adaptive-policy benefit are separate claims. The 8–8191 B Larson configuration includes direct-bypass requests; graph churn uses 256 B objects; selection has no allocator calls during work. These different paths are retained rather than selecting only favorable traces.','',
        '## Total runtime','', '| Workload | Baseline | IA | IF | SA | SF | PA | PF |',
        '| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |']
    for w in run.WORKLOADS:
        g={r['variant']:r for r in main if r['workload']==w}
        text.append('| '+LABELS[w]+' | '+' | '.join(f"{g[v]['total_ms']:.3f}" for v in run.VARIANTS)+' |')
    text+=['','## Full-execution memory commands and storage costs','',
        'DRAM read/write counts include final checks, exclude WRAM accesses, instruction fetches, row activation/precharge and host transfers, and are not source-level DMA-call counts. Pinning moves upper metadata traffic into WRAM; it does not eliminate all memory accesses. MRAM retains the same buddy metadata reservation/shadow. Payload peak excludes the owner map, buddy metadata and identical application tables/traces.','',
        '| Workload | Variant | DRAM reads+writes | DRAM MiB | Backend allocations | Peak payload KiB | WRAM heap endpoint B |',
        '| --- | --- | ---: | ---: | ---: | ---: | ---: |']
    for r in main:
        text.append(f"| {LABELS[r['workload']]} | {SHORT[r['variant']]} | {r['dram_commands']:,} | {r['dram_bytes']/2**20:.3f} | {r['backend_allocations']:,} | {r['peak_payload_bytes']/1024:.1f} | {r['wram_heap_end']:,} |")
    text+=['','## Second seed / trace window','',
        'Only slab versus slab+pinning is repeated: Larson seed 9173 and CloudPhysics offset 4096. All other parameters remain unchanged; both retention policies are reported.','',
        '| Workload | Policy | Unpinned work ms | Pinned work ms | Pin work speedup | Pin total speedup |',
        '| --- | --- | ---: | ---: | ---: | ---: |']
    for p in pairs:
        if p['preset']=='heldout':text.append(f"| {LABELS[p['workload']]} | {p['policy']} | {p['unpinned_work_ms']:.3f} | {p['pinned_work_ms']:.3f} | {p['incremental_work_speedup']:.3f}x | {p['incremental_total_speedup']:.3f}x |")
    text+=['','## Validation and limits','',
        f"All **{audit['main_configurations']+audit['heldout_configurations']} performance configurations**, **{audit['dpu_validation_configurations']} DPU invariant-validation cases**, and **{audit['native_sanitizer_cases']} native sanitizer executables passed**. Native coverage includes six differential backend programs, each covering three heap sizes, six layered allocator programs and all 35 workload/stage combinations. Every accepted run has exact source/trace/simulator hashes, identical offered requests within its comparisons, no allocation failures, balanced calls, complete coalescing and zero final live/retained payload. The canonical backend hash is unchanged.",
        '', f"Maximum reported WRAM endpoint: **{audit['max_wram_heap_end']:,} / 65,536 B**. The pinned metadata allocation is shared; it is not multiplied by tasklets. Endpoint checks do not establish worst-case stack depth, IRAM fit or physical hardware throughput. Deterministic runs do not supply independent confidence intervals. All workloads here use ample payload capacity; existing slab fragmentation and sparse-pinning costs remain applicable.",
        '', 'Native checkpoint tests compare every tree state and raw flushed metadata bytes, catching stale overlapping windows. The MRAM pinned-prefix shadow is not read by normal allocation; explicit `syncPinnedMetadata` is available to a quiescent caller. It is not required for live allocator consistency and is not invoked on every request.',
        '', '- [All timings, latencies, traffic, memory and accepted run paths](analysis/all-runs.csv)',
        '- [Incremental pinning comparisons](analysis/incremental-pinning.csv)',
        '- [Request-size distributions](analysis/request-profiles.json)',
        '- [Complete audit](analysis/audit.json)',
        '- [Design and reproduction commands](README.md)', '']
    (HERE/'RESULTS.md').write_text('\n'.join(text))


if __name__=='__main__':main()
