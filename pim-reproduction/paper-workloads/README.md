# Paper-derived allocator workloads

This directory evaluates the existing combined allocator on documented workload ports from PrIM/uPIMulator, PIM-malloc, Larson/mimalloc, and faimGraph. See [SOURCES.md](SOURCES.md) for source links, preserved behavior, and explicit changes from the original programs. See [RESULTS.md](RESULTS.md) for measurements once the declared matrices complete.

## Workloads and comparisons

- `larson`: 512 live objects/tasklet, uniform 8–999 B requests, random victim replacement. Preload, permutation and 2,048 warmup replacements precede 2,048 timed replacements per tasklet.
- `pim-list`: Gowalla sample, 2,048 existing + 1,024 newly inserted undirected edges, 256 B linked adjacency blocks.
- `pim-array`: identical edges and insertion order, power-of-two growing adjacency arrays (minimum 64 B).
- `faimgraph`: preload 3,072 edges; two rounds of 1,024 arbitrary existing-edge deletions and reinsertions with tail compaction.
- `prim-sel`: 32,768 `uint64_t` elements, four passes, 256 B blocks, odd-value filtering and stable compaction. Preallocated buffers; zero allocator requests in the timed kernel.

The main matrix declares 40 configurations: five workloads × tasklets {1,4} × {scan fixed-1, scan fixed-2, indexed fixed-2, indexed adaptive}. The 12 second-seed configurations cover Larson and faimGraph, seed 9173. The eight ablations hold retention fixed-2 while independently toggling partial-page lists and ownership lookup at one tasklet. The allocator's four-empty-page budget, adaptive maximum two pages/class, decay 64 operations and pressure threshold 75% remain unchanged from the previous experiments. No thresholds are fitted to these workloads.

**Fixed-2 retains up to two completely empty pages per size class**, subject to the four-empty-page budget per tasklet. It does not limit live or partially occupied pages to two. A full class obtains another 4 KiB backend page and serves multiple small objects from that page. The periodic adaptive interval counts allocations **and frees**, not DRAM memory transactions.

## Measurement and validation

The primary runtime is concurrent **work makespan**: earliest tasklet work entry through last tasklet work exit, including trace replay, application DMA and synchronization. Individual alloc/free probes bracket only allocator calls. Work-window mean and p99 latencies exclude warmup and destruction. Total covers initialization, preload/warmup, work, output validation and final destruction/drain, ending before final accounting checks. No host transfer, compilation or simulation wall time is counted as DPU time. Frequencies are 350 MHz; results are simulator measurements, not physical UPMEM measurements.

The indexed modes add a dense 64 KiB MRAM ownership map for the same 32 MiB payload capacity. Total includes its initialization. Runtime WRAM heap endpoint is checked against 65,536 B, with inherited 24×1,536 B stack reservations; worst-case stack depth and IRAM hardware feasibility remain unverified. The simulator executable is `../adaptive/runtime/build/uPIMulator`; compilation/linking uses the existing `../combined/run.py` workflow and shared preparation lock.

Each accepted run must finish with zero simulator error, exactly one work interval/tasklet, balanced alloc/free event counts, successful application checks, no failed allocations, complete payload-heap coalescing, zero remaining live/retained pages and three memory snapshots. Graph checks compare independent Python degree/checksum references; native tests additionally check every neighbor for exact membership and uniqueness. Native ASan/UBSan tests check non-overlapping live intervals, sizes on free, payload tags and complete coalescing. A star/chain fixture exercises multi-block adjacency, growth, arbitrary deletion and release.

`results/<tag>/matrix-*.json` declares the configurations before launch. Each run retains `command.json`, exact `task.c`, assembly, binary inputs, `trace.json`, passive `events.csv`, allocator `stats.csv`, `snapshots.csv` and `summary.json`. Source, trace, dataset and simulator SHA-256 values bind the artifacts. Completed configurations are reused only when source and simulator hashes still match.

