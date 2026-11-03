#!/usr/bin/env python3
"""Sanitizer checks for all six paper request/graph shapes and seven stages."""
import json
import hashlib
import os
import subprocess
import run
sweep=run.stack.module('paper_extension_sweep',run.HERE/'sweep.py')

native=run.stack.module('paper_extension_native',run.HERE.parent.parent/'paper-workloads/test.py')


def main():
    # Keep the existing backend and 47-case application evidence untouched.
    folder=run.HERE/'validation';folder.mkdir(exist_ok=True)
    checks=[]
    for c in sweep.configurations(native=True):
        a=run.argparse.Namespace(**c)
        source,_=run.source_for(a)
        source=native.PRELUDE+source+native.POSTLUDE
        name=f'host-{a.workload}-{a.variant}'
        path=folder/(name+'.c');path.write_text(source)
        log=folder/(name+'.log');exe=folder/name
        command=['cc','-std=gnu11','-O1','-g','-no-pie','-fsanitize=address,undefined',
            '-fno-sanitize-recover=all','-fno-omit-frame-pointer',
            '-Wno-pointer-to-int-cast','-Wno-int-to-pointer-cast','-Wno-int-conversion',
            '-I'+str(run.core.ADAPTIVE/'tests/shim'),str(path),'-o',str(exe)]
        with log.open('w') as stream:
            subprocess.run(command,stdout=stream,stderr=subprocess.STDOUT,check=True)
            subprocess.run([str(exe)],stdout=stream,stderr=subprocess.STDOUT,check=True,
                env=os.environ|{'ASAN_OPTIONS':'detect_leaks=0'})
        checks.append(dict(name=name,passed=True,configuration=vars(a),
            source_sha256=hashlib.sha256(path.read_bytes()).hexdigest(),
            log_sha256=hashlib.sha256(log.read_bytes()).hexdigest()))
        print('PASS:',name,flush=True)
    (run.HERE/'validation/checks.json').write_text(json.dumps(checks,indent=2)+'\n')


if __name__=='__main__':main()
