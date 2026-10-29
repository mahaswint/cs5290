#include <dpu.h>
#include <stdio.h>

#define NUM_DPUS 1
#define NUM_TASKLETS 1

int main() {
  int mallocableRegionSize = 256 * 1024 * 1024;
  int pageSize = 4096;
  int bufferSize = 1 * 1024;
  int levelCacheSize = 8;

  struct dpu_set_t dpu_set;
  struct dpu_set_t dpu;

  dpu_alloc(NUM_DPUS, NULL, &dpu_set);

  dpu_load(dpu_set, DPU_BINARY, NULL);

  int i;
  DPU_FOREACH(dpu_set, dpu, i) { dpu_prepare_xfer(dpu, &mallocableRegionSize); }
  dpu_push_xfer(dpu_set, DPU_XFER_TO_DPU, "mallocableRegionSize", 0,
                sizeof(int), DPU_XFER_DEFAULT);

  DPU_FOREACH(dpu_set, dpu, i) { dpu_prepare_xfer(dpu, &pageSize); }
  dpu_push_xfer(dpu_set, DPU_XFER_TO_DPU, "pageSize", 0, sizeof(int),
                DPU_XFER_DEFAULT);

  DPU_FOREACH(dpu_set, dpu, i) { dpu_prepare_xfer(dpu, &bufferSize); }
  dpu_push_xfer(dpu_set, DPU_XFER_TO_DPU, "bufferSize", 0, sizeof(int),
                DPU_XFER_DEFAULT);

  DPU_FOREACH(dpu_set, dpu, i) { dpu_prepare_xfer(dpu, &levelCacheSize); }
    dpu_push_xfer(dpu_set, DPU_XFER_TO_DPU, "levelCacheSize", 0, sizeof(int),
                  DPU_XFER_DEFAULT);

  dpu_launch(dpu_set, DPU_SYNCHRONOUS);

  dpu_free(dpu_set);

  return 0;
}
