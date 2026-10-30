# Gowalla small-object dynamic graph updates

A reduced UPMEM adjacency-list implementation using real PrIM/SNAP Gowalla friendship edges. The changing update batches are synthetic, not recorded social-network arrival timestamps. This is a workload port/extension, not an exact Figure 17 reproduction or execution of CUDA faimGraph.

Each peak batch uses the sampled 1:2 new/existing edge split. Both directions of each undirected edge are stored. Each batch inserts edges, traverses/validates adjacency lists, deletes inserted edges in reverse order, and checks restoration. Low phases perform four adjacency read passes; high/rebound one. Batch sizes follow low/high/low-again/rebound for each cycle.

All policies use the same input edges, seed, ownership, payload work, and allocator code. A 64 B graph block is an additional object-size variant; the paper uses 256 B. Allocator pages remain 4096 B for both variants.

Update time and allocation latency exclude graph preload/final teardown. Frontend hit percentages and refill counters include preload; they count allocator requests, not ordinary MRAM reads/writes. Peak reserved memory is measured under the backend lock. OOM runs are not ranked by speed.

| Tasklets | Object (B) | Policy | Outcome | Update alloc mean / p99 (us) | Update time (ms) | Frontend hits (%) | Peak reserved (KiB) |
| ---: | ---: | --- | --- | ---: | ---: | ---: | ---: |
| 1 | 256 | adaptive | completed | 8.97 / 119.13 | 9.55 | 94.49 | 52 |
| 4 | 64 | adaptive | completed | 4.41 / 4.51 | 2.19 | 98.31 | 16 |

Per-request cycles, graph phase durations, memory snapshots, generated input with original vertex IDs, source hashes, and binaries are retained in every run folder. The inherited simulator/IRAM limitations and reconstructed baseline repairs in the parent README still apply.
