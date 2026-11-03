# Implementability assessment of the eight proposed research directions

Implementation update: Idea 1 has now been selected and implemented separately. See [adaptive/README.md](../adaptive/README.md) for the controller, common baseline repairs, verification, and commands. This assessment describes the preceding design review.

29 October 2026. This reviews the initial design proposal against the released allocator, local SDK, and cited primary sources. No proposed allocator was implemented or benchmarked during this review. Difficulty refers to a small, correct prototype plus a meaningful evaluation, not a publication-level implementation. The potential significance labels in the proposal are hypotheses, not established novelty or expected speedups.

## Recommendation for this project

**Best main direction: Idea 1, adaptive retention of empty thread-cache pages.** Start with on-demand population, per-class empty-page counts, a fixed retention policy, and a small adaptive policy. Compare eager, lazy, fixed retention, and adaptive retention under identical traces. This directly addresses the assigned paper's latency/capacity tradeoff while retaining its allocator interface and architecture.

**Best alternative: Idea 3, partial-page lists and direct ownership lookup.** It has a particularly concrete code target and can expose scaling with live-set size. Implement the allocation-side list change independently of the free-side map so each effect can be evaluated.

**Smallest focused pilot: Idea 6, pin a prefix of buddy metadata in WRAM.** This is smaller than the full adaptive-cache project and directly targets backend latency. Equal-WRAM-budget comparisons are essential. It is a different software policy from the paper's costly LRU, but success is unmeasured.

Treat backend sharding as an ambitious alternative rather than combining it with the first contribution. Defer cross-DPU balancing. A small implemented policy with clear evidence satisfies the assignment; there is no requirement to implement all these mechanisms.

## At-a-glance assessment

| Proposal | Implementability | Smallest useful version | Main qualification |
| --- | --- | --- | --- |
| 1. Adaptive thread caches/reclamation | Good; moderate effort | Retain a bounded number of wholly empty pages per thread/class; adapt from recent refill/reuse behavior | Correct free/reclamation paths required; can increase refill contention |
| 2. Sharded backend | Feasible; high effort | 2/4 fixed disjoint arenas, separate locks and metadata buffers, preferred arena plus bounded fallback | Does not eliminate contention or MRAM bandwidth sharing; heap becomes partitioned |
| 3. Scalable allocation/free | Good; moderate effort in stages | Non-full-page list, followed by MRAM ownership map | Constant-time lookup is not constant total allocator latency; large-block ownership and map updates need care |
| 4. Lifetime regions | Good with explicit lifetimes; moderate effort | Per-tasklet bump allocation from buddy-backed chunks, reset at a known phase boundary | Changes the application interface/lifetime contract; not suitable for all graph objects |
| 5. Cross-DPU balancing | Technically possible; very high effort | Host chooses placement of new independent work using memory capacity | A runtime/data-placement project; pointers cannot be dereferenced across DPUs |
| 6. Hierarchy-aware buddy metadata | Very good; low-to-moderate effort | Pinned upper-tree prefix plus a streaming lower-tree window | Need equal memory budgets, coherent updates, and measured instruction overhead |
| 7. Adaptive classes/slabs | Fixed choices easy; online changes hard | Select classes/slab sizes at initialization or change policy for new pages only | Existing live objects must keep their original class identity |
| 8. Long-running/irregular workloads | Good; moderate benchmark work | Deterministic bounded-live-set allocate/free trace with phases and preconditioning | Evaluation infrastructure, not by itself an implemented allocator contribution |

Ideas 1–4, 6, and 7 can use existing DPU instructions and memory operations; they do not inherently need architectural/ISA changes. Idea 5 additionally changes host code, work placement, and multi-DPU evaluation. Generalizing the current released cache implementation beyond 16 tasklets is separate work; use 1/2/4/8/16 first.

## Shared prerequisites

Preserve the original allocation-only Figure 15 reproduction. Create a separately labeled maintained baseline for new free/reuse experiments. Static inspection identifies:

- The allocation/free bitmap indexing differs: chunk 0 allocates word 3 bit 31, while `freeThreadCacheNode` frees word 0 bit 0.
- `freeUMalloc` never sets the flag guarding its backend-free call.
- The empty-page removal helper is not called by the free path. It uses assignment in its assertions and dereferences both neighbors without head/tail checks.
- `safeBuddyFree` starts at the minimum-block leaf; freeing larger backend allocations needs a dedicated correctness test. Do not assume region chunks or large objects are already reclaimed correctly after fixing frontend routing.
- `safeBuddyAllocate` asserts on allocation failure. Memory-pressure tests and cross-arena fallback need a defined nonfatal `try_allocate`/failure path.

