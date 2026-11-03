#!/usr/bin/env python3
"""Predeclare every requested stage before measuring the full matrix."""
import argparse
import concurrent.futures
import json
import subprocess
import sys
import run


def configurations(preset,tag):
    workloads=run.WORKLOADS if preset!='heldout' else ['larson','cloud-cache']
    variants=run.VARIANTS if preset!='heldout' else ['slab-adaptive64','slab-pinned-adaptive64','slab-fixed2','slab-pinned-fixed2']
    for workload in workloads:
        for variant in variants:
            a=run.options().parse_args([])
            vars(a).update(workload=workload,variant=variant,tag=tag,rounds=4 if workload=='prim-sel' else 2)
            if preset=='validation':vars(a).update(objects=16,operations=32,warmup=32,items=256,fixture=True,validate=True)
            if preset=='heldout':vars(a).update(seed=9173,trace_offset=4096)
            yield vars(run.normalize(a))


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--preset',choices=['main','heldout','validation'],default='main')
    p.add_argument('--tag',default='pinned-v1')
    p.add_argument('--jobs',type=int,choices=range(1,5),default=4)
    a=p.parse_args()
    if not a.tag or any(s in a.tag for s in ['/','\\','..']):p.error('invalid tag')
    configs=list(configurations(a.preset,a.tag))
    folder=run.HERE/'results'/a.tag;folder.mkdir(parents=True,exist_ok=True)
    (folder/f'matrix-{a.preset}.json').write_text(json.dumps(configs,indent=2)+'\n')
    allowed={action.dest for action in run.options()._actions}
    def launch(c):
        command=[sys.executable,str(run.HERE/'run.py')]
        for k,v in c.items():
            if k not in allowed:continue
            if isinstance(v,bool):
                if v:command.append('--'+k.replace('_','-'))
            else:command+=['--'+k.replace('_','-'),str(v)]
        subprocess.run(command,check=True)
    with concurrent.futures.ThreadPoolExecutor(max_workers=a.jobs) as pool:list(pool.map(launch,configs))
    print('Complete:',len(configs),'declared',a.preset,'configurations',flush=True)


if __name__=='__main__':main()
