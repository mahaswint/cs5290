#!/usr/bin/env python3
"""Paper workload request shapes on the unchanged seven-stage allocator stack."""
import argparse
import fcntl
import hashlib
import importlib.util
import json
from pathlib import Path

HERE=Path(__file__).resolve().parent
spec=importlib.util.spec_from_file_location('paper_stack',HERE.parent/'run.py')
stack=importlib.util.module_from_spec(spec);spec.loader.exec_module(stack)
core=stack.core
VARIANTS=stack.VARIANTS
WORKLOADS=['pim-list','pim-array','micro-32','micro-256','micro-4096','kv-growth']


def options():
    p=stack.options()
    p.description=__doc__
    for action in p._actions:
        if action.dest=='workload':action.choices=WORKLOADS;action.default='pim-list'
        if action.dest=='threads':action.choices=[1,4,16]
        if action.dest=='tag':action.default='paper-v1'
    p.add_argument('--requests',type=int,default=128)
    p.add_argument('--prompt-tokens',type=int,default=128)
    p.add_argument('--decode-tokens',type=int,default=256)
    return p


def normalize(a):
    return stack.normalize(a)


def source_for(a):
    if a.workload in ['pim-list','pim-array']:
        return stack.source_for(a)
    size=512 if a.workload=='kv-growth' else int(a.workload.split('-')[1])
    kv=a.workload=='kv-growth'
    count=a.prompt_tokens+a.decode_tokens if kv else a.requests
    warm=a.prompt_tokens if kv else 0
    allocator=stack.allocator_source(a)
    header=''
    harness=(HERE.parent.parent/'paper-workloads/harness.h').read_text()
    harness=harness.replace('#include "allocator.c"',allocator).replace('#include "trace.inc"',header)
    body=(HERE/'requests.c').read_text().replace('#include "harness.h"',harness)
    lists,lookup=core.MODES[a.mode]
    definitions=dict(NR_TASKLETS=a.threads,POLICY=core.shared.POLICIES[a.policy],LISTS=lists,LOOKUP=lookup,
        HEAP_BYTES=a.heap_kib*1024,KEEP_PAGES=a.keep,MAX_KEEP=a.max_keep,EMPTY_BUDGET=a.budget_pages,
        DECAY_OPS=a.decay_ops,PRESSURE_PERCENT=a.pressure_percent,VALIDATE=int(a.validate),
        PAPER_KIND=4,BUDDY_PIN_LEVELS=a.pin_levels,REQUEST_BYTES=size,REQUEST_COUNT=count,
        WARM_REQUESTS=warm,KV_GROWTH=int(kv))
    if a.slab_kib:definitions['SLAB_BYTES']=a.slab_kib*1024
    source=''.join(f'#define {k} {v}\n' for k,v in definitions.items())+body
    schedule=dict(threads=a.threads,bytes=size,count_per_tasklet=count,warmup_per_tasklet=warm,
        timed_per_tasklet=count-warm,free_order='allocation order after work',payload_in_work=kv)
    manifest=dict(workload=a.workload,source='PIM-malloc, HPCA 2026, Sections V and VI-A',
        paper_sha256='ba1a8f6143102ca3fba63651a67b2dbf457cecc427a11e52cc2e6c1b3f2f0212',
        schedule=schedule,trace_sha256=hashlib.sha256(json.dumps(schedule,sort_keys=True).encode()).hexdigest(),
        allocator_sha256=hashlib.sha256(allocator.encode()).hexdigest(),
        backend_sha256=hashlib.sha256((stack.PINNED if a.pin_levels else stack.BACKEND).encode()).hexdigest(),
        pin_levels=a.pin_levels,pinned_metadata_bytes=stack.pinned_bytes(a.heap_kib,a.pin_levels),
        limitations=('Allocator-only replay of 512 B KV-block growth, one stream/tasklet, 128 prompt + 256 decode tokens. '
            'No GEMV/softmax, model weights, 100-request arrival process, host transfers or LLMServingSim. '
            'Not an end-to-end LLM attention or serving result.' if kv else
            'Paper request sizes/count/tasklets; maintained scan fixed-2 baseline, not paper straw-man. '
            'MRAM pointer recording is outside per-call probes but inside work; checking and reclamation follow work.'))
    return source,manifest


def parse(out,a,started,source_hash):
    result=stack.paper.parse(out,a,started,source_hash)
    if a.workload not in ['pim-list','pim-array']:
        kv=a.workload=='kv-growth'
        count=a.prompt_tokens+a.decode_tokens if kv else a.requests
        work=a.decode_tokens if kv else count
        assert result['totals']['alloc_calls']==result['totals']['free_calls']==a.threads*count
        assert result['work_metrics']['alloc']['samples']==a.threads*work
        assert result['work_metrics']['free']['samples']==0
    return result


def main():
    p=options();a=normalize(p.parse_args())
    if not 1<=a.requests<=512:p.error('requests must be 1..512')
    if not 0<=a.prompt_tokens<=256 or not 1<=a.decode_tokens<=256:p.error('invalid token counts')
    if not 64<=a.heap_kib<=32768 or a.heap_kib&(a.heap_kib-1):p.error('invalid heap')
    if a.graph_edges<48 or a.graph_edges>12288 or a.graph_edges%3:p.error('invalid graph sample size')
    if not 0<=a.budget_pages<=64:p.error('invalid budget')
    if not a.tag or any(s in a.tag for s in ['/','\\','..']):p.error('invalid tag')
    core.HERE=HERE;core.source_for=source_for;core.parse=parse
    identity=hashlib.sha256(json.dumps(vars(a),sort_keys=True).encode()).hexdigest()
    locks=HERE/'results'/a.tag/'.locks';locks.mkdir(parents=True,exist_ok=True)
    with (locks/(identity+'.lock')).open('a') as lock:
        fcntl.flock(lock,fcntl.LOCK_EX);core.run(a)


if __name__=='__main__':main()
