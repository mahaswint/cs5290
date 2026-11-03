# Large-request workload results

These compare unchanged **slab + indexed + fixed/adaptive** allocators on two deliberately selected allocation-heavy workloads: mixed-size Larson (explicit 8–4095 B parameterization) and a variable-size LRU write-cache replay of actual CloudPhysics I/O requests. See [README.md](README.md) for source mapping, original-size preservation, workload details and limitations. These are deterministic 350 MHz single-DPU kernel simulations, not full original applications or hardware measurements.

All pairs use the same input, policy, 32 MiB payload heap, 64 KiB MRAM owner table, 256 KiB empty-byte budget/tasklet and 128-operation adaptive interval. The baseline is indexed **without** the added slab class. Ratios below 1 are regressions. Work excludes preload/warmup, final checking and drain; total includes them and initialization. Host transfers, storage/network service time, compilation and simulation wall time are excluded.

Larson emphasizes allocation/replacement overhead and retains the existing 8 B payload-tag checks. The cache performs full payload processing with the original mixed request sizes. Their speedups measure different application/allocator cost balances. Neither establishes an advantage from the adaptive pressure controller: payload use remains far below the 75% pressure threshold.

**Larson 8–4095 B, four tasklets, 64 KiB slabs:** fixed reaches 8.03x work / 5.50x total; adaptive reaches 6.88x work / 5.14x total, versus the matching no-slab policy.

**CloudPhysics cache, four tasklets, 64 KiB slabs:** fixed reaches 1.07x work / 1.05x total; adaptive reaches 1.07x work / 1.05x total, versus the matching no-slab policy.

## Main comparison

| Workload | Tasklets | Policy | Slab KiB | No slab work ms | Slab work ms | Work speedup | Total speedup | Backend allocations, no slab → slab | Peak payload KiB, no slab → slab |
| --- | ---: | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Larson 8–4095 B | 1 | fixed | 32 | 42.564 | 9.815 | 4.336x | 3.328x | 342 → 68 | 252.0 → 316.0 |
| Larson 8–4095 B | 1 | fixed | 64 | 42.564 | 9.759 | 4.361x | 3.427x | 342 → 65 | 252.0 → 316.0 |
| Larson 8–4095 B | 1 | adaptive | 32 | 43.556 | 10.813 | 4.028x | 3.193x | 346 → 72 | 252.0 → 312.0 |
| Larson 8–4095 B | 1 | adaptive | 64 | 43.556 | 10.752 | 4.051x | 3.283x | 346 → 69 | 252.0 → 312.0 |
| Larson 8–4095 B | 4 | fixed | 32 | 159.826 | 20.221 | 7.904x | 5.203x | 1,416 → 267 | 992.0 → 1256.0 |
| Larson 8–4095 B | 4 | fixed | 64 | 159.826 | 19.894 | 8.034x | 5.502x | 1,416 → 255 | 992.0 → 1256.0 |
| Larson 8–4095 B | 4 | adaptive | 32 | 163.351 | 23.589 | 6.925x | 4.979x | 1,435 → 286 | 964.0 → 1228.0 |
| Larson 8–4095 B | 4 | adaptive | 64 | 163.351 | 23.756 | 6.876x | 5.137x | 1,435 → 274 | 964.0 → 1228.0 |
| CloudPhysics cache | 1 | fixed | 32 | 261.728 | 243.668 | 1.074x | 1.068x | 192 → 89 | 616.0 → 668.0 |
| CloudPhysics cache | 1 | fixed | 64 | 261.728 | 243.539 | 1.075x | 1.070x | 192 → 87 | 616.0 → 700.0 |
| CloudPhysics cache | 1 | adaptive | 32 | 261.980 | 243.910 | 1.074x | 1.068x | 192 → 89 | 616.0 → 668.0 |
| CloudPhysics cache | 1 | adaptive | 64 | 261.980 | 243.782 | 1.075x | 1.070x | 192 → 87 | 616.0 → 700.0 |
| CloudPhysics cache | 4 | fixed | 32 | 122.784 | 115.286 | 1.065x | 1.045x | 183 → 99 | 1408.0 → 1500.0 |
| CloudPhysics cache | 4 | fixed | 64 | 122.784 | 114.528 | 1.072x | 1.049x | 183 → 95 | 1408.0 → 1596.0 |
| CloudPhysics cache | 4 | adaptive | 32 | 122.926 | 114.893 | 1.070x | 1.048x | 183 → 99 | 1408.0 → 1500.0 |
| CloudPhysics cache | 4 | adaptive | 64 | 122.926 | 114.590 | 1.073x | 1.050x | 183 → 95 | 1408.0 → 1596.0 |

Backend allocations count all buddy calls, including direct-bypass objects. They differ from frontend refill counts in the CSV. Payload peak is the actual simultaneous backend high-water mark; it excludes the extra 64 KiB owner map and identical preallocated application/trace storage. Larger slabs may reduce calls while increasing reserved bytes.

Most of the gain over no slab comes from serving previously direct-bypass 2049–4096 B objects through the added frontend class. Comparing 32 against 64 KiB isolates the additional geometry change; 64 KiB is not universally faster than 32 KiB. These results support the selected class and reuse pattern, not an unlimited bigger-is-better rule.

## Offered allocation sizes

These are timed allocation requests, not all trace operations or DRAM transactions. A cache hit may perform payload work without allocating. All sizes and offered operations are identical across geometry and policy variants in a group.

| Workload | Tasklets | Timed allocations | Slab-eligible 2049–4096 B | Direct bypass >4096 B |
| --- | ---: | ---: | ---: | ---: |
| Larson 8–4095 B | 1 | 256 | 128 | 0 |
| Larson 8–4095 B | 4 | 1,024 | 523 | 0 |
| CloudPhysics cache | 1 | 166 | 68 | 44 |
| CloudPhysics cache | 4 | 150 | 54 | 43 |

