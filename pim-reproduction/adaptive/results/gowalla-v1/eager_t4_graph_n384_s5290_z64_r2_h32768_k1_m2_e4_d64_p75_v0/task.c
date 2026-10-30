#define NR_TASKLETS 4
#define POLICY 0
#define WORKLOAD 4
#define BURST 1
#define ROUNDS 2
#define HEAP_BYTES 33554432
#define KEEP_PAGES 1
#define MAX_KEEP 2
#define EMPTY_BUDGET 4
#define DECAY_OPS 64
#define PRESSURE_PERCENT 75
#define VALIDATE 0
#define GRAPH_BLOCK_BYTES 64
#define BEGIN(kind) __asm__ volatile("policy_begin_" #kind "_%=:" ::: "memory")
#define END(kind) __asm__ volatile("policy_end_" #kind "_%=:" ::: "memory")
/* A tasklet owns each source vertex and its adjacency blocks for their lifetime.
 * This is a small UPMEM port of a paged adjacency list, not the CUDA faimGraph.
 * Each graph block is an allocator object, distinct from its 4096-byte slab.
 */
/* Same-tasklet bitmap allocator with empty-page retention policies.
 * Object geometry and MRAM list search are common across all policies.
 * Policy: 0 eager, 1 lazy, 2 fixed retention, 3 adaptive retention.
 */
/* Derived from the authors' buddy backend. Shared correctness repairs
 * are documented in README.md and source_manifest.json. */
#include <alloc.h>
#include <assert.h>
#include <barrier.h>
#include <buddy_alloc.h>
#include <defs.h>
#include <mram.h>
#include <mutex.h>
#include <dpuruntime.h>
#include <string.h>

#ifndef NR_TASKLETS
#define NR_TASKLETS 16
#endif

struct BuddyAllocator {
  int mallocableRegionSize;
  int minAccessGranularity;
  int logMinAccessGranularity;
  int numBuddyLevels;

  int bufferSize;
  int *wramBuffer;
  int bufferedMetadataIndex;
  int bufferedMetadataLength;

  int metadataSize;
  int *sumMetadataSizePerLevels;
  __mram_ptr int *mramMetadataBase;

  __mram_ptr void *mramMallocableRegionBase;
};

// NOTE(bongjoon.hyun@gmail.com)
// UPMEM's buddy allocator must be initialized prior to the initBuddyAllocator
// function call
struct BuddyAllocator *initBuddyAllocator(int mallocableRegionSize,
                                          int minAccessGranularity,
                                          int bufferSize,
                                          __mram_ptr void *mramBase);
void finiBuddyAllocator(struct BuddyAllocator *buddyAllocator);

__mram_ptr void *buddyAllocate(struct BuddyAllocator *buddyAllocator, int size);
void buddyFree(struct BuddyAllocator *buddyAllocator, __mram_ptr void *pointer);

__mram_ptr void *safeBuddyAllocate(struct BuddyAllocator *buddyAllocator,
                                   int size);
void safeBuddyFree(struct BuddyAllocator *buddyAllocator,
                   __mram_ptr void *pointer);

__mram_ptr void *buddyAllocateForward(struct BuddyAllocator *buddyAllocator,
                                      int level, int leafIndex, int size);

void buddyFreeBackward(struct BuddyAllocator *buddyAllocator, int level,
                       __mram_ptr void *pointer);

void setAllocationsBackward(struct BuddyAllocator *buddyAllocator, int level,
                            int leafIndex);

void initMetadata(struct BuddyAllocator *buddyAllocator);

int getMetadataSize(struct BuddyAllocator *buddyAllocator);
int getMetadataSizePerLevel(struct BuddyAllocator *buddyAllocator, int level);

__mram_ptr void *getMramAddress(struct BuddyAllocator *buddyAllocator,
                                int level, int leafIndex);

int getLeafSize(struct BuddyAllocator *buddyAllocator, int level);
int getNumLeafs(struct BuddyAllocator *buddyAllocator, int level);
int getLeafIndex(struct BuddyAllocator *buddyAllocator, int level,
                 __mram_ptr void *pointer);

int getMetadataIndex(struct BuddyAllocator *buddyAllocator, int level,
                     int leafIndex);
int getMetadataShift(struct BuddyAllocator *buddyAllocator, int level,
                     int leafIndex);
int getMetadata(struct BuddyAllocator *buddyAllocator, int level,
                int leafIndex);
void setMetadata(struct BuddyAllocator *buddyAllocator, int level,
                 int leafIndex, int value);
void flushMetadata(struct BuddyAllocator *buddyAllocator);

int getAllocationLevel(struct BuddyAllocator *buddyAllocator, int size);

int isPowOf2(int value);
int powOf2(int exponent);
int logOf2(int value);


MUTEX_INIT(buddyAllocatorMutex);
// NOTE(bongjoon.hyun@gmail.com): metadata meanings
// 3: empty
// 1 or 2: partially allocated
// 0: fully allocated
struct BuddyAllocator *initBuddyAllocator(int mallocableRegionSize,
                                          int minAccessGranularity,
                                          int bufferSize,
                                          __mram_ptr void *mramBase) {
  struct BuddyAllocator *buddyAllocator =
      mem_alloc(sizeof(struct BuddyAllocator));

  buddyAllocator->mallocableRegionSize = mallocableRegionSize;
  buddyAllocator->minAccessGranularity = minAccessGranularity;
  buddyAllocator->logMinAccessGranularity = logOf2(minAccessGranularity);

  assert(isPowOf2(mallocableRegionSize));
  assert(isPowOf2(minAccessGranularity));

  buddyAllocator->numBuddyLevels =
      logOf2(mallocableRegionSize >> buddyAllocator->logMinAccessGranularity) +
      1;

  assert(bufferSize % sizeof(int) == 0);

  buddyAllocator->bufferSize = bufferSize;
  buddyAllocator->wramBuffer = mem_alloc(bufferSize);

  // NOTE(bongjoon.hyun@gmail.com)
  // -1 means that bufferMetadataIndex doesn't exist
  buddyAllocator->bufferedMetadataIndex = -1;

  // NOTE(bongjoon.hyun@gmail.com)
  // -1 means that bufferedMetadataLength doesn't exist
  buddyAllocator->bufferedMetadataLength = -1;

  buddyAllocator->metadataSize = getMetadataSize(buddyAllocator);

  buddyAllocator->sumMetadataSizePerLevels =
      mem_alloc((buddyAllocator->numBuddyLevels + 1) * sizeof(int));
  int sum = 0;
  for (int i = 0; i < buddyAllocator->numBuddyLevels + 1; i++) {
    buddyAllocator->sumMetadataSizePerLevels[i] = sum;

    if (i != buddyAllocator->numBuddyLevels) {
        sum += getMetadataSizePerLevel(buddyAllocator, i);
    }
  }

  buddyAllocator->mramMetadataBase = mramBase;

  initMetadata(buddyAllocator);

  if (((uint32_t)(mramBase + buddyAllocator->metadataSize)) % 8 == 0) {
    buddyAllocator->mramMallocableRegionBase =
        mramBase + buddyAllocator->metadataSize;
  } else {
    buddyAllocator->mramMallocableRegionBase =
        (__mram_ptr void *)((((uint32_t)(mramBase +
                                         buddyAllocator->metadataSize)) /
                                 8 +
                             1) *
                            8);
  }

  return buddyAllocator;
}

void finiBuddyAllocator(struct BuddyAllocator *buddyAllocator) {}

__mram_ptr void *buddyAllocate(struct BuddyAllocator *buddyAllocator,
                               int size) {
  assert(size >= buddyAllocator->minAccessGranularity);

  mutex_lock(buddyAllocatorMutex);
  __mram_ptr void *pointer = safeBuddyAllocate(buddyAllocator, size);
  mutex_unlock(buddyAllocatorMutex);
  return pointer;
}

void buddyFree(struct BuddyAllocator *buddyAllocator,
               __mram_ptr void *pointer) {
  mutex_lock(buddyAllocatorMutex);
  safeBuddyFree(buddyAllocator, pointer);
  mutex_unlock(buddyAllocatorMutex);
}

__mram_ptr void *safeBuddyAllocate(struct BuddyAllocator *b, int size) {
  return buddyAllocateForward(b, 0, 0, size);
}

