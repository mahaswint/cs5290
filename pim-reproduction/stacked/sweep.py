#!/usr/bin/env python3
"""Declare and run matched slab/indexed/fixed-adaptive configurations."""
import argparse
import concurrent.futures
import json
import subprocess
import sys
import run

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--preset',choices=['main','validation'],default='main')
    p.add_argument('--tag',default='stacked-v1')
    p.add_argument('--jobs',type=int,choices=range(1,5),default=4)
    a=p.parse_args()
    if not a.tag or any(s in a.tag for s in ['/','\\','..']):p.error('invalid tag')
    configurations=[]
    if a.preset=='validation':
        for slab in [0,16,32,64]:
            for policy in ['fixed','adaptive']:
                configurations.append(dict(slab_kib=slab,policy=policy,threads=4,workload='mixed',
                    objects=16,rounds=1,heap_kib=4096,validate=True))
        for policy in ['fixed','adaptive']:
            configurations.append(dict(slab_kib=64,policy=policy,threads=16,workload='burst',
                objects=8,rounds=1,heap_kib=8192,validate=True))
            configurations.append(dict(slab_kib=64,policy=policy,threads=4,workload='burst',
                objects=32,rounds=1,heap_kib=8192,budget_pages=4,validate=True))
        configurations.append(dict(slab_kib=32,policy='adaptive',threads=4,workload='pool',
            objects=32,operations=64,heap_kib=4096,validate=True))
        configurations.append(dict(slab_kib=64,policy='fixed',threads=4,workload='sparse',
            objects=32,rounds=1,heap_kib=8192,validate=True))
    else:
        for slab in [0,16,32,64]:
            for policy in ['fixed','adaptive']:
                for threads in [1,4]:
                    configurations.append(dict(slab_kib=slab,policy=policy,threads=threads,workload='burst'))
        for slab in [0,64]:
            for policy in ['fixed','adaptive']:
                configurations.append(dict(slab_kib=slab,policy=policy,threads=4,workload='pool',objects=64,operations=128))
                configurations.append(dict(slab_kib=slab,policy=policy,threads=1,workload='sparse'))
        for size in [256,8192]:
            for slab in [0,64]:
                configurations.append(dict(slab_kib=slab,policy='adaptive',threads=4,workload='burst',size=size,rounds=2))
    folder=run.HERE/'results'/a.tag;folder.mkdir(parents=True,exist_ok=True)
    (folder/f'matrix-{a.preset}.json').write_text(json.dumps(configurations,indent=2)+'\n')
    def launch(c):
        command=[sys.executable,str(run.HERE/'run.py'),'--tag',a.tag]
        for k,v in c.items():
            if isinstance(v,bool):
                if v:command.append('--'+k.replace('_','-'))
            else:command+=['--'+k.replace('_','-'),str(v)]
        subprocess.run(command,check=True)
    with concurrent.futures.ThreadPoolExecutor(max_workers=a.jobs) as pool:list(pool.map(launch,configurations))
    print('Complete:',len(configurations),'declared',a.preset,'configurations',flush=True)


if __name__=='__main__':main()
