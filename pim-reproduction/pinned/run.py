#!/usr/bin/env python3
"""Full scan/indexed/slab/pinned comparison, retaining historical experiments."""
import argparse
import fcntl
import hashlib
import importlib.util
import json
import re
from pathlib import Path

HERE=Path(__file__).resolve().parent
def module(name,path):
    spec=importlib.util.spec_from_file_location(name,path)
    value=importlib.util.module_from_spec(spec);spec.loader.exec_module(value);return value
heavy=module('pinned_heavy',HERE.parent/'heavy-workloads/run.py')
paper=heavy.paper;core=heavy.core
BACKEND=(core.ADAPTIVE/'buddy_backend.inc').read_text()
PINNED=(HERE/'buddy_backend.inc').read_text()
VARIANTS={
    'baseline':('scan','fixed',0,0),
    'indexed-adaptive64':('indexed','adaptive',0,0),
    'indexed-fixed2':('indexed','fixed',0,0),
    'slab-adaptive64':('indexed','adaptive',64,0),
    'slab-fixed2':('indexed','fixed',64,0),
    'slab-pinned-adaptive64':('indexed','adaptive',64,11),
    'slab-pinned-fixed2':('indexed','fixed',64,11),
}
WORKLOADS=['larson','cloud-cache','larson-wide','graph-churn','prim-sel']


def normalize(a):
    a.mode,a.policy,a.slab_kib,a.pin_levels=VARIANTS[a.variant]
    a.keep=a.max_keep=2;a.decay_ops=64;a.pressure_percent=75
    a.larson_max=8192 if a.workload=='larson-wide' else 4096
    return a


def allocator_source(a,pinned=True):
    source=paper.ALLOCATOR
    if a.slab_kib:
        source=(HERE.parent/'stacked/allocator.c').read_text().replace('#include "../combined/allocator.c"',source)
        source=source.replace('#include "../adaptive/buddy_backend.inc"',BACKEND)
    if pinned and a.pin_levels:
        assert source.count(BACKEND)>=1
        source=source.replace(BACKEND,PINNED)
    return source


def pinned_bytes(heap_kib,pin_levels):
    levels=(heap_kib*1024//4096).bit_length()
    return (sum(4 if i<4 else (1<<i)//4 for i in range(min(levels,pin_levels)))+7)&~7 if pin_levels else 0


def source_for(a):
    b=argparse.Namespace(**vars(a))
    b.workload={'larson-wide':'larson','graph-churn':'faimgraph'}.get(a.workload,a.workload)
    if b.workload in ['larson','cloud-cache']:source,manifest=heavy.source_for(b)
    else:source,manifest=paper.source_for(b)
    # The old cache wrapper fixes indexed mode. Only its leading feature flags
    # change here; application code and trace are shared by all seven variants.
    lists,lookup=core.MODES[a.mode]
    source=re.sub(r'^#define LISTS [01]\n',f'#define LISTS {lists}\n',source,count=1,flags=re.M)
    source=re.sub(r'^#define LOOKUP [01]\n',f'#define LOOKUP {lookup}\n',source,count=1,flags=re.M)
    old=allocator_source(a,False);new=allocator_source(a)
    assert source.count(old)==1
    source=source.replace(old,new,1)
    source=f'#define BUDDY_PIN_LEVELS {a.pin_levels}\n'+source
    manifest.update(workload=a.workload,allocator_sha256=hashlib.sha256(new.encode()).hexdigest(),
        backend_sha256=hashlib.sha256((PINNED if a.pin_levels else BACKEND).encode()).hexdigest(),
        pin_levels=a.pin_levels,pinned_metadata_bytes=pinned_bytes(a.heap_kib,a.pin_levels))
    if b.workload=='larson':manifest['selection']=f'Owner-local Larson; explicit uniform 8..{a.larson_max-1} B range.'
    return source,manifest


def parse(out,a,started,source_hash):
    result=paper.parse(out,a,started,source_hash)
    if a.workload in ['larson','larson-wide']:
        assert result['totals']['alloc_calls']==a.threads*(5*a.objects+a.operations)
        assert result['work_metrics']['alloc']['samples']==a.threads*a.operations
    if a.workload=='cloud-cache':
        counts=result['trace']['counts']
        assert result['totals']['alloc_calls']==sum(c['allocations'] for c in counts)
        assert result['work_metrics']['alloc']['samples']==sum(c['work_allocations'] for c in counts)
        assert result['work_metrics']['free']['samples']==sum(c['work_frees'] for c in counts)
    return result


def options():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--variant',choices=VARIANTS,default='slab-pinned-adaptive64')
    p.add_argument('--workload',choices=WORKLOADS,default='larson')
    p.add_argument('--threads',type=int,choices=[1,4],default=4)
    p.add_argument('--objects',type=int,default=64)
    p.add_argument('--operations',type=int,default=256)
    p.add_argument('--warmup',type=int,default=128)
    p.add_argument('--trace-offset',type=int,default=0)
    p.add_argument('--seed',type=int,default=4141)
    p.add_argument('--heap-kib',type=int,default=32768)
    p.add_argument('--budget-pages',type=int,default=64)
    p.add_argument('--items',type=int,default=32768)
    p.add_argument('--rounds',type=int,default=2)
    p.add_argument('--graph-edges',type=int,default=3072)
    p.add_argument('--fixture',action='store_true')
    p.add_argument('--validate',action='store_true')
    p.add_argument('--tag',default='pinned-v1')
    return p


def main():
    p=options();a=normalize(p.parse_args())
    if not 8<=a.objects<=64 or not 1<=a.operations<=2048:p.error('invalid object/operation count')
    if not 0<=a.warmup<=2048 or not 0<=a.trace_offset<=100000:p.error('invalid trace window')
    if not 64<=a.heap_kib<=32768 or a.heap_kib&(a.heap_kib-1):p.error('invalid heap')
    if not 0<=a.budget_pages<=64 or not 1<=a.rounds<=8:p.error('invalid budget/round count')
    if not a.tag or any(s in a.tag for s in ['/','\\','..']):p.error('invalid tag')
    core.HERE=HERE;core.source_for=source_for;core.parse=parse
    identity=hashlib.sha256(json.dumps(vars(a),sort_keys=True).encode()).hexdigest()
    locks=HERE/'results'/a.tag/'.locks';locks.mkdir(parents=True,exist_ok=True)
    with (locks/(identity+'.lock')).open('a') as lock:
        fcntl.flock(lock,fcntl.LOCK_EX);core.run(a)


if __name__=='__main__':main()