void safeBuddyFree(struct BuddyAllocator *b, __mram_ptr void *pointer) {
  /* Directly allocated large nodes leave their descendants empty. The lowest
     fully occupied node on this path identifies the actual allocation. */
  for (int level = b->numBuddyLevels - 1; level >= 0; --level) {
    int index = getLeafIndex(b, level, pointer);
    if (getMetadata(b, level, index) == 0) {
      assert(pointer == b->mramMallocableRegionBase + index * getLeafSize(b, level));
      buddyFreeBackward(b, level, pointer);
      return;
    }
  }
  assert(0 && "invalid or double backend free");
}
__mram_ptr void *buddyAllocateForward(struct BuddyAllocator *buddyAllocator,
                                      int level, int leafIndex, int size) {
  assert(0 <= level && level < buddyAllocator->numBuddyLevels);
  assert(0 <= leafIndex && leafIndex < getNumLeafs(buddyAllocator, level));

  int allocationLevel = getAllocationLevel(buddyAllocator, size);

  if (level == allocationLevel) {
    int metadata = getMetadata(buddyAllocator, level, leafIndex);

    if (metadata == 3) {
      setMetadata(buddyAllocator, allocationLevel, leafIndex, 0);

      if (level != 0) {
        setAllocationsBackward(buddyAllocator, level - 1, leafIndex / 2);
      }

      int leafSize = getLeafSize(buddyAllocator, allocationLevel);
      int logLeafSize = logOf2(leafSize);

      int offset = leafIndex << logLeafSize;

      return buddyAllocator->mramMallocableRegionBase + offset;
    } else {
      return NULL;
    }
  } else {
    int metadata = getMetadata(buddyAllocator, level, leafIndex);

    if (metadata == 0) {
      return NULL;
    } else if (metadata == 1 || metadata == 2) {
      __mram_ptr void *leftPointer =
          buddyAllocateForward(buddyAllocator, level + 1, leafIndex * 2, size);
      if (leftPointer != NULL) {
        return leftPointer;
      }

      __mram_ptr void *rightPointer = buddyAllocateForward(
          buddyAllocator, level + 1, leafIndex * 2 + 1, size);
      if (rightPointer != NULL) {
        return rightPointer;
      }

      return NULL;
    } else if (metadata == 3) {
      return buddyAllocateForward(buddyAllocator, level + 1, leafIndex * 2,
                                  size);
    } else {
      assert(0 && "metadata is corrupted");
    }
  }
}

void buddyFreeBackward(struct BuddyAllocator *buddyAllocator, int level,
                       __mram_ptr void *pointer) {
  int leafIndex = getLeafIndex(buddyAllocator, level, pointer);

  int metadata = getMetadata(buddyAllocator, level, leafIndex);

  if (metadata == 0) {
    setMetadata(buddyAllocator, level, leafIndex, 3);

    if (level != 0) {
      setAllocationsBackward(buddyAllocator, level - 1, leafIndex / 2);
    }
  }
}

void setAllocationsBackward(struct BuddyAllocator *buddyAllocator, int level,
                            int leafIndex) {
  assert(0 <= level && level < buddyAllocator->numBuddyLevels);
  assert(0 <= leafIndex && leafIndex < getNumLeafs(buddyAllocator, level));

  int targetMetadata = getMetadata(buddyAllocator, level + 1, 2 * leafIndex);
  int buddyMetadata = getMetadata(buddyAllocator, level + 1, 2 * leafIndex + 1);

  if (targetMetadata == 3 && buddyMetadata == 3) {
    setMetadata(buddyAllocator, level, leafIndex, 3);
  } else if (targetMetadata == 0 && buddyMetadata == 0) {
    setMetadata(buddyAllocator, level, leafIndex, 0);
  } else {
    setMetadata(buddyAllocator, level, leafIndex, 1);
  }

  if (level != 0) {
    setAllocationsBackward(buddyAllocator, level - 1, leafIndex / 2);
  }
}

void initMetadata(struct BuddyAllocator *buddyAllocator) {
  memset(buddyAllocator->wramBuffer, 0xFFFFFFFF, buddyAllocator->bufferSize);

  int pos = 0;
  while (pos < buddyAllocator->metadataSize) {
    int size;
    if (pos + buddyAllocator->bufferSize < buddyAllocator->metadataSize) {
      size = buddyAllocator->bufferSize;
    } else {
      size = buddyAllocator->metadataSize - pos;

      memset(buddyAllocator->wramBuffer, 0xFFFFFFFF, size);
    }

    mram_write(buddyAllocator->wramBuffer,
               buddyAllocator->mramMetadataBase + (pos >> 2),
               size);

    pos += size;
  }
}

int getMetadataSize(struct BuddyAllocator *buddyAllocator) {
  int metadataSize = 0;
  for (int i = 0; i < buddyAllocator->numBuddyLevels; i++) {
    metadataSize += getMetadataSizePerLevel(buddyAllocator, i);
  }
  return (metadataSize + 7) & ~7;
}

int getMetadataSizePerLevel(struct BuddyAllocator *buddyAllocator, int level) {
  assert(0 <= level && level < buddyAllocator->numBuddyLevels);

  // NOTE(bongjoon.hyun@gmail.com): sizeof(int) is assumed to be four
  if (level < 4) {
    return 4;
  } else {
    return powOf2(level) >> 2;
  }
}

__mram_ptr void* getMramAddress(struct BuddyAllocator *buddyAllocator, int level,
                                        int leafIndex) {
    assert(0 <= level && level < buddyAllocator->numBuddyLevels);

    if (leafIndex >= 0) {
        __mram_ptr void* mramAddress = buddyAllocator->mramMetadataBase + (buddyAllocator->sumMetadataSizePerLevels[level] >> 2);
        if (level >= 4) {
            mramAddress += (leafIndex >> 2);
        }

        return mramAddress;
    } else {
        if (level == 0) {
            assert(0 && "out-of-range");
        } else {
            return getMramAddress(buddyAllocator, level - 1, getNumLeafs(buddyAllocator, level - 1) + leafIndex);
        }
    }
}

int getLeafSize(struct BuddyAllocator *buddyAllocator, int level) {
  assert(0 <= level && level < buddyAllocator->numBuddyLevels);

  int distanceFromLastLevel = buddyAllocator->numBuddyLevels - 1 - level;

  return powOf2(distanceFromLastLevel)
         << buddyAllocator->logMinAccessGranularity;
}

int getNumLeafs(struct BuddyAllocator *buddyAllocator, int level) {
  assert(0 <= level && level < buddyAllocator->numBuddyLevels);

  return powOf2(level);
}

int getLeafIndex(struct BuddyAllocator *buddyAllocator, int level,
                 __mram_ptr void *pointer) {
  assert(0 <= level && level < buddyAllocator->numBuddyLevels);

  int offset = pointer - buddyAllocator->mramMallocableRegionBase;
  int leafSize = getLeafSize(buddyAllocator, level);
  int logLeafSize = logOf2(leafSize);

  int leafIndex = offset >> logLeafSize;

  assert(0 <= leafIndex && leafIndex < getNumLeafs(buddyAllocator, level));

  return leafIndex;
}

int getMetadataIndex(struct BuddyAllocator *buddyAllocator, int level,
                     int leafIndex) {
  assert(0 <= level && level < buddyAllocator->numBuddyLevels);
  assert(0 <= leafIndex && leafIndex < getNumLeafs(buddyAllocator, level));

  int metadataIndex =
      (buddyAllocator->sumMetadataSizePerLevels[level] >> 2) + (leafIndex >> 4);

  return metadataIndex;
}

int getMetadataShift(struct BuddyAllocator *buddyAllocator, int level,
                     int leafIndex) {
  assert(0 <= level && level < buddyAllocator->numBuddyLevels);
  assert(0 <= leafIndex && leafIndex < getNumLeafs(buddyAllocator, level));

  return (leafIndex % 16) * 2;
}

