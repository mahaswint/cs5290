#!/usr/bin/env python3
"""Require all declared heavy-request runs and report speed and memory together."""
import argparse
import csv
import json
import re
from collections import Counter, defaultdict
import run

HERE=run.HERE
original=run.module('heavy_original_analysis',HERE.parent/'paper-workloads/analyze.py')
original.HERE=HERE;original.run=run
native=run.module('heavy_native_checks',HERE/'test.py')
LABELS={'larson':'Larson 8–4095 B','cloud-cache':'CloudPhysics cache'}


def profile(result):
    c=result['configuration'];a=argparse.Namespace(**c)
    if c['workload']=='cloud-cache':
        t=result['trace']
        return {int(k):v for k,v in t['request_sizes'].items()}, {int(k):v for k,v in t['work_request_sizes'].items()}
    header,_=run.paper.trace.make_trace(a)
    body=re.search(r'paper_ops\[\d+\] = \{\n(.*?)\n\};',header,re.S).group(1)
    ops=[tuple(map(int,re.findall(r'(\d+)U',line))) for line in body.splitlines()]
    def values(name):return list(map(int,re.findall(r'(\d+)U',re.search(name+r'\[\d+\] = \{([^}]*)\}',header).group(1))))
    offsets=values('paper_offsets');starts=values('paper_work_starts');all_sizes=Counter();work_sizes=Counter()
    for t in range(a.threads):
        for i in range(offsets[t],offsets[t+1]):
            kind,index,size,tag=ops[i]
            if kind in [0,2]:
                all_sizes[size]+=1
                if i>=starts[t]:work_sizes[size]+=1
    return dict(sorted(all_sizes.items())),dict(sorted(work_sizes.items()))


def counters(folder):
    values={}
    for line in (folder/'log.txt').read_text().splitlines():
        m=re.fullmatch(r'(RowBuffer|Logic)\[0_0_0\]_(\w+): (\d+)',line)
        if m:values[m[1]+'_'+m[2]]=int(m[3])
    return dict(dram_read_commands=values['RowBuffer_num_reads'],dram_write_commands=values['RowBuffer_num_writes'],
        dram_access_commands=values['RowBuffer_num_reads']+values['RowBuffer_num_writes'],
        dram_read_bytes=values['RowBuffer_read_bytes'],dram_write_bytes=values['RowBuffer_write_bytes'],
        dram_read_write_bytes=values['RowBuffer_read_bytes']+values['RowBuffer_write_bytes'],
        instructions=values['Logic_num_instructions'])