Confirm these cases using small simulator tests, then share required correctness repairs across compared variants. Do not report a corrected allocator beating a leaking baseline as a policy benefit. See [existing audit](IMPROVEMENT_OPTIONS.md) for exact source links.

The inherited simulator permits 256 KiB WRAM and reserves 24 × 4 KiB stacks. Physical UPMEM has 64 KiB. Keep simulation-reproduction results distinct from a physical-capacity fit check; see [WRAM audit](WRAM_AND_SLAB_CONSTRAINTS.md). All candidate memory budgets must include stacks, application buffers, and metadata.

## 1. Adaptive empty-page retention

Implement page occupancy state, correct unlink/return-to-buddy handling, and an empty-page list for each thread/class. The paper describes return of wholly empty pages; the released source does not implement that complete path. Bring the common baseline to the documented behavior before attributing gains to a different retention policy.

Start with fixed keep limits 0/1/2 pages, then a lightweight policy that retains more after repeated refills and shrinks after sustained low use or pressure. Update counters on the slow path or periodically; no dedicated background thread is necessary. A few 32-bit counters per each of 16 × 8 thread/class pairs can use roughly 1–2 KiB WRAM, depending on the chosen fields. Page-list metadata can remain in MRAM. This is an estimate, not a compiled policy footprint.

Reclaim only completely empty pages. A partially occupied page cannot safely be taken away without moving live objects and updating application pointers. Owner-local maintenance is simplest. Reclaiming another inactive thread's pages needs additional locking or a quiescent-phase protocol; a global pressure flag alone does not make that safe.

Evaluate changing hot classes, skewed thread demand, pressure, and repeated allocation/free phases. Compare original eager population, lazy population, fixed retention with several budgets, and adaptation under the same budget. Include initialization, empirical p99, end-to-end time, reserved/live bytes, refill counts, and failures. A reduced-memory footprint may be worthwhile even if some latencies rise; the goal is a tradeoff curve, not guaranteed dominance everywhere.

## 2. Sharded buddy backend

Use 2 or 4 power-of-two arenas first, with explicitly separated metadata and payload ranges. Each arena needs its own lock, buffer, and buddy state. Merely creating multiple `BuddyAllocator` structures is insufficient: [buddyAllocate/buddyFree](../../PIM-malloc/simulator/uPIMulator/golang_vm/uPIMulator/benchmark/microbenchmark/dpu/task.c) reference one global `buddyAllocatorMutex`. The SDK already supports multiple mutexes and try-lock, so a lock-free implementation is unnecessary.

Route allocation to a preferred arena, and use bounded fallback that releases one arena lock before acquiring another. Track the owning arena for free. The current asserting failure path must be replaced for fallback. Do not silently grow total heap capacity by giving every arena the original 32 MiB. Do not assume every split arena remains perfectly aligned after reserving its metadata; define the layout explicitly.

Shards shorten trees as well as divide locks. Separate this effect from contention using one-thread measurements and lock-wait/refill statistics. Other tasklets can execute while one waits for MRAM, but they still share a DPU pipeline and MRAM resources: K arenas do not imply K-times throughput. Requests concentrated in one arena still contend. More arenas constrain the maximum contiguous allocation and can strand capacity; fallback cannot merge free blocks across arena boundaries.

Prototype fixed arena count and simple routing. Online splitting/merging and a shared overflow hierarchy add substantial synchronization and fragmentation work. Compare 1/2/4 arenas at equal total heap and explicitly budget per-arena buffers. Use both balanced and skewed demand, 4–32 KiB requests, and frontend refill bursts.

## 3. Partial-page allocation lists and direct free lookup

