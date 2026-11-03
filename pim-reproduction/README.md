# PIM-malloc reproduction and software allocator extensions

See the [report-to-experiment guide](../docs/EXPERIMENTS.md) for all completed stages and [reproduction instructions](../docs/REPRODUCING.md) for environment setup.

The completed [submission package](submission/README.md) contains the IEEE report, complete LaTeX project, editable 16-minute presentation, separate baseline/contribution figures and tables, and an offline source snapshot. [SNAPSHOT.md](SNAPSHOT.md) describes the implementation progression and reproduction dependencies.

The original experiment below reproduces the **software-only microbenchmark comparison in Figure 15** of the attached HPCA 2026 paper. Hardware buddy-cache experiments and the full language-model serving case study are outside its scope. The completed software extensions and report are linked above and at the end of this document.

## Experiment matrix

- Allocators: simple MRAM buddy allocator and the authors' PIM-malloc-SW.
- One DPU; 1 and 16 tasklets (UPMEM hardware threads).
- Allocation sizes: 32 B, 256 B, 4096 B.
- 128 consecutive allocations per thread, without frees, as described for Figure 15.
- 32 MiB heap, 64 B software metadata buffer.
- Buddy allocator minimum block: 32 B. PIM-malloc-SW backend page: 4096 B.
- SW frontend: eight size classes, 16 B through 2048 B, powers of two.
- The authors' default DPU architecture and DRAM timing: 350 MHz logic, 2400 MHz memory, 14 pipeline stages, 11 revolver scheduling cycles, 1024 B row buffer. Remaining device defaults come from the baseline `src/main.go`.

The buddy allocator is instantiated directly from the same authors' `initBuddyAllocator` and `buddyAllocate` implementation with a 32 B minimum block. PIM-malloc-SW calls `initUMalloc` and `allocateUMalloc`. The allocator function bodies are copied unchanged; only `main` is parameterized for these experiments. Thus the buddy baseline is a reconstruction from released allocator code, not a separately supplied, labeled Figure 15 straw-man executable.

## Results and provenance

- `reference/figure15_published.csv`: manually transcribed **printed paper values**, rounded as shown in the figure. These are comparison targets, not local measurements.
- `results/fig15/<allocator>_t<threads>_s<size>_n128/`: one directory per actual configuration.
- `allocations.csv`: every allocation's tasklet, request index, begin/end cycles, and elapsed cycles.
- `summary.json`: aggregate local measurements and compiler version.
- `simulate.log`, `log.txt`: simulator output and native statistics.
- `task.c`, `task.c.o`, `main.S`, `addresses.txt`, binaries: exact benchmark sources, assembly, and linked inputs used by that run.
- `analysis/figure15_comparison.csv`: comparison including both aggregation definitions below.
- `analysis/figure15_reproduction.png` and `.svg`: generated raster and vector graphs.
- `analysis/mean_request_latency.png` and `.svg`: the separate arithmetic mean of individual allocation latencies, without treating the paper's aggregation as confirmed.
- `analysis/software_results.csv`: the six completed PIM-malloc-SW configurations with both metrics.
- `analysis/RESULTS.md`: numerical results, discrepancies, and speedups.

Failed early toolchain smoke tests remain under explicitly named `results/smoke-*` directories. They are excluded from the results and plots. The accepted `results/fig15` matrix contains twelve full, successful 128-request configurations. A resumed interrupted attempt is retained with `.incomplete-<timestamp>` and excluded from analysis.

## Measurement definition

Zero-instruction inline assembly labels bracket each allocation call. Passive probes in the simulator record the logic cycle when the tasklet reaches each label. This includes time spent waiting for the buddy mutex and memory accesses, and excludes allocator initialization. Every completed run must produce exactly 128 records per tasklet and pass the C pointer validity assertions. Cycles are divided by 350 to obtain microseconds.

We retain **two different statistics**, especially important for multiple threads:

1. **Mean individual request latency**: sum of every allocation's measured cycles divided by `128 × threads × 350`.
2. **Batch-normalized completion latency**: `(last allocation end − first allocation start) / (128 × 350)`. This uses the full concurrent batch completion interval and includes the loop and validation work between allocations. It is not divided by the number of threads.