Earlier `papers-v1`, `papers-v2`, `validation-v1` and `validation-v2` tags contain development attempts with invalid timing labels, and are excluded. LLVM can duplicate zero-instruction labels across fault paths or merge return blocks; dedicated noinline phase functions and fault assertions after call probes avoid those paths. A source-frozen `gate-v3` production Larson run and `validation-v3` matrix passed before the final performance sweep. The analyzer accepts only explicitly named final matrices and requires all declared records.

## Reproduce

From the repository root, with the existing toolchain/runtime prepared:

```sh
python3 pim-reproduction/paper-workloads/test.py
python3 pim-reproduction/paper-workloads/sweep.py --preset validation --tag validation-v3 --jobs 2
python3 pim-reproduction/paper-workloads/run.py --workload larson --mode indexed --policy adaptive --tag gate-v3
python3 pim-reproduction/paper-workloads/sweep.py --preset main --tag papers-v3 --jobs 2
python3 pim-reproduction/paper-workloads/sweep.py --preset heldout --tag heldout-v3 --jobs 1
python3 pim-reproduction/paper-workloads/sweep.py --preset ablation --tag ablation-v3 --jobs 1
pim-reproduction/.venv/bin/python pim-reproduction/paper-workloads/analyze.py
```

Use a new tag after changing the workload code. `--validate` enables extra allocator invariant traversals for small correctness configurations; it is disabled for performance runs. `.stop` is an optional ignored file of tags whose queued launches should be cancelled.

## 128-operation interval experiment

[INTERVAL128.md](INTERVAL128.md) reports a paired change from 64 to 128 operations, with all other settings and traces held constant. It includes exact full-execution DRAM read/write command counts for both tasklet counts, timing comparisons with indexed fixed-2, a second seed, and separate correctness evidence. `DECAY_OPS` changes both the check cadence and idle-decay time measured in operations; per-call bookkeeping still runs.

```sh
python3 pim-reproduction/paper-workloads/test.py --decay-ops 128 --mode indexed --policy adaptive --output-tag interval-128
python3 pim-reproduction/paper-workloads/sweep.py --preset interval-validation --tag validation-interval128-v1 --jobs 2
python3 pim-reproduction/paper-workloads/sweep.py --preset interval --tag interval128-v1 --jobs 4
pim-reproduction/.venv/bin/python pim-reproduction/paper-workloads/analyze_interval.py
```

## Optional slab class

`run.py --slab-kib 16|32|64` uses [the stacked implementation](../stacked/README.md) with indexed lists/lookup and either fixed or adaptive retention. Omitting the option preserves the original allocator and generated sources. The option adds caching for requests in 2049–4096 B; the existing Larson and reduced graph inputs mostly request smaller objects, so they serve as controls rather than evidence of a 4 KiB slab benefit. A 32/64 KiB empty slab exceeds the paper runner's default 16 KiB retention budget; use an explicit equal byte budget for paired comparisons.

```sh
python3 pim-reproduction/paper-workloads/run.py --workload larson --mode indexed --policy adaptive --slab-kib 32 --decay-ops 128 --budget-pages 64 --tag paper-slab-example
```

## Slabs on the unchanged paper workload ports

The `slab-real` matrix evaluates all five workloads at 1/4 tasklets with indexed fixed/adaptive, with and without 64 KiB slabs: 40 performance configurations. Both sides use the same 256 KiB empty-byte budget per tasklet and 128-operation adaptive interval. Workload inputs and allocation sizes are unchanged. Ten separate DPU fixture configurations and ten native sanitizer/oracle cases validate the slab-enabled application paths. The independent allocation-size replay checks which requests can actually use the new class.

```sh
python3 pim-reproduction/paper-workloads/test.py --mode indexed --decay-ops 128 --slab-kib 64 --budget-pages 64 --output-tag slab-real-v1
python3 pim-reproduction/paper-workloads/sweep.py --preset slab-real-validation --tag validation-slab-real-v1 --jobs 2
python3 pim-reproduction/paper-workloads/sweep.py --preset slab-real --tag slab-real-v1 --jobs 4
python3 pim-reproduction/paper-workloads/analyze_slab_real.py
```

[SLAB_REAL.md](SLAB_REAL.md) reports the completed paired timings, full-execution DRAM commands, request-size histograms and memory costs. These remain the documented reduced workload ports, not full original application or hardware measurements.
