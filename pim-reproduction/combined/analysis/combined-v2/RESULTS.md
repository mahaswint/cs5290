# Combined retention and indexed page management

Single-DPU simulated measurements at 350 MHz. Scan uses the maintained allocator verbatim. Lists and lookup preserve its page/slot geometry and same-tasklet free contract. All offered configurations, including regressions and allocation failures, are reported.

Work time includes payload checks, pointer transfers, barriers and snapshots; graph work time includes update validation traversals and excludes preload/teardown. Total includes initialization and final drain (graph and resident: also preload/teardown). Allocation/free means for graph/resident use work windows. Do not rank OOM runs by runtime against completed runs.

Peak payload and owner-table bytes are shown separately. The owner table uses 8 B per 4 KiB payload page: 64 KiB for a 32 MiB heap, outside the equal-capacity payload heap. Additional metadata is charged, not hidden. Common buddy metadata, benchmark storage, and WRAM are separate. WRAM endpoint checks are simulation capacity checks, not complete hardware validation.

## backend, 1 tasklets, burst 8, heap 32768 KiB, seed 5290

| Variant | Outcome | Work / total (ms) | Alloc / free mean (us) | Peak payload + map (KiB) | Refills | Failures |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| indexed/adaptive | completed | 17.40 / 17.67 | 158.68 / 109.64 | 32 + 64.00 | 0 | 0 |
| indexed/fixed-1 | completed | 17.24 / 17.51 | 157.63 / 108.18 | 32 + 64.00 | 0 | 0 |
| indexed/fixed-2 | completed | 17.24 / 17.51 | 157.63 / 108.18 | 32 + 64.00 | 0 | 0 |
| indexed/lazy | completed | 17.24 / 17.51 | 157.63 / 108.18 | 32 + 64.00 | 0 | 0 |
| scan/adaptive | completed | 17.27 / 17.34 | 157.69 / 108.69 | 32 + 0.00 | 0 | 0 |
| scan/fixed-1 | completed | 17.18 / 17.25 | 157.19 / 107.77 | 32 + 0.00 | 0 | 0 |
| scan/fixed-2 | completed | 17.18 / 17.25 | 157.19 / 107.77 | 32 + 0.00 | 0 | 0 |
| scan/lazy | completed | 17.17 / 17.24 | 157.06 / 107.70 | 32 + 0.00 | 0 | 0 |

Combined vs fastest successful non-adaptive scan control (scan/lazy): **0.987x** work speedup. Values below 1 are regressions.
Adaptive vs indexed fixed-2: 0.991x.

## backend, 4 tasklets, burst 8, heap 32768 KiB, seed 5290

| Variant | Outcome | Work / total (ms) | Alloc / free mean (us) | Peak payload + map (KiB) | Refills | Failures |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| indexed/adaptive | completed | 68.02 / 68.33 | 559.43 / 360.17 | 128 + 64.00 | 0 | 0 |
| indexed/fixed-1 | completed | 67.88 / 68.19 | 482.08 / 357.26 | 128 + 64.00 | 0 | 0 |
| indexed/fixed-2 | completed | 67.88 / 68.19 | 482.08 / 357.26 | 128 + 64.00 | 0 | 0 |
| indexed/lazy | completed | 67.88 / 68.19 | 482.08 / 357.26 | 128 + 64.00 | 0 | 0 |
| scan/adaptive | completed | 67.85 / 67.95 | 587.04 / 362.09 | 128 + 0.00 | 0 | 0 |
| scan/fixed-1 | completed | 67.84 / 67.93 | 489.58 / 358.73 | 128 + 0.00 | 0 | 0 |
| scan/fixed-2 | completed | 67.84 / 67.93 | 489.58 / 358.73 | 128 + 0.00 | 0 | 0 |
| scan/lazy | completed | 67.88 / 67.97 | 493.88 / 376.56 | 128 + 0.00 | 0 | 0 |

Combined vs fastest successful non-adaptive scan control (scan/fixed-1): **0.997x** work speedup. Values below 1 are regressions.
Adaptive vs indexed fixed-2: 0.998x.

## control, 1 tasklets, burst 8, heap 32768 KiB, seed 5290

