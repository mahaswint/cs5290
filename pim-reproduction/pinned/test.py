#!/usr/bin/env python3
"""Differential buddy, layered allocator and application sanitizer tests."""
import hashlib
import json
import os
import re
import subprocess
import run

HERE=run.HERE
native=run.module('pinned_native_harness',HERE.parent/'paper-workloads/test.py')
FUNCTIONS=sorted(set(re.findall(r'\b([A-Za-z_]\w*)\([^;{}]*\)\s*[{;]',run.BACKEND))-
                 {'if','for','while','assert','mem_alloc','memset','mram_read','mram_write','mutex_lock','mutex_unlock'})
# Explicit names avoid confusing calls/annotations with backend declarations.
NAMES=['BuddyAllocator','buddyAllocatorMutex','initBuddyAllocator','finiBuddyAllocator','buddyAllocate','buddyFree',
       'safeBuddyAllocate','safeBuddyFree','buddyAllocateForward','buddyFreeBackward','setAllocationsBackward',
       'initMetadata','getMetadataSize','getMetadataSizePerLevel','getMramAddress','getLeafSize','getNumLeafs',
       'getLeafIndex','getMetadataIndex','getMetadataShift','getMetadata','setMetadata','flushMetadata',
       'getAllocationLevel','isPowOf2','powOf2','logOf2','syncPinnedMetadata']


def renamed(source,prefix):
    mapped={n:(prefix+'BuddyAllocator' if n=='BuddyAllocator' else prefix.lower()+'_'+n) for n in NAMES}
    return ''.join(f'#define {k} {v}\n' for k,v in mapped.items())+source+'\n'+''.join(f'#undef {k}\n' for k in mapped)


def app_configurations():
    for workload in run.WORKLOADS:
        for variant in run.VARIANTS:
            a=run.options().parse_args([])
            vars(a).update(workload=workload,variant=variant,threads=1,objects=32,operations=64,
                warmup=32,items=256,rounds=2,fixture=True,heap_kib=4096,validate=True)
            yield run.normalize(a)


def compile_and_run(name,source,checks,details):
    folder=HERE/'validation';folder.mkdir(exist_ok=True)
    path=folder/(name+'.c');path.write_text(source);exe=folder/name;log=folder/(name+'.log')
    command=['cc','-std=gnu11','-O1','-g','-no-pie','-fsanitize=address,undefined',
        '-fno-sanitize-recover=all','-fno-omit-frame-pointer',
        '-Wno-pointer-to-int-cast','-Wno-int-to-pointer-cast','-Wno-int-conversion',
        '-I'+str(run.core.ADAPTIVE/'tests/shim'),str(path),'-o',str(exe)]
    with log.open('w') as stream:
        subprocess.run(command,stdout=stream,stderr=subprocess.STDOUT,check=True)
        subprocess.run([str(exe)],stdout=stream,stderr=subprocess.STDOUT,check=True,
            env=os.environ|{'ASAN_OPTIONS':'detect_leaks=0'})
    checks.append(dict(name=name,passed=True,source_sha256=hashlib.sha256(path.read_bytes()).hexdigest(),
        log_sha256=hashlib.sha256(log.read_bytes()).hexdigest(),**details))
    print('PASS:',name,flush=True)


def main():
    checks=[]
    backend=(HERE/'backend_test.c').read_text()
    backend=backend.replace('#include "reference_backend.inc"',renamed(run.BACKEND,'Ref'))
    backend=backend.replace('#include "pinned_backend.inc"',renamed(run.PINNED,'Pin'))
    for levels in [8,11,12]:
        for buffer in [8,64]:
            source=f'#define BUDDY_PIN_LEVELS {levels}\n#define TEST_BUFFER {buffer}\n'+backend
            compile_and_run(f'host-backend-p{levels}-b{buffer}',source,checks,dict(kind='backend',pin_levels=levels,buffer_bytes=buffer))
    for slab in [0,64]:
        for policy in ['fixed','adaptive']:
            for budget in ([4,64] if slab else [64]):
                a=run.normalize(run.options().parse_args(['--variant','slab-pinned-'+('fixed2' if policy=='fixed' else 'adaptive64')]))
                a.slab_kib=slab
                source=(HERE.parent/'stacked/host_test.c').read_text().replace('#include "allocator.c"',run.allocator_source(a))
                definitions=dict(NR_TASKLETS=4,SLAB_BYTES=slab*1024,POLICY=run.core.shared.POLICIES[policy],
                    BUDDY_PIN_LEVELS=11,LISTS=1,LOOKUP=1,KEEP_PAGES=2,MAX_KEEP=2,EMPTY_BUDGET=budget,
                    DECAY_OPS=64,PRESSURE_PERCENT=75)
                source=''.join(f'#define {k} {v}\n' for k,v in definitions.items())+source
                compile_and_run(f'host-layered-s{slab}-{policy}-b{budget}',source,checks,
                    dict(kind='layered',slab_kib=slab,policy=policy,budget_pages=budget))
    for a in app_configurations():
        source,_=run.source_for(a)
        source=native.PRELUDE+source+native.POSTLUDE
        compile_and_run(f'host-app-{a.workload}-{a.variant}',source,checks,dict(kind='application',configuration=vars(a)))
    (HERE/'validation/checks.json').write_text(json.dumps(checks,indent=2)+'\n')


if __name__=='__main__':main()
