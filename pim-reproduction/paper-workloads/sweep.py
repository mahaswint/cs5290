#!/usr/bin/env python3
"""Save the declared paper-derived matrix, then run each exact configuration."""
import argparse
import concurrent.futures
import json
import subprocess
import sys
import run


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--preset',choices=['main','heldout','validation','ablation','interval','interval-validation',
                                     'slab-real','slab-real-validation'],default='main')
    p.add_argument('--tag',default='papers-v3')
    p.add_argument('--jobs',type=int,choices=range(1,5),default=2)
    a=p.parse_args()
    if not a.tag or any(x in a.tag for x in ['/','\\','..']):p.error('invalid tag')
    configs=[]
    if a.preset in ['slab-real','slab-real-validation']:
        validation=a.preset=='slab-real-validation'
        for workload in run.KINDS:
            for threads in ([4] if validation else [1,4]):
                for policy in ['fixed','adaptive']:
                    for slab_kib in ([64] if validation else [0,64]):
                        c=dict(workload=workload,mode='indexed',policy=policy,threads=threads,
                            seed=4141,objects=32 if validation else 512,
                            operations=64 if validation else 2048,graph_edges=96 if validation else 3072,
                            items=256 if validation else 32768,
                            rounds=2 if validation or workload!='prim-sel' else 4,
                            decay_ops=128,budget_pages=64)
                        if slab_kib:c['slab_kib']=slab_kib
                        if validation:c.update(fixture=True,validate=True,heap_kib=1024)
                        configs.append(c)
    elif a.preset in ['validation','interval-validation']:
        for workload in run.KINDS:
            for mode in (['scan','indexed'] if a.preset=='validation' else ['indexed']):
                configs.append(dict(workload=workload,mode=mode,policy='adaptive',threads=4,objects=32,
                                    operations=64,graph_edges=96,items=256,rounds=2,fixture=True,validate=True,heap_kib=1024,
                                    decay_ops=128 if a.preset=='interval-validation' else 64))
    elif a.preset=='interval':
        # Paired against the completed 64-operation main and heldout matrices.
        # Change only DECAY_OPS; keep traces and every other setting identical.
        for seed in [4141,9173]:
            workloads=list(run.KINDS) if seed==4141 else ['larson','faimgraph']
            for workload in workloads:
                for threads in [1,4]:
                    configs.append(dict(workload=workload,mode='indexed',policy='adaptive',threads=threads,
                        seed=seed,objects=512,operations=2048,graph_edges=3072,
                        rounds=4 if workload=='prim-sel' else 2,decay_ops=128))
    elif a.preset=='ablation':
        for workload in ['larson','faimgraph']:
            for mode in run.core.MODES:
                configs.append(dict(workload=workload,mode=mode,policy='fixed',threads=1,seed=4141,
                                    objects=512,operations=2048,graph_edges=3072,rounds=2))
    else:
        workloads=list(run.KINDS) if a.preset=='main' else ['larson','faimgraph']
        seed=4141 if a.preset=='main' else 9173
        for workload in workloads:
            for threads in [1,4]:
                variants=[('scan','fixed',2),('indexed','fixed',2),('indexed','adaptive',2)]
                if a.preset=='main':variants.append(('scan','fixed',1))
                for mode,policy,keep in variants:
                    configs.append(dict(workload=workload,threads=threads,mode=mode,policy=policy,keep=keep,
                                        seed=seed,objects=512,operations=2048,graph_edges=3072,
                                        rounds=4 if workload=='prim-sel' else 2))
    folder=run.HERE/'results'/a.tag;folder.mkdir(parents=True,exist_ok=True)
    (folder/f'matrix-{a.preset}.json').write_text(json.dumps(configs,indent=2)+'\n')
    def launch(c):
        command=[sys.executable,str(run.HERE/'run.py'),'--tag',a.tag]
        for key,value in c.items():
            if isinstance(value,bool):
                if value:command.append('--'+key.replace('_','-'))
            else:command+=['--'+key.replace('_','-'),str(value)]
        subprocess.run(command,check=True)
    with concurrent.futures.ThreadPoolExecutor(max_workers=a.jobs) as pool:list(pool.map(launch,configs))
    print(f'Complete: {len(configs)} declared {a.preset} configurations',flush=True)


if __name__=='__main__':main()
