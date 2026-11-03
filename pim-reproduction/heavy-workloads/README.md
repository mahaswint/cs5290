# Workloads with frequent large allocation requests

These experiments target the added 2049–4096 B class in **slab + indexed + fixed/adaptive**. High DRAM traffic alone does not imply allocator speedup: larger spans help when eligible objects are allocated/freed frequently and their slots can be reused. The allocator and controller are unchanged from `../stacked/`.

## Selected workloads and provenance

- **Mixed-size Larson:** the original owner-local Larson port with its configurable exclusive upper size limit set to 4096, giving uniform 8–4095 B requests. This explicitly changes the prior 8–999 B parameterization. It retains the upstream RNG, pool permutation, 4×pool warmup replacements and random victim replacement. There are 64 live objects and 256 timed replacements **per tasklet**. Cross-thread frees/worker respawning are outside the port. The [upstream source](https://github.com/daanx/mimalloc-bench/blob/c65a68834c8fbd619f3ef449f72b9ec659cb551e/bench/larson/larson.cpp) accepts minimum and maximum sizes as command-line arguments. This is a configured allocator benchmark, not a new unchanged application.
- **CloudPhysics variable-size LRU write-cache replay:** actual I/O lengths, logical-block keys, operation types and per-owner request order from the [libCacheSim CloudPhysics sample](https://github.com/1a1a11a/libCacheSim/blob/c6ffd976c83d9e731b37fc91e1ce930039f5ddb9/data/cloudPhysicsIO.csv). Every key is assigned to one tasklet by `(key ^ (key >> 16)) % tasklets`, with 64 LRU entries per tasklet. The first 128 global records warm the cache; the next 256 are timed. Request lengths are **not rounded or filtered** into the slab class; the trace includes small requests and direct-bypass requests up to tens of KiB. The kernel reads/checks and writes full buffer payloads, reuses same-size cache hits, and frees/reallocates on eviction or size change. Cache metadata is separate WRAM storage, identical across allocator variants. Data contents are deterministic because the trace supplies no payload bytes.

The [CloudPhysics dataset description](https://github.com/cacheMon/cache_dataset#cloudphysics-traces) associates these I/O traces with [Efficient MRC Construction with SHARDS (FAST 2015)](https://www.usenix.org/conference/fast15/technical-sessions/presentation/waldspurger). This replay models a bounded variable-size write cache, not RocksDB, the original VM application, or a storage device. Timestamps, storage/network latency and host transfers are omitted. Cache capacity is an entry count, not a byte limit. The source repository warns that this small sample is unsuitable for production miss-ratio or cache-policy claims; we compare allocators under the **same** LRU decisions, and make neither claim. The separate [RocksDB memory documentation](https://github.com/meta/rocksdb/wiki/Memory-usage-in-RocksDB) motivates block caching as an allocation-heavy setting but also notes larger production block sizes; we do not treat every database allocation as exactly 4 KiB.

The complete 3,116,791 B source CSV is retained unmodified. Its SHA-256 is `987ff2213050e47d24e8ba6e010d4b3127e51aafef6a76a8a6d43d13b9156fa1`; its Git blob SHA-1 matches the pinned upstream file. [source_manifest.json](source_manifest.json) records source URLs, commits and explicit adaptations. The legacy paper-port generator still produces the exact old sources/traces when the new optional Larson parameter is absent.

## Declared comparison

The main matrix has **24 configurations**: two workloads × 1/4 tasklets × fixed/adaptive × no slab/32/64 KiB slabs. Every case uses indexed lists and direct ownership lookup, a 32 MiB payload heap plus a 64 KiB owner table, equal 256 KiB empty-byte budgets per tasklet, fixed keep=2, adaptive maximum=2 and interval=128 allocations plus frees. A fixed cap counts empty spans, not live spans. All geometry choices are declared before timing; all are reported, including any regressions.

Four held-out configurations repeat no slab versus 64 KiB with adaptive at four tasklets: Larson seed 9173 and CloudPhysics records starting at offset 4096. Twelve DPU correctness configurations use smaller traces with invariant checks. Twelve native ASan/UBSan cases cover both workloads/policies and all geometries. Existing allocator tests separately cover OOM, fragmentation, sparse pinning, owner-map lifetime and wrong-pointer rejection.

Larson scales pool/work per tasklet; the cache keeps the global trace length fixed while increasing total cache capacity with tasklet count. Cross-tasklet rows therefore are not an isolated scaling experiment. Compare slab variants **within** each tasklet/policy/input group.

Larger spans reserve more bytes and require contiguous blocks. Mixed live sizes/lifetimes can leave partially full slabs pinned. Peak payload, requested/slot/reserved/empty bytes, owner-map cost and WRAM endpoint accompany timing. The report separates backend allocation calls from frontend refill counters: direct-bypass allocations are backend calls even though they do not refill a frontend cache.

## Reproduce

```sh
python3 pim-reproduction/heavy-workloads/test.py
python3 pim-reproduction/heavy-workloads/sweep.py --preset validation --tag validation-heavy-v1 --jobs 2
python3 pim-reproduction/heavy-workloads/sweep.py --preset main --tag heavy-v1 --jobs 4
python3 pim-reproduction/heavy-workloads/sweep.py --preset heldout --tag heldout-heavy-v1 --jobs 4
python3 pim-reproduction/heavy-workloads/analyze.py
```

Work is concurrent DPU makespan including trace reads, payload work and synchronization. Total includes initialization, warmup/preload, work, final output checks and drain, ending before final accounting. Full DRAM counters include those final checks, exclude WRAM accesses/host transfers, and count read/write commands rather than source-level DMA calls. These are deterministic 350 MHz single-DPU simulations, not hardware or complete original application results. A WRAM endpoint below 65,536 B does not establish worst-case stack depth or IRAM fit.

[RESULTS.md](RESULTS.md) reports the completed timings, traffic, allocator calls, memory costs, request distributions and held-out outcomes.
