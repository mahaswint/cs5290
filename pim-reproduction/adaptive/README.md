# Adaptive empty-page retention

This is the first contribution implementation. It is separate from the preserved Figure 15 reproduction. The experiment changes **how many completely empty 4 KiB pages a tasklet retains for each size class**. Live objects never move, and partially occupied pages are never reclaimed.

## Run the comparison

The runtime is already prepared on this PC. Start with the quick comparison:

```bash
cd /home/mahaswint/cs5290
python3 pim-reproduction/adaptive/experiment.py sweep --preset quick --jobs 2 --tag comparison-v1
python3 pim-reproduction/adaptive/analyze.py --tag comparison-v1
```

This runs **10 configurations**: eager, lazy, fixed-1, fixed-2, and adaptive at 1 and 4 tasklets, using four size phases, 16 allocations per burst, and two bursts per phase. The quick preset deliberately fixes burst/round counts for an inexpensive first check. Four jobs are supported, but two are a reasonable starting point on this PC. A resumed command skips matching completed runs and preserves failed/stale attempts separately.

Results appear under:

- `adaptive/results/comparison-v1/<configuration>/`: exact flattened C, SDK assembly, compiler/link commands, linked binaries, simulator logs, raw cycle events, memory snapshots, statistics, and summary JSON.
- `adaptive/analysis/comparison-v1/comparison.csv`: all measurements and controller counters.
- `adaptive/analysis/comparison-v1/RESULTS.md`: readable comparison table.
- `adaptive/analysis/comparison-v1/phases-00.png` and `.svg`: plots of allocation mean/p99, free mean, total time, peak reserved MRAM, and failures. Parameter variations produce additional numbered plots.

The analyzer automatically uses the existing reproduction `.venv` for Matplotlib when needed. No further package installation is required on this PC.

For the larger matrix, use a separate tag:

```bash
python3 pim-reproduction/adaptive/experiment.py sweep --preset full --jobs 2 --tag comparison-full-v1
python3 pim-reproduction/adaptive/analyze.py --tag comparison-full-v1
```

The full matrix has **75 configurations**: five policy settings × 1/2/4/8/16 tasklets × reuse/phases/pressure workloads. Default burst is 32 and rounds per phase is four. Normal workloads use a 32 MiB heap; pressure uses 64 KiB per tasklet to expose capacity tradeoffs. The full sweep takes substantially longer. Failed allocations remain visible; an OOM run must not be called faster than a run that completed more work.

Run one configuration, or vary parameters, with:

```bash
python3 pim-reproduction/adaptive/experiment.py run --policy adaptive --threads 4 --workload phases --burst 32 --rounds 4 --tag individual-v1
python3 pim-reproduction/adaptive/experiment.py run --policy fixed --keep 2 --threads 4 --workload phases --burst 32 --rounds 4 --tag individual-v1
python3 pim-reproduction/adaptive/analyze.py --tag individual-v1
```

Use the same heap, burst, rounds, thread count, and memory budget for a comparison. `experiment.py run --help` lists all parameters. Use a different tag for separate campaigns. Changing implementation sources invalidates completion reuse; old runs are retained with `.superseded-...`. Incomplete attempts are retained with `.incomplete-...`; neither is included in analysis. Mixing implementation versions in one analyzed tag is rejected.

## Policies and exact controller

All policies use the same maintained buddy backend, frontend bitmap operations, page-list search, object geometry, payload workload, and correctness repairs. The following is a software implementation; no hardware cache or ISA extension is added.

| Policy | Population | Retention when a page becomes wholly empty |
| --- | --- | --- |
| `eager` | One page per active tasklet/class at initialization | Return used pages immediately; untouched prepopulated pages remain until final drain |
| `lazy` | On demand | Return every empty page immediately |
| `fixed --keep K` | On demand | Retain at most K empty pages per class, subject to the per-tasklet empty-page budget |
| `adaptive` | On demand | Retain according to demand/idle/pressure rules below, under that same budget |

