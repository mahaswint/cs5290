# WRAM feasibility and the cost of larger slabs

Checked 29 October 2026. This note answers the two follow-up questions about the proposed improvements. No allocator policy or simulator configuration was changed for this audit.

## 1. Can an active-page bitmap and pointer fit in WRAM?

Real UPMEM exposes **64 KiB (65,536 bytes) of WRAM per DPU, shared by its tasklets**. It is a software-managed scratchpad used for globals, runtime state, stacks, heap allocations, and application DMA buffers. It is not 64 KiB per thread, and it is not all available to the allocator. The local SDK's [linker script](../toolchain/LoCaLUT/upmem-2023.2.0-Linux-x86_64/src/dpu-rt/src/misc/dpu.lds) explicitly sets WRAM to 64K. The assigned paper describes the same capacity in Section II-A.

The proposed cache holds **metadata only**; object payloads and slabs stay in MRAM. The released frontend bitmap is 16 B. A compile-only descriptor layout uses:

| Field | Bytes |
| --- | ---: |
| Four 32-bit bitmap words | 16 |
| MRAM page pointer | 4 |
| Class tag | 4 |
| Valid state | 4 |
| Dirty state | 4 |
| Total, with 8-byte alignment | **32** |

Putting the bitmap first gives it the 8-byte alignment needed for MRAM DMA. [wram_budget_probe.c](wram_budget_probe.c) was compiled successfully by the actual SDK 2023.2 DPU compiler; static assertions verify the pointer width, descriptor size, and alignment. Its [assembly](wram_budget_probe.S) returns the following sizes:

| Cache arrangement | Additional WRAM | Fraction of 64 KiB |
| --- | ---: | ---: |
| One active page per tasklet, 16 tasklets | 512 B | 0.78125% |
| Two active pages per tasklet, 16 tasklets | 1 KiB | 1.5625% |
| One active page per class, 8 classes and 16 tasklets | 4 KiB | 6.25% |
| One active page per tasklet, 24 tasklets | 768 B | 1.171875% |

This establishes that the proposed descriptor has a small footprint. It is **not** a whole-program fit test or an implemented optimization. One or two slots per tasklet are the sensible starting points. Caching every slab's bitmap is unbounded as allocations grow and is a different proposal.

Whole-program feasibility still requires: linked WRAM sections plus dynamically allocated metadata plus worst observed/analysed stacks plus application buffers plus the proposed cache <= 64 KiB. Bitmap coherence on free, dirty writeback, and slot lookup costs must also be correct and measured.

### A material limitation in the released simulation configuration

The authors' [ConfigLoader.WramSize](../../PIM-malloc/simulator/uPIMulator/golang_vm/uPIMulator/src/misc/config_loader.go) returns **256 KiB**, and the isolated runtime inherited it. Its linker reserves **24 stacks of 4 KiB**, even for our 16-tasklet experiment: see [InitLinkerConstants](../runtime/src/device/linker/logic/linker_script.go) and [AssignWram](../runtime/src/device/linker/logic/linker_script.go).

The preserved 16-thread SW run's [values.txt](../results/fig15/sw_t16_s32_n128/values.txt) puts `__sys_heap_pointer_reset` at address **99,784**, already beyond real 64 KiB WRAM. This is a linker reservation, not a measurement of actually touched stack bytes. It nevertheless means that the current linked image is not a valid demonstration of fitting on real hardware.

The completed results remain measurements of the released simulation configuration. They must not be described as proving deployment within the physical 64 KiB limit. A hardware-capacity validation needs a separate configuration/build with tasklet stack reservations and the full memory budget checked; keep it separate from the preserved paper-artifact reproduction. Enlarged address capacity does not by itself imply a particular latency error, and we have not measured the effect of changing that layout.

### Why a small cache might still fail to improve speed

Section IV-B reports that a more sophisticated software LRU cache for **buddy metadata** caused **29% performance degradation** in the 16-thread, 4 KiB microbenchmark despite reducing MRAM transfers. Bookkeeping instructions outweighed the savings. Our proposal concerns the frontend's active-page bitmap, not that exact backend LRU design; it therefore needs its own evaluation. Use a direct per-tasklet slot or a very small bounded lookup rather than assuming an LRU policy is worthwhile.

The same section reports that the frontend serves most requests but the backend dominates aggregate allocator time in the studied workloads. Faster frontend hits alone may have limited end-to-end benefit. Measure total workload time, including refills and final dirty writeback, not just metadata-cache hits.

