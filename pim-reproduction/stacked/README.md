# Slabs + indexed allocation + fixed/adaptive retention

This implementation layers the existing 4 KiB object slab idea onto partial/full/empty lists and direct ownership lookup. It supports `--policy fixed` and `--policy adaptive`. The default periodic interval is 128 **allocation plus free** operations per tasklet.

```sh
python3 pim-reproduction/stacked/run.py --slab-kib 32 --policy fixed --threads 4
python3 pim-reproduction/stacked/run.py --slab-kib 32 --policy adaptive --threads 4
```

The existing paper-workload runner also accepts `--slab-kib 16|32|64` with indexed fixed/adaptive. Its original default remains unchanged. The reduced paper traces mainly use smaller objects and do not become 4 KiB slab workloads merely by enabling this option.

[The unchanged paper-workload evaluation](../paper-workloads/SLAB_REAL.md) compares 64 KiB slabs against matching no-slab indexed baselines on Larson, linked-block insertion, growing arrays, graph churn and PrIM selection, under both retention policies and at 1/4 tasklets.

[The large-request evaluation](../heavy-workloads/README.md) selects mixed-size Larson with an explicit 8–4095 B range and a real CloudPhysics I/O-trace write cache. Both naturally exercise the added class under their documented configurations; original variable I/O sizes and existing smaller-workload results remain preserved.

[The pinned-metadata extension](../pinned/README.md) layers shared WRAM upper buddy metadata onto this stack, preserving lower MRAM streaming. Its separate full-stage matrix uses adaptive interval 64, compares both retention policies, and reports incremental pinning effects alongside the earlier stages.

## Geometry and ownership

| Setting | Requests 16–2048 B | Requests 2049–4096 B | 4096 B objects/refill |
| --- | --- | --- | ---: |
| `--slab-kib 0` | Original indexed 4 KiB pages | Direct buddy allocation | 1 |
| `--slab-kib 16` | Original indexed 4 KiB pages | New 16 KiB slab class | 3 |
| `--slab-kib 32` | Original indexed 4 KiB pages | New 32 KiB slab class | 7 |
| `--slab-kib 64` | Original indexed 4 KiB pages | New 64 KiB slab class | 15 |

Capacity is `floor((slab_bytes − 24) / 4096)`. The 24 B header and 128-bit bitmap remain in MRAM. Requests above 4096 B bypass the frontend. Object pointers have 8 B DMA alignment; the interface does not promise 4096 B alignment. Geometry is chosen at compilation; live objects retain their geometry and never move.

For slabs, every covered physical 4 KiB leaf receives an 8 B MRAM ownership entry containing tasklet/class and the span's head index. Class tags use a four-bit class field so routing uses bit operations. A free locates the containing span even when its object starts on a later leaf. Indices are relative to the actual payload base. Entries for **all** leaves are cleared before returning the span to buddy. Direct allocations register their base and block size, as in the original indexed allocator. Only the allocating tasklet may free an object; interior and double frees are rejected.

The ownership table remains **64 KiB MRAM for a 32 MiB payload heap**. The ninth class adds one 56 B descriptor per tasklet (896 B at 16 tasklets); descriptors/stack/application storage are WRAM costs checked by the simulator endpoint. The slab payload remains in MRAM.

## Retention and byte budgets

Fixed-2 retains up to **two completely empty spans per class**, subject to the tasklet's shared empty-byte budget. Adaptive grows or decays the corresponding span-count targets. Full and partially occupied spans have no two-span cap.

`--budget-pages` is always charged in **physical 4 KiB units**. A retained 64 KiB slab consumes 16 units, not one. The default here is 64 units = **256 KiB per tasklet**, identically for baseline and slab variants, so two 64 KiB slabs can be retained. This is an explicit larger budget than the previous paper-workload experiment's four units / 16 KiB. It is a ceiling, not an eager reservation. `--budget-pages 4` preserves that earlier byte budget; then an empty 32/64 KiB slab cannot be retained. Native and DPU tests cover that case.

The passive reporter's `retained` value counts 4 KiB units, so memory snapshots remain `retained × 4096`. Cache `pages`/`empty` and retention targets count spans; refill/return counters count backend span calls. Physical units also apply to trim counters. Pressure thresholds still use actual backend bytes. On adaptive refill failure, the allocating tasklet drains its own retained spans and retries once. Fixed follows its configured cap/budget without that emergency retry.

Larger spans require contiguous buddy blocks and can pin more bytes with a sparse live set. There is no hidden direct-allocation fallback: a 64 KiB heap holds 16 direct 4 KiB objects, versus 12/14/15 objects with the respective slab sizes. OOM is nonfatal at the allocator interface. These capacity costs must be reported alongside throughput.

## Validation and measurements

```sh
python3 pim-reproduction/stacked/test.py
python3 pim-reproduction/stacked/sweep.py --preset validation --tag validation-v1 --jobs 2
python3 pim-reproduction/stacked/sweep.py --preset main --tag stacked-v1 --jobs 3
pim-reproduction/.venv/bin/python pim-reproduction/stacked/analyze.py
```

Native ASan/UBSan tests exercise all four slab settings × both policies × 16/256 KiB byte budgets: full payloads, non-overlap, mixed size boundaries, cross-leaf frees, arbitrary reuse, all owner entries cleared, byte limits, idle decay, sparse pinning, wrong-tasklet/interior/double frees, exact capacity, large blocks, nonfatal OOM, missing contiguous spans and full coalescing. Actual DPU validation covers mixed boundaries, both policies, all slab sizes, 16 tasklets, constrained byte budgets, persistent pools and sparse lifetimes.

The main matrix declares 28 performance cases: 16 dense 4 KiB burst comparisons, four persistent-pool comparisons, four sparse-survivor comparisons and four controls at 256/8192 B. The synthetic workloads use the same offered operations across slab settings. `burst` repeats 32 allocations/frees four times; `pool` preloads and replaces seeded victims; `sparse` keeps indices 0,16,… and drains empty spans before its work-end snapshot. Performance runs include sentinel checks; native tests check every payload byte.

Work is concurrent tasklet makespan; total includes initialization, preload, checking and teardown, ending before final accounting. No host transfer, compilation or simulation wall time is counted as DPU time. [RESULTS.md](RESULTS.md) contains full comparisons, memory, counter scope and audit paths. All measurements are from the existing 350 MHz single-DPU simulator. WRAM endpoint checks do not establish worst-case stack depth, IRAM fit or physical hardware deployment.

## Provenance

`origin_manifest.json` pins the starting combined allocator. With slabs disabled, `allocator.c` includes that maintained indexed allocator; slab-enabled code adds the ninth class, span ownership and byte accounting. The standalone slab experiment remains a separate attribution source. Original experiment sources/results are retained under their original directories. Native and DPU generated sources and SHA-256 values bind the new comparisons.

The runner reuses the existing compilation/probe workflow with the shared preparation lock. Each result has its own linked binaries, source, trace manifest, events, statistics, snapshots and logs. Prototype `gate-v1` failed because the legacy assembly lexer treated a helper named `release` as an instruction; the helper was renamed. `gate-v2` predates the final tag encoding. Reports accept only the final declared matrices, excluding prototypes.
