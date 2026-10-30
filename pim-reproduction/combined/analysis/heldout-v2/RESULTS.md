# Combined retention and indexed page management

Single-DPU simulated measurements at 350 MHz. Scan uses the maintained allocator verbatim. Lists and lookup preserve its page/slot geometry and same-tasklet free contract. All offered configurations, including regressions and allocation failures, are reported.

Work time includes payload checks, pointer transfers, barriers and snapshots; graph work time includes update validation traversals and excludes preload/teardown. Total includes initialization and final drain (graph and resident: also preload/teardown). Allocation/free means for graph/resident use work windows. Do not rank OOM runs by runtime against completed runs.

Peak payload and owner-table bytes are shown separately. The owner table uses 8 B per 4 KiB payload page: 64 KiB for a 32 MiB heap, outside the equal-capacity payload heap. Additional metadata is charged, not hidden. Common buddy metadata, benchmark storage, and WRAM are separate. WRAM endpoint checks are simulation capacity checks, not complete hardware validation.

## graph, 1 tasklets, burst 128, heap 32768 KiB, seed 9173

| Variant | Outcome | Work / total (ms) | Alloc / free mean (us) | Peak payload + map (KiB) | Refills | Failures |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| indexed/adaptive | completed | 85.37 / 111.20 | 14.57 / 12.82 | 204 + 64.00 | 78 | 0 |
| indexed/fixed-2 | completed | 84.09 / 109.16 | 13.89 / 11.97 | 204 + 64.00 | 78 | 0 |
| scan/adaptive | completed | 88.60 / 114.34 | 19.87 / 11.36 | 204 + 0.00 | 78 | 0 |
| scan/fixed-2 | completed | 87.35 / 112.34 | 19.17 / 10.57 | 204 + 0.00 | 78 | 0 |

Combined vs fastest successful non-adaptive scan control (scan/fixed-2): **1.023x** work speedup. Values below 1 are regressions.
Adaptive vs indexed fixed-2: 0.985x.

## graph, 1 tasklets, burst 128, heap 32768 KiB, seed 24107

| Variant | Outcome | Work / total (ms) | Alloc / free mean (us) | Peak payload + map (KiB) | Refills | Failures |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| indexed/adaptive | completed | 83.92 / 109.61 | 14.38 / 12.65 | 200 + 64.00 | 76 | 0 |
| indexed/fixed-2 | completed | 82.77 / 107.69 | 13.66 / 11.80 | 200 + 64.00 | 76 | 0 |
| scan/adaptive | completed | 87.03 / 112.65 | 19.59 / 11.22 | 200 + 0.00 | 76 | 0 |
| scan/fixed-2 | completed | 85.96 / 110.85 | 18.87 / 10.45 | 200 + 0.00 | 76 | 0 |

Combined vs fastest successful non-adaptive scan control (scan/fixed-2): **1.024x** work speedup. Values below 1 are regressions.
Adaptive vs indexed fixed-2: 0.986x.

## graph, 4 tasklets, burst 128, heap 32768 KiB, seed 9173

| Variant | Outcome | Work / total (ms) | Alloc / free mean (us) | Peak payload + map (KiB) | Refills | Failures |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| indexed/adaptive | completed | 25.58 / 37.21 | 16.84 / 12.43 | 208 + 64.00 | 60 | 0 |
| indexed/fixed-2 | completed | 25.50 / 36.90 | 16.42 / 11.74 | 208 + 64.00 | 60 | 0 |
| scan/adaptive | completed | 25.59 / 36.85 | 17.14 / 10.93 | 208 + 0.00 | 60 | 0 |
| scan/fixed-2 | completed | 25.28 / 36.45 | 16.58 / 10.21 | 208 + 0.00 | 60 | 0 |

Combined vs fastest successful non-adaptive scan control (scan/fixed-2): **0.988x** work speedup. Values below 1 are regressions.
Adaptive vs indexed fixed-2: 0.997x.

## graph, 4 tasklets, burst 128, heap 32768 KiB, seed 24107

| Variant | Outcome | Work / total (ms) | Alloc / free mean (us) | Peak payload + map (KiB) | Refills | Failures |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| indexed/adaptive | completed | 27.63 / 39.25 | 19.45 / 13.96 | 208 + 64.00 | 66 | 0 |
| indexed/fixed-2 | completed | 26.95 / 38.06 | 18.56 / 13.38 | 208 + 64.00 | 66 | 0 |
| scan/adaptive | completed | 27.49 / 38.58 | 18.40 / 12.45 | 208 + 0.00 | 66 | 0 |
| scan/fixed-2 | completed | 26.95 / 37.90 | 17.94 / 12.18 | 208 + 0.00 | 66 | 0 |

