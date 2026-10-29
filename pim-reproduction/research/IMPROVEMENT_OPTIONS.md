# Software improvement options for PIM-malloc

Implementation update: the user selected adaptive empty-page retention. It is now implemented and correctness-tested in [adaptive/README.md](/home/mahaswint/cs5290/pim-reproduction/adaptive/README.md). The discussion below records the earlier research shortlist; the full contribution comparison remains to be run.

Research date: 29 October 2026. These are implementation proposals, not measured improvements. Scope: the existing UPMEM SDK/uPIMulator setup, software allocator changes only; hardware buddy cache and language-model serving remain excluded.

Follow-up audit: [WRAM_AND_SLAB_CONSTRAINTS.md](/home/mahaswint/cs5290/pim-reproduction/research/WRAM_AND_SLAB_CONSTRAINTS.md) quantifies metadata-cache space, separates explicit author statements from inferred slab-size tradeoffs, and records a material capacity limitation: the released simulator uses 256 KiB WRAM rather than the physical 64 KiB. A successful simulation is not a hardware-fit check. The paper also reports that software LRU for buddy metadata slowed its 16-thread, 4 KiB benchmark by 29%; keep any proposed frontend cache simple and evaluate its bookkeeping costs.

Expanded review: [CANDIDATE_IMPLEMENTABILITY.md](/home/mahaswint/cs5290/pim-reproduction/research/CANDIDATE_IMPLEMENTABILITY.md) assesses the eight directions in the user's later research proposal. For a larger policy contribution, it recommends adaptive empty-page retention, with partial-page lists/direct lookup as an alternative and pinned buddy metadata as the smallest focused pilot. The slab experiment below remains a small prototype option; no policy has been implemented or selected by the user yet.

## Recommendation

Start with **size-dependent slabs for medium-sized allocations**, initially one new 4096 B size class. Compare 16, 32, and 64 KiB slabs against the original software allocator. This targets a weakness already visible in our reproduction and needs DPU C changes rather than simulator architecture changes. A second, independent option is a small WRAM cache of active-page metadata.

Adapting a published policy to this PIM allocator, implementing it, and evaluating its tradeoffs can satisfy the assignment's contribution requirement. It does not establish that the underlying allocator idea is globally new. Credit the source techniques and identify our PIM-specific changes precisely.

## What the current evidence supports

- In the [local Figure 15 results](/home/mahaswint/cs5290/pim-reproduction/analysis/RESULTS.md), the 16-thread, 4096 B configuration takes 7810.92 microseconds for buddy and 6808.97 for PIM-malloc-SW using batch completion time / 128: only 1.15x improvement. These are local measurements. The paper's multithread aggregation formula remains unconfirmed; individual-request means are retained separately.
- The released frontend uses classes 16–2048 B. Larger requests reach the shared buddy allocator through [allocateUMalloc](/home/mahaswint/cs5290/PIM-malloc/simulator/uPIMulator/golang_vm/uPIMulator/benchmark/microbenchmark/dpu/task.c:1143) and [getThreadCache](/home/mahaswint/cs5290/PIM-malloc/simulator/uPIMulator/golang_vm/uPIMulator/benchmark/microbenchmark/dpu/task.c:1311).
- Each cached allocation reads a 16 B bitmap from MRAM and writes it back on success in [allocateThreadCacheNode](/home/mahaswint/cs5290/PIM-malloc/simulator/uPIMulator/golang_vm/uPIMulator/benchmark/microbenchmark/dpu/task.c:894). [allocateThreadCache](/home/mahaswint/cs5290/PIM-malloc/simulator/uPIMulator/golang_vm/uPIMulator/benchmark/microbenchmark/dpu/task.c:754) restarts its page-list search at the root for every request.
- Section VI-D and Table III of the assigned paper already evaluate **PIM-malloc-lazy**, disabling eager cache population. For its two graph representations, the reported reserved/requested memory ratios fall from 1.95 to 1.21 and from 1.72 to 1.49. The authors leave the latency–memory balance for future work. Plain lazy initialization is therefore an additional baseline, not our new contribution.

Paper source: [assigned PDF](</home/mahaswint/Downloads/Group-11_PIM-Malloc_A Fast and Scalable Dynamic Memory Allocator for Processing-In-Memory PIM Architectures (HPCA 2026) (2).pdf>); searchable extract: [paper.txt](/home/mahaswint/cs5290/pim-reproduction/reference/paper.txt:1201).

## Ranked candidates

