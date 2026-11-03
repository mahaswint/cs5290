# HBM Address Mapping and Bandwidth Analysis in Ramulator 2.0

## Group 11

This earlier CS5290 assignment is independent of the PIM-malloc project. Run the
commands below from `ramulator2/` inside this archive. The assignment handout is
`hbm_addrmap_assignment.pdf`; the original shared snapshot is `ramulator2-share.zip`.

This submission studies the effect of Ramulator 2.0's `RoBaRaCoCh` address mapping on sequential and power-of-two strided HBM3 accesses, and implements an XOR-based channel mapper to recover channel-level parallelism.

### Assigned configuration

- Channels: `N = 32`
- Stride: `512 KiB = 524288 B = 2^19 B`
- XOR shift: `k = 0`

---

## 1. Ramulator version and compiler

Ramulator 2.0 was built at the exact assignment commit:

```text
be93be78055d922aa1d4d33e15bcc8f2b0c61a9d
```

Compiler used:

```text
g++ 13.3.0
```

The implementation requires C++20.

---

## 2. Submission contents

```text
README.md
hbm_addrmap_assignment.pdf
ramulator2-share.zip
ramulator2/
  src/addr_mapper/impl/linear_mappers.cpp
  configs/hbm3.yaml
  configs/hbm3_xor.yaml
  results/stream_robaracoch.txt
  results/strided_robaracoch.txt
  results/stream_xor.txt
  results/strided_xor.txt
```

Trace files are intentionally not included. They are regenerated using the commands below.

No HBM3 DRAM-standard source file was modified. The only source modification is the new address mapper in:

```text
src/addr_mapper/impl/linear_mappers.cpp
```

No CMake change is required because the mapper was added to the existing `linear_mappers.cpp` file.

---

## 3. Build from the pinned commit

Clone Ramulator and check out the required commit:

```bash
git clone https://github.com/CMU-SAFARI/ramulator2
cd ramulator2
git checkout be93be78055d922aa1d4d33e15bcc8f2b0c61a9d
```

Copy the submitted modified mapper and configuration files into this fresh clone:

```bash
cp <submission-dir>/src/addr_mapper/impl/linear_mappers.cpp \
   src/addr_mapper/impl/linear_mappers.cpp

cp <submission-dir>/configs/hbm3.yaml configs/hbm3.yaml
cp <submission-dir>/configs/hbm3_xor.yaml configs/hbm3_xor.yaml
```

Build Ramulator:

```bash
mkdir -p build
cd build
cmake ..
make -j
cp ./ramulator2 ../ramulator2
cd ..
```

Verify the build:

```bash
./ramulator2 -f ./example_config.yaml
```

The command should complete successfully and print the simulator statistics.

---

## 4. Trace format

The experiments use the `LoadStoreTrace` frontend.

Each trace line must have the form:

```text
LD <address>
```

or

```text
ST <address>
```

For example:

```text
LD 0
LD 16
LD 32
```

The copy of `tracegen.py` used for these experiments originally emitted the address before an `R/W` token. Its output routine was adjusted so that it emits `LD/ST` first, followed by the address, as required by `LoadStoreTrace`.

If the provided `tracegen.py` already emits `LD/ST <address>`, no further change is needed.

---

## 5. Generate the traces

Create the trace directory:

```bash
mkdir -p traces
```

### Sequential trace

```bash
python3 tracegen.py \
  --pattern seq \
  --n 30000 \
  --granularity 16 \
  --out traces/stream.trace
```

The first few addresses should be:

```text
LD 0
LD 16
LD 32
LD 48
LD 64
```

### 512 KiB strided trace

```bash
python3 tracegen.py \
  --pattern stride \
  --n 30000 \
  --stride 524288 \
  --region 17179869184 \
  --out traces/strided.trace
```

The first few addresses should be:

```text
LD 0
LD 524288
LD 1048576
LD 1572864
LD 2097152
```

A 16 GiB trace-generator region is used so that the 30,000-request 512 KiB trace does not wrap. This option only controls trace generation; it does not change the simulated HBM3 capacity.

---

## 6. Baseline configuration

`configs/hbm3.yaml` uses:

