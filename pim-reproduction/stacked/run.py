#!/usr/bin/env python3
"""Run slab + indexed + fixed/adaptive with the maintained simulator probes."""
import argparse
import fcntl
import hashlib
import importlib.util
import json
import sys
from pathlib import Path

HERE=Path(__file__).resolve().parent
def module(name,path):
    spec=importlib.util.spec_from_file_location(name,path)
    result=importlib.util.module_from_spec(spec);spec.loader.exec_module(result);return result
core=module('stacked_core',HERE.parent/'combined/run.py')
paper=module('stacked_metrics',HERE.parent/'paper-workloads/run.py')
BASELINE=core.allocator_source()


def allocator_source():
    return (HERE/'allocator.c').read_text().replace('#include "../combined/allocator.c"',BASELINE).replace(
        '#include "../adaptive/buddy_backend.inc"',(core.ADAPTIVE/'buddy_backend.inc').read_text())


def source_for(a):
    definitions=dict(NR_TASKLETS=a.threads,POLICY=core.shared.POLICIES[a.policy],LISTS=1,LOOKUP=1,
        SLAB_BYTES=a.slab_kib*1024,HEAP_BYTES=a.heap_kib*1024,KEEP_PAGES=a.keep,MAX_KEEP=a.max_keep,
        EMPTY_BUDGET=a.budget_pages,DECAY_OPS=a.decay_ops,PRESSURE_PERCENT=a.pressure_percent,
        VALIDATE=int(a.validate),OBJECTS=a.objects,OPERATIONS=a.operations,REPEATS=a.rounds,
        OBJECT_SIZE=a.size,TRACE_SEED=a.seed,BENCH_WORKLOAD={'burst':0,'pool':1,'sparse':2,'mixed':3}[a.workload])
    harness=(HERE.parent/'paper-workloads/harness.h').read_text().replace('#include "allocator.c"',allocator_source())
    harness=harness.replace('#include "trace.inc"','')
    body=(HERE/'benchmark.c').read_text().replace('#include "harness.h"',harness)
    source=''.join(f'#define {k} {v}\n' for k,v in definitions.items())+body
    manifest=dict(workload=a.workload,threads=a.threads,objects=a.objects,operations=a.operations,
        size=a.size,rounds=a.rounds,seed=a.seed,
        description='Synthetic dense bursts, persistent replacements, sparse survivors, or mixed size boundaries.',
        trace_sha256=hashlib.sha256(json.dumps({k:v for k,v in definitions.items() if k in [
            'NR_TASKLETS','OBJECTS','OPERATIONS','REPEATS','OBJECT_SIZE','TRACE_SEED','BENCH_WORKLOAD']},sort_keys=True).encode()).hexdigest(),
        allocator_sha256=hashlib.sha256(allocator_source().encode()).hexdigest())
    return source,manifest


def parse(out,a,started,source_hash):
    result=paper.parse(out,a,started,source_hash)
    expected=a.objects+a.operations if a.workload=='pool' else a.objects if a.workload=='sparse' else a.objects*a.rounds
    assert result['totals']['alloc_calls']==expected*a.threads
    result.update(slab_bytes=a.slab_kib*1024,retention_budget_bytes_per_tasklet=a.budget_pages*4096,
        retention_units='retained counts physical 4 KiB units; class targets count empty spans',
        size_4096_capacity=(a.slab_kib*1024-24)//4096 if a.slab_kib else 0,
        capacity_profile='64 KiB WRAM endpoint; 24 x 1536 B stack reservations; physical stack/IRAM validation not claimed')
    (out/'summary.json').write_text(json.dumps(result,indent=2)+'\n')
    return result


def options():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--slab-kib',type=int,choices=[0,16,32,64],default=32)
    p.add_argument('--policy',choices=['fixed','adaptive'],default='adaptive')
    p.add_argument('--mode',choices=['indexed'],default='indexed')
    p.add_argument('--threads',type=int,choices=[1,4,8,16],default=1)
    p.add_argument('--workload',choices=['burst','pool','sparse','mixed'],default='burst')
    p.add_argument('--objects',type=int,default=32)
    p.add_argument('--operations',type=int,default=256)
    p.add_argument('--size',type=int,default=4096)
    p.add_argument('--rounds',type=int,default=4)
    p.add_argument('--heap-kib',type=int,default=32768)
    p.add_argument('--keep',type=int,default=2)
    p.add_argument('--max-keep',type=int,default=2)
    p.add_argument('--budget-pages',type=int,default=64,help='Empty-byte budget in physical 4 KiB units, shared by all classes per tasklet')
    p.add_argument('--decay-ops',type=int,default=128)
    p.add_argument('--pressure-percent',type=int,default=75)
    p.add_argument('--seed',type=int,default=4141)
    p.add_argument('--validate',action='store_true')
    p.add_argument('--tag',default='stacked-v1')
    return p


def main():
    p=options();a=p.parse_args()
    if not 4<=a.objects<=128 or a.objects&(a.objects-1):p.error('objects must be a power of two in 4..128')
    if not 1<=a.operations<=8192 or not 1<=a.rounds<=8:p.error('invalid operation/round count')
    if not 16<=a.size<=8192 or a.size%8:p.error('size must be a multiple of 8 in 16..8192')
    if not 4<=a.heap_kib<=32768 or a.heap_kib&(a.heap_kib-1):p.error('heap must be a power of two in 4..32768 KiB')
    if a.decay_ops<=0 or a.decay_ops&(a.decay_ops-1):p.error('decay-ops must be a positive power of two')
    if not 0<=a.keep<=8 or not 0<=a.max_keep<=8 or not 0<=a.budget_pages<=256:p.error('invalid retention setting')
    if not 1<=a.pressure_percent<=100 or not 0<=a.seed<=0x3fffffff:p.error('invalid pressure or seed')
    if not a.tag or any(s in a.tag for s in ['/','\\','..']):p.error('invalid tag')
    core.HERE=HERE;core.source_for=source_for;core.parse=parse
    locks=HERE/'results'/a.tag/'.locks';locks.mkdir(parents=True,exist_ok=True)
    identity=hashlib.sha256(json.dumps(vars(a),sort_keys=True).encode()).hexdigest()
    with (locks/(identity+'.lock')).open('a') as lock:
        fcntl.flock(lock,fcntl.LOCK_EX);core.run(a)


if __name__=='__main__':main()