Default adaptive rules:

1. Initial per-class retention target: one page; maximum `--max-keep 2`.
2. Two backend page refills for a class raise its target by one, up to that maximum. Reusing a retained empty page is counted separately.
3. Every `--decay-ops 64` allocation/free operations, examine the tasklet's eight classes. If a class had no activity since the previous examination, lower its target by one and release excess empty pages. The interval must be a power of two.
4. The shared backend updates a pressure hint under its mutex when reserved payload bytes reach `--pressure-percent 75` of the heap. During maintenance, pressure releases the tasklet's empty pages and lowers its targets to zero. A page becoming empty under pressure is returned immediately.
5. Total retained empty pages are limited to `--budget-pages 4` per tasklet. This is an **unused-memory budget**, not a cap on live allocations or partially occupied pages. Fixed and adaptive policies obey the same limit.
6. If a backend allocation fails, adaptive retention drains its own empty caches and retries once. Allocation failure is nonfatal and is recorded if retry fails.

Maintenance is cooperative and runs inside allocator calls. It does not steal another inactive tasklet's pages, use a background thread, compact objects, or support cross-tasklet free. Each object must be freed by the tasklet that allocated it. Thresholds are deliberately simple; their quality is an experimental question. The controller may trade some latency for memory savings and can be slower than a good fixed policy.

Important parameters: `--keep`, `--max-keep`, `--budget-pages`, `--decay-ops`, `--pressure-percent`, `--heap-kib`, `--burst`, `--rounds`, and `--threads`. The sweep includes fixed keep=2 as a stronger static competitor. For a report, evaluate threshold sensitivity and held-out workload patterns rather than tuning only on the final reported trace.

## Workloads and measurements

- `reuse`: repeated 256 B allocation/free bursts.
- `phases`: 256 B → 1024 B → 64 B → 256 B, with repeated bursts in each phase.
- `pressure`: 256 B → 1024 B → 2048 B → 64 B, typically using the smaller heap selected by the full sweep.
- `correctness`: 16 B → 129 B → 4096 B → 8192 B, for simulator tests rather than contribution claims.

Each burst allocates its objects, reaches a barrier, reads/checks payload sentinels, then frees objects in reverse order. Pointer tables live in MRAM. All policies execute the same requested trace. If an allocation fails, the tasklet records failure, stops that burst's allocation loop, and frees the successful allocations; later bursts still execute. Initialization can also record failed eager prepopulation.

Zero-instruction assembly labels let passive simulator probes record issue cycles around allocation, free, initialization, workload, final drain, and total execution. Cycles are divided by 350 for microseconds. Allocation/free timing includes controller work, MRAM operations, and mutex waits. Memory snapshots at barriers are stored on the DPU; a passive reporting probe reads their final WRAM values after validation. SDK printf is avoided because it reaches an instruction form the released simulator does not implement. Reporting itself does not issue DPU instructions or affect timed execution.

The public allocation/free functions are explicitly not inlined for all policies. This keeps each call's end probe at a single unambiguous return site, including failed allocations; compiler tail merging can otherwise make a zero-instruction label reachable from an unrelated branch. Function-call costs are included equally in the compared policies.

The workload makespan includes payload reads/writes, pointer-table transfers, barriers, and memory-snapshot bookkeeping. `total` includes initialization, workload, and final empty-page drain; it excludes final reporting/verification. `--validate` adds page-list validation inside the workload and is intended for correctness tests. Compare performance runs with identical validation settings; default performance runs leave that extra check off. Payload sentinel checks and final zero-live/root-coalescing assertions remain enabled.

`events.csv` contains individual allocation/free latencies. Report their arithmetic mean and empirical nearest-rank p99, keeping small sample counts in mind. These are distinct from the original Figure 15 batch-time/128 interpretation. `snapshots.csv` records requested bytes, rounded slot bytes, reserved payload bytes, and retained empty bytes at common allocation/free barriers. Reserved/requested ratios omit zero-live points. Peak reserved payload bytes are tracked under the backend lock; the ratio of unrelated maxima is not used as a fragmentation measurement.

