# Slabs on the existing paper-derived workloads

This evaluates **slab + indexed + fixed/adaptive** on the same Larson, PIM linked-block insertion, growing arrays, graph churn and PrIM selection ports previously measured. Inputs, sizes and application code are unchanged. These are reduced single-DPU workload ports, not the complete original paper applications or physical hardware runs; see [SOURCES.md](SOURCES.md).

Every new comparison uses indexed lists and ownership lookup, a 32 MiB payload heap plus the same 64 KiB MRAM owner table, seed 4141, and an equal **256 KiB empty-byte budget per tasklet**. The slab variant adds 64 KiB spans serving 2049–4096 B objects. Fixed retains up to two empty spans/class; adaptive checks every 128 allocations plus frees with maximum two spans/class. Live/partial spans have no two-span cap.

The 256 KiB budget matches the synthetic slab experiment and is larger than the earlier paper experiment's 16 KiB budget. The no-slab baselines were rerun with the new budget and interval, so the ratios isolate enabling the slab implementation.

**Measured result:** no consistent work-time benefit from enabling slabs. Larson work is 4.7–5.5% slower and graph churn is 3.1–4.2% slower. Total runtime on the allocation workloads is 0.4–4.5% slower. Backend refill counts and full-execution DRAM byte totals are identical in every pair.

## Work runtime

Work is concurrent tasklet makespan, including application DMA, trace reads and synchronization, excluding preload/warmup and final checking/destruction. All times are simulated milliseconds at 350 MHz; speedup = same-policy no-slab indexed time / slab indexed time. Values below 1 are regressions.

| Workload | Tasklets | Fixed no slab ms | Fixed + slab ms | Fixed speedup | Adaptive no slab ms | Adaptive + slab ms | Adaptive speedup |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Larson worker | 1 | 38.759 | 40.898 | 0.948x | 41.811 | 43.791 | 0.955x |
| Larson worker | 4 | 39.723 | 41.852 | 0.949x | 42.816 | 44.840 | 0.955x |
| PIM-malloc linked blocks | 1 | 44.060 | 44.823 | 0.983x | 45.118 | 45.714 | 0.987x |
| PIM-malloc linked blocks | 4 | 19.192 | 18.995 | 1.010x | 18.967 | 18.914 | 1.003x |
| PIM-malloc growing arrays | 1 | 19.626 | 20.146 | 0.974x | 20.733 | 21.256 | 0.975x |
| PIM-malloc growing arrays | 4 | 5.864 | 5.894 | 0.995x | 6.213 | 6.322 | 0.983x |
| faimGraph-style updates | 1 | 89.790 | 93.534 | 0.960x | 94.508 | 97.678 | 0.968x |
| faimGraph-style updates | 4 | 24.961 | 25.830 | 0.966x | 26.019 | 26.826 | 0.970x |
| PrIM SEL control | 1 | 39.250 | 39.250 | 1.000x | 39.250 | 39.250 | 1.000x |
| PrIM SEL control | 4 | 14.180 | 14.135 | 1.003x | 14.180 | 14.135 | 1.003x |

## Which allocations exercise slabs?

An independent Python replay reconstructs every allocation size from the unchanged operation traces and data-structure rules, and its full/work allocation and free counts must equal the measured DPU counts. The exact histograms are in [request-profiles.json](analysis/slab-real/request-profiles.json).

| Workload | Tasklets | Request min–max B | All allocations | Timed allocations | Allocations in 2049–4096 B |
| --- | ---: | ---: | ---: | ---: | ---: |
| Larson worker | 1 | 8–999 | 4,608 | 2,048 | 0 |
| Larson worker | 4 | 8–999 | 18,432 | 8,192 | 0 |
| PIM-malloc linked blocks | 1 | 256–256 | 5,372 | 1,687 | 0 |
| PIM-malloc linked blocks | 4 | 256–256 | 5,372 | 1,687 | 0 |
| PIM-malloc growing arrays | 1 | 64–256 | 5,377 | 1,690 | 0 |
| PIM-malloc growing arrays | 4 | 64–256 | 5,377 | 1,690 | 0 |
| faimGraph-style updates | 1 | 256–256 | 8,769 | 3,397 | 0 |
| faimGraph-style updates | 4 | 256–256 | 8,769 | 3,397 | 0 |
| PrIM SEL control | 1 | none | 0 | 0 | 0 |
| PrIM SEL control | 4 | none | 0 | 0 | 0 |

**None of these unchanged inputs requests the new slab class.** The dense 4 KiB synthetic speedups therefore do not carry over to these traces. The small-object paths still use 4 KiB pages. With no slab-class allocations, timing differences cannot be attributed to slab reuse; possible contributors include changed routing, bookkeeping, controller loops, generated code and tasklet interleaving. PrIM SEL is an allocation-free control. A realistic slab benefit needs a workload that naturally allocates/reuses 2049–4096 B objects; changing these request sizes would be a separate modified-workload experiment.

## Total runtime and full-execution memory commands

