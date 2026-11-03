# Reproducing the project

Run commands from the repository root. Preserve the sibling locations of
`pim-reproduction/`, `PIM-malloc/`, and `prim-benchmarks/`: runners and source
manifests use those paths. The older HBM assignment is self-contained under
`archive/hbm-address-mapping/`.

## Environment

Use Python 3.12+, Go 1.22+, and a C compiler supporting AddressSanitizer and
UndefinedBehaviorSanitizer. Plotting dependencies are pinned in
[`requirements-lock.txt`](../pim-reproduction/requirements-lock.txt):

```sh
python3 -m venv pim-reproduction/.venv
pim-reproduction/.venv/bin/python -m pip install -r pim-reproduction/requirements-lock.txt
```

DPU compilation uses UPMEM SDK **2023.2.0**, recovered from AIS-SNU/LoCaLUT commit
`c4cf67ae623c5e313b0628ac119d957887b5f6df`. Place it at
`pim-reproduction/toolchain/LoCaLUT/upmem-2023.2.0-Linux-x86_64/`.
On systems requiring `libtinfo.so.5`, the runner expects the extracted library
under `pim-reproduction/toolchain/compat/lib/x86_64-linux-gnu/`.
[Toolchain provenance](../pim-reproduction/README.md) records the compiler revision
and compatibility package checksum. The root `UPMEM/` SDK 2023.1 checkout is a
reference dependency and does not replace the measured compiler.

Existing runtime source trees are included. Preparation scripts regenerate them
from upstream sources and intentionally refuse to overwrite existing trees.
Refer to each stage README before using a preparation script. Native compilation
uses the recovered compiler directly; Docker is not required by these runners.

## Check the saved submission

```sh
make verify
```

This checks the 14-by-7 matrix, raw summary paths, completion and correctness
flags, timing and provenance fields, recorded WRAM limits, required deliverables,
and ZIP integrity. It does not rerun DPU simulations. The final stage analyzers
perform the deeper source/trace/simulator audits:

```sh
make analyze
```

## Native correctness checks

```sh
make test
```

The final parent suite has 47 cases covering buddy differential tests, layered
allocator tests, and application/stage combinations. The paper extension has 42
native cases. Both use ASan/UBSan and write evidence in their respective
`validation/` directories. DPU validation additionally checks the simulated
memory layout and final heap state.

## Final DPU experiments

Use `--jobs 1` when memory is limited. Independent simulations can run in
parallel; compilation/linking are serialized by the harness.

```sh
python3 pim-reproduction/pinned/sweep.py --preset validation --tag validation-pinned-v1 --jobs 2
python3 pim-reproduction/pinned/sweep.py --preset main --tag pinned-v1 --jobs 4
python3 pim-reproduction/pinned/sweep.py --preset heldout --tag heldout-pinned-v1 --jobs 4
python3 pim-reproduction/pinned/paper/sweep.py --tag paper-v1 --jobs 4
make analyze
```

The sweeps resume accepted configurations when their evidence matches. Use a new
tag for a separate run. Simulated time is deterministic at 350 MHz; simulator
host wall time is not an application performance measurement.

## Earlier experiments

[EXPERIMENTS.md](EXPERIMENTS.md) links every earlier stage and its commands.
For the original twelve-configuration Figure 15 reproduction:

```sh
python3 pim-reproduction/sweep.py --jobs 4
pim-reproduction/.venv/bin/python pim-reproduction/analyze.py
```

## Submission packages

The report PDF is in `pim-reproduction/submission/output/pdf/`, and its editable
source is in `pim-reproduction/submission/latex/`. The supplied report is retained
without rewriting its scientific claims. The presentation and separate baseline,
contribution, and source archives are listed in the
[submission README](../pim-reproduction/submission/README.md).

After changing included source or documentation, refresh the offline source and
submission ZIP files, then verify them:

```sh
make package
make verify
```

Packaging requires the existing report/results PDFs, presentation, analysis CSVs,
and raw final summaries. It rebuilds archives and checksums; it does not compile
the report or rerun simulations. `source-code.zip` contains a compact source and
evidence snapshot. Full timing-event audit requires the complete result folders
or regenerated matrices, as explained in [SNAPSHOT.md](../pim-reproduction/SNAPSHOT.md).

Upstream licenses and source manifests are retained. SDK binaries, native test
executables, caches, and temporary chart workspaces are excluded from new source
exports. Generated build trees and per-run binaries are untracked while local
copies remain available. Earlier Git history is preserved.
