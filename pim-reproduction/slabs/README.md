# Size-dependent slabs: standalone contribution

This experiment implements **only larger slabs for 4 KiB objects**. It has no adaptive allocation, retention targets, feedback, idle decay, pressure controller, or retry-driven reclamation. It is isolated from the `adaptive` experiment: its allocator, simulator runtime, generated source, build products, tests, results, and analysis are under this directory. Do not combine the two contributions when attributing results.

## Exact change

The baseline has the paper-style eight frontend classes (16, 32, ..., 2048 B), backed by 4 KiB MRAM pages. Requests larger than 2048 B bypass the frontend and call the shared buddy allocator. The variant adds one 4096 B size class for requests in 2049..4096 B, with an on-demand slab size selected at compile time:

| `--slab-kib` | Handling of 4096 B request | Objects per backend refill |
| ---: | --- | ---: |
| 0 | Baseline: direct 4 KiB buddy allocation | 1 |
| 16 | 16 KiB MRAM slab | 3 |
| 32 | 32 KiB MRAM slab | 7 |
| 64 | 64 KiB MRAM slab | 15 |

Each slab uses the existing 24 B in-band header: two 32-bit list links and a 128-bit bitmap. Capacity is `floor((slab_bytes - 24) / 4096)`, not slab_bytes / 4096. Smaller classes retain the original geometry and 128-slot bitmap limit; requests above 4096 B still bypass the frontend. The buddy minimum block stays at 4 KiB and its metadata buffer stays at 64 B. Larger slabs are MRAM allocations; the whole slab or its payload does not move into WRAM. A 20 B cache descriptor is kept per tasklet/class, with a 16 B temporary bitmap during operations. The baseline reserves the same nine descriptor entries, including an inactive extra class, for a controlled comparison.

A slab is linked into its allocating tasklet's class list. Objects are allocated by scanning the existing MRAM bitmap/list. Free finds the containing slab using its full span, updates the matching bit, and returns the entire slab to buddy immediately when its last live object is freed. There is no extra empty-slab retention. Live objects never move. Frees must come from the allocating tasklet. Returned object addresses have the same 8 B DMA alignment as the maintained frontend; this is not an interface promising 4 KiB alignment.

`--population eager` (default) initializes one 4 KiB page for each of the original eight classes, identically in the baseline and all slab variants. The new 4096 B class is always on demand. An untouched eager page remains reserved until final drain; a used page is returned when wholly empty. `--population lazy` makes all classes on demand and can be used as a separate sensitivity experiment. Compare like initialization modes.

