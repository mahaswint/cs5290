# Software results to reproduce

These targets were read from the user-provided HPCA 2026 PDF. They are published values, not simulator measurements made on this PC.

## First: Figure 15 microbenchmark

One DPU, 32 MiB heap, 128 allocation calls per thread; 1 or 16 tasklets; request sizes 32 B, 256 B, and 4 KB. Figure labels explicitly give latency in microseconds.

| Threads | Size | Simple buddy (µs) | PIM-malloc-SW (µs) |
| --- | --- | ---: | ---: |
| 1 | 32 B | 630 | 6.9 |
| 1 | 256 B | 520 | 32 |
| 1 | 4 KB | 350 | 310 |
| 16 | 32 B | 13000 | 51 |
| 16 | 256 B | 11000 | 430 |
| 16 | 4 KB | 7700 | 6700 |

The paper reports an overall 66× software speedup. Ratios computed from these rounded labels depend on the averaging method: the arithmetic mean of the six individual speedup ratios is about 65×, while their geometric mean is about 15×. The report must state its aggregation formula instead of treating these as interchangeable.

## Next: Figure 17 dynamic graph updates

The paper uses the loc-gowalla graph, randomly splitting new and existing edges at a 1:2 ratio. It tests a linked-list array with 256 B allocation blocks and a variable-sized array with requests from 64 B to 32 KB.

The top labels in Figure 17(a) show the following absolute-latency values. The caption identifies them as latency, but does not explicitly print a unit next to these labels, so the CSV preserves the numbers without assigning an unverified unit.

| Layout | Buddy label | PIM-malloc-SW label | SW / buddy speedup derived from labels |
| --- | ---: | ---: | ---: |
| Array of linked lists | 331 | 14.6 | 22.67× |
| Variable-sized array | 351 | 3.3 | 106.36× |

The static CSR reference label is 83.1. Figure 17(b,c) also shows latency distributions and traces; the public microbenchmark artifact does not include their raw graph-update series. Do not invent points or present a newly implemented graph workload as an exact reproduction of these traces.

## Boundaries

The current local reproduction is Figure 15. The graph-update experiment requires a separate workload implementation and dataset preparation. Hardware buddy-cache results, Figure 16, and the Figure 18 language-model serving experiment are excluded. Earlier hardware characterization results in Sections III–IV are not claimed as PC reproductions.
