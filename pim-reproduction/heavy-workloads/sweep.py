#!/usr/bin/env python3
"""Declare all workload/geometry/policy cases before running any measurements."""
import argparse
import concurrent.futures
import json
import subprocess
import sys
import run


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--preset',choices=['main','heldout','validation'],default='main')
    p.add_argument('--tag',default='heavy-v1')
    p.add_argument('--jobs',type=int,choices=range(1,5),default=4)
    a=p.parse_args()
    if not a.tag or any(s in a.tag for s in ['/','\\','..']):p.error('invalid tag')
    configs=[]
    for workload in ['larson','cloud-cache']:
        for threads in ([1,4] if a.preset=='main' else [4]):
            for policy in (['adaptive'] if a.preset=='heldout' else ['fixed','adaptive']):
                for slab in ([0,64] if a.preset=='heldout' else [0,32,64]):
                    c=dict(workload=workload,threads=threads,policy=policy,
                        objects=16 if a.preset=='validation' else 64,
                        operations=32 if a.preset=='validation' else 256,
                        warmup=32 if a.preset=='validation' else 128)
                    if slab:c['slab_kib']=slab
                    if a.preset=='validation':c.update(validate=True,heap_kib=4096)
                    if a.preset=='heldout':c.update(seed=9173,trace_offset=4096)
                    configs.append(c)
    folder=run.HERE/'results'/a.tag;folder.mkdir(parents=True,exist_ok=True)
    (folder/f'matrix-{a.preset}.json').write_text(json.dumps(configs,indent=2)+'\n')
    def launch(c):
        command=[sys.executable,str(run.HERE/'run.py'),'--tag',a.tag]
        for k,v in c.items():
            if isinstance(v,bool):
                if v:command.append('--'+k.replace('_','-'))
            else:command+=['--'+k.replace('_','-'),str(v)]
        subprocess.run(command,check=True)
    with concurrent.futures.ThreadPoolExecutor(max_workers=a.jobs) as pool:list(pool.map(launch,configs))
    print('Complete:',len(configs),'declared',a.preset,'configurations',flush=True)


if __name__=='__main__':main()