The comparison plot uses the second statistic, explicitly labeled as batch time / 128. The paper labels Figure 15 as average latency but does not explicitly give its multithread aggregation formula. Our interpretation is suggested by the observed 16-thread 32 B result: 51.86 µs using batch normalization, close to the printed 51 µs, versus 32.89 µs as the mean of individual requests. This is an inference, not author-confirmed metadata. Both statistics must remain available in any report. Do not relabel one as the other or adjust the implementation to force a match to rounded values.

The measurement harness checks allocation success and accumulates a volatile pointer checksum after each measured call. Those instructions are outside individual request timing; they are inside the batch completion interval. Small differences from the paper can also arise from reconstruction of the baseline and unspecified measurement details. Discrepancies are reported rather than hidden.

## Local toolchain and simulator adaptations

The original SDK download host did not resolve. We obtained SDK **2023.2.0** from the bundled SDK directory in the [LoCaLUT HPCA 2026 artifact](https://github.com/AIS-SNU/LoCaLUT), commit `c4cf67ae623c5e313b0628ac119d957887b5f6df`. The compiler reports LLVM/Clang 12.0.0, UPMEM LLVM revision `846fdda8285dcc9b20ee5d2fec9e54dfea6a8928`. Its wrapper and compiler executable permissions needed restoring after checkout.

The legacy compiler needs `libtinfo.so.5`, absent on Ubuntu 24.04. The official [Ubuntu package](https://packages.ubuntu.com/jammy/amd64/libtinfo5/download) was downloaded and extracted into `toolchain/compat`, without a system installation. Package SHA-256: `4df4288404108f1a156d014e8764a064e977e34e6d44931ab60451694c03c90d`. The runner supplies the local library search path needed by the compiler.

Compilation calls the UPMEM compiler directly with the authors' `-O3 -S -DNR_TASKLETS=N` flags and SDK sources. Docker is not required for this native workflow. A staged main routine separates linking from simulation; no hardware buddy-cache source or ISA extension is used. The original cloned repositories are left unchanged. All experiment changes reside under `runtime`.

The default host VM eagerly allocates 32 × 128 MiB of host memory banks, which is expensive on this 16 GiB PC. The harness sets **one host VM rank and one bank**, keeping host-side parameters small. It does not change the simulated DPU, MRAM, tasklets, or memory timings. Host launch/transfer times are outside our allocation measurement interval. This host configuration differs from the simulator's defaults and must be disclosed.

**Physical capacity caveat discovered during the improvement audit:** the released simulator permits **256 KiB WRAM**, whereas physical UPMEM has **64 KiB**. Its linker reserves 24 × 4 KiB tasklet stacks, including inactive tasklets, placing the preserved 16-thread SW image's WRAM heap start at address 99,784. These inherited settings mean this reproduction does not demonstrate a hardware-valid WRAM layout. Keep the existing results as measurements of the released simulation configuration; validate a separate physical-capacity build before claiming hardware feasibility. See [research/WRAM_AND_SLAB_CONSTRAINTS.md](research/WRAM_AND_SLAB_CONSTRAINTS.md) for evidence and the proposed metadata-cache budget. No capacity or layout setting was changed in this audit.

Host-side caches reuse decoded instruction descriptors (with distinct issued instruction objects) and immutable opcode membership sets. They change host execution speed only. `validation/host-caches-sw1-32` and `validation/host-caches-sw16-32` compare every allocation's exact simulated begin/end cycles and all native simulator statistics with uncached runs. The runner enables the decode cache only when both validation success files are present. The source changes are in `cache_decode.py` and `cache_opcodes.py`; no simulated pipeline, instruction timing, or scheduler rule is changed. Early decode-only checks are also retained under `validation/decode-cache-*`.

Independent simulations can run concurrently. Native compilation and linking are serialized with a file lock because their generated paths are shared; each simulation reads its own linked binaries and writes its own result directory. Use `--jobs 1` on a machine with limited RAM. Four jobs were used for the final expensive multithreaded configurations on this PC.

## Commands

With the runtime prepared and built, run the Figure 15 matrix with:

```bash
python3 pim-reproduction/sweep.py --jobs 4
```

Run one configuration in a fresh results tag:

```bash
python3 pim-reproduction/run.py --allocator sw --threads 16 --size 256 --tag repeat-01
```

Analyze the twelve completed configurations:

```bash
python3 pim-reproduction/analyze.py
```

To prepare a new experiment folder from scratch, run `prepare.py` once before running the sweep. It deliberately refuses to overwrite an existing runtime. The SDK mirror and local compatibility library must be available at the paths documented above. Matplotlib dependencies are pinned in `requirements-lock.txt`.

## Implemented experiment stages

Empty-page retention is implemented separately in [adaptive/README.md](adaptive/README.md): adaptive empty-page retention with eager, lazy, fixed-1/fixed-2, and adaptive comparison modes, raw measurements, a resumable sweep, and plots. See that README for the commands. The preserved Figure 15 runs and reference runtime remain unchanged.

The initial retention comparison covers 1 and 16 threads, matching Figure 15. Design alternatives and literature sources are recorded in [research/IMPROVEMENT_OPTIONS.md](research/IMPROVEMENT_OPTIONS.md). The isolated implementation and commands are in [adaptive/README.md](adaptive/README.md). The released reference frontend has hardcoded caches for tasklets 0 through 15; the contribution runner also currently limits experiments to 16 threads.

The dynamic-allocation graph workload now has a separate reduced implementation in [adaptive/graph/README.md](adaptive/graph/README.md). It uses real loc-gowalla edges, the paper's 256 B linked-list blocks and a 1:2 new-edge to existing-edge split at the peak batch. It adds 64 B blocks and a changing low/high/low/rebound insertion/deletion schedule inspired by faimGraph. The paper's full Figure 17 experiment, including its variable-array representation and full-dataset evaluation, is not claimed as reproduced. The released artifact's microbenchmark-only build does not include that graph kernel.

The combined retention/list/ownership implementation is documented in [combined/SUMMARY.md](combined/SUMMARY.md). Its additional paper-derived evaluation is in [paper-workloads/README.md](paper-workloads/README.md), with [source mappings and port limitations](paper-workloads/SOURCES.md): Larson persistent replacement, both PIM-malloc graph representations, faimGraph-style arbitrary edge updates, and a preallocated PrIM SEL control. These scaled DPU ports have separate declared matrices, correctness oracles, raw measurements and an audited [results report](paper-workloads/RESULTS.md).

The extended [stacked allocator](stacked/README.md) combines 4 KiB object slabs with indexed lists/ownership and selectable fixed/adaptive retention. Its [results](stacked/RESULTS.md) compare slab sizes, byte budgets, dense reuse, sparse survivors and bypass controls.

[Large-request workloads](heavy-workloads/README.md) add an explicit mixed-size 8–4095 B Larson configuration and a variable-size LRU write-cache replay of actual CloudPhysics I/O records. The paired [results](heavy-workloads/RESULTS.md) report both policies, 32/64 KiB slabs, 1/4 tasklets, a second seed/trace window, backend calls and memory costs.

[Pinned buddy metadata](pinned/README.md) adds an authoritative 528 B upper-tree prefix in shared WRAM while streaming disjoint lower metadata from MRAM. Its [full-stage comparison](pinned/RESULTS.md) runs scan fixed-2, indexed adaptive-64/fixed-2, slabs with both policies, and slabs plus pinning with both policies on five workload cases.

The [paper workload extension](pinned/paper/README.md) applies all seven stages to the PIM-malloc paper's two Gowalla insertion structures and its 32 B / 256 B / 4 KiB allocation microbenchmarks at 1/16 tasklets. It also includes a separately labelled allocator-only KV-growth replay; the paper's full attention kernel and LLM-serving trace are not supplied in the pinned artifact. [Results and memory traffic](pinned/paper/RESULTS.md) cover every declared case.