The [authors' documentation](../../PIM-malloc/hw/README.md) really does describe linked-list scans and acknowledge class lookup as future work. The code's page scan is a separate, potentially larger issue than scanning eight classes.

Stage A: maintain a list containing only pages with available slots. Allocate from its head; move a page out when full, and back when a free creates space. Keep enough state for constant-length list edits. Test the extra MRAM writes against saved scans. For the released layout, 256 B objects have 15 slots per page; 128 sequential allocations already require nine pages. Larger fixed-size traces should expose the difference without introducing other policies.

Stage B: index an ownership table by `(ptr - payload_heap_base) >> 12`, where the base is the allocator's actual payload region, not an arbitrary MRAM address. For 32 MiB and 4 KiB pages there are **8,192 entries**. Packed 2 B descriptors need 16 KiB, 4 B need 32 KiB, and simple DMA-aligned 8 B descriptors need 64 KiB. These can live in **MRAM**. The cheapest robust starting representation may be an 8 B entry with one aligned DMA lookup; optimize packing later.

The table records page kind, owner/class, and enough information to locate a larger allocation's head when applicable. Size class alone is insufficient for variable-sized spans or cross-thread operations. Packed entries sharing an 8 B DMA unit need protected read-modify-write updates to avoid lost neighboring entries. Use relative heap alignment consistently and reserve the map outside the application heap or charge its bytes explicitly.

Initially support same-thread frees. Cross-thread free requires a synchronization/ownership policy in addition to this lookup. Table lookup may remove list traversal, but refill, reclamation, mutex waiting, and MRAM access still make total latency variable. Evaluate allocation-only list changes, map-only free changes, and their combination separately at equal live-set sizes.

## 4. Explicit lifetime regions

Provide `region_init`, `region_alloc`, `region_reset`, and `region_destroy`; use per-tasklet WRAM cursor/end pointers and MRAM chunk links. Allocate another buddy-backed chunk only when necessary. Charge internal alignment, unused tail space, and chunk refills. Distinguish reset that retains chunks from destroy that returns them to buddy.

A single API call can release a region logically, but returning a region with C chunks generally needs O(C) backend operations, not one constant-time buddy free. Avoid overstating the bulk-reclamation cost. Generic `free` does not automatically provide region lifetime semantics.

Use temporary graph-update buffers or traversal scratch data with explicit phase boundaries. Persistent edges must remain alive after the batch; putting all inserted edges into a resettable region would change the workload's meaning. Keep outputs identical and charge all resets/destruction. Automatic lifetime prediction is much harder and unnecessary for the first prototype.

The cited [ISMM 2026 paper](https://arxiv.org/html/2605.17119v1) is real and supports region-allocation and preconditioned-heap evaluation as related ideas. Its CPU cache-locality findings are not a UPMEM speedup prediction; the DPU has explicitly managed WRAM/MRAM, so initially test fewer metadata operations, refills, and frees. This is a valid workload-specific extension, not a replacement for arbitrary-lifetime allocation.

## 5. Memory-aware placement across DPUs

The simulator supports multiple DPUs, but our harness currently launches one and times DPU allocation calls. A useful runtime experiment needs several independently growing partitions, capacity reporting, placement decisions, host-mediated data transfer, and end-to-end timing. Existing MRAM pointer values are local to their DPU; migration needs relative offsets or pointer relocation, plus consistent application state.

A narrow host policy can place new independent work on a DPU with sufficient free memory. Migrating a growing linked graph partition is far more involved. Transfer time must be charged; a result that ignores host movement would not substantiate a runtime improvement. This is the least suitable first contribution for this assignment.

[DAWN](https://ksp.etri.re.kr/ksp/article/read?id=72409) is an LLM/PIM/NPU workload-distribution study, not an allocator implementation or evidence of a drop-in UPMEM solution. Our scope excludes LLM serving. [Non-Clairvoyant Scheduling for Processing-in-Memory](https://pdl.cmu.edu/PDL-FTP/associated/non-clairvoyantPIM-SPAA26_abs.shtml) is verified SPAA 2026 work on scheduling/data movement, including real UPMEM evaluation. It supports the relevance of host placement, not the claim that this allocator extension is small.

## 6. Pinned upper buddy metadata

This is one of the most directly implementable proposals. Add a WRAM-resident prefix that is authoritative for upper-tree metadata; route `getMetadata` and `setMetadata` through it when the word is pinned. Keep a small streaming window for lower levels, initialize both consistently, and ensure streaming loads/writebacks cannot overwrite newer pinned values. The existing buddy mutex can continue protecting updates. Include final flush/drain where required.

Exact sizes from the released layout for a 32 MiB heap with a 4 KiB minimum backend block:

- All 14 levels total **4,108 B**, or **4,112 B** when rounding a complete allocation to 8-byte DMA alignment. This is roughly 6.3% of physical WRAM, before the rest of the program.
- First 4 levels: **16 B**, representing 15 logical nodes.
- First 6 levels: **28 B**, representing 63 logical nodes.
- First 7 levels: **44 B**, representing 127 logical nodes.

The top levels are padded to whole words; do not assume tightly packed two-bit node counts when calculating the prefix. A concrete equal-budget trial is **16 B pinned + 48 B streaming** versus the existing **64 B streaming** buffer. Use aligned boundaries and sufficient buffering fields. Separately sweep larger budgets and include the original streaming policy at each same budget. A full 4,112 B resident tree is useful as an additional software reference, not a fair same-memory comparison with 64 B.

Test 4 KiB/mixed backend-heavy allocations at 1 and 16 tasklets, allocation/free churn after correctness checks, metadata DMA bytes, instruction counts, lock waiting, and total latency. Low frontend-only benefit is an informative control. Claim improvement over software baseline first; approaching excluded hardware-cache performance is an unverified research ambition and does not require us to add hardware experiments.

## 7. Adaptive classes and slab geometry

The API accepts a size-class array, so fixed alternative classes are straightforward. Enforce ascending classes, nonzero slot capacity, and 8 B MRAM alignment. A 96 B class fits alignment requirements, but arbitrary new classes may replace a cheap shift/divide with software arithmetic on the DPU. Measure instructions as well as bytes saved.

Online class changes cannot reinterpret existing pages: each page must retain the size/geometry under which live objects were allocated. New policy epochs can apply to new pages; the old ones drain under their old class. This needs direct ownership metadata or versioned descriptors and more routing logic. Do not start by moving live objects.

For a smaller experiment, select a few static class/slab configurations from a training trace and evaluate held-out traces. Compare with an equally informed static policy before claiming online adaptation helps. This fits naturally into Idea 1, but combining both controllers immediately makes cause and effect harder to identify.

## 8. Controlled long-running workloads

Build a deterministic operation generator with per-thread pointer pools, fixed seeds, mixed size distributions, bounded live sets, and allocation/free phases. Store large operation/pointer arrays in MRAM or generate operations in constant space; otherwise the benchmark itself can exhaust WRAM. Instrument free calls and end-to-end completion, preserving separate allocation/request means and batch makespan.

Vary occupancy, page count, class changes, and free order separately. A fragmented initial heap is a useful stress test, not automatically a representative application trace. The ISMM 2026 paper provides relevant preconditioning methodology. For direct-list scaling, compare the same live set and operation trace across allocators. For adaptation, reserve held-out traces and avoid tuning all thresholds on the reported tests. Include one dynamic graph workload with identical outputs.

The pasted 1–24-thread plan is not immediately supported by the released frontend or our runner, which currently handle at most 16 caches/tasklets. Start at 1/2/4/8/16; generalize and validate the cache representation before adding 24. Do not infer a degradation from running unsupported thread counts.

## Source verification

- [PIM-malloc source/documentation](https://github.com/VIA-Research/PIM-malloc), local commit `4fdcbcac3b0d94f2d4d51d8d84c3a3a640ad14a4`: class/page scans, shared lock, future-work notes and metadata layout checked directly. Paper claims are anchored in the assigned PDF and earlier notes, rather than treating the pasted proposal as evidence.
- [TCMalloc design](https://google.github.io/tcmalloc/design.html): pagemap ownership lookup and runtime cache sizing confirmed. Our MRAM table is a proposed adaptation, not the CPU library ported unchanged.
- [Reconsidering "Reconsidering Custom Memory Allocation"](https://conf.researchr.org/details/ismm-2026/ismm-2026-papers/10/Reconsidering-Reconsidering-Custom-Memory-Allocation-), Nicolas van Kempen and Emery D. Berger, ISMM 2026, DOI 10.1145/3814942.3816132; [author preprint](https://arxiv.org/html/2605.17119v1) read. Regions and preconditioning are applicable ideas; CPU result magnitudes are not portable predictions.
- [Accelerating Transactional Execution via Processing-In-Memory](https://www.dpss.inesc-id.pt/~dcastro/publications.html), André Lopes, Daniel Castro and Paolo Romano, EuroSys 2026, DOI 10.1145/3767295.3803621: publication and transaction-coordination scope verified through author/institution sources. Direct PDF retrieval repeatedly timed out, so the pasted precise CAS attribution was not independently confirmed from its full text. The local SDK independently confirms mutex/try-lock APIs backed by acquire/release instructions; practical lock sharding does not depend on that citation.
- [DAWN institutional record](https://ksp.etri.re.kr/ksp/article/read?id=72409), IEEE Computer Architecture Letters 2026, DOI 10.1109/LCA.2026.3665202: verified. Simulation-based LLM workload distribution; not our allocator workload or a UPMEM allocator speedup.
- [SPAA 2026 author/institution abstract](https://pdl.cmu.edu/PDL-FTP/associated/non-clairvoyantPIM-SPAA26_abs.shtml): scheduling/host movement and real UPMEM evaluation verified. Useful background for a larger runtime project.
