#!/usr/bin/env python3
"""Run paper-derived DPU kernels against frozen scan/indexed allocators."""
import argparse
import csv
import fcntl
import hashlib
import importlib.util
import json
import statistics
import sys
import time
from pathlib import Path

HERE=Path(__file__).resolve().parent
COMBINED=HERE.parent/'combined'
sys.path.insert(0,str(COMBINED))
spec=importlib.util.spec_from_file_location('combined_runner',COMBINED/'run.py')
core=importlib.util.module_from_spec(spec);spec.loader.exec_module(core)
spec=importlib.util.spec_from_file_location('paper_trace',HERE/'trace.py')
trace=importlib.util.module_from_spec(spec);spec.loader.exec_module(trace)
ALLOCATOR=core.allocator_source()
KINDS={'larson':0,'pim-list':1,'pim-array':2,'faimgraph':1,'prim-sel':3}


def source_for(a):
    allocator=ALLOCATOR
    slab_kib=getattr(a,'slab_kib',0)
    if slab_kib:
        allocator=(HERE.parent/'stacked/allocator.c').read_text().replace('#include "../combined/allocator.c"',ALLOCATOR)
        allocator=allocator.replace('#include "../adaptive/buddy_backend.inc"',(core.ADAPTIVE/'buddy_backend.inc').read_text())
    header,manifest=trace.make_trace(a)
    body=(HERE/('larson.c' if a.workload=='larson' else 'selection.c' if a.workload=='prim-sel' else 'graph.c')).read_text()
    body=body.replace('#include "driver.c"',(HERE/'driver.c').read_text())
    harness=(HERE/'harness.h').read_text().replace('#include "allocator.c"',allocator)
    harness=harness.replace('#include "trace.inc"',header)
    body=body.replace('#include "harness.h"',harness)
    lists,lookup=core.MODES[a.mode]
    definitions=dict(NR_TASKLETS=a.threads,POLICY=core.shared.POLICIES[a.policy],LISTS=lists,LOOKUP=lookup,
                     HEAP_BYTES=a.heap_kib*1024,KEEP_PAGES=a.keep,MAX_KEEP=a.max_keep,EMPTY_BUDGET=a.budget_pages,
                     DECAY_OPS=a.decay_ops,PRESSURE_PERCENT=a.pressure_percent,VALIDATE=int(a.validate),
                     POOL_SIZE=a.objects,INPUT_ITEMS=a.items,TRACE_SEED=a.seed,REPEATS=a.rounds,PAPER_KIND=KINDS[a.workload])
    if slab_kib:definitions['SLAB_BYTES']=slab_kib*1024
    source=''.join(f'#define {k} {v}\n' for k,v in definitions.items())+body
    manifest['allocator_sha256']=hashlib.sha256(allocator.encode()).hexdigest()
    return source,manifest


