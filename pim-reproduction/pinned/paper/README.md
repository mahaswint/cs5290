# PIM-malloc paper workloads on all seven allocator stages

This addition preserves the parent experiment and its source/results. It tests the same seven stages on request shapes and graph structures from **PIM-malloc: A Fast and Scalable Dynamic Memory Allocator for Processing-In-Memory (PIM) Architectures**, HPCA 2026. The reference is the locally archived paper in [`../../reference/paper.txt`](../../reference/paper.txt), Sections V and VI, and its [public version](https://arxiv.org/abs/2505.13002). The archived PDF hash is `ba1a8f6143102ca3fba63651a67b2dbf457cecc427a11e52cc2e6c1b3f2f0212`.

## What is reproduced

| Case | Paper method retained | Scope of this evaluation |
| --- | --- | --- |
| Gowalla linked blocks | Random edge sampling, 1:2 new/existing split, 256 B adjacency blocks | Existing single-DPU port; 3,072 undirected edges, 2,048 preloaded and 1,024 timed inserts, storing both directed arcs; four tasklets |
| Gowalla growing arrays | Same graph and update order; one power-of-two array per vertex, copying on growth | Same reduced sample; allocations start at 64 B, with a smaller maximum than the full paper's 32 KiB |
| Allocation microbenchmark | 128 allocation requests per tasklet, fixed 32 B / 256 B / 4 KiB requests, 1 and 16 tasklets | Six cases, without object reclamation during work; all returned pointers are recorded and objects are checked and freed afterward |
| KV-growth replay | 512 B blocks when KV space fills; 128 prompt and 256 output tokens | **Allocator-only synthetic replay**, one stream per tasklet, four tasklets; one 512 B block per token/stream, writing deterministic payloads |

The two graph ports are the paper's insertion-only use cases, rather than the previously measured deletion/reinsertion graph churn. They share the original trace generation, exact adjacency oracle and implementation in [`../../paper-workloads/`](../../paper-workloads/). Neither Larson nor CloudPhysics nor PrIM SEL is presented as a workload from the PIM-malloc paper.

The paper's attention evaluation extends PrIM GEMV and uses a uPIMulator-generated trace in LLMServingSim (100 requests at 10 requests/s, 128 prompt tokens and 256 output tokens). The pinned released artifact at commit `4fdcbcac3b0d94f2d4d51d8d84c3a3a640ad14a4` supplies allocator/microbenchmark implementations, but no graph or attention kernel or serving trace was found in the local checkout. The replay here exercises the described **allocation size and growth mechanism only**. It does not run GEMV, softmax, model weights, the arrival schedule or LLMServingSim, and cannot report tokens/s, TPOT or an end-to-end attention speedup. One block per token per stream is an explicit replay assumption, not a recovered original trace.

The released software microbenchmark source is [`task.c`](../../../PIM-malloc/simulator/uPIMulator/golang_vm/uPIMulator/benchmark/microbenchmark/dpu/task.c); its `main` issues 128 allocation requests per tasklet. Section VI-A supplies the 32 B / 256 B / 4 KiB and 1/16-tasklet settings. This extension follows those request settings while using the common corrected allocator implementations. [`source_manifest.json`](source_manifest.json) records the artifact commit, original microbenchmark hash and released software benchmark inventory.

## Controlled allocator comparison

Each of the nine cases runs all seven variants without adjusting its requests to the allocator:

1. Maintained scan fixed-2 baseline.
2. Indexed adaptive, interval 64.
3. Indexed fixed-2.
4. Indexed adaptive-64 + 64 KiB slabs.
5. Indexed fixed-2 + 64 KiB slabs.
6. Indexed adaptive-64 + slabs + pinned upper buddy metadata.
7. Indexed fixed-2 + slabs + pinned upper buddy metadata.

The source generator imports the unchanged parent stack. All runs have a 32 MiB payload heap and 256 KiB shared empty-byte budget per tasklet. Indexed variants use an additional 64 KiB MRAM ownership map. Pinning keeps the aligned 528 B upper metadata prefix in shared WRAM, with the same lower streamed buffer and consistency design as the parent. Adaptive interval 64 counts allocation **plus free** operations. Fixed-2 limits retained **empty** pages/spans, not live pages or the number of allocations.

The current slab extension serves only requests in 2049–4096 B. The paper's 4 KiB microbenchmark therefore directly exercises it. The 32 B / 256 B microbenchmarks, 512 B KV replay and reduced graph sample test its limits. The original object sizes are kept even when the slab class is unused. Allocation-only microbenchmarks do not measure the benefits of retaining empty pages during the work phase; policy differences there mostly reflect bookkeeping.

The original eight frontend classes stop at 2 KiB, so every 4 KiB request originally bypasses the frontend and calls buddy. This slab extension adds a ninth, 4 KiB class backed by 64 KiB spans. Its gains on the 4 KiB microbenchmark show the value of caching that previously uncached request size; they do not establish that increasing page sizes for the existing small classes would produce the same gains.

The scan fixed-2 baseline is **not** the paper's straw-man buddy allocator or static CSR baseline. Our WRAM pinning is a software change, not the paper's ISA-based hardware buddy cache. The paper's published speedups cannot be equated with speedups in this matrix.

## Timing, checks and artifacts

Work is concurrent DPU makespan. For graphs it includes only the new-edge insertion batch; for microbenchmarks it includes allocation, pointer recording and the end barrier; for the KV replay it includes output-token allocation and payload writes. Per-allocation probes isolate allocator latency, including lock wait, from pointer recording/payload DMA. Arithmetic mean allocation latency is distinct from makespan divided by the total concurrent requests.

Total includes initialization, graph/KV preload, work, output checks and final free/drain, ending before passive final accounting. DRAM commands/bytes cover the full simulated execution, including final accounting. They include application and allocator traffic, and do not count WRAM accesses or host transfers. No latency or throughput includes simulator wall time.

All objects remain live until checks finish. New request kernels check complete deterministic payloads; microbenchmark payload writes occur after work timing. Native ASan/UBSan tests independently track live intervals and check exact graph neighbors. Every accepted DPU run checks payloads or graph degrees/checksums, successful counts, final owner/list invariants, no live allocations and complete buddy root coalescing. The existing backend differential and layered allocator tests remain unchanged.

The sweep declares 63 configurations before launching them: two graphs + six microbenchmark cases + one KV replay, each with all seven variants. All are reported, including regressions. The analyzer regenerates sources/traces, checks source/simulator/dataset/native evidence hashes, verifies matching offered work, independently calculates request sizes/counts, and rejects missing or extra results.

```sh
python3 pim-reproduction/pinned/paper/test.py
python3 pim-reproduction/pinned/paper/sweep.py --tag paper-v1 --jobs 4
python3 pim-reproduction/pinned/paper/analyze.py --tag paper-v1
pim-reproduction/.venv/bin/python pim-reproduction/pinned/paper/plot.py
```

[`RESULTS.md`](RESULTS.md) contains all seven runtimes, speedups, mean allocation latencies and memory traffic. `analysis/` contains the raw comparison CSVs, request profiles and audit. `results/paper-v1/` stores each generated source, full trace, compilation/link logs, PC timing events, simulator counters and summary. Separate `gate-paper-v1` smoke runs are excluded from the declared matrix.
