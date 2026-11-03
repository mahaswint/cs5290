#!/usr/bin/env python3
"""Audit unchanged paper workload ports with/without the added slab class."""
import argparse
import csv
import importlib.util
import json
import re
from collections import Counter
from pathlib import Path
import analyze as original
import analyze_interval
import run

HERE=Path(__file__).resolve().parent


def request_profile(a):
    """Replay request decisions independently of allocator addresses/timing."""
    all_sizes=Counter();work_sizes=Counter();all_frees=work_frees=0
    if a.workload=='prim-sel':
        return dict(all_sizes={},work_sizes={},all_frees=0,work_frees=0)
    header,_=run.trace.make_trace(a)
    ops_text=re.search(r'paper_ops\[\d+\] = \{\n(.*?)\n\};',header,re.S).group(1)
    ops=[tuple(map(int,re.findall(r'(\d+)U',line))) for line in ops_text.splitlines()]
    def values(name):
        return list(map(int,re.findall(r'(\d+)U',re.search(name+r'\[\d+\] = \{([^}]*)\}',header).group(1))))
    offsets=values('paper_offsets');starts=values('paper_work_starts')
    degrees=Counter();capacities=Counter()
    for t in range(a.threads):
        for i in range(offsets[t],offsets[t+1]):
            kind,index,value,_=ops[i];timed=i>=starts[t]
            size=freed=0
            if a.workload=='larson':
                if kind in [0,2]:size=value
                if kind==2:freed=1
            else:
                assert index%a.threads==t
                degree=degrees[index]
                if kind==3:
                    if a.workload=='pim-array':
                        if (degree+1)*4>capacities[index]:
                            freed=int(bool(capacities[index]))
                            size=capacities[index]*2 if capacities[index] else 64
                            capacities[index]=size
                    elif degree%62==0:size=256
                    degrees[index]+=1
                else:
                    assert kind==4 and degree>0 and a.workload=='faimgraph'
                    freed=int(degree%62==1);degrees[index]-=1
            if size:
                all_sizes[size]+=1
                if timed:work_sizes[size]+=1
            all_frees+=freed
            if timed:work_frees+=freed
    if a.workload=='larson':all_frees+=a.objects*a.threads
    elif a.workload=='pim-array':all_frees+=sum(bool(v) for v in capacities.values())
    else:all_frees+=sum((degree+61)//62 for degree in degrees.values())
    return dict(all_sizes=dict(sorted(all_sizes.items())),work_sizes=dict(sorted(work_sizes.items())),
                all_frees=all_frees,work_frees=work_frees)


def native_audit():
    folder=HERE/'validation/slab-real-v1'
    evidence=json.loads((folder/'native-evidence.json').read_text())
    assert evidence==json.loads((folder/'checks.json').read_text())
    assert len(evidence)==10
    assert {(r['workload'],r['policy']) for r in evidence}=={(w,p) for w in run.KINDS for p in ['fixed','adaptive']}
    spec=importlib.util.spec_from_file_location('paper_slab_native',HERE/'test.py')
    native=importlib.util.module_from_spec(spec);spec.loader.exec_module(native)
    for r in evidence:
        assert r['passed'] and r['mode']=='indexed' and r['slab_kib']==64 and r['budget_pages']==64 and r['decay_ops']==128
        a=run.options().parse_args([])
        vars(a).update(workload=r['workload'],mode='indexed',policy=r['policy'],threads=1,
            objects=64,operations=128,items=256,rounds=2,fixture=True,heap_kib=1024,validate=True,
            decay_ops=128,budget_pages=64,slab_kib=64)
        source,_=run.source_for(a)
        assert original.digest((native.PRELUDE+source+native.POSTLUDE).encode())==r['generated_native_source_sha256']
        name=f"host-{r['workload']}-indexed-{r['policy']}"
        assert original.digest((folder/(name+'.log')).read_bytes())==r['native_log_sha256']
    return evidence


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--tag',default='slab-real-v1')
    p.add_argument('--validation-tag',default='validation-slab-real-v1')
    a=p.parse_args()
    results=original.load_matrix(a.tag,'slab-real')
    validation=original.load_matrix(a.validation_tag,'slab-real-validation')
    assert len(results)==40 and len(validation)==10
    native=native_audit()
    rows=original.rows_for(results)
    profiles={}
    for result,row in zip(results,rows):
        c=result['configuration'];profile_key=(c['workload'],c['threads'],c['seed'])
        if profile_key not in profiles:profiles[profile_key]=request_profile(argparse.Namespace(**c))
        profile=profiles[profile_key]
        assert sum(profile['all_sizes'].values())==result['totals']['alloc_calls']
        assert sum(profile['work_sizes'].values())==result['work_metrics']['alloc']['samples']
        assert profile['all_frees']==result['totals']['free_calls']
        assert profile['work_frees']==result['work_metrics']['free']['samples']
        row.update(analyze_interval.counters(result))
        row.update(slab_kib=c.get('slab_kib',0),empty_budget_bytes=c['budget_pages']*4096,
            all_slab_eligible_allocations=sum(n for size,n in profile['all_sizes'].items() if 2049<=size<=4096),
            work_slab_eligible_allocations=sum(n for size,n in profile['work_sizes'].items() if 2049<=size<=4096),
            request_min_bytes=min(profile['all_sizes'],default=0),request_max_bytes=max(profile['all_sizes'],default=0))
        assert c['mode']=='indexed' and c['decay_ops']==128 and c['budget_pages']==64
    paired=[]
    for workload in run.KINDS:
        for threads in [1,4]:
            for policy in ['fixed','adaptive']:
                group=[r for r in rows if (r['workload'],r['threads'],r['policy'])==(workload,threads,policy)]
                assert len(group)==2 and {r['slab_kib'] for r in group}=={0,64}
                base=next(r for r in group if not r['slab_kib']);slab=next(r for r in group if r['slab_kib'])
                for field in ['trace_sha256','all_allocations','work_allocations','work_frees',
                              'all_slab_eligible_allocations','work_slab_eligible_allocations','empty_budget_bytes']:
                    assert base[field]==slab[field]
                sources=[next(d for d in results if d['_path']==r['summary']) for r in [base,slab]]
                clean_traces=[{k:v for k,v in d['trace'].items() if k!='allocator_sha256'} for d in sources]
                assert clean_traces[0]==clean_traces[1]
                clean_configs=[{k:v for k,v in d['configuration'].items() if k!='slab_kib'} for d in sources]
                assert clean_configs[0]==clean_configs[1]
                pair=dict(workload=workload,threads=threads,policy=policy,
                    baseline_work_ms=base['work_ms'],slab_work_ms=slab['work_ms'],work_speedup=base['work_ms']/slab['work_ms'],
                    baseline_total_ms=base['total_ms'],slab_total_ms=slab['total_ms'],total_speedup=base['total_ms']/slab['total_ms'])
                for prefix,row in [('baseline',base),('slab',slab)]:
                    for field in ['dram_access_commands','dram_read_write_bytes','full_execution_instructions',
                                  'all_refills','all_allocations','work_allocations','peak_payload_bytes',
                                  'work_end_reserved_bytes','work_end_empty_bytes','wram_heap_end','summary']:
                        pair[prefix+'_'+field]=row[field]
                paired.append(pair)
    output=HERE/'analysis/slab-real';output.mkdir(exist_ok=True)
    original.write_csv(output/'all-runs.csv',rows)
    original.write_csv(output/'comparisons.csv',paired)
    profile_rows=[]
    for (workload,threads,seed),profile in profiles.items():
        profile_rows.append(dict(workload=workload,threads=threads,seed=seed,**profile))
    (output/'request-profiles.json').write_text(json.dumps(profile_rows,indent=2)+'\n')
    audit=dict(performance_configurations=len(results),dpu_validation_configurations=len(validation),
        native_sanitizer_checks=len(native),all_complete=True,same_offered_work_per_pair=True,
        exact_source_trace_simulator_and_native_hashes_match=True,request_model_counts_match_measured_calls=True,
        all_backend_refill_counts_unchanged=all(r['baseline_all_refills']==r['slab_all_refills'] for r in paired),
        all_full_execution_dram_bytes_unchanged=all(r['baseline_dram_read_write_bytes']==r['slab_dram_read_write_bytes'] for r in paired),
        eligible_slab_requests=sum(r['all_slab_eligible_allocations'] for r in rows),
        slab_bytes=65536,empty_budget_bytes_per_tasklet=262144,adaptive_interval_alloc_plus_free=128,
        max_wram_heap_end=max(d['wram']['heap_end_address'] for d in results+validation),
        simulator_sha256=results[0]['simulator_sha256'],
        result_paths=[d['_path'] for d in results+validation],
        counter_log_sha256={d['_path']:original.digest((HERE/d['_path']).parent.joinpath('log.txt').read_bytes()) for d in results},
        scope='Unchanged reduced paper workload ports; deterministic 350 MHz single-DPU simulation; no original application or hardware speedup claim.')
    (output/'audit.json').write_text(json.dumps(audit,indent=2)+'\n')
    text=['# Slabs on the existing paper-derived workloads','',
        'This evaluates **slab + indexed + fixed/adaptive** on the same Larson, PIM linked-block insertion, growing arrays, graph churn and PrIM selection ports previously measured. Inputs, sizes and application code are unchanged. These are reduced single-DPU workload ports, not the complete original paper applications or physical hardware runs; see [SOURCES.md](SOURCES.md).','',
        'Every new comparison uses indexed lists and ownership lookup, a 32 MiB payload heap plus the same 64 KiB MRAM owner table, seed 4141, and an equal **256 KiB empty-byte budget per tasklet**. The slab variant adds 64 KiB spans serving 2049–4096 B objects. Fixed retains up to two empty spans/class; adaptive checks every 128 allocations plus frees with maximum two spans/class. Live/partial spans have no two-span cap.','',
        'The 256 KiB budget matches the synthetic slab experiment and is larger than the earlier paper experiment\'s 16 KiB budget. The no-slab baselines were rerun with the new budget and interval, so the ratios isolate enabling the slab implementation.','',
        '**Measured result:** no consistent work-time benefit from enabling slabs. Larson work is 4.7–5.5% slower and graph churn is 3.1–4.2% slower. Total runtime on the allocation workloads is 0.4–4.5% slower. Backend refill counts and full-execution DRAM byte totals are identical in every pair.','',
        '## Work runtime','',
        'Work is concurrent tasklet makespan, including application DMA, trace reads and synchronization, excluding preload/warmup and final checking/destruction. All times are simulated milliseconds at 350 MHz; speedup = same-policy no-slab indexed time / slab indexed time. Values below 1 are regressions.','',
        '| Workload | Tasklets | Fixed no slab ms | Fixed + slab ms | Fixed speedup | Adaptive no slab ms | Adaptive + slab ms | Adaptive speedup |',
        '| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |']
    for workload in run.KINDS:
        for threads in [1,4]:
            f=next(r for r in paired if (r['workload'],r['threads'],r['policy'])==(workload,threads,'fixed'))
            d=next(r for r in paired if (r['workload'],r['threads'],r['policy'])==(workload,threads,'adaptive'))
            text.append(f"| {original.LABELS[workload]} | {threads} | {f['baseline_work_ms']:.3f} | {f['slab_work_ms']:.3f} | {f['work_speedup']:.3f}x | {d['baseline_work_ms']:.3f} | {d['slab_work_ms']:.3f} | {d['work_speedup']:.3f}x |")
    text+=['','## Which allocations exercise slabs?','',
        'An independent Python replay reconstructs every allocation size from the unchanged operation traces and data-structure rules, and its full/work allocation and free counts must equal the measured DPU counts. The exact histograms are in [request-profiles.json](analysis/slab-real/request-profiles.json).','',
        '| Workload | Tasklets | Request min–max B | All allocations | Timed allocations | Allocations in 2049–4096 B |',
        '| --- | ---: | ---: | ---: | ---: | ---: |']
    for workload in run.KINDS:
        for threads in [1,4]:
            r=next(r for r in rows if (r['workload'],r['threads'],r['policy'],r['slab_kib'])==(workload,threads,'fixed',0))
            size_range=f"{r['request_min_bytes']}–{r['request_max_bytes']}" if r['all_allocations'] else 'none'
            text.append(f"| {original.LABELS[workload]} | {threads} | {size_range} | {r['all_allocations']:,} | {r['work_allocations']:,} | {r['all_slab_eligible_allocations']:,} |")
    if not audit['eligible_slab_requests']:
        text+=['', '**None of these unchanged inputs requests the new slab class.** The dense 4 KiB synthetic speedups therefore do not carry over to these traces. The small-object paths still use 4 KiB pages. With no slab-class allocations, timing differences cannot be attributed to slab reuse; possible contributors include changed routing, bookkeeping, controller loops, generated code and tasklet interleaving. PrIM SEL is an allocation-free control. A realistic slab benefit needs a workload that naturally allocates/reuses 2049–4096 B objects; changing these request sizes would be a separate modified-workload experiment.']
    text+=['','## Total runtime and full-execution memory commands','',
        'Total includes initialization, preload/warmup, work, output validation and teardown/drain, ending before passive final accounting. It excludes compilation, simulation wall time and host transfers. DRAM command counts cover the entire DPU execution including final checks, and exclude WRAM accesses, instruction fetches and row activation/precharge commands. They are not allocator-only accesses or C-level DMA-call counts; one DMA can split into multiple DRAM commands. Counts differ slightly despite identical byte totals, consistent with different DMA alignment/row splitting.','',
        '| Workload | Tasklets | Policy | No slab total ms | Slab total ms | Total speedup | No slab DRAM reads+writes | Slab DRAM reads+writes |',
        '| --- | ---: | --- | ---: | ---: | ---: | ---: | ---: |']
    for r in paired:
        text.append(f"| {original.LABELS[r['workload']]} | {r['threads']} | {r['policy']} | {r['baseline_total_ms']:.3f} | {r['slab_total_ms']:.3f} | {r['total_speedup']:.3f}x | {r['baseline_dram_access_commands']:,} | {r['slab_dram_access_commands']:,} |")
    text+=['','## Validation, memory and reproduction','',
        f"All **{len(results)} performance configurations, {len(validation)} DPU invariant-validation configurations and {len(native)} native ASan/UBSan/oracle cases passed**. Native fixtures check independent non-overlap, payloads, exact graph neighbors and coalescing; DPU checks validate outputs, zero allocation failures, balanced calls, complete coalescing and zero final live/retained memory. Generated C, traces/dataset manifests, simulator and native evidence hashes match. Maximum WRAM heap endpoint across the new DPU runs is {audit['max_wram_heap_end']:,} / 65,536 B. Worst-case stack depth, IRAM fit and hardware throughput remain unverified.",
        '',
        'Peak payload, work-end reservation/empty bytes, owner-map bytes, instruction counts, latency distributions and exact run paths are retained in the CSV. The ninth class adds 56 B of cache descriptor per tasklet; there is no active slab payload overhead when its class receives no requests. Timing is deterministic per input, so these runs do not supply independent confidence intervals or original paper speedup claims.',
        '', '```sh',
        'python3 pim-reproduction/paper-workloads/test.py --mode indexed --decay-ops 128 --slab-kib 64 --budget-pages 64 --output-tag slab-real-v1',
        'python3 pim-reproduction/paper-workloads/sweep.py --preset slab-real-validation --tag validation-slab-real-v1 --jobs 2',
        'python3 pim-reproduction/paper-workloads/sweep.py --preset slab-real --tag slab-real-v1 --jobs 4',
        'python3 pim-reproduction/paper-workloads/analyze_slab_real.py',
        '```','',
        '- [Paired timings, traffic and memory](analysis/slab-real/comparisons.csv)',
        '- [Every run, latencies and counters](analysis/slab-real/all-runs.csv)',
        '- [Exact allocation-size histograms](analysis/slab-real/request-profiles.json)',
        '- [Audit and accepted paths](analysis/slab-real/audit.json)',
        '']
    (HERE/'SLAB_REAL.md').write_text('\n'.join(text))
    print(json.dumps({k:v for k,v in audit.items() if k not in ['counter_log_sha256','result_paths']},indent=2))


if __name__=='__main__':main()
