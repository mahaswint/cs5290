# Experiments and report mapping

The report is [Improving Dynamic Memory Allocation in PIM-malloc](../pim-reproduction/submission/output/pdf/report.pdf).
The supplied report matches this PDF byte for byte. Earlier stages isolate design
choices; the final comparison combines them using common corrected allocator paths.

| Report section | Experiment and implementation | Results |
| --- | --- | --- |
| III-A, implementation and memory limits | [Common allocator fixes and native checks](../pim-reproduction/adaptive/README.md), [WRAM analysis](../pim-reproduction/research/WRAM_AND_SLAB_CONSTRAINTS.md) | Stage validation records and final WRAM endpoints |
| V-A, original paper baseline | [Figure 15 reproduction](../pim-reproduction/README.md), `run.py`, `sweep.py`, preserved `runtime/` | [Twelve-configuration comparison](../pim-reproduction/analysis/RESULTS.md) |
| V-B, retention, indexing, check interval | [Retention](../pim-reproduction/adaptive/README.md), [indexed allocator](../pim-reproduction/combined/README.md), [paper workloads](../pim-reproduction/paper-workloads/README.md) | [Combined summary](../pim-reproduction/combined/SUMMARY.md), [workload results](../pim-reproduction/paper-workloads/RESULTS.md), [64/128-operation study](../pim-reproduction/paper-workloads/INTERVAL128.md) |
| V-C, standalone slab geometry | [16/32/64 KiB slab study](../pim-reproduction/slabs/README.md) | `pim-reproduction/slabs/analysis/slabs-full-v1/` and `slabs-capacity-v1/` |
| V-D, slabs with indexing and retention | [Combined slab allocator](../pim-reproduction/stacked/README.md) | [Dense reuse, sparse survivors, capacity and bypass controls](../pim-reproduction/stacked/RESULTS.md), [workload slab study](../pim-reproduction/paper-workloads/SLAB_REAL.md) |
| V-E, larger real request sizes | [Configured Larson and CloudPhysics replay](../pim-reproduction/heavy-workloads/README.md) | [Main and held-out comparisons](../pim-reproduction/heavy-workloads/RESULTS.md) |
| V-F/G, full comparison and incremental pinning | [Pinned buddy backend and seven variants](../pim-reproduction/pinned/README.md) | [Five application settings and held-out inputs](../pim-reproduction/pinned/RESULTS.md) |
| V-H, paper workloads | [Insertion graphs, allocation microbenchmarks and KV allocation replay](../pim-reproduction/pinned/paper/README.md) | [Nine settings across seven variants](../pim-reproduction/pinned/paper/RESULTS.md) |
| V-I, total time and memory | Both final matrices | [All 98 runs](../pim-reproduction/submission/evidence/final-98-runs.csv), [traffic and memory](../pim-reproduction/submission/output/tables/traffic-and-memory.csv), [full progression](../pim-reproduction/submission/output/tables/final-progression.csv) |

## Final matrix

| Family | Settings | Variants per setting | Runs | Saved tag |
| --- | --- | --- | --- | --- |
| Main stack | Larson 8–4095 B; CloudPhysics cache; Larson 8–8191 B; graph churn; PrIM selection | 7 | 35 | `pinned/results/pinned-v1/` |
| Paper cases | Two insertion graphs; 32 B/256 B/4 KiB allocations at 1/16 tasklets; KV allocation replay | 7 | 63 | `pinned/paper/results/paper-v1/` |

The seven variants are `baseline`, `indexed-adaptive64`, `indexed-fixed2`,
`slab-adaptive64`, `slab-fixed2`, `slab-pinned-adaptive64`, and
`slab-pinned-fixed2`. Each row in the final CSV points to its raw `summary.json`.
Held-out inputs, DPU validation runs, and failed or superseded development attempts
are separate from these 98 accepted configurations.

## Reading a stage

- C and `.inc` files implement allocators and workload kernels.
- `run.py` or `experiment.py` compiles and runs a configuration; `sweep.py` declares a matrix.
- `test.py` executes native correctness checks with sanitizers.
- `results/<tag>/` stores generated sources, timing events, summaries, and simulator logs.
- `analysis/` stores audited CSV tables, figures, and audit manifests.
- `validation/` stores correctness evidence; transient native executables are ignored.

Original-paper measurements use the released simulator layout. Contribution
measurements share corrected free/reuse handling and a 64 KiB WRAM address limit.
Timing comparisons belong within a stage. Work makespan, individual request
latency, lifecycle time, and full-execution DRAM traffic have separate definitions
in the report and stage READMEs.