int getMetadata(struct BuddyAllocator *buddyAllocator, int level,
                int leafIndex) {
  assert(0 <= level && level < buddyAllocator->numBuddyLevels);
  assert(0 <= leafIndex && leafIndex < getNumLeafs(buddyAllocator, level));

  int metadataIndex = getMetadataIndex(buddyAllocator, level, leafIndex);
  int metadataShift = getMetadataShift(buddyAllocator, level, leafIndex);

  if (buddyAllocator->bufferedMetadataIndex != -1 &&
      buddyAllocator->bufferedMetadataLength != -1 &&
      buddyAllocator->bufferedMetadataIndex <= metadataIndex &&
      metadataIndex < buddyAllocator->bufferedMetadataIndex + buddyAllocator->bufferedMetadataLength) {
    int bufferIndex = metadataIndex - buddyAllocator->bufferedMetadataIndex;

    int metadata = ((unsigned int)(buddyAllocator->wramBuffer[bufferIndex] &
                                   (0b11U << metadataShift))) >>
                   metadataShift;

    return metadata;
  } else {
    flushMetadata(buddyAllocator);

    __mram_ptr void *mramAddress = (__mram_ptr void *)
        ((uint32_t)getMramAddress(buddyAllocator, level, leafIndex) & ~7U);
    int available = (uint32_t)buddyAllocator->mramMetadataBase +
                    buddyAllocator->metadataSize - (uint32_t)mramAddress;
    int dmaTransferSize = available < buddyAllocator->bufferSize ?
                         available : buddyAllocator->bufferSize;
    assert(dmaTransferSize >= 8 && dmaTransferSize % 8 == 0);

    mram_read(mramAddress, buddyAllocator->wramBuffer, dmaTransferSize);

    buddyAllocator->bufferedMetadataIndex =
       ((int) (((__mram_ptr void* ) mramAddress) - ((__mram_ptr void* ) buddyAllocator->mramMetadataBase))) >> 2;

    buddyAllocator->bufferedMetadataLength = dmaTransferSize >> 2;

    int bufferIndex = metadataIndex - buddyAllocator->bufferedMetadataIndex;

    int metadata = ((unsigned int)(buddyAllocator->wramBuffer[bufferIndex] &
                                   (0b11U << metadataShift))) >>
                   metadataShift;

    return metadata;
  }
}

void setMetadata(struct BuddyAllocator *buddyAllocator, int level,
                 int leafIndex, int value) {
  assert(0 <= level && level < buddyAllocator->numBuddyLevels);
  assert(0 <= leafIndex && leafIndex < getNumLeafs(buddyAllocator, level));
  assert(value == 0 || value == 1 || value == 2 || value == 3);

  int metadataIndex = getMetadataIndex(buddyAllocator, level, leafIndex);
  int metadataShift = getMetadataShift(buddyAllocator, level, leafIndex);

  if (buddyAllocator->bufferedMetadataIndex != -1 &&
    buddyAllocator->bufferedMetadataLength != -1 &&
    buddyAllocator->bufferedMetadataIndex <= metadataIndex &&
    metadataIndex < buddyAllocator->bufferedMetadataIndex + buddyAllocator->bufferedMetadataLength) {
    int bufferIndex = metadataIndex - buddyAllocator->bufferedMetadataIndex;

    assert(0 <= bufferIndex &&
           bufferIndex < buddyAllocator->bufferedMetadataLength);

    if (value == 0) {
      int mask = ~(0b11U << metadataShift);

      buddyAllocator->wramBuffer[bufferIndex] &= mask;
    } else if (value == 1) {
      int mask0 = 0b1U << metadataShift;
      int mask1 = ~(0b1U << (metadataShift + 1));

      buddyAllocator->wramBuffer[bufferIndex] |= mask0;
      buddyAllocator->wramBuffer[bufferIndex] &= mask1;
    } else if (value == 2) {
      int mask0 = ~(0b1U << metadataShift);
      int mask1 = 0b1U << (metadataShift + 1);

      buddyAllocator->wramBuffer[bufferIndex] &= mask0;
      buddyAllocator->wramBuffer[bufferIndex] |= mask1;
    } else if (value == 3) {
      int mask = 0b11U << metadataShift;

      buddyAllocator->wramBuffer[bufferIndex] |= mask;
    } else {
      assert(0 && "value is corrupted");
    }
  } else {
    flushMetadata(buddyAllocator);

    __mram_ptr void *mramAddress = (__mram_ptr void *)
        ((uint32_t)getMramAddress(buddyAllocator, level, leafIndex) & ~7U);
    int available = (uint32_t)buddyAllocator->mramMetadataBase +
                    buddyAllocator->metadataSize - (uint32_t)mramAddress;
    int dmaTransferSize = available < buddyAllocator->bufferSize ?
                         available : buddyAllocator->bufferSize;
    assert(dmaTransferSize >= 8 && dmaTransferSize % 8 == 0);

    mram_read(mramAddress, buddyAllocator->wramBuffer, dmaTransferSize);

    buddyAllocator->bufferedMetadataIndex =
       ((int) (((__mram_ptr void* ) mramAddress) - ((__mram_ptr void* ) buddyAllocator->mramMetadataBase))) >> 2;

    buddyAllocator->bufferedMetadataLength = dmaTransferSize >> 2;

    int bufferIndex = metadataIndex - buddyAllocator->bufferedMetadataIndex;

    assert(0 <= bufferIndex &&
           bufferIndex < buddyAllocator->bufferedMetadataLength);

    if (value == 0) {
      int mask = ~(0b11U << metadataShift);

      buddyAllocator->wramBuffer[bufferIndex] &= mask;
    } else if (value == 1) {
      int mask0 = 0b1U << metadataShift;
      int mask1 = ~(0b1U << (metadataShift + 1));

      buddyAllocator->wramBuffer[bufferIndex] |= mask0;
      buddyAllocator->wramBuffer[bufferIndex] &= mask1;
    } else if (value == 2) {
      int mask0 = ~(0b1U << metadataShift);
      int mask1 = 0b1U << (metadataShift + 1);

      buddyAllocator->wramBuffer[bufferIndex] &= mask0;
      buddyAllocator->wramBuffer[bufferIndex] |= mask1;
    } else if (value == 3) {
      int mask = 0b11U << metadataShift;

      buddyAllocator->wramBuffer[bufferIndex] |= mask;
    } else {
      assert(0 && "value is corrupted");
    }
  }
}

// NOTE(bongjoon.hyun@gmail.com): sizeof(int) is assumed to be four
void flushMetadata(struct BuddyAllocator *buddyAllocator) {
  if (buddyAllocator->bufferedMetadataIndex != -1 &&
      buddyAllocator->bufferedMetadataLength != -1) {
    __mram_ptr void *mramAddress = buddyAllocator->mramMetadataBase + buddyAllocator->bufferedMetadataIndex;
    int size = (buddyAllocator->bufferedMetadataLength << 2);

    mram_write(buddyAllocator->wramBuffer, mramAddress, size);
  }
}

int getAllocationLevel(struct BuddyAllocator *buddyAllocator, int size) {
  for (int i = buddyAllocator->numBuddyLevels - 1; i >= 0; i--) {
    int leafSize = getLeafSize(buddyAllocator, i);

    if (leafSize >= size) {
      return i;
    }
  }

  assert(0 &&
         "size is bigger than the half of the "
         "buddyAllocator->mallocableRegionSize");
}

int isPowOf2(int value) {
  if (value <= 0) {
    return 0;
  }

  return (value & (value - 1)) == 0;
}

int powOf2(int exponent) { return 1 << exponent; }

int logOf2(int value) { return sizeof(int) * 8 - 1 - __builtin_clz(value); }


#include <stdint.h>

#ifndef POLICY
#define POLICY 3
#endif
#ifndef KEEP_PAGES
#define KEEP_PAGES 1
#endif
#ifndef MAX_KEEP
#define MAX_KEEP 2
#endif
#ifndef EMPTY_BUDGET
#define EMPTY_BUDGET 4
#endif
#ifndef DECAY_OPS
#define DECAY_OPS 64
#endif
#ifndef PRESSURE_PERCENT
#define PRESSURE_PERCENT 75
#endif

#define POLICY_CLASSES 8
#define POLICY_PAGE 4096

struct PolicyCache {
  __mram_ptr void *root;
  unsigned size, pages, empty, target, refill_votes, activity, last_activity;
};
struct PolicyStats {
  unsigned alloc_calls, successes, free_calls, cache_hits, refills, returns;
  unsigned failures, retained_hits, retained, live_slots, peak_slots;
  unsigned grows, decays, idle_trims, pressure_trims, budget_trims, emergency_trims;
  unsigned operations;
};
struct PolicyBackend {
  unsigned used, peak, allocs, frees, failed, pressure_limit;
  volatile unsigned pressure;
};
static struct PolicyCache policy_caches[NR_TASKLETS][POLICY_CLASSES];
static struct PolicyStats policy_stats[NR_TASKLETS];
static struct PolicyBackend policy_backend;
static struct BuddyAllocator *policy_buddy;
static unsigned policy_heap_size;

static unsigned policy_block_size(unsigned size) {
  unsigned block = POLICY_PAGE;
  while (block < size) block <<= 1;
  return block;
}

static __mram_ptr void *policy_backend_alloc(unsigned size) {
  unsigned block = policy_block_size(size);
  __mram_ptr void *p = NULL;
  mutex_lock(buddyAllocatorMutex);
  if (block <= policy_heap_size) p = safeBuddyAllocate(policy_buddy, block);
  if (p) {
    policy_backend.used += block;
    if (policy_backend.used > policy_backend.peak) policy_backend.peak = policy_backend.used;
    policy_backend.allocs++;
  } else policy_backend.failed++;
  policy_backend.pressure = policy_backend.used >= policy_backend.pressure_limit;
  mutex_unlock(buddyAllocatorMutex);
  return p;
}

