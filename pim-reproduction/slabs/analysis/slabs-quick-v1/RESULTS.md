# Size-dependent slab results

This experiment implements larger slabs only. It has no adaptive allocation, demand controller, or empty-slab retention.
Baseline sends 2049..4096 B requests directly to the common buddy allocator. Variants split 16/32/64 KiB buddy blocks into 4 KiB slots behind a 24 B header (3/7/15 slots).
The original eight size classes, buddy backend, metadata buffer, initialization mode, workload, and simulator timing are shared.
Latencies are individual request issue cycles / 350 MHz. Total includes initialization, workload, and final drain. Payload checks and barriers are common.
These are controlled microbenchmarks with a maintained baseline; they are not an exact Figure 15 or graph-workload reproduction.
OOM runs have different completed work: speedup ratios are blank unless both cases complete the same requested trace.

| Workload | Threads | Heap KiB | Variant | Outcome | Alloc mean / p99 us | Total ms | Work backend allocations | Cache hits % | Peak reserved KiB | Failures (request/init) |
| --- | ---: | ---: | --- | --- | ---: | ---: | ---: | ---: | ---: | ---: |
| burst | 1 | 32768 | baseline | completed | 160.58 / 166.41 | 19.842 | 64 | 0.0 | 160.0 | 0/0 |
| burst | 1 | 32768 | 16 KiB slab | completed | 58.61 / 181.01 | 8.948 | 22 | 65.6 | 208.0 | 0/0 |
| burst | 1 | 32768 | 32 KiB slab | completed | 25.46 / 151.99 | 5.587 | 10 | 84.4 | 192.0 | 0/0 |
| burst | 1 | 32768 | 64 KiB slab | completed | 16.28 / 146.44 | 4.596 | 6 | 90.6 | 224.0 | 0/0 |
| burst | 4 | 32768 | baseline | completed | 488.51 / 9703.51 | 77.338 | 256 | 0.0 | 640.0 | 0/0 |
| burst | 4 | 32768 | 16 KiB slab | completed | 181.66 / 1013.74 | 30.188 | 88 | 65.6 | 832.0 | 0/0 |
| burst | 4 | 32768 | 32 KiB slab | completed | 67.85 / 920.58 | 18.006 | 40 | 84.4 | 768.0 | 0/0 |
| burst | 4 | 32768 | 64 KiB slab | completed | 36.24 / 731.70 | 13.933 | 24 | 90.6 | 896.0 | 0/0 |

## Interpretation limits

- A cache hit means an allocation served from an existing frontend page/slab, without a backend refill. It is not a hardware data-cache hit.
- Work backend allocations exclude the successful initial eager population; the raw counter includes it.
- Reserved bytes include live slabs, their headers, free slots, and untouched eager pages. Backend metadata, benchmark storage, and WRAM are separate.
- Sparse tails are sampled after freeing all but object indices 0,16,32,...; every variant receives the same allocation/free trace.
- Peak memory and phase samples do not prove behavior for all lifetime patterns. Larger contiguous refills may fail where a 4 KiB direct allocation succeeds; no direct-allocation fallback is hidden.
- Same-tasklet frees only. WRAM is capped at 64 KiB; inherited IRAM and stack bounds still need validation for a physical hardware image.
- Exact parameters, counters, successful allocation counts, source hashes, and paths are in comparison.csv and per-run summaries.

Plot: burst-00.png / .svg
