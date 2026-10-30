# Combined retention and indexed page management

Single-DPU simulated measurements at 350 MHz. Scan uses the maintained allocator verbatim. Lists and lookup preserve its page/slot geometry and same-tasklet free contract. All offered configurations, including regressions and allocation failures, are reported.

Work time includes payload checks, pointer transfers, barriers and snapshots; graph work time includes update validation traversals and excludes preload/teardown. Total includes initialization and final drain (graph and resident: also preload/teardown). Allocation/free means for graph/resident use work windows. Do not rank OOM runs by runtime against completed runs.

Peak payload and owner-table bytes are shown separately. The owner table uses 8 B per 4 KiB payload page: 64 KiB for a 32 MiB heap, outside the equal-capacity payload heap. Additional metadata is charged, not hidden. Common buddy metadata, benchmark storage, and WRAM are separate. WRAM endpoint checks are simulation capacity checks, not complete hardware validation.

## scatter, 1 tasklets, burst 512, heap 32768 KiB, seed 5290

| Variant | Outcome | Work / total (ms) | Alloc / free mean (us) | Peak payload + map (KiB) | Refills | Failures |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| indexed/adaptive | completed | 206.38 / 206.87 | 15.89 / 13.97 | 140 + 64.00 | 371 | 0 |
| indexed/fixed-2 | completed | 196.82 / 197.30 | 15.18 / 13.11 | 140 + 64.00 | 371 | 0 |
| scan/adaptive | completed | 241.25 / 241.53 | 20.93 / 14.60 | 140 + 0.00 | 371 | 0 |
| scan/fixed-2 | completed | 232.14 / 232.42 | 20.22 / 13.83 | 140 + 0.00 | 371 | 0 |

Combined vs fastest successful non-adaptive scan control (scan/fixed-2): **1.125x** work speedup. Values below 1 are regressions.
Adaptive vs indexed fixed-2: 0.954x.

## scatter, 4 tasklets, burst 512, heap 32768 KiB, seed 5290

| Variant | Outcome | Work / total (ms) | Alloc / free mean (us) | Peak payload + map (KiB) | Refills | Failures |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| indexed/adaptive | completed | 408.89 / 410.05 | 35.97 / 24.73 | 560 + 64.00 | 1484 | 0 |
| indexed/fixed-2 | completed | 408.82 / 409.98 | 35.78 / 24.02 | 560 + 64.00 | 1484 | 0 |
| scan/adaptive | completed | 427.18 / 428.12 | 38.57 / 24.73 | 560 + 0.00 | 1484 | 0 |
| scan/fixed-2 | completed | 425.22 / 426.16 | 38.26 / 24.46 | 560 + 0.00 | 1484 | 0 |

Combined vs fastest successful non-adaptive scan control (scan/fixed-2): **1.040x** work speedup. Values below 1 are regressions.
Adaptive vs indexed fixed-2: 1.000x.

## Interpretation limits

Scatter, resident, handoff, pressure, control, and backend are explicit synthetic workload families, chosen to isolate mechanisms. Scatter preserves half a live set while refilling scattered holes; resident keeps a pool live across rounds and replaces half its objects; handoff shifts repeated bursts between classes; pressure constrains the payload heap. The stable small-live-set control and uncached backend control expose overhead. Graph uses the existing reservoir-sampled Gowalla edges and a synthetic batch schedule. These are neither full-dataset throughput results nor recorded production traces.

Simulator executions are deterministic; seeds vary operation order or graph samples, not timing noise. No measured speedup is a guarantee for other workloads. The original Figure 15 results are unchanged.