Larson is a configured benchmark: increasing its upper size parameter is explicit, and its previous 8–999 B result is preserved. The cache retains the original variable I/O lengths, keys, commands and per-owner order. It neither filters for eligible sizes nor rounds requests to 4 KiB. Full payload writes/reads/checks, metadata lookup and LRU decisions remain in timing. Its small trace window supports allocator-path conclusions, not production cache hit-ratio conclusions.

## Held-out seed / trace window

Adaptive, four tasklets, 64 KiB slabs. Larson seed 9173; cache window starts at source record 4096. Every other setting is unchanged.

| Workload | No slab work ms | Slab work ms | Work speedup | Total speedup | Peak payload KiB, no slab → slab |
| --- | ---: | ---: | ---: | ---: | ---: |
| Larson 8–4095 B | 162.309 | 22.754 | 7.133x | 4.953x | 992.0 → 1296.0 |
| CloudPhysics cache | 107.254 | 101.077 | 1.061x | 1.044x | 1464.0 → 1612.0 |

## DRAM counters and memory accounting

Full-execution DRAM read/write commands and bytes include final accounting checks. They exclude WRAM accesses, instruction fetches, row activation/precharge and host transfers; one DMA may split into multiple commands. Work/total timing boundaries are narrower, so these are not work-window or allocator-only traffic counts.

| Workload | Tasklets | Policy | Slab KiB | DRAM reads+writes, no slab → slab | DRAM MiB, no slab → slab | Work-end requested/reserved KiB with slab |
| --- | ---: | --- | ---: | ---: | ---: | ---: |
| Larson 8–4095 B | 1 | fixed | 32 | 168,383 → 53,401 | 1.284 → 0.407 | 113.7 / 312.0 |
| Larson 8–4095 B | 1 | fixed | 64 | 168,383 → 51,265 | 1.284 → 0.391 | 113.7 / 312.0 |
| Larson 8–4095 B | 1 | adaptive | 32 | 170,135 → 55,173 | 1.298 → 0.421 | 113.7 / 304.0 |
| Larson 8–4095 B | 1 | adaptive | 64 | 170,135 → 53,037 | 1.298 → 0.404 | 113.7 / 304.0 |
| Larson 8–4095 B | 4 | fixed | 32 | 666,990 → 186,384 | 5.088 → 1.421 | 507.6 / 1228.0 |
| Larson 8–4095 B | 4 | fixed | 64 | 666,990 → 177,044 | 5.088 → 1.350 | 507.6 / 1228.0 |
| Larson 8–4095 B | 4 | adaptive | 32 | 675,342 → 194,708 | 5.151 → 1.484 | 507.6 / 1192.0 |
| Larson 8–4095 B | 4 | adaptive | 64 | 675,342 → 185,416 | 5.151 → 1.413 | 507.6 / 1192.0 |
| CloudPhysics cache | 1 | fixed | 32 | 699,485 → 652,293 | 5.337 → 4.977 | 401.0 / 516.0 |
| CloudPhysics cache | 1 | fixed | 64 | 699,485 → 650,717 | 5.337 → 4.965 | 401.0 / 548.0 |
| CloudPhysics cache | 1 | adaptive | 32 | 699,485 → 652,287 | 5.337 → 4.977 | 401.0 / 516.0 |
| CloudPhysics cache | 1 | adaptive | 64 | 699,485 → 650,711 | 5.337 → 4.965 | 401.0 / 548.0 |
| CloudPhysics cache | 4 | fixed | 32 | 778,789 → 739,175 | 5.942 → 5.639 | 1050.0 / 1420.0 |
| CloudPhysics cache | 4 | fixed | 64 | 778,789 → 735,889 | 5.942 → 5.614 | 1050.0 / 1484.0 |
| CloudPhysics cache | 4 | adaptive | 32 | 778,895 → 737,975 | 5.942 → 5.630 | 1050.0 / 1420.0 |
| CloudPhysics cache | 4 | adaptive | 64 | 778,895 → 735,825 | 5.942 → 5.614 | 1050.0 / 1484.0 |

## Validation and reproduction

All **28 performance configurations, 12 DPU invariant-validation configurations and 12 native ASan/UBSan cases passed**. The analyzer requires every declared run, audits exact generated sources/traces/simulator/native evidence and matches offered allocation counts. Each run ends with balanced alloc/free calls, zero failures/live/retained bytes and complete buddy coalescing. Maximum WRAM endpoint is 48,932 / 65,536 B; worst-case stacks and IRAM/hardware fit remain unverified.

Native interval oracles check non-overlap and free sizes. Cache checks every payload word and exact final key/size/version membership, and independently predicted hit/allocation/eviction counts. DPU performance runs retain application correctness checks but disable expensive allocator invariant traversal. Earlier allocator tests cover the separate sparse-pinning and tight-heap failure tradeoffs; successful workloads here do not eliminate those costs.

```sh
python3 pim-reproduction/heavy-workloads/test.py
python3 pim-reproduction/heavy-workloads/sweep.py --preset validation --tag validation-heavy-v1 --jobs 2
python3 pim-reproduction/heavy-workloads/sweep.py --preset main --tag heavy-v1 --jobs 4
python3 pim-reproduction/heavy-workloads/sweep.py --preset heldout --tag heldout-heavy-v1 --jobs 4
python3 pim-reproduction/heavy-workloads/analyze.py
```

- [All timing, latency, traffic and memory rows](analysis/all-runs.csv)
- [Paired comparisons](analysis/comparisons.csv)
- [Exact allocation-size histograms](analysis/request-profiles.json)
- [Audit and accepted paths](analysis/audit.json)
