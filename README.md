**Please refer to the final-submission branch**

# Improving Dynamic Memory Allocation in PIM-malloc
CS5290 course project. We reproduce the software allocation benchmark from
PIM-malloc (HPCA 2026) and evaluate empty-page retention, indexed page lookup,
a 4 KiB slab class, and buddy metadata placement in WRAM.

Read the [report](pim-reproduction/submission/output/pdf/report.pdf),
[experiment guide](docs/EXPERIMENTS.md), and
[reproduction instructions](docs/REPRODUCING.md).
The [submission directory](pim-reproduction/submission/README.md) contains the
report source, presentation, numerical tables, and packaged deliverables.

## Repository layout

| Path | Contents |
| --- | --- |
| `pim-reproduction/` | Allocator implementations, experiment runners, measured results, and analysis |
| `pim-reproduction/pinned/` | Final seven-variant comparison on five application settings |
| `pim-reproduction/pinned/paper/` | Final comparison on nine paper-derived settings |
| `pim-reproduction/submission/` | Report, LaTeX sources, presentation, and submission archives |
| `docs/` | Report-to-code mapping, environment setup, and reproduction workflow |
| `PIM-malloc/` | Released allocator and simulator sources used as the starting point |
| `prim-benchmarks/` | PrIM reference sources and Gowalla input data |
| `uPIMulator/`, `UPMEM/` | Upstream simulator and SDK 2023.1 reference checkouts |
| `archive/hbm-address-mapping/` | Earlier, separate Ramulator assignment |

Experiment directories stay in their original locations so recorded paths,
source hashes, and runner imports remain valid. Within each stage, source files
and runners sit beside `analysis/` tables, `results/` run evidence, and
`validation/` correctness evidence. Build products and local caches are excluded
from source exports.

## Evaluation

The original Figure 15 reproduction contains twelve configurations. The final
contribution matrix contains **14 workload settings × 7 variants = 98
configurations**, with separate held-out inputs and earlier ablations.
[EXPERIMENTS.md](docs/EXPERIMENTS.md) maps every evaluation section to its code,
commands, and results.

The seven final variants are scan fixed-2, indexed adaptive-64, indexed fixed-2,
slabs with either retention policy, and slabs plus pinned metadata with either
policy. Adaptive-64 checks every 64 allocation/free operations. Final comparisons
use one simulated DPU at 350 MHz, a 32 MiB payload heap, a 256 KiB empty-byte
budget per tasklet, and a 64 KiB WRAM address limit.

Work time measures the concurrent application phase; total time also includes
initialization, preload, checking, and reclamation. Full-execution DRAM traffic
is a separate metric. Original-paper reproduction uses the released memory
layout and must be compared separately from the corrected contribution control.
Reduced graph ports and the allocator-only KV replay retain their stated scope;
hardware fit and end-to-end serving performance are not established here.

## Quick start

From the repository root:

```sh
make verify          # Check saved final evidence and submission archive integrity
make test            # Run native ASan/UBSan allocator and workload checks
make analyze         # Audit and regenerate final comparison tables
```

See [REPRODUCING.md](docs/REPRODUCING.md) before building or running simulations.
The native tests require a C compiler; DPU runs additionally require Go and the
recorded UPMEM SDK 2023.2.0. The root `UPMEM/` checkout is a different SDK version.

## Sources and attribution

The starting allocator is [VIA-Research/PIM-malloc](https://github.com/VIA-Research/PIM-malloc),
commit `4fdcbcac3b0d94f2d4d51d8d84c3a3a640ad14a4`.
Upstream licenses remain with their source trees; allocator attribution is in
[AUTHORS_LICENSE](pim-reproduction/AUTHORS_LICENSE). Workload sources,
adaptations, and pins are documented in
[SOURCES.md](pim-reproduction/paper-workloads/SOURCES.md) and the stage manifests.
