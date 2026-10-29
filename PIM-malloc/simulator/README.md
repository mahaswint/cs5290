# ⚙️ Install and Build

`uPIMulator` supports three different versions: `python_cpp`, `golang`, and `golang_vm`. For our simulation, we forked `uPIMulator` and **exclusively** used the `golang_vm` version.

The installation and build procedures are identical to the original `uPIMulator`. Please refer to the official [uPIMulator documentation](https://github.com/VIA-Research/uPIMulator/blob/main/golang_vm/README.md) for detailed instructions.

# 👀 Overview

While we achieved significant performance improvements with `microbenchmark` compared to naive buddy allocators, we sought further optimization through HW/SW co-design. To achieve this, we modified the DPU logic at the simulation level using uPIMulator. Below, we explain the modifications made to uPIMulator and analyze `microbenchmark` performance with and without HW/SW co-design.

## uPIMulator (PIM-malloc-SW)

The `uPIMulator` directory is forked from the original [uPIMulator GitHub repository](https://github.com/VIA-Research/uPIMulator) without changes to the simulator code. We added a new benchmark, `hw/microbenchmark`, which is fully compatible with real UPMEM hardware, following the official [benchmark addition guidelines](https://github.com/VIA-Research/uPIMulator/blob/main/golang_vm/README.md). Minor modifications were applied to the `hw/microbenchmark` source code to enable it to run on the `uPIMulator` host-side virtual machine, adhering to the [official guidelines](https://github.com/VIA-Research/uPIMulator/blob/main/golang_vm/README.md).

## uPIMulatorLevelCache (PIM-malloc-HW/SW)

We introduced a small hardware metadata buffer, referred to as the **buddy cache**, into the DPU logic of `uPIMulatorLevelCache` to evaluate its performance implications. The following functions were added to the `microbenchmark` source code:

```c
void __attribute__((noinline)) init_level_cache(__mram_ptr int* mramMetadataBase);
int __attribute__((noinline)) lookup_level_cache(int metadataIndex);
int __attribute__((noinline)) read_level_cache(int metadataIndex);
int __attribute__((noinline)) write_level_cache(int metadataIndex, int value);
```

These functions leverage a custom instruction, `lookup_lc`, which extends the official UPMEM-PIM ISA. Below are their definitions:

```assembly
init_level_cache:
    sd r22, 0, d22
    add r22, r22, 8
    init_lc r0
    ld d22, r22, -8
    jump r23

lookup_level_cache:
    sd r22, 0, d22
    add r22, r22, 8
    lookup_lc r0, r0
    ld d22, r22, -8
    jump r23

read_level_cache:
    sd r22, 0, d22
    add r22, r22, 8
    read_lc r0, r0
    ld d22, r22, -8
    jump r23

write_level_cache:
    sd r22, 0, d22
    add r22, r22, 8
    write_lc r0, r1
    move r0, 1
    ld d22, r22, -8
    jump r23
```

To simulate this hardware metadata buffer, we followed these steps:

### 1. Compilation

Run the following command:

```bash
./build/uPIMulator --benchmark microbenchmark --num_tasklets 16 --mode compile
```

This generates the file `./benchmark/build/microbenchmark/dpu/CMakeFiles/microbenchmark/dpu/task.c.o`. After compilation, the content of the assembly code shown above must be copied into `./benchmark/build/microbenchmark/dpu/CMakeFiles/microbenchmark/dpu/task.c.o` to provide the definitions for the `init_level_cache`, `lookup_level_cache`, `read_level_cache`, and `write_level_cache` functions. 

> This copy-paste step must be repeated after each compilation, especially if the number of threads is modified.

### 2. Linking and Simulation

Run the following commands:

```bash
./build/uPIMulator --benchmark microbenchmark --num_tasklets 16 --mode link

./build/uPIMulator --benchmark microbenchmark --num_tasklets 16 --mode simulate
```

These commands link the compiled code and run the simulation. The `uPIMulatorLevelCache` outputs simulation logs to `stdout`, similar to the original `uPIMulator`.