The reserved-byte counter includes live/retained buddy blocks, including untouched eager pages. Buddy metadata, descriptors, benchmark storage, and WRAM are reported/accounted separately rather than folded into payload reservation. OOM is a measured outcome, not an omitted failed sample.

These are controlled allocator workloads. The assignment's dynamic graph workload remains a separate next milestone; these tests do not constitute a reproduction of Figure 17.

## Shared baseline repairs and provenance

`prepare.py` extracts the released buddy backend into `buddy_backend.inc`; hashes of normalized source text and its originating commit are in `source_manifest.json`. Each run additionally hashes its exact generated C bytes. The common maintained frontend in `allocator.c` preserves 4 KiB pages, a 24 B in-band header, eight classes 16–2048 B, the 128-bit bitmap limit, and MRAM page-list scans. The original 16 B class therefore remains limited to 128 representable slots per page; changing that geometry is not part of this contribution.

Shared correctness changes required before studying allocation/free workloads:

- Allocation and free use matching bitmap word/bit positions, with live-bit checks.
- Page removal handles root, middle, and tail safely, and fully empty pages can return to buddy.
- Backend frees find the actual allocated node for blocks larger than 4 KiB, and restore/coalesce its ancestors.
- Allocation failure returns NULL rather than aborting, enabling pressure tests and adaptive retry.
- Root-sized allocations are allowed for recovery/coalescing tests.
- Metadata storage and transfers respect eight-byte DMA granularity; tail bounds use byte addresses. Unsigned bitmap shifts avoid undefined signed-shift behavior.
- Cache arrays follow the active tasklet count rather than allocating 16 sets unconditionally. This is common to all new policies. Eager initializes one page for each active tasklet/class.

This is a repaired/reconstructed baseline for the new experiments, not a claim of bit-for-bit equivalence to the original source. Do not mix its latencies with the preserved Figure 15 results as though only the retention policy changed. Those original sources/results are untouched.

## Runtime capacity

The isolated `adaptive/runtime` caps WRAM addresses at 65,536 and reserves 1,536 B for each of the linker's 24 stacks. DPU frequency, pipeline, MRAM timing, and the decoded-instruction host optimization remain inherited from the reference runtime. Runs assert/report the WRAM bump-heap end; no extra hardware is modeled. All tested tasklet counts fit this layout.

This is a WRAM capacity validation in simulation, not proof of all worst-case stack depths or a complete physical deployment. The inherited IRAM capacity is unchanged and still needs a separate hardware image validation before any real-hardware deployment claim. The reference runtime retains its original capacity settings.

## Verification and rebuilding

```bash
python3 pim-reproduction/adaptive/test.py
python3 pim-reproduction/adaptive/test.py --simulator --jobs 2
```

Native tests compile the same allocator with strict DMA checks and AddressSanitizer/UndefinedBehaviorSanitizer, checking full payloads, pairwise non-overlap, mixed sizes, random frees, retention growth/decay, pressure trimming, large frees, full coalescing, and OOM recovery. Native tasklets are interleaved sequentially; the DPU simulator suite additionally exercises actual concurrent tasklets for all four policies at 1 and 4 threads. A separate 16-thread adaptive check is retained under `results/validation-v1`. LeakSanitizer is disabled because the host environment uses ptrace; the address/undefined-behavior checks remain active.

For a fresh contribution runtime on an already prepared reference installation:

```bash
python3 pim-reproduction/adaptive/prepare.py
```

The runner compiles C with the local SDK 2023.2 and automatically rebuilds its isolated simulator when Go sources change. Compilation/linking use the reference runner's lock; simulations have separate linked inputs and can run concurrently. Generated backend regeneration refuses to overwrite unrecognized local edits.
