# Indexed retention + slabs + pinned buddy metadata

This experiment adds **authoritative upper buddy metadata in shared WRAM** to the maintained allocator. It leaves payloads, slab headers, ownership tables and lower buddy metadata in MRAM. Historical allocator sources/results remain unchanged.

## Metadata design and consistency

`buddy_backend.inc` is a separate overlay of `../adaptive/buddy_backend.inc`; [origin_manifest.json](origin_manifest.json) pins that starting source. The default pins levels 0–10 (`BUDDY_PIN_LEVELS=11`). For a 32 MiB heap with 4 KiB minimum buddy blocks, the full packed metadata occupies 4,112 B. The top eleven levels occupy 524 B, rounded to a **528 B prefix** at an 8 B DMA boundary. That prefix also includes the first packed word of level 11. Two extra descriptor fields cost 8 B on the DPU, giving 536 B additional dynamic WRAM before any alignment effects. This allocation is **shared per buddy instance**, not per tasklet.

`getMetadata` and `setMetadata` route by packed word index. Words below the boundary live in WRAM; all remaining words use the original 64 B streamed buffer. Both DMA loads and flushes assert that the streamed window begins beyond the pinned prefix. This avoids stale overlapping copies at the boundary. The original buddy mutex still covers allocations, frees and tree propagation; no additional lock or thread-private root copy is introduced. Small heaps clamp the prefix to their actual metadata size.

The MRAM copy of the pinned prefix is an initialized shadow, not a live authority. `syncPinnedMetadata` provides an explicit checkpoint that flushes lower buffered data and copies the WRAM prefix to MRAM. Its caller must hold the buddy lock or quiesce users. Normal allocator operations do not need that checkpoint: every pinned lookup reads shared WRAM. This is a volatile allocator, with the same reset-on-initialization behavior as the maintained baseline.

Native differential tests compare exact returned offsets, every tree node and checkpointed raw metadata bytes with the unmodified backend. They test 8/11/12 pinned levels, 8/64 B stream buffers, 64 KiB/2 MiB/32 MiB heaps, mixed sizes, OOM, alternating-leaf fragmentation, continued use after checkpoints and complete root coalescing. Layered tests retain payload/non-overlap, owner-map, byte-budget, adaptive decay, sparse pinning and invalid-free checks.

The recorded 32 MiB comparisons measure **540 B higher WRAM heap endpoints**: 528 B of pinned metadata, 8 B of descriptor fields and 4 B of allocation alignment padding. The results table reports that measured endpoint difference rather than only the prefix size.

## Requested comparison stages

| Variant | Discovery/free | Retention | Slab class | Pinned metadata |
| --- | --- | --- | --- | --- |
| `baseline` | Scan | Fixed-2 | None | None |
| `indexed-adaptive64` | Indexed lists + owner lookup | Adaptive, interval 64 | None | None |
| `indexed-fixed2` | Indexed lists + owner lookup | Fixed-2 | None | None |
| `slab-adaptive64` | Indexed | Adaptive, interval 64 | 64 KiB spans | None |
| `slab-fixed2` | Indexed | Fixed-2 | 64 KiB spans | None |
| `slab-pinned-adaptive64` | Indexed | Adaptive, interval 64 | 64 KiB spans | 528 B prefix |
| `slab-pinned-fixed2` | Indexed | Fixed-2 | 64 KiB spans | 528 B prefix |

Here `adaptive(64)` means **64 allocation plus free operations per tasklet**, while slab size is separately 64 **KiB**. All adaptive stages use that interval, not the prior 128-operation setting. Fixed-2 retains at most two **empty** pages/spans per class; live/partial spans have no two-span cap. Every stage uses a 32 MiB payload heap and a shared 256 KiB empty-byte budget per tasklet. The indexed variants additionally use the same 64 KiB MRAM owner map. The scan baseline is the maintained scan fixed-2 implementation, not an unmodified original application or a bare-buddy baseline.

Slabs still affect only 2049–4096 B requests, with 15 objects per 64 KiB span because of the 24 B header. Smaller frontend classes keep their original 4 KiB pages; larger requests go directly to buddy. Pinning changes metadata placement, not allocation policy or requested sizes. The full-stage report also compares each pinned stage **directly with its unpinned slab counterpart**, so indexing/slab gains do not get attributed to pinning.

## Workloads

