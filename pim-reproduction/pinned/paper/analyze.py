#!/usr/bin/env python3
"""Audit every declared paper case and report regressions alongside speedups."""
import argparse
from collections import Counter
import hashlib
import json
import re
import run

HERE=run.HERE
original=run.stack.module('paper_extension_analysis',HERE.parent.parent/'paper-workloads/analyze.py')
original.HERE=HERE;original.run=run
native=run.stack.module('paper_extension_harness',HERE.parent.parent/'paper-workloads/test.py')
sweep=run.stack.module('paper_extension_matrix',HERE/'sweep.py')
LABELS={'pim-list':'Gowalla linked blocks','pim-array':'Gowalla growing arrays',
    'micro-32':'32 B allocation','micro-256':'256 B allocation','micro-4096':'4 KiB allocation',
    'kv-growth':'KV growth replay (allocator only)'}


def counters(folder):
    values={}
    for line in (folder/'log.txt').read_text().splitlines():
        m=re.fullmatch(r'(RowBuffer|Logic)\[0_0_0\]_(\w+): (\d+)',line)
        if m:values[m[1]+'_'+m[2]]=int(m[3])
    reads=values['RowBuffer_num_reads'];writes=values['RowBuffer_num_writes']
    rb=values['RowBuffer_read_bytes'];wb=values['RowBuffer_write_bytes']
    return dict(dram_reads=reads,dram_writes=writes,dram_commands=reads+writes,
        dram_read_bytes=rb,dram_write_bytes=wb,dram_bytes=rb+wb,instructions=values['Logic_num_instructions'])


