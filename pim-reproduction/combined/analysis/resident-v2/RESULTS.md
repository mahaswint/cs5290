# Combined retention and indexed page management

Single-DPU simulated measurements at 350 MHz. Scan uses the maintained allocator verbatim. Lists and lookup preserve its page/slot geometry and same-tasklet free contract. All offered configurations, including regressions and allocation failures, are reported.

Work time includes payload checks, pointer transfers, barriers and snapshots; graph work time includes update validation traversals and excludes preload/teardown. Total includes initialization and final drain (graph and resident: also preload/teardown). Allocation/free means for graph/resident use work windows. Do not rank OOM runs by runtime against completed runs.

Peak payload and owner-table bytes are shown separately. The owner table uses 8 B per 4 KiB payload page: 64 KiB for a 32 MiB heap, outside the equal-capacity payload heap. Additional metadata is charged, not hidden. Common buddy metadata, benchmark storage, and WRAM are separate. WRAM endpoint checks are simulation capacity checks, not complete hardware validation.

## resident, 1 tasklets, burst 512, heap 32768 KiB, seed 5290

| Variant | Outcome | Work / total (ms) | Alloc / free mean (us) | Peak payload + map (KiB) | Refills | Failures |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| indexed/adaptive | completed | 74.50 / 92.60 | 10.05 / 10.07 | 140 + 64.00 | 111 | 0 |
| indexed/fixed-2 | completed | 69.72 / 87.02 | 9.35 / 9.21 | 140 + 64.00 | 111 | 0 |
| lists/fixed-2 | completed | 74.26 / 92.29 | 8.85 / 11.19 | 140 + 0.00 | 111 | 0 |
| lookup/fixed-2 | completed | 140.41 / 157.90 | 33.86 / 7.71 | 140 + 64.00 | 112 | 0 |
| scan/adaptive | completed | 155.56 / 175.04 | 34.37 / 12.13 | 140 + 0.00 | 112 | 0 |
| scan/fixed-2 | completed | 151.10 / 169.82 | 33.69 / 11.36 | 140 + 0.00 | 112 | 0 |

Combined vs fastest successful non-adaptive scan control (scan/fixed-2): **2.028x** work speedup. Values below 1 are regressions.
Adaptive vs indexed fixed-2: 0.936x.

## resident, 4 tasklets, burst 512, heap 32768 KiB, seed 5290

| Variant | Outcome | Work / total (ms) | Alloc / free mean (us) | Peak payload + map (KiB) | Refills | Failures |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| indexed/adaptive | completed | 121.64 / 160.62 | 18.16 / 14.32 | 560 + 64.00 | 444 | 0 |
| indexed/fixed-2 | completed | 117.98 / 157.15 | 17.48 / 13.83 | 560 + 64.00 | 444 | 0 |
| scan/adaptive | completed | 200.47 / 240.25 | 41.82 / 16.39 | 560 + 0.00 | 448 | 0 |
| scan/fixed-2 | completed | 198.28 / 237.63 | 41.32 / 15.75 | 560 + 0.00 | 448 | 0 |

Combined vs fastest successful non-adaptive scan control (scan/fixed-2): **1.630x** work speedup. Values below 1 are regressions.
Adaptive vs indexed fixed-2: 0.970x.

## Interpretation limits

Scatter, resident, handoff, pressure, control, and backend are explicit synthetic workload families, chosen to isolate mechanisms. Scatter preserves half a live set while refilling scattered holes; resident keeps a pool live across rounds and replaces half its objects; handoff shifts repeated bursts between classes; pressure constrains the payload heap. The stable small-live-set control and uncached backend control expose overhead. Graph uses the existing reservoir-sampled Gowalla edges and a synthetic batch schedule. These are neither full-dataset throughput results nor recorded production traces.

Simulator executions are deterministic; seeds vary operation order or graph samples, not timing noise. No measured speedup is a guarantee for other workloads. The original Figure 15 results are unchanged.