## 2. What does using larger slabs mean?

The released thread cache already grows by linking additional 4 KiB MRAM pages. The proposal changes **refill granularity and supported object sizes**, not simply a fixed thread-cache capacity limit.

Current 4096 B request:

`thread -> frontend class lookup misses (largest class is 2048 B) -> shared buddy lock -> allocate one object`

Proposed 4096 B request:

`thread -> new 4096 B class -> use a free slot in an MRAM slab -> refill from buddy only when no slot remains`

For example, a 64 KiB slab holds **15** such objects with the released 24 B in-band header. One backend refill can therefore supply up to 15 allocations, followed by local bitmap operations. This is a potential amortization, not a promised speedup. Original class sizes need not change; each class can have its own slab size.

| Slab size for the new 4096 B class | Object capacity in the current layout | Reservation if all 16 threads obtain one slab |
| --- | ---: | ---: |
| 4 KiB | 0 | Invalid for this layout |
| 16 KiB | 3 | 256 KiB |
| 32 KiB | 7 | 512 KiB |
| 64 KiB | 15 | 1 MiB |

Capacity is `min(128, floor((slab_bytes - 24) / object_bytes))` for the current bitmap/layout. The objects remain in MRAM, so a 64 KiB slab does not consume the DPU's 64 KiB WRAM.

## What the authors say, and what we infer

**Explicit in the assigned paper:**

- Section IV-A uses 4 KiB pages and size classes 16–2048 B. A class can contain multiple pages. Pages are returned to the backend when all their sub-blocks have been freed.
- Section IV-A chooses a simple two-level allocator because PIM has tight on-chip storage, limited instruction memory (24 KiB as described in the paper), and limited runtime support. This explains avoiding complex CPU allocator machinery; it is not a proof that a larger slab is impossible.
- Section VI-D identifies unused per-thread, per-class preallocation as a source of memory waste and evaluates disabling it. Table III shows the latency/memory tradeoff is already relevant at 4 KiB.
- Section IV-B provides the negative result for software LRU described above.

**Not found in the assigned paper:** a slab-size sweep proving 4 KiB optimal, a hardware rule requiring 4 KiB allocator slabs, or an explicit statement that larger frontend size classes cannot work. We should not invent an author rationale or assume their choice can be improved universally.

**Consequences of the design, to evaluate ourselves:**

1. **Reservation multiplies across tasklets and classes.** Eagerly reserving one page for 16 threads and 8 classes costs 512 KiB at 4 KiB/page. Making every page 64 KiB would cost 8 MiB, or 25% of the 32 MiB experiment heap, before useful allocation. Adding just one new on-demand class avoids that global expansion.
2. **Partial occupancy prevents page return.** If one 4096 B object remains live in a 64 KiB slab, the entire slab remains owned by that thread/class. Free slots cannot directly serve another class or another thread in the current design. These are reservation and reclamation costs, not just object-rounding fragmentation.
3. **Large refills require contiguous free buddy blocks.** An aligned 64 KiB block may be unavailable even if enough separate 4 KiB blocks are free. Fallback and allocation-failure behavior matter under churn.
4. **Metadata and search overhead can grow.** The released bitmap has only 128 bits. Raising the slab size for small objects without changing the bitmap leaves slots unrepresentable; enlarging the bitmap adds storage/scan work. This limit does not block the proposed 4 KiB class, whose largest proposed slab has 15 objects. The existing page-list scans can also eat into refill savings.
5. **Cold allocation may cost more.** More reservation, initialization, and possible memory waste must be charged along with steady-state benefits. A single request per class can be worse than allocating directly from buddy.

## Updated choice of first experiments

Both ideas remain plausible but conditional. The smallest test is a single aligned active-page descriptor per tasklet (512 B total for 16 threads), measuring the full allocation path and MRAM traffic. The medium-object experiment should add one class on demand, sweep 16/32/64 KiB slabs, and compare latency against peak reserved/live bytes under dense and sparse use. A separate physical-capacity build must validate the WRAM budget before claiming either design is deployable on real UPMEM.

Primary sources: assigned HPCA 2026 PDF, Sections II-A, IV-A, IV-B, VI-D/Table III; released allocator/simulator code and local SDK linker script linked above. Relevant paper pages were rendered and inspected as `reference/constraints-page-06.png`, `constraints-page-07.png`, and `constraints-page-08.png`. The 4 KiB/2 KiB choices and reported LRU result are author statements; the proposed slot layouts and slab tradeoff calculations are our analysis.