| Rank | Proposed adaptation | Literature inspiration | Main hypothesis | Cost / difficulty |
| --- | --- | --- | --- | --- |
| 1 | Size-dependent slabs and a 4096 B thread-cache class | TCMalloc page-size/cache tuning; mimalloc page geometry | Amortize shared buddy calls over several medium objects | More MRAM reserved per refill; moderate implementation |
| 2 | Active-page pointer and bitmap retained in WRAM | mimalloc page-local fast paths | Avoid repeated MRAM bitmap transfers and full-page scans | WRAM capacity and metadata consistency; moderate implementation |
| 3 | Demand-aware cache warming and retention under a budget | ASPLOS 2024 allocator characterization; RT-Mimalloc, RTAS 2024 | Retain hot classes without reserving pages for unused classes | Adaptation overhead and cold misses; moderate to substantial implementation |
| 4 | Owner-aware, batched cross-thread frees | snmalloc, ISMM 2019 | Support producer/consumer allocations with amortized synchronization | New ownership/queue machinery; substantial implementation |

All four can be expressed in DPU C using existing memory operations and synchronization. The CPU libraries themselves are not drop-in DPU dependencies. No extra repository is required to prototype these policies.

### 1. Size-dependent slabs for medium objects

TCMalloc's [official tuning documentation](https://google.github.io/tcmalloc/tuning.html) describes the allocation-speed versus memory-retention tradeoff in page and cache sizing. The proposed PIM adaptation uses different slab sizes for different object classes, while keeping the buddy backend's minimum block at 4 KiB. This is our proposed policy experiment, not a claim that TCMalloc implements this exact PIM layout.

Prototype:

1. Keep existing classes and their 4 KiB slabs; add a 4096 B class with a configurable 16/32/64 KiB slab. Consider 8192 B objects only after the first pilot.
2. Extend each class descriptor with its slab size; route class lookup, refill, and free through that descriptor. Handle an initially empty cache explicitly, including assigning its first root.
3. Initially allocate the new class on demand. An eager variant is a control for separating cold refill cost from steady allocation cost; laziness itself is not the contribution.
4. Evaluate an active-page cursor separately if list scans dominate. Before claiming a combined improvement, include a cursor-only ablation.
5. For an extended policy, limit retained wholly empty slabs per class. Live slabs cannot be reclaimed merely to meet an unused-cache budget.

Geometry matters: the released layout uses a 24 B in-band header, so capacity is `floor((slab_bytes - 24) / object_bytes)`, capped by its 128-bit bitmap. A 4096 B object has capacity **0** in a 4 KiB slab, **3** in 16 KiB, **7** in 32 KiB, and **15** in 64 KiB. Merely appending a size class without changing the slab size fails. Larger slabs might reduce buddy locking, but sparse use can reserve much more memory. No speedup is assumed.

### 2. Keep active-page metadata in WRAM