The tradeoff follows the same rationale as [TCMalloc's page-size discussion](https://google.github.io/tcmalloc/design.html#tcmalloc-page-sizes): bigger spans can amortize backend management but leave more memory tied up by live objects. This is an adaptation to the PIM allocator, not an implementation of TCMalloc, nor a claim that slab allocation is globally novel.

## Run the initial comparison

The runtime/toolchain are already prepared on this PC. From any directory:

```bash
python3 /home/mahaswint/cs5290/pim-reproduction/slabs/experiment.py sweep --preset quick --jobs 2 --tag slabs-quick-v1
python3 /home/mahaswint/cs5290/pim-reproduction/slabs/analyze.py --tag slabs-quick-v1
```

Quick runs **eight configurations**: baseline plus 16/32/64 KiB slabs, at 1 and 4 tasklets. Every case requests 4096 B objects (unless `--size` is explicitly changed), uses a 32 MiB heap, and performs two rounds of 32 allocations per tasklet followed by frees. The quick preset fixes burst/round counts and workload. Matching completed runs are reused; modified implementation sources invalidate reuse, and stale/failed attempts are preserved separately. Use a different tag for an independent campaign.

The initial run was completed successfully on this PC. At four tasklets, each case completed 256 allocations and 256 frees:

| Variant | Mean allocation us | Total init + work + drain ms | Work backend allocations | Peak reserved KiB |
| --- | ---: | ---: | ---: | ---: |
| Baseline | 488.51 | 77.338 | 256 | 640 |
| 16 KiB slab | 181.66 | 30.188 | 88 | 832 |
| 32 KiB slab | 67.85 | 18.006 | 40 | 768 |
| 64 KiB slab | 36.24 | 13.933 | 24 | 896 |

These are small, deterministic burst microbenchmarks. They show amortization on this trace, not a general application speedup. The complete 1/4-tasklet results, p99 values, parameters, and source hashes are in `analysis/slabs-quick-v1/RESULTS.md` and `comparison.csv`. The six-panel plot is `burst-00.png` / `.svg`. Raw events, counters, snapshots, flattened C, assembly, linked inputs, and simulator logs are retained under `results/slabs-quick-v1/`.

## Larger matrix and tradeoff workloads

```bash
python3 /home/mahaswint/cs5290/pim-reproduction/slabs/experiment.py sweep --preset full --jobs 2 --tag slabs-full-v1
python3 /home/mahaswint/cs5290/pim-reproduction/slabs/analyze.py --tag slabs-full-v1
```

Full runs **60 configurations**: four slab choices x 1/2/4/8/16 tasklets x three controlled workloads. Burst/round defaults are 32 and four; the heap is 32 MiB. Each workload uses the same requested object size across variants:

- `burst`: allocate the entire burst, then free in reverse order.
- `phases`: demand changes over four phases: burst/8, burst, burst/4, burst/2 objects (minimum one), with `--rounds` rounds in each phase. This is a synthetic variation in live allocation demand, not a recorded arrival-time trace.
- `sparse`: allocate the burst, free all except indices 0,16,32,..., take a snapshot, then free those survivors. The lifetime schedule is identical across slab choices and exposes memory pinned by a few live objects.
- `correctness`: small-class and bypass checks (16, 129, 4096, 8192 B), intended for verification.

Single example:

```bash
python3 /home/mahaswint/cs5290/pim-reproduction/slabs/experiment.py run --slab-kib 32 --threads 4 --workload sparse --burst 32 --rounds 2 --heap-kib 32768 --tag slabs-sparse-v1
```

Run the matching `--slab-kib 0` case with the same parameters before comparing. `run --help` lists all options. `--size 256` is a useful smaller-class control; `--size 8192` is a direct-bypass control. Payload sizes must be multiples of eight for the shared DMA sentinels.

### Tight-heap capacity check

```bash
for slab in 0 16 32 64; do
  python3 /home/mahaswint/cs5290/pim-reproduction/slabs/experiment.py run --slab-kib "$slab" --population lazy --threads 1 --workload burst --burst 16 --rounds 1 --heap-kib 64 --tag slabs-capacity-v1
done
python3 /home/mahaswint/cs5290/pim-reproduction/slabs/analyze.py --tag slabs-capacity-v1
```

This check is already run. A 64 KiB payload heap holds 16 direct 4 KiB objects, but only 12, 14, or 15 objects using 16, 32, or 64 KiB slabs because of header/tail overhead. The benchmark records the first failed request and frees successful allocations. OOM configurations complete different amounts of work; **do not rank them by shorter latency or runtime**. The analyzer leaves speedup ratios blank for them. Larger contiguous refills may also fail under external fragmentation; there is deliberately no hidden direct-allocation fallback.

## Measurement and correctness

The isolated simulator uses passive zero-instruction labels around allocations, frees, initialization, work, drain, and total execution. Mean/p99 are individual request issue cycles divided by 350 MHz; this is not the Figure 15 batch normalization. Allocation/free timing includes MRAM traffic and buddy-lock waits. The workload time includes the common pointer-table transfers, payload checks, snapshots, and barriers; total additionally includes initialization and final drain. SDK printf is avoided because the released simulator lacks an instruction form it uses; final records are read passively from WRAM.

- `events.csv`: individual request and region timings.
- `stats.csv`: requests, successes/failures, backend-bypass allocations, cache hits, slab refills, and matching frees.
- `snapshots.csv`: requested bytes, rounded live-slot bytes, total reserved payload bytes, and wholly empty bytes at common barriers.
- `summary.json`: outcome, counters, cycle summaries, source/runtime hashes, WRAM address, and exact configuration.

A frontend hit is a request served from an existing page/slab without a backend refill. Peak reserved bytes include live/partial slabs, slab headers, free slots, and untouched eager pages. Backend metadata, WRAM, descriptors, and benchmark storage are separate. The backend counter includes eager initialization; analysis also reports work-only backend allocations. Peak and sampled memory are not interchangeable with fragmentation. Sparse tails and reserved/live ratios must be compared at matching barriers.

```bash
python3 /home/mahaswint/cs5290/pim-reproduction/slabs/test.py
python3 /home/mahaswint/cs5290/pim-reproduction/slabs/test.py --simulator --jobs 2
```

Native tests use the same allocator/backend under AddressSanitizer and UndefinedBehaviorSanitizer, strict DMA alignment/bounds checks, full payload checks, pairwise live-object non-overlap, arbitrary frees, mixed size boundaries, exact capacity, large-node frees, complete coalescing, and nonfatal OOM recovery. All four slab choices are checked with eager and lazy initialization. Native tasklets are interleaved sequentially; the simulator suite checks all slab choices at 1/4 tasklets, baseline/64 KiB phases and sparse lifetimes at 4 tasklets, and 64 KiB slabs at 16 tasklets (13 correctness configurations). `--validate` adds list validation inside a benchmark and is for correctness checks; keep it off consistently in performance comparisons. Payload checks and final root-coalescing assertions remain enabled in performance runs.

## Provenance, isolation, and limits

The buddy backend is extracted from the public [VIA-Research/PIM-malloc artifact](https://github.com/VIA-Research/PIM-malloc), with the common repairs needed for free/pressure experiments: nonfatal OOM, root-sized allocations, matching large-node free/coalescing, DMA-aligned metadata storage, byte-address tail bounds, and unsigned shifts. See `source_manifest.json` for author/source hashes. `origin_manifest.json` records the infrastructure snapshot copied at the start; the allocator was rewritten here without the adaptive controller. This is a maintained/reconstructed baseline, not the unchanged authors' executable. Original Figure 15 results and the other contribution directory are untouched.

Preparation for a fresh local runtime (using the existing reference runtime and SDK setup):

```bash
python3 /home/mahaswint/cs5290/pim-reproduction/slabs/prepare.py
```

Runtime configuration/probes are private to this directory. DPU frequency, pipeline, and MRAM timing remain inherited from the reference configuration. WRAM is capped at 65,536 with the linker's 24 stacks each reserved 1,536 B; every successful simulator run checks its WRAM end. This is not a complete physical-hardware or worst-case stack/IRAM deployment validation. The existing SDK cache and preparation lock are shared only for serialized toolchain compilation; no other experiment's runtime, source, or results are edited. No new hardware or ISA extension is introduced.