def profile(d):
    a=argparse.Namespace(**d['configuration'])
    if a.workload not in ['pim-list','pim-array']:
        s=d['trace']['schedule'];size=s['bytes']
        return dict(all_sizes={size:a.threads*s['count_per_tasklet']},
            work_sizes={size:a.threads*s['timed_per_tasklet']},
            all_frees=a.threads*s['count_per_tasklet'],work_frees=0)
    header,_=run.stack.paper.trace.make_trace(a)
    body=re.search(r'paper_ops\[\d+\] = \{\n(.*?)\n\};',header,re.S).group(1)
    ops=[tuple(map(int,re.findall(r'(\d+)U',line))) for line in body.splitlines()]
    def values(name):return list(map(int,re.findall(r'(\d+)U',re.search(name+r'\[\d+\] = \{([^}]*)\}',header).group(1))))
    offsets=values('paper_offsets');starts=values('paper_work_starts')
    degrees=Counter();capacities=Counter();all_sizes=Counter();work_sizes=Counter()
    all_frees=work_frees=0
    for t in range(a.threads):
        for i in range(offsets[t],offsets[t+1]):
            kind,u,v,tag=ops[i];assert kind==3 and u%a.threads==t
            timed=i>=starts[t];size=freed=0
            if a.workload=='pim-list':
                if degrees[u]%62==0:size=256
            elif 4*(degrees[u]+1)>capacities[u]:
                freed=int(capacities[u]>0)
                size=capacities[u]*2 if capacities[u] else 64
                capacities[u]=size
            degrees[u]+=1
            if size:
                all_sizes[size]+=1
                if timed:work_sizes[size]+=1
            all_frees+=freed
            if timed:work_frees+=freed
    all_frees+=sum((d+61)//62 for d in degrees.values()) if a.workload=='pim-list' else len(capacities)
    return dict(all_sizes=dict(sorted(all_sizes.items())),work_sizes=dict(sorted(work_sizes.items())),
        all_frees=all_frees,work_frees=work_frees)


def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--tag',default='paper-v1');a=p.parse_args()
    results=original.load_matrix(a.tag,'main');assert len(results)==63
    evidence=json.loads((HERE/'validation/checks.json').read_text());assert len(evidence)==42
    expected_native={original.key(c) for c in sweep.configurations(native=True)}
    assert {original.key(r['configuration']) for r in evidence}==expected_native
    for r in evidence:
        assert r['passed']
        source,_=run.source_for(argparse.Namespace(**r['configuration']))
        source=native.PRELUDE+source+native.POSTLUDE
        path=HERE/'validation'/(r['name']+'.c')
        assert original.digest(source.encode())==r['source_sha256']==original.digest(path.read_bytes())
        assert r['log_sha256']==original.digest(path.with_suffix('.log').read_bytes())
    rows=original.rows_for(results);profiles={}
    for d,r in zip(results,rows):
        c=d['configuration'];r.update(counters((HERE/d['_path']).parent))
        r.update(variant=c['variant'],backend_allocations=d['backend']['allocations'],
            backend_frees=d['backend']['frees'],pin_levels=c['pin_levels'],
            pinned_metadata_bytes=d['trace']['pinned_metadata_bytes'])
        pr=profile(d)
        assert sum(pr['all_sizes'].values())==r['all_allocations']==pr['all_frees']
        assert sum(pr['work_sizes'].values())==r['work_allocations']
        assert pr['work_frees']==r['work_frees']
        r['work_slab_eligible_allocations']=sum(n for s,n in pr['work_sizes'].items() if 2049<=s<=4096)
        key=(r['workload'],r['threads'])
        assert profiles.setdefault(key,pr)==pr
    comparisons=[];pairs=[]
    feature_fields=['allocator_sha256','backend_sha256','pin_levels','pinned_metadata_bytes']
    for key,pr in profiles.items():
        group={r['variant']:r for r in rows if (r['workload'],r['threads'])==key}
        assert set(group)==set(run.VARIANTS)
        raw=[d for d in results if (d['configuration']['workload'],d['configuration']['threads'])==key]
        traces=[{k:v for k,v in d['trace'].items() if k not in feature_fields} for d in raw]
        assert all(t==traces[0] for t in traces)
        baseline=group['baseline']
        for v,r in group.items():
            comparisons.append(dict(workload=key[0],threads=key[1],variant=v,
                work_speedup=baseline['work_ms']/r['work_ms'],total_speedup=baseline['total_ms']/r['total_ms'],
                work_alloc_mean_us=r['work_alloc_mean_us'],work_ms=r['work_ms'],total_ms=r['total_ms'],
                dram_commands=r['dram_commands'],dram_bytes=r['dram_bytes'],backend_allocations=r['backend_allocations']))
            if not r['pin_levels']:continue
            b=group[v.replace('slab-pinned-','slab-')]
            pairs.append(dict(workload=key[0],threads=key[1],policy=r['policy'],
                incremental_work_speedup=b['work_ms']/r['work_ms'],
                incremental_total_speedup=b['total_ms']/r['total_ms'],
                dram_byte_reduction_percent=100*(1-r['dram_bytes']/b['dram_bytes']),
                wram_endpoint_difference=r['wram_heap_end']-b['wram_heap_end']))
    output=HERE/'analysis';output.mkdir(exist_ok=True)
    original.write_csv(output/'all-runs.csv',rows)
    original.write_csv(output/'comparisons.csv',comparisons)
    original.write_csv(output/'incremental-pinning.csv',pairs)
    (output/'request-profiles.json').write_text(json.dumps([dict(workload=k[0],threads=k[1],**v) for k,v in profiles.items()],indent=2)+'\n')
    audit=dict(performance_configurations=len(results),native_sanitizer_cases=len(evidence),
        complete_declared_matrix=True,source_trace_simulator_native_hashes_match=True,
        matching_offered_work=True,all_successful=True,
        max_wram_heap_end=max(d['wram']['heap_end_address'] for d in results),
        simulator_sha256=results[0]['simulator_sha256'],
        results=[d['_path'] for d in results],
        counter_log_sha256={d['_path']:original.digest((HERE/d['_path']).parent.joinpath('log.txt').read_bytes()) for d in results})
    provenance=json.loads((HERE/'source_manifest.json').read_text())
    released=HERE.parents[2]/provenance['released_microbenchmark_path']
    assert original.digest(released.read_bytes())==provenance['released_microbenchmark_sha256']
    gates=list((HERE/'results/gate-paper-v1').glob('*/summary.json'));assert len(gates)==2
    for path in gates:
        gate=json.loads(path.read_text())
        matching=[d for d in results if d['source_sha256']==gate['source_sha256']]
        assert len(matching)==1 and gate['correctness_passed']
        assert gate['metrics']==matching[0]['metrics']
        assert gate['work_makespan_us']==matching[0]['work_makespan_us']
    audit['independent_gate_repeats_match_main']=len(gates)
    origin=json.loads((HERE.parent/'origin_manifest.json').read_text())
    assert original.digest(run.stack.BACKEND.encode())==origin['base_sha256']
    audit['original_backend_unchanged']=True
    (output/'audit.json').write_text(json.dumps(audit,indent=2)+'\n')
    report(rows,pairs,audit)
    print(json.dumps({k:v for k,v in audit.items() if k not in ['results','counter_log_sha256']},indent=2))


def report(rows,pairs,audit):
    cases=list(dict.fromkeys((r['workload'],r['threads']) for r in rows))
    text=['# PIM-malloc paper workload shapes on the full stack','',
        'All **63 configurations** completed: nine cases × seven variants. The graph cases are scaled kernel ports of the paper methodology; the allocation microbenchmarks retain the paper’s sizes, request count and tasklet counts. KV growth is an **allocator-only replay**, not a completed attention/LLM-serving reproduction. [README.md](README.md) gives exact scope and missing artifact components.','',
        'Every run uses the same maintained allocator stack, 32 MiB payload heap, 256 KiB empty-byte budget/tasklet and 350 MHz single-DPU simulator. Adaptive checks occur every 64 allocation plus free operations. Slabs use 64 KiB spans only for 2049–4096 B requests. Pinning uses a 528 B WRAM metadata prefix. Baseline is maintained **scan fixed-2**, not the paper’s straw-man buddy or static CSR baseline.','',
        '## Work runtime across every stage','',
        'Milliseconds, lower is faster. IA/IF: indexed adaptive-64/fixed-2. SA/SF: indexed + slab. PA/PF: indexed + slab + pinned metadata.','',
        '| Case | Tasklets | Baseline | IA | IF | SA | SF | PA | PF |',
        '| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |']
    for w,t in cases:
        g={r['variant']:r for r in rows if (r['workload'],r['threads'])==(w,t)}
        text.append('| '+LABELS[w]+f' | {t} | '+' | '.join(f"{g[v]['work_ms']:.3f}" for v in run.VARIANTS)+' |')
    text+=['','## Harness total runtime across every stage','',
        'Milliseconds, including initialization/preload, work, complete payload or graph checks, and final reclamation. For the allocation microbenchmarks this is a checked harness lifecycle, not the metric plotted in the paper; the per-call allocation latency table below is the closer methodological comparison.','',
        '| Case | Tasklets | Baseline | IA | IF | SA | SF | PA | PF |',
        '| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |']
    for w,t in cases:
        g={r['variant']:r for r in rows if (r['workload'],r['threads'])==(w,t)}
        text.append('| '+LABELS[w]+f' | {t} | '+' | '.join(f"{g[v]['total_ms']:.3f}" for v in run.VARIANTS)+' |')
    text+=['','## Full-stack speedups and incremental pinning','',
        'Full-stack work/total compare with scan fixed-2. Pin columns compare with the same policy and slabs before pinning. Values below one are regressions.','',
        '| Case | Tasklets | Adaptive work | Adaptive total | Fixed work | Fixed total | Pin adaptive work | Pin fixed work |',
        '| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |']
    for w,t in cases:
        g={r['variant']:r for r in rows if (r['workload'],r['threads'])==(w,t)}
        base=g['baseline'];pa=g['slab-pinned-adaptive64'];pf=g['slab-pinned-fixed2']
        values=[base['work_ms']/pa['work_ms'],base['total_ms']/pa['total_ms'],
            base['work_ms']/pf['work_ms'],base['total_ms']/pf['total_ms'],
            g['slab-adaptive64']['work_ms']/pa['work_ms'],g['slab-fixed2']['work_ms']/pf['work_ms']]
        text.append('| '+LABELS[w]+f' | {t} | '+' | '.join(f'{v:.3f}×' for v in values)+' |')
    text+=['','## What the comparisons show','']
    def case(w,t):return {r['variant']:r for r in rows if (r['workload'],r['threads'])==(w,t)}
    for t in [1,16]:
        g=case('micro-4096',t);b=g['baseline'];pf=g['slab-pinned-fixed2'];sf=g['slab-fixed2']
        text.append(f'- **4 KiB, {t} tasklet'+('s' if t!=1 else '')+
            f":** the full fixed stack provides {b['work_ms']/pf['work_ms']:.2f}× work and "
            f"{b['total_ms']/pf['total_ms']:.2f}× harness-total speedup. Backend allocations fall from "
            f"{b['backend_allocations']:,} to {pf['backend_allocations']:,}; pinning adds "
            f"{sf['work_ms']/pf['work_ms']:.2f}× over the same fixed slab variant. Peak payload reservation "
            f"rises from {b['peak_payload_bytes']/1024:,.0f} to {pf['peak_payload_bytes']/1024:,.0f} KiB, "
            'plus the indexed variants’ separate 64 KiB ownership map.')
    gl=case('pim-list',4);ga=case('pim-array',4);small=case('micro-32',1)
    text.append(f"- **Paper graph ports:** full fixed-stack work gains are {gl['baseline']['work_ms']/gl['slab-pinned-fixed2']['work_ms']:.2f}× "
        f"for linked blocks and {ga['baseline']['work_ms']/ga['slab-pinned-fixed2']['work_ms']:.2f}× for growing arrays. "
        f"The adaptive stack reaches {ga['baseline']['work_ms']/ga['slab-pinned-adaptive64']['work_ms']:.3f}× on growing arrays, a regression. "
        'The new slab class is unused here; pinning benefits backend refills, while indexed ownership can cost more on small insertion-only cases.')
    text.append(f"- **Small-object control:** the full fixed stack reaches only {small['baseline']['work_ms']/small['slab-pinned-fixed2']['work_ms']:.3f}× "
        'on single-tasklet 32 B allocations. Extra list/ownership bookkeeping can outweigh savings. Allocation-only cases keep their objects live, '
        'so they do not exercise the benefit of retaining empty pages during work.')
    text+=['','The original frontend stops at 2 KiB, so every original 4 KiB request calls buddy. The slab extension adds a ninth, 4 KiB class backed by 64 KiB spans. Its large gains here come from caching a previously uncached size. They do not demonstrate equivalent gains from enlarging pages for the existing small-object classes. Fixed-2 limits empty retention and does not cap live capacity.']
    text+=['','![Every measured stage](analysis/paper-full-stack.png)']
    text+=['','## Paper microbenchmark: mean allocation latency','',
        'Microseconds per allocation, including lock wait. These per-call PC probes exclude pointer-record DMA, checking and frees; work makespan above includes pointer recording and the end barrier. Arithmetic mean of concurrent request latencies is distinct from elapsed work divided by total requests.','',
        '| Request | Tasklets | Baseline | IA | IF | SA | SF | PA | PF |',
        '| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |']
    for w,t in cases:
        if not w.startswith('micro-'):continue
        g={r['variant']:r for r in rows if (r['workload'],r['threads'])==(w,t)}
        text.append('| '+LABELS[w]+f' | {t} | '+' | '.join(f"{g[v]['work_alloc_mean_us']:.3f}" for v in run.VARIANTS)+' |')
    text+=['','## Memory requests and traffic','',
        'Counts cover the full run, including preload, checking, reclamation and passive final accounting. They are simulator DRAM read/write commands, not every WRAM access or source-level DMA invocation. Traffic also includes application data, graph/record storage and the ownership map.','',
        '| Case | Tasklets | Alloc calls | Timed alloc calls | Baseline DRAM commands | PA commands | PF commands | Baseline DRAM bytes | PA bytes | PF bytes |',
        '| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |']
    for w,t in cases:
        g={r['variant']:r for r in rows if (r['workload'],r['threads'])==(w,t)}
        b=g['baseline'];pa=g['slab-pinned-adaptive64'];pf=g['slab-pinned-fixed2']
        vals=[b['all_allocations'],b['work_allocations'],b['dram_commands'],pa['dram_commands'],pf['dram_commands'],b['dram_bytes'],pa['dram_bytes'],pf['dram_bytes']]
        text.append('| '+LABELS[w]+f' | {t} | '+' | '.join(f'{v:,}' for v in vals)+' |')
    text+=['','## Validation and interpretation','',
        f'All {audit["native_sanitizer_cases"]} new native ASan/UBSan cases passed, with independent live-allocation interval checks, complete payload checks, exact graph-neighbor checks, allocator invariants and final buddy root coalescing. All 63 DPU runs passed application/count/final-state checks without allocation failures. Maximum recorded WRAM heap endpoint: **{audit["max_wram_heap_end"]:,} / 65,536 B**. Existing differential backend and layered tests are preserved under the parent experiment.','',
        'The accepted matrix was declared before measurements and includes every case, including regressions. Graph updates use a 3,072-edge real Gowalla sample split 1:2 new/existing; its growing arrays are smaller than the full paper dataset’s 64 B–32 KiB range. No graph requests in this sample use the new slab class. 512 B KV requests also use the existing small-object frontend. Consequently, large slab benefits should be assessed on the unchanged 4 KiB request microbenchmark, rather than by changing graph/KV object sizes.','',
        'Work excludes preload/initialization and output checking/drain. Total includes those phases and ends before passive final accounting. These measurements do not reproduce the paper’s static CSR speedups, original allocator baselines, full attention computation, tokens/s or TPOT.','',
        'The earlier five-case comparison remains available in the [parent results](../RESULTS.md); its sources and saved measurements are unchanged.','',
        'Machine-readable evidence: [all stages and traffic](analysis/all-runs.csv), [speedups](analysis/comparisons.csv), [pin-only changes](analysis/incremental-pinning.csv), [request profiles](analysis/request-profiles.json), [source/trace/simulator audit](analysis/audit.json).','']
    (HERE/'RESULTS.md').write_text('\n'.join(text))


if __name__=='__main__':main()
