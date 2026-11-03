#!/usr/bin/env python3
"""ASan/UBSan application checks across both workloads, policies and geometries."""
import hashlib
import json
import os
import subprocess
import run

native=run.module('heavy_native_harness',run.HERE.parent/'paper-workloads/test.py')


def configurations():
    for workload in ['larson','cloud-cache']:
        for policy in ['fixed','adaptive']:
            for slab in [0,32,64]:
                a=run.options().parse_args([])
                vars(a).update(workload=workload,policy=policy,threads=1,objects=32,operations=128,
                    warmup=64,heap_kib=4096,validate=True)
                if slab:a.slab_kib=slab
                yield a


def main():
    out=run.HERE/'validation';out.mkdir(exist_ok=True);checks=[]
    for a in configurations():
        source,_=run.source_for(a)
        name=f"host-{a.workload}-{a.policy}-slab{getattr(a,'slab_kib',0)}"
        path=out/(name+'.c');path.write_text(native.PRELUDE+source+native.POSTLUDE)
        exe=out/name;log_path=out/(name+'.log')
        command=['cc','-std=gnu11','-O1','-g','-no-pie','-fsanitize=address,undefined',
            '-fno-sanitize-recover=all','-fno-omit-frame-pointer',
            '-Wno-pointer-to-int-cast','-Wno-int-to-pointer-cast','-Wno-int-conversion',
            '-I'+str(run.core.ADAPTIVE/'tests/shim'),str(path),'-o',str(exe)]
        with log_path.open('w') as log:
            subprocess.run(command,stdout=log,stderr=subprocess.STDOUT,check=True)
            subprocess.run([str(exe)],stdout=log,stderr=subprocess.STDOUT,check=True,
                env=os.environ|{'ASAN_OPTIONS':'detect_leaks=0'})
        checks.append(dict(configuration=vars(a),passed=True,
            generated_native_source_sha256=hashlib.sha256(path.read_bytes()).hexdigest(),
            native_log_sha256=hashlib.sha256(log_path.read_bytes()).hexdigest()))
        print('PASS:',name,flush=True)
    (out/'checks.json').write_text(json.dumps(checks,indent=2)+'\n')


if __name__=='__main__':main()