| Variant | Outcome | Work / total (ms) | Alloc / free mean (us) | Peak payload + map (KiB) | Refills | Failures |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| indexed/adaptive | completed | 1.77 / 2.15 | 7.49 / 7.48 | 4 + 64.00 | 1 | 0 |
| indexed/fixed-1 | completed | 1.62 / 1.99 | 6.93 / 6.42 | 4 + 64.00 | 1 | 0 |
| indexed/fixed-2 | completed | 1.62 / 1.99 | 6.93 / 6.42 | 4 + 64.00 | 1 | 0 |
| indexed/lazy | completed | 4.51 / 4.79 | 23.79 / 19.75 | 4 + 64.00 | 12 | 0 |
| scan/adaptive | completed | 1.43 / 1.60 | 5.95 / 5.47 | 4 + 0.00 | 1 | 0 |
| scan/fixed-1 | completed | 1.29 / 1.46 | 5.38 / 4.53 | 4 + 0.00 | 1 | 0 |
| scan/fixed-2 | completed | 1.29 / 1.46 | 5.38 / 4.53 | 4 + 0.00 | 1 | 0 |
| scan/lazy | completed | 4.17 / 4.23 | 22.16 / 17.75 | 4 + 0.00 | 12 | 0 |

Combined vs fastest successful non-adaptive scan control (scan/fixed-1): **0.726x** work speedup. Values below 1 are regressions.
Adaptive vs indexed fixed-2: 0.912x.

## control, 4 tasklets, burst 8, heap 32768 KiB, seed 5290

| Variant | Outcome | Work / total (ms) | Alloc / free mean (us) | Peak payload + map (KiB) | Refills | Failures |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| indexed/adaptive | completed | 2.33 / 3.06 | 9.97 / 7.53 | 16 + 64.00 | 4 | 0 |
| indexed/fixed-1 | completed | 2.17 / 2.91 | 9.44 / 6.45 | 16 + 64.00 | 4 | 0 |
| indexed/fixed-2 | completed | 2.17 / 2.91 | 9.44 / 6.45 | 16 + 64.00 | 4 | 0 |
| indexed/lazy | completed | 14.02 / 14.33 | 53.10 / 39.72 | 16 + 64.00 | 48 | 0 |
| scan/adaptive | completed | 1.99 / 2.50 | 8.42 / 5.57 | 16 + 0.00 | 4 | 0 |
| scan/fixed-1 | completed | 1.84 / 2.36 | 7.85 / 4.63 | 16 + 0.00 | 4 | 0 |
| scan/fixed-2 | completed | 1.84 / 2.36 | 7.85 / 4.63 | 16 + 0.00 | 4 | 0 |
| scan/lazy | completed | 13.66 / 13.75 | 51.42 / 37.80 | 16 + 0.00 | 48 | 0 |

Combined vs fastest successful non-adaptive scan control (scan/fixed-1): **0.792x** work speedup. Values below 1 are regressions.
Adaptive vs indexed fixed-2: 0.932x.

## graph, 1 tasklets, burst 128, heap 32768 KiB, seed 5290

| Variant | Outcome | Work / total (ms) | Alloc / free mean (us) | Peak payload + map (KiB) | Refills | Failures |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| indexed/adaptive | completed | 84.56 / 110.39 | 14.40 / 12.62 | 200 + 64.00 | 76 | 0 |
| indexed/fixed-2 | completed | 83.27 / 108.32 | 13.66 / 11.80 | 200 + 64.00 | 76 | 0 |
| scan/adaptive | completed | 87.54 / 113.29 | 19.47 / 11.17 | 200 + 0.00 | 76 | 0 |
| scan/fixed-2 | completed | 86.32 / 111.32 | 18.73 / 10.42 | 200 + 0.00 | 76 | 0 |

Combined vs fastest successful non-adaptive scan control (scan/fixed-2): **1.021x** work speedup. Values below 1 are regressions.
Adaptive vs indexed fixed-2: 0.985x.

## graph, 4 tasklets, burst 128, heap 32768 KiB, seed 5290

| Variant | Outcome | Work / total (ms) | Alloc / free mean (us) | Peak payload + map (KiB) | Refills | Failures |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| indexed/adaptive | completed | 25.49 / 37.16 | 16.47 / 12.42 | 208 + 64.00 | 60 | 0 |
| indexed/fixed-2 | completed | 24.89 / 36.30 | 15.94 / 11.84 | 208 + 64.00 | 60 | 0 |
| scan/adaptive | completed | 24.98 / 36.33 | 16.33 / 10.82 | 208 + 0.00 | 60 | 0 |
| scan/fixed-2 | completed | 24.86 / 36.43 | 15.82 / 9.95 | 208 + 0.00 | 60 | 0 |

Combined vs fastest successful non-adaptive scan control (scan/fixed-2): **0.975x** work speedup. Values below 1 are regressions.
Adaptive vs indexed fixed-2: 0.976x.

## handoff, 1 tasklets, burst 32, heap 32768 KiB, seed 5290

