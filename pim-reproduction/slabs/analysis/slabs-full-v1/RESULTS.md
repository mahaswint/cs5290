# Size-dependent slab results

This experiment implements larger slabs only. It has no adaptive allocation, demand controller, or empty-slab retention.
Baseline sends 2049..4096 B requests directly to the common buddy allocator. Variants split 16/32/64 KiB buddy blocks into 4 KiB slots behind a 24 B header (3/7/15 slots).
The original eight size classes, buddy backend, metadata buffer, initialization mode, workload, and simulator timing are shared.
Latencies are individual request issue cycles / 350 MHz. Total includes initialization, workload, and final drain. Payload checks and barriers are common.
These are controlled microbenchmarks with a maintained baseline; they are not an exact Figure 15 or graph-workload reproduction.
OOM runs have different completed work: speedup ratios are blank unless both cases complete the same requested trace.

| Workload | Threads | Heap KiB | Variant | Outcome | Alloc mean / p99 us | Total ms | Work backend allocations | Cache hits % | Peak reserved KiB | Failures (request/init) |
| --- | ---: | ---: | --- | --- | ---: | ---: | ---: | ---: | ---: | ---: |
| burst | 1 | 32768 | baseline | completed | 160.56 / 166.35 | 37.500 | 128 | 0.0 | 160.0 | 0/0 |
| burst | 1 | 32768 | 16 KiB slab | completed | 58.60 / 180.95 | 15.711 | 44 | 65.6 | 208.0 | 0/0 |
| burst | 1 | 32768 | 32 KiB slab | completed | 25.45 / 151.93 | 8.990 | 20 | 84.4 | 192.0 | 0/0 |
| burst | 1 | 32768 | 64 KiB slab | completed | 16.27 / 146.38 | 7.008 | 12 | 90.6 | 224.0 | 0/0 |
| burst | 2 | 32768 | baseline | completed | 318.61 / 332.66 | 72.710 | 256 | 0.0 | 320.0 | 0/0 |
| burst | 2 | 32768 | 16 KiB slab | completed | 94.99 / 283.89 | 25.721 | 88 | 65.6 | 416.0 | 0/0 |
| burst | 2 | 32768 | 32 KiB slab | completed | 38.73 / 265.49 | 13.718 | 40 | 84.4 | 384.0 | 0/0 |
| burst | 2 | 32768 | 64 KiB slab | completed | 20.14 / 242.10 | 10.286 | 24 | 90.6 | 448.0 | 0/0 |
| burst | 4 | 32768 | baseline | completed | 486.71 / 10498.88 | 145.924 | 512 | 0.0 | 640.0 | 0/0 |
| burst | 4 | 32768 | 16 KiB slab | completed | 184.36 / 1003.67 | 51.515 | 176 | 65.6 | 832.0 | 0/0 |
| burst | 4 | 32768 | 32 KiB slab | completed | 73.98 / 768.88 | 27.291 | 80 | 84.4 | 768.0 | 0/0 |
| burst | 4 | 32768 | 64 KiB slab | completed | 33.03 / 847.60 | 19.176 | 48 | 90.6 | 896.0 | 0/0 |
| burst | 8 | 32768 | baseline | completed | 1138.91 / 2434.90 | 294.461 | 1024 | 0.0 | 1280.0 | 0/0 |
| burst | 8 | 32768 | 16 KiB slab | completed | 350.98 / 2312.31 | 104.152 | 352 | 65.6 | 1664.0 | 0/0 |
| burst | 8 | 32768 | 32 KiB slab | completed | 132.53 / 2131.50 | 54.702 | 160 | 84.4 | 1536.0 | 0/0 |
| burst | 8 | 32768 | 64 KiB slab | completed | 63.37 / 1535.71 | 38.525 | 96 | 90.6 | 1792.0 | 0/0 |
| burst | 16 | 32768 | baseline | completed | 3857.35 / 4054.53 | 861.406 | 2048 | 0.0 | 2560.0 | 0/0 |
| burst | 16 | 32768 | 16 KiB slab | completed | 1148.33 / 3601.15 | 294.416 | 704 | 65.6 | 3328.0 | 0/0 |
| burst | 16 | 32768 | 32 KiB slab | completed | 451.65 / 3282.55 | 147.165 | 320 | 84.4 | 3072.0 | 0/0 |
| burst | 16 | 32768 | 64 KiB slab | completed | 228.04 / 2951.16 | 99.031 | 192 | 90.6 | 3584.0 | 0/0 |
| phases | 1 | 32768 | baseline | completed | 160.19 / 166.35 | 68.399 | 240 | 0.0 | 160.0 | 0/0 |
| phases | 1 | 32768 | 16 KiB slab | completed | 60.06 / 181.01 | 28.537 | 88 | 63.3 | 208.0 | 0/0 |
| phases | 1 | 32768 | 32 KiB slab | completed | 28.65 / 151.93 | 16.430 | 44 | 81.7 | 192.0 | 0/0 |
| phases | 1 | 32768 | 64 KiB slab | completed | 18.72 / 146.38 | 12.414 | 28 | 88.3 | 224.0 | 0/0 |
| phases | 2 | 32768 | baseline | completed | 314.68 / 330.48 | 132.509 | 480 | 0.0 | 320.0 | 0/0 |
| phases | 2 | 32768 | 16 KiB slab | completed | 98.80 / 283.73 | 47.320 | 176 | 63.3 | 416.0 | 0/0 |
| phases | 2 | 32768 | 32 KiB slab | completed | 43.59 / 265.08 | 25.241 | 88 | 81.7 | 384.0 | 0/0 |
| phases | 2 | 32768 | 64 KiB slab | completed | 24.25 / 241.59 | 18.445 | 56 | 88.3 | 448.0 | 0/0 |
| phases | 4 | 32768 | baseline | completed | 630.29 / 823.86 | 267.580 | 960 | 0.0 | 640.0 | 0/0 |
| phases | 4 | 32768 | 16 KiB slab | completed | 185.66 / 999.23 | 95.400 | 352 | 63.3 | 832.0 | 0/0 |
| phases | 4 | 32768 | 32 KiB slab | completed | 80.30 / 657.23 | 50.181 | 176 | 81.7 | 768.0 | 0/0 |
| phases | 4 | 32768 | 64 KiB slab | completed | 41.57 / 540.42 | 34.695 | 112 | 88.3 | 896.0 | 0/0 |
| phases | 8 | 32768 | baseline | completed | 1262.80 / 1970.74 | 536.627 | 1920 | 0.0 | 1280.0 | 0/0 |
| phases | 8 | 32768 | 16 KiB slab | completed | 376.09 / 1766.68 | 190.483 | 704 | 63.3 | 1664.0 | 0/0 |
| phases | 8 | 32768 | 32 KiB slab | completed | 149.69 / 1738.22 | 100.000 | 352 | 81.7 | 1536.0 | 0/0 |
| phases | 8 | 32768 | 64 KiB slab | completed | 76.15 / 1480.19 | 67.686 | 224 | 88.3 | 1792.0 | 0/0 |
| phases | 16 | 32768 | baseline | completed | 3765.00 / 4038.05 | 1564.258 | 3840 | 0.0 | 2560.0 | 0/0 |
| phases | 16 | 32768 | 16 KiB slab | completed | 1151.10 / 3593.77 | 536.232 | 1408 | 63.3 | 3328.0 | 0/0 |
| phases | 16 | 32768 | 32 KiB slab | completed | 471.02 / 3274.73 | 265.400 | 704 | 81.7 | 3072.0 | 0/0 |
| phases | 16 | 32768 | 64 KiB slab | completed | 234.90 / 2955.89 | 169.093 | 448 | 88.3 | 3584.0 | 0/0 |
| sparse | 1 | 32768 | baseline | completed | 160.56 / 166.35 | 37.524 | 128 | 0.0 | 160.0 | 0/0 |
| sparse | 1 | 32768 | 16 KiB slab | completed | 58.62 / 181.01 | 15.757 | 44 | 65.6 | 208.0 | 0/0 |
| sparse | 1 | 32768 | 32 KiB slab | completed | 25.45 / 151.93 | 9.030 | 20 | 84.4 | 192.0 | 0/0 |
| sparse | 1 | 32768 | 64 KiB slab | completed | 16.27 / 146.38 | 7.048 | 12 | 90.6 | 224.0 | 0/0 |
| sparse | 2 | 32768 | baseline | completed | 318.29 / 332.33 | 72.715 | 256 | 0.0 | 320.0 | 0/0 |
| sparse | 2 | 32768 | 16 KiB slab | completed | 94.78 / 283.33 | 25.853 | 88 | 65.6 | 416.0 | 0/0 |
| sparse | 2 | 32768 | 32 KiB slab | completed | 38.62 / 264.93 | 14.302 | 40 | 84.4 | 384.0 | 0/0 |
| sparse | 2 | 32768 | 64 KiB slab | completed | 20.07 / 241.53 | 10.997 | 24 | 90.6 | 448.0 | 0/0 |
| sparse | 4 | 32768 | baseline | completed | 638.80 / 990.03 | 146.139 | 512 | 0.0 | 640.0 | 0/0 |
| sparse | 4 | 32768 | 16 KiB slab | completed | 182.28 / 1008.21 | 51.892 | 176 | 65.6 | 832.0 | 0/0 |
| sparse | 4 | 32768 | 32 KiB slab | completed | 68.01 / 1160.14 | 27.762 | 80 | 84.4 | 768.0 | 0/0 |
| sparse | 4 | 32768 | 64 KiB slab | completed | 35.35 / 662.45 | 20.638 | 48 | 90.6 | 896.0 | 0/0 |
| sparse | 8 | 32768 | baseline | completed | 1287.41 / 2684.15 | 294.502 | 1024 | 0.0 | 1280.0 | 0/0 |
| sparse | 8 | 32768 | 16 KiB slab | completed | 369.14 / 2171.54 | 104.089 | 352 | 65.6 | 1664.0 | 0/0 |
| sparse | 8 | 32768 | 32 KiB slab | completed | 139.96 / 2134.94 | 55.085 | 160 | 84.4 | 1536.0 | 0/0 |
| sparse | 8 | 32768 | 64 KiB slab | completed | 69.96 / 1420.59 | 39.878 | 96 | 90.6 | 1792.0 | 0/0 |
| sparse | 16 | 32768 | baseline | completed | 3855.81 / 4049.37 | 858.810 | 2048 | 0.0 | 2560.0 | 0/0 |
| sparse | 16 | 32768 | 16 KiB slab | completed | 1148.61 / 3602.36 | 292.635 | 704 | 65.6 | 3328.0 | 0/0 |
| sparse | 16 | 32768 | 32 KiB slab | completed | 451.56 / 3278.13 | 145.601 | 320 | 84.4 | 3072.0 | 0/0 |
| sparse | 16 | 32768 | 64 KiB slab | completed | 227.95 / 2950.21 | 98.850 | 192 | 90.6 | 3584.0 | 0/0 |

## Interpretation limits

- A cache hit means an allocation served from an existing frontend page/slab, without a backend refill. It is not a hardware data-cache hit.
- Work backend allocations exclude the successful initial eager population; the raw counter includes it.
- Reserved bytes include live slabs, their headers, free slots, and untouched eager pages. Backend metadata, benchmark storage, and WRAM are separate.
- Sparse tails are sampled after freeing all but object indices 0,16,32,...; every variant receives the same allocation/free trace.
- Peak memory and phase samples do not prove behavior for all lifetime patterns. Larger contiguous refills may fail where a 4 KiB direct allocation succeeds; no direct-allocation fallback is hidden.
- Same-tasklet frees only. WRAM is capped at 64 KiB; inherited IRAM and stack bounds still need validation for a physical hardware image.
- Exact parameters, counters, successful allocation counts, source hashes, and paths are in comparison.csv and per-run summaries.

Plot: burst-00.png / .svg
Plot: phases-01.png / .svg
Plot: sparse-02.png / .svg
