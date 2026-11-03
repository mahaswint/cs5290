#!/usr/bin/env python3
"""Run large-request Larson and a real-trace LRU write-cache kernel."""
import argparse
import fcntl
import hashlib
import importlib.util
import json
from pathlib import Path

HERE=Path(__file__).resolve().parent
def module(name,path):
    spec=importlib.util.spec_from_file_location(name,path)
    value=importlib.util.module_from_spec(spec);spec.loader.exec_module(value);return value
paper=module('heavy_paper',HERE.parent/'paper-workloads/run.py')
core=paper.core
cache_trace=module('heavy_cache_trace',HERE/'trace.py')


def source_for(a):
    if a.workload=='larson':
        source,manifest=paper.source_for(a)
        manifest.update(larson_max_exclusive=a.larson_max,
            selection='Explicit mixed-size 8..4095 B Larson parameterization; default old 8..999 B workload remains unchanged.')
        return source,manifest
    allocator=paper.ALLOCATOR
    if getattr(a,'slab_kib',0):
        allocator=(HERE.parent/'stacked/allocator.c').read_text().replace('#include "../combined/allocator.c"',allocator)
        allocator=allocator.replace('#include "../adaptive/buddy_backend.inc"',(core.ADAPTIVE/'buddy_backend.inc').read_text())
    header,manifest=cache_trace.make_trace(a)
    definitions=dict(NR_TASKLETS=a.threads,POLICY=core.shared.POLICIES[a.policy],LISTS=1,LOOKUP=1,
        HEAP_BYTES=a.heap_kib*1024,KEEP_PAGES=a.keep,MAX_KEEP=a.max_keep,EMPTY_BUDGET=a.budget_pages,
        DECAY_OPS=a.decay_ops,PRESSURE_PERCENT=a.pressure_percent,VALIDATE=int(a.validate),
        POOL_SIZE=a.objects,INPUT_ITEMS=a.items,TRACE_SEED=a.seed,REPEATS=a.rounds,PAPER_KIND=4)
    if getattr(a,'slab_kib',0):definitions['SLAB_BYTES']=a.slab_kib*1024
    harness=(HERE.parent/'paper-workloads/harness.h').read_text().replace('#include "allocator.c"',allocator)
    harness=harness.replace('#include "trace.inc"',header)
    body=(HERE/'cache.c').read_text().replace('#include "harness.h"',harness)
    manifest['allocator_sha256']=hashlib.sha256(allocator.encode()).hexdigest()
    return ''.join(f'#define {k} {v}\n' for k,v in definitions.items())+body,manifest


def parse(out,a,started,source_hash):
    result=paper.parse(out,a,started,source_hash)
    if a.workload=='cloud-cache':
        tr=result['trace'];counts=tr['counts']
        assert result['totals']['alloc_calls']==sum(c['allocations'] for c in counts)
        assert result['work_metrics']['alloc']['samples']==sum(c['work_allocations'] for c in counts)
        assert result['work_metrics']['free']['samples']==sum(c['work_frees'] for c in counts)
    return result


def options():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--workload',choices=['larson','cloud-cache'],default='larson')
    p.add_argument('--slab-kib',type=int,choices=[16,32,64],default=argparse.SUPPRESS)
    p.add_argument('--mode',choices=['indexed'],default='indexed')
    p.add_argument('--policy',choices=['fixed','adaptive'],default='adaptive')
    p.add_argument('--threads',type=int,choices=[1,4],default=4)
    p.add_argument('--objects',type=int,default=64)
    p.add_argument('--operations',type=int,default=256)
    p.add_argument('--larson-max',type=int,choices=[4096,8192],default=4096)
    p.add_argument('--warmup',type=int,default=128)
    p.add_argument('--trace-offset',type=int,default=0)
    p.add_argument('--seed',type=int,default=4141)
    p.add_argument('--heap-kib',type=int,default=32768)
    p.add_argument('--keep',type=int,default=2)
    p.add_argument('--max-keep',type=int,default=2)
    p.add_argument('--budget-pages',type=int,default=64)
    p.add_argument('--decay-ops',type=int,default=128)
    p.add_argument('--pressure-percent',type=int,default=75)
    p.add_argument('--items',type=int,default=32768)
    p.add_argument('--rounds',type=int,default=2)
    p.add_argument('--validate',action='store_true')
    p.add_argument('--tag',default='heavy-v1')
    return p


def main():
    p=options();a=p.parse_args()
    if not 8<=a.objects<=64 or not 1<=a.operations<=2048:p.error('invalid object/operation count')
    if not 0<=a.warmup<=2048 or not 0<=a.trace_offset<=100000:p.error('invalid trace window')
    if not 1024<=a.heap_kib<=32768 or a.heap_kib&(a.heap_kib-1):p.error('invalid heap')
    if a.decay_ops<=0 or a.decay_ops&(a.decay_ops-1):p.error('invalid interval')
    if not 0<=a.budget_pages<=64 or not 0<=a.keep<=8 or not 0<=a.max_keep<=8:p.error('invalid retention')
    if not a.tag or any(s in a.tag for s in ['/','\\','..']):p.error('invalid tag')
    core.HERE=HERE;core.source_for=source_for;core.parse=parse
    identity=hashlib.sha256(json.dumps(vars(a),sort_keys=True).encode()).hexdigest()
    locks=HERE/'results'/a.tag/'.locks';locks.mkdir(parents=True,exist_ok=True)
    with (locks/(identity+'.lock')).open('a') as lock:
        fcntl.flock(lock,fcntl.LOCK_EX);core.run(a)


if __name__=='__main__':main()