The main matrix contains **35 performance configurations**: five cases × all seven stages, at four tasklets, with identical offered work within a case.

- **Mixed-size Larson 8–4095 B:** the same 64-object/tasklet pool, permutation, 4×pool warmup and 256 timed replacements/tasklet as [the earlier heavy-request experiment](../heavy-workloads/README.md). The upstream size parameterization is explicit.
- **CloudPhysics write cache:** the same original variable I/O sizes, 64 entries/tasklet, 128 global warmup records and 256 timed records from the pinned public sample. Full payload processing, exact LRU/count and final-state checks remain enabled. The first selected window consists of writes; this is a write-cache kernel, not a complete storage application.
- **Wider Larson 8–8191 B:** a new explicit benchmark configuration. It mixes small requests, slab-eligible requests, and frequent requests above the slab limit, helping expose the buddy metadata cost that remains after slabs.
- **Gowalla graph churn:** the existing [faimGraph-style port](../paper-workloads/SOURCES.md), with 3,072 real sampled edges and two deletion/reinsertion rounds, using 256 B linked adjacency blocks. It checks a small-object workload where the new slab class is unused. The independent reference graph and arbitrary deletion/tail compaction are retained.
- **PrIM selection:** the [SEL kernel port](../paper-workloads/SOURCES.md), 32,768 elements and four passes. It is a negative control with zero allocator calls during work; its initialization cost still appears in total time.

Eight held-out runs repeat slab versus slab+pinning under both policies for Larson seed 9173 and CloudPhysics offset 4096. Thirty-five DPU invariant-validation cases use smaller fixtures. Forty-seven native sanitizer executables cover six differential backend configurations (each spanning three heap sizes), six layered allocator configurations and all 35 workload/stage combinations.

These are reduced owner-local workload ports/configurations, not executions of full original CPU/GPU/storage applications. [The earlier source mapping](../paper-workloads/SOURCES.md) and [heavy-request provenance](../heavy-workloads/source_manifest.json) retain original commits and adaptations. The wider Larson range is a new parameter choice, not a claim about the default Larson command. Graph/selection cases and bypass-heavy inputs prevent interpreting one favorable size range as universal improvement.

## Measurement and reproduction

```sh
python3 pim-reproduction/pinned/test.py
python3 pim-reproduction/pinned/sweep.py --preset validation --tag validation-pinned-v1 --jobs 2
python3 pim-reproduction/pinned/sweep.py --preset main --tag pinned-v1 --jobs 4
python3 pim-reproduction/pinned/sweep.py --preset heldout --tag heldout-pinned-v1 --jobs 4
python3 pim-reproduction/pinned/analyze.py
pim-reproduction/.venv/bin/python pim-reproduction/pinned/plot.py
```

Work is concurrent DPU makespan, including application/trace DMA and synchronization. Total additionally includes initialization, preload/warmup, output checking and drain, ending before passive final accounting. DRAM counters cover the full execution including those final checks, exclude WRAM accesses and host transfers, and count commands rather than source DMA calls. The analyzer requires complete declared matrices, exact source/trace/simulator/native hashes, matching offered work and successful heap coalescing. It reports work/total times, incremental pinning gain, DRAM commands/bytes, instructions, backend calls, payload reservation, owner-map MRAM and measured WRAM endpoints.

Results use the existing deterministic 350 MHz single-DPU simulator. A WRAM endpoint below 65,536 B verifies that recorded allocation endpoint, not worst-case stack depth, IRAM fit or hardware throughput. Larger slabs can still pin memory or fail contiguous allocation; those prior costs are unchanged. No controller threshold is fitted to the measured outcomes.

`gate-v1` is an excluded compile failure in the diagnostic checkpoint's pointer qualifier. `gate-v2` passed after correction; accepted comparisons use only the named complete matrices. [RESULTS.md](RESULTS.md) contains the final numerical results and accepted artifact paths.

## Paper workload extension

The separate [paper/](paper/README.md) experiment adds both PIM-malloc graph insertion structures, the paper's six allocation microbenchmark settings (three request sizes × 1/16 tasklets), and a clearly labelled KV allocation-only replay to all seven stages. Its [results](paper/RESULTS.md) preserve all cases and distinguish these ports/replays from the paper's full application and published baselines. This extension does not change the five-case parent matrix or its saved evidence.
