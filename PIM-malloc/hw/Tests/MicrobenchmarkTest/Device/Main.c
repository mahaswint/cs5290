#include <alloc.h>
#include <assert.h>
#include <barrier.h>
#include <buddy_alloc.h>
#include <defs.h>
#include <mram.h>
#include <perfcounter.h>
#include <stdio.h>

#include "Common/Device/Timer.h"
#include "Microbenchmark/Device/uMalloc.h"

#ifndef NR_TASKLETS
#define NR_TASKLETS 16
#endif

__host int mallocableRegionSize;
__host int pageSize;
__host int bufferSize;

struct UMalloc *uMalloc;

BARRIER_INIT(deviceBarrier, NR_TASKLETS);

int main() {
  int threadId = me();

  if (threadId == 0) {
    mem_reset();
    perfcounter_config(COUNT_CYCLES, true);

    int sizeClasses[8] = {16, 32, 64, 128, 256, 512, 1024, 2048};
    int sizeClassesLength = 8;

    uMalloc =
        initUMalloc(mallocableRegionSize, pageSize, bufferSize,
                    DPU_MRAM_HEAP_POINTER, &sizeClasses, sizeClassesLength);
  }
  barrier_wait(&deviceBarrier);

  struct Timer timer = {0, 0};

  startTimer(&timer);

  __mram_ptr void *pointer;

  for (int i = 0; i < 128; i++) {
    pointer = allocateUMalloc(uMalloc, 32);
  }

  stopTimer(&timer);

  return 0;
}
