# Adaptive maintenance diagnostic

Same graph, seed 5290, 384 undirected edges, four tasklets, two schedule cycles, 32 MiB heap. Only the maintenance interval changes between adaptive runs. The allocator implementation is unchanged.

The interval of 4096 exceeds every tasklet's entire allocation/free operation count. Thus no periodic class-maintenance pass executes; per-call policy checks, growth, pressure checks, and the adaptive policy otherwise remain enabled. This is a diagnostic setting, not a proposed replacement policy.

| Object (B) | Policy | Update time (ms) | Allocation mean (us) | Free mean (us) | Refills | Hits | Peak (KiB) |
| ---: | --- | ---: | ---: | ---: | ---: | ---: | ---: |
| 64 | fixed-2 | 15.804 | 4.373 | 4.734 | 12 | 1317 | 48 |
| 64 | adaptive, interval 64 | 16.152 | 5.079 | 5.521 | 12 | 1317 | 48 |
| 64 | adaptive, interval 4096 | 16.098 | 4.962 | 5.408 | 12 | 1317 | 48 |
| 256 | fixed-2 | 24.527 | 15.897 | 10.211 | 60 | 1269 | 208 |
| 256 | adaptive, interval 64 | 25.114 | 16.277 | 10.826 | 60 | 1269 | 208 |
| 256 | adaptive, interval 4096 | 24.657 | 16.485 | 10.899 | 60 | 1269 | 208 |

All compared runs completed identical graph and allocator work, with matching input and implementation hashes. Refills, hits, and peak reservation are identical within each object-size group.

Default adaptive recorded four growth events and 28 target decays: each active class grows from one to two; the seven unused classes per tasklet decay from one to zero. Idle trims, pressure trims, and emergency trims are zero.

Suppressing periodic passes reduces elapsed update time by about 0.053 ms for 64 B and 0.457 ms for 256 B. Remaining differences from fixed-2 include the other adaptive checks and the resulting instruction/lock/MRAM scheduling. The 256 B mean allocation/free latencies increase slightly despite lower elapsed time, so this does not isolate a fixed cost per request; concurrent waits overlap and scheduling changes.

The graph uses one allocation size class per run. Under an unconstrained heap, fixed-2 already offers the same retention ceiling that adaptive grows toward. The adaptive controller has no demonstrated cache-hit or memory-capacity advantage on this trace.

Exact run paths and additional counters are in comparison.csv.
