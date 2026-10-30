#define NR_TASKLETS 1
#define POLICY 3
#define WORKLOAD 4
#define BURST 32
#define ROUNDS 1
#define HEAP_BYTES 524288
#define KEEP_PAGES 1
#define MAX_KEEP 2
#define EMPTY_BUDGET 4
#define DECAY_OPS 64
#define PRESSURE_PERCENT 75
#define VALIDATE 1
#define GRAPH_BLOCK_BYTES 256
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

#define GRAPH_VERTICES 192
#define GRAPH_NEW_EDGES 32
#define GRAPH_BASE_ARCS 128
#define GRAPH_UPDATE_ARCS 64
struct GraphEdge { unsigned source, destination; };
static const unsigned graph_limits[4] = {2,32,4,16};
static const struct GraphEdge graph_base[128] = {
  {174U,170761U},
  {186U,135786U},
  {149U,165717U},
  {183U,89378U},
  {110U,61307U},
  {123U,51842U},
  {74U,68052U},
  {135U,27628U},
  {120U,91559U},
  {151U,57941U},
  {64U,30363U},
  {77U,21539U},
  {127U,106272U},
  {162U,62577U},
  {154U,170206U},
  {185U,98010U},
  {1U,4361U},
  {23U,157U},
  {61U,82523U},
  {146U,18808U},
  {125U,62510U},
  {126U,62503U},
  {104U,55240U},
  {116U,49660U},
  {57U,55452U},
  {117U,15120U},
  {49U,86107U},
  {147U,10687U},
  {51U,13426U},
  {56U,11304U},
  {118U,148751U},
  {178U,56600U},
  {139U,74047U},
  {140U,74044U},
  {46U,50451U},
  {106U,9804U},
  {12U,19942U},
  {63U,1511U},
  {113U,110639U},
  {166U,53286U},
  {84U,64195U},
  {129U,31578U},
  {71U,52430U},
  {111U,24878U},
  {98U,64937U},
  {133U,44365U},
  {78U,57822U},
  {119U,30410U},
  {175U,168602U},
  {184U,136520U},
  {155U,112615U},
  {169U,98874U},
  {92U,99364U},
  {157U,38573U},
  {66U,113731U},
  {170U,22232U},
  {107U,112554U},
  {168U,50718U},
  {34U,78336U},
  {144U,6325U},
  {109U,106418U},
  {163U,51102U},
  {21U,29737U},
  {76U,3979U},
  {37U,10130U},
  {47U,6807U},
  {13U,24281U},
  {70U,1638U},
  {101U,77418U},
  {143U,47612U},
  {80U,70870U},
  {137U,30561U},
  {179U,177469U},
  {189U,149692U},
  {11U,49973U},
  {105U,1394U},
  {138U,141050U},
  {176U,73875U},
  {86U,125742U},
  {172U,33267U},
  {19U,5442U},
  {29U,3738U},
  {145U,161470U},
  {181U,81133U},
  {165U,155669U},
  {180U,109865U},
  {60U,35829U},
  {90U,16544U},
  {141U,77290U},
  {142U,77288U},
  {9U,35000U},
  {89U,592U},
  {14U,52518U},
  {112U,1939U},
  {75U,68367U},
  {136U,27882U},
  {28U,7578U},
  {43U,5438U},
  {58U,100788U},
  {158U,15844U},
  {62U,40999U},
  {94U,19652U},
  {188U,193394U},
  {191U,176883U},
  {72U,33743U},
  {87U,26517U},
  {156U,171097U},
  {187U,99329U},
  {16U,64893U},
  {132U,2580U},
  {17U,4711U},
  {25U,3237U},
  {44U,30652U},
  {81U,7647U},
  {82U,60442U},
  {122U,31018U},
  {160U,104892U},
  {161U,104881U},
  {0U,6873U},
  {39U,67U},
  {3U,6137U},
  {33U,203U},
  {93U,42272U},
  {95U,39742U},
  {97U,53816U},
  {114U,43060U},
  {79U,61373U},
  {124U,30547U},
};
static const unsigned graph_base_offsets[2] = {0,128};
static const struct GraphEdge graph_updates[64] = {
  {45U,34914U},
  {88U,8979U},
  {6U,1071839U},
  {68U,1048873U},
  {182U,2287899U},
  {190U,2261310U},
  {10U,3184143U},
  {91U,3146582U},
  {20U,4201509U},
  {42U,4198226U},
  {31U,5287308U},
  {99U,5248835U},
  {50U,6377786U},
  {148U,6302754U},
  {18U,7407600U},
  {134U,7343523U},
  {85U,8513984U},
  {171U,8421542U},
  {38U,9448678U},
  {52U,9444013U},
  {96U,10596489U},
  {167U,10528577U},
  {2U,11539142U},
  {27U,11534514U},
  {8U,12586992U},
  {22U,12583219U},
  {53U,13723918U},
  {153U,13643515U},
  {130U,14812644U},
  {173U,14744451U},
  {115U,15875452U},
  {177U,15783664U},
  {24U,16787448U},
  {48U,16781815U},
  {73U,17885261U},
  {121U,17852937U},
  {35U,18938961U},
  {131U,18880875U},
  {100U,19986381U},
  {128U,19970010U},
  {5U,20994306U},
  {67U,20971796U},
  {41U,22051239U},
  {83U,22027055U},
  {102U,23118058U},
  {103U,23118057U},
  {36U,24141213U},
  {69U,24124009U},
  {26U,25178045U},
  {54U,25170607U},
  {152U,26322858U},
  {164U,26306336U},
  {4U,27275946U},
  {55U,27263183U},
  {65U,28362390U},
  {108U,28333237U},
  {150U,29464413U},
  {159U,29449756U},
  {7U,30414735U},
  {32U,30409002U},
  {30U,31464237U},
  {40U,31463140U},
  {15U,32521776U},
  {59U,32507917U},
};
static const unsigned graph_updates_offsets[2] = {0,64};
static const unsigned graph_expected_sums[5][NR_TASKLETS] = {
  {4137197553U},
  {2328033063U},
  {2466001779U},
  {2443376032U},
  {3092199008U},
};
static const unsigned graph_expected_counts[5][NR_TASKLETS] = {
  {128U},
  {132U},
  {192U},
  {136U},
  {160U},
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