static unsigned policy_backend_free(__mram_ptr void *p) {
  unsigned block = 0;
  mutex_lock(buddyAllocatorMutex);
  for (int level = policy_buddy->numBuddyLevels - 1; level >= 0; --level) {
    int index = getLeafIndex(policy_buddy, level, p);
    if (getMetadata(policy_buddy, level, index) == 0) {
      block = getLeafSize(policy_buddy, level);
      assert(p == policy_buddy->mramMallocableRegionBase + index * block);
      buddyFreeBackward(policy_buddy, level, p);
      break;
    }
  }
  assert(block && policy_backend.used >= block);
  policy_backend.used -= block;
  policy_backend.frees++;
  policy_backend.pressure = policy_backend.used >= policy_backend.pressure_limit;
  mutex_unlock(buddyAllocatorMutex);
  return block;
}

static void policy_links(__mram_ptr void *page, uint32_t links[2]) {
  mram_read(page, links, 8);
}
static int policy_empty(unsigned bitmap[4]) {
  return bitmap[0] == ~0U && bitmap[1] == ~0U &&
         bitmap[2] == ~0U && bitmap[3] == ~0U;
}
static unsigned policy_capacity(struct PolicyCache *c) {
  unsigned n = (POLICY_PAGE - 24) / c->size;
  return n < 128 ? n : 128;
}

static void policy_add_page(struct PolicyCache *c, __mram_ptr void *p, unsigned tid) {
  uint32_t links[2] __attribute__((aligned(8))) = {0, (uint32_t)c->root};
  unsigned bitmap[4] __attribute__((aligned(8))) = {~0U, ~0U, ~0U, ~0U};
  if (c->root) {
    uint32_t old[2] __attribute__((aligned(8)));
    policy_links(c->root, old);
    old[0] = (uint32_t)p;
    mram_write(old, c->root, 8);
  }
  mram_write(links, p, 8);
  mram_write(bitmap, p + 8, 16);
  c->root = p;
  c->pages++;
  c->empty++;
  policy_stats[tid].retained++;
}

static void policy_release_page(struct PolicyCache *c, __mram_ptr void *page, unsigned tid) {
  uint32_t links[2] __attribute__((aligned(8)));
  policy_links(page, links);
  if (links[0]) {
    uint32_t prev[2] __attribute__((aligned(8)));
    policy_links((__mram_ptr void *)links[0], prev);
    prev[1] = links[1];
    mram_write(prev, (__mram_ptr void *)links[0], 8);
  } else {
    assert(c->root == page);
    c->root = (__mram_ptr void *)links[1];
  }
  if (links[1]) {
    uint32_t next[2] __attribute__((aligned(8)));
    policy_links((__mram_ptr void *)links[1], next);
    next[0] = links[0];
    mram_write(next, (__mram_ptr void *)links[1], 8);
  }
  assert(c->empty && c->pages && policy_stats[tid].retained);
  c->empty--;
  c->pages--;
  policy_stats[tid].retained--;
  policy_stats[tid].returns++;
  assert(policy_backend_free(page) == POLICY_PAGE);
}

/* Remove empty pages only; live payloads never move. */
static unsigned policy_trim_cache(struct PolicyCache *c, unsigned keep, unsigned tid) {
  unsigned removed = 0;
  __mram_ptr void *page = c->root;
  while (page && c->empty > keep) {
    uint32_t links[2] __attribute__((aligned(8)));
    unsigned bitmap[4] __attribute__((aligned(8)));
    policy_links(page, links);
    mram_read(page + 8, bitmap, 16);
    if (policy_empty(bitmap)) {
      policy_release_page(c, page, tid);
      removed++;
    }
    page = (__mram_ptr void *)links[1];
  }
  assert(c->empty <= keep);
  return removed;
}

static void policy_tick(unsigned tid) {
  struct PolicyStats *s = &policy_stats[tid];
  s->operations++;
  if (POLICY != 3 || (s->operations & (DECAY_OPS - 1))) return;
  unsigned pressure = policy_backend.pressure;
  for (unsigned i = 0; i < POLICY_CLASSES; ++i) {
    struct PolicyCache *c = &policy_caches[tid][i];
    if (pressure) {
      s->pressure_trims += policy_trim_cache(c, 0, tid);
      if (c->target) { c->target = 0; s->decays++; }
    } else if (c->activity == c->last_activity) {
      if (c->target) { c->target--; s->decays++; }
      s->idle_trims += policy_trim_cache(c, c->target, tid);
      c->refill_votes = 0;
    }
    c->last_activity = c->activity;
  }
}

static __attribute__((noinline)) void policy_drain(unsigned tid) {
  for (unsigned i = 0; i < POLICY_CLASSES; ++i)
    policy_trim_cache(&policy_caches[tid][i], 0, tid);
}

static __attribute__((noinline)) void policy_init(unsigned heap_size) {
  assert(heap_size >= POLICY_PAGE && isPowOf2(heap_size));
  assert(DECAY_OPS && !(DECAY_OPS & (DECAY_OPS - 1)));
  assert(PRESSURE_PERCENT > 0 && PRESSURE_PERCENT <= 100);
  memset(policy_caches, 0, sizeof(policy_caches));
  memset(policy_stats, 0, sizeof(policy_stats));
  memset(&policy_backend, 0, sizeof(policy_backend));
  policy_heap_size = heap_size;
  policy_backend.pressure_limit = (heap_size / 100) * PRESSURE_PERCENT;
  policy_buddy = initBuddyAllocator(heap_size, POLICY_PAGE, 64, DPU_MRAM_HEAP_POINTER);
  for (unsigned t = 0; t < NR_TASKLETS; ++t) {
    for (unsigned i = 0; i < POLICY_CLASSES; ++i) {
      struct PolicyCache *c = &policy_caches[t][i];
      c->size = 16U << i;
      c->target = MAX_KEEP ? 1 : 0;
      if (POLICY == 0) {
        __mram_ptr void *p = policy_backend_alloc(POLICY_PAGE);
        if (p) policy_add_page(c, p, t);
        else policy_stats[t].failures++;
      }
    }
  }
}

static __attribute__((noinline)) __mram_ptr void *policy_malloc(unsigned size) {
  unsigned tid = me();
  struct PolicyStats *s = &policy_stats[tid];
  s->alloc_calls++;
  policy_tick(tid);
  if (!size || size > policy_heap_size) { s->failures++; return NULL; }
  unsigned class_index = 0;
  while (class_index < POLICY_CLASSES && size > (16U << class_index)) class_index++;
  if (class_index == POLICY_CLASSES) {
    __mram_ptr void *p = policy_backend_alloc(size);
    if (!p && POLICY == 3 && s->retained) {
      unsigned before = s->retained;
      policy_drain(tid);
      s->emergency_trims += before;
      p = policy_backend_alloc(size);
    }
    if (p) {
      s->successes++; s->live_slots += policy_block_size(size);
      if (s->live_slots > s->peak_slots) s->peak_slots = s->live_slots;
    } else s->failures++;
    return p;
  }
  struct PolicyCache *c = &policy_caches[tid][class_index];
  c->activity++;
  __mram_ptr void *page = c->root;
  unsigned bitmap[4] __attribute__((aligned(8)));
  int refill = 0;
  for (;;) {
    while (page) {
      mram_read(page + 8, bitmap, 16);
      for (int w = 3; w >= 0; --w) {
        if (!bitmap[w]) continue;
        unsigned bit = __builtin_clz(bitmap[w]);
        unsigned index = (3 - w) * 32 + bit;
        if (index >= policy_capacity(c)) continue;
        if (policy_empty(bitmap)) {
          assert(c->empty && s->retained);
          c->empty--; s->retained--;
          if (!refill) s->retained_hits++;
        }
        bitmap[w] &= ~(1U << (31 - bit));
        mram_write(bitmap, page + 8, 16);
        if (!refill) s->cache_hits++;
        s->successes++; s->live_slots += c->size;
        if (s->live_slots > s->peak_slots) s->peak_slots = s->live_slots;
        return page + 24 + index * c->size;
      }
      uint32_t links[2] __attribute__((aligned(8)));
      policy_links(page, links);
      page = (__mram_ptr void *)links[1];
    }
    page = policy_backend_alloc(POLICY_PAGE);
    if (!page && POLICY == 3 && s->retained) {
      unsigned before = s->retained;
      policy_drain(tid);
      s->emergency_trims += before;
      page = policy_backend_alloc(POLICY_PAGE);
    }
    if (!page) { s->failures++; return NULL; }
    policy_add_page(c, page, tid);
    s->refills++;
    refill = 1;
    if (POLICY == 3 && ++c->refill_votes >= 2) {
      c->refill_votes = 0;
      if (c->target < MAX_KEEP) { c->target++; s->grows++; }
    }
  }
}

