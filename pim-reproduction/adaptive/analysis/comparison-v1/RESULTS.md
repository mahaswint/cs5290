# Empty-page retention experiment results

Simulated request latencies at 350 MHz. These are maintained-baseline experiments, separate from Figure 15.
Work time includes identical benchmark payload checks, memory snapshots, and barriers. Total time includes initialization and final empty-page drain.
OOM configurations have different completed work and must not be ranked by runtime against successful configurations.

| Workload | Threads | Policy | Outcome | Alloc mean / p99 (us) | Free mean (us) | Total (ms) | Peak reserved (KiB) | Failed requests/init pages |
| --- | ---: | --- | --- | ---: | ---: | ---: | ---: | ---: |
| phases | 1 | adaptive | completed | 26.26 / 181.87 | 16.25 | 6.21 | 32.0 | 0 |
| phases | 1 | eager | completed | 29.35 / 183.53 | 24.45 | 9.02 | 48.0 | 0 |
| phases | 1 | fixed-1 | completed | 26.83 / 181.18 | 16.93 | 6.27 | 28.0 | 0 |
| phases | 1 | fixed-2 | completed | 23.26 / 181.34 | 13.52 | 5.48 | 32.0 | 0 |
| phases | 1 | lazy | completed | 32.37 / 181.24 | 23.12 | 7.45 | 24.0 | 0 |
| phases | 4 | adaptive | completed | 69.29 / 803.60 | 36.42 | 18.52 | 128.0 | 0 |
| phases | 4 | eager | completed | 80.43 / 798.47 | 61.66 | 29.66 | 192.0 | 0 |
| phases | 4 | fixed-1 | completed | 71.65 / 781.01 | 40.47 | 19.40 | 112.0 | 0 |
| phases | 4 | fixed-2 | completed | 61.31 / 641.01 | 30.54 | 16.25 | 128.0 | 0 |
| phases | 4 | lazy | completed | 90.87 / 1020.53 | 58.97 | 23.82 | 96.0 | 0 |

Detailed controller counters, memory ratios at common barriers, all configuration parameters, and exact run paths are in comparison.csv.
Peak reserved memory is measured under the backend lock. Peak requested bytes and reserved/live ratios are sampled at quiescent allocation/free barriers; zero-live ratios are omitted.
The WRAM address limit is 65,536 with 24 x 1,536 B stack reservations. This is a simulation capacity check, not a complete real-hardware or IRAM deployment validation.

Plot: phases-00.png / .svg
