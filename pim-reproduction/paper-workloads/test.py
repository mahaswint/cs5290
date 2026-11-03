#!/usr/bin/env python3
"""Sanitizer tests with interval and exact adjacency oracles for kernel ports."""
import json
import argparse
import hashlib
import os
import subprocess
from pathlib import Path
import run

HERE=Path(__file__).resolve().parent
PRELUDE=r'''
#define _GNU_SOURCE
#define HOST_TEST 1
#include "common.h"
#include <sys/mman.h>
#define BARRIER_INIT(name,n) static int name
static void barrier_wait(int *b){(void)b;}
static void mem_reset(void){host_wram_used=0;}
static unsigned host_storage_offset;
static void *host_storage(unsigned bytes){
  host_storage_offset=(host_storage_offset+7)&~7U;
  assert(host_storage_offset+bytes<=host_mram_size);
  void *p=host_mram+host_storage_offset;host_storage_offset+=bytes;memset(p,0,bytes);return p;
}
struct HostInterval{unsigned char *p;unsigned bytes;};
static struct HostInterval host_intervals[8192];static unsigned host_interval_count;
static void host_add_interval(void *p,unsigned bytes){
  for(unsigned i=0;i<host_interval_count;++i){
    struct HostInterval *o=&host_intervals[i];
    assert((unsigned char *)p+bytes<=o->p || o->p+o->bytes<=(unsigned char *)p);
  }
  assert(host_interval_count<8192);host_intervals[host_interval_count++]=(struct HostInterval){p,bytes};
}
static void host_remove_interval(void *p,unsigned bytes){
  unsigned i=0;while(i<host_interval_count && host_intervals[i].p!=p)++i;
  assert(i<host_interval_count && host_intervals[i].bytes==bytes);
  host_intervals[i]=host_intervals[--host_interval_count];
}
static void host_expect_neighbor(unsigned u,unsigned v);
#define main paper_main
'''
POSTLUDE=r'''
#undef main
#if PAPER_KIND==1 || PAPER_KIND==2
static unsigned char *host_seen;
static void host_expect_neighbor(unsigned u,unsigned v){
  unsigned found=0;
  for(unsigned i=0;i<sizeof(paper_exact_edges)/sizeof(paper_exact_edges[0]);++i)
    if(paper_exact_edges[i].index==u && paper_exact_edges[i].value==v){found=1;break;}
  assert(found && !host_seen[u*PAPER_VERTICES+v]);host_seen[u*PAPER_VERTICES+v]=1;
}
#else
static void host_expect_neighbor(unsigned u,unsigned v){(void)u;(void)v;assert(0);}
#endif
int main(void){
  host_mram_size=HEAP_BYTES+8*1024*1024;
  host_mram=mmap(NULL,host_mram_size,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_32BIT,-1,0);
  assert(host_mram!=MAP_FAILED && (uintptr_t)host_mram+host_mram_size<0x80000000U);
  host_storage_offset=HEAP_BYTES+256*1024;
#if PAPER_KIND==1 || PAPER_KIND==2
  host_seen=calloc(PAPER_VERTICES*PAPER_VERTICES,1);assert(host_seen);
#endif
  assert(paper_main()==0);assert(host_interval_count==0 && policy_backend.used==0);
  munmap(host_mram,host_mram_size);puts("PASS: independent intervals, payloads, exact graph neighbors, complete coalescing");
  return 0;
}
'''


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--decay-ops',type=int,default=64)
    p.add_argument('--mode',choices=['all',*run.core.MODES],default='all')
    p.add_argument('--policy',choices=['all','fixed','adaptive'],default='all')
    p.add_argument('--output-tag',default=None)
    p.add_argument('--slab-kib',type=int,choices=[16,32,64],default=None)
    p.add_argument('--budget-pages',type=int,default=4)
    args=p.parse_args()
    if args.decay_ops<=0 or args.decay_ops&(args.decay_ops-1):p.error('decay-ops must be a positive power of two')
    if args.output_tag and any(s in args.output_tag for s in ['/','\\','..']):p.error('invalid output tag')
    if args.slab_kib and args.mode!='indexed':p.error('slabs require --mode indexed')
    if not 0<=args.budget_pages<=64:p.error('budget-pages must be in 0..64')
    if not args.output_tag and (args.decay_ops!=64 or args.mode!='all' or args.policy!='all'
                               or args.slab_kib or args.budget_pages!=4):
        p.error('use --output-tag for a non-default test matrix to preserve the original evidence')
    out=HERE/'validation'
    if args.output_tag:out=out/args.output_tag
    out.mkdir(parents=True,exist_ok=True);checks=[]
    for workload in run.KINDS:
        for mode in (run.core.MODES if args.mode=='all' else [args.mode]):
            for policy in (['fixed','adaptive'] if args.policy=='all' else [args.policy]):
                a=run.options().parse_args([])
                a.workload=workload;a.mode=mode;a.policy=policy;a.threads=1
                a.objects=64;a.operations=128;a.items=256;a.rounds=2;a.fixture=True;a.heap_kib=1024;a.validate=True
                a.decay_ops=args.decay_ops
                a.budget_pages=args.budget_pages
                if args.slab_kib:a.slab_kib=args.slab_kib
                source,_=run.source_for(a)
                name=f'{workload}-{mode}-{policy}'
                test=out/f'host-{name}.c';test.write_text(PRELUDE+source+POSTLUDE)
                exe=out/f'host-{name}'
                command=['cc','-std=gnu11','-O1','-g','-no-pie','-fsanitize=address,undefined',
                         '-fno-sanitize-recover=all','-fno-omit-frame-pointer',
                         '-Wno-pointer-to-int-cast','-Wno-int-to-pointer-cast','-Wno-int-conversion',
                         '-I'+str(run.core.ADAPTIVE/'tests/shim'),str(test),'-o',str(exe)]
                with (out/f'host-{name}.log').open('w') as log:
                    subprocess.run(command,stdout=log,stderr=subprocess.STDOUT,check=True)
                    subprocess.run([str(exe)],stdout=log,stderr=subprocess.STDOUT,check=True,
                                   env=os.environ|{'ASAN_OPTIONS':'detect_leaks=0'})
                checks.append(dict(workload=workload,mode=mode,policy=policy,passed=True,
                    decay_ops=a.decay_ops,
                    slab_kib=args.slab_kib or 0,budget_pages=a.budget_pages,
                    generated_native_source_sha256=hashlib.sha256(test.read_bytes()).hexdigest(),
                    native_log_sha256=hashlib.sha256((out/f'host-{name}.log').read_bytes()).hexdigest()))
                print('PASS:',name,flush=True)
    (out/'checks.json').write_text(json.dumps(checks,indent=2)+'\n')
    (out/'native-evidence.json').write_text(json.dumps(checks,indent=2)+'\n')


if __name__=='__main__':main()
