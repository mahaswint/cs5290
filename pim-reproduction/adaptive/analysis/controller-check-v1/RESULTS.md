# Empty-page retention experiment results

Simulated request latencies at 350 MHz. These are maintained-baseline experiments, separate from Figure 15.
Work time includes identical benchmark payload checks, memory snapshots, and barriers. Total time includes initialization and final empty-page drain.
OOM configurations have different completed work and must not be ranked by runtime against successful configurations.

| Workload | Threads | Policy | Outcome | Alloc mean / p99 (us) | Free mean (us) | Total (ms) | Peak reserved (KiB) | Failed requests/init pages |
| --- | ---: | --- | --- | ---: | ---: | ---: | ---: | ---: |
| phases | 1 | adaptive | completed | 26.26 / 181.87 | 16.25 | 6.21 | 32.0 | 0 |

Detailed controller counters, memory ratios at common barriers, all configuration parameters, and exact run paths are in comparison.csv.
Peak reserved memory is measured under the backend lock. Peak requested bytes and reserved/live ratios are sampled at quiescent allocation/free barriers; zero-live ratios are omitted.
The WRAM address limit is 65,536 with 24 x 1,536 B stack reservations. This is a simulation capacity check, not a complete real-hardware or IRAM deployment validation.

Plot: phases-00.png / .svg
