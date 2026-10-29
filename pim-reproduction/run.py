#!/usr/bin/env python3
"""Compile the unchanged allocator bodies and run a Figure 15 configuration."""
import argparse
import concurrent.futures
import csv
import fcntl
import hashlib
import json
import os
from pathlib import Path
import shutil
import statistics
import subprocess
import time

BASE = Path(__file__).resolve().parent
ROOT = BASE.parent
RUNTIME = BASE / 'runtime'
AUTHOR_SOURCE = ROOT / 'PIM-malloc/simulator/uPIMulator/golang_vm/uPIMulator'


def execute(command, log, **kwargs):
    with open(log, 'w') as stream:
        result = subprocess.run([str(x) for x in command], stdout=stream, stderr=subprocess.STDOUT, **kwargs)
    if result.returncode:
        raise RuntimeError(f'Command failed ({result.returncode}); see {log}')


def compile_sdk(compiler, tasklets, version):
    cache = BASE / 'build' / f'sdk-{version}-t{tasklets}'
    if not (cache / 'complete.json').exists():
        cache.mkdir(parents=True, exist_ok=True)
        jobs = []
        for group in ('misc', 'stdlib', 'syslib'):
            output = cache / group / 'CMakeFiles' / f'{group}.dir'
            output.mkdir(parents=True, exist_ok=True)
            for source in sorted((RUNTIME / 'sdk' / group).glob('*.c')):
                cmd = [compiler, '-w', '-O3', '-S', f'-DNR_TASKLETS={tasklets}']
                for include in ('misc', 'stdlib', 'syslib'):
                    cmd += ['-I', RUNTIME / 'sdk' / include]
                cmd += ['-c', source, '-o', output / f'{source.name}.o']
                jobs.append((cmd, cache / f'{group}-{source.stem}.log'))
        with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
            list(pool.map(lambda job: execute(*job), jobs))
        (cache / 'complete.json').write_text(json.dumps({'compiler': str(compiler), 'tasklets': tasklets}))
    sdk_build = RUNTIME / 'sdk/build'
    if sdk_build.is_symlink():
        sdk_build.unlink()
    elif sdk_build.exists():
        raise RuntimeError('Unexpected SDK build directory; refusing to replace it')
    sdk_build.symlink_to(cache, target_is_directory=True)