Total includes initialization, preload/warmup, work, output validation and teardown/drain, ending before passive final accounting. It excludes compilation, simulation wall time and host transfers. DRAM command counts cover the entire DPU execution including final checks, and exclude WRAM accesses, instruction fetches and row activation/precharge commands. They are not allocator-only accesses or C-level DMA-call counts; one DMA can split into multiple DRAM commands. Counts differ slightly despite identical byte totals, consistent with different DMA alignment/row splitting.

| Workload | Tasklets | Policy | No slab total ms | Slab total ms | Total speedup | No slab DRAM reads+writes | Slab DRAM reads+writes |
| --- | ---: | --- | ---: | ---: | ---: | ---: | ---: |
| Larson worker | 1 | fixed | 120.189 | 125.186 | 0.960x | 198,032 | 198,056 |
| Larson worker | 1 | adaptive | 127.844 | 132.483 | 0.965x | 200,254 | 199,960 |
| Larson worker | 4 | fixed | 208.441 | 214.323 | 0.973x | 757,034 | 757,070 |
| Larson worker | 4 | adaptive | 216.691 | 220.386 | 0.983x | 759,222 | 759,208 |
| PIM-malloc linked blocks | 1 | fixed | 238.655 | 245.167 | 0.973x | 696,492 | 696,494 |
| PIM-malloc linked blocks | 1 | adaptive | 246.232 | 251.824 | 0.978x | 696,482 | 696,480 |
| PIM-malloc linked blocks | 4 | fixed | 111.849 | 112.314 | 0.996x | 696,210 | 696,796 |
| PIM-malloc linked blocks | 4 | adaptive | 112.341 | 113.143 | 0.993x | 696,210 | 696,790 |
| PIM-malloc growing arrays | 1 | fixed | 126.811 | 132.531 | 0.957x | 191,291 | 191,299 |
| PIM-malloc growing arrays | 1 | adaptive | 134.486 | 139.839 | 0.962x | 191,283 | 191,298 |
| PIM-malloc growing arrays | 4 | fixed | 40.877 | 42.190 | 0.969x | 193,379 | 193,401 |
| PIM-malloc growing arrays | 4 | adaptive | 42.464 | 43.692 | 0.972x | 193,385 | 193,388 |
| faimGraph-style updates | 1 | fixed | 328.511 | 338.767 | 0.970x | 1,224,273 | 1,224,235 |
| faimGraph-style updates | 1 | adaptive | 340.805 | 349.568 | 0.975x | 1,224,199 | 1,224,229 |
| faimGraph-style updates | 4 | fixed | 136.326 | 137.731 | 0.990x | 1,223,601 | 1,224,619 |
| faimGraph-style updates | 4 | adaptive | 138.047 | 139.366 | 0.991x | 1,223,601 | 1,224,589 |
| PrIM SEL control | 1 | fixed | 198.422 | 198.423 | 1.000x | 256,049 | 256,044 |
| PrIM SEL control | 1 | adaptive | 198.422 | 198.423 | 1.000x | 256,049 | 256,044 |
| PrIM SEL control | 4 | fixed | 114.140 | 114.124 | 1.000x | 256,049 | 256,049 |
| PrIM SEL control | 4 | adaptive | 114.140 | 114.124 | 1.000x | 256,049 | 256,049 |

## Validation, memory and reproduction

All **40 performance configurations, 10 DPU invariant-validation configurations and 10 native ASan/UBSan/oracle cases passed**. Native fixtures check independent non-overlap, payloads, exact graph neighbors and coalescing; DPU checks validate outputs, zero allocation failures, balanced calls, complete coalescing and zero final live/retained memory. Generated C, traces/dataset manifests, simulator and native evidence hashes match. Maximum WRAM heap endpoint across the new DPU runs is 40,868 / 65,536 B. Worst-case stack depth, IRAM fit and hardware throughput remain unverified.

Peak payload, work-end reservation/empty bytes, owner-map bytes, instruction counts, latency distributions and exact run paths are retained in the CSV. The ninth class adds 56 B of cache descriptor per tasklet; there is no active slab payload overhead when its class receives no requests. Timing is deterministic per input, so these runs do not supply independent confidence intervals or original paper speedup claims.

```sh
python3 pim-reproduction/paper-workloads/test.py --mode indexed --decay-ops 128 --slab-kib 64 --budget-pages 64 --output-tag slab-real-v1
python3 pim-reproduction/paper-workloads/sweep.py --preset slab-real-validation --tag validation-slab-real-v1 --jobs 2
python3 pim-reproduction/paper-workloads/sweep.py --preset slab-real --tag slab-real-v1 --jobs 4
python3 pim-reproduction/paper-workloads/analyze_slab_real.py
```

- [Paired timings, traffic and memory](analysis/slab-real/comparisons.csv)
- [Every run, latencies and counters](analysis/slab-real/all-runs.csv)
- [Exact allocation-size histograms](analysis/slab-real/request-profiles.json)
- [Audit and accepted paths](analysis/slab-real/audit.json)