Combined vs fastest successful non-adaptive scan control (scan/fixed-2): **0.975x** work speedup. Values below 1 are regressions.
Adaptive vs indexed fixed-2: 0.975x.

## handoff, 1 tasklets, burst 32, heap 32768 KiB, seed 9173

| Variant | Outcome | Work / total (ms) | Alloc / free mean (us) | Peak payload + map (KiB) | Refills | Failures |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| indexed/adaptive | completed | 25.38 / 25.86 | 25.99 / 20.13 | 52 + 64.00 | 60 | 0 |
| indexed/fixed-2 | completed | 26.50 / 27.20 | 27.92 / 20.37 | 52 + 64.00 | 68 | 0 |
| scan/adaptive | completed | 24.80 / 25.09 | 26.44 / 18.54 | 52 + 0.00 | 60 | 0 |
| scan/fixed-2 | completed | 26.00 / 26.50 | 28.29 / 19.05 | 52 + 0.00 | 68 | 0 |

Combined vs fastest successful non-adaptive scan control (scan/fixed-2): **1.025x** work speedup. Values below 1 are regressions.
Adaptive vs indexed fixed-2: 1.044x.

## handoff, 1 tasklets, burst 32, heap 32768 KiB, seed 24107

| Variant | Outcome | Work / total (ms) | Alloc / free mean (us) | Peak payload + map (KiB) | Refills | Failures |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| indexed/adaptive | completed | 25.35 / 25.84 | 25.95 / 20.13 | 52 + 64.00 | 60 | 0 |
| indexed/fixed-2 | completed | 26.46 / 27.16 | 27.85 / 20.36 | 52 + 64.00 | 68 | 0 |
| scan/adaptive | completed | 24.82 / 25.10 | 26.42 / 18.59 | 52 + 0.00 | 60 | 0 |
| scan/fixed-2 | completed | 26.00 / 26.50 | 28.23 / 19.11 | 52 + 0.00 | 68 | 0 |

Combined vs fastest successful non-adaptive scan control (scan/fixed-2): **1.026x** work speedup. Values below 1 are regressions.
Adaptive vs indexed fixed-2: 1.044x.

## handoff, 4 tasklets, burst 32, heap 32768 KiB, seed 9173

| Variant | Outcome | Work / total (ms) | Alloc / free mean (us) | Peak payload + map (KiB) | Refills | Failures |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| indexed/adaptive | completed | 69.96 / 71.12 | 69.39 / 47.10 | 208 + 64.00 | 240 | 0 |
| indexed/fixed-2 | completed | 76.40 / 78.41 | 76.15 / 49.82 | 208 + 64.00 | 272 | 0 |
| scan/adaptive | completed | 69.44 / 70.37 | 68.37 / 45.06 | 208 + 0.00 | 240 | 0 |
| scan/fixed-2 | completed | 75.64 / 77.42 | 75.44 / 49.18 | 208 + 0.00 | 272 | 0 |

Combined vs fastest successful non-adaptive scan control (scan/fixed-2): **1.081x** work speedup. Values below 1 are regressions.
Adaptive vs indexed fixed-2: 1.092x.

## handoff, 4 tasklets, burst 32, heap 32768 KiB, seed 24107

| Variant | Outcome | Work / total (ms) | Alloc / free mean (us) | Peak payload + map (KiB) | Refills | Failures |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| indexed/adaptive | completed | 70.15 / 71.31 | 69.40 / 47.05 | 208 + 64.00 | 240 | 0 |
| indexed/fixed-2 | completed | 77.03 / 79.03 | 76.30 / 50.04 | 208 + 64.00 | 272 | 0 |
| scan/adaptive | completed | 69.48 / 70.42 | 68.37 / 46.25 | 208 + 0.00 | 240 | 0 |
| scan/fixed-2 | completed | 76.33 / 78.12 | 76.11 / 49.74 | 208 + 0.00 | 272 | 0 |

Combined vs fastest successful non-adaptive scan control (scan/fixed-2): **1.088x** work speedup. Values below 1 are regressions.
Adaptive vs indexed fixed-2: 1.098x.

## resident, 1 tasklets, burst 256, heap 32768 KiB, seed 9173