def benchmark_source(allocator, size, requests):
    original = (AUTHOR_SOURCE / 'benchmark/microbenchmark/dpu/task.c').read_text()
    begin = original.index('int main() {')
    end = original.index('// NOTE(bongjoon.hyun@gmail.com)', begin)
    init = '''
    int *sizeClasses = mem_alloc(8 * sizeof(int));
    sizeClasses[0] = 16; sizeClasses[1] = 32;
    sizeClasses[2] = 64; sizeClasses[3] = 128;
    sizeClasses[4] = 256; sizeClasses[5] = 512;
    sizeClasses[6] = 1024; sizeClasses[7] = 2048;
    uMalloc = initUMalloc(mallocableRegionSize, pageSize, bufferSize,
                         DPU_MRAM_HEAP_POINTER, sizeClasses, 8);
'''
    if allocator == 'buddy':
        init = 'reproBuddy = initBuddyAllocator(mallocableRegionSize, 32, bufferSize, DPU_MRAM_HEAP_POINTER);'
    allocation = 'allocateUMalloc(uMalloc, REPRO_SIZE)' if allocator == 'sw' else 'buddyAllocate(reproBuddy, REPRO_SIZE)'
    main = f'''
#define REPRO_SIZE {size}
#define REPRO_REQUESTS {requests}
struct BuddyAllocator *reproBuddy;
volatile unsigned int reproChecksum[NR_TASKLETS];
int main() {{
  int threadId = me();
  if (threadId == 0) {{
    mem_reset();
    {init}
  }}
  barrier_wait(&deviceBarrier);
  #pragma clang loop unroll(disable)
  for (unsigned int i = 0; i < REPRO_REQUESTS; i++) {{
    __asm__ volatile("repro_begin_%=:" ::: "memory");
    __mram_ptr void *pointer = {allocation};
    __asm__ volatile("repro_end_%=:" ::: "memory");
    assert(pointer != NULL);
    reproChecksum[threadId] ^= (unsigned int)pointer;
  }}
  return 0;
}}

'''
    return original[:begin] + main + original[end:]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--allocator', choices=['buddy', 'sw'], required=True)
    parser.add_argument('--threads', type=int, choices=range(1, 17), required=True)
    parser.add_argument('--size', type=int, required=True)
    parser.add_argument('--requests', type=int, default=128)
    parser.add_argument('--sdk', type=Path, default=BASE / 'toolchain/LoCaLUT/upmem-2023.2.0-Linux-x86_64')
    parser.add_argument('--tag', default='fig15')
    args = parser.parse_args()
    compat = BASE / 'toolchain/compat/lib/x86_64-linux-gnu'
    os.environ['LD_LIBRARY_PATH'] = str(compat) + (':' + os.environ['LD_LIBRARY_PATH'] if os.environ.get('LD_LIBRARY_PATH') else '')
    os.environ['UPMEM_NO_OS_WARNING'] = '1'
    compiler = args.sdk.resolve() / 'bin/dpu-upmem-dpurte-clang'
    if not compiler.exists():
        raise SystemExit(f'SDK compiler is not ready: {compiler}')
    if args.size < 32 or args.size & (args.size - 1):
        raise SystemExit('Use a power-of-two allocation size of at least 32 bytes')
    out = BASE / 'results' / args.tag / f'{args.allocator}_t{args.threads}_s{args.size}_n{args.requests}'
    out.mkdir(parents=True, exist_ok=False)
    started = time.time()
    version = args.sdk.name
    compiler_version = subprocess.check_output([str(compiler), '--version'], text=True)
    (out / 'compiler.txt').write_text(compiler_version)
    # Compilation/linking share generated assembly paths; simulations use separate binaries.
    preparation_lock = (BASE / '.prepare.lock').open('w')
    fcntl.flock(preparation_lock, fcntl.LOCK_EX)
    compile_sdk(compiler, args.threads, version)
    source = benchmark_source(args.allocator, args.size, args.requests)
    source_path = RUNTIME / 'benchmark/microbenchmark/dpu/task.c'
    source_path.write_text(source)
    (out / 'task.c').write_text(source)
    asm = RUNTIME / 'benchmark/build/microbenchmark/dpu/CMakeFiles/microbenchmark_device.dir/task.c.o'
    asm.parent.mkdir(parents=True, exist_ok=True)
    execute([compiler, '-w', '-O3', '-S', f'-DNR_TASKLETS={args.threads}', '-c', source_path, '-o', asm], out / 'compile.log')
    shutil.copy2(asm, out / 'task.c.o')
    exe = RUNTIME / 'build/uPIMulator'
    exe.parent.mkdir(exist_ok=True)
    if not exe.exists():
        env = os.environ | {'GOCACHE': str(ROOT / '.cache/go-build')}
        execute(['go', 'build', '-C', RUNTIME / 'src', '-buildvcs=false', '-o', exe], out / 'go-build.log', env=env)
    common = [exe, '--root_dirpath', RUNTIME, '--bin_dirpath', out,
              '--benchmark', 'microbenchmark', '--num_channels', '1',
              '--num_ranks_per_channel', '1', '--num_dpus_per_rank', '1',
              '--num_vm_ranks_per_channel', '1', '--num_vm_banks_per_rank', '1',
              '--num_tasklets', str(args.threads), '--verbose', '0']
    (out / 'command.json').write_text(json.dumps([str(x) for x in common], indent=2))
    execute(common + ['--mode', 'link'], out / 'link.log')
    addresses = (out / 'addresses.txt').read_text()
    if 'repro_begin_' not in addresses or 'repro_end_' not in addresses:
        raise RuntimeError('Assembly markers did not survive compilation/linking')
    fcntl.flock(preparation_lock, fcntl.LOCK_UN)
    preparation_lock.close()
    cache_enabled = all((BASE / f'validation/host-caches-sw{threads}-32/validation.json').exists()
                        for threads in (1, 16))
    execute(common + ['--mode', 'simulate'], out / 'simulate.log',
            env=os.environ | {'REPRO_CPU_PROFILE': str(out / 'cpu.pprof'),
                              'REPRO_IRAM_CACHE': '1' if cache_enabled else '0'})
    rows = []
    for line in (out / 'simulate.log').read_text().splitlines():
        if line.startswith('REPRO_ALLOC,'):
            rows.append([int(x) for x in line.split(',')[1:]])
    for thread in range(args.threads):
        indices = [r[1] for r in rows if r[0] == thread]
        if indices != list(range(args.requests)):
            raise RuntimeError(f'Missing or repeated measurements for thread {thread}: {len(indices)}')
    with (out / 'allocations.csv').open('w', newline='') as stream:
        writer = csv.writer(stream)
        writer.writerow(['thread', 'request', 'begin_cycle', 'end_cycle', 'cycles'])
        writer.writerows(rows)
    cycles = [r[4] for r in rows]
    batch_cycles = max(r[3] for r in rows) - min(r[2] for r in rows)
    result = {'allocator': args.allocator, 'threads': args.threads, 'size_bytes': args.size,
              'requests_per_thread': args.requests, 'samples': len(cycles), 'frequency_mhz': 350,
              'average_cycles': statistics.mean(cycles), 'average_latency_us': statistics.mean(cycles) / 350,
              'batch_cycles': batch_cycles, 'batch_latency_per_request_us': batch_cycles / args.requests / 350,
              'min_latency_us': min(cycles) / 350, 'max_latency_us': max(cycles) / 350,
              'sdk': str(args.sdk.resolve()), 'compiler_version': compiler_version.strip(),
              'source_sha256': hashlib.sha256(source.encode()).hexdigest(),
              'simulator_sha256': hashlib.sha256(exe.read_bytes()).hexdigest(),
              'decode_cache_enabled': cache_enabled,
              'host_vm_ranks': 1, 'host_vm_banks_per_rank': 1,
              'host_runtime_seconds': time.time() - started}
    (out / 'summary.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2), flush=True)


if __name__ == '__main__':
    main()
