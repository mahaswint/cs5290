# Paper and artifact mapping

These experiments are **single-DPU workload ports**, not executions of the original CPU/GPU applications or reproductions of their published speedups. The allocator implementation and thresholds are frozen from `../combined/`. Every allocator variant receives the same generated operations within a comparison group.

## uPIMulator and PrIM

[uPIMulator](https://github.com/VIA-Research/uPIMulator), associated with *Pathfinding Future PIM Architectures by Demystifying a Commercial PIM Technology* (HPCA 2024), supports [PrIM](https://github.com/CMU-SAFARI/prim-benchmarks). The local upstream commits are `870d916334e9ff0b190f555f951a9ec3c4257781` and `67179099730cdccc650d52d25f458cb2e4f8a675`, respectively. The actual simulator executable used here is the maintained PIM-malloc-based runtime, whose SHA-256 is saved in every result.

`prim-sel` ports [SEL/dpu/task.c](https://github.com/CMU-SAFARI/prim-benchmarks/blob/67179099730cdccc650d52d25f458cb2e4f8a675/SEL/dpu/task.c): 256 B blocks, `uint64_t` values, retaining odd values, strided tasklets, and stable global compaction into a preallocated MRAM output. The port uses barriers and a prefix calculation instead of the original handshake wrapper. Input is deterministic and the complete output is checked. It makes **zero allocator calls during work**, so it is a negative control for allocator speedup. It is not the complete PrIM host benchmark.

## PIM-malloc dynamic graph use case

The assigned HPCA 2026 PDF, recorded in `../source_manifest.json` (SHA-256 `ba1a8f6143102ca3fba63651a67b2dbf457cecc427a11e52cc2e6c1b3f2f0212`), is authoritative for the graph methodology. See `../reference/paper.txt`, the paragraph beginning “Benchmark for PIM-malloc use case.” [Public paper](https://arxiv.org/abs/2505.13002) and [artifact](https://github.com/VIA-Research/PIM-malloc); artifact commit `4fdcbcac3b0d94f2d4d51d8d84c3a3a640ad14a4`.

`pim-list` uses 256 B adjacency blocks. `pim-array` uses one power-of-two array per vertex, beginning at 64 B and copying existing neighbors on growth. Both use the paper's 1:2 new/existing-edge split and Gowalla dataset. We reservoir-sample 3,072 undirected edges from all 950,327 unique edges, compact vertex IDs, preload 2,048 edges and time insertion of 1,024. Both directed arcs are maintained. The reduced sample does not reproduce the full graph's reported 64 B–32 KiB allocation range; actual degree and request distributions are retained. No original graph executable was supplied, so these kernels implement the documented data structures and methodology.

## Larson / mimalloc

[Larson and Krishnan, *Memory Allocation for Long-Running Server Applications*](https://doi.org/10.1145/286860.286880) (ISMM 1998) supplies a persistent object-pool replacement benchmark, also evaluated in [*Mimalloc: Free List Sharding in Action*](https://www.microsoft.com/en-us/research/publication/mimalloc-free-list-sharding-in-action/) (2019).

The reference implementation is [mimalloc-bench's Larson source](https://github.com/daanx/mimalloc-bench/blob/c65a68834c8fbd619f3ef449f72b9ec659cb551e/bench/larson/larson.cpp), pinned at commit `c65a68834c8fbd619f3ef449f72b9ec659cb551e` (source blob `676223af31bc2a4b8db00c74b0c3eac4da976cc2`). The [driver](https://github.com/daanx/mimalloc-bench/blob/c65a68834c8fbd619f3ef449f72b9ec659cb551e/bench.sh) invokes `larson 5 8 1000 5000 100 4141 $procs`.

`larson` preserves uniform requested sizes 8–999 B, initial pool allocation/permutation, 4×pool random replacements before measurement, the `lran2` generator, worker seeding, and random victim replacement. Its pool is scaled to 512 objects per tasklet and timed work is 2,048 replacements per tasklet. Payload tags support integrity checks. The Python generator matched 128 outputs from the upstream C RNG. This is the **owner-local worker subset**: independent tasklet streams, fixed operations, no thread respawn or inter-thread object migration. Those changes preclude claims about Larson's original concurrent scalability.

## faimGraph

[Winter et al., *faimGraph*](https://hpdgp.mpi-inf.mpg.de/pappe/faimGraph.pdf) (SC 2018), [artifact](https://github.com/GPUPeople/faimGraph), motivates uniform dynamic edge updates and compaction that releases unused pages.

`faimgraph` is an adaptation of that update pattern using our 256 B MRAM adjacency blocks. It preloads all 3,072 sampled Gowalla edges and performs two rounds, each deleting 1,024 uniformly sampled **existing** undirected edges and reinserting the same edges in shuffled order. Arbitrary-edge deletion moves the tail neighbor into the hole and frees an empty tail block. It preserves an exact reference graph after each complete batch. It does not execute the GPU artifact, duplicate its complete sampling scheme, or include vertex updates, PageRank, triangle counting, or its page-queue implementation.

## Common restrictions

Only the owning tasklet mutates a vertex or frees its allocations. Graphs use source-vertex modulo tasklet ownership. Main comparisons use 1 and 4 tasklets and a 32 MiB payload heap. This is weak scaling for Larson (512 objects per tasklet) and strong scaling for the fixed graph and SEL inputs; tasklet-count rows are not interchangeable throughput claims. Second-seed tests use the same sizes and thresholds. All sampling inputs, expected values, seeds, traces and exact generated C are retained with each run.