| Variant | Outcome | Work / total (ms) | Alloc / free mean (us) | Peak payload + map (KiB) | Refills | Failures |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| indexed/adaptive | completed | 37.09 / 37.57 | 25.14 / 19.71 | 52 + 64.00 | 86 | 0 |
| indexed/fixed-1 | completed | 39.42 / 40.13 | 27.50 / 20.37 | 48 + 64.00 | 100 | 0 |
| indexed/fixed-2 | completed | 39.46 / 40.16 | 27.55 / 20.37 | 52 + 64.00 | 100 | 0 |
| indexed/lazy | completed | 44.80 / 45.07 | 31.28 / 23.61 | 44 + 64.00 | 120 | 0 |
| scan/adaptive | completed | 36.23 / 36.51 | 25.65 / 18.06 | 52 + 0.00 | 86 | 0 |
| scan/fixed-1 | completed | 38.33 / 38.82 | 27.53 / 18.93 | 48 + 0.00 | 100 | 0 |
| scan/fixed-2 | completed | 38.72 / 39.22 | 27.94 / 19.04 | 52 + 0.00 | 100 | 0 |
| scan/lazy | completed | 43.43 / 43.49 | 31.24 / 21.87 | 44 + 0.00 | 120 | 0 |

Combined vs fastest successful non-adaptive scan control (scan/fixed-1): **1.033x** work speedup. Values below 1 are regressions.
Adaptive vs indexed fixed-2: 1.064x.

## handoff, 4 tasklets, burst 32, heap 32768 KiB, seed 5290

| Variant | Outcome | Work / total (ms) | Alloc / free mean (us) | Peak payload + map (KiB) | Refills | Failures |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| indexed/adaptive | completed | 101.04 / 102.20 | 67.57 / 46.07 | 208 + 64.00 | 344 | 0 |
| indexed/fixed-1 | completed | 112.94 / 114.94 | 77.07 / 50.59 | 192 + 64.00 | 400 | 0 |
| indexed/fixed-2 | completed | 113.76 / 115.77 | 76.73 / 49.15 | 208 + 64.00 | 400 | 0 |
| indexed/lazy | completed | 133.19 / 133.51 | 89.62 / 60.58 | 176 + 64.00 | 480 | 0 |
| scan/adaptive | completed | 100.58 / 101.52 | 65.06 / 44.45 | 208 + 0.00 | 344 | 0 |
| scan/fixed-1 | completed | 112.11 / 113.89 | 75.62 / 50.37 | 192 + 0.00 | 400 | 0 |
| scan/fixed-2 | completed | 112.69 / 114.47 | 75.49 / 47.89 | 208 + 0.00 | 400 | 0 |
| scan/lazy | completed | 132.47 / 132.57 | 88.48 / 59.80 | 176 + 0.00 | 480 | 0 |

Combined vs fastest successful non-adaptive scan control (scan/fixed-1): **1.109x** work speedup. Values below 1 are regressions.
Adaptive vs indexed fixed-2: 1.126x.

## pressure, 1 tasklets, burst 16, heap 64 KiB, seed 5290

| Variant | Outcome | Work / total (ms) | Alloc / free mean (us) | Peak payload + map (KiB) | Refills | Failures |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| indexed/adaptive | completed | 9.10 / 9.21 | 24.76 / 19.09 | 64 + 0.12 | 62 | 0 |
| indexed/fixed-1 | oom | 8.53 / 8.72 | 23.19 / 18.34 | 64 + 0.12 | 61 | 6 |
| indexed/fixed-2 | oom | 7.97 / 8.15 | 21.72 / 17.43 | 64 + 0.12 | 55 | 12 |
| indexed/lazy | completed | 9.85 / 9.89 | 26.58 / 21.16 | 64 + 0.12 | 75 | 0 |
| scan/adaptive | completed | 9.54 / 9.63 | 29.09 / 17.04 | 64 + 0.00 | 62 | 0 |
| scan/fixed-1 | oom | 9.01 / 9.17 | 27.50 / 16.42 | 64 + 0.00 | 61 | 6 |
| scan/fixed-2 | oom | 8.48 / 8.65 | 25.95 / 15.76 | 64 + 0.00 | 55 | 12 |
| scan/lazy | completed | 10.25 / 10.28 | 30.80 / 19.06 | 64 + 0.00 | 75 | 0 |

Combined vs fastest successful non-adaptive scan control (scan/lazy): **1.127x** work speedup. Values below 1 are regressions.

## pressure, 4 tasklets, burst 16, heap 256 KiB, seed 5290

