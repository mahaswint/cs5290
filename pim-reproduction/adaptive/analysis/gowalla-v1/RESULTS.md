# Gowalla small-object dynamic graph updates

A reduced UPMEM adjacency-list implementation using real PrIM/SNAP Gowalla friendship edges. The changing update batches are synthetic, not recorded social-network arrival timestamps. This is a workload port/extension, not an exact Figure 17 reproduction or execution of CUDA faimGraph.

Each peak batch uses the sampled 1:2 new/existing edge split. Both directions of each undirected edge are stored. Each batch inserts edges, traverses/validates adjacency lists, deletes inserted edges in reverse order, and checks restoration. Low phases perform four adjacency read passes; high/rebound one. Batch sizes follow low/high/low-again/rebound for each cycle.

All policies use the same input edges, seed, ownership, payload work, and allocator code. A 64 B graph block is an additional object-size variant; the paper uses 256 B. Allocator pages remain 4096 B for both variants.

Update time and allocation latency exclude graph preload/final teardown. Frontend hit percentages and refill counters include preload; they count allocator requests, not ordinary MRAM reads/writes. Peak reserved memory is measured under the backend lock. OOM runs are not ranked by speed.

Update time spans the earliest update-phase start to the latest finish across tasklets. Individual phase windows can overlap slightly at barrier release, so their duration sum is retained as a diagnostic rather than used as overall elapsed time.

The empirical nearest-rank p99 can be below the mean when fewer than 1% of requests are extremely expensive page refills. Raw samples are retained rather than removing those outliers.

| Tasklets | Object (B) | Policy | Outcome | Update alloc mean / p99 (us) | Update time (ms) | Frontend hits (%) | Peak reserved (KiB) |
| ---: | ---: | --- | --- | ---: | ---: | ---: | ---: |
| 1 | 64 | eager | completed | 5.52 / 3.81 | 61.84 | 98.80 | 76 |
| 1 | 64 | lazy | completed | 5.55 / 3.81 | 61.27 | 98.72 | 48 |
| 1 | 64 | fixed-1 | completed | 5.48 / 6.45 | 60.89 | 98.95 | 48 |
| 1 | 64 | fixed-2 | completed | 5.66 / 9.12 | 60.90 | 99.02 | 48 |
| 1 | 64 | adaptive | completed | 6.39 / 13.68 | 62.15 | 99.02 | 48 |
| 1 | 256 | eager | completed | 22.30 / 304.38 | 91.55 | 93.45 | 228 |
| 1 | 256 | lazy | completed | 22.20 / 307.45 | 90.29 | 93.38 | 200 |
| 1 | 256 | fixed-1 | completed | 20.07 / 307.45 | 87.88 | 93.91 | 200 |
| 1 | 256 | fixed-2 | completed | 18.73 / 307.45 | 86.32 | 94.28 | 200 |
| 1 | 256 | adaptive | completed | 19.48 / 308.31 | 87.54 | 94.28 | 200 |
| 4 | 64 | eager | completed | 8.27 / 173.32 | 19.19 | 98.34 | 160 |
| 4 | 64 | lazy | completed | 8.19 / 170.35 | 18.99 | 98.04 | 48 |
| 4 | 64 | fixed-1 | completed | 4.37 / 4.49 | 15.80 | 99.10 | 48 |
| 4 | 64 | fixed-2 | completed | 4.37 / 4.49 | 15.80 | 99.10 | 48 |
| 4 | 64 | adaptive | completed | 5.08 / 13.06 | 16.15 | 99.10 | 48 |
| 4 | 256 | eager | completed | 26.40 / 592.01 | 30.42 | 93.83 | 320 |
| 4 | 256 | lazy | completed | 27.31 / 597.89 | 30.14 | 93.53 | 208 |
| 4 | 256 | fixed-1 | completed | 20.69 / 448.66 | 27.01 | 94.58 | 208 |
| 4 | 256 | fixed-2 | completed | 15.90 / 436.72 | 24.53 | 95.49 | 208 |
| 4 | 256 | adaptive | completed | 16.28 / 427.22 | 25.11 | 95.49 | 208 |

## Interpretation of this run

- 1 tasklets, 64 B objects: adaptive update time is 2.05% higher than fixed-2, and 0.50% higher than eager. Peak reservation is 48 KiB for adaptive and 48 KiB for fixed-2.
- 1 tasklets, 256 B objects: adaptive update time is 1.41% higher than fixed-2, and 4.39% lower than eager. Peak reservation is 200 KiB for adaptive and 200 KiB for fixed-2.
- 4 tasklets, 64 B objects: adaptive update time is 2.20% higher than fixed-2, and 15.83% lower than eager. Peak reservation is 48 KiB for adaptive and 48 KiB for fixed-2.
- 4 tasklets, 256 B objects: adaptive update time is 2.39% higher than fixed-2, and 17.45% lower than eager. Peak reservation is 208 KiB for adaptive and 208 KiB for fixed-2.

High frontend hit rates support the suitability of this workload for small-object thread caching. They do not by themselves demonstrate that adaptive retention improves on fixed retention. The graph kernel uses one size class at a time, so it does not exercise migration of demand between allocator size classes. The present controller checks idleness in allocator-operation counts; elapsed graph-traversal time alone does not make a class decay. Block-size comparisons also change graph payload transfer size and adjacency capacity, so their runtime difference is not solely an allocator effect.
Pressure trims: 0; emergency trims: 0. A separate constrained-heap experiment is needed to evaluate reclamation.


Plots: comparison-64B / comparison-256B, phases-t<tasklets>-<object>B, and latency-over-time (PNG/SVG). Time plots show each policy's own simulated phase boundaries.

Per-request cycles, graph phase durations, memory snapshots, generated input with original vertex IDs, source hashes, and binaries are retained in every run folder. The inherited simulator/IRAM limitations and reconstructed baseline repairs in the parent README still apply.