static __attribute__((noinline)) void policy_free(__mram_ptr void *p) {
  if (!p) return;
  unsigned tid = me();
  struct PolicyStats *s = &policy_stats[tid];
  s->free_calls++;
  policy_tick(tid);
  for (unsigned i = 0; i < POLICY_CLASSES; ++i) {
    struct PolicyCache *c = &policy_caches[tid][i];
    __mram_ptr void *page = c->root;
    while (page) {
      uint32_t links[2] __attribute__((aligned(8)));
      policy_links(page, links);
      if (p >= page + 24 && p < page + POLICY_PAGE) {
        unsigned offset = p - page - 24;
        assert(offset % c->size == 0);
        unsigned index = offset / c->size;
        assert(index < policy_capacity(c));
        unsigned bitmap[4] __attribute__((aligned(8)));
        mram_read(page + 8, bitmap, 16);
        unsigned word = 3 - index / 32, bit = 31 - index % 32;
        assert(!(bitmap[word] & (1U << bit)));
        bitmap[word] |= 1U << bit;
        mram_write(bitmap, page + 8, 16);
        assert(s->live_slots >= c->size);
        s->live_slots -= c->size;
        c->activity++;
        if (policy_empty(bitmap)) {
          c->empty++; s->retained++;
          unsigned keep = POLICY == 2 ? KEEP_PAGES : (POLICY == 3 ? c->target : 0);
          unsigned pressure = POLICY == 3 && policy_backend.pressure;
          if (pressure || c->empty > keep || (POLICY != 0 && s->retained > EMPTY_BUDGET)) {
            if (pressure) s->pressure_trims++;
            else if (s->retained > EMPTY_BUDGET && POLICY >= 2) s->budget_trims++;
            policy_release_page(c, page, tid);
          }
        }
        return;
      }
      page = (__mram_ptr void *)links[1];
    }
  }
  /* Contract: frees are issued by their allocating tasklet. */
  unsigned block = policy_backend_free(p);
  assert(s->live_slots >= block);
  s->live_slots -= block;
}

/* Called at a quiescent barrier by tests/benchmarks, outside request timing. */
static void policy_verify(unsigned tid) {
  unsigned total_empty = 0;
  for (unsigned i = 0; i < POLICY_CLASSES; ++i) {
    struct PolicyCache *c = &policy_caches[tid][i];
    unsigned pages = 0, empty = 0;
    uint32_t previous = 0;
    __mram_ptr void *page = c->root;
    while (page) {
      uint32_t links[2] __attribute__((aligned(8)));
      unsigned bitmap[4] __attribute__((aligned(8)));
      policy_links(page, links);
      mram_read(page + 8, bitmap, 16);
      assert(links[0] == previous);
      previous = (uint32_t)page;
      page = (__mram_ptr void *)links[1];
      empty += policy_empty(bitmap);
      assert(++pages <= c->pages);
    }
    assert(pages == c->pages && empty == c->empty);
    total_empty += empty;
  }
  assert(total_empty == policy_stats[tid].retained);
  if (POLICY >= 2) assert(total_empty <= EMPTY_BUDGET);
}

#ifndef GRAPH_BLOCK_BYTES
#define GRAPH_BLOCK_BYTES 256
#endif
#define GRAPH_CAPACITY ((GRAPH_BLOCK_BYTES - 8) / 4)
struct GraphVertex { uint32_t tail; unsigned degree; };
struct GraphBlock { uint32_t previous; unsigned count, neighbors[GRAPH_CAPACITY]; };
static __mram_ptr struct GraphVertex *graph_vertices;
static unsigned graph_live_edges[NR_TASKLETS], graph_live_sum[NR_TASKLETS];
static unsigned benchmark_live[NR_TASKLETS];

static unsigned graph_signature(unsigned source, unsigned destination) {
  return ((source + 1) * 0x9e3779b1U) ^ ((destination + 1) * 0x85ebca6bU);
}

static __attribute__((noinline)) int graph_insert(unsigned source, unsigned destination) {
  unsigned tid = me();
  assert(source % NR_TASKLETS == tid);
  struct GraphVertex vertex __attribute__((aligned(8)));
  struct GraphBlock block __attribute__((aligned(8)));
  mram_read(&graph_vertices[source], &vertex, 8);
  if (!vertex.tail || vertex.degree % GRAPH_CAPACITY == 0) {
    BEGIN(alloc);
    __mram_ptr void *p = policy_malloc(GRAPH_BLOCK_BYTES);
    END(alloc);
    if (!p) return 0;
    memset(&block, 0, sizeof(block));
    block.previous = vertex.tail;
    vertex.tail = (uint32_t)p;
    benchmark_live[tid] += GRAPH_BLOCK_BYTES;
  } else mram_read((__mram_ptr void *)vertex.tail, &block, sizeof(block));
  assert(block.count < GRAPH_CAPACITY);
  block.neighbors[block.count++] = destination;
  vertex.degree++;
  mram_write(&block, (__mram_ptr void *)vertex.tail, sizeof(block));
  mram_write(&vertex, &graph_vertices[source], 8);
  graph_live_edges[tid]++;
  graph_live_sum[tid] += graph_signature(source, destination);
  return 1;
}

/* Reverse-batch deletion is deliberate: no hidden scan to find an edge.
 * Other deletion orders/compaction policies are separate workload variants.
 */
static __attribute__((noinline)) void graph_remove_last(unsigned source, unsigned destination) {
  unsigned tid = me();
  assert(source % NR_TASKLETS == tid);
  struct GraphVertex vertex __attribute__((aligned(8)));
  struct GraphBlock block __attribute__((aligned(8)));
  mram_read(&graph_vertices[source], &vertex, 8);
  assert(vertex.degree && vertex.tail);
  mram_read((__mram_ptr void *)vertex.tail, &block, sizeof(block));
  assert(block.count && block.neighbors[block.count - 1] == destination);
  vertex.degree--; block.count--;
  if (!block.count) {
    __mram_ptr void *p = (__mram_ptr void *)vertex.tail;
    vertex.tail = block.previous;
    BEGIN(free);
    policy_free(p);
    END(free);
    benchmark_live[tid] -= GRAPH_BLOCK_BYTES;
  } else mram_write(&block, (__mram_ptr void *)vertex.tail, sizeof(block));
  mram_write(&vertex, &graph_vertices[source], 8);
  graph_live_edges[tid]--;
  graph_live_sum[tid] -= graph_signature(source, destination);
}

static void graph_check(unsigned vertices) {
  unsigned tid = me(), edges = 0, checksum = 0;
  for (unsigned u = tid; u < vertices; u += NR_TASKLETS) {
    struct GraphVertex vertex __attribute__((aligned(8)));
    mram_read(&graph_vertices[u], &vertex, 8);
    unsigned count = 0;
    uint32_t p = vertex.tail;
    while (p) {
      struct GraphBlock block __attribute__((aligned(8)));
      mram_read((__mram_ptr void *)p, &block, sizeof(block));
      assert(block.count && block.count <= GRAPH_CAPACITY);
      if (count) assert(block.count == GRAPH_CAPACITY);
      for (unsigned i = 0; i < block.count; ++i)
        checksum += graph_signature(u, block.neighbors[i]);
      count += block.count;
      assert(count <= vertex.degree);
      p = block.previous;
    }
    assert(count == vertex.degree);
    edges += count;
  }
  assert(edges == graph_live_edges[tid] && checksum == graph_live_sum[tid]);
}

