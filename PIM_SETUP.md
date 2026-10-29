# PIM-malloc workspace

Prepared on 29 October 2026 in `/home/mahaswint/cs5290` for **PIM-malloc: A Fast and Scalable Dynamic Memory Allocator for Processing-In-Memory (PIM) Architectures**, HPCA 2026, by Dongjae Lee, Bongjoon Hyun, Youngjin Kwon, and Minsoo Rhu.

## Where to start

Use the simulators bundled with `PIM-malloc` for the paper experiments. The standalone `uPIMulator` checkout is an upstream reference.

| Directory | Purpose |
| --- | --- |
| `PIM-malloc/hw` | Allocator implementations and tests for real UPMEM hardware |
| `PIM-malloc/simulator/uPIMulator/golang_vm/uPIMulator` | Paper's software allocator simulator and microbenchmark |
| `PIM-malloc/simulator/uPIMulatorLevelCache/golang_vm/uPIMulator` | Paper's simulator with the hardware buddy cache |
| `uPIMulator` | Upstream simulator, including Python/C++, Go, and Go + VM versions |
| `prim-benchmarks` | Original PrIM benchmark suite for reference; adapted workloads are already bundled with uPIMulator |
| `UPMEM` | CMU-SAFARI mirror of SDK 2023.1.0, referenced by PrIM's current README |

Sources: [PIM-malloc](https://github.com/VIA-Research/PIM-malloc), [uPIMulator](https://github.com/VIA-Research/uPIMulator), [PrIM](https://github.com/CMU-SAFARI/prim-benchmarks), [SDK mirror](https://github.com/CMU-SAFARI/UPMEM).

The existing Ramulator assignment files were left unchanged.

## Build status

Built with Go 1.22.2 on Ubuntu 24.04, x86_64:

- `PIM-malloc/simulator/uPIMulator/golang_vm/uPIMulator/build/uPIMulator`
- `PIM-malloc/simulator/uPIMulatorLevelCache/golang_vm/uPIMulator/build/uPIMulator`
- `uPIMulator/golang_vm/uPIMulator/build/uPIMulator`

All three executables passed their `-help` checks. Use a single hyphen: these parsers do not accept `--help`.

To rebuild the paper's two variants:

```bash
cd /home/mahaswint/cs5290
export GOCACHE=/home/mahaswint/cs5290/.cache/go-build
for variant in uPIMulator uPIMulatorLevelCache; do
    sim_root="/home/mahaswint/cs5290/PIM-malloc/simulator/$variant/golang_vm/uPIMulator"
    mkdir -p "$sim_root/build"
    go build -C "$sim_root/src" -o "$sim_root/build/uPIMulator"
done
```

## What remains before running benchmarks

The software reproduction now has a working native toolchain and successful full simulator runs. Use `pim-reproduction/README.md` for the current workflow and `python3 pim-reproduction/sweep.py --jobs 4` to resume the Figure 15 sweep.

SDK **2023.2.0** was recovered from the LoCaLUT HPCA 2026 artifact and is stored in `pim-reproduction/toolchain/LoCaLUT/upmem-2023.2.0-Linux-x86_64`. Its missing `libtinfo.so.5` dependency was extracted locally under `pim-reproduction/toolchain/compat`. The native harness sets the required paths without a system-wide installation. Compiler provenance, checksums, and experiment adaptations are recorded in the reproduction README.

Docker is not installed. The authors' original automatic compilation stage still invokes Docker and references the unavailable `sdk-releases.upmem.com` host. The reproduction harness calls the recovered compiler directly. The standalone `UPMEM` clone remains the SDK 2023.1.0 reference.

Real UPMEM hardware is needed for the hardware implementation. The cycle-level simulator models the DPUs in software.

After Docker and the compiler image work, a baseline invocation is:

```bash
cd /home/mahaswint/cs5290
sim_root=/home/mahaswint/cs5290/PIM-malloc/simulator/uPIMulator/golang_vm/uPIMulator
run_dir=$(mktemp -d /home/mahaswint/cs5290/pim-baseline.XXXXXX)
"$sim_root/build/uPIMulator" \
    --root_dirpath "$sim_root" \
    --bin_dirpath "$run_dir" \
    --benchmark microbenchmark \
    --num_channels 1 --num_ranks_per_channel 1 --num_dpus_per_rank 1 \
    --num_tasklets 16 --verbose 0
```

Use a fresh output directory for every run: the baseline simulator removes the directory passed as `bin_dirpath` before writing outputs. This command has not yet been run, and is a starting configuration, not a claim of paper reproduction.

For the buddy-cache variant, follow `PIM-malloc/simulator/README.md`: run `--mode compile`, insert the documented cache-instruction assembly into the generated `task.c.o`, then run `--mode link` and `--mode simulate`. Supply absolute `root_dirpath` and `bin_dirpath` values consistently. The assembly insertion must be repeated after recompilation. The baseline variant does not expose the `mode` option.

## Source versions

These are the checkout versions fetched during setup, not independently verified paper-release pins:

| Repository | Commit |
| --- | --- |
| `VIA-Research/PIM-malloc` | `4fdcbcac3b0d94f2d4d51d8d84c3a3a640ad14a4` |
| `VIA-Research/uPIMulator` | `870d916334e9ff0b190f555f951a9ec3c4257781` |
| `CMU-SAFARI/prim-benchmarks` | `67179099730cdccc650d52d25f458cb2e4f8a675` |
| `CMU-SAFARI/UPMEM` | `84a0dcd3f57ded98ea049f2be5d45c71eda96ace` |

Each repository was cloned with `--recurse-submodules`.