| Variant | Outcome | Work / total (ms) | Alloc / free mean (us) | Peak payload + map (KiB) | Refills | Failures |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| indexed/adaptive | oom | 29.74 / 30.32 | 87.48 / 52.65 | 256 + 0.50 | 241 | 8 |
| indexed/fixed-1 | oom | 29.51 / 30.33 | 76.54 / 52.58 | 256 + 0.50 | 244 | 24 |
| indexed/fixed-2 | oom | 27.18 / 27.99 | 78.10 / 46.69 | 256 + 0.50 | 220 | 48 |
| indexed/lazy | completed | 35.88 / 35.97 | 96.44 / 66.62 | 256 + 0.50 | 300 | 0 |
| scan/adaptive | oom | 29.49 / 30.05 | 84.64 / 49.95 | 256 + 0.00 | 239 | 11 |
| scan/fixed-1 | oom | 29.61 / 30.40 | 83.04 / 53.40 | 256 + 0.00 | 244 | 24 |
| scan/fixed-2 | oom | 27.28 / 28.06 | 77.22 / 45.61 | 256 + 0.00 | 220 | 48 |
| scan/lazy | completed | 35.70 / 35.75 | 92.49 / 65.38 | 256 + 0.00 | 300 | 0 |

## scatter, 1 tasklets, burst 128, heap 32768 KiB, seed 5290

| Variant | Outcome | Work / total (ms) | Alloc / free mean (us) | Peak payload + map (KiB) | Refills | Failures |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| indexed/adaptive | completed | 63.13 / 63.62 | 12.21 / 11.46 | 36 + 64.00 | 88 | 0 |
| indexed/fixed-1 | completed | 63.83 / 64.21 | 12.59 / 11.39 | 36 + 64.00 | 104 | 0 |
| indexed/fixed-2 | completed | 59.48 / 59.96 | 11.50 / 10.59 | 36 + 64.00 | 88 | 0 |
| indexed/lazy | completed | 69.39 / 69.66 | 14.00 / 12.38 | 36 + 64.00 | 125 | 0 |
| scan/adaptive | completed | 66.28 / 66.56 | 14.54 / 10.50 | 36 + 0.00 | 88 | 0 |
| scan/fixed-1 | completed | 65.90 / 66.07 | 14.41 / 10.46 | 36 + 0.00 | 104 | 0 |
| scan/fixed-2 | completed | 62.88 / 63.16 | 13.83 / 9.73 | 36 + 0.00 | 88 | 0 |
| scan/lazy | completed | 70.47 / 70.54 | 15.48 / 11.38 | 36 + 0.00 | 125 | 0 |

Combined vs fastest successful non-adaptive scan control (scan/fixed-2): **0.996x** work speedup. Values below 1 are regressions.
Adaptive vs indexed fixed-2: 0.942x.

## scatter, 4 tasklets, burst 128, heap 32768 KiB, seed 5290

| Variant | Outcome | Work / total (ms) | Alloc / free mean (us) | Peak payload + map (KiB) | Refills | Failures |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| indexed/adaptive | completed | 118.29 / 119.44 | 23.97 / 18.77 | 144 + 64.00 | 352 | 0 |
| indexed/fixed-1 | completed | 129.35 / 130.08 | 26.55 / 20.81 | 144 + 64.00 | 416 | 0 |
| indexed/fixed-2 | completed | 115.24 / 116.39 | 24.21 / 18.21 | 144 + 64.00 | 352 | 0 |
| indexed/lazy | completed | 147.00 / 147.32 | 30.22 / 23.65 | 144 + 64.00 | 500 | 0 |
| scan/adaptive | completed | 123.76 / 124.69 | 26.18 / 18.57 | 144 + 0.00 | 352 | 0 |
| scan/fixed-1 | completed | 132.69 / 133.21 | 28.64 / 19.74 | 144 + 0.00 | 416 | 0 |
| scan/fixed-2 | completed | 120.70 / 121.64 | 25.51 / 17.34 | 144 + 0.00 | 352 | 0 |
| scan/lazy | completed | 148.36 / 148.45 | 32.62 / 23.30 | 144 + 0.00 | 500 | 0 |

Combined vs fastest successful non-adaptive scan control (scan/fixed-2): **1.020x** work speedup. Values below 1 are regressions.
Adaptive vs indexed fixed-2: 0.974x.

## Interpretation limits

Scatter, resident, handoff, pressure, control, and backend are explicit synthetic workload families, chosen to isolate mechanisms. Scatter preserves half a live set while refilling scattered holes; resident keeps a pool live across rounds and replaces half its objects; handoff shifts repeated bursts between classes; pressure constrains the payload heap. The stable small-live-set control and uncached backend control expose overhead. Graph uses the existing reservoir-sampled Gowalla edges and a synthetic batch schedule. These are neither full-dataset throughput results nor recorded production traces.

Simulator executions are deterministic; seeds vary operation order or graph samples, not timing noise. No measured speedup is a guarantee for other workloads. The original Figure 15 results are unchanged.
