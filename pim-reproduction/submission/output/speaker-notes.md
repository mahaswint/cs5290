# Speaker notes and timing

Main presentation: slides 1-20, exactly 16 minutes. Slides 21 onward are reference material.

## 1. Improving dynamic memory allocation in PIM-malloc

Main presentation: 0:00-0:20; 20 seconds.

Our baseline is the PIM-malloc paper by Lee and colleagues. We first explain the DPU and the original allocator. Then we describe our software changes, the experiment setup and the results. The main question is which part of allocation each change can improve.

Sources: reference/paper.txt

## 2. What is a DPU?

Main presentation: 0:20-1:05; 45 seconds.

DPU stands for DRAM Processing Unit. In the UPMEM system used by the paper, a DPU is a small processor next to a local DRAM bank. It runs at 350 MHz and supports up to 24 tasklets, which are hardware threads. Those tasklets share one DPU’s memory. They cannot directly access another DPU’s memory. Each DPU therefore needs its own local heap.

Sources: reference/paper.txt

## 3. Memory inside a DPU

Main presentation: 1:05-1:55; 50 seconds.

MRAM holds the large application data. WRAM is the small, fast scratchpad. IRAM holds instructions. Tasklets use normal loads and stores for WRAM and move MRAM data through DMA. An allocator must decide where its metadata goes. Keeping everything in WRAM uses too much of a limited resource, while keeping it in MRAM adds transfers to allocation and free.

Sources: reference/paper.txt

## 4. The PIM-malloc baseline

Main presentation: 1:55-2:50; 55 seconds.

The paper studies dynamic memory allocation on PIM devices. Its software design has two parts. A tasklet-local frontend handles small objects using pages divided into slots. A shared buddy backend manages the MRAM heap and provides pages or larger blocks. The paper also proposes a hardware metadata cache. Our project works on the software allocator and its metadata placement.

Sources: https://arxiv.org/abs/2505.13002; reference/paper.txt

## 5. Costs in the allocation path

Main presentation: 2:50-3:20; 30 seconds.

A frontend hit avoids a backend call, but finding a suitable page can still require a scan. Free also needs to identify the owner and page. Empty pages create a tradeoff: return them immediately and a later request may refill, or retain them and use more memory. Requests above two KiB still call buddy for each object. Buddy traversals access metadata in MRAM under a shared lock. These are the costs our changes target.

Sources: README.md; research/IMPROVEMENT_OPTIONS.md

## 6. Fixed and adaptive empty-page retention

Main presentation: 3:20-4:15; 55 seconds.

Fixed-two keeps at most two completely empty pages per class. It does not cap live or partially filled pages. The adaptive policy raises a target after repeated refills and reduces idle targets during periodic checks. A byte budget covers all classes of one tasklet. A tasklet reclaims its own pages under pressure. We also compare checks every 64 and 128 allocations plus frees.

Sources: adaptive/README.md; pinned/README.md

## 7. Indexed allocation and free

Main presentation: 4:15-5:10; 55 seconds.

We keep partial, full and empty page lists so allocation can choose a page without scanning full pages. An ownership map lets free find the tasklet, class and span head from the pointer’s heap leaf. For a 32 MiB payload heap this adds 64 KiB MRAM. List state uses 24 bytes per tasklet and class in WRAM. Frees still belong to the allocating tasklet.

Sources: combined/README.md; paper-workloads/README.md

## 8. A new slab class for medium-sized objects

Main presentation: 5:10-6:05; 55 seconds.

The original frontend ends at two KiB. We add a class for requests from 2049 through 4096 bytes. It uses a sixteen, thirty-two or sixty-four-KiB MRAM span. The header leaves three, seven or fifteen four-KiB slots. Smaller classes keep their existing pages, and larger requests still bypass. Retention counts actual span bytes, so a sixty-four-KiB empty span consumes sixteen units of the byte budget.

Sources: slabs/README.md; stacked/README.md

## 9. Pinned buddy metadata

Main presentation: 6:05-7:00; 55 seconds.

The upper tree levels are visited repeatedly. We keep levels zero through ten in one shared WRAM prefix, while lower metadata continues through the original stream buffer. The aligned prefix takes 528 bytes. It holds the current upper-tree state, and lower DMA windows cannot overlap it. The buddy lock protects updates. A checkpoint can synchronize the MRAM shadow while the allocator is locked or idle.

Sources: pinned/README.md

## 10. Experimental setup and variants

Main presentation: 7:00-7:40; 40 seconds.

We preserve the released layout for the paper reproduction. All contribution variants share the same free/reuse corrections and the sixty-four-KiB WRAM address cap. The final comparison has a thirty-two-MiB payload heap, a two-hundred-and-fifty-six-KiB empty-byte budget per tasklet and checks every sixty-four allocation-plus-free operations for adaptive variants. This slide defines all seven stages before we show their results.

Sources: pinned/README.md; pinned/paper/README.md

## 11. Workloads

Main presentation: 7:40-8:35; 55 seconds.

The final comparison covers fourteen settings. We keep the paper allocation sizes and tasklet counts and use reduced versions of both Gowalla insertion structures. Larson uses explicit size ranges, and the cache preserves sizes and order from a real CloudPhysics sample. Churn and preallocated selection test different behavior. The KV replay exercises allocation only and does not execute attention or a serving system.

Sources: pinned/paper/README.md; paper-workloads/SOURCES.md; heavy-workloads/README.md

## 12. Timing and correctness checks

Main presentation: 8:35-9:30; 55 seconds.

