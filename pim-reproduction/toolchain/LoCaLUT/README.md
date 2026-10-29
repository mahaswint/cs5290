# [HPCA'26] LoCaLUT: Harnessing Capacity-Computation Tradeoffs for LUT-Based Inference in DRAM-PIM
This repository contains the source code for LoCaLUT, an efficient LUT-based PIM design for low-bit DNN inference.
Please cite the paper for more information.

## Hardware Dependencies
- DDR4 channels equipped with UPMEM DIMMs
- Multicore x86_64 CPU

## Software Dependencies
- UPMEM SDK driver (version 2023.2.0, available from the [UPMEM website] (https://sdk.upmem.com/).)
- g++
- Python3.6
- Scipy
- Pandas
- Numpy
- Matplotlib

## Directories
- lut_kernel_w1a3/host.c #Source code for LoCaLUT and baseline implementations with 1-bit weights and 3-bit activations.
- lut_kernel_w1a3/dpu_[method].c #Code for executing GEMM operation on PIM side.
- upmem-2023.2.0_opt/ #UPMEM SDK driver

## Environment Setup
```
cd {your LoCaLUT dir};
source ./upmem-2023.2.0-Linux-x86_64/upmem_env.sh
```

## How to run?
You only need to run the following commands, which will display the execution time for all methods, including traditional MAC operations without LUTs, the activation-driven LUT approach (e.g., LUT TENSOR CORE), operation-packed LUT, a version of LoCaLUT without LUT slice streaming, and LoCaLUT with full functionality.
This facilitates comparing implementations across different matrix sizes by adjusting the parameters N/M/K.

```
cd {your LoCaLUT dir};
./script.sh
```

Additionally, users can analyze the advantages and tradeoffs of LUT use by modifying the packing degree with the PD parameters.
After executing the code, a GEMM operation will be performed on the PIM according to the selected method, and the execution time will be returned.
The code also contains procedures for generating both the canonical LUT and the reordering LUT.


## TO DO
Additional implementations for different bit widths and other benchmarks
