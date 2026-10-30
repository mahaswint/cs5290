# Adaptive retention with indexed page management

Measured outcomes, recommended workloads, ablations, held-out checks, and limitations:
**[SUMMARY.md](SUMMARY.md)**. The concise figure is [headline-speedups.png](headline-speedups.png).

This directory combines ideas **1 (adaptive empty-page retention)** and **3 (scalable allocation/free)**.
It preserves the earlier allocator, graph implementation, and Figure 15 experiments. It uses their
maintained buddy backend and correctness repairs, with a separate generated-source/runtime directory.

## Implementation and independent controls

`allocator.c` exposes two independent compile-time mechanisms:

| Mode | Allocation / reclamation | Free routing |
| --- | --- | --- |
| `scan` | Existing page-list scans | Existing class/page scans |
| `lists` | Separate partial, full, and empty page lists | Search those lists |
| `lookup` | Existing page-list scans | MRAM ownership table |
| `indexed` | Separate partial, full, and empty page lists | MRAM ownership table |

The `scan` mode includes the existing `adaptive/allocator.c` verbatim. The other modes use the
same retention rules, buddy backend, eight size classes, 4096 B pages, 24 B in-band header, bitmap
layout, and slot capacity. Full-bitmap patterns are precomputed in the indexed class descriptors
so checking a state transition does not repeat geometry arithmetic on every request.

Allocation prefers a partially occupied page, then a retained empty page, then a buddy refill.
A page moves between the three lists only when its occupancy state changes. Reclamation pops an
empty list directly. A free uses `(pointer - actual_payload_base) >> 12` to address an independently
writable 8 B MRAM entry. It identifies the owning tasklet/class or a large allocation's size.
The payload base need not be aligned to an absolute 4096 B boundary.

Large allocations register only their head entry; `free` accepts allocation bases. Entries are
cleared before pages return to buddy, so later spans cannot inherit stale small-page entries.
No live objects move. Frees must be issued by their allocating tasklet; the table does not add
cross-tasklet synchronization. Frontend lookup/list operations have bounded work; backend traversal,
refills, reclamation and mutex waits still have variable latency.

The table is **64 KiB MRAM for a 32 MiB payload heap**, not WRAM. Initialization and updates are
included in timing. Comparisons keep payload-heap capacity equal and report the additional table
bytes explicitly. List heads and bitmap patterns add 24 B WRAM per tasklet/class: 3072 B at 16
tasklets. The actual linked WRAM bump-heap endpoint is checked against 65536. Buddy metadata,
benchmark arrays, stacks, and application storage are separate from payload reservation. Inherited
IRAM and stack-depth limitations still prevent a complete physical-deployment claim.

Fixed retention (`--keep 1` / `--keep 2`), immediate return (`lazy`), eager population, and adaptive
retention are supported. Fixed and adaptive use the same `--budget-pages` and offered traces.
The adaptive controller is unchanged: refill votes grow targets, idle examinations shrink them,
pressure trims empty pages, and a failed refill drains the tasklet's own empty pages before one retry.
This is a mechanism combination and PIM adaptation, not a claim that pagemaps or adaptive caches
are globally new. See [TCMalloc design](https://google.github.io/tcmalloc/design.html) for related techniques.

## Workloads and scope

| Family | Offered work | Mechanism it tests |
| --- | --- | --- |
| `scatter` | Allocate a bounded pool, free half in a seeded permutation, refill the holes, then destroy the pool | Page-count scaling and scattered free lookup |
| `resident` | Keep a pool live across rounds, replacing half its objects each round | Lookup/reuse without repeated whole-pool teardown dominating work time |
| `handoff` | Repeated bursts in 256 B, 1024 B, 64 B, then 512 B classes | Retaining the currently useful class while cold classes lose their budget |
| `pressure` | 256 B, 1024 B, 2048 B, then 64 B bursts in a constrained heap | Reclamation, allocation failures, and recovery |
| `graph` | Existing Gowalla-sampled paged-adjacency insert/delete kernel | An application workload with real input edges |
| `control` | Eight 64 B objects per burst by default | Low live-set overhead |
| `backend` | Uncached 4096 B allocations | Overhead when frontend improvements cannot help |

These are declared workload families, not production traces. `resident`, `scatter`, and the class
handoff/pressure schedules are synthetic. The graph's edges are sampled from real Gowalla data;
its varying batch schedule is synthetic and its reverse deletion order is limited. Graph code and
sampling are reused from [the existing graph workload](../adaptive/graph/README.md).

Seed changes vary free order or graph samples. No controller threshold was fitted to a result in
this directory. The first pilot exposed modest gains and negative cases; the larger pool experiment
tests an explicit page-count hypothesis. Controls and failures are retained in the reports.

## Validation

```bash
python3 pim-reproduction/combined/test.py
python3 pim-reproduction/combined/sweep.py --preset validation --tag validation-final --jobs 2
```

The native suite uses AddressSanitizer/UndefinedBehaviorSanitizer and the existing independent
live-interval/full-payload oracle for all 4 modes × 4 policies. Added cases preserve half a live
set while filling holes, check every ownership entry after complete reclamation, and require
wrong-tasklet and double frees to fail. It also covers mixed sizes, large spans/root blocks,
OOM recovery, all buddy leaves, retention growth/decay, and complete coalescing.
Native tasklets are interleaved sequentially. Simulator runs exercise actual concurrent tasklets,
payload sentinels, event counts, zero-live teardown, page-list/ownership invariants with `--validate`,
and final root coalescing. Additional 16-tasklet and resident-pool checks are retained separately.

## Reproduce and inspect

```bash
python3 pim-reproduction/combined/sweep.py --preset main --tag combined-v2 --jobs 2
python3 pim-reproduction/combined/sweep.py --preset ablation --tag ablation-v2 --jobs 2
python3 pim-reproduction/combined/sweep.py --preset resident --tag resident-v2 --jobs 2
python3 pim-reproduction/combined/sweep.py --preset large --tag large-v2 --jobs 2
python3 pim-reproduction/combined/run.py --workload resident --mode indexed --policy adaptive \
  --threads 4 --burst 512 --rounds 3 --tag resident-example
pim-reproduction/.venv/bin/python pim-reproduction/combined/analyze.py --tag combined-v2
```

Available sweeps also include `pilot`, `scaling`, and `heldout`. Each sweep saves its offered matrix
before launching. Each run retains exact generated C/assembly, compiler and simulator commands,
source and simulator hashes, raw cycle events, statistics, snapshots, binaries, and a summary.
Completed runs resume only when their generated source hash matches. A superseded source is retained
under a suffixed directory rather than overwritten.

Work time includes payload checks, MRAM pointer-table operations, barriers and snapshots.
For `resident` and `graph`, preload and teardown are outside work time but included in total time;
their allocation/free latency summaries select only requests in work windows. Other microbenchmarks
include all requests in work. Total includes allocator initialization and final drain. Passive
simulator probes do not issue DPU instructions. Empirical p99 uses nearest-rank samples.

Reports compare only identical completed offered work, and include all regressions and OOM outcomes.
An OOM run must not be called faster than a completed run. The fastest successful non-adaptive
scan control is selected separately for each trace, rather than assuming fixed-2 always wins.
