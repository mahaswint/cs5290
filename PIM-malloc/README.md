# Introduction

**Welcome to the PIM-malloc Dynamic Memory Allocator Documentation!**

PIM-malloc is a scalable and high-performance dynamic memory allocator specifically designed for Processing-In-Memory (PIM) architectures. This allocator enables efficient memory management for general-purpose PIM systems, empowering researchers and developers to optimize their workloads.

This documentation provides a comprehensive overview of PIM-malloc, from foundational concepts to advanced features, making it accessible to both new users and experienced researchers. Explore the guide to fully harness the power of PIM-malloc and elevate your PIM-based applications.

# Contact Information
## Address
[KAIST](https://www.kaist.ac.kr/en/), School of Electrical Engineering

[Vertically Integrated Architecture Research Group](https://sites.google.com/view/kaist-via/home)

Office: N1 818 @ [KAIST](https://www.kaist.ac.kr/en/)

## Email
- Bongjoon Hyun: [bongjoon.hyun@gmail.com](mailto:bongjoon.hyun@gmail.com)
- Dongjae Lee: [dongjae.lee@kaist.ac.kr](mailto:dongjae.lee@kaist.ac.kr)
- Minsoo Rhu: [minsoo.rhu@gmail.com](mailto:minsoo.rhu@gmail.com)

Feel free to reach out with any questions or feedback.

# Released Versions
> **PIM-malloc: A Fast and Scalable Dynamic Memory Allocator for Processing-In-Memory (PIM) Architectures**

For comprehensive insights into PIM-malloc and its applications, we invite you to refer to our HPCA-2026 publication:

"[PIM-malloc: A Fast and Scalable Dynamic Memory Allocator for Processing-In-Memory (PIM) Architectures](https://arxiv.org/pdf/2505.13002)", HPCA, 2026

Currently available versions are:
1. PIM-malloc that is fully compatible with real UPMEM-PIM hardware
2. PIM-malloc that is fully compatible with [uPIMulator](https://github.com/VIA-Research/uPIMulator), an UPMEM-PIM cycle-level simulator

All source code and version history can be accessed in our GitHub repository.

## PIM-malloc that is fully compatible with real UPMEM-PIM hardware
This version of PIM-malloc can run on real UPMEM-PIM hardware.
You will find their source code in the `hw` directory.


Refer to the [README](./hw/README.md) in the `hw` directory for usage instructions.

## PIM-malloc that is fully compatible with uPIMulator
This version of PIM-malloc can run on uPIMulator, a cycle-level UPMEM-PIM simulator. You can find more information on uPIMulator at [its official GitHub repo](https://github.com/VIA-Research/uPIMulator) and [HPCA-24 paper](https://www.computer.org/csdl/proceedings-article/hpca/2024/931300a263/1VOAAZSdy0w).

You will find their source code in the `simulator` directory. The included memory allocators are as follows:

1. **uPIMulator**: We leverage the original uPIMulator code from [its official GitHub repo](https://github.com/VIA-Research/uPIMulator) and applied minor modifications to microbenchmark (`hw/Microbenchmark`) that is irrelevant to PIM-malloc's memory allocation performance. These minor modifications are applied for the compatibility with uPIMulator.

2. **uPIMulatorLevelCache**: We assumed that UPMEM-PIM can be augmented with a small dedicated software cache named **buddy cache** to further accelerate the performance of microbenchmark (`hw/Microbenchmark`). To simulate impacts of such hardware modification, we modified the original uPIMulator backend hardware performance model to include the buddy cache in the UPMEM-PIM's DPU logic.

For more details, refer to the [README](./simulator/README.md) in the simulator directory.

# Contributions
We welcome and encourage contributions to PIM-malloc! If you are interested in contributing or have questions, please feel free to open an issue or submit a pull request.

## List of Maintainers
- Bongjoon Hyun (bongjoon.hyun@gmail.com)
- Dongjae Lee (dongjae.lee@kaist.ac.kr)
- Minsoo Rhu (minsoo.rhu@gmail.com)


# Citation
Dongjae Lee, Bongjoon Hyun, Youngjin Kwon, and Minsoo Rhu, "PIM-malloc: A Fast and Scalable Dynamic Memory Allocator for Processing-In-Memory (PIM) Architectures", IEEE International Symposium on Computer Architecture (HPCA), Feb 2026.