#define GRAPH_VERTICES 749
#define GRAPH_NEW_EDGES 128
#define GRAPH_BASE_ARCS 512
#define GRAPH_UPDATE_ARCS 256
struct GraphEdge { unsigned source, destination; };
static const unsigned graph_limits[4] = {8,128,16,64};
static const struct GraphEdge graph_base[512] = {
  {124U,8830U},
  {168U,5969U},
  {676U,164437U},
  {276U,119003U},
  {456U,86093U},
  {536U,59622U},
  {244U,50973U},
  {296U,3715U},
  {712U,172180U},
  {720U,165725U},
  {692U,164782U},
  {724U,103193U},
  {180U,10377U},
  {64U,5889U},
  {156U,82395U},
  {740U,137366U},
  {596U,17500U},
  {548U,147319U},
  {316U,56169U},
  {436U,32466U},
  {564U,50817U},
  {472U,153130U},
  {696U,63046U},
  {120U,1640U},
  {528U,83150U},
  {176U,87874U},
  {608U,175272U},
  {704U,81437U},
  {496U,282U},
  {364U,41464U},
  {368U,307U},
  {248U,7809U},
  {76U,5569U},
  {464U,60701U},
  {532U,9112U},
  {356U,31054U},
  {352U,6267U},
  {668U,44391U},
  {20U,29481U},
  {140U,67530U},
  {664U,101212U},
  {432U,99981U},
  {576U,55065U},
  {560U,92845U},
  {224U,87207U},
  {28U,2266U},
  {660U,47176U},
  {16U,1591U},
  {112U,75586U},
  {512U,5469U},
  {636U,27673U},
  {208U,94435U},
  {388U,59745U},
  {380U,80920U},
  {524U,46420U},
  {324U,79077U},
  {520U,34414U},
  {440U,86895U},
  {540U,56804U},
  {572U,101421U},
  {376U,38312U},
  {604U,135212U},
  {504U,115774U},
  {8U,12926U},
  {552U,57941U},
  {736U,116311U},
  {212U,6035U},
  {544U,56630U},
  {0U,5219U},
  {428U,30607U},
  {680U,111620U},
  {632U,100026U},
  {144U,28889U},
  {104U,38947U},
  {284U,30497U},
  {88U,12150U},
  {4U,854U},
  {72U,23195U},
  {40U,2584U},
  {152U,68647U},
  {500U,76547U},
  {404U,49696U},
  {100U,9660U},
  {344U,38589U},
  {268U,35695U},
  {328U,24899U},
  {416U,133784U},
  {656U,51873U},
  {732U,108722U},
  {600U,70964U},
  {108U,307U},
  {384U,15993U},
  {48U,2410U},
  {228U,220U},
  {252U,53581U},
  {312U,60158U},
  {120U,61157U},
  {396U,1390U},
  {612U,175603U},
  {36U,6232U},
  {136U,1405U},
  {52U,484U},
  {264U,1860U},
  {232U,5977U},
  {672U,185475U},
  {44U,5937U},
  {460U,6571U},
  {444U,20625U},
  {640U,64400U},
  {624U,135162U},
  {628U,22800U},
  {300U,33817U},
  {96U,4534U},
  {84U,3901U},
  {260U,48847U},
  {492U,3584U},
  {652U,166201U},
  {308U,44167U},
  {400U,50594U},
  {92U,8032U},
  {160U,4476U},
  {272U,25166U},
  {132U,10199U},
  {292U,2186U},
  {480U,83238U},
  {620U,110074U},
  {748U,185151U},
  {592U,135131U},
  {508U,138444U},
  {332U,73368U},
  {424U,3287U},
  {588U,172849U},
  {172U,1762U},
  {301U,7836U},
  {557U,53459U},
  {709U,142708U},
  {449U,1927U},
  {701U,144309U},
  {29U,1185U},
  {33U,596U},
  {477U,132630U},
  {165U,5447U},
  {361U,8387U},
  {229U,104672U},
  {609U,99041U},
  {405U,96021U},
  {529U,83142U},
  {545U,10121U},
  {729U,107452U},
  {697U,63832U},
  {13U,68497U},
  {185U,2169U},
  {625U,91117U},
  {157U,22126U},
  {593U,104131U},
  {249U,34292U},
  {433U,42851U},
  {613U,137143U},
  {665U,108242U},
  {501U,47881U},
  {309U,41105U},
  {137U,40613U},
  {417U,104802U},
  {597U,52406U},
  {293U,458U},
  {53U,59069U},
  {377U,21367U},
  {661U,184050U},
  {149U,41413U},
  {621U,39763U},
  {469U,30597U},
  {41U,341U},
  {101U,21337U},
  {241U,4771U},
  {73U,6439U},
  {385U,141075U},
  {673U,47359U},
  {457U,48263U},
  {677U,143210U},
  {537U,10947U},
  {161U,10483U},
  {181U,8237U},
  {629U,72774U},
  {353U,4608U},
  {205U,12803U},
  {689U,153049U},
  {445U,91552U},
  {129U,13594U},
  {577U,117151U},
  {289U,6800U},
  {489U,9009U},
  {617U,176163U},
  {733U,109611U},
  {345U,4936U},
  {201U,4081U},
  {481U,27055U},
  {257U,3550U},
  {497U,7493U},
  {173U,4730U},
  {217U,35191U},
  {341U,38855U},
  {21U,30897U},
  {645U,45086U},
  {429U,101162U},
  {581U,54694U},
  {681U,175524U},
  {569U,170495U},
  {221U,47203U},
  {65U,1799U},
  {425U,22280U},
  {233U,41264U},
  {357U,18612U},
  {461U,31750U},
  {521U,6672U},
  {49U,23839U},
  {125U,18420U},
  {121U,1717U},
  {549U,51855U},
  {225U,55733U},
  {373U,35831U},
  {721U,137145U},
  {637U,72413U},
  {141U,60146U},
  {265U,5860U},
  {325U,33328U},
  {561U,28983U},
  {273U,3651U},
  {509U,75935U},
  {513U,74228U},
  {705U,136578U},
  {5U,5498U},
  {113U,161U},
  {81U,3970U},
  {453U,2023U},
  {713U,130147U},
  {317U,125454U},
  {473U,16447U},
  {153U,1945U},
  {221U,21363U},
  {725U,104943U},
  {177U,6080U},
  {261U,12073U},
  {485U,65322U},
  {337U,129695U},
  {313U,50779U},
  {741U,194913U},
  {565U,100764U},
  {245U,113168U},
  {25U,33193U},
  {657U,103609U},
  {97U,4671U},
  {93U,68343U},
  {37U,20262U},
  {237U,1412U},
  {693U,162149U},
  {505U,36375U},
  {45U,9253U},
  {525U,62713U},
  {401U,13783U},
  {422U,92376U},
  {398U,7664U},
  {698U,106802U},
  {50U,58542U},
  {78U,29862U},
  {98U,40967U},
  {590U,173157U},
  {538U,86891U},
  {654U,64387U},
  {430U,68051U},
  {494U,54712U},
  {570U,106533U},
  {606U,98562U},
  {146U,12293U},
  {42U,5897U},
  {526U,161598U},
  {58U,10912U},
  {550U,112338U},
  {182U,563U},
  {290U,450U},
  {114U,3675U},
  {594U,103629U},
  {690U,59582U},
  {490U,6529U},
  {582U,136781U},
  {298U,61227U},
  {542U,16724U},
  {62U,568U},
  {358U,6965U},
  {350U,110614U},
  {554U,166999U},
  {302U,61859U},
  {562U,12953U},
  {678U,143209U},
  {186U,86662U},
  {122U,38806U},
  {402U,95799U},
  {98U,40650U},
  {598U,123398U},
  {206U,12766U},
  {426U,78062U},
  {518U,53977U},
  {630U,178651U},
  {438U,87606U},
  {106U,30U},
  {326U,127396U},
  {650U,34692U},
  {622U,145913U},
  {158U,1795U},
  {170U,67383U},
  {738U,194325U},
  {534U,43243U},
  {726U,105615U},
  {378U,139138U},
  {278U,2186U},
  {30U,112U},
  {506U,121555U},
  {462U,30946U},
  {66U,1542U},
  {394U,50810U},
  {306U,459U},
  {46U,28665U},
  {286U,1784U},
  {314U,99684U},
  {686U,150188U},
  {174U,13307U},
  {446U,58019U},
  {730U,146445U},
  {502U,105030U},
  {218U,22253U},
  {250U,15288U},
  {10U,17440U},
  {466U,5897U},
  {142U,79185U},
  {386U,74746U},
  {510U,47678U},
  {22U,31574U},
  {310U,483U},
  {742U,140991U},
  {714U,91504U},
  {546U,103474U},
  {434U,16816U},
  {330U,44465U},
  {666U,172217U},
  {238U,57621U},
  {478U,122150U},
  {658U,111817U},
  {118U,23951U},
  {602U,17673U},
  {322U,30527U},
  {450U,54611U},
  {94U,4590U},
  {662U,161616U},
  {138U,11661U},
  {390U,23596U},
  {74U,67786U},
  {254U,79248U},
  {522U,22610U},
  {102U,6050U},
  {370U,31008U},
  {614U,91881U},
  {646U,33016U},
  {222U,63211U},
  {498U,73872U},
  {242U,15993U},
  {346U,11352U},
  {198U,23647U},
  {486U,65321U},
  {530U,64502U},
  {578U,96097U},
  {626U,21785U},
  {234U,107701U},
  {318U,505U},
  {670U,74117U},
  {706U,151577U},
  {710U,170370U},
  {718U,164537U},
  {334U,26391U},
  {70U,53545U},
  {214U,50698U},
  {382U,97282U},
  {566U,47135U},
  {159U,30592U},
  {155U,50225U},
  {607U,153729U},
  {635U,26579U},
  {679U,157093U},
  {407U,21612U},
  {83U,4023U},
  {87U,3958U},
  {311U,98087U},
  {567U,31603U},
  {355U,4608U},
  {711U,151450U},
  {179U,10405U},
  {539U,86878U},
  {111U,8684U},
  {163U,42187U},
  {119U,2290U},
  {527U,7738U},
  {667U,184313U},
  {683U,89675U},
  {571U,107463U},
  {203U,6818U},
  {475U,153517U},
  {359U,43024U},
  {15U,43694U},
  {27U,10666U},
  {19U,28898U},
  {391U,142659U},
  {675U,49111U},
  {323U,22157U},
  {363U,55419U},
  {463U,60713U},
  {227U,104449U},
  {595U,17428U},
  {623U,136666U},
  {663U,111730U},
  {455U,151116U},
  {387U,70698U},
  {171U,85361U},
  {371U,137983U},
  {451U,1985U},
  {243U,45343U},
  {467U,30445U},
  {531U,162451U},
  {707U,83527U},
  {559U,92857U},
  {739U,136365U},
  {383U,135813U},
  {715U,91689U},
  {283U,120143U},
  {139U,3553U},
  {583U,99243U},
  {339U,45217U},
  {659U,106103U},
  {343U,5964U},
  {43U,8784U},
  {167U,1677U},
  {563U,50758U},
  {207U,207U},
  {643U,104928U},
  {695U,150326U},
  {199U,23803U},
  {263U,12097U},
  {303U,54634U},
  {47U,7831U},
  {747U,181870U},
  {367U,85856U},
  {603U,174413U},
  {299U,27928U},
  {671U,45591U},
  {59U,27013U},
  {639U,73628U},
  {279U,64833U},
  {307U,60290U},
  {515U,70692U},
  {327U,15178U},
  {251U,46105U},
  {379U,22269U},
  {375U,125017U},
  {423U,60043U},
  {459U,53530U},
  {615U,175824U},
  {575U,31921U},
  {687U,150186U},
  {211U,9839U},
  {447U,58011U},
  {719U,98366U},
  {455U,113295U},
  {627U,59582U},
  {15U,5304U},
  {35U,49884U},
  {731U,108240U},
  {23U,1963U},
  {551U,166914U},
  {415U,90775U},
  {591U,88938U},
  {103U,24449U},
  {267U,4839U},
  {503U,121118U},
  {255U,114321U},
  {319U,34630U},
  {231U,105237U},
  {291U,93222U},
  {427U,58771U},
  {75U,25602U},
  {15U,57360U},
  {443U,307U},
  {195U,6340U},
  {55U,59520U},
  {523U,161056U},
  {703U,80279U},
  {131U,4795U},
  {555U,108543U},
  {399U,50605U},
  {51U,7515U},
  {507U,68837U},
  {271U,25169U},
  {191U,39375U},
  {599U,174068U},
  {59U,29140U},
  {619U,110244U},
  {651U,37829U},
  {403U,31860U},
  {611U,18857U},
  {99U,4604U},
  {495U,4521U},
  {275U,37056U},
  {175U,87585U},
  {543U,10086U},
  {723U,102639U},
  {471U,81297U},
};
static const unsigned graph_base_offsets[5] = {0,133,259,381,512};
static const struct GraphEdge graph_updates[256] = {
  {80U,3156024U},
  {700U,6359489U},
  {188U,7340210U},
  {280U,9529693U},
  {68U,12606040U},
  {256U,12585885U},
  {360U,14738098U},
  {448U,14721560U},
  {200U,15734721U},
  {688U,17915234U},
  {408U,19924374U},
  {584U,21125442U},
  {196U,25177173U},
  {728U,31598954U},
  {32U,34611877U},
  {304U,35680359U},
  {580U,38928971U},
  {288U,39967140U},
  {128U,41949035U},
  {164U,45089188U},
  {340U,53498332U},
  {60U,55596812U},
  {616U,58740017U},
  {24U,59773412U},
  {420U,61919026U},
  {412U,64024212U},
  {192U,66060492U},
  {336U,67168417U},
  {452U,68159434U},
  {684U,71452196U},
  {184U,74488475U},
  {516U,75657293U},
  {484U,76610996U},
  {488U,80762396U},
  {320U,82838009U},
  {236U,84971926U},
  {204U,87106602U},
  {568U,89299220U},
  {348U,91265655U},
  {744U,93517138U},
  {56U,98570245U},
  {476U,102817207U},
  {148U,105913237U},
  {12U,108003671U},
  {220U,109152858U},
  {116U,110100520U},
  {648U,112324997U},
  {216U,113246515U},
  {708U,115529136U},
  {556U,118656285U},
  {716U,118581195U},
  {392U,119550773U},
  {468U,120635740U},
  {240U,124845403U},
  {644U,126909881U},
  {372U,132258594U},
  {589U,2122235U},
  {493U,6447179U},
  {21U,8418761U},
  {297U,8389067U},
  {109U,10490130U},
  {413U,11586076U},
  {61U,13636430U},
  {105U,13633737U},
  {133U,15740747U},
  {517U,22026324U},
  {365U,23121776U},
  {421U,23111755U},
  {305U,24133022U},
  {641U,27303805U},
  {329U,28358094U},
  {381U,28347248U},
  {189U,33560432U},
  {169U,34603872U},
  {213U,36711681U},
  {745U,37926599U},
  {653U,38898295U},
  {285U,40923957U},
  {553U,46260205U},
  {69U,49349838U},
  {177U,51386192U},
  {601U,52446448U},
  {9U,54529643U},
  {77U,54526161U},
  {253U,55576755U},
  {333U,56635256U},
  {193U,60828119U},
  {145U,62957210U},
  {465U,64014859U},
  {685U,71452191U},
  {57U,73404751U},
  {349U,74459768U},
  {541U,77619032U},
  {573U,78742673U},
  {145U,81845265U},
  {437U,81795737U},
  {25U,82871236U},
  {397U,83940774U},
  {429U,83936121U},
  {441U,85985857U},
  {533U,88166251U},
  {717U,89227066U},
  {197U,90183584U},
  {389U,92317820U},
  {209U,94388751U},
  {737U,96587704U},
  {269U,97528523U},
  {89U,98568210U},
  {101U,99641986U},
  {281U,99619491U},
  {1U,100668558U},
  {85U,101763072U},
  {17U,108003584U},
  {605U,111323751U},
  {649U,112324930U},
  {393U,120647759U},
  {321U,121678569U},
  {369U,121668621U},
  {633U,122809723U},
  {409U,125843246U},
  {117U,127933447U},
  {585U,130196176U},
  {277U,131078845U},
  {669U,132165012U},
  {34U,49869U},
  {682U,1103467U},
  {270U,2200044U},
  {178U,3149490U},
  {342U,5293795U},
  {406U,5281638U},
  {6U,7351191U},
  {558U,9464399U},
  {90U,10491091U},
  {414U,11586060U},
  {110U,16799193U},
  {246U,16782611U},
  {26U,18874436U},
  {38U,19974027U},
  {190U,25177595U},
  {354U,27385305U},
  {118U,29396408U},
  {458U,30560044U},
  {674U,31632114U},
  {474U,32557062U},
  {194U,36713850U},
  {638U,39874692U},
  {294U,40922404U},
  {126U,41949053U},
  {418U,42993138U},
  {514U,44056796U},
  {18U,45097375U},
  {642U,46228909U},
  {154U,47186175U},
  {230U,52533964U},
  {202U,56660019U},
  {374U,57673950U},
  {362U,62921369U},
  {86U,65081530U},
  {454U,67146398U},
  {54U,68216607U},
  {282U,69322753U},
  {14U,70277910U},
  {258U,70254895U},
  {442U,72407673U},
  {702U,75575017U},
  {266U,77681653U},
  {574U,78742664U},
  {618U,79868019U},
  {734U,79801561U},
  {130U,90189550U},
  {366U,92323168U},
  {746U,93512566U},
  {226U,94384881U},
  {634U,96648371U},
  {410U,101715853U},
  {262U,103847221U},
  {338U,103832797U},
  {586U,104960065U},
  {150U,105913044U},
  {610U,107033459U},
  {2U,110106109U},
  {274U,114334842U},
  {14U,116415270U},
  {82U,117449268U},
  {166U,117444442U},
  {210U,119587145U},
  {482U,124801591U},
  {470U,129127686U},
  {694U,129037488U},
  {722U,130125878U},
  {134U,133177509U},
  {162U,133175262U},
  {395U,1388U},
  {431U,1195705U},
  {15U,4328046U},
  {655U,4194611U},
  {547U,17976029U},
  {3U,18874879U},
  {699U,21073955U},
  {135U,22098143U},
  {219U,24148056U},
  {75U,26278807U},
  {479U,26218051U},
  {331U,29365988U},
  {691U,30468571U},
  {411U,32569444U},
  {127U,33565621U},
  {287U,35682319U},
  {735U,37941332U},
  {39U,43044479U},
  {223U,44116341U},
  {11U,47193527U},
  {71U,48302141U},
  {491U,48238020U},
  {487U,49286252U},
  {79U,50337247U},
  {115U,50335398U},
  {123U,51390423U},
  {239U,53515915U},
  {63U,57716342U},
  {235U,58829641U},
  {95U,59769329U},
  {183U,60828835U},
  {419U,61919030U},
  {499U,65015695U},
  {7U,66071658U},
  {631U,69233602U},
  {435U,72409083U},
  {91U,73402486U},
  {483U,76611003U},
  {247U,80807429U},
  {335U,84954489U},
  {67U,86040254U},
  {511U,87044116U},
  {535U,88166223U},
  {347U,91265659U},
  {31U,95424067U},
  {75U,95421275U},
  {187U,97542632U},
  {107U,100663328U},
  {439U,102824479U},
  {587U,104960062U},
  {519U,107062429U},
  {579U,109067821U},
  {727U,111255183U},
  {15U,113260883U},
  {351U,114320715U},
  {743U,115505912U},
  {259U,116392239U},
  {647U,122800920U},
  {143U,123761481U},
  {295U,123738734U},
  {215U,125880224U},
  {315U,127002149U},
  {151U,127931949U},
  {147U,131098974U},
};
static const unsigned graph_updates_offsets[5] = {0,56,124,192,256};
static const unsigned graph_expected_sums[5][NR_TASKLETS] = {
  {2237034884U,4085678662U,3197858807U,3164553153U},
  {1174285197U,156311626U,2414938325U,2230330035U},
  {1900349192U,2714032278U,2510906817U,2826364923U},
  {3548885770U,3225528433U,2690247899U,2230330035U},
  {1340520940U,248486883U,912900798U,2015207557U},
};
static const unsigned graph_expected_counts[5][NR_TASKLETS] = {
  {133U,126U,122U,131U},
  {136U,128U,129U,135U},
  {189U,194U,190U,195U},
  {142U,135U,132U,135U},
  {160U,159U,156U,165U},
};