[Mimalloc: Free List Sharding in Action](https://www.microsoft.com/en-us/research/publication/mimalloc-free-list-sharding-in-action/) (MSR-TR-2019-18, 2019) uses page-local free lists and short allocation paths. Our adaptation would retain a bounded number of active-page descriptors and bitmaps in explicitly managed WRAM; this bitmap cache is our proposed adaptation, not mimalloc's original mechanism.

Try one or two slots per tasklet, holding page address, class tag, bitmap, and dirty state. A 32 B descriptor would use 512 B for one slot across 16 tasklets, or 1 KiB for two; confirm actual compiled size, DMA alignment, and remaining WRAM. Load on a miss and write back on eviction/drain. Frees must update the correct resident bitmap or its MRAM copy consistently. Include final dirty writeback in workload completion time so deferred work is charged.

Test a single hot class, alternating classes, phase changes, and many full slabs. Compare cursor-only, bitmap-only, and combined versions. Expect any benefit mainly on cached small objects; uncached 4096 B requests provide a control. This is software metadata management, distinct from the excluded hardware buddy cache.

### 3. Demand-aware cache warming and retention

[Characterizing a Memory Allocator at Warehouse Scale](https://people.csail.mit.edu/delimitrou/papers/2024.asplos.memory.pdf) (ASPLOS 2024, DOI 10.1145/3620666.3651350) motivates usage-based allocator-cache sizing. [RT-Mimalloc](https://retis.santannapisa.it/~a.biondi/papers/RTAS24.pdf) (RTAS 2024, DOI 10.1109/RTAS61025.2024.00022) studies profile-driven pre-initialization to reduce long allocation paths. Both are earlier than PIM-malloc; they supply related ideas, not a newer PIM result.

Our possible contribution: a fixed per-DPU budget for unused cached memory, with hot-class counters and hysteresis deciding which empty slabs to retain or prepopulate. An offline profile could be a simpler first variant, but evaluate it on held-out traces rather than its training trace. Compare original eager, authors' lazy policy, fixed selected-class warming, and the adaptive policy. Include class changes and skewed thread activity. Measure initialization, first request, empirical p99/maximum, and reserved/live bytes; Figure 15's initialization-excluded allocation sweep alone cannot establish this policy's benefit. It would not establish a worst-case real-time guarantee.

### 4. Batched cross-thread frees

[snmalloc: A Message Passing Allocator](https://www.microsoft.com/en-us/research/uploads/prod/2020/04/snmalloc.pdf) (ISMM 2019, DOI 10.1145/3315573.3329980) returns remotely freed objects to their originating allocator in batches. A PIM adaptation could use slab ownership metadata and bounded per-owner queues protected by supported UPMEM synchronization. Do not assume the CPU implementation's lock-free atomic operations are available unchanged.

Evaluate producer/consumer traces and batch sizes, including draining and memory retention. The released free path searches only the calling tasklet's caches, so cross-thread ownership requires functional work first. A valid comparison needs a correct immediate synchronized return baseline; comparing against a free path that leaks is invalid. This is a useful later extension, but less suitable as the first contribution.

## Correctness gate before allocation/free experiments

Static inspection found issues requiring focused tests before adding churn workloads:

- Allocation maps chunk 0 to bitmap word 3, bit 31; [freeThreadCacheNode](/home/mahaswint/cs5290/PIM-malloc/simulator/uPIMulator/golang_vm/uPIMulator/benchmark/microbenchmark/dpu/task.c:977) instead maps it to word 0, bit 0.
- `freeUMalloc` initializes `isFreedInUMalloc` to zero and does not set it before the final conditional call to `buddyFree`. Backend deallocation therefore appears to be skipped.
- Empty-page removal needs valid head/tail handling; do not assume the existing unused removal helper supplies it safely.

These observations have not yet been exercised by new simulator correctness tests. They do not invalidate the completed allocation-only Figure 15 experiment, which never calls these free paths. Confirm and repair common correctness issues in both the maintained software baseline and proposed variant before comparing free/reuse behavior; preserve the original reproduction and record the shared patch. Bug repairs by themselves are not the proposed contribution.

## Evaluation plan for the recommended option

First pilot: buddy, original SW, and SW with the new 4096 B class; 1 and 16 tasklets; 32/256/4096 B; 128 requests per thread; slab choices 16/32/64 KiB only where relevant. Use the existing 32 MiB heap and architectural timing. Test pointer alignment, non-overlap, successful writes/reads, free/reuse, and exhaustion on small correctness cases before timing. Use a separate tag and retain original raw results.

Then expand:

- Threads: 1, 2, 4, 8, 16. Sizes: 32 B, 256 B, 1 KiB, 2 KiB, 4 KiB, optionally 8 KiB. Add mixed-size and phase-changing traces, bounded-live-set allocation/free churn, and one dynamic graph-update workload. A thread-count sweep expands evaluation; it is not an implemented contribution.
- Report individual-request mean and empirical p99, batch completion/throughput, initialization and first-allocation cost, backend refill/lock counts, MRAM metadata traffic, peak reserved bytes, and WRAM footprint. Optional passive simulator probes for lock-wait cycles must not alter scheduling.
- Define reserved/live memory at the same instant; report peak reserved bytes separately and handle zero-live phases explicitly. Use the same trace and memory budget for each allocator. Publish failures and slowdowns as well as gains.
- Isolate effects: original SW; added class with fixed slab size; slab-size choices; cursor-only if used; combined policy. Additional eager/lazy controls are needed if cache population changes. Charge setup and drains in end-to-end workload time.

The claim to test is: **Can software caching of medium allocations reduce shared buddy traffic without an unacceptable increase in reserved MRAM?** The completed microbenchmarks motivate this question; they do not answer it yet.

## Ideas not selected for the first prototype

Whole CPU allocators cannot be integrated by changing the host allocator: that would accelerate the simulator process without improving simulated DPU allocation. Designs requiring virtual-memory remapping, a dedicated allocation core, new cache hardware, or GPU-specific execution primitives would require mechanisms outside this software-only scope. Implement the selected policy in the simulated allocator and let uPIMulator account for its instructions and MRAM operations.
