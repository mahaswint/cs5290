#!/usr/bin/env python3
"""Sanitized slab/indexed retention tests across span sizes and byte budgets."""
import hashlib
import json
import os
import subprocess
from pathlib import Path
import run

HERE=Path(__file__).resolve().parent
def main():
    out=HERE/'validation';out.mkdir(exist_ok=True);checks=[]
    test=(HERE/'host_test.c').read_text().replace('#include "allocator.c"',run.allocator_source())
    for slab in [0,16,32,64]:
        for policy in ['fixed','adaptive']:
            for budget in [4,64]:
                definitions=dict(NR_TASKLETS=4,SLAB_BYTES=slab*1024,POLICY=run.core.shared.POLICIES[policy],
                    LISTS=1,LOOKUP=1,KEEP_PAGES=2,MAX_KEEP=2,EMPTY_BUDGET=budget,DECAY_OPS=128,PRESSURE_PERCENT=75)
                source=''.join(f'#define {k} {v}\n' for k,v in definitions.items())+test
                name=f'host-slab{slab}-{policy}-budget{budget}'
                path=out/(name+'.c');path.write_text(source);exe=out/name;log=out/(name+'.log')
                command=['cc','-std=gnu11','-O1','-g','-no-pie','-fsanitize=address,undefined',
                    '-fno-sanitize-recover=all','-fno-omit-frame-pointer',
                    '-Wno-pointer-to-int-cast','-Wno-int-to-pointer-cast','-Wno-int-conversion',
                    '-I'+str(run.core.ADAPTIVE/'tests/shim'),str(path),'-o',str(exe)]
                with log.open('w') as stream:
                    subprocess.run(command,stdout=stream,stderr=subprocess.STDOUT,check=True)
                    subprocess.run([str(exe)],stdout=stream,stderr=subprocess.STDOUT,check=True,
                        env=os.environ|{'ASAN_OPTIONS':'detect_leaks=0'})
                checks.append(dict(slab_kib=slab,policy=policy,budget_pages=budget,passed=True,
                    source_sha256=hashlib.sha256(source.encode()).hexdigest(),log_sha256=hashlib.sha256(log.read_bytes()).hexdigest()))
                print('PASS:',name,flush=True)
    (out/'checks.json').write_text(json.dumps(checks,indent=2)+'\n')


if __name__=='__main__':main()