__host int mallocableRegionSize, pageSize, bufferSize;
__mram_noinit struct GraphVertex graph_vertex_storage[GRAPH_VERTICES];
__mram_noinit uint64_t graph_base_applied[GRAPH_BASE_ARCS];
__mram_noinit uint64_t graph_update_applied[GRAPH_UPDATE_ARCS];
struct Snapshot { unsigned phase, round, requested, slots, reserved, empty_bytes; };
static struct Snapshot snapshots[8 * ROUNDS + 2];
static unsigned snapshot_count;
static volatile unsigned policy_report_meta[5];
BARRIER_INIT(policy_barrier, NR_TASKLETS);

static void applied_write(__mram_ptr uint64_t *address, unsigned value) {
  uint32_t cell[2] __attribute__((aligned(8))) = {value, 0};
  mram_write(cell, address, 8);
}
static unsigned applied_read(__mram_ptr uint64_t *address) {
  uint32_t cell[2] __attribute__((aligned(8)));
  mram_read(address, cell, 8);
  return cell[0];
}
static void snapshot(unsigned phase, unsigned round) {
  struct Snapshot *s = &snapshots[snapshot_count++];
  assert(snapshot_count <= 8 * ROUNDS + 2);
  s->phase = phase; s->round = round;
  s->reserved = policy_backend.used;
  s->requested = s->slots = s->empty_bytes = 0;
  for (unsigned t = 0; t < NR_TASKLETS; ++t) {
    s->requested += benchmark_live[t];
    s->slots += policy_stats[t].live_slots;
    s->empty_bytes += policy_stats[t].retained * POLICY_PAGE;
  }
}