def parse(out,a,started,source_hash):
    events=[];stats=[];snapshots=[];backend=wram=None;passed=False
    for line in (out/'simulate.log').read_text().splitlines():
        f=line.split(',')
        if f[0]=='POLICY_EVENT':events.append(dict(zip(['kind','thread','index','begin_cycle','end_cycle','cycles'],[f[1]]+list(map(int,f[2:])),strict=True)))
        elif f[0]=='POLICY_STATS':stats.append(dict(zip(core.shared.STATS,map(int,f[1:]),strict=True)))
        elif f[0]=='POLICY_SNAPSHOT':snapshots.append(dict(zip(['phase','round','requested_bytes','slot_bytes','reserved_bytes','empty_bytes'],map(int,f[1:]),strict=True)))
        elif f[0]=='POLICY_BACKEND':backend=dict(zip(['used_bytes','peak_bytes','allocations','frees','failed_attempts'],map(int,f[1:]),strict=True))
        elif f[0]=='POLICY_WRAM':wram=dict(zip(['heap_end_address','cache_descriptor_bytes','stats_bytes'],map(int,f[1:]),strict=True))
        elif line=='POLICY_CHECK,PASS':passed=True
    assert passed and len(stats)==a.threads and len(snapshots)==3 and backend is not None and wram is not None
    assert not backend['used_bytes'] and backend['allocations']==backend['frees'] and wram['heap_end_address']<=65536
    for s in stats:
        assert s['successes']==s['free_calls'] and not s['failures'] and not s['live_slots'] and not s['retained']
        for kind,key in [('alloc','alloc_calls'),('free','free_calls')]:
            rows=[e for e in events if e['kind']==kind and e['thread']==s['thread']]
            assert len(rows)==s[key] and [e['index'] for e in rows]==list(range(s[key]))
    for filename,rows in [('events.csv',events),('stats.csv',stats),('snapshots.csv',snapshots)]:
        with (out/filename).open('w',newline='') as stream:
            w=csv.DictWriter(stream,fieldnames=list(rows[0]));w.writeheader();w.writerows(rows)
    def metrics(values):
        return dict(samples=len(values),mean_us=statistics.mean(values) if values else None,
                    p99_us=core.shared.percentile(values,.99),max_us=max(values) if values else None)
    m={kind:metrics([e['cycles']/350 for e in events if e['kind']==kind]) for kind in ['alloc','free','init','work','drain','total']}
    windows=[e for e in events if e['kind']=='work'];assert len(windows)==a.threads
    spans={e['thread']:e for e in windows}
    makespan=(max(e['end_cycle'] for e in windows)-min(e['begin_cycle'] for e in windows))/350
    wm={kind:metrics([e['cycles']/350 for e in events if e['kind']==kind and
                     spans[e['thread']]['begin_cycle']<=e['begin_cycle'] and e['end_cycle']<=spans[e['thread']]['end_cycle']])
        for kind in ['alloc','free']}
    totals={k:sum(s[k] for s in stats) for k in core.shared.STATS if k!='thread'}
    if a.workload=='prim-sel':assert not totals['alloc_calls'] and not totals['free_calls']
    if a.workload=='larson':
        assert totals['alloc_calls']==a.threads*(5*a.objects+a.operations)
        assert wm['alloc']['samples']==wm['free']['samples']==a.threads*a.operations
    owner_bytes=(a.heap_kib*1024//4096)*8 if core.MODES[a.mode][1] else 0
    result=dict(configuration=vars(a),correctness_passed=True,outcome='completed',frequency_mhz=350,
                metrics=m,work_metrics=wm,work_makespan_us=makespan,totals=totals,backend=backend,wram=wram,
                owner_table_mram_bytes=owner_bytes,peak_payload_plus_owner_bytes=backend['peak_bytes']+owner_bytes,
                source_sha256=source_hash,simulator_sha256=hashlib.sha256((core.ADAPTIVE/'runtime/build/uPIMulator').read_bytes()).hexdigest(),
                trace=json.loads((out/'trace.json').read_text()),host_runtime_seconds=time.time()-started)
    temp=out/'.summary.json.tmp';temp.write_text(json.dumps(result,indent=2)+'\n');temp.replace(out/'summary.json')
    return result


def options():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--workload',choices=KINDS,default='larson')
    p.add_argument('--mode',choices=core.MODES,default='indexed')
    p.add_argument('--slab-kib',type=int,choices=[16,32,64],default=argparse.SUPPRESS,
                   help='Add the 4 KiB slab class to indexed fixed/adaptive; omitted preserves the original allocator')
    p.add_argument('--policy',choices=core.shared.POLICIES,default='adaptive')
    p.add_argument('--threads',type=int,choices=[1,4,8,16],default=1)
    p.add_argument('--objects',type=int,default=512)
    p.add_argument('--operations',type=int,default=2048)
    p.add_argument('--graph-edges',type=int,default=3072)
    p.add_argument('--items',type=int,default=32768)
    p.add_argument('--rounds',type=int,default=2)
    p.add_argument('--seed',type=int,default=4141)
    p.add_argument('--heap-kib',type=int,default=32768)
    p.add_argument('--keep',type=int,default=2)
    p.add_argument('--max-keep',type=int,default=2)
    p.add_argument('--budget-pages',type=int,default=4)
    p.add_argument('--decay-ops',type=int,default=64)
    p.add_argument('--pressure-percent',type=int,default=75)
    p.add_argument('--validate',action='store_true')
    p.add_argument('--fixture',action='store_true',help='Star graph fixture for correctness only')
    p.add_argument('--tag',default='papers-v3')
    return p


def main():
    p=options();a=p.parse_args()
    if (HERE/'.stop').exists() and a.tag in (HERE/'.stop').read_text().splitlines():
        p.error('Experiment launch paused by .stop sentinel')
    if hasattr(a,'slab_kib') and (a.mode!='indexed' or a.policy not in ['fixed','adaptive']):
        p.error('slabs require --mode indexed and --policy fixed or adaptive')
    if not 8<=a.objects<=4096 or not 1<=a.operations<=65536:p.error('invalid pool/operation count')
    if a.graph_edges<48 or a.graph_edges>12288 or a.graph_edges%3:p.error('graph-edges must be a multiple of 3 in 48..12288')
    if a.items<32*a.threads or a.items%(32*a.threads):p.error('items must be a positive multiple of 32*tasklets')
    if not 1<=a.rounds<=8:p.error('rounds must be 1..8')
    if a.decay_ops<=0 or a.decay_ops&(a.decay_ops-1):p.error('decay-ops must be a positive power of two')
    if not a.tag or any(x in a.tag for x in ['/','\\','..']):p.error('invalid tag')
    if not 4<=a.heap_kib<=32768 or a.heap_kib&(a.heap_kib-1):p.error('invalid heap-kib')
    core.HERE=HERE;core.source_for=source_for;core.parse=parse
    # A resumed sweep may meet a configuration pre-run by another process.
    # Serialize that identity before checking its summary or touching artifacts.
    identity=hashlib.sha256(json.dumps(vars(a),sort_keys=True).encode()).hexdigest()
    locks=HERE/'results'/a.tag/'.locks';locks.mkdir(parents=True,exist_ok=True)
    with (locks/(identity+'.lock')).open('a') as lock:
        fcntl.flock(lock,fcntl.LOCK_EX)
        core.run(a)


if __name__=='__main__':main()
