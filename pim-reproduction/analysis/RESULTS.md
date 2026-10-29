# Figure 15 reproduction results

Paper values are rounded labels transcribed from Figure 15. Measured values come from local uPIMulator runs, not digitized bars. The comparison uses allocation-batch completion time divided by 128 requests per thread; the arithmetic mean of individual request latencies is retained separately in the CSV.

| Threads | Size | Buddy: paper / measured (µs) | SW: paper / measured (µs) | Measured speedup |
| --- | --- | ---: | ---: | ---: |
| 1 | 32 B | 630 / 584.66 | 6.9 / 6.99 | 83.66× |
| 1 | 256 B | 520 / 492.92 | 32 / 32.12 | 15.35× |
| 1 | 4096 B | 350 / 362.71 | 310 / 312.71 | 1.16× |
| 16 | 32 B | 13000 / 13026.39 | 51 / 51.86 | 251.17× |
| 16 | 256 B | 11000 / 10911.77 | 430 / 457.72 | 23.84× |
| 16 | 4096 B | 7700 / 7810.92 | 6700 / 6808.97 | 1.15× |

Arithmetic mean of the six speedup ratios: **62.72×**.
Geometric mean speedup over these six configurations: **14.73×**.
Maximum absolute deviation from a printed paper label: **7.20%**.

This geometric mean is defined over the six listed microbenchmark configurations. It does not aggregate workloads or include hardware-cache results.

Capacity audit (29 October 2026): the inherited authors' simulator permits 256 KiB WRAM, while physical UPMEM has 64 KiB. Its fixed 24 × 4 KiB stack reservations place the 16-thread SW image's heap start above the physical limit. The figures above remain measurements of that released simulation configuration, not a demonstrated hardware-valid memory layout. See `../research/WRAM_AND_SLAB_CONSTRAINTS.md`; original measurements and simulator settings are preserved.

Each configuration contains 128 requests per thread. Initialization is excluded; lock waiting is included. Cycles are converted at 350 MHz. The paper labels its metric average latency but does not explicitly state its multithread aggregation formula; using batch completion time / 128 is our interpretation, supported by the comparison, not confirmed author metadata. See ../README.md for harness and host VM configuration differences.