| Variant | Outcome | Work / total (ms) | Alloc / free mean (us) | Peak payload + map (KiB) | Refills | Failures |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| indexed/adaptive | completed | 27.04 / 36.33 | 11.31 / 10.95 | 72 + 64.00 | 51 | 0 |
| indexed/fixed-2 | completed | 25.42 / 34.30 | 10.60 / 10.09 | 72 + 64.00 | 51 | 0 |
| scan/adaptive | completed | 41.25 / 50.36 | 24.25 / 11.89 | 72 + 0.00 | 58 | 0 |
| scan/fixed-2 | completed | 39.74 / 48.47 | 23.55 / 11.12 | 72 + 0.00 | 58 | 0 |

Combined vs fastest successful non-adaptive scan control (scan/fixed-2): **1.470x** work speedup. Values below 1 are regressions.
Adaptive vs indexed fixed-2: 0.940x.

## resident, 1 tasklets, burst 256, heap 32768 KiB, seed 24107

| Variant | Outcome | Work / total (ms) | Alloc / free mean (us) | Peak payload + map (KiB) | Refills | Failures |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| indexed/adaptive | completed | 21.30 / 30.59 | 7.99 / 8.67 | 72 + 64.00 | 30 | 0 |
| indexed/fixed-2 | completed | 19.69 / 28.57 | 7.28 / 7.81 | 72 + 64.00 | 30 | 0 |
| scan/adaptive | completed | 42.52 / 51.47 | 28.01 / 9.37 | 72 + 0.00 | 32 | 0 |
| scan/fixed-2 | completed | 41.04 / 49.60 | 27.33 / 8.61 | 72 + 0.00 | 32 | 0 |

Combined vs fastest successful non-adaptive scan control (scan/fixed-2): **1.927x** work speedup. Values below 1 are regressions.
Adaptive vs indexed fixed-2: 0.924x.

## resident, 4 tasklets, burst 256, heap 32768 KiB, seed 9173

| Variant | Outcome | Work / total (ms) | Alloc / free mean (us) | Peak payload + map (KiB) | Refills | Failures |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| indexed/adaptive | completed | 48.78 / 69.25 | 21.48 / 16.93 | 288 + 64.00 | 204 | 0 |
| indexed/fixed-2 | completed | 48.04 / 68.35 | 20.13 / 16.40 | 288 + 64.00 | 204 | 0 |
| scan/adaptive | completed | 67.71 / 87.94 | 35.39 / 19.87 | 288 + 0.00 | 232 | 0 |
| scan/fixed-2 | completed | 66.34 / 86.26 | 35.87 / 19.18 | 288 + 0.00 | 232 | 0 |

Combined vs fastest successful non-adaptive scan control (scan/fixed-2): **1.360x** work speedup. Values below 1 are regressions.
Adaptive vs indexed fixed-2: 0.985x.

## resident, 4 tasklets, burst 256, heap 32768 KiB, seed 24107

| Variant | Outcome | Work / total (ms) | Alloc / free mean (us) | Peak payload + map (KiB) | Refills | Failures |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| indexed/adaptive | completed | 29.95 / 50.24 | 11.17 / 10.63 | 288 + 64.00 | 120 | 0 |
| indexed/fixed-2 | completed | 28.47 / 48.55 | 10.75 / 9.96 | 288 + 64.00 | 120 | 0 |
| scan/adaptive | completed | 52.26 / 72.73 | 31.85 / 11.61 | 288 + 0.00 | 128 | 0 |
| scan/fixed-2 | completed | 50.86 / 70.94 | 31.31 / 11.02 | 288 + 0.00 | 128 | 0 |

Combined vs fastest successful non-adaptive scan control (scan/fixed-2): **1.698x** work speedup. Values below 1 are regressions.
Adaptive vs indexed fixed-2: 0.951x.

## Interpretation limits

Scatter, resident, handoff, pressure, control, and backend are explicit synthetic workload families, chosen to isolate mechanisms. Scatter preserves half a live set while refilling scattered holes; resident keeps a pool live across rounds and replaces half its objects; handoff shifts repeated bursts between classes; pressure constrains the payload heap. The stable small-live-set control and uncached backend control expose overhead. Graph uses the existing reservoir-sampled Gowalla edges and a synthetic batch schedule. These are neither full-dataset throughput results nor recorded production traces.

Simulator executions are deterministic; seeds vary operation order or graph samples, not timing noise. No measured speedup is a guarantee for other workloads. The original Figure 15 results are unchanged.
