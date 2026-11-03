# Submission source snapshot

Source: https://github.com/mahaswint/cs5290/tree/main/pim-reproduction

The implementation progresses through `adaptive/`, `combined/`, `slabs/`, `paper-workloads/`, `stacked/`, `heavy-workloads/`, `pinned/` and `pinned/paper/`. `submission/` contains the paper, editable presentation and separate result packages. The original Figure 15 reproduction stays at the top level.

The offline `submission/output/source-code.zip` preserves the directory hierarchy. It includes allocator and workload C sources, native test shims, Python runners/analyzers, the maintained and original Go simulator/SDK sources, public CloudPhysics and Gowalla inputs, source manifests, comparison tables and selected final summary evidence. It excludes compiled simulators, compiler binaries, caches and per-run assembly/log/binary directories. `SOURCE_MANIFEST.json` in the archive records SHA-256 hashes of the included files. Upstream attribution remains in `AUTHORS_LICENSE`.

To reproduce, extract into a workspace retaining `pim-reproduction/`, `PIM-malloc/` and `prim-benchmarks/` as siblings. Use Python 3.12 or later, Go 1.22 or later, a C compiler with ASan/UBSan for native tests, and the UPMEM SDK 2023.2.0. Plotting dependencies are pinned in `requirements-lock.txt`. The existing runtime sources can be built directly; preparation scripts are only needed to regenerate a runtime from an upstream checkout and intentionally refuse to overwrite an existing runtime.

Place the SDK at `pim-reproduction/toolchain/LoCaLUT/upmem-2023.2.0-Linux-x86_64`. The original download was recovered from AIS-SNU/LoCaLUT commit `c4cf67ae623c5e313b0628ac119d957887b5f6df`. Its compiler reports UPMEM LLVM revision `846fdda8285dcc9b20ee5d2fec9e54dfea6a8928`. On systems requiring libtinfo.so.5, the runner expects the locally extracted compatibility library under `pim-reproduction/toolchain/compat/lib/x86_64-linux-gnu`; provenance and the package checksum are in `README.md`. The compiler and compatibility library are external dependencies, not included in the source ZIP.

From the workspace root, representative commands are:

```sh
python3 pim-reproduction/pinned/test.py
python3 pim-reproduction/pinned/paper/test.py
python3 pim-reproduction/pinned/sweep.py --preset main --tag pinned-v1 --jobs 4
python3 pim-reproduction/pinned/sweep.py --preset heldout --tag heldout-pinned-v1 --jobs 4
python3 pim-reproduction/pinned/paper/sweep.py --tag paper-v1 --jobs 4
python3 pim-reproduction/pinned/analyze.py
python3 pim-reproduction/pinned/paper/analyze.py --tag paper-v1
```

Use the individual stage READMEs for earlier ablations and alternative presets. Exact timing reproduction requires the recorded compiler and simulator sources, not merely a newer SDK. The final analyzers expect complete declared matrices and their generated timing events and counters; the compact selected summary evidence in the archive is for provenance, and does not replace rerunning those matrices.

The original artifact uses a different WRAM layout from the maintained contribution runtime. Keep their results separate. The simulator is deterministic at 350 MHz; graph/application kernels are reduced ports, and the KV allocation replay does not execute attention or a serving system. The report defines the corresponding limits on performance claims.