Work is the concurrent application phase. Total also includes initialization, preload or warmup, output checking and drain. A speedup divides the matching baseline phase by the variant phase. DRAM counters cover the complete execution and exclude WRAM and instruction fetch. Native and DPU tests check payloads, ownership, completed requests and full heap coalescing. The simulator is deterministic, so identical repeats are not independent statistical samples.

Sources: pinned/README.md; pinned/paper/README.md

## 13. Evaluation: paper baseline reproduction

Main presentation: 9:30-10:15; 45 seconds.

We first reproduce the original allocation microbenchmark. Each tasklet makes 128 requests. This table shows PIM-malloc software latency using batch completion time divided by 128. The separate baseline package also gives buddy latency and mean individual request latency. Our maximum absolute deviation from a printed paper label is seven point two percent. This uses the released memory layout, separately from the corrected contribution controls.

Sources: analysis/figure15_comparison.csv; analysis/RESULTS.md

## 14. Evaluation: retention and indexing

Main presentation: 10:15-11:05; 50 seconds.

Fixed retention is faster on regular phases: sixteen point two five ms versus eighteen point five two for adaptive at the same peak. Under changing class demand, adaptive takes 101 ms instead of 114 and makes fewer refills. The ablation below holds fixed-two constant and separates page lists from ownership lookup. Using both gives the lowest work time in these cases. This shows that page discovery and retention are different costs.

Sources: adaptive/analysis/comparison-v1/RESULTS.md; combined/SUMMARY.md; paper-workloads/RESULTS.md

## 15. Evaluation: check frequency and slab size

Main presentation: 11:05-12:00; 55 seconds.

Moving maintenance from sixty-four to 128 operations gives only small gains, usually between point three and point eight percent, with a regression for linked insertion at four tasklets. Traffic mostly stays the same. Slabs make a much larger difference when requests actually use the new class. The dense burst below uses the same byte budget for every span size. Two sparse survivors can keep 128 KiB reserved for only eight KiB of live payload.

Sources: paper-workloads/INTERVAL128.md; stacked/RESULTS.md

## 16. Evaluation: the complete allocator

Main presentation: 12:00-13:05; 65 seconds.

This chart compares the fixed stages on five four-tasklet cases. Larson eight to 4095 bytes gains most from the new class. Wider Larson still has many requests above the slab limit. CloudPhysics gains less because payload processing remains. Graph churn gets most of its benefit from indexing. Selection remains flat. The backup table gives all seven variants, including adaptive. Full adaptive is slower than full fixed on stable Larson demand.

Sources: pinned/RESULTS.md

## 17. Evaluation: workloads from the paper

Main presentation: 13:05-14:10; 65 seconds.

The four-KiB allocation case gains most because it originally calls buddy for every object. At sixteen tasklets, work falls from 508 ms to twenty-two ms. The backend serves 144 span refills instead of 2048 object allocations. Reduced linked blocks gain 1.27-fold and growing arrays 1.05-fold. They never use the new class. The one-tasklet thirty-two-byte case becomes slower, so the complete allocator is not a good choice for every small workload.

Sources: pinned/paper/RESULTS.md

## 18. Evaluation: pinning, total time and memory

Main presentation: 14:10-15:00; 50 seconds.

Pinning alone adds about 22 percent work speedup to fixed Larson and about one percent to CloudPhysics. It uses a measured 540 extra bytes of WRAM. Churn work barely changes, although total improves because setup and drain also use buddy. Work and total speedups can differ greatly. For the sixteen-tasklet four-KiB case, work improves almost twenty-three-fold but total improves six point seven five-fold. Payload checking stays in the total. The slab also raises payload reservation from eight to nine MiB, plus the ownership map.

Sources: pinned/RESULTS.md; pinned/paper/RESULTS.md

## 19. Limitations and next steps

Main presentation: 15:00-15:40; 40 seconds.

All frees and reclamation remain local to the owner. An inactive tasklet can therefore keep memory that another tasklet needs. Larger contiguous spans can fail under fragmentation and can retain too much memory when few objects survive. These are deterministic single-DPU simulations of reduced kernels. The next steps are hardware image checks, broader captured traces, coordinated reclamation and a comparison of metadata schemes with equal WRAM budgets.

Sources: pinned/README.md; pinned/paper/README.md

## 20. Conclusion

Main presentation: 15:40-16:00; 20 seconds.

Indexing helps page discovery. The new slab class helps medium-sized requests with reuse. Pinning helps the backend calls that remain. Fixed retention is strong under steady demand, while adaptive retention is more useful when demand changes or memory becomes tight. The choice depends on the request pattern and the available memory.

Sources: pinned/RESULTS.md; pinned/paper/RESULTS.md

## 21. All seven variants: work time

Backup slide: use only if asked.

Reference work times in milliseconds for the five four-tasklet cases. B is scan fixed-two. IA and IF add indexing. SA and SF add slabs. PA and PF add pinned metadata. Adaptive uses interval sixty-four.

Sources: pinned/RESULTS.md

## 22. Paper graph ports and KV allocation replay

Backup slide: use only if asked.

The graph ports share the sampled Gowalla input. The KV case includes allocation and deterministic payload writes. It excludes the complete attention and serving workload.

Sources: pinned/paper/RESULTS.md

## 23. Paper allocation microbenchmarks

Backup slide: use only if asked.

All original request-size and tasklet settings with maintained scan controls. Each tasklet makes 128 requests, then checks and frees its objects after the work phase.

Sources: pinned/paper/RESULTS.md

## 24. Source code and report

Backup slide: use only if asked.

The repository includes each allocator stage, workload sources, input provenance and result tables. The submission ZIP contains the report PDF, complete LaTeX project, editable presentation, baseline and contribution packages and an offline source snapshot.

Sources: https://github.com/mahaswint/cs5290/tree/main/pim-reproduction