```text
Frontend       : LoadStoreTrace
MemorySystem   : GenericDRAM
DRAM           : HBM3
Organization   : HBM3_8Gb
Channels       : 32
Timing         : HBM3_2Gbps
Controller     : Generic
Scheduler      : FRFCFS
RefreshManager : AllBank
RowPolicy      : OpenRowPolicy
AddrMapper     : RoBaRaCoCh
```

Both the frontend and memory system use:

```text
clock_ratio: 1
```

---

## 7. Baseline runs

Create the results directory:

```bash
mkdir -p results
```

### Sequential trace with `RoBaRaCoCh`

```bash
./ramulator2 -f ./configs/hbm3.yaml \
  > results/stream_robaracoch.txt
```

### Strided trace with `RoBaRaCoCh`

```bash
./ramulator2 -f ./configs/hbm3.yaml \
  -p "Frontend.path=./traces/strided.trace" \
  > results/strided_robaracoch.txt
```

---

## 8. XOR mapper

The new mapper is registered as:

```text
RoBaRaCoChXOR
```

It first performs the same field slicing as `RoBaRaCoCh`, then XORs a row-derived value into the channel index.

For Group 11:

```text
N = 32
k = 0
```

so the effective mapping is:

```text
new_channel = old_channel XOR (row & 31)
```

The mapper is selected in:

```text
configs/hbm3_xor.yaml
```

using:

```text
AddrMapper:
  impl: RoBaRaCoChXOR
```

---

## 9. XOR runs

### Sequential trace with `RoBaRaCoChXOR`

```bash
./ramulator2 -f ./configs/hbm3_xor.yaml \
  > results/stream_xor.txt
```

### Strided trace with `RoBaRaCoChXOR`

```bash
./ramulator2 -f ./configs/hbm3_xor.yaml \
  -p "Frontend.path=./traces/strided.trace" \
  > results/strided_xor.txt
```

---

## 10. Expected sanity checks

| Run | `memory_system_cycles` | `total_num_read_requests` | Channels with traffic |
|---|---:|---:|---:|
| Stream + `RoBaRaCoCh` | 29999 | 30000 | 32 |
| Strided + `RoBaRaCoCh` | 719185 | 30000 | 1 |
| Stream + `RoBaRaCoChXOR` | 29999 | 30000 | 32 |
| Strided + `RoBaRaCoChXOR` | 29999 | 30000 | 32 |

For the balanced 32-channel runs, channels 0--15 receive 938 requests each and channels 16--31 receive 937 requests each.

Measured aggregate results:

| Trace | Mapper | Aggregate BW | Avg. read latency | Row-buffer hit rate |
|---|---|---:|---:|---:|
| Sequential | `RoBaRaCoCh` | 16.001 GB/s | 12.219 ns | 96.80% |
| Strided | `RoBaRaCoCh` | 0.667 GB/s | 33.730 ns | 0% |
| Sequential | `RoBaRaCoChXOR` | 16.001 GB/s | 12.219 ns | 96.80% |
| Strided | `RoBaRaCoChXOR` | 16.001 GB/s | 25.970 ns | 0% |

All bandwidth values in this table are **aggregate bandwidth across the complete configured HBM3 system**, not per-channel bandwidth.

---

## 11. Relevant model parameters

```text
internal prefetch size = 2
channel width          = 64 bits
transaction size       = 16 B
m_tx_offset            = 4 bits
tCK                     = 1 ns
pseudo-channels/channel = 2
nBL                     = 4
```

The configured theoretical peak bandwidth is:

```text
256 GB/s aggregate
```

This corresponds to:

```text
4 GB/s per pseudo-channel
8 GB/s per channel
256 GB/s across all 32 channels and 64 pseudo-channel buses
```

---

## 12. Reproduction checklist

1. Clone Ramulator 2.0.
2. Check out commit `be93be78055d922aa1d4d33e15bcc8f2b0c61a9d`.
3. Copy the submitted `linear_mappers.cpp` and YAML files into the clone.
4. Build Ramulator.
5. Verify `example_config.yaml`.
6. Ensure `tracegen.py` emits `LD/ST <address>` lines.
7. Regenerate `stream.trace` and `strided.trace` using the exact commands above.
8. Run the two baseline experiments.
9. Run the two XOR-mapper experiments.
10. Compare the generated statistics with the sanity-check values above.

The raw simulator stdout for all four runs is included in the `results/` directory.