def main():
    main_runs=original.load_matrix('heavy-v1','main')
    heldout=original.load_matrix('heldout-heavy-v1','heldout')
    validation=original.load_matrix('validation-heavy-v1','validation')
    assert len(main_runs)==24 and len(heldout)==4 and len(validation)==12
    evidence=json.loads((HERE/'validation/checks.json').read_text())
    assert len(evidence)==12 and all(r['passed'] for r in evidence)
    assert {original.key(r['configuration']) for r in evidence}=={original.key(vars(a)) for a in native.configurations()}
    for r in evidence:
        a=argparse.Namespace(**r['configuration']);source,_=run.source_for(a)
        assert original.digest((native.native.PRELUDE+source+native.native.POSTLUDE).encode())==r['generated_native_source_sha256']
        name=f"host-{a.workload}-{a.policy}-slab{getattr(a,'slab_kib',0)}.log"
        assert original.digest((HERE/'validation'/name).read_bytes())==r['native_log_sha256']
    results=main_runs+heldout;rows=original.rows_for(results);profiles={}
    for result,row in zip(results,rows):
        c=result['configuration'];all_sizes,work_sizes=profile(result)
        assert sum(all_sizes.values())==row['all_allocations']
        assert sum(work_sizes.values())==row['work_allocations']
        row.update(counters((HERE/result['_path']).parent))
        row.update(slab_kib=c.get('slab_kib',0),trace_offset=c['trace_offset'],
            all_backend_allocations=result['backend']['allocations'],all_backend_frees=result['backend']['frees'],
            empty_budget_bytes=c['budget_pages']*4096,
            all_slab_eligible_allocations=sum(n for size,n in all_sizes.items() if 2049<=size<=4096),
            work_slab_eligible_allocations=sum(n for size,n in work_sizes.items() if 2049<=size<=4096),
            all_direct_bypass_allocations=sum(n for size,n in all_sizes.items() if size>4096),
            work_direct_bypass_allocations=sum(n for size,n in work_sizes.items() if size>4096))
        key=(row['preset'],row['workload'],row['threads'],row['seed'],row['trace_offset'])
        old=profiles.setdefault(key,dict(all_sizes=all_sizes,work_sizes=work_sizes))
        assert old==dict(all_sizes=all_sizes,work_sizes=work_sizes)
    groups=defaultdict(list)
    for row in rows:groups[(row['preset'],row['workload'],row['threads'],row['policy'],row['seed'],row['trace_offset'])].append(row)
    paired=[]
    by_path={r['_path']:r for r in results}
    for key,group in groups.items():
        baseline=next(r for r in group if not r['slab_kib'])
        assert len(group)==(3 if key[0]=='main' else 2)
        for row in group:
            if not row['slab_kib']:continue
            for field in ['trace_sha256','all_allocations','work_allocations','work_frees',
                          'all_slab_eligible_allocations','work_slab_eligible_allocations','empty_budget_bytes']:
                assert baseline[field]==row[field]
            records=[by_path[r['summary']] for r in [baseline,row]]
            cleaned=[{k:v for k,v in d['trace'].items() if k!='allocator_sha256'} for d in records]
            assert cleaned[0]==cleaned[1]
            configs=[{k:v for k,v in d['configuration'].items() if k!='slab_kib'} for d in records]
            assert configs[0]==configs[1]
            pair=dict(preset=key[0],workload=key[1],threads=key[2],policy=key[3],seed=key[4],trace_offset=key[5],
                slab_kib=row['slab_kib'],work_speedup=baseline['work_ms']/row['work_ms'],
                total_speedup=baseline['total_ms']/row['total_ms'])
            for prefix,r in [('baseline',baseline),('slab',row)]:
                for field in ['work_ms','total_ms','all_backend_allocations','all_refills','dram_access_commands',
                              'dram_read_write_bytes','instructions','peak_payload_bytes','work_end_requested_bytes',
                              'work_end_reserved_bytes','work_end_empty_bytes','work_allocations','work_slab_eligible_allocations',
                              'work_direct_bypass_allocations','summary']:
                    pair[prefix+'_'+field]=r[field]
            paired.append(pair)
    output=HERE/'analysis';output.mkdir(exist_ok=True)
    original.write_csv(output/'all-runs.csv',rows);original.write_csv(output/'comparisons.csv',paired)
    (output/'request-profiles.json').write_text(json.dumps([dict(preset=k[0],workload=k[1],threads=k[2],seed=k[3],trace_offset=k[4],**v) for k,v in profiles.items()],indent=2)+'\n')
    audit=dict(main_performance_configurations=len(main_runs),heldout_performance_configurations=len(heldout),
        dpu_validation_configurations=len(validation),native_sanitizer_cases=len(evidence),
        complete_declared_matrices=True,all_sources_traces_simulator_native_hashes_match=True,
        same_offered_work_in_every_pair=True,all_completed_without_failures=True,
        max_wram_heap_end=max(d['wram']['heap_end_address'] for d in results+validation),
        simulator_sha256=results[0]['simulator_sha256'],
        result_paths=[d['_path'] for d in results+validation],
        counter_log_sha256={d['_path']:original.digest((HERE/d['_path']).parent.joinpath('log.txt').read_bytes()) for d in results},
        scope='Deterministic 350 MHz DPU kernels; modified-size Larson and reduced real-I/O-trace LRU write-cache replay; no physical hardware or full original application speedup claim.')
    (output/'audit.json').write_text(json.dumps(audit,indent=2)+'\n')
    text=['# Large-request workload results','',
        'These compare unchanged **slab + indexed + fixed/adaptive** allocators on two deliberately selected allocation-heavy workloads: mixed-size Larson (explicit 8–4095 B parameterization) and a variable-size LRU write-cache replay of actual CloudPhysics I/O requests. See [README.md](README.md) for source mapping, original-size preservation, workload details and limitations. These are deterministic 350 MHz single-DPU kernel simulations, not full original applications or hardware measurements.','',
        'All pairs use the same input, policy, 32 MiB payload heap, 64 KiB MRAM owner table, 256 KiB empty-byte budget/tasklet and 128-operation adaptive interval. The baseline is indexed **without** the added slab class. Ratios below 1 are regressions. Work excludes preload/warmup, final checking and drain; total includes them and initialization. Host transfers, storage/network service time, compilation and simulation wall time are excluded.','',
        'Larson emphasizes allocation/replacement overhead and retains the existing 8 B payload-tag checks. The cache performs full payload processing with the original mixed request sizes. Their speedups measure different application/allocator cost balances. Neither establishes an advantage from the adaptive pressure controller: payload use remains far below the 75% pressure threshold.','']
    for workload in LABELS:
        pair={r['policy']:r for r in paired if (r['preset'],r['workload'],r['threads'],r['slab_kib'])==('main',workload,4,64)}
        f,d=pair['fixed'],pair['adaptive']
        text.append(f"**{LABELS[workload]}, four tasklets, 64 KiB slabs:** fixed reaches {f['work_speedup']:.2f}x work / {f['total_speedup']:.2f}x total; adaptive reaches {d['work_speedup']:.2f}x work / {d['total_speedup']:.2f}x total, versus the matching no-slab policy.")
        text.append('')
    text+=['## Main comparison','',
        '| Workload | Tasklets | Policy | Slab KiB | No slab work ms | Slab work ms | Work speedup | Total speedup | Backend allocations, no slab → slab | Peak payload KiB, no slab → slab |',
        '| --- | ---: | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |']
    for r in paired:
        if r['preset']!='main':continue
        text.append(f"| {LABELS[r['workload']]} | {r['threads']} | {r['policy']} | {r['slab_kib']} | {r['baseline_work_ms']:.3f} | {r['slab_work_ms']:.3f} | {r['work_speedup']:.3f}x | {r['total_speedup']:.3f}x | {r['baseline_all_backend_allocations']:,} → {r['slab_all_backend_allocations']:,} | {r['baseline_peak_payload_bytes']/1024:.1f} → {r['slab_peak_payload_bytes']/1024:.1f} |")
    text+=['','Backend allocations count all buddy calls, including direct-bypass objects. They differ from frontend refill counts in the CSV. Payload peak is the actual simultaneous backend high-water mark; it excludes the extra 64 KiB owner map and identical preallocated application/trace storage. Larger slabs may reduce calls while increasing reserved bytes.',
        '', 'Most of the gain over no slab comes from serving previously direct-bypass 2049–4096 B objects through the added frontend class. Comparing 32 against 64 KiB isolates the additional geometry change; 64 KiB is not universally faster than 32 KiB. These results support the selected class and reuse pattern, not an unlimited bigger-is-better rule.','',
        '## Offered allocation sizes','',
        'These are timed allocation requests, not all trace operations or DRAM transactions. A cache hit may perform payload work without allocating. All sizes and offered operations are identical across geometry and policy variants in a group.','',
        '| Workload | Tasklets | Timed allocations | Slab-eligible 2049–4096 B | Direct bypass >4096 B |',
        '| --- | ---: | ---: | ---: | ---: |']
    for workload in LABELS:
        for threads in [1,4]:
            r=next(r for r in rows if (r['preset'],r['workload'],r['threads'],r['policy'],r['slab_kib'])==('main',workload,threads,'fixed',0))
            text.append(f"| {LABELS[workload]} | {threads} | {r['work_allocations']:,} | {r['work_slab_eligible_allocations']:,} | {r['work_direct_bypass_allocations']:,} |")
    text+=['','Larson is a configured benchmark: increasing its upper size parameter is explicit, and its previous 8–999 B result is preserved. The cache retains the original variable I/O lengths, keys, commands and per-owner order. It neither filters for eligible sizes nor rounds requests to 4 KiB. Full payload writes/reads/checks, metadata lookup and LRU decisions remain in timing. Its small trace window supports allocator-path conclusions, not production cache hit-ratio conclusions.','',
        '## Held-out seed / trace window','',
        'Adaptive, four tasklets, 64 KiB slabs. Larson seed 9173; cache window starts at source record 4096. Every other setting is unchanged.','',
        '| Workload | No slab work ms | Slab work ms | Work speedup | Total speedup | Peak payload KiB, no slab → slab |',
        '| --- | ---: | ---: | ---: | ---: | ---: |']
    for r in paired:
        if r['preset']=='heldout':text.append(f"| {LABELS[r['workload']]} | {r['baseline_work_ms']:.3f} | {r['slab_work_ms']:.3f} | {r['work_speedup']:.3f}x | {r['total_speedup']:.3f}x | {r['baseline_peak_payload_bytes']/1024:.1f} → {r['slab_peak_payload_bytes']/1024:.1f} |")
    text+=['','## DRAM counters and memory accounting','',
        'Full-execution DRAM read/write commands and bytes include final accounting checks. They exclude WRAM accesses, instruction fetches, row activation/precharge and host transfers; one DMA may split into multiple commands. Work/total timing boundaries are narrower, so these are not work-window or allocator-only traffic counts.','',
        '| Workload | Tasklets | Policy | Slab KiB | DRAM reads+writes, no slab → slab | DRAM MiB, no slab → slab | Work-end requested/reserved KiB with slab |',
        '| --- | ---: | --- | ---: | ---: | ---: | ---: |']
    for r in paired:
        if r['preset']!='main':continue
        text.append(f"| {LABELS[r['workload']]} | {r['threads']} | {r['policy']} | {r['slab_kib']} | {r['baseline_dram_access_commands']:,} → {r['slab_dram_access_commands']:,} | {r['baseline_dram_read_write_bytes']/2**20:.3f} → {r['slab_dram_read_write_bytes']/2**20:.3f} | {r['slab_work_end_requested_bytes']/1024:.1f} / {r['slab_work_end_reserved_bytes']/1024:.1f} |")
    text+=['','## Validation and reproduction','',
        f"All **{len(results)} performance configurations, {len(validation)} DPU invariant-validation configurations and {len(evidence)} native ASan/UBSan cases passed**. The analyzer requires every declared run, audits exact generated sources/traces/simulator/native evidence and matches offered allocation counts. Each run ends with balanced alloc/free calls, zero failures/live/retained bytes and complete buddy coalescing. Maximum WRAM endpoint is {audit['max_wram_heap_end']:,} / 65,536 B; worst-case stacks and IRAM/hardware fit remain unverified.",
        '',
        'Native interval oracles check non-overlap and free sizes. Cache checks every payload word and exact final key/size/version membership, and independently predicted hit/allocation/eviction counts. DPU performance runs retain application correctness checks but disable expensive allocator invariant traversal. Earlier allocator tests cover the separate sparse-pinning and tight-heap failure tradeoffs; successful workloads here do not eliminate those costs.',
        '',
        '```sh',
        'python3 pim-reproduction/heavy-workloads/test.py',
        'python3 pim-reproduction/heavy-workloads/sweep.py --preset validation --tag validation-heavy-v1 --jobs 2',
        'python3 pim-reproduction/heavy-workloads/sweep.py --preset main --tag heavy-v1 --jobs 4',
        'python3 pim-reproduction/heavy-workloads/sweep.py --preset heldout --tag heldout-heavy-v1 --jobs 4',
        'python3 pim-reproduction/heavy-workloads/analyze.py',
        '```','',
        '- [All timing, latency, traffic and memory rows](analysis/all-runs.csv)',
        '- [Paired comparisons](analysis/comparisons.csv)',
        '- [Exact allocation-size histograms](analysis/request-profiles.json)',
        '- [Audit and accepted paths](analysis/audit.json)',
        '']
    (HERE/'RESULTS.md').write_text('\n'.join(text))
    print(json.dumps({k:v for k,v in audit.items() if k not in ['counter_log_sha256','result_paths']},indent=2))


if __name__=='__main__':main()