/* Keep a zero-instruction end label inside a function only tasklet 0 calls.
 * At a conditional branch join the same PC could also be reached by others.
 */
static __attribute__((noinline)) void graph_init_end(void) {
  snapshot(0, 0);
  END(init);
}

static __attribute__((noinline)) void graph_final_report(void) {
  END(total);
  assert(policy_backend.used == 0 && getMetadata(policy_buddy, 0, 0) == 3);
  for (unsigned t = 0; t < NR_TASKLETS; ++t) policy_verify(t);
  snapshot(99, 0);
  policy_report_meta[0] = (unsigned)__HEAP_POINTER;
  policy_report_meta[1] = sizeof(policy_caches);
  policy_report_meta[2] = sizeof(policy_stats);
  policy_report_meta[3] = snapshot_count;
  policy_report_meta[4] = NR_TASKLETS;
  __asm__ volatile("policy_report_ready_%=:" :: "r"(policy_stats), "r"(&policy_backend),
        "r"(snapshots), "r"(policy_report_meta) : "memory");
}

int main(void) {
  unsigned tid = me();
  if (tid == 0) {
    mem_reset(); BEGIN(total); BEGIN(init);
    policy_init(HEAP_BYTES);
    graph_vertices = graph_vertex_storage;
  }
  barrier_wait(&policy_barrier);
  for (unsigned u = tid; u < GRAPH_VERTICES; u += NR_TASKLETS) {
    struct GraphVertex vertex __attribute__((aligned(8))) = {0, 0};
    mram_write(&vertex, &graph_vertices[u], 8);
  }
  for (unsigned i = graph_base_offsets[tid]; i < graph_base_offsets[tid + 1]; ++i)
    applied_write(&graph_base_applied[i], graph_insert(graph_base[i].source, graph_base[i].destination));
  graph_check(GRAPH_VERTICES);
  if (!policy_stats[tid].failures) {
    assert(graph_live_sum[tid] == graph_expected_sums[0][tid]);
    assert(graph_live_edges[tid] == graph_expected_counts[0][tid]);
  }
  barrier_wait(&policy_barrier);
  if (tid == 0) graph_init_end();
  barrier_wait(&policy_barrier);

  for (unsigned round = 0; round < ROUNDS; ++round) {
    for (unsigned phase = 0; phase < 4; ++phase) {
      BEGIN(work);
      for (unsigned i = graph_updates_offsets[tid]; i < graph_updates_offsets[tid + 1]; ++i) {
        unsigned packed = graph_updates[i].destination;
        if (packed >> 20 >= graph_limits[phase]) continue;
        applied_write(&graph_update_applied[i], graph_insert(graph_updates[i].source, packed & 0xfffff));
      }
      barrier_wait(&policy_barrier);
      if (tid == 0) snapshot(2 * phase + 1, round);
      barrier_wait(&policy_barrier);
      unsigned read_passes = phase % 2 ? 1 : 4;
      for (unsigned pass = 0; pass < read_passes; ++pass) graph_check(GRAPH_VERTICES);
      if (!policy_stats[tid].failures) {
        assert(graph_live_sum[tid] == graph_expected_sums[phase + 1][tid]);
        assert(graph_live_edges[tid] == graph_expected_counts[phase + 1][tid]);
      }
      for (unsigned i = graph_updates_offsets[tid + 1]; i > graph_updates_offsets[tid]; ) {
        --i;
        unsigned packed = graph_updates[i].destination;
        if (packed >> 20 < graph_limits[phase] && applied_read(&graph_update_applied[i]))
          graph_remove_last(graph_updates[i].source, packed & 0xfffff);
      }
      graph_check(GRAPH_VERTICES);
      barrier_wait(&policy_barrier);
      if (VALIDATE) policy_verify(tid);
      barrier_wait(&policy_barrier);
      if (tid == 0) snapshot(2 * phase + 2, round);
      barrier_wait(&policy_barrier);
      END(work);
    }
  }
  BEGIN(drain);
  for (unsigned i = graph_base_offsets[tid + 1]; i > graph_base_offsets[tid]; ) {
    --i;
    if (applied_read(&graph_base_applied[i]))
      graph_remove_last(graph_base[i].source, graph_base[i].destination);
  }
  assert(graph_live_edges[tid] == 0 && graph_live_sum[tid] == 0 && benchmark_live[tid] == 0);
  policy_drain(tid);
  barrier_wait(&policy_barrier);
  END(drain);
  if (tid == 0) graph_final_report();
  return 0;
}
