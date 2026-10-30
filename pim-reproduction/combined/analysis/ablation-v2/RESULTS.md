# Combined retention and indexed page management

Single-DPU simulated measurements at 350 MHz. Scan uses the maintained allocator verbatim. Lists and lookup preserve its page/slot geometry and same-tasklet free contract. All offered configurations, including regressions and allocation failures, are reported.

Work time includes payload checks, pointer transfers, barriers and snapshots; graph work time includes update validation traversals and excludes preload/teardown. Total includes initialization and final drain (graph and resident: also preload/teardown). Allocation/free means for graph/resident use work windows. Do not rank OOM runs by runtime against completed runs.

Peak payload and owner-table bytes are shown separately. The owner table uses 8 B per 4 KiB payload page: 64 KiB for a 32 MiB heap, outside the equal-capacity payload heap. Additional metadata is charged, not hidden. Common buddy metadata, benchmark storage, and WRAM are separate. WRAM endpoint checks are simulation capacity checks, not complete hardware validation.

## scatter, 1 tasklets, burst 128, heap 32768 KiB, seed 5290

| Variant | Outcome | Work / total (ms) | Alloc / free mean (us) | Peak payload + map (KiB) | Refills | Failures |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| indexed/adaptive | completed | 63.13 / 63.62 | 12.21 / 11.46 | 36 + 64.00 | 88 | 0 |
| indexed/fixed-2 | completed | 59.48 / 59.96 | 11.50 / 10.59 | 36 + 64.00 | 88 | 0 |
| lists/adaptive | completed | 66.35 / 66.64 | 11.74 / 13.33 | 36 + 0.00 | 88 | 0 |
| lists/fixed-2 | completed | 62.49 / 62.78 | 11.00 / 12.40 | 36 + 0.00 | 88 | 0 |
| lookup/adaptive | completed | 65.10 / 65.58 | 14.71 / 9.81 | 36 + 64.00 | 88 | 0 |
| lookup/fixed-2 | completed | 61.71 / 62.19 | 14.01 / 9.04 | 36 + 64.00 | 88 | 0 |
| scan/adaptive | completed | 66.28 / 66.56 | 14.54 / 10.50 | 36 + 0.00 | 88 | 0 |
| scan/fixed-2 | completed | 62.88 / 63.16 | 13.83 / 9.73 | 36 + 0.00 | 88 | 0 |

Combined vs fastest successful non-adaptive scan control (scan/fixed-2): **0.996x** work speedup. Values below 1 are regressions.
Adaptive vs indexed fixed-2: 0.942x.

## scatter, 4 tasklets, burst 128, heap 32768 KiB, seed 5290

| Variant | Outcome | Work / total (ms) | Alloc / free mean (us) | Peak payload + map (KiB) | Refills | Failures |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| indexed/adaptive | completed | 118.29 / 119.44 | 23.97 / 18.77 | 144 + 64.00 | 352 | 0 |
| indexed/fixed-2 | completed | 115.24 / 116.39 | 24.21 / 18.21 | 144 + 64.00 | 352 | 0 |
| lists/adaptive | completed | 120.97 / 121.93 | 23.54 / 20.00 | 144 + 0.00 | 352 | 0 |
| lists/fixed-2 | completed | 118.89 / 119.85 | 23.67 / 19.33 | 144 + 0.00 | 352 | 0 |
| lookup/adaptive | completed | 122.40 / 123.54 | 26.48 / 17.91 | 144 + 64.00 | 352 | 0 |
| lookup/fixed-2 | completed | 119.92 / 121.05 | 26.46 / 17.09 | 144 + 64.00 | 352 | 0 |
| scan/adaptive | completed | 123.76 / 124.69 | 26.18 / 18.57 | 144 + 0.00 | 352 | 0 |
| scan/fixed-2 | completed | 120.70 / 121.64 | 25.51 / 17.34 | 144 + 0.00 | 352 | 0 |

Combined vs fastest successful non-adaptive scan control (scan/fixed-2): **1.020x** work speedup. Values below 1 are regressions.
Adaptive vs indexed fixed-2: 0.974x.

## Interpretation limits

Scatter, resident, handoff, pressure, control, and backend are explicit synthetic workload families, chosen to isolate mechanisms. Scatter preserves half a live set while refilling scattered holes; resident keeps a pool live across rounds and replaces half its objects; handoff shifts repeated bursts between classes; pressure constrains the payload heap. The stable small-live-set control and uncached backend control expose overhead. Graph uses the existing reservoir-sampled Gowalla edges and a synthetic batch schedule. These are neither full-dataset throughput results nor recorded production traces.

Simulator executions are deterministic; seeds vary operation order or graph samples, not timing noise. No measured speedup is a guarantee for other workloads. The original Figure 15 results are unchanged.
