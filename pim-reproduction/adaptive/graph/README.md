# Small-object dynamic graph workload

This is an executable UPMEM graph kernel using the allocator policies in `../allocator.c`.
It ports the paged adjacency-list workload described by **PIM-malloc, Sections V and VI-B**,
with repeated insert/delete batches motivated by **faimGraph (SC 2018), reference [134] in PIM-malloc**.
It is a reduced workload implementation and extension, not the original authors' graph kernel,
the CUDA faimGraph executable, or an exact reproduction of Figure 17.

## Sources

- PIM-malloc: the supplied paper, `../../reference/paper.txt`, methodology and Figure 17.
  Its linked-list graph objects are 256 B; it samples Gowalla edges into a 1:2 new/existing split.
- [SNAP Gowalla](https://snap.stanford.edu/data/loc-gowalla.html): 196,591 vertices and 950,327 undirected friendship edges.
- [faimGraph paper](https://hpdgp.mpi-inf.mpg.de/pappe/faimGraph.pdf) and
  [authors' repository](https://github.com/GPUPeople/faimGraph): dynamic paged adjacency lists,
  repeated edge insertion/deletion rounds and configurable update batch/page sizes.
  This port uses its workload pattern rather than its GPU implementation or memory manager.
- The already cloned PrIM COO input is `../../../prim-benchmarks/BFS/data/loc-gowalla_edges.txt`.
  No further dataset download or CUDA installation is needed.

## Exact workload

The default samples **384 undirected edges uniformly from the entire 950,327-edge input**
using reservoir sampling, seed 5290. It does not take the first source-sorted rows.
It splits these into 256 existing and 128 new edges, then stores both directions of every edge.
Source vertex IDs are compactly remapped; destination IDs remain original Gowalla IDs.
The edge sample is small and does not preserve the full graph's connectivity/degree distribution.
Larger samples can be selected with `--graph-edges`, within the current WRAM input-storage limit.

Each source vertex belongs to `compact_source_id % tasklets`, so its graph blocks are allocated
and freed by the same tasklet. The actual adjacency data is in MRAM. A graph block contains an
8 B previous-block/count header and 4 B destination IDs. A vertex stores its tail pointer and degree.
Blocks are allocated only when a vertex needs additional adjacency capacity.
Deletion removes the same batch in reverse order, releasing an empty tail block immediately.
This is an explicit LIFO deletion pattern, not arbitrary edge deletion or graph compaction.

After preloading the existing graph, each cycle executes:

| Phase | New undirected edges | New adjacency arcs | Extra adjacency traversal passes |
| --- | ---: | ---: | ---: |
| low | 8 | 16 | 4 |
| high | 128 | 256 | 1 |
| low-again | 16 | 32 | 4 |
| rebound | 64 | 128 | 1 |

Every phase inserts its selected new edges, reaches a snapshot barrier, traverses and validates
the graph, removes the inserted edges, and verifies restoration of the existing graph.
The default repeats this schedule twice. Fractions scale with `--graph-edges`.
These are **synthetic closed-loop phases**, not timestamps measured from Gowalla.
The friendship edge file has no edge-update timestamps. Batch variation is our added test condition.

Both 256 B objects (paper setting) and 64 B objects (additional variant) are evaluated.
Both use the same **4 KiB allocator pages**. These are two distinct sizes.
Graph traversal generates ordinary MRAM accesses; only allocation calls count as frontend-cache requests.
One edge update does not necessarily allocate: existing adjacency capacity can absorb it.

## Run and inspect

```bash
python3 /home/mahaswint/cs5290/pim-reproduction/adaptive/graph/run.py --jobs 2 --tag gowalla-v1
python3 /home/mahaswint/cs5290/pim-reproduction/adaptive/graph/analyze.py --tag gowalla-v1
```

This compares eager, lazy, fixed-1, fixed-2, and adaptive at 1/4 tasklets and 64/256 B graph objects:
**20 configurations**, all using the same seed/edges and a 32 MiB heap.
Fixed/adaptive retain the same four-empty-page budget per tasklet.
This is a cache-locality/load-variation experiment; it is not designed to force memory pressure.

For one run, with a different tag:

```bash
python3 /home/mahaswint/cs5290/pim-reproduction/adaptive/experiment.py run \
  --workload graph --policy adaptive --threads 4 --rounds 2 \
  --graph-edges 384 --graph-seed 5290 --graph-block-bytes 256 \
  --heap-kib 32768 --tag gowalla-individual-v1
```

`--burst` is unused by the graph kernel; `--graph-edges` and the schedule define its demand.
Use a new tag when changing the sample, heap, or controller parameters. The graph analyzer verifies
identical trace hashes within each tasklet-count comparison; do not combine different graph seeds
or sample sizes into one analyzed tag.

Raw results are in `../results/<tag>/<configuration>/`. Each contains the complete generated C,
input header and manifest with original edges/IDs and checksums, compiler/linker commands, binaries,
per-request cycle events, phase measurements, memory snapshots, allocator statistics, and a summary.
Analyzed tables and PNG/SVG figures are in `../analysis/<tag>/`.

## Measurement and validation

- `graph.update_alloc` and `graph.update_free` in `summary.json` include only requests within
  each graph-update phase. Graph preload and final teardown are excluded.
- Update time includes insertion, deletion, MRAM payload access, graph validation traversals,
  barriers, and snapshots. It is not allocator-only latency.
  It uses the earliest update start and latest finish across tasklets/stages. Individual phase
  windows can overlap at barrier release; `graph.update_phase_makespan_us` is their diagnostic
  duration sum, while the analyzer uses the overall `work_makespan_us` interval.
- Existing allocator counters, including frontend hits and refills, include preload and teardown.
  The analyzer labels this scope explicitly. A frontend hit means no buddy page refill; it does
  not imply that software metadata is stored in WRAM or that an MRAM access hit a hardware cache.
- Snapshots give live requested graph-object bytes, reserved MRAM, and retained empty pages.
  Peak reserved bytes are measured under the backend lock. WRAM addresses must fit below 65,536.
- DPU checks compare edge counts and checksums against the independent Python-generated reference
  after every complete batch and validate list structure. Allocation failures are recorded;
  successful insertions are tracked so partial batches can still be rolled back safely.
- `python3 .../graph/test.py` runs ten native AddressSanitizer/UndefinedBehaviorSanitizer cases
  against exact ordered adjacency reference lists, including chains longer than one graph block,
  changing batches, rollback, and complete buddy coalescing. Native tasklets are interleaved;
  the DPU simulator separately checks actual concurrent execution.
- Passive labels add no simulated instructions. Tasklet-0-only timing endpoints are kept inside
  non-inlined functions so compiler branch joins cannot make them execute for other tasklets.
- The original Figure 15 implementation/results and the allocator contribution code are unchanged.
  Non-graph implementation hashes remain unchanged; graph sources/input have a separate hash.
- This single-DPU, small edge-sample experiment does not establish full-dataset throughput,
  real-time arrivals, general deletion performance, hardware/IRAM deployability, or statistical
  variability over multiple graph samples. Those require separate evaluations.
