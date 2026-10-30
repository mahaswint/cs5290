#!/usr/bin/env python3
"""Reuse the native interval/payload oracle for every independent ablation."""
import json
import os
import subprocess
from pathlib import Path
import run

HERE = Path(__file__).resolve().parent


def main():
    out = HERE / 'validation'; out.mkdir(exist_ok=True)
    source = (run.ADAPTIVE / 'tests/host_test.c').read_text()
    source = source.replace('#include "shim/common.h"', '#include "common.h"\n#include <sys/wait.h>\n#include <unistd.h>\n#include <signal.h>')
    source = source.replace('#include "../allocator.c"', run.allocator_source())
    # Allocate far more pages than the original burst, retain half the objects,
    # and fill holes while every payload and interval remains independently checked.
    extra = '''
  for (unsigned round = 0; round < 4; ++round) {
    for (unsigned i = 0; i < 64; ++i) allocate_object(0, i, 1024);
    for (unsigned i = 0; i < 64; i += 2) free_object(0, i);
    policy_verify(0); check_objects();
    for (unsigned i = 0; i < 64; i += 2) allocate_object(0, i, 1024);
    check_objects(); policy_verify(0);
    for (unsigned i = 0; i < 64; ++i) free_object(0, (i * 37) % 64);
    policy_drain(0); policy_verify(0);
  }
  assert(policy_backend.used == 0 && getMetadata(policy_buddy, 0, 0) == 3);
#if LOOKUP
  for (unsigned i = 0; i < heap / POLICY_PAGE; ++i) {
    struct PolicyOwner info __attribute__((aligned(8)));
    mram_read(&policy_owners[i], &info, 8); assert(!info.tag && !info.bytes);
  }
#endif
  host_tid = 0;
  void *bad_free = policy_malloc(256); assert(bad_free);
  pid_t child = fork(); assert(child >= 0);
  if (!child) { freopen("/dev/null", "w", stderr); host_tid = 1; policy_free(bad_free); _exit(0); }
  int status; assert(waitpid(child, &status, 0) == child);
  assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT);
  policy_free(bad_free);
  child = fork(); assert(child >= 0);
  if (!child) { freopen("/dev/null", "w", stderr); policy_free(bad_free); _exit(0); }
  assert(waitpid(child, &status, 0) == child);
  assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT);
  policy_drain(0); assert(policy_backend.used == 0);
'''
    source = source.replace('  munmap(host_mram, host_mram_size);', extra + '  munmap(host_mram, host_mram_size);')
    test = out / 'host-properties.c'; test.write_text(source)
    records = []
    for mode, (lists, lookup) in run.MODES.items():
        for policy, number in run.shared.POLICIES.items():
            name = f'{mode}-{policy}'
            exe = out / f'host-{name}'
            command = ['cc', '-std=gnu11', '-O1', '-g', '-no-pie', '-fsanitize=undefined,address',
                       '-fno-sanitize-recover=all', '-fno-omit-frame-pointer',
                       '-Wno-pointer-to-int-cast', '-Wno-int-to-pointer-cast', '-Wno-int-conversion',
                       '-I'+str(run.ADAPTIVE / 'tests/shim'), '-DNR_TASKLETS=4',
                       f'-DLISTS={lists}', f'-DLOOKUP={lookup}', f'-DPOLICY={number}', '-DKEEP_PAGES=2',
                       str(test), '-o', str(exe)]
            with (out / f'host-{name}.log').open('w') as log:
                subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=True)
                subprocess.run([str(exe)], stdout=log, stderr=subprocess.STDOUT, check=True,
                               env=os.environ | {'ASAN_OPTIONS': 'detect_leaks=0'})
            records.append({'mode': mode, 'policy': policy, 'native_passed': True})
            print(f'PASS: {name}', flush=True)
    (out / 'checks.json').write_text(json.dumps(records, indent=2) + '\n')


if __name__ == '__main__': main()
