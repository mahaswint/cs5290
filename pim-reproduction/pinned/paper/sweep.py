#!/usr/bin/env python3
"""Declare the complete paper matrix before running it, without filtering gains."""
import argparse
import concurrent.futures
import json
import subprocess
import sys
import run


def configurations(tag='paper-v1',native=False):
    cases=[('pim-list',4),('pim-array',4),('kv-growth',4)]
    cases += [(w,t) for w in ['micro-32','micro-256','micro-4096'] for t in [1,16]]
    if native:cases=[(w,1) for w in run.WORKLOADS]
    for workload,threads in cases:
        for variant in run.VARIANTS:
            a=run.options().parse_args([])
            vars(a).update(workload=workload,threads=threads,variant=variant,tag=tag)
            if native:vars(a).update(requests=32,prompt_tokens=8,decode_tokens=32,fixture=True,validate=True)
            yield vars(run.normalize(a))


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--tag',default='paper-v1')
    p.add_argument('--jobs',type=int,choices=range(1,5),default=4)
    args=p.parse_args()
    if not args.tag or any(s in args.tag for s in ['/','\\','..']):p.error('invalid tag')
    configs=list(configurations(args.tag))
    folder=run.HERE/'results'/args.tag;folder.mkdir(parents=True,exist_ok=True)
    (folder/'matrix-main.json').write_text(json.dumps(configs,indent=2)+'\n')
    allowed={action.dest for action in run.options()._actions}
    def launch(c):
        command=[sys.executable,str(run.HERE/'run.py')]
        for k,v in c.items():
            if k not in allowed:continue
            if isinstance(v,bool):
                if v:command.append('--'+k.replace('_','-'))
            else:command+=['--'+k.replace('_','-'),str(v)]
        subprocess.run(command,check=True)
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:list(pool.map(launch,configs))
    print('Complete:',len(configs),'declared configurations',flush=True)


if __name__=='__main__':main()
