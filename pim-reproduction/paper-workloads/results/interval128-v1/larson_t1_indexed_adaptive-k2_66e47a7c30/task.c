#define NR_TASKLETS 1
#define POLICY 3
#define LISTS 1
#define LOOKUP 1
#define HEAP_BYTES 33554432
#define KEEP_PAGES 2
#define MAX_KEEP 2
#define EMPTY_BUDGET 4
#define DECAY_OPS 128
#define PRESSURE_PERCENT 75
#define VALIDATE 0
#define POOL_SIZE 512
#define INPUT_ITEMS 32768
#define TRACE_SEED 9173
#define REPEATS 2
#define PAPER_KIND 0
/* Owner-local Larson worker port. Original sizes, random victim selection,
 * Fisher-Yates preconditioning and 4*N warmup; bounded work replaces wall time.
 */
/* Idea 1 + Idea 3. Same-tasklet frees, original 4 KiB/24 B page geometry.
 * LISTS and LOOKUP are independent ablations. The unoptimized case includes
 * the maintained baseline verbatim. No live object is moved.
 */
#ifndef LISTS
#define LISTS 1
#endif
#ifndef LOOKUP
#define LOOKUP 1
#endif
#if !LISTS && !LOOKUP
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

#else
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
#define KEEP_PAGES 2
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
#if LISTS
  __mram_ptr void *empty_root, *full_root;
  unsigned full_bitmap[4];
#endif
};
/* Preserve the passive reporter's 18-word stride. */
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
#if LOOKUP
/* One independently writable, DMA-aligned entry per payload page in MRAM.
 * tag=0 unallocated; 1+tasklet*8+class small; high bit + tasklet large.
 * For a large allocation only its head is registered: free accepts bases,
 * never interior pointers. Other entries are cleared before buddy reuse.
 */
struct PolicyOwner { unsigned tag, bytes; };
static __mram_ptr struct PolicyOwner *policy_owners;
static unsigned policy_owner_bytes;
static unsigned policy_page_index(__mram_ptr void *p) {
  unsigned offset = p - policy_buddy->mramMallocableRegionBase;
  assert(offset < policy_heap_size);
  return offset >> 12;
}
static void policy_owner_write(__mram_ptr void *p, unsigned tag, unsigned bytes) {
  struct PolicyOwner info __attribute__((aligned(8))) = {tag, bytes};
  mram_write(&info, &policy_owners[policy_page_index(p)], 8);
}
#endif

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
static void policy_links(__mram_ptr void *p, uint32_t links[2]) { mram_read(p, links, 8); }
static int policy_empty(unsigned b[4]) {
  return b[0] == ~0U && b[1] == ~0U && b[2] == ~0U && b[3] == ~0U;
}
static unsigned policy_capacity(struct PolicyCache *c) {
  unsigned n = (POLICY_PAGE - 24) / c->size;
  return n < 128 ? n : 128;
}
#if LISTS
/* Invalid bitmap bits stay one in the legacy layout. Compare only valid bits.
 * No per-object header or change in page capacity is introduced.
 */
static int policy_full(struct PolicyCache *c, unsigned b[4]) {
  return b[0] == c->full_bitmap[0] && b[1] == c->full_bitmap[1] &&
         b[2] == c->full_bitmap[2] && b[3] == c->full_bitmap[3];
}
/* State: 0 partial, 1 empty, 2 full. Each page is in exactly one list. */
static __mram_ptr void **policy_head(struct PolicyCache *c, unsigned state) {
  return state == 1 ? &c->empty_root : state == 2 ? &c->full_root : &c->root;
}
#endif
static void policy_push(struct PolicyCache *c, __mram_ptr void *p, unsigned state) {
  __mram_ptr void **head = &c->root;
#if LISTS
  head = policy_head(c, state);
#endif
  uint32_t links[2] __attribute__((aligned(8))) = {0, (uint32_t)*head};
  if (*head) {
    uint32_t old[2] __attribute__((aligned(8)));
    policy_links(*head, old); old[0] = (uint32_t)p;
    mram_write(old, *head, 8);
  }
  mram_write(links, p, 8); *head = p;
}
static void policy_unlink(struct PolicyCache *c, __mram_ptr void *p, unsigned state) {
  __mram_ptr void **head = &c->root;
#if LISTS
  head = policy_head(c, state);
#endif
  uint32_t links[2] __attribute__((aligned(8)));
  policy_links(p, links);
  if (links[0]) {
    uint32_t prev[2] __attribute__((aligned(8)));
    policy_links((__mram_ptr void *)links[0], prev); prev[1] = links[1];
    mram_write(prev, (__mram_ptr void *)links[0], 8);
  } else { assert(*head == p); *head = (__mram_ptr void *)links[1]; }
  if (links[1]) {
    uint32_t next[2] __attribute__((aligned(8)));
    policy_links((__mram_ptr void *)links[1], next); next[0] = links[0];
    mram_write(next, (__mram_ptr void *)links[1], 8);
  }
}
static void policy_add_page(struct PolicyCache *c, __mram_ptr void *p, unsigned tid) {
  unsigned bitmap[4] __attribute__((aligned(8))) = {~0U, ~0U, ~0U, ~0U};
  policy_push(c, p, 1); mram_write(bitmap, p + 8, 16);
#if LOOKUP
  policy_owner_write(p, 1 + tid * POLICY_CLASSES + (c - policy_caches[tid]), 0);
#endif
  c->pages++; c->empty++; policy_stats[tid].retained++;
}
static void policy_release_page(struct PolicyCache *c, __mram_ptr void *p, unsigned tid) {
  policy_unlink(c, p, 1);
  assert(c->empty && c->pages && policy_stats[tid].retained);
  c->empty--; c->pages--; policy_stats[tid].retained--; policy_stats[tid].returns++;
#if LOOKUP
  policy_owner_write(p, 0, 0);
#endif
  assert(policy_backend_free(p) == POLICY_PAGE);
}
static unsigned policy_trim_cache(struct PolicyCache *c, unsigned keep, unsigned tid) {
  unsigned removed = 0;
#if LISTS
  while (c->empty > keep) {
    assert(c->empty_root); policy_release_page(c, c->empty_root, tid); removed++;
  }
#else
  __mram_ptr void *page = c->root;
  while (page && c->empty > keep) {
    uint32_t links[2] __attribute__((aligned(8)));
    unsigned bitmap[4] __attribute__((aligned(8)));
    policy_links(page, links); mram_read(page + 8, bitmap, 16);
    if (policy_empty(bitmap)) { policy_release_page(c, page, tid); removed++; }
    page = (__mram_ptr void *)links[1];
  }
#endif
  assert(c->empty <= keep); return removed;
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
      s->idle_trims += policy_trim_cache(c, c->target, tid); c->refill_votes = 0;
    }
    c->last_activity = c->activity;
  }
}
static __attribute__((noinline)) void policy_drain(unsigned tid) {
  for (unsigned i = 0; i < POLICY_CLASSES; ++i) policy_trim_cache(&policy_caches[tid][i], 0, tid);
}
static __attribute__((noinline)) void policy_init(unsigned heap_size) {
  assert(heap_size >= POLICY_PAGE && isPowOf2(heap_size));
  assert(DECAY_OPS && !(DECAY_OPS & (DECAY_OPS - 1)));
  assert(PRESSURE_PERCENT > 0 && PRESSURE_PERCENT <= 100);
  memset(policy_caches, 0, sizeof(policy_caches)); memset(policy_stats, 0, sizeof(policy_stats));
  memset(&policy_backend, 0, sizeof(policy_backend)); policy_heap_size = heap_size;
  policy_backend.pressure_limit = (heap_size / 100) * PRESSURE_PERCENT;
  __mram_ptr void *backend_base = DPU_MRAM_HEAP_POINTER;
#if LOOKUP
  policy_owner_bytes = (heap_size >> 12) * 8;
  policy_owners = (__mram_ptr struct PolicyOwner *)backend_base;
  unsigned zero[64] __attribute__((aligned(8))) = {0};
  for (unsigned offset = 0; offset < policy_owner_bytes; offset += sizeof(zero)) {
    unsigned bytes = policy_owner_bytes - offset;
    if (bytes > sizeof(zero)) bytes = sizeof(zero);
    mram_write(zero, backend_base + offset, bytes);
  }
  backend_base += policy_owner_bytes;
#endif
  policy_buddy = initBuddyAllocator(heap_size, POLICY_PAGE, 64, backend_base);
  for (unsigned t = 0; t < NR_TASKLETS; ++t) for (unsigned i = 0; i < POLICY_CLASSES; ++i) {
    struct PolicyCache *c = &policy_caches[t][i];
    c->size = 16U << i; c->target = MAX_KEEP ? 1 : 0;
#if LISTS
    unsigned remaining = policy_capacity(c);
    for (int w = 3; w >= 0; --w) {
      unsigned bits = remaining < 32 ? remaining : 32;
      c->full_bitmap[w] = bits == 32 ? 0 : bits ? ~(~0U << (32 - bits)) : ~0U;
      remaining -= bits;
    }
#endif
    if (POLICY == 0) {
      __mram_ptr void *p = policy_backend_alloc(POLICY_PAGE);
      if (p) policy_add_page(c, p, t); else policy_stats[t].failures++;
    }
  }
}
static __attribute__((noinline)) __mram_ptr void *policy_malloc(unsigned size) {
  unsigned tid = me(); struct PolicyStats *s = &policy_stats[tid];
  s->alloc_calls++; policy_tick(tid);
  if (!size || size > policy_heap_size) { s->failures++; return NULL; }
  unsigned class_index = 0;
  while (class_index < POLICY_CLASSES && size > (16U << class_index)) class_index++;
  if (class_index == POLICY_CLASSES) {
    __mram_ptr void *p = policy_backend_alloc(size);
    if (!p && POLICY == 3 && s->retained) {
      unsigned before = s->retained; policy_drain(tid); s->emergency_trims += before;
      p = policy_backend_alloc(size);
    }
    if (p) {
#if LOOKUP
      policy_owner_write(p, 0x80000000U | tid, policy_block_size(size));
#endif
      s->successes++; s->live_slots += policy_block_size(size);
      if (s->live_slots > s->peak_slots) s->peak_slots = s->live_slots;
    } else s->failures++;
    return p;
  }
  struct PolicyCache *c = &policy_caches[tid][class_index]; c->activity++;
  __mram_ptr void *page = c->root;
#if LISTS
  if (!page) page = c->empty_root;
#endif
  unsigned bitmap[4] __attribute__((aligned(8)));
  int refill = 0;
  for (;;) {
    while (page) {
      mram_read(page + 8, bitmap, 16);
      for (int w = 3; w >= 0; --w) {
        if (!bitmap[w]) continue;
        unsigned bit = __builtin_clz(bitmap[w]), index = (3 - w) * 32 + bit;
        if (index >= policy_capacity(c)) continue;
        unsigned was_empty = policy_empty(bitmap);
        if (was_empty) {
          assert(c->empty && s->retained); c->empty--; s->retained--;
          if (!refill) s->retained_hits++;
        }
        bitmap[w] &= ~(1U << (31 - bit)); mram_write(bitmap, page + 8, 16);
#if LISTS
        unsigned full = policy_full(c, bitmap);
        if (was_empty || full) {
          policy_unlink(c, page, was_empty ? 1 : 0); policy_push(c, page, full ? 2 : 0);
        }
#endif
        if (!refill) s->cache_hits++;
        s->successes++; s->live_slots += c->size;
        if (s->live_slots > s->peak_slots) s->peak_slots = s->live_slots;
        return page + 24 + index * c->size;
      }
#if LISTS
      assert(0 && "available-page list contains a full page");
#else
      uint32_t links[2] __attribute__((aligned(8)));
      policy_links(page, links); page = (__mram_ptr void *)links[1];
#endif
    }
    page = policy_backend_alloc(POLICY_PAGE);
    if (!page && POLICY == 3 && s->retained) {
      unsigned before = s->retained; policy_drain(tid); s->emergency_trims += before;
      page = policy_backend_alloc(POLICY_PAGE);
    }
    if (!page) { s->failures++; return NULL; }
    policy_add_page(c, page, tid); s->refills++; refill = 1;
    if (POLICY == 3 && ++c->refill_votes >= 2) {
      c->refill_votes = 0;
      if (c->target < MAX_KEEP) { c->target++; s->grows++; }
    }
  }
}
static void policy_free_small(struct PolicyCache *c, __mram_ptr void *page, __mram_ptr void *p, unsigned tid) {
  struct PolicyStats *s = &policy_stats[tid];
  unsigned offset = p - page - 24;
  assert(p >= page + 24 && p < page + POLICY_PAGE && offset % c->size == 0);
  unsigned index = offset / c->size; assert(index < policy_capacity(c));
  unsigned bitmap[4] __attribute__((aligned(8)));
  mram_read(page + 8, bitmap, 16);
#if LISTS
  unsigned was_full = policy_full(c, bitmap);
#endif
  unsigned word = 3 - index / 32, bit = 31 - index % 32;
  assert(!(bitmap[word] & (1U << bit))); bitmap[word] |= 1U << bit;
  mram_write(bitmap, page + 8, 16);
  assert(s->live_slots >= c->size); s->live_slots -= c->size; c->activity++;
  unsigned empty = policy_empty(bitmap);
#if LISTS
  if (was_full || empty) {
    policy_unlink(c, page, was_full ? 2 : 0); policy_push(c, page, empty ? 1 : 0);
  }
#endif
  if (empty) {
    c->empty++; s->retained++;
    unsigned keep = POLICY == 2 ? KEEP_PAGES : (POLICY == 3 ? c->target : 0);
    unsigned pressure = POLICY == 3 && policy_backend.pressure;
    if (pressure || c->empty > keep || (POLICY != 0 && s->retained > EMPTY_BUDGET)) {
      if (pressure) s->pressure_trims++;
      else if (s->retained > EMPTY_BUDGET && POLICY >= 2) s->budget_trims++;
      policy_release_page(c, page, tid);
    }
  }
}
static __attribute__((noinline)) void policy_free(__mram_ptr void *p) {
  if (!p) return;
  unsigned tid = me(); struct PolicyStats *s = &policy_stats[tid];
  s->free_calls++; policy_tick(tid);
#if LOOKUP
  unsigned page_index = policy_page_index(p);
  struct PolicyOwner info __attribute__((aligned(8)));
  mram_read(&policy_owners[page_index], &info, 8); assert(info.tag);
  if (!(info.tag & 0x80000000U)) {
    unsigned owner = info.tag - 1; assert(owner / POLICY_CLASSES == tid);
    __mram_ptr void *page = policy_buddy->mramMallocableRegionBase + (page_index << 12);
    policy_free_small(&policy_caches[tid][owner % POLICY_CLASSES], page, p, tid); return;
  }
  assert((info.tag & 0x7fffffffU) == tid);
  assert(p == policy_buddy->mramMallocableRegionBase + (page_index << 12));
  policy_owner_write(p, 0, 0);
#else
  for (unsigned i = 0; i < POLICY_CLASSES; ++i) {
    struct PolicyCache *c = &policy_caches[tid][i];
    for (unsigned state = 0; state < (LISTS ? 3U : 1U); ++state) {
      __mram_ptr void *page = c->root;
#if LISTS
      page = *policy_head(c, state);
#endif
      while (page) {
        if (p >= page + 24 && p < page + POLICY_PAGE) {
          policy_free_small(c, page, p, tid); return;
        }
        uint32_t links[2] __attribute__((aligned(8)));
        policy_links(page, links); page = (__mram_ptr void *)links[1];
      }
    }
  }
#endif
  unsigned block = policy_backend_free(p);
#if LOOKUP
  assert(block == info.bytes);
#endif
  assert(s->live_slots >= block); s->live_slots -= block;
}
/* Quiescent structural validation; not used in performance request timing. */
static void policy_verify(unsigned tid) {
  unsigned total_empty = 0;
  for (unsigned i = 0; i < POLICY_CLASSES; ++i) {
    struct PolicyCache *c = &policy_caches[tid][i]; unsigned pages = 0, empty = 0;
    for (unsigned state = 0; state < (LISTS ? 3U : 1U); ++state) {
      __mram_ptr void *page = c->root;
#if LISTS
      page = *policy_head(c, state);
#endif
      uint32_t previous = 0;
      while (page) {
        uint32_t links[2] __attribute__((aligned(8)));
        unsigned bitmap[4] __attribute__((aligned(8)));
        policy_links(page, links); mram_read(page + 8, bitmap, 16);
        assert(links[0] == previous); empty += policy_empty(bitmap);
#if LISTS
        assert((state == 1) == policy_empty(bitmap));
        assert((state == 2) == policy_full(c, bitmap));
#endif
#if LOOKUP
        struct PolicyOwner info __attribute__((aligned(8)));
        mram_read(&policy_owners[policy_page_index(page)], &info, 8);
        assert(info.tag == 1 + tid * POLICY_CLASSES + i && info.bytes == 0);
#endif
        previous = (uint32_t)page; page = (__mram_ptr void *)links[1]; assert(++pages <= c->pages);
      }
    }
    assert(pages == c->pages && empty == c->empty); total_empty += empty;
  }
  assert(total_empty == policy_stats[tid].retained);
  if (POLICY >= 2) assert(total_empty <= EMPTY_BUDGET);
}
#endif

#define BEGIN(kind) __asm__ volatile("policy_begin_" #kind "_%=:" ::: "memory")
#define END(kind) __asm__ volatile("policy_end_" #kind "_%=:" ::: "memory")
#ifdef HOST_TEST
#define PAPER_INPUT const
#define PAPER_STORAGE(type, name, count) static type *name; static unsigned name##_count = count
#define PAPER_BIND(name) name = host_storage(sizeof(*name) * name##_count)
static void input_read(const void *src, void *dst, unsigned n) { memcpy(dst, src, n); }
#else
#define PAPER_INPUT __mram
#define PAPER_STORAGE(type, name, count) __mram_noinit type name[count]
#define PAPER_BIND(name) ((void)0)
static void input_read(__mram_ptr const void *src, void *dst, unsigned n) { mram_read(src, dst, n); }
__host int mallocableRegionSize, pageSize, bufferSize;
#endif
struct PaperOp { unsigned kind, index, value, tag; };
static PAPER_INPUT struct PaperOp paper_ops[5120] = {
  {0U,0U,707U,0U},
  {0U,1U,513U,0U},
  {0U,2U,391U,0U},
  {0U,3U,77U,0U},
  {0U,4U,672U,0U},
  {0U,5U,735U,0U},
  {0U,6U,653U,0U},
  {0U,7U,556U,0U},
  {0U,8U,798U,0U},
  {0U,9U,529U,0U},
  {0U,10U,35U,0U},
  {0U,11U,688U,0U},
  {0U,12U,308U,0U},
  {0U,13U,812U,0U},
  {0U,14U,346U,0U},
  {0U,15U,858U,0U},
  {0U,16U,573U,0U},
  {0U,17U,818U,0U},
  {0U,18U,980U,0U},
  {0U,19U,680U,0U},
  {0U,20U,363U,0U},
  {0U,21U,621U,0U},
  {0U,22U,486U,0U},
  {0U,23U,410U,0U},
  {0U,24U,988U,0U},
  {0U,25U,252U,0U},
  {0U,26U,254U,0U},
  {0U,27U,773U,0U},
  {0U,28U,840U,0U},
  {0U,29U,150U,0U},
  {0U,30U,691U,0U},
  {0U,31U,347U,0U},
  {0U,32U,630U,0U},
  {0U,33U,897U,0U},
  {0U,34U,201U,0U},
  {0U,35U,631U,0U},
  {0U,36U,941U,0U},
  {0U,37U,590U,0U},
  {0U,38U,321U,0U},
  {0U,39U,872U,0U},
  {0U,40U,652U,0U},
  {0U,41U,791U,0U},
  {0U,42U,351U,0U},
  {0U,43U,765U,0U},
  {0U,44U,804U,0U},
  {0U,45U,462U,0U},
  {0U,46U,948U,0U},
  {0U,47U,454U,0U},
  {0U,48U,788U,0U},
  {0U,49U,262U,0U},
  {0U,50U,406U,0U},
  {0U,51U,206U,0U},
  {0U,52U,440U,0U},
  {0U,53U,228U,0U},
  {0U,54U,662U,0U},
  {0U,55U,414U,0U},
  {0U,56U,769U,0U},
  {0U,57U,17U,0U},
  {0U,58U,844U,0U},
  {0U,59U,41U,0U},
  {0U,60U,984U,0U},
  {0U,61U,991U,0U},
  {0U,62U,566U,0U},
  {0U,63U,483U,0U},
  {0U,64U,143U,0U},
  {0U,65U,465U,0U},
  {0U,66U,401U,0U},
  {0U,67U,926U,0U},
  {0U,68U,172U,0U},
  {0U,69U,360U,0U},
  {0U,70U,801U,0U},
  {0U,71U,547U,0U},
  {0U,72U,274U,0U},
  {0U,73U,436U,0U},
  {0U,74U,485U,0U},
  {0U,75U,713U,0U},
  {0U,76U,451U,0U},
  {0U,77U,53U,0U},
  {0U,78U,886U,0U},
  {0U,79U,596U,0U},
  {0U,80U,659U,0U},
  {0U,81U,639U,0U},
  {0U,82U,574U,0U},
  {0U,83U,211U,0U},
  {0U,84U,777U,0U},
  {0U,85U,120U,0U},
  {0U,86U,279U,0U},
  {0U,87U,684U,0U},
  {0U,88U,388U,0U},
  {0U,89U,182U,0U},
  {0U,90U,320U,0U},
  {0U,91U,589U,0U},
  {0U,92U,882U,0U},
  {0U,93U,913U,0U},
  {0U,94U,529U,0U},
  {0U,95U,324U,0U},
  {0U,96U,22U,0U},
  {0U,97U,861U,0U},
  {0U,98U,359U,0U},
  {0U,99U,262U,0U},
  {0U,100U,855U,0U},
  {0U,101U,303U,0U},
  {0U,102U,350U,0U},
  {0U,103U,246U,0U},
  {0U,104U,335U,0U},
  {0U,105U,789U,0U},
  {0U,106U,140U,0U},
  {0U,107U,785U,0U},
  {0U,108U,840U,0U},
  {0U,109U,581U,0U},
  {0U,110U,66U,0U},
  {0U,111U,246U,0U},
  {0U,112U,695U,0U},
  {0U,113U,352U,0U},
  {0U,114U,155U,0U},
  {0U,115U,121U,0U},
  {0U,116U,26U,0U},
  {0U,117U,680U,0U},
  {0U,118U,234U,0U},
  {0U,119U,788U,0U},
  {0U,120U,369U,0U},
  {0U,121U,251U,0U},
  {0U,122U,180U,0U},
  {0U,123U,704U,0U},
  {0U,124U,218U,0U},
  {0U,125U,783U,0U},
  {0U,126U,593U,0U},
  {0U,127U,83U,0U},
  {0U,128U,860U,0U},
  {0U,129U,546U,0U},
  {0U,130U,786U,0U},
  {0U,131U,378U,0U},
  {0U,132U,865U,0U},
  {0U,133U,547U,0U},
  {0U,134U,649U,0U},
  {0U,135U,774U,0U},
  {0U,136U,346U,0U},
  {0U,137U,776U,0U},
  {0U,138U,499U,0U},
  {0U,139U,131U,0U},
  {0U,140U,424U,0U},
  {0U,141U,312U,0U},
  {0U,142U,927U,0U},
  {0U,143U,730U,0U},
  {0U,144U,380U,0U},
  {0U,145U,879U,0U},
  {0U,146U,453U,0U},
  {0U,147U,306U,0U},
  {0U,148U,272U,0U},
  {0U,149U,862U,0U},
  {0U,150U,102U,0U},
  {0U,151U,17U,0U},
  {0U,152U,282U,0U},
  {0U,153U,321U,0U},
  {0U,154U,672U,0U},
  {0U,155U,963U,0U},
  {0U,156U,824U,0U},
  {0U,157U,303U,0U},
  {0U,158U,905U,0U},
  {0U,159U,724U,0U},
  {0U,160U,241U,0U},
  {0U,161U,180U,0U},
  {0U,162U,608U,0U},
  {0U,163U,306U,0U},
  {0U,164U,923U,0U},
  {0U,165U,446U,0U},
  {0U,166U,195U,0U},
  {0U,167U,388U,0U},
  {0U,168U,103U,0U},
  {0U,169U,846U,0U},
  {0U,170U,316U,0U},
  {0U,171U,779U,0U},
  {0U,172U,674U,0U},
  {0U,173U,64U,0U},
  {0U,174U,129U,0U},
  {0U,175U,496U,0U},
  {0U,176U,712U,0U},
  {0U,177U,717U,0U},
  {0U,178U,87U,0U},
  {0U,179U,193U,0U},
  {0U,180U,471U,0U},
  {0U,181U,251U,0U},
  {0U,182U,263U,0U},
  {0U,183U,728U,0U},
  {0U,184U,54U,0U},
  {0U,185U,337U,0U},
  {0U,186U,412U,0U},
  {0U,187U,908U,0U},
  {0U,188U,877U,0U},
  {0U,189U,197U,0U},
  {0U,190U,702U,0U},
  {0U,191U,407U,0U},
  {0U,192U,165U,0U},
  {0U,193U,810U,0U},
  {0U,194U,233U,0U},
  {0U,195U,12U,0U},
  {0U,196U,9U,0U},
  {0U,197U,812U,0U},
  {0U,198U,97U,0U},
  {0U,199U,891U,0U},
  {0U,200U,997U,0U},
  {0U,201U,829U,0U},
  {0U,202U,690U,0U},
  {0U,203U,221U,0U},
  {0U,204U,917U,0U},
  {0U,205U,456U,0U},
  {0U,206U,584U,0U},
  {0U,207U,662U,0U},
  {0U,208U,641U,0U},
  {0U,209U,893U,0U},
  {0U,210U,13U,0U},
  {0U,211U,614U,0U},
  {0U,212U,517U,0U},
  {0U,213U,279U,0U},
  {0U,214U,130U,0U},
  {0U,215U,876U,0U},
  {0U,216U,818U,0U},
  {0U,217U,184U,0U},
  {0U,218U,351U,0U},
  {0U,219U,563U,0U},
  {0U,220U,219U,0U},
  {0U,221U,268U,0U},
  {0U,222U,956U,0U},
  {0U,223U,355U,0U},
  {0U,224U,186U,0U},
  {0U,225U,617U,0U},
  {0U,226U,68U,0U},
  {0U,227U,909U,0U},
  {0U,228U,357U,0U},
  {0U,229U,424U,0U},
  {0U,230U,615U,0U},
  {0U,231U,942U,0U},
  {0U,232U,833U,0U},
  {0U,233U,544U,0U},
  {0U,234U,319U,0U},
  {0U,235U,43U,0U},
  {0U,236U,631U,0U},
  {0U,237U,834U,0U},
  {0U,238U,325U,0U},
  {0U,239U,502U,0U},
  {0U,240U,971U,0U},
  {0U,241U,442U,0U},
  {0U,242U,451U,0U},
  {0U,243U,135U,0U},
  {0U,244U,172U,0U},
  {0U,245U,632U,0U},
  {0U,246U,622U,0U},
  {0U,247U,765U,0U},
  {0U,248U,758U,0U},
  {0U,249U,461U,0U},
  {0U,250U,207U,0U},
  {0U,251U,615U,0U},
  {0U,252U,813U,0U},
  {0U,253U,858U,0U},
  {0U,254U,602U,0U},
  {0U,255U,466U,0U},
  {0U,256U,867U,0U},
  {0U,257U,476U,0U},
  {0U,258U,239U,0U},
  {0U,259U,501U,0U},
  {0U,260U,599U,0U},
  {0U,261U,300U,0U},
  {0U,262U,631U,0U},
  {0U,263U,354U,0U},
  {0U,264U,196U,0U},
  {0U,265U,747U,0U},
  {0U,266U,650U,0U},
  {0U,267U,52U,0U},
  {0U,268U,317U,0U},
  {0U,269U,683U,0U},
  {0U,270U,148U,0U},
  {0U,271U,863U,0U},
  {0U,272U,183U,0U},
  {0U,273U,136U,0U},
  {0U,274U,644U,0U},
  {0U,275U,784U,0U},
  {0U,276U,710U,0U},
  {0U,277U,791U,0U},
  {0U,278U,120U,0U},
  {0U,279U,941U,0U},
  {0U,280U,881U,0U},
  {0U,281U,17U,0U},
  {0U,282U,847U,0U},
  {0U,283U,539U,0U},
  {0U,284U,358U,0U},
  {0U,285U,26U,0U},
  {0U,286U,443U,0U},
  {0U,287U,150U,0U},
  {0U,288U,449U,0U},
  {0U,289U,275U,0U},
  {0U,290U,174U,0U},
  {0U,291U,858U,0U},
  {0U,292U,998U,0U},
  {0U,293U,522U,0U},
  {0U,294U,302U,0U},
  {0U,295U,738U,0U},
  {0U,296U,369U,0U},
  {0U,297U,950U,0U},
  {0U,298U,864U,0U},
  {0U,299U,171U,0U},
  {0U,300U,769U,0U},
  {0U,301U,49U,0U},
  {0U,302U,293U,0U},
  {0U,303U,360U,0U},
  {0U,304U,511U,0U},
  {0U,305U,295U,0U},
  {0U,306U,294U,0U},
  {0U,307U,872U,0U},
  {0U,308U,948U,0U},
  {0U,309U,162U,0U},
  {0U,310U,663U,0U},
  {0U,311U,126U,0U},
  {0U,312U,946U,0U},
  {0U,313U,480U,0U},
  {0U,314U,377U,0U},
  {0U,315U,833U,0U},
  {0U,316U,915U,0U},
  {0U,317U,575U,0U},
  {0U,318U,750U,0U},
  {0U,319U,749U,0U},
  {0U,320U,966U,0U},
  {0U,321U,973U,0U},
  {0U,322U,52U,0U},
  {0U,323U,164U,0U},
  {0U,324U,972U,0U},
  {0U,325U,899U,0U},
  {0U,326U,986U,0U},
  {0U,327U,206U,0U},
  {0U,328U,391U,0U},
  {0U,329U,407U,0U},
  {0U,330U,657U,0U},
  {0U,331U,972U,0U},
  {0U,332U,472U,0U},
  {0U,333U,747U,0U},
  {0U,334U,37U,0U},
  {0U,335U,305U,0U},
  {0U,336U,617U,0U},
  {0U,337U,308U,0U},
  {0U,338U,657U,0U},
  {0U,339U,624U,0U},
  {0U,340U,221U,0U},
  {0U,341U,768U,0U},
  {0U,342U,29U,0U},
  {0U,343U,923U,0U},
  {0U,344U,18U,0U},
  {0U,345U,412U,0U},
  {0U,346U,51U,0U},
  {0U,347U,43U,0U},
  {0U,348U,393U,0U},
  {0U,349U,825U,0U},
  {0U,350U,625U,0U},
  {0U,351U,45U,0U},
  {0U,352U,599U,0U},
  {0U,353U,174U,0U},
  {0U,354U,205U,0U},
  {0U,355U,384U,0U},
  {0U,356U,183U,0U},
  {0U,357U,120U,0U},
  {0U,358U,144U,0U},
  {0U,359U,529U,0U},
  {0U,360U,25U,0U},
  {0U,361U,192U,0U},
  {0U,362U,874U,0U},
  {0U,363U,213U,0U},
  {0U,364U,948U,0U},
  {0U,365U,973U,0U},
  {0U,366U,818U,0U},
  {0U,367U,468U,0U},
  {0U,368U,344U,0U},
  {0U,369U,864U,0U},
  {0U,370U,45U,0U},
  {0U,371U,576U,0U},
  {0U,372U,704U,0U},
  {0U,373U,659U,0U},
  {0U,374U,869U,0U},
  {0U,375U,210U,0U},
  {0U,376U,911U,0U},
  {0U,377U,802U,0U},
  {0U,378U,87U,0U},
  {0U,379U,48U,0U},
  {0U,380U,208U,0U},
  {0U,381U,630U,0U},
  {0U,382U,105U,0U},
  {0U,383U,570U,0U},
  {0U,384U,744U,0U},
  {0U,385U,394U,0U},
  {0U,386U,47U,0U},
  {0U,387U,231U,0U},
  {0U,388U,459U,0U},
  {0U,389U,28U,0U},
  {0U,390U,128U,0U},
  {0U,391U,976U,0U},
  {0U,392U,672U,0U},
  {0U,393U,894U,0U},
  {0U,394U,437U,0U},
  {0U,395U,503U,0U},
  {0U,396U,22U,0U},
  {0U,397U,263U,0U},
  {0U,398U,559U,0U},
  {0U,399U,807U,0U},
  {0U,400U,327U,0U},
  {0U,401U,285U,0U},
  {0U,402U,713U,0U},
  {0U,403U,94U,0U},
  {0U,404U,244U,0U},
  {0U,405U,804U,0U},
  {0U,406U,116U,0U},
  {0U,407U,857U,0U},
  {0U,408U,985U,0U},
  {0U,409U,989U,0U},
  {0U,410U,719U,0U},
  {0U,411U,622U,0U},
  {0U,412U,845U,0U},
  {0U,413U,818U,0U},
  {0U,414U,307U,0U},
  {0U,415U,502U,0U},
  {0U,416U,337U,0U},
  {0U,417U,442U,0U},
  {0U,418U,372U,0U},
  {0U,419U,878U,0U},
  {0U,420U,663U,0U},
  {0U,421U,37U,0U},
  {0U,422U,815U,0U},
  {0U,423U,909U,0U},
  {0U,424U,996U,0U},
  {0U,425U,712U,0U},
  {0U,426U,361U,0U},
  {0U,427U,299U,0U},
  {0U,428U,521U,0U},
  {0U,429U,676U,0U},
  {0U,430U,619U,0U},
  {0U,431U,894U,0U},
  {0U,432U,215U,0U},
  {0U,433U,843U,0U},
  {0U,434U,175U,0U},
  {0U,435U,334U,0U},
  {0U,436U,427U,0U},
  {0U,437U,521U,0U},
  {0U,438U,39U,0U},
  {0U,439U,698U,0U},
  {0U,440U,418U,0U},
  {0U,441U,721U,0U},
  {0U,442U,160U,0U},
  {0U,443U,672U,0U},
  {0U,444U,570U,0U},
  {0U,445U,397U,0U},
  {0U,446U,234U,0U},
  {0U,447U,311U,0U},
  {0U,448U,614U,0U},
  {0U,449U,763U,0U},
  {0U,450U,169U,0U},
  {0U,451U,548U,0U},
  {0U,452U,868U,0U},
  {0U,453U,388U,0U},
  {0U,454U,304U,0U},
  {0U,455U,835U,0U},
  {0U,456U,519U,0U},
  {0U,457U,434U,0U},
  {0U,458U,788U,0U},
  {0U,459U,160U,0U},
  {0U,460U,355U,0U},
  {0U,461U,202U,0U},
  {0U,462U,789U,0U},
  {0U,463U,758U,0U},
  {0U,464U,984U,0U},
  {0U,465U,148U,0U},
  {0U,466U,625U,0U},
  {0U,467U,150U,0U},
  {0U,468U,827U,0U},
  {0U,469U,739U,0U},
  {0U,470U,870U,0U},
  {0U,471U,439U,0U},
  {0U,472U,112U,0U},
  {0U,473U,281U,0U},
  {0U,474U,882U,0U},
  {0U,475U,458U,0U},
  {0U,476U,671U,0U},
  {0U,477U,257U,0U},
  {0U,478U,116U,0U},
  {0U,479U,845U,0U},
  {0U,480U,839U,0U},
  {0U,481U,200U,0U},
  {0U,482U,619U,0U},
  {0U,483U,963U,0U},
  {0U,484U,925U,0U},
  {0U,485U,567U,0U},
  {0U,486U,990U,0U},
  {0U,487U,512U,0U},
  {0U,488U,481U,0U},
  {0U,489U,325U,0U},
  {0U,490U,449U,0U},
  {0U,491U,839U,0U},
  {0U,492U,689U,0U},
  {0U,493U,939U,0U},
  {0U,494U,859U,0U},
  {0U,495U,173U,0U},
  {0U,496U,47U,0U},
  {0U,497U,87U,0U},
  {0U,498U,695U,0U},
  {0U,499U,655U,0U},
  {0U,500U,699U,0U},
  {0U,501U,781U,0U},
  {0U,502U,619U,0U},
  {0U,503U,251U,0U},
  {0U,504U,468U,0U},
  {0U,505U,83U,0U},
  {0U,506U,610U,0U},
  {0U,507U,998U,0U},
  {0U,508U,734U,0U},
  {0U,509U,696U,0U},
  {0U,510U,647U,0U},
  {0U,511U,848U,0U},
  {1U,0U,511U,0U},
  {1U,28U,510U,0U},
  {1U,161U,509U,0U},
  {1U,149U,508U,0U},
  {1U,267U,507U,0U},
  {1U,235U,506U,0U},
  {1U,104U,505U,0U},
  {1U,402U,504U,0U},
  {1U,110U,503U,0U},
  {1U,46U,502U,0U},
  {1U,229U,501U,0U},
  {1U,286U,500U,0U},
  {1U,163U,499U,0U},
  {1U,452U,498U,0U},
  {1U,463U,497U,0U},
  {1U,152U,496U,0U},
  {1U,408U,495U,0U},
  {1U,240U,494U,0U},
  {1U,262U,493U,0U},
  {1U,375U,492U,0U},
  {1U,79U,491U,0U},
  {1U,450U,490U,0U},
  {1U,352U,489U,0U},
  {1U,98U,488U,0U},
  {1U,326U,487U,0U},
  {1U,428U,486U,0U},
  {1U,94U,485U,0U},
  {1U,83U,484U,0U},
  {1U,27U,483U,0U},
  {1U,99U,482U,0U},
  {1U,29U,481U,0U},
  {1U,318U,480U,0U},
  {1U,166U,479U,0U},
  {1U,9U,478U,0U},
  {1U,84U,477U,0U},
  {1U,400U,476U,0U},
  {1U,414U,475U,0U},
  {1U,40U,474U,0U},
  {1U,456U,473U,0U},
  {1U,79U,472U,0U},
  {1U,83U,471U,0U},
  {1U,7U,470U,0U},
  {1U,436U,469U,0U},
  {1U,160U,468U,0U},
  {1U,66U,467U,0U},
  {1U,35U,466U,0U},
  {1U,380U,465U,0U},
  {1U,276U,464U,0U},
  {1U,406U,463U,0U},
  {1U,456U,462U,0U},
  {1U,113U,461U,0U},
  {1U,178U,460U,0U},
  {1U,25U,459U,0U},
  {1U,7U,458U,0U},
  {1U,143U,457U,0U},
  {1U,356U,456U,0U},
  {1U,413U,455U,0U},
  {1U,336U,454U,0U},
  {1U,57U,453U,0U},
  {1U,20U,452U,0U},
  {1U,217U,451U,0U},
  {1U,130U,450U,0U},
  {1U,188U,449U,0U},
  {1U,268U,448U,0U},
  {1U,130U,447U,0U},
  {1U,332U,446U,0U},
  {1U,376U,445U,0U},
  {1U,416U,444U,0U},
  {1U,185U,443U,0U},
  {1U,163U,442U,0U},
  {1U,30U,441U,0U},
  {1U,395U,440U,0U},
  {1U,390U,439U,0U},
  {1U,272U,438U,0U},
  {1U,350U,437U,0U},
  {1U,52U,436U,0U},
  {1U,420U,435U,0U},
  {1U,361U,434U,0U},
  {1U,417U,433U,0U},
  {1U,417U,432U,0U},
  {1U,417U,431U,0U},
  {1U,397U,430U,0U},
  {1U,301U,429U,0U},
  {1U,176U,428U,0U},
  {1U,242U,427U,0U},
  {1U,158U,426U,0U},
  {1U,36U,425U,0U},
  {1U,52U,424U,0U},
  {1U,307U,423U,0U},
  {1U,72U,422U,0U},
  {1U,100U,421U,0U},
  {1U,415U,420U,0U},
  {1U,79U,419U,0U},
  {1U,268U,418U,0U},
  {1U,322U,417U,0U},
  {1U,21U,416U,0U},
  {1U,203U,415U,0U},
  {1U,328U,414U,0U},
  {1U,97U,413U,0U},
  {1U,142U,412U,0U},
  {1U,323U,411U,0U},
  {1U,201U,410U,0U},
  {1U,300U,409U,0U},
  {1U,361U,408U,0U},
  {1U,211U,407U,0U},
  {1U,314U,406U,0U},
  {1U,289U,405U,0U},
  {1U,198U,404U,0U},
  {1U,323U,403U,0U},
  {1U,373U,402U,0U},
  {1U,100U,401U,0U},
  {1U,282U,400U,0U},
  {1U,216U,399U,0U},
  {1U,387U,398U,0U},
  {1U,134U,397U,0U},
  {1U,232U,396U,0U},
  {1U,333U,395U,0U},
  {1U,24U,394U,0U},
  {1U,269U,393U,0U},
  {1U,328U,392U,0U},
  {1U,365U,391U,0U},
  {1U,32U,390U,0U},
  {1U,114U,389U,0U},
  {1U,200U,388U,0U},
  {1U,311U,387U,0U},
  {1U,326U,386U,0U},
  {1U,98U,385U,0U},
  {1U,37U,384U,0U},
  {1U,241U,383U,0U},
  {1U,319U,382U,0U},
  {1U,171U,381U,0U},
  {1U,107U,380U,0U},
  {1U,39U,379U,0U},
  {1U,82U,378U,0U},
  {1U,140U,377U,0U},
  {1U,265U,376U,0U},
  {1U,85U,375U,0U},
  {1U,149U,374U,0U},
  {1U,324U,373U,0U},
  {1U,75U,372U,0U},
  {1U,236U,371U,0U},
  {1U,213U,370U,0U},
  {1U,297U,369U,0U},
  {1U,356U,368U,0U},
  {1U,317U,367U,0U},
  {1U,340U,366U,0U},
  {1U,154U,365U,0U},
  {1U,113U,364U,0U},
  {1U,236U,363U,0U},
  {1U,344U,362U,0U},
  {1U,336U,361U,0U},
  {1U,177U,360U,0U},
  {1U,269U,359U,0U},
  {1U,88U,358U,0U},
  {1U,137U,357U,0U},
  {1U,185U,356U,0U},
  {1U,170U,355U,0U},
  {1U,86U,354U,0U},
  {1U,260U,353U,0U},
  {1U,236U,352U,0U},
  {1U,123U,351U,0U},
  {1U,95U,350U,0U},
  {1U,138U,349U,0U},
  {1U,143U,348U,0U},
  {1U,149U,347U,0U},
  {1U,119U,346U,0U},
  {1U,26U,345U,0U},
  {1U,74U,344U,0U},
  {1U,91U,343U,0U},
  {1U,69U,342U,0U},
  {1U,135U,341U,0U},
  {1U,201U,340U,0U},
  {1U,117U,339U,0U},
  {1U,270U,338U,0U},
  {1U,246U,337U,0U},
  {1U,46U,336U,0U},
  {1U,315U,335U,0U},
  {1U,121U,334U,0U},
  {1U,127U,333U,0U},
  {1U,250U,332U,0U},
  {1U,187U,331U,0U},
  {1U,24U,330U,0U},
  {1U,294U,329U,0U},
  {1U,89U,328U,0U},
  {1U,34U,327U,0U},
  {1U,247U,326U,0U},
  {1U,133U,325U,0U},
  {1U,273U,324U,0U},
  {1U,304U,323U,0U},
  {1U,310U,322U,0U},
  {1U,30U,321U,0U},
  {1U,318U,320U,0U},
  {1U,81U,319U,0U},
  {1U,289U,318U,0U},
  {1U,230U,317U,0U},
  {1U,246U,316U,0U},
  {1U,243U,315U,0U},
  {1U,266U,314U,0U},
  {1U,14U,313U,0U},
  {1U,300U,312U,0U},
  {1U,52U,311U,0U},
  {1U,90U,310U,0U},
  {1U,63U,309U,0U},
  {1U,145U,308U,0U},
  {1U,27U,307U,0U},
  {1U,247U,306U,0U},
  {1U,174U,305U,0U},
  {1U,26U,304U,0U},
  {1U,179U,303U,0U},
  {1U,194U,302U,0U},
  {1U,242U,301U,0U},
  {1U,283U,300U,0U},
  {1U,297U,299U,0U},
  {1U,259U,298U,0U},
  {1U,191U,297U,0U},
  {1U,218U,296U,0U},
  {1U,221U,295U,0U},
  {1U,152U,294U,0U},
  {1U,250U,293U,0U},
  {1U,37U,292U,0U},
  {1U,86U,291U,0U},
  {1U,40U,290U,0U},
  {1U,252U,289U,0U},
  {1U,167U,288U,0U},
  {1U,225U,287U,0U},
  {1U,131U,286U,0U},
  {1U,186U,285U,0U},
  {1U,97U,284U,0U},
  {1U,160U,283U,0U},
  {1U,228U,282U,0U},
  {1U,260U,281U,0U},
  {1U,28U,280U,0U},
  {1U,120U,279U,0U},
  {1U,136U,278U,0U},
  {1U,66U,277U,0U},
  {1U,227U,276U,0U},
  {1U,221U,275U,0U},
  {1U,153U,274U,0U},
  {1U,43U,273U,0U},
  {1U,113U,272U,0U},
  {1U,125U,271U,0U},
  {1U,180U,270U,0U},
  {1U,136U,269U,0U},
  {1U,14U,268U,0U},
  {1U,163U,267U,0U},
  {1U,92U,266U,0U},
  {1U,59U,265U,0U},
  {1U,68U,264U,0U},
  {1U,140U,263U,0U},
  {1U,32U,262U,0U},
  {1U,18U,261U,0U},
  {1U,42U,260U,0U},
  {1U,3U,259U,0U},
  {1U,208U,258U,0U},
  {1U,96U,257U,0U},
  {1U,181U,256U,0U},
  {1U,215U,255U,0U},
  {1U,115U,254U,0U},
  {1U,245U,253U,0U},
  {1U,196U,252U,0U},
  {1U,121U,251U,0U},
  {1U,2U,250U,0U},
  {1U,235U,249U,0U},
  {1U,107U,248U,0U},
  {1U,38U,247U,0U},
  {1U,240U,246U,0U},
  {1U,165U,245U,0U},
  {1U,44U,244U,0U},
  {1U,99U,243U,0U},
  {1U,27U,242U,0U},
  {1U,68U,241U,0U},
  {1U,4U,240U,0U},
  {1U,238U,239U,0U},
  {1U,108U,238U,0U},
  {1U,225U,237U,0U},
  {1U,231U,236U,0U},
  {1U,143U,235U,0U},
  {1U,205U,234U,0U},
  {1U,61U,233U,0U},
  {1U,216U,232U,0U},
  {1U,60U,231U,0U},
  {1U,90U,230U,0U},
  {1U,35U,229U,0U},
  {1U,131U,228U,0U},
  {1U,1U,227U,0U},
  {1U,93U,226U,0U},
  {1U,128U,225U,0U},
  {1U,42U,224U,0U},
  {1U,171U,223U,0U},
  {1U,215U,222U,0U},
  {1U,133U,221U,0U},
  {1U,195U,220U,0U},
  {1U,14U,219U,0U},
  {1U,192U,218U,0U},
  {1U,77U,217U,0U},
  {1U,132U,216U,0U},
  {1U,10U,215U,0U},
  {1U,163U,214U,0U},
  {1U,99U,213U,0U},
  {1U,35U,212U,0U},
  {1U,198U,211U,0U},
  {1U,87U,210U,0U},
  {1U,82U,209U,0U},
  {1U,48U,208U,0U},
  {1U,86U,207U,0U},
  {1U,121U,206U,0U},
  {1U,73U,205U,0U},
  {1U,163U,204U,0U},
  {1U,116U,203U,0U},
  {1U,174U,202U,0U},
  {1U,51U,201U,0U},
  {1U,143U,200U,0U},
  {1U,43U,199U,0U},
  {1U,193U,198U,0U},
  {1U,159U,197U,0U},
  {1U,69U,196U,0U},
  {1U,44U,195U,0U},
  {1U,1U,194U,0U},
  {1U,115U,193U,0U},
  {1U,177U,192U,0U},
  {1U,53U,191U,0U},
  {1U,167U,190U,0U},
  {1U,5U,189U,0U},
  {1U,103U,188U,0U},
  {1U,174U,187U,0U},
  {1U,57U,186U,0U},
  {1U,8U,185U,0U},
  {1U,56U,184U,0U},
  {1U,122U,183U,0U},
  {1U,151U,182U,0U},
  {1U,67U,181U,0U},
  {1U,144U,180U,0U},
  {1U,67U,179U,0U},
  {1U,36U,178U,0U},
  {1U,19U,177U,0U},
  {1U,42U,176U,0U},
  {1U,87U,175U,0U},
  {1U,69U,174U,0U},
  {1U,4U,173U,0U},
  {1U,110U,172U,0U},
  {1U,63U,171U,0U},
  {1U,56U,170U,0U},
  {1U,56U,169U,0U},
  {1U,93U,168U,0U},
  {1U,126U,167U,0U},
  {1U,79U,166U,0U},
  {1U,60U,165U,0U},
  {1U,129U,164U,0U},
  {1U,25U,163U,0U},
  {1U,74U,162U,0U},
  {1U,58U,161U,0U},
  {1U,145U,160U,0U},
  {1U,122U,159U,0U},
  {1U,95U,158U,0U},
  {1U,138U,157U,0U},
  {1U,156U,156U,0U},
  {1U,121U,155U,0U},
  {1U,51U,154U,0U},
  {1U,117U,153U,0U},
  {1U,118U,152U,0U},
  {1U,62U,151U,0U},
  {1U,116U,150U,0U},
  {1U,86U,149U,0U},
  {1U,67U,148U,0U},
  {1U,99U,147U,0U},
  {1U,78U,146U,0U},
  {1U,124U,145U,0U},
  {1U,27U,144U,0U},
  {1U,29U,143U,0U},
  {1U,35U,142U,0U},
  {1U,82U,141U,0U},
  {1U,66U,140U,0U},
  {1U,139U,139U,0U},
  {1U,77U,138U,0U},
  {1U,38U,137U,0U},
  {1U,39U,136U,0U},
  {1U,111U,135U,0U},
  {1U,73U,134U,0U},
  {1U,117U,133U,0U},
  {1U,64U,132U,0U},
  {1U,82U,131U,0U},
  {1U,81U,130U,0U},
  {1U,120U,129U,0U},
  {1U,82U,128U,0U},
  {1U,121U,127U,0U},
  {1U,97U,126U,0U},
  {1U,117U,125U,0U},
  {1U,3U,124U,0U},
  {1U,114U,123U,0U},
  {1U,0U,122U,0U},
  {1U,119U,121U,0U},
  {1U,50U,120U,0U},
  {1U,15U,119U,0U},
  {1U,42U,118U,0U},
  {1U,0U,117U,0U},
  {1U,8U,116U,0U},
  {1U,106U,115U,0U},
  {1U,65U,114U,0U},
  {1U,27U,113U,0U},
  {1U,12U,112U,0U},
  {1U,20U,111U,0U},
  {1U,5U,110U,0U},
  {1U,59U,109U,0U},
  {1U,48U,108U,0U},
  {1U,23U,107U,0U},
  {1U,14U,106U,0U},
  {1U,73U,105U,0U},
  {1U,66U,104U,0U},
  {1U,70U,103U,0U},
  {1U,67U,102U,0U},
  {1U,86U,101U,0U},
  {1U,88U,100U,0U},
  {1U,93U,99U,0U},
  {1U,76U,98U,0U},
  {1U,42U,97U,0U},
  {1U,13U,96U,0U},
  {1U,67U,95U,0U},
  {1U,83U,94U,0U},
  {1U,37U,93U,0U},
  {1U,61U,92U,0U},
  {1U,28U,91U,0U},
  {1U,6U,90U,0U},
  {1U,61U,89U,0U},
  {1U,48U,88U,0U},
  {1U,79U,87U,0U},
  {1U,19U,86U,0U},
  {1U,53U,85U,0U},
  {1U,40U,84U,0U},
  {1U,37U,83U,0U},
  {1U,70U,82U,0U},
  {1U,44U,81U,0U},
  {1U,30U,80U,0U},
  {1U,66U,79U,0U},
  {1U,67U,78U,0U},
  {1U,1U,77U,0U},
  {1U,27U,76U,0U},
  {1U,18U,75U,0U},
  {1U,66U,74U,0U},
  {1U,56U,73U,0U},
  {1U,12U,72U,0U},
  {1U,53U,71U,0U},
  {1U,45U,70U,0U},
  {1U,2U,69U,0U},
  {1U,44U,68U,0U},
  {1U,1U,67U,0U},
  {1U,47U,66U,0U},
  {1U,43U,65U,0U},
  {1U,36U,64U,0U},
  {1U,3U,63U,0U},
  {1U,60U,62U,0U},
  {1U,60U,61U,0U},
  {1U,22U,60U,0U},
  {1U,53U,59U,0U},
  {1U,6U,58U,0U},
  {1U,0U,57U,0U},
  {1U,21U,56U,0U},
  {1U,6U,55U,0U},
  {1U,46U,54U,0U},
  {1U,6U,53U,0U},
  {1U,16U,52U,0U},
  {1U,21U,51U,0U},
  {1U,50U,50U,0U},
  {1U,14U,49U,0U},
  {1U,25U,48U,0U},
  {1U,17U,47U,0U},
  {1U,2U,46U,0U},
  {1U,37U,45U,0U},
  {1U,14U,44U,0U},
  {1U,6U,43U,0U},
  {1U,29U,42U,0U},
  {1U,20U,41U,0U},
  {1U,25U,40U,0U},
  {1U,9U,39U,0U},
  {1U,37U,38U,0U},
  {1U,21U,37U,0U},
  {1U,12U,36U,0U},
  {1U,19U,35U,0U},
  {1U,29U,34U,0U},
  {1U,30U,33U,0U},
  {1U,24U,32U,0U},
  {1U,23U,31U,0U},
  {1U,13U,30U,0U},
  {1U,0U,29U,0U},
  {1U,3U,28U,0U},
  {1U,19U,27U,0U},
  {1U,5U,26U,0U},
  {1U,15U,25U,0U},
  {1U,24U,24U,0U},
  {1U,12U,23U,0U},
  {1U,2U,22U,0U},
  {1U,11U,21U,0U},
  {1U,4U,20U,0U},
  {1U,3U,19U,0U},
  {1U,12U,18U,0U},
  {1U,15U,17U,0U},
  {1U,4U,16U,0U},
  {1U,9U,15U,0U},
  {1U,5U,14U,0U},
  {1U,9U,13U,0U},
  {1U,12U,12U,0U},
  {1U,4U,11U,0U},
  {1U,2U,10U,0U},
  {1U,1U,9U,0U},
  {1U,6U,8U,0U},
  {1U,0U,7U,0U},
  {1U,3U,6U,0U},
  {1U,0U,5U,0U},
  {1U,1U,4U,0U},
  {1U,1U,3U,0U},
  {1U,2U,2U,0U},
  {1U,0U,1U,0U},
  {1U,0U,0U,0U},
  {2U,459U,381U,1025U},
  {2U,414U,515U,1026U},
  {2U,242U,849U,1027U},
  {2U,327U,887U,1028U},
  {2U,321U,618U,1029U},
  {2U,426U,611U,1030U},
  {2U,86U,679U,1031U},
  {2U,187U,9U,1032U},
  {2U,109U,560U,1033U},
  {2U,485U,488U,1034U},
  {2U,415U,728U,1035U},
  {2U,204U,917U,1036U},
  {2U,488U,952U,1037U},
  {2U,346U,157U,1038U},
  {2U,373U,811U,1039U},
  {2U,136U,172U,1040U},
  {2U,313U,763U,1041U},
  {2U,224U,577U,1042U},
  {2U,22U,961U,1043U},
  {2U,25U,122U,1044U},
  {2U,221U,370U,1045U},
  {2U,503U,117U,1046U},
  {2U,421U,854U,1047U},
  {2U,249U,522U,1048U},
  {2U,43U,861U,1049U},
  {2U,248U,136U,1050U},
  {2U,156U,65U,1051U},
  {2U,445U,23U,1052U},
  {2U,499U,37U,1053U},
  {2U,192U,827U,1054U},
  {2U,236U,616U,1055U},
  {2U,20U,528U,1056U},
  {2U,238U,24U,1057U},
  {2U,94U,937U,1058U},
  {2U,418U,528U,1059U},
  {2U,345U,384U,1060U},
  {2U,305U,148U,1061U},
  {2U,180U,408U,1062U},
  {2U,3U,865U,1063U},
  {2U,405U,497U,1064U},
  {2U,138U,951U,1065U},
  {2U,141U,710U,1066U},
  {2U,249U,754U,1067U},
  {2U,47U,396U,1068U},
  {2U,266U,662U,1069U},
  {2U,352U,684U,1070U},
  {2U,301U,200U,1071U},
  {2U,282U,935U,1072U},
  {2U,445U,965U,1073U},
  {2U,469U,203U,1074U},
  {2U,155U,985U,1075U},
  {2U,452U,574U,1076U},
  {2U,360U,208U,1077U},
  {2U,57U,447U,1078U},
  {2U,241U,535U,1079U},
  {2U,146U,935U,1080U},
  {2U,79U,190U,1081U},
  {2U,131U,380U,1082U},
  {2U,272U,547U,1083U},
  {2U,238U,888U,1084U},
  {2U,97U,536U,1085U},
  {2U,163U,350U,1086U},
  {2U,215U,105U,1087U},
  {2U,288U,989U,1088U},
  {2U,231U,285U,1089U},
  {2U,176U,778U,1090U},
  {2U,81U,214U,1091U},
  {2U,42U,224U,1092U},
  {2U,131U,858U,1093U},
  {2U,161U,953U,1094U},
  {2U,205U,167U,1095U},
  {2U,329U,725U,1096U},
  {2U,346U,101U,1097U},
  {2U,453U,373U,1098U},
  {2U,59U,926U,1099U},
  {2U,267U,101U,1100U},
  {2U,74U,580U,1101U},
  {2U,66U,244U,1102U},
  {2U,168U,398U,1103U},
  {2U,356U,93U,1104U},
  {2U,499U,535U,1105U},
  {2U,376U,487U,1106U},
  {2U,232U,736U,1107U},
  {2U,115U,969U,1108U},
  {2U,72U,784U,1109U},
  {2U,138U,71U,1110U},
  {2U,227U,884U,1111U},
  {2U,326U,289U,1112U},
  {2U,431U,258U,1113U},
  {2U,173U,478U,1114U},
  {2U,410U,741U,1115U},
  {2U,63U,838U,1116U},
  {2U,204U,340U,1117U},
  {2U,297U,999U,1118U},
  {2U,394U,107U,1119U},
  {2U,131U,579U,1120U},
  {2U,14U,370U,1121U},
  {2U,337U,221U,1122U},
  {2U,281U,90U,1123U},
  {2U,347U,67U,1124U},
  {2U,143U,26U,1125U},
  {2U,75U,579U,1126U},
  {2U,175U,721U,1127U},
  {2U,303U,535U,1128U},
  {2U,239U,664U,1129U},
  {2U,28U,60U,1130U},
  {2U,352U,142U,1131U},
  {2U,439U,365U,1132U},
  {2U,111U,512U,1133U},
  {2U,146U,12U,1134U},
  {2U,353U,799U,1135U},
  {2U,290U,965U,1136U},
  {2U,310U,67U,1137U},
  {2U,443U,705U,1138U},
  {2U,86U,588U,1139U},
  {2U,41U,10U,1140U},
  {2U,323U,568U,1141U},
  {2U,127U,844U,1142U},
  {2U,210U,296U,1143U},
  {2U,419U,264U,1144U},
  {2U,130U,301U,1145U},
  {2U,334U,231U,1146U},
  {2U,345U,630U,1147U},
  {2U,474U,476U,1148U},
  {2U,114U,65U,1149U},
  {2U,375U,29U,1150U},
  {2U,310U,319U,1151U},
  {2U,161U,730U,1152U},
  {2U,81U,383U,1153U},
  {2U,494U,768U,1154U},
  {2U,300U,668U,1155U},
  {2U,215U,674U,1156U},
  {2U,125U,504U,1157U},
  {2U,125U,335U,1158U},
  {2U,180U,465U,1159U},
  {2U,418U,921U,1160U},
  {2U,108U,468U,1161U},
  {2U,361U,540U,1162U},
  {2U,169U,899U,1163U},
  {2U,14U,860U,1164U},
  {2U,0U,65U,1165U},
  {2U,388U,97U,1166U},
  {2U,447U,365U,1167U},
  {2U,389U,201U,1168U},
  {2U,127U,288U,1169U},
  {2U,164U,703U,1170U},
  {2U,198U,290U,1171U},
  {2U,22U,668U,1172U},
  {2U,295U,412U,1173U},
  {2U,172U,882U,1174U},
  {2U,353U,703U,1175U},
  {2U,99U,175U,1176U},
  {2U,132U,500U,1177U},
  {2U,219U,848U,1178U},
  {2U,441U,48U,1179U},
  {2U,0U,326U,1180U},
  {2U,163U,298U,1181U},
  {2U,359U,269U,1182U},
  {2U,43U,739U,1183U},
  {2U,260U,189U,1184U},
  {2U,66U,39U,1185U},
  {2U,178U,759U,1186U},
  {2U,193U,34U,1187U},
  {2U,278U,711U,1188U},
  {2U,314U,614U,1189U},
  {2U,385U,318U,1190U},
  {2U,418U,296U,1191U},
  {2U,508U,527U,1192U},
  {2U,108U,896U,1193U},
  {2U,397U,599U,1194U},
  {2U,153U,523U,1195U},
  {2U,66U,666U,1196U},
  {2U,23U,690U,1197U},
  {2U,335U,79U,1198U},
  {2U,312U,596U,1199U},
  {2U,20U,316U,1200U},
  {2U,318U,720U,1201U},
  {2U,162U,452U,1202U},
  {2U,26U,581U,1203U},
  {2U,21U,748U,1204U},
  {2U,446U,566U,1205U},
  {2U,297U,553U,1206U},
  {2U,159U,240U,1207U},
  {2U,261U,504U,1208U},
  {2U,292U,936U,1209U},
  {2U,90U,277U,1210U},
  {2U,196U,311U,1211U},
  {2U,474U,224U,1212U},
  {2U,182U,962U,1213U},
  {2U,413U,114U,1214U},
  {2U,50U,103U,1215U},
  {2U,45U,289U,1216U},
  {2U,70U,198U,1217U},
  {2U,168U,879U,1218U},
  {2U,421U,170U,1219U},
  {2U,392U,829U,1220U},
  {2U,231U,499U,1221U},
  {2U,96U,384U,1222U},
  {2U,302U,76U,1223U},
  {2U,193U,880U,1224U},
  {2U,495U,776U,1225U},
  {2U,280U,696U,1226U},
  {2U,389U,731U,1227U},
  {2U,264U,983U,1228U},
  {2U,172U,267U,1229U},
  {2U,44U,784U,1230U},
  {2U,146U,347U,1231U},
  {2U,124U,766U,1232U},
  {2U,365U,785U,1233U},
  {2U,232U,391U,1234U},
  {2U,22U,742U,1235U},
  {2U,98U,997U,1236U},
  {2U,238U,732U,1237U},
  {2U,291U,998U,1238U},
  {2U,455U,479U,1239U},
  {2U,311U,573U,1240U},
  {2U,509U,425U,1241U},
  {2U,436U,488U,1242U},
  {2U,184U,846U,1243U},
  {2U,1U,681U,1244U},
  {2U,164U,487U,1245U},
  {2U,250U,105U,1246U},
  {2U,235U,19U,1247U},
  {2U,342U,298U,1248U},
  {2U,485U,759U,1249U},
  {2U,161U,521U,1250U},
  {2U,26U,505U,1251U},
  {2U,14U,165U,1252U},
  {2U,364U,760U,1253U},
  {2U,345U,882U,1254U},
  {2U,149U,542U,1255U},
  {2U,303U,462U,1256U},
  {2U,483U,393U,1257U},
  {2U,374U,401U,1258U},
  {2U,471U,279U,1259U},
  {2U,75U,556U,1260U},
  {2U,109U,553U,1261U},
  {2U,228U,788U,1262U},
  {2U,66U,31U,1263U},
  {2U,138U,160U,1264U},
  {2U,184U,594U,1265U},
  {2U,468U,294U,1266U},
  {2U,490U,434U,1267U},
  {2U,23U,850U,1268U},
  {2U,159U,139U,1269U},
  {2U,419U,651U,1270U},
  {2U,109U,150U,1271U},
  {2U,94U,150U,1272U},
  {2U,91U,733U,1273U},
  {2U,226U,512U,1274U},
  {2U,466U,167U,1275U},
  {2U,290U,131U,1276U},
  {2U,181U,822U,1277U},
  {2U,13U,987U,1278U},
  {2U,414U,980U,1279U},
  {2U,199U,794U,1280U},
  {2U,155U,369U,1281U},
  {2U,208U,530U,1282U},
  {2U,320U,436U,1283U},
  {2U,167U,624U,1284U},
  {2U,394U,614U,1285U},
  {2U,465U,110U,1286U},
  {2U,387U,118U,1287U},
  {2U,45U,633U,1288U},
  {2U,464U,787U,1289U},
  {2U,11U,348U,1290U},
  {2U,166U,807U,1291U},
  {2U,277U,736U,1292U},
  {2U,156U,388U,1293U},
  {2U,405U,768U,1294U},
  {2U,422U,94U,1295U},
  {2U,118U,10U,1296U},
  {2U,235U,412U,1297U},
  {2U,452U,370U,1298U},
  {2U,30U,556U,1299U},
  {2U,94U,33U,1300U},
  {2U,257U,266U,1301U},
  {2U,251U,666U,1302U},
  {2U,207U,286U,1303U},
  {2U,214U,483U,1304U},
  {2U,254U,697U,1305U},
  {2U,18U,910U,1306U},
  {2U,384U,540U,1307U},
  {2U,351U,865U,1308U},
  {2U,14U,11U,1309U},
  {2U,209U,23U,1310U},
  {2U,95U,316U,1311U},
  {2U,448U,975U,1312U},
  {2U,277U,298U,1313U},
  {2U,73U,616U,1314U},
  {2U,406U,254U,1315U},
  {2U,356U,245U,1316U},
  {2U,107U,961U,1317U},
  {2U,212U,467U,1318U},
  {2U,33U,695U,1319U},
  {2U,464U,820U,1320U},
  {2U,22U,10U,1321U},
  {2U,394U,227U,1322U},
  {2U,416U,402U,1323U},
  {2U,224U,387U,1324U},
  {2U,251U,88U,1325U},
  {2U,355U,575U,1326U},
  {2U,417U,190U,1327U},
  {2U,49U,321U,1328U},
  {2U,144U,744U,1329U},
  {2U,189U,525U,1330U},
  {2U,13U,368U,1331U},
  {2U,2U,490U,1332U},
  {2U,508U,256U,1333U},
  {2U,446U,556U,1334U},
  {2U,295U,73U,1335U},
  {2U,79U,23U,1336U},
  {2U,377U,334U,1337U},
  {2U,50U,265U,1338U},
  {2U,508U,849U,1339U},
  {2U,351U,715U,1340U},
  {2U,206U,234U,1341U},
  {2U,248U,373U,1342U},
  {2U,1U,452U,1343U},
  {2U,16U,656U,1344U},
  {2U,14U,579U,1345U},
  {2U,39U,230U,1346U},
  {2U,379U,296U,1347U},
  {2U,75U,983U,1348U},
  {2U,504U,605U,1349U},
  {2U,343U,77U,1350U},
  {2U,276U,523U,1351U},
  {2U,391U,883U,1352U},
  {2U,97U,118U,1353U},
  {2U,295U,498U,1354U},
  {2U,262U,96U,1355U},
  {2U,143U,467U,1356U},
  {2U,423U,169U,1357U},
  {2U,125U,517U,1358U},
  {2U,201U,768U,1359U},
  {2U,172U,190U,1360U},
  {2U,250U,915U,1361U},
  {2U,470U,429U,1362U},
  {2U,423U,457U,1363U},
  {2U,153U,335U,1364U},
  {2U,459U,77U,1365U},
  {2U,328U,947U,1366U},
  {2U,314U,84U,1367U},
  {2U,340U,497U,1368U},
  {2U,111U,108U,1369U},
  {2U,239U,942U,1370U},
  {2U,74U,551U,1371U},
  {2U,45U,785U,1372U},
  {2U,369U,640U,1373U},
  {2U,171U,870U,1374U},
  {2U,34U,740U,1375U},
  {2U,45U,746U,1376U},
  {2U,483U,748U,1377U},
  {2U,55U,122U,1378U},
  {2U,486U,162U,1379U},
  {2U,100U,311U,1380U},
  {2U,15U,417U,1381U},
  {2U,139U,610U,1382U},
  {2U,409U,475U,1383U},
  {2U,257U,421U,1384U},
  {2U,410U,441U,1385U},
  {2U,289U,302U,1386U},
  {2U,311U,743U,1387U},
  {2U,326U,532U,1388U},
  {2U,503U,32U,1389U},
  {2U,509U,451U,1390U},
  {2U,265U,327U,1391U},
  {2U,385U,360U,1392U},
  {2U,54U,543U,1393U},
  {2U,265U,998U,1394U},
  {2U,162U,99U,1395U},
  {2U,409U,762U,1396U},
  {2U,383U,323U,1397U},
  {2U,415U,343U,1398U},
  {2U,316U,440U,1399U},
  {2U,357U,761U,1400U},
  {2U,403U,369U,1401U},
  {2U,79U,474U,1402U},
  {2U,345U,253U,1403U},
  {2U,16U,689U,1404U},
  {2U,313U,997U,1405U},
  {2U,431U,549U,1406U},
  {2U,347U,783U,1407U},
  {2U,49U,630U,1408U},
  {2U,142U,559U,1409U},
  {2U,146U,42U,1410U},
  {2U,215U,946U,1411U},
  {2U,227U,131U,1412U},
  {2U,476U,471U,1413U},
  {2U,461U,818U,1414U},
  {2U,272U,805U,1415U},
  {2U,167U,535U,1416U},
  {2U,187U,406U,1417U},
  {2U,75U,204U,1418U},
  {2U,412U,813U,1419U},
  {2U,229U,774U,1420U},
  {2U,298U,664U,1421U},
  {2U,246U,273U,1422U},
  {2U,307U,907U,1423U},
  {2U,61U,113U,1424U},
  {2U,166U,556U,1425U},
  {2U,290U,787U,1426U},
  {2U,92U,222U,1427U},
  {2U,412U,527U,1428U},
  {2U,301U,925U,1429U},
  {2U,106U,892U,1430U},
  {2U,176U,611U,1431U},
  {2U,165U,946U,1432U},
  {2U,417U,750U,1433U},
  {2U,371U,848U,1434U},
  {2U,278U,842U,1435U},
  {2U,197U,924U,1436U},
  {2U,0U,754U,1437U},
  {2U,149U,337U,1438U},
  {2U,280U,356U,1439U},
  {2U,246U,954U,1440U},
  {2U,409U,314U,1441U},
  {2U,113U,153U,1442U},
  {2U,384U,542U,1443U},
  {2U,460U,211U,1444U},
  {2U,343U,882U,1445U},
  {2U,284U,941U,1446U},
  {2U,184U,271U,1447U},
  {2U,12U,675U,1448U},
  {2U,327U,736U,1449U},
  {2U,400U,506U,1450U},
  {2U,208U,618U,1451U},
  {2U,274U,81U,1452U},
  {2U,257U,854U,1453U},
  {2U,197U,52U,1454U},
  {2U,496U,960U,1455U},
  {2U,108U,64U,1456U},
  {2U,484U,805U,1457U},
  {2U,161U,734U,1458U},
  {2U,47U,267U,1459U},
  {2U,177U,836U,1460U},
  {2U,231U,604U,1461U},
  {2U,268U,700U,1462U},
  {2U,50U,699U,1463U},
  {2U,481U,824U,1464U},
  {2U,324U,618U,1465U},
  {2U,54U,793U,1466U},
  {2U,334U,216U,1467U},
  {2U,172U,185U,1468U},
  {2U,74U,939U,1469U},
  {2U,189U,101U,1470U},
  {2U,338U,243U,1471U},
  {2U,293U,875U,1472U},
  {2U,1U,966U,1473U},
  {2U,268U,405U,1474U},
  {2U,5U,792U,1475U},
  {2U,357U,819U,1476U},
  {2U,132U,313U,1477U},
  {2U,152U,235U,1478U},
  {2U,231U,148U,1479U},
  {2U,465U,923U,1480U},
  {2U,95U,504U,1481U},
  {2U,322U,630U,1482U},
  {2U,187U,738U,1483U},
  {2U,165U,963U,1484U},
  {2U,382U,960U,1485U},
  {2U,7U,287U,1486U},
  {2U,205U,897U,1487U},
  {2U,212U,400U,1488U},
  {2U,35U,710U,1489U},
  {2U,479U,761U,1490U},
  {2U,374U,444U,1491U},
  {2U,358U,62U,1492U},
  {2U,483U,181U,1493U},
  {2U,40U,674U,1494U},
  {2U,34U,298U,1495U},
  {2U,372U,528U,1496U},
  {2U,34U,542U,1497U},
  {2U,505U,794U,1498U},
  {2U,303U,435U,1499U},
  {2U,321U,169U,1500U},
  {2U,167U,652U,1501U},
  {2U,415U,148U,1502U},
  {2U,493U,320U,1503U},
  {2U,163U,551U,1504U},
  {2U,222U,801U,1505U},
  {2U,174U,750U,1506U},
  {2U,236U,998U,1507U},
  {2U,337U,8U,1508U},
  {2U,327U,283U,1509U},
  {2U,282U,710U,1510U},
  {2U,254U,970U,1511U},
  {2U,448U,964U,1512U},
  {2U,179U,209U,1513U},
  {2U,82U,573U,1514U},
  {2U,61U,536U,1515U},
  {2U,370U,55U,1516U},
  {2U,57U,887U,1517U},
  {2U,152U,28U,1518U},
  {2U,388U,186U,1519U},
  {2U,439U,120U,1520U},
  {2U,247U,390U,1521U},
  {2U,328U,472U,1522U},
  {2U,377U,758U,1523U},
  {2U,430U,307U,1524U},
  {2U,295U,258U,1525U},
  {2U,83U,238U,1526U},
  {2U,192U,122U,1527U},
  {2U,354U,876U,1528U},
  {2U,96U,976U,1529U},
  {2U,192U,563U,1530U},
  {2U,282U,189U,1531U},
  {2U,106U,176U,1532U},
  {2U,212U,603U,1533U},
  {2U,499U,687U,1534U},
  {2U,282U,90U,1535U},
  {2U,289U,251U,1536U},
  {2U,270U,859U,1537U},
  {2U,372U,403U,1538U},
  {2U,433U,197U,1539U},
  {2U,406U,21U,1540U},
  {2U,69U,822U,1541U},
  {2U,217U,242U,1542U},
  {2U,421U,667U,1543U},
  {2U,180U,667U,1544U},
  {2U,325U,971U,1545U},
  {2U,230U,388U,1546U},
  {2U,133U,922U,1547U},
  {2U,110U,455U,1548U},
  {2U,102U,45U,1549U},
  {2U,465U,624U,1550U},
  {2U,245U,782U,1551U},
  {2U,454U,237U,1552U},
  {2U,209U,400U,1553U},
  {2U,184U,871U,1554U},
  {2U,420U,675U,1555U},
  {2U,14U,682U,1556U},
  {2U,492U,971U,1557U},
  {2U,421U,9U,1558U},
  {2U,234U,579U,1559U},
  {2U,226U,122U,1560U},
  {2U,325U,487U,1561U},
  {2U,261U,897U,1562U},
  {2U,77U,618U,1563U},
  {2U,137U,143U,1564U},
  {2U,450U,640U,1565U},
  {2U,7U,776U,1566U},
  {2U,93U,557U,1567U},
  {2U,434U,482U,1568U},
  {2U,280U,186U,1569U},
  {2U,420U,442U,1570U},
  {2U,371U,9U,1571U},
  {2U,332U,99U,1572U},
  {2U,53U,970U,1573U},
  {2U,413U,222U,1574U},
  {2U,135U,729U,1575U},
  {2U,372U,640U,1576U},
  {2U,161U,794U,1577U},
  {2U,270U,381U,1578U},
  {2U,405U,964U,1579U},
  {2U,64U,568U,1580U},
  {2U,466U,302U,1581U},
  {2U,177U,345U,1582U},
  {2U,124U,558U,1583U},
  {2U,185U,607U,1584U},
  {2U,298U,988U,1585U},
  {2U,100U,543U,1586U},
  {2U,109U,63U,1587U},
  {2U,438U,978U,1588U},
  {2U,35U,543U,1589U},
  {2U,510U,550U,1590U},
  {2U,425U,64U,1591U},
  {2U,257U,298U,1592U},
  {2U,398U,174U,1593U},
  {2U,247U,119U,1594U},
  {2U,79U,23U,1595U},
  {2U,452U,277U,1596U},
  {2U,271U,196U,1597U},
  {2U,486U,172U,1598U},
  {2U,511U,665U,1599U},
  {2U,347U,507U,1600U},
  {2U,507U,641U,1601U},
  {2U,460U,943U,1602U},
  {2U,59U,234U,1603U},
  {2U,186U,783U,1604U},
  {2U,348U,362U,1605U},
  {2U,206U,125U,1606U},
  {2U,493U,675U,1607U},
  {2U,416U,754U,1608U},
  {2U,332U,680U,1609U},
  {2U,485U,548U,1610U},
  {2U,160U,117U,1611U},
  {2U,214U,957U,1612U},
  {2U,236U,374U,1613U},
  {2U,401U,22U,1614U},
  {2U,484U,328U,1615U},
  {2U,269U,232U,1616U},
  {2U,301U,908U,1617U},
  {2U,438U,496U,1618U},
  {2U,434U,834U,1619U},
  {2U,213U,386U,1620U},
  {2U,256U,874U,1621U},
  {2U,290U,925U,1622U},
  {2U,166U,251U,1623U},
  {2U,279U,680U,1624U},
  {2U,218U,59U,1625U},
  {2U,67U,960U,1626U},
  {2U,30U,669U,1627U},
  {2U,298U,19U,1628U},
  {2U,345U,638U,1629U},
  {2U,248U,778U,1630U},
  {2U,102U,812U,1631U},
  {2U,84U,833U,1632U},
  {2U,434U,633U,1633U},
  {2U,56U,49U,1634U},
  {2U,252U,29U,1635U},
  {2U,502U,853U,1636U},
  {2U,332U,979U,1637U},
  {2U,314U,592U,1638U},
  {2U,141U,679U,1639U},
  {2U,479U,608U,1640U},
  {2U,270U,329U,1641U},
  {2U,234U,136U,1642U},
  {2U,264U,34U,1643U},
  {2U,51U,963U,1644U},
  {2U,212U,205U,1645U},
  {2U,208U,942U,1646U},
  {2U,244U,226U,1647U},
  {2U,65U,65U,1648U},
  {2U,445U,785U,1649U},
  {2U,472U,345U,1650U},
  {2U,399U,725U,1651U},
  {2U,208U,207U,1652U},
  {2U,148U,949U,1653U},
  {2U,330U,558U,1654U},
  {2U,61U,797U,1655U},
  {2U,495U,393U,1656U},
  {2U,433U,458U,1657U},
  {2U,51U,703U,1658U},
  {2U,387U,36U,1659U},
  {2U,379U,141U,1660U},
  {2U,44U,562U,1661U},
  {2U,506U,468U,1662U},
  {2U,397U,928U,1663U},
  {2U,319U,457U,1664U},
  {2U,469U,824U,1665U},
  {2U,193U,466U,1666U},
  {2U,231U,845U,1667U},
  {2U,461U,752U,1668U},
  {2U,483U,991U,1669U},
  {2U,502U,702U,1670U},
  {2U,332U,657U,1671U},
  {2U,444U,988U,1672U},
  {2U,209U,126U,1673U},
  {2U,156U,321U,1674U},
  {2U,347U,478U,1675U},
  {2U,85U,348U,1676U},
  {2U,257U,370U,1677U},
  {2U,381U,653U,1678U},
  {2U,122U,965U,1679U},
  {2U,185U,738U,1680U},
  {2U,380U,441U,1681U},
  {2U,499U,555U,1682U},
  {2U,332U,580U,1683U},
  {2U,217U,369U,1684U},
  {2U,348U,835U,1685U},
  {2U,429U,410U,1686U},
  {2U,327U,292U,1687U},
  {2U,125U,874U,1688U},
  {2U,478U,575U,1689U},
  {2U,435U,307U,1690U},
  {2U,230U,911U,1691U},
  {2U,93U,921U,1692U},
  {2U,34U,627U,1693U},
  {2U,487U,605U,1694U},
  {2U,370U,199U,1695U},
  {2U,192U,316U,1696U},
  {2U,291U,836U,1697U},
  {2U,60U,422U,1698U},
  {2U,307U,956U,1699U},
  {2U,353U,672U,1700U},
  {2U,466U,760U,1701U},
  {2U,413U,471U,1702U},
  {2U,248U,41U,1703U},
  {2U,381U,723U,1704U},
  {2U,29U,683U,1705U},
  {2U,105U,953U,1706U},
  {2U,18U,792U,1707U},
  {2U,451U,607U,1708U},
  {2U,89U,497U,1709U},
  {2U,33U,73U,1710U},
  {2U,90U,493U,1711U},
  {2U,100U,242U,1712U},
  {2U,393U,209U,1713U},
  {2U,127U,712U,1714U},
  {2U,102U,263U,1715U},
  {2U,216U,295U,1716U},
  {2U,326U,137U,1717U},
  {2U,468U,790U,1718U},
  {2U,291U,331U,1719U},
  {2U,81U,495U,1720U},
  {2U,112U,878U,1721U},
  {2U,381U,938U,1722U},
  {2U,325U,80U,1723U},
  {2U,395U,985U,1724U},
  {2U,65U,792U,1725U},
  {2U,426U,611U,1726U},
  {2U,124U,610U,1727U},
  {2U,87U,783U,1728U},
  {2U,47U,594U,1729U},
  {2U,18U,809U,1730U},
  {2U,327U,298U,1731U},
  {2U,92U,490U,1732U},
  {2U,88U,902U,1733U},
  {2U,73U,729U,1734U},
  {2U,237U,328U,1735U},
  {2U,16U,575U,1736U},
  {2U,436U,522U,1737U},
  {2U,231U,646U,1738U},
  {2U,100U,373U,1739U},
  {2U,243U,306U,1740U},
  {2U,316U,27U,1741U},
  {2U,49U,681U,1742U},
  {2U,260U,675U,1743U},
  {2U,452U,453U,1744U},
  {2U,53U,241U,1745U},
  {2U,97U,289U,1746U},
  {2U,263U,349U,1747U},
  {2U,420U,417U,1748U},
  {2U,221U,195U,1749U},
  {2U,394U,577U,1750U},
  {2U,292U,857U,1751U},
  {2U,125U,145U,1752U},
  {2U,256U,27U,1753U},
  {2U,22U,660U,1754U},
  {2U,184U,622U,1755U},
  {2U,492U,692U,1756U},
  {2U,320U,444U,1757U},
  {2U,96U,774U,1758U},
  {2U,140U,844U,1759U},
  {2U,318U,850U,1760U},
  {2U,417U,272U,1761U},
  {2U,353U,387U,1762U},
  {2U,484U,451U,1763U},
  {2U,7U,114U,1764U},
  {2U,313U,247U,1765U},
  {2U,173U,431U,1766U},
  {2U,7U,201U,1767U},
  {2U,474U,132U,1768U},
  {2U,200U,421U,1769U},
  {2U,66U,268U,1770U},
  {2U,481U,636U,1771U},
  {2U,181U,607U,1772U},
  {2U,218U,230U,1773U},
  {2U,9U,139U,1774U},
  {2U,442U,209U,1775U},
  {2U,346U,573U,1776U},
  {2U,244U,333U,1777U},
  {2U,209U,294U,1778U},
  {2U,157U,212U,1779U},
  {2U,487U,967U,1780U},
  {2U,356U,766U,1781U},
  {2U,87U,71U,1782U},
  {2U,311U,400U,1783U},
  {2U,445U,481U,1784U},
  {2U,76U,928U,1785U},
  {2U,171U,796U,1786U},
  {2U,353U,607U,1787U},
  {2U,90U,506U,1788U},
  {2U,144U,47U,1789U},
  {2U,37U,343U,1790U},
  {2U,92U,605U,1791U},
  {2U,52U,629U,1792U},
  {2U,290U,957U,1793U},
  {2U,227U,779U,1794U},
  {2U,464U,287U,1795U},
  {2U,405U,404U,1796U},
  {2U,255U,793U,1797U},
  {2U,318U,159U,1798U},
  {2U,222U,456U,1799U},
  {2U,284U,542U,1800U},
  {2U,189U,237U,1801U},
  {2U,396U,484U,1802U},
  {2U,491U,477U,1803U},
  {2U,120U,423U,1804U},
  {2U,91U,980U,1805U},
  {2U,398U,332U,1806U},
  {2U,28U,314U,1807U},
  {2U,375U,400U,1808U},
  {2U,386U,381U,1809U},
  {2U,253U,283U,1810U},
  {2U,213U,296U,1811U},
  {2U,473U,463U,1812U},
  {2U,2U,180U,1813U},
  {2U,508U,819U,1814U},
  {2U,491U,852U,1815U},
  {2U,235U,742U,1816U},
  {2U,145U,966U,1817U},
  {2U,396U,397U,1818U},
  {2U,122U,995U,1819U},
  {2U,155U,278U,1820U},
  {2U,366U,111U,1821U},
  {2U,91U,539U,1822U},
  {2U,260U,161U,1823U},
  {2U,15U,24U,1824U},
  {2U,356U,454U,1825U},
  {2U,252U,937U,1826U},
  {2U,503U,103U,1827U},
  {2U,364U,658U,1828U},
  {2U,242U,772U,1829U},
  {2U,417U,221U,1830U},
  {2U,484U,731U,1831U},
  {2U,236U,347U,1832U},
  {2U,291U,214U,1833U},
  {2U,335U,696U,1834U},
  {2U,47U,484U,1835U},
  {2U,487U,768U,1836U},
  {2U,455U,279U,1837U},
  {2U,347U,568U,1838U},
  {2U,199U,567U,1839U},
  {2U,174U,372U,1840U},
  {2U,465U,856U,1841U},
  {2U,321U,83U,1842U},
  {2U,348U,796U,1843U},
  {2U,306U,171U,1844U},
  {2U,78U,373U,1845U},
  {2U,108U,805U,1846U},
  {2U,267U,322U,1847U},
  {2U,436U,768U,1848U},
  {2U,398U,253U,1849U},
  {2U,262U,285U,1850U},
  {2U,165U,717U,1851U},
  {2U,30U,388U,1852U},
  {2U,156U,403U,1853U},
  {2U,58U,636U,1854U},
  {2U,422U,199U,1855U},
  {2U,503U,701U,1856U},
  {2U,224U,581U,1857U},
  {2U,484U,655U,1858U},
  {2U,31U,44U,1859U},
  {2U,356U,999U,1860U},
  {2U,161U,630U,1861U},
  {2U,197U,235U,1862U},
  {2U,378U,611U,1863U},
  {2U,80U,766U,1864U},
  {2U,275U,927U,1865U},
  {2U,135U,841U,1866U},
  {2U,94U,626U,1867U},
  {2U,247U,151U,1868U},
  {2U,171U,438U,1869U},
  {2U,231U,614U,1870U},
  {2U,292U,532U,1871U},
  {2U,260U,774U,1872U},
  {2U,388U,42U,1873U},
  {2U,45U,606U,1874U},
  {2U,173U,494U,1875U},
  {2U,397U,520U,1876U},
  {2U,353U,325U,1877U},
  {2U,335U,161U,1878U},
  {2U,405U,595U,1879U},
  {2U,21U,857U,1880U},
  {2U,486U,146U,1881U},
  {2U,178U,144U,1882U},
  {2U,130U,684U,1883U},
  {2U,22U,605U,1884U},
  {2U,308U,725U,1885U},
  {2U,221U,241U,1886U},
  {2U,239U,632U,1887U},
  {2U,488U,295U,1888U},
  {2U,421U,981U,1889U},
  {2U,335U,498U,1890U},
  {2U,150U,111U,1891U},
  {2U,195U,474U,1892U},
  {2U,54U,251U,1893U},
  {2U,365U,443U,1894U},
  {2U,129U,681U,1895U},
  {2U,326U,626U,1896U},
  {2U,6U,87U,1897U},
  {2U,418U,620U,1898U},
  {2U,176U,558U,1899U},
  {2U,86U,381U,1900U},
  {2U,397U,839U,1901U},
  {2U,396U,818U,1902U},
  {2U,337U,899U,1903U},
  {2U,502U,188U,1904U},
  {2U,352U,404U,1905U},
  {2U,412U,963U,1906U},
  {2U,162U,602U,1907U},
  {2U,276U,634U,1908U},
  {2U,286U,885U,1909U},
  {2U,45U,571U,1910U},
  {2U,440U,471U,1911U},
  {2U,344U,112U,1912U},
  {2U,216U,898U,1913U},
  {2U,27U,593U,1914U},
  {2U,279U,577U,1915U},
  {2U,476U,691U,1916U},
  {2U,6U,412U,1917U},
  {2U,313U,731U,1918U},
  {2U,439U,16U,1919U},
  {2U,496U,662U,1920U},
  {2U,261U,848U,1921U},
  {2U,154U,741U,1922U},
  {2U,386U,864U,1923U},
  {2U,7U,665U,1924U},
  {2U,347U,97U,1925U},
  {2U,443U,762U,1926U},
  {2U,357U,665U,1927U},
  {2U,384U,461U,1928U},
  {2U,453U,598U,1929U},
  {2U,231U,571U,1930U},
  {2U,390U,200U,1931U},
  {2U,22U,842U,1932U},
  {2U,176U,541U,1933U},
  {2U,230U,932U,1934U},
  {2U,402U,137U,1935U},
  {2U,243U,191U,1936U},
  {2U,285U,625U,1937U},
  {2U,115U,707U,1938U},
  {2U,394U,857U,1939U},
  {2U,477U,109U,1940U},
  {2U,459U,744U,1941U},
  {2U,320U,325U,1942U},
  {2U,511U,736U,1943U},
  {2U,444U,722U,1944U},
  {2U,497U,783U,1945U},
  {2U,331U,232U,1946U},
  {2U,10U,808U,1947U},
  {2U,392U,774U,1948U},
  {2U,416U,126U,1949U},
  {2U,45U,689U,1950U},
  {2U,65U,594U,1951U},
  {2U,42U,790U,1952U},
  {2U,487U,314U,1953U},
  {2U,279U,103U,1954U},
  {2U,64U,85U,1955U},
  {2U,455U,563U,1956U},
  {2U,70U,922U,1957U},
  {2U,281U,515U,1958U},
  {2U,357U,607U,1959U},
  {2U,390U,260U,1960U},
  {2U,250U,747U,1961U},
  {2U,411U,230U,1962U},
  {2U,17U,891U,1963U},
  {2U,301U,342U,1964U},
  {2U,121U,66U,1965U},
  {2U,114U,835U,1966U},
  {2U,239U,595U,1967U},
  {2U,182U,850U,1968U},
  {2U,466U,641U,1969U},
  {2U,495U,345U,1970U},
  {2U,297U,73U,1971U},
  {2U,48U,322U,1972U},
  {2U,332U,401U,1973U},
  {2U,89U,215U,1974U},
  {2U,399U,947U,1975U},
  {2U,415U,131U,1976U},
  {2U,277U,945U,1977U},
  {2U,211U,475U,1978U},
  {2U,199U,620U,1979U},
  {2U,117U,608U,1980U},
  {2U,313U,666U,1981U},
  {2U,402U,999U,1982U},
  {2U,407U,478U,1983U},
  {2U,88U,976U,1984U},
  {2U,413U,672U,1985U},
  {2U,272U,523U,1986U},
  {2U,71U,385U,1987U},
  {2U,386U,143U,1988U},
  {2U,88U,55U,1989U},
  {2U,357U,307U,1990U},
  {2U,234U,8U,1991U},
  {2U,122U,46U,1992U},
  {2U,469U,345U,1993U},
  {2U,214U,656U,1994U},
  {2U,146U,785U,1995U},
  {2U,433U,238U,1996U},
  {2U,450U,621U,1997U},
  {2U,63U,328U,1998U},
  {2U,233U,979U,1999U},
  {2U,506U,57U,2000U},
  {2U,188U,768U,2001U},
  {2U,234U,33U,2002U},
  {2U,38U,248U,2003U},
  {2U,78U,860U,2004U},
  {2U,198U,284U,2005U},
  {2U,258U,716U,2006U},
  {2U,362U,964U,2007U},
  {2U,40U,494U,2008U},
  {2U,213U,385U,2009U},
  {2U,2U,730U,2010U},
  {2U,468U,666U,2011U},
  {2U,454U,488U,2012U},
  {2U,320U,790U,2013U},
  {2U,469U,158U,2014U},
  {2U,449U,62U,2015U},
  {2U,81U,113U,2016U},
  {2U,146U,738U,2017U},
  {2U,228U,435U,2018U},
  {2U,290U,404U,2019U},
  {2U,184U,698U,2020U},
  {2U,466U,781U,2021U},
  {2U,302U,390U,2022U},
  {2U,159U,546U,2023U},
  {2U,496U,893U,2024U},
  {2U,290U,562U,2025U},
  {2U,309U,243U,2026U},
  {2U,45U,833U,2027U},
  {2U,61U,823U,2028U},
  {2U,75U,683U,2029U},
  {2U,285U,921U,2030U},
  {2U,505U,721U,2031U},
  {2U,371U,86U,2032U},
  {2U,1U,876U,2033U},
  {2U,56U,161U,2034U},
  {2U,120U,25U,2035U},
  {2U,300U,868U,2036U},
  {2U,252U,20U,2037U},
  {2U,327U,896U,2038U},
  {2U,455U,352U,2039U},
  {2U,77U,494U,2040U},
  {2U,463U,871U,2041U},
  {2U,135U,581U,2042U},
  {2U,394U,671U,2043U},
  {2U,327U,634U,2044U},
  {2U,184U,444U,2045U},
  {2U,292U,750U,2046U},
  {2U,166U,936U,2047U},
  {2U,364U,742U,2048U},
  {2U,12U,899U,2049U},
  {2U,178U,441U,2050U},
  {2U,73U,662U,2051U},
  {2U,139U,332U,2052U},
  {2U,86U,927U,2053U},
  {2U,371U,495U,2054U},
  {2U,44U,371U,2055U},
  {2U,225U,431U,2056U},
  {2U,296U,97U,2057U},
  {2U,453U,757U,2058U},
  {2U,388U,225U,2059U},
  {2U,262U,411U,2060U},
  {2U,391U,345U,2061U},
  {2U,257U,237U,2062U},
  {2U,352U,525U,2063U},
  {2U,379U,274U,2064U},
  {2U,31U,770U,2065U},
  {2U,283U,809U,2066U},
  {2U,253U,873U,2067U},
  {2U,452U,96U,2068U},
  {2U,385U,870U,2069U},
  {2U,490U,96U,2070U},
  {2U,287U,405U,2071U},
  {2U,221U,33U,2072U},
  {2U,334U,352U,2073U},
  {2U,432U,374U,2074U},
  {2U,110U,696U,2075U},
  {2U,36U,248U,2076U},
  {2U,286U,287U,2077U},
  {2U,354U,179U,2078U},
  {2U,74U,62U,2079U},
  {2U,144U,45U,2080U},
  {2U,80U,975U,2081U},
  {2U,142U,323U,2082U},
  {2U,65U,272U,2083U},
  {2U,371U,406U,2084U},
  {2U,199U,58U,2085U},
  {2U,260U,567U,2086U},
  {2U,45U,650U,2087U},
  {2U,123U,952U,2088U},
  {2U,372U,76U,2089U},
  {2U,446U,252U,2090U},
  {2U,132U,243U,2091U},
  {2U,274U,478U,2092U},
  {2U,197U,60U,2093U},
  {2U,470U,730U,2094U},
  {2U,297U,975U,2095U},
  {2U,286U,402U,2096U},
  {2U,429U,622U,2097U},
  {2U,46U,100U,2098U},
  {2U,446U,710U,2099U},
  {2U,270U,27U,2100U},
  {2U,59U,886U,2101U},
  {2U,325U,998U,2102U},
  {2U,320U,706U,2103U},
  {2U,151U,522U,2104U},
  {2U,477U,918U,2105U},
  {2U,489U,585U,2106U},
  {2U,178U,594U,2107U},
  {2U,135U,17U,2108U},
  {2U,222U,392U,2109U},
  {2U,425U,880U,2110U},
  {2U,398U,456U,2111U},
  {2U,498U,131U,2112U},
  {2U,338U,736U,2113U},
  {2U,320U,223U,2114U},
  {2U,414U,142U,2115U},
  {2U,125U,518U,2116U},
  {2U,152U,925U,2117U},
  {2U,173U,701U,2118U},
  {2U,311U,130U,2119U},
  {2U,16U,241U,2120U},
  {2U,225U,110U,2121U},
  {2U,23U,422U,2122U},
  {2U,239U,342U,2123U},
  {2U,198U,521U,2124U},
  {2U,337U,479U,2125U},
  {2U,22U,205U,2126U},
  {2U,282U,655U,2127U},
  {2U,153U,536U,2128U},
  {2U,303U,579U,2129U},
  {2U,491U,657U,2130U},
  {2U,281U,111U,2131U},
  {2U,187U,554U,2132U},
  {2U,375U,92U,2133U},
  {2U,439U,712U,2134U},
  {2U,322U,496U,2135U},
  {2U,235U,216U,2136U},
  {2U,147U,465U,2137U},
  {2U,252U,957U,2138U},
  {2U,496U,60U,2139U},
  {2U,73U,279U,2140U},
  {2U,33U,300U,2141U},
  {2U,18U,783U,2142U},
  {2U,190U,281U,2143U},
  {2U,326U,505U,2144U},
  {2U,128U,78U,2145U},
  {2U,147U,655U,2146U},
  {2U,185U,319U,2147U},
  {2U,67U,278U,2148U},
  {2U,119U,360U,2149U},
  {2U,111U,715U,2150U},
  {2U,104U,649U,2151U},
  {2U,2U,853U,2152U},
  {2U,337U,941U,2153U},
  {2U,316U,145U,2154U},
  {2U,158U,374U,2155U},
  {2U,31U,65U,2156U},
  {2U,107U,398U,2157U},
  {2U,106U,729U,2158U},
  {2U,110U,11U,2159U},
  {2U,278U,646U,2160U},
  {2U,448U,446U,2161U},
  {2U,271U,317U,2162U},
  {2U,366U,785U,2163U},
  {2U,11U,24U,2164U},
  {2U,491U,565U,2165U},
  {2U,98U,467U,2166U},
  {2U,58U,796U,2167U},
  {2U,172U,32U,2168U},
  {2U,129U,242U,2169U},
  {2U,449U,263U,2170U},
  {2U,154U,604U,2171U},
  {2U,90U,652U,2172U},
  {2U,262U,629U,2173U},
  {2U,222U,536U,2174U},
  {2U,69U,370U,2175U},
  {2U,237U,596U,2176U},
  {2U,85U,533U,2177U},
  {2U,241U,211U,2178U},
  {2U,178U,323U,2179U},
  {2U,10U,688U,2180U},
  {2U,366U,435U,2181U},
  {2U,278U,545U,2182U},
  {2U,157U,408U,2183U},
  {2U,395U,578U,2184U},
  {2U,252U,860U,2185U},
  {2U,39U,116U,2186U},
  {2U,236U,167U,2187U},
  {2U,465U,933U,2188U},
  {2U,93U,760U,2189U},
  {2U,225U,944U,2190U},
  {2U,361U,636U,2191U},
  {2U,302U,570U,2192U},
  {2U,127U,414U,2193U},
  {2U,191U,549U,2194U},
  {2U,289U,621U,2195U},
  {2U,299U,837U,2196U},
  {2U,23U,122U,2197U},
  {2U,257U,718U,2198U},
  {2U,216U,525U,2199U},
  {2U,371U,680U,2200U},
  {2U,98U,700U,2201U},
  {2U,258U,199U,2202U},
  {2U,386U,863U,2203U},
  {2U,112U,296U,2204U},
  {2U,236U,898U,2205U},
  {2U,414U,24U,2206U},
  {2U,19U,14U,2207U},
  {2U,296U,643U,2208U},
  {2U,500U,403U,2209U},
  {2U,160U,180U,2210U},
  {2U,324U,269U,2211U},
  {2U,509U,346U,2212U},
  {2U,101U,979U,2213U},
  {2U,36U,687U,2214U},
  {2U,204U,839U,2215U},
  {2U,195U,164U,2216U},
  {2U,425U,113U,2217U},
  {2U,98U,810U,2218U},
  {2U,157U,530U,2219U},
  {2U,478U,176U,2220U},
  {2U,121U,446U,2221U},
  {2U,7U,175U,2222U},
  {2U,218U,687U,2223U},
  {2U,167U,297U,2224U},
  {2U,29U,873U,2225U},
  {2U,104U,793U,2226U},
  {2U,306U,563U,2227U},
  {2U,246U,815U,2228U},
  {2U,229U,449U,2229U},
  {2U,185U,258U,2230U},
  {2U,354U,27U,2231U},
  {2U,121U,547U,2232U},
  {2U,386U,284U,2233U},
  {2U,9U,624U,2234U},
  {2U,69U,299U,2235U},
  {2U,463U,190U,2236U},
  {2U,401U,836U,2237U},
  {2U,261U,696U,2238U},
  {2U,61U,538U,2239U},
  {2U,281U,82U,2240U},
  {2U,353U,267U,2241U},
  {2U,50U,229U,2242U},
  {2U,31U,565U,2243U},
  {2U,141U,836U,2244U},
  {2U,42U,768U,2245U},
  {2U,418U,499U,2246U},
  {2U,459U,491U,2247U},
  {2U,477U,218U,2248U},
  {2U,392U,886U,2249U},
  {2U,239U,91U,2250U},
  {2U,246U,771U,2251U},
  {2U,215U,120U,2252U},
  {2U,24U,400U,2253U},
  {2U,302U,442U,2254U},
  {2U,265U,898U,2255U},
  {2U,164U,519U,2256U},
  {2U,450U,653U,2257U},
  {2U,402U,276U,2258U},
  {2U,267U,248U,2259U},
  {2U,44U,731U,2260U},
  {2U,306U,843U,2261U},
  {2U,225U,646U,2262U},
  {2U,113U,898U,2263U},
  {2U,85U,253U,2264U},
  {2U,14U,341U,2265U},
  {2U,203U,448U,2266U},
  {2U,224U,748U,2267U},
  {2U,228U,485U,2268U},
  {2U,347U,675U,2269U},
  {2U,186U,258U,2270U},
  {2U,370U,238U,2271U},
  {2U,92U,681U,2272U},
  {2U,399U,258U,2273U},
  {2U,49U,85U,2274U},
  {2U,497U,487U,2275U},
  {2U,467U,538U,2276U},
  {2U,191U,181U,2277U},
  {2U,101U,776U,2278U},
  {2U,482U,536U,2279U},
  {2U,127U,857U,2280U},
  {2U,315U,891U,2281U},
  {2U,221U,565U,2282U},
  {2U,121U,244U,2283U},
  {2U,306U,458U,2284U},
  {2U,478U,919U,2285U},
  {2U,204U,794U,2286U},
  {2U,83U,768U,2287U},
  {2U,338U,489U,2288U},
  {2U,398U,448U,2289U},
  {2U,509U,517U,2290U},
  {2U,483U,472U,2291U},
  {2U,15U,249U,2292U},
  {2U,229U,89U,2293U},
  {2U,399U,180U,2294U},
  {2U,470U,237U,2295U},
  {2U,153U,814U,2296U},
  {2U,306U,284U,2297U},
  {2U,390U,257U,2298U},
  {2U,314U,802U,2299U},
  {2U,25U,951U,2300U},
  {2U,181U,622U,2301U},
  {2U,347U,611U,2302U},
  {2U,295U,76U,2303U},
  {2U,81U,96U,2304U},
  {2U,145U,497U,2305U},
  {2U,214U,326U,2306U},
  {2U,354U,57U,2307U},
  {2U,176U,479U,2308U},
  {2U,470U,662U,2309U},
  {2U,40U,954U,2310U},
  {2U,35U,250U,2311U},
  {2U,218U,404U,2312U},
  {2U,397U,408U,2313U},
  {2U,64U,259U,2314U},
  {2U,257U,931U,2315U},
  {2U,277U,555U,2316U},
  {2U,217U,192U,2317U},
  {2U,496U,613U,2318U},
  {2U,406U,981U,2319U},
  {2U,49U,446U,2320U},
  {2U,14U,484U,2321U},
  {2U,438U,794U,2322U},
  {2U,454U,682U,2323U},
  {2U,276U,228U,2324U},
  {2U,73U,577U,2325U},
  {2U,98U,222U,2326U},
  {2U,273U,118U,2327U},
  {2U,189U,354U,2328U},
  {2U,343U,200U,2329U},
  {2U,429U,765U,2330U},
  {2U,462U,883U,2331U},
  {2U,380U,998U,2332U},
  {2U,397U,593U,2333U},
  {2U,178U,108U,2334U},
  {2U,421U,904U,2335U},
  {2U,143U,755U,2336U},
  {2U,210U,125U,2337U},
  {2U,67U,384U,2338U},
  {2U,10U,48U,2339U},
  {2U,171U,760U,2340U},
  {2U,81U,642U,2341U},
  {2U,103U,857U,2342U},
  {2U,110U,468U,2343U},
  {2U,437U,855U,2344U},
  {2U,96U,853U,2345U},
  {2U,471U,841U,2346U},
  {2U,357U,121U,2347U},
  {2U,172U,710U,2348U},
  {2U,261U,262U,2349U},
  {2U,4U,565U,2350U},
  {2U,134U,158U,2351U},
  {2U,329U,662U,2352U},
  {2U,432U,278U,2353U},
  {2U,275U,716U,2354U},
  {2U,199U,543U,2355U},
  {2U,227U,233U,2356U},
  {2U,395U,198U,2357U},
  {2U,244U,961U,2358U},
  {2U,499U,952U,2359U},
  {2U,235U,457U,2360U},
  {2U,325U,733U,2361U},
  {2U,147U,42U,2362U},
  {2U,414U,946U,2363U},
  {2U,273U,438U,2364U},
  {2U,11U,775U,2365U},
  {2U,244U,341U,2366U},
  {2U,112U,806U,2367U},
  {2U,259U,631U,2368U},
  {2U,502U,147U,2369U},
  {2U,237U,982U,2370U},
  {2U,111U,34U,2371U},
  {2U,184U,97U,2372U},
  {2U,406U,28U,2373U},
  {2U,319U,859U,2374U},
  {2U,456U,339U,2375U},
  {2U,32U,604U,2376U},
  {2U,348U,946U,2377U},
  {2U,73U,462U,2378U},
  {2U,157U,356U,2379U},
  {2U,322U,714U,2380U},
  {2U,466U,593U,2381U},
  {2U,223U,639U,2382U},
  {2U,463U,25U,2383U},
  {2U,39U,758U,2384U},
  {2U,104U,26U,2385U},
  {2U,164U,120U,2386U},
  {2U,330U,853U,2387U},
  {2U,183U,863U,2388U},
  {2U,176U,134U,2389U},
  {2U,83U,149U,2390U},
  {2U,116U,184U,2391U},
  {2U,426U,320U,2392U},
  {2U,90U,694U,2393U},
  {2U,244U,163U,2394U},
  {2U,434U,553U,2395U},
  {2U,116U,299U,2396U},
  {2U,401U,550U,2397U},
  {2U,47U,468U,2398U},
  {2U,263U,154U,2399U},
  {2U,61U,662U,2400U},
  {2U,228U,221U,2401U},
  {2U,226U,74U,2402U},
  {2U,450U,13U,2403U},
  {2U,282U,976U,2404U},
  {2U,84U,847U,2405U},
  {2U,309U,931U,2406U},
  {2U,11U,377U,2407U},
  {2U,32U,996U,2408U},
  {2U,390U,291U,2409U},
  {2U,305U,694U,2410U},
  {2U,496U,377U,2411U},
  {2U,381U,919U,2412U},
  {2U,105U,383U,2413U},
  {2U,458U,378U,2414U},
  {2U,191U,576U,2415U},
  {2U,305U,973U,2416U},
  {2U,110U,428U,2417U},
  {2U,275U,393U,2418U},
  {2U,171U,688U,2419U},
  {2U,260U,518U,2420U},
  {2U,25U,404U,2421U},
  {2U,393U,654U,2422U},
  {2U,481U,139U,2423U},
  {2U,176U,646U,2424U},
  {2U,194U,609U,2425U},
  {2U,217U,169U,2426U},
  {2U,58U,240U,2427U},
  {2U,122U,972U,2428U},
  {2U,6U,21U,2429U},
  {2U,219U,988U,2430U},
  {2U,99U,70U,2431U},
  {2U,143U,229U,2432U},
  {2U,196U,406U,2433U},
  {2U,328U,140U,2434U},
  {2U,220U,710U,2435U},
  {2U,1U,562U,2436U},
  {2U,30U,549U,2437U},
  {2U,55U,427U,2438U},
  {2U,38U,999U,2439U},
  {2U,119U,487U,2440U},
  {2U,496U,269U,2441U},
  {2U,71U,925U,2442U},
  {2U,466U,287U,2443U},
  {2U,343U,721U,2444U},
  {2U,242U,247U,2445U},
  {2U,216U,108U,2446U},
  {2U,71U,554U,2447U},
  {2U,62U,693U,2448U},
  {2U,409U,960U,2449U},
  {2U,78U,374U,2450U},
  {2U,53U,53U,2451U},
  {2U,47U,50U,2452U},
  {2U,160U,765U,2453U},
  {2U,326U,450U,2454U},
  {2U,71U,982U,2455U},
  {2U,217U,272U,2456U},
  {2U,184U,207U,2457U},
  {2U,259U,512U,2458U},
  {2U,326U,933U,2459U},
  {2U,480U,147U,2460U},
  {2U,327U,479U,2461U},
  {2U,166U,807U,2462U},
  {2U,496U,110U,2463U},
  {2U,116U,623U,2464U},
  {2U,52U,293U,2465U},
  {2U,427U,322U,2466U},
  {2U,249U,66U,2467U},
  {2U,339U,161U,2468U},
  {2U,131U,825U,2469U},
  {2U,61U,163U,2470U},
  {2U,100U,412U,2471U},
  {2U,39U,392U,2472U},
  {2U,342U,953U,2473U},
  {2U,204U,547U,2474U},
  {2U,394U,572U,2475U},
  {2U,222U,737U,2476U},
  {2U,21U,746U,2477U},
  {2U,462U,397U,2478U},
  {2U,74U,651U,2479U},
  {2U,82U,30U,2480U},
  {2U,155U,310U,2481U},
  {2U,135U,644U,2482U},
  {2U,410U,984U,2483U},
  {2U,138U,857U,2484U},
  {2U,214U,192U,2485U},
  {2U,227U,763U,2486U},
  {2U,131U,784U,2487U},
  {2U,235U,100U,2488U},
  {2U,65U,95U,2489U},
  {2U,168U,534U,2490U},
  {2U,126U,886U,2491U},
  {2U,209U,858U,2492U},
  {2U,152U,282U,2493U},
  {2U,91U,683U,2494U},
  {2U,71U,676U,2495U},
  {2U,223U,350U,2496U},
  {2U,157U,840U,2497U},
  {2U,191U,321U,2498U},
  {2U,121U,423U,2499U},
  {2U,458U,65U,2500U},
  {2U,9U,275U,2501U},
  {2U,226U,817U,2502U},
  {2U,294U,274U,2503U},
  {2U,382U,243U,2504U},
  {2U,277U,341U,2505U},
  {2U,240U,390U,2506U},
  {2U,181U,354U,2507U},
  {2U,497U,337U,2508U},
  {2U,206U,845U,2509U},
  {2U,413U,233U,2510U},
  {2U,64U,383U,2511U},
  {2U,128U,644U,2512U},
  {2U,66U,881U,2513U},
  {2U,338U,405U,2514U},
  {2U,398U,27U,2515U},
  {2U,465U,20U,2516U},
  {2U,179U,524U,2517U},
  {2U,449U,889U,2518U},
  {2U,455U,165U,2519U},
  {2U,425U,199U,2520U},
  {2U,164U,462U,2521U},
  {2U,491U,626U,2522U},
  {2U,247U,378U,2523U},
  {2U,266U,545U,2524U},
  {2U,443U,115U,2525U},
  {2U,335U,937U,2526U},
  {2U,410U,98U,2527U},
  {2U,361U,480U,2528U},
  {2U,82U,85U,2529U},
  {2U,144U,314U,2530U},
  {2U,459U,579U,2531U},
  {2U,4U,132U,2532U},
  {2U,481U,773U,2533U},
  {2U,123U,886U,2534U},
  {2U,269U,770U,2535U},
  {2U,135U,677U,2536U},
  {2U,243U,113U,2537U},
  {2U,69U,216U,2538U},
  {2U,276U,910U,2539U},
  {2U,363U,909U,2540U},
  {2U,3U,638U,2541U},
  {2U,492U,888U,2542U},
  {2U,162U,478U,2543U},
  {2U,450U,543U,2544U},
  {2U,386U,246U,2545U},
  {2U,123U,349U,2546U},
  {2U,505U,288U,2547U},
  {2U,31U,682U,2548U},
  {2U,5U,556U,2549U},
  {2U,444U,701U,2550U},
  {2U,110U,363U,2551U},
  {2U,367U,526U,2552U},
  {2U,466U,173U,2553U},
  {2U,227U,644U,2554U},
  {2U,299U,672U,2555U},
  {2U,503U,206U,2556U},
  {2U,51U,266U,2557U},
  {2U,156U,551U,2558U},
  {2U,423U,438U,2559U},
  {2U,14U,348U,2560U},
  {2U,114U,888U,2561U},
  {2U,362U,967U,2562U},
  {2U,98U,238U,2563U},
  {2U,32U,20U,2564U},
  {2U,88U,971U,2565U},
  {2U,467U,113U,2566U},
  {2U,473U,903U,2567U},
  {2U,315U,183U,2568U},
  {2U,302U,722U,2569U},
  {2U,349U,87U,2570U},
  {2U,505U,208U,2571U},
  {2U,257U,904U,2572U},
  {2U,208U,411U,2573U},
  {2U,212U,776U,2574U},
  {2U,266U,898U,2575U},
  {2U,228U,806U,2576U},
  {2U,503U,743U,2577U},
  {2U,423U,665U,2578U},
  {2U,327U,526U,2579U},
  {2U,377U,712U,2580U},
  {2U,287U,132U,2581U},
  {2U,326U,609U,2582U},
  {2U,20U,551U,2583U},
  {2U,13U,553U,2584U},
  {2U,104U,16U,2585U},
  {2U,428U,557U,2586U},
  {2U,210U,835U,2587U},
  {2U,217U,128U,2588U},
  {2U,98U,675U,2589U},
  {2U,182U,233U,2590U},
  {2U,400U,102U,2591U},
  {2U,489U,302U,2592U},
  {2U,359U,868U,2593U},
  {2U,500U,831U,2594U},
  {2U,446U,388U,2595U},
  {2U,49U,38U,2596U},
  {2U,453U,324U,2597U},
  {2U,108U,407U,2598U},
  {2U,440U,907U,2599U},
  {2U,185U,156U,2600U},
  {2U,431U,818U,2601U},
  {2U,401U,43U,2602U},
  {2U,144U,121U,2603U},
  {2U,217U,734U,2604U},
  {2U,286U,210U,2605U},
  {2U,412U,401U,2606U},
  {2U,342U,228U,2607U},
  {2U,328U,727U,2608U},
  {2U,214U,963U,2609U},
  {2U,72U,573U,2610U},
  {2U,224U,820U,2611U},
  {2U,97U,623U,2612U},
  {2U,150U,408U,2613U},
  {2U,330U,275U,2614U},
  {2U,160U,996U,2615U},
  {2U,19U,229U,2616U},
  {2U,446U,720U,2617U},
  {2U,296U,330U,2618U},
  {2U,7U,447U,2619U},
  {2U,214U,924U,2620U},
  {2U,107U,342U,2621U},
  {2U,42U,269U,2622U},
  {2U,215U,357U,2623U},
  {2U,230U,896U,2624U},
  {2U,491U,120U,2625U},
  {2U,290U,153U,2626U},
  {2U,407U,489U,2627U},
  {2U,202U,987U,2628U},
  {2U,51U,625U,2629U},
  {2U,233U,419U,2630U},
  {2U,129U,661U,2631U},
  {2U,277U,688U,2632U},
  {2U,71U,292U,2633U},
  {2U,373U,706U,2634U},
  {2U,314U,199U,2635U},
  {2U,14U,974U,2636U},
  {2U,261U,31U,2637U},
  {2U,476U,218U,2638U},
  {2U,93U,743U,2639U},
  {2U,203U,429U,2640U},
  {2U,24U,14U,2641U},
  {2U,150U,450U,2642U},
  {2U,383U,679U,2643U},
  {2U,272U,454U,2644U},
  {2U,403U,288U,2645U},
  {2U,49U,486U,2646U},
  {2U,247U,513U,2647U},
  {2U,19U,862U,2648U},
  {2U,255U,228U,2649U},
  {2U,404U,980U,2650U},
  {2U,9U,971U,2651U},
  {2U,436U,405U,2652U},
  {2U,319U,902U,2653U},
  {2U,344U,410U,2654U},
  {2U,180U,369U,2655U},
  {2U,47U,164U,2656U},
  {2U,338U,138U,2657U},
  {2U,98U,364U,2658U},
  {2U,456U,450U,2659U},
  {2U,355U,309U,2660U},
  {2U,223U,169U,2661U},
  {2U,305U,775U,2662U},
  {2U,263U,71U,2663U},
  {2U,195U,663U,2664U},
  {2U,391U,262U,2665U},
  {2U,476U,18U,2666U},
  {2U,352U,427U,2667U},
  {2U,65U,609U,2668U},
  {2U,134U,318U,2669U},
  {2U,507U,562U,2670U},
  {2U,345U,89U,2671U},
  {2U,419U,197U,2672U},
  {2U,347U,992U,2673U},
  {2U,78U,209U,2674U},
  {2U,476U,854U,2675U},
  {2U,154U,909U,2676U},
  {2U,387U,510U,2677U},
  {2U,8U,634U,2678U},
  {2U,316U,429U,2679U},
  {2U,495U,992U,2680U},
  {2U,210U,753U,2681U},
  {2U,340U,730U,2682U},
  {2U,176U,413U,2683U},
  {2U,445U,303U,2684U},
  {2U,140U,817U,2685U},
  {2U,223U,801U,2686U},
  {2U,262U,599U,2687U},
  {2U,505U,787U,2688U},
  {2U,61U,937U,2689U},
  {2U,173U,569U,2690U},
  {2U,438U,993U,2691U},
  {2U,223U,260U,2692U},
  {2U,399U,123U,2693U},
  {2U,33U,806U,2694U},
  {2U,329U,682U,2695U},
  {2U,266U,951U,2696U},
  {2U,216U,130U,2697U},
  {2U,56U,189U,2698U},
  {2U,118U,557U,2699U},
  {2U,252U,995U,2700U},
  {2U,447U,833U,2701U},
  {2U,439U,855U,2702U},
  {2U,94U,51U,2703U},
  {2U,393U,985U,2704U},
  {2U,130U,188U,2705U},
  {2U,440U,843U,2706U},
  {2U,194U,70U,2707U},
  {2U,315U,824U,2708U},
  {2U,310U,660U,2709U},
  {2U,263U,430U,2710U},
  {2U,372U,878U,2711U},
  {2U,502U,552U,2712U},
  {2U,478U,324U,2713U},
  {2U,469U,961U,2714U},
  {2U,198U,655U,2715U},
  {2U,336U,575U,2716U},
  {2U,94U,695U,2717U},
  {2U,208U,467U,2718U},
  {2U,391U,359U,2719U},
  {2U,192U,991U,2720U},
  {2U,170U,437U,2721U},
  {2U,241U,634U,2722U},
  {2U,81U,706U,2723U},
  {2U,140U,281U,2724U},
  {2U,401U,882U,2725U},
  {2U,323U,900U,2726U},
  {2U,505U,642U,2727U},
  {2U,184U,860U,2728U},
  {2U,112U,130U,2729U},
  {2U,322U,390U,2730U},
  {2U,49U,101U,2731U},
  {2U,86U,95U,2732U},
  {2U,410U,607U,2733U},
  {2U,228U,705U,2734U},
  {2U,334U,608U,2735U},
  {2U,378U,103U,2736U},
  {2U,320U,870U,2737U},
  {2U,277U,197U,2738U},
  {2U,20U,75U,2739U},
  {2U,26U,465U,2740U},
  {2U,500U,116U,2741U},
  {2U,25U,478U,2742U},
  {2U,180U,390U,2743U},
  {2U,42U,217U,2744U},
  {2U,422U,128U,2745U},
  {2U,238U,105U,2746U},
  {2U,431U,24U,2747U},
  {2U,10U,196U,2748U},
  {2U,221U,30U,2749U},
  {2U,307U,35U,2750U},
  {2U,21U,889U,2751U},
  {2U,170U,692U,2752U},
  {2U,0U,459U,2753U},
  {2U,378U,152U,2754U},
  {2U,401U,658U,2755U},
  {2U,157U,999U,2756U},
  {2U,65U,189U,2757U},
  {2U,233U,450U,2758U},
  {2U,174U,345U,2759U},
  {2U,133U,444U,2760U},
  {2U,287U,17U,2761U},
  {2U,352U,894U,2762U},
  {2U,373U,923U,2763U},
  {2U,320U,273U,2764U},
  {2U,391U,125U,2765U},
  {2U,73U,215U,2766U},
  {2U,88U,371U,2767U},
  {2U,184U,628U,2768U},
  {2U,496U,856U,2769U},
  {2U,412U,502U,2770U},
  {2U,386U,956U,2771U},
  {2U,146U,300U,2772U},
  {2U,201U,249U,2773U},
  {2U,383U,912U,2774U},
  {2U,123U,167U,2775U},
  {2U,294U,888U,2776U},
  {2U,88U,565U,2777U},
  {2U,77U,722U,2778U},
  {2U,511U,35U,2779U},
  {2U,506U,524U,2780U},
  {2U,54U,778U,2781U},
  {2U,141U,77U,2782U},
  {2U,10U,156U,2783U},
  {2U,2U,658U,2784U},
  {2U,482U,782U,2785U},
  {2U,204U,961U,2786U},
  {2U,167U,91U,2787U},
  {2U,283U,93U,2788U},
  {2U,302U,191U,2789U},
  {2U,410U,776U,2790U},
  {2U,279U,836U,2791U},
  {2U,140U,978U,2792U},
  {2U,285U,253U,2793U},
  {2U,45U,246U,2794U},
  {2U,325U,159U,2795U},
  {2U,85U,446U,2796U},
  {2U,190U,971U,2797U},
  {2U,503U,539U,2798U},
  {2U,117U,139U,2799U},
  {2U,117U,244U,2800U},
  {2U,4U,480U,2801U},
  {2U,304U,626U,2802U},
  {2U,429U,450U,2803U},
  {2U,195U,824U,2804U},
  {2U,324U,174U,2805U},
  {2U,425U,258U,2806U},
  {2U,205U,573U,2807U},
  {2U,161U,143U,2808U},
  {2U,477U,993U,2809U},
  {2U,44U,987U,2810U},
  {2U,445U,263U,2811U},
  {2U,53U,669U,2812U},
  {2U,3U,523U,2813U},
  {2U,379U,563U,2814U},
  {2U,379U,415U,2815U},
  {2U,276U,396U,2816U},
  {2U,56U,583U,2817U},
  {2U,444U,674U,2818U},
  {2U,183U,180U,2819U},
  {2U,379U,555U,2820U},
  {2U,195U,497U,2821U},
  {2U,143U,714U,2822U},
  {2U,162U,194U,2823U},
  {2U,189U,679U,2824U},
  {2U,276U,909U,2825U},
  {2U,123U,986U,2826U},
  {2U,122U,507U,2827U},
  {2U,243U,182U,2828U},
  {2U,366U,783U,2829U},
  {2U,444U,978U,2830U},
  {2U,72U,995U,2831U},
  {2U,114U,148U,2832U},
  {2U,452U,724U,2833U},
  {2U,42U,847U,2834U},
  {2U,94U,182U,2835U},
  {2U,186U,709U,2836U},
  {2U,476U,167U,2837U},
  {2U,511U,944U,2838U},
  {2U,424U,254U,2839U},
  {2U,84U,208U,2840U},
  {2U,106U,979U,2841U},
  {2U,2U,379U,2842U},
  {2U,383U,797U,2843U},
  {2U,468U,339U,2844U},
  {2U,436U,599U,2845U},
  {2U,78U,652U,2846U},
  {2U,80U,98U,2847U},
  {2U,144U,956U,2848U},
  {2U,351U,90U,2849U},
  {2U,76U,560U,2850U},
  {2U,192U,886U,2851U},
  {2U,22U,805U,2852U},
  {2U,160U,22U,2853U},
  {2U,4U,662U,2854U},
  {2U,198U,356U,2855U},
  {2U,98U,888U,2856U},
  {2U,48U,562U,2857U},
  {2U,228U,475U,2858U},
  {2U,174U,897U,2859U},
  {2U,73U,484U,2860U},
  {2U,91U,40U,2861U},
  {2U,365U,617U,2862U},
  {2U,105U,523U,2863U},
  {2U,256U,676U,2864U},
  {2U,227U,225U,2865U},
  {2U,259U,573U,2866U},
  {2U,267U,991U,2867U},
  {2U,504U,213U,2868U},
  {2U,271U,553U,2869U},
  {2U,66U,726U,2870U},
  {2U,124U,887U,2871U},
  {2U,305U,439U,2872U},
  {2U,28U,560U,2873U},
  {2U,4U,347U,2874U},
  {2U,391U,190U,2875U},
  {2U,475U,118U,2876U},
  {2U,394U,612U,2877U},
  {2U,78U,391U,2878U},
  {2U,466U,681U,2879U},
  {2U,209U,104U,2880U},
  {2U,437U,10U,2881U},
  {2U,57U,692U,2882U},
  {2U,51U,629U,2883U},
  {2U,414U,484U,2884U},
  {2U,168U,788U,2885U},
  {2U,490U,996U,2886U},
  {2U,190U,877U,2887U},
  {2U,138U,117U,2888U},
  {2U,299U,478U,2889U},
  {2U,117U,758U,2890U},
  {2U,365U,973U,2891U},
  {2U,380U,77U,2892U},
  {2U,325U,262U,2893U},
  {2U,398U,156U,2894U},
  {2U,218U,452U,2895U},
  {2U,443U,210U,2896U},
  {2U,509U,555U,2897U},
  {2U,212U,517U,2898U},
  {2U,45U,412U,2899U},
  {2U,463U,453U,2900U},
  {2U,441U,797U,2901U},
  {2U,106U,848U,2902U},
  {2U,35U,630U,2903U},
  {2U,36U,74U,2904U},
  {2U,372U,416U,2905U},
  {2U,99U,172U,2906U},
  {2U,5U,496U,2907U},
  {2U,484U,130U,2908U},
  {2U,59U,485U,2909U},
  {2U,505U,598U,2910U},
  {2U,326U,305U,2911U},
  {2U,384U,303U,2912U},
  {2U,397U,890U,2913U},
  {2U,372U,967U,2914U},
  {2U,181U,910U,2915U},
  {2U,214U,357U,2916U},
  {2U,8U,102U,2917U},
  {2U,44U,409U,2918U},
  {2U,64U,535U,2919U},
  {2U,233U,866U,2920U},
  {2U,292U,945U,2921U},
  {2U,116U,828U,2922U},
  {2U,365U,245U,2923U},
  {2U,158U,366U,2924U},
  {2U,209U,303U,2925U},
  {2U,395U,12U,2926U},
  {2U,7U,926U,2927U},
  {2U,53U,918U,2928U},
  {2U,45U,58U,2929U},
  {2U,368U,923U,2930U},
  {2U,148U,249U,2931U},
  {2U,413U,425U,2932U},
  {2U,297U,654U,2933U},
  {2U,202U,435U,2934U},
  {2U,440U,576U,2935U},
  {2U,137U,554U,2936U},
  {2U,113U,175U,2937U},
  {2U,337U,714U,2938U},
  {2U,6U,905U,2939U},
  {2U,192U,270U,2940U},
  {2U,410U,388U,2941U},
  {2U,84U,899U,2942U},
  {2U,132U,681U,2943U},
  {2U,77U,560U,2944U},
  {2U,284U,680U,2945U},
  {2U,140U,523U,2946U},
  {2U,374U,805U,2947U},
  {2U,10U,853U,2948U},
  {2U,425U,517U,2949U},
  {2U,331U,236U,2950U},
  {2U,476U,510U,2951U},
  {2U,185U,473U,2952U},
  {2U,113U,136U,2953U},
  {2U,331U,395U,2954U},
  {2U,295U,452U,2955U},
  {2U,229U,591U,2956U},
  {2U,69U,217U,2957U},
  {2U,248U,194U,2958U},
  {2U,180U,135U,2959U},
  {2U,107U,140U,2960U},
  {2U,72U,835U,2961U},
  {2U,169U,308U,2962U},
  {2U,495U,919U,2963U},
  {2U,65U,280U,2964U},
  {2U,231U,108U,2965U},
  {2U,44U,503U,2966U},
  {2U,282U,917U,2967U},
  {2U,52U,919U,2968U},
  {2U,373U,752U,2969U},
  {2U,15U,461U,2970U},
  {2U,213U,319U,2971U},
  {2U,313U,678U,2972U},
  {2U,128U,779U,2973U},
  {2U,431U,497U,2974U},
  {2U,152U,342U,2975U},
  {2U,153U,760U,2976U},
  {2U,253U,593U,2977U},
  {2U,44U,350U,2978U},
  {2U,497U,526U,2979U},
  {2U,69U,334U,2980U},
  {2U,241U,781U,2981U},
  {2U,489U,380U,2982U},
  {2U,180U,552U,2983U},
  {2U,342U,730U,2984U},
  {2U,373U,235U,2985U},
  {2U,215U,654U,2986U},
  {2U,98U,364U,2987U},
  {2U,120U,441U,2988U},
  {2U,113U,950U,2989U},
  {2U,351U,466U,2990U},
  {2U,271U,283U,2991U},
  {2U,182U,957U,2992U},
  {2U,447U,908U,2993U},
  {2U,477U,645U,2994U},
  {2U,284U,70U,2995U},
  {2U,215U,8U,2996U},
  {2U,302U,91U,2997U},
  {2U,189U,177U,2998U},
  {2U,327U,249U,2999U},
  {2U,184U,673U,3000U},
  {2U,273U,713U,3001U},
  {2U,439U,723U,3002U},
  {2U,203U,420U,3003U},
  {2U,442U,586U,3004U},
  {2U,78U,354U,3005U},
  {2U,413U,239U,3006U},
  {2U,68U,109U,3007U},
  {2U,182U,632U,3008U},
  {2U,298U,557U,3009U},
  {2U,101U,669U,3010U},
  {2U,92U,884U,3011U},
  {2U,379U,999U,3012U},
  {2U,317U,459U,3013U},
  {2U,494U,779U,3014U},
  {2U,297U,524U,3015U},
  {2U,297U,60U,3016U},
  {2U,378U,305U,3017U},
  {2U,499U,266U,3018U},
  {2U,397U,766U,3019U},
  {2U,257U,906U,3020U},
  {2U,211U,119U,3021U},
  {2U,494U,239U,3022U},
  {2U,314U,157U,3023U},
  {2U,356U,174U,3024U},
  {2U,12U,77U,3025U},
  {2U,85U,493U,3026U},
  {2U,225U,129U,3027U},
  {2U,264U,254U,3028U},
  {2U,123U,10U,3029U},
  {2U,251U,308U,3030U},
  {2U,10U,211U,3031U},
  {2U,41U,881U,3032U},
  {2U,179U,730U,3033U},
  {2U,254U,580U,3034U},
  {2U,447U,890U,3035U},
  {2U,284U,563U,3036U},
  {2U,82U,384U,3037U},
  {2U,241U,37U,3038U},
  {2U,222U,520U,3039U},
  {2U,499U,694U,3040U},
  {2U,228U,517U,3041U},
  {2U,66U,432U,3042U},
  {2U,346U,235U,3043U},
  {2U,37U,93U,3044U},
  {2U,14U,722U,3045U},
  {2U,379U,677U,3046U},
  {2U,278U,465U,3047U},
  {2U,503U,359U,3048U},
  {2U,366U,757U,3049U},
  {2U,275U,861U,3050U},
  {2U,293U,57U,3051U},
  {2U,276U,407U,3052U},
  {2U,470U,74U,3053U},
  {2U,205U,429U,3054U},
  {2U,281U,137U,3055U},
  {2U,391U,771U,3056U},
  {2U,320U,933U,3057U},
  {2U,270U,664U,3058U},
  {2U,55U,861U,3059U},
  {2U,115U,450U,3060U},
  {2U,313U,522U,3061U},
  {2U,149U,373U,3062U},
  {2U,131U,901U,3063U},
  {2U,301U,436U,3064U},
  {2U,377U,160U,3065U},
  {2U,500U,69U,3066U},
  {2U,406U,399U,3067U},
  {2U,224U,519U,3068U},
  {2U,152U,598U,3069U},
  {2U,468U,612U,3070U},
  {2U,347U,255U,3071U},
  {2U,166U,980U,3072U},
  {2U,308U,230U,3073U},
  {2U,176U,617U,3074U},
  {2U,398U,740U,3075U},
  {2U,441U,707U,3076U},
  {2U,129U,728U,3077U},
  {2U,243U,762U,3078U},
  {2U,145U,440U,3079U},
  {2U,373U,198U,3080U},
  {2U,396U,666U,3081U},
  {2U,41U,25U,3082U},
  {2U,394U,168U,3083U},
  {2U,505U,33U,3084U},
  {2U,408U,787U,3085U},
  {2U,462U,949U,3086U},
  {2U,61U,468U,3087U},
  {2U,361U,714U,3088U},
  {2U,39U,443U,3089U},
  {2U,449U,357U,3090U},
  {2U,302U,970U,3091U},
  {2U,417U,433U,3092U},
  {2U,431U,128U,3093U},
  {2U,463U,938U,3094U},
  {2U,319U,542U,3095U},
  {2U,417U,144U,3096U},
  {2U,245U,317U,3097U},
  {2U,28U,234U,3098U},
  {2U,435U,856U,3099U},
  {2U,74U,732U,3100U},
  {2U,371U,548U,3101U},
  {2U,463U,799U,3102U},
  {2U,98U,78U,3103U},
  {2U,413U,64U,3104U},
  {2U,434U,444U,3105U},
  {2U,488U,320U,3106U},
  {2U,166U,875U,3107U},
  {2U,106U,454U,3108U},
  {2U,369U,11U,3109U},
  {2U,411U,390U,3110U},
  {2U,454U,629U,3111U},
  {2U,284U,651U,3112U},
  {2U,139U,561U,3113U},
  {2U,52U,693U,3114U},
  {2U,490U,479U,3115U},
  {2U,110U,861U,3116U},
  {2U,474U,584U,3117U},
  {2U,52U,766U,3118U},
  {2U,468U,264U,3119U},
  {2U,464U,490U,3120U},
  {2U,341U,823U,3121U},
  {2U,71U,319U,3122U},
  {2U,111U,194U,3123U},
  {2U,268U,342U,3124U},
  {2U,352U,436U,3125U},
  {2U,50U,29U,3126U},
  {2U,238U,831U,3127U},
  {2U,408U,934U,3128U},
  {2U,116U,715U,3129U},
  {2U,273U,187U,3130U},
  {2U,60U,971U,3131U},
  {2U,216U,700U,3132U},
  {2U,60U,399U,3133U},
  {2U,142U,602U,3134U},
  {2U,213U,581U,3135U},
  {2U,314U,464U,3136U},
  {2U,275U,410U,3137U},
  {2U,37U,443U,3138U},
  {2U,143U,230U,3139U},
  {2U,151U,761U,3140U},
  {2U,364U,772U,3141U},
  {2U,209U,929U,3142U},
  {2U,167U,108U,3143U},
  {2U,244U,939U,3144U},
  {2U,19U,592U,3145U},
  {2U,130U,830U,3146U},
  {2U,123U,927U,3147U},
  {2U,251U,827U,3148U},
  {2U,12U,471U,3149U},
  {2U,212U,565U,3150U},
  {2U,204U,866U,3151U},
  {2U,346U,265U,3152U},
  {2U,273U,218U,3153U},
  {2U,187U,365U,3154U},
  {2U,231U,15U,3155U},
  {2U,327U,655U,3156U},
  {2U,397U,657U,3157U},
  {2U,194U,776U,3158U},
  {2U,206U,554U,3159U},
  {2U,482U,922U,3160U},
  {2U,86U,197U,3161U},
  {2U,86U,851U,3162U},
  {2U,252U,705U,3163U},
  {2U,285U,614U,3164U},
  {2U,60U,501U,3165U},
  {2U,479U,29U,3166U},
  {2U,139U,824U,3167U},
  {2U,234U,73U,3168U},
  {2U,185U,438U,3169U},
  {2U,104U,728U,3170U},
  {2U,239U,55U,3171U},
  {2U,221U,741U,3172U},
  {2U,13U,587U,3173U},
  {2U,3U,219U,3174U},
  {2U,232U,701U,3175U},
  {2U,207U,975U,3176U},
  {2U,91U,173U,3177U},
  {2U,238U,140U,3178U},
  {2U,231U,826U,3179U},
  {2U,426U,922U,3180U},
  {2U,361U,77U,3181U},
  {2U,333U,347U,3182U},
  {2U,222U,987U,3183U},
  {2U,266U,718U,3184U},
  {2U,56U,125U,3185U},
  {2U,493U,924U,3186U},
  {2U,142U,233U,3187U},
  {2U,302U,488U,3188U},
  {2U,502U,700U,3189U},
  {2U,182U,253U,3190U},
  {2U,37U,635U,3191U},
  {2U,94U,214U,3192U},
  {2U,371U,684U,3193U},
  {2U,38U,124U,3194U},
  {2U,131U,973U,3195U},
  {2U,223U,258U,3196U},
  {2U,397U,438U,3197U},
  {2U,104U,249U,3198U},
  {2U,363U,462U,3199U},
  {2U,16U,988U,3200U},
  {2U,487U,364U,3201U},
  {2U,403U,204U,3202U},
  {2U,225U,91U,3203U},
  {2U,307U,320U,3204U},
  {2U,236U,970U,3205U},
  {2U,271U,129U,3206U},
  {2U,46U,875U,3207U},
  {2U,380U,962U,3208U},
  {2U,263U,154U,3209U},
  {2U,450U,152U,3210U},
  {2U,215U,52U,3211U},
  {2U,232U,623U,3212U},
  {2U,497U,457U,3213U},
  {2U,254U,202U,3214U},
  {2U,173U,400U,3215U},
  {2U,485U,53U,3216U},
  {2U,276U,80U,3217U},
  {2U,244U,373U,3218U},
  {2U,392U,60U,3219U},
  {2U,88U,930U,3220U},
  {2U,285U,649U,3221U},
  {2U,330U,645U,3222U},
  {2U,43U,162U,3223U},
  {2U,300U,225U,3224U},
  {2U,330U,618U,3225U},
  {2U,465U,758U,3226U},
  {2U,300U,295U,3227U},
  {2U,506U,544U,3228U},
  {2U,161U,959U,3229U},
  {2U,259U,532U,3230U},
  {2U,40U,440U,3231U},
  {2U,213U,372U,3232U},
  {2U,224U,685U,3233U},
  {2U,505U,991U,3234U},
  {2U,496U,887U,3235U},
  {2U,449U,546U,3236U},
  {2U,14U,349U,3237U},
  {2U,394U,555U,3238U},
  {2U,437U,790U,3239U},
  {2U,86U,559U,3240U},
  {2U,355U,211U,3241U},
  {2U,448U,645U,3242U},
  {2U,458U,975U,3243U},
  {2U,426U,676U,3244U},
  {2U,146U,304U,3245U},
  {2U,0U,327U,3246U},
  {2U,251U,274U,3247U},
  {2U,196U,552U,3248U},
  {2U,239U,120U,3249U},
  {2U,349U,159U,3250U},
  {2U,19U,962U,3251U},
  {2U,17U,217U,3252U},
  {2U,200U,261U,3253U},
  {2U,234U,709U,3254U},
  {2U,356U,857U,3255U},
  {2U,403U,335U,3256U},
  {2U,405U,239U,3257U},
  {2U,371U,877U,3258U},
  {2U,329U,673U,3259U},
  {2U,413U,204U,3260U},
  {2U,291U,792U,3261U},
  {2U,489U,658U,3262U},
  {2U,134U,934U,3263U},
  {2U,388U,476U,3264U},
  {2U,263U,626U,3265U},
  {2U,194U,848U,3266U},
  {2U,199U,307U,3267U},
  {2U,22U,507U,3268U},
  {2U,210U,406U,3269U},
  {2U,508U,895U,3270U},
  {2U,3U,980U,3271U},
  {2U,107U,524U,3272U},
  {2U,261U,247U,3273U},
  {2U,503U,755U,3274U},
  {2U,307U,120U,3275U},
  {2U,440U,602U,3276U},
  {2U,323U,31U,3277U},
  {2U,465U,249U,3278U},
  {2U,52U,608U,3279U},
  {2U,88U,967U,3280U},
  {2U,403U,685U,3281U},
  {2U,340U,433U,3282U},
  {2U,92U,560U,3283U},
  {2U,350U,38U,3284U},
  {2U,493U,867U,3285U},
  {2U,392U,17U,3286U},
  {2U,411U,961U,3287U},
  {2U,406U,775U,3288U},
  {2U,196U,234U,3289U},
  {2U,466U,182U,3290U},
  {2U,204U,117U,3291U},
  {2U,180U,587U,3292U},
  {2U,344U,869U,3293U},
  {2U,54U,360U,3294U},
  {2U,417U,758U,3295U},
  {2U,347U,870U,3296U},
  {2U,37U,932U,3297U},
  {2U,47U,57U,3298U},
  {2U,135U,199U,3299U},
  {2U,117U,762U,3300U},
  {2U,18U,938U,3301U},
  {2U,473U,572U,3302U},
  {2U,437U,170U,3303U},
  {2U,2U,685U,3304U},
  {2U,314U,275U,3305U},
  {2U,471U,194U,3306U},
  {2U,11U,511U,3307U},
  {2U,304U,10U,3308U},
  {2U,444U,445U,3309U},
  {2U,55U,831U,3310U},
  {2U,478U,430U,3311U},
  {2U,53U,511U,3312U},
  {2U,507U,338U,3313U},
  {2U,218U,891U,3314U},
  {2U,495U,726U,3315U},
  {2U,358U,903U,3316U},
  {2U,498U,122U,3317U},
  {2U,312U,488U,3318U},
  {2U,262U,889U,3319U},
  {2U,283U,120U,3320U},
  {2U,204U,507U,3321U},
  {2U,371U,172U,3322U},
  {2U,269U,449U,3323U},
  {2U,53U,312U,3324U},
  {2U,259U,269U,3325U},
  {2U,2U,242U,3326U},
  {2U,502U,45U,3327U},
  {2U,424U,365U,3328U},
  {2U,203U,823U,3329U},
  {2U,222U,756U,3330U},
  {2U,254U,956U,3331U},
  {2U,312U,472U,3332U},
  {2U,435U,660U,3333U},
  {2U,381U,478U,3334U},
  {2U,12U,117U,3335U},
  {2U,447U,196U,3336U},
  {2U,251U,640U,3337U},
  {2U,258U,717U,3338U},
  {2U,445U,740U,3339U},
  {2U,372U,884U,3340U},
  {2U,339U,320U,3341U},
  {2U,76U,947U,3342U},
  {2U,471U,42U,3343U},
  {2U,328U,17U,3344U},
  {2U,245U,349U,3345U},
  {2U,118U,210U,3346U},
  {2U,368U,202U,3347U},
  {2U,190U,139U,3348U},
  {2U,221U,724U,3349U},
  {2U,187U,933U,3350U},
  {2U,29U,523U,3351U},
  {2U,469U,34U,3352U},
  {2U,269U,211U,3353U},
  {2U,0U,68U,3354U},
  {2U,485U,90U,3355U},
  {2U,10U,371U,3356U},
  {2U,401U,952U,3357U},
  {2U,493U,860U,3358U},
  {2U,483U,33U,3359U},
  {2U,109U,725U,3360U},
  {2U,216U,482U,3361U},
  {2U,457U,296U,3362U},
  {2U,39U,296U,3363U},
  {2U,234U,740U,3364U},
  {2U,40U,90U,3365U},
  {2U,20U,248U,3366U},
  {2U,22U,152U,3367U},
  {2U,297U,779U,3368U},
  {2U,158U,120U,3369U},
  {2U,502U,680U,3370U},
  {2U,187U,395U,3371U},
  {2U,501U,737U,3372U},
  {2U,8U,403U,3373U},
  {2U,52U,533U,3374U},
  {2U,366U,441U,3375U},
  {2U,397U,178U,3376U},
  {2U,70U,378U,3377U},
  {2U,59U,302U,3378U},
  {2U,167U,703U,3379U},
  {2U,331U,255U,3380U},
  {2U,211U,618U,3381U},
  {2U,451U,589U,3382U},
  {2U,133U,180U,3383U},
  {2U,187U,91U,3384U},
  {2U,36U,565U,3385U},
  {2U,416U,409U,3386U},
  {2U,507U,191U,3387U},
  {2U,259U,913U,3388U},
  {2U,407U,207U,3389U},
  {2U,141U,318U,3390U},
  {2U,194U,865U,3391U},
  {2U,364U,481U,3392U},
  {2U,49U,118U,3393U},
  {2U,291U,222U,3394U},
  {2U,442U,250U,3395U},
  {2U,206U,845U,3396U},
  {2U,243U,482U,3397U},
  {2U,196U,636U,3398U},
  {2U,459U,502U,3399U},
  {2U,208U,831U,3400U},
  {2U,403U,860U,3401U},
  {2U,84U,445U,3402U},
  {2U,244U,208U,3403U},
  {2U,154U,52U,3404U},
  {2U,135U,662U,3405U},
  {2U,226U,585U,3406U},
  {2U,93U,548U,3407U},
  {2U,127U,751U,3408U},
  {2U,341U,624U,3409U},
  {2U,132U,417U,3410U},
  {2U,11U,805U,3411U},
  {2U,405U,182U,3412U},
  {2U,145U,747U,3413U},
  {2U,350U,199U,3414U},
  {2U,67U,865U,3415U},
  {2U,86U,470U,3416U},
  {2U,76U,644U,3417U},
  {2U,25U,849U,3418U},
  {2U,11U,244U,3419U},
  {2U,13U,195U,3420U},
  {2U,392U,694U,3421U},
  {2U,113U,19U,3422U},
  {2U,318U,201U,3423U},
  {2U,424U,148U,3424U},
  {2U,30U,251U,3425U},
  {2U,376U,417U,3426U},
  {2U,327U,59U,3427U},
  {2U,33U,317U,3428U},
  {2U,217U,41U,3429U},
  {2U,190U,134U,3430U},
  {2U,190U,254U,3431U},
  {2U,484U,487U,3432U},
  {2U,255U,996U,3433U},
  {2U,397U,616U,3434U},
  {2U,389U,287U,3435U},
  {2U,94U,152U,3436U},
  {2U,359U,409U,3437U},
  {2U,419U,929U,3438U},
  {2U,289U,317U,3439U},
  {2U,103U,843U,3440U},
  {2U,54U,586U,3441U},
  {2U,249U,948U,3442U},
  {2U,362U,485U,3443U},
  {2U,290U,830U,3444U},
  {2U,156U,975U,3445U},
  {2U,211U,932U,3446U},
  {2U,164U,843U,3447U},
  {2U,211U,484U,3448U},
  {2U,331U,66U,3449U},
  {2U,478U,379U,3450U},
  {2U,205U,381U,3451U},
  {2U,398U,952U,3452U},
  {2U,33U,878U,3453U},
  {2U,60U,220U,3454U},
  {2U,198U,728U,3455U},
  {2U,153U,720U,3456U},
  {2U,355U,459U,3457U},
  {2U,440U,562U,3458U},
  {2U,371U,309U,3459U},
  {2U,393U,568U,3460U},
  {2U,234U,623U,3461U},
  {2U,368U,329U,3462U},
  {2U,374U,890U,3463U},
  {2U,431U,394U,3464U},
  {2U,306U,947U,3465U},
  {2U,270U,727U,3466U},
  {2U,136U,164U,3467U},
  {2U,433U,990U,3468U},
  {2U,20U,990U,3469U},
  {2U,172U,903U,3470U},
  {2U,430U,622U,3471U},
  {2U,222U,985U,3472U},
  {2U,223U,122U,3473U},
  {2U,507U,246U,3474U},
  {2U,401U,729U,3475U},
  {2U,205U,237U,3476U},
  {2U,33U,194U,3477U},
  {2U,505U,900U,3478U},
  {2U,479U,279U,3479U},
  {2U,25U,451U,3480U},
  {2U,70U,522U,3481U},
  {2U,318U,884U,3482U},
  {2U,470U,178U,3483U},
  {2U,169U,258U,3484U},
  {2U,452U,390U,3485U},
  {2U,341U,504U,3486U},
  {2U,199U,208U,3487U},
  {2U,417U,876U,3488U},
  {2U,126U,510U,3489U},
  {2U,67U,988U,3490U},
  {2U,392U,921U,3491U},
  {2U,362U,168U,3492U},
  {2U,406U,271U,3493U},
  {2U,7U,561U,3494U},
  {2U,145U,992U,3495U},
  {2U,251U,106U,3496U},
  {2U,354U,768U,3497U},
  {2U,300U,966U,3498U},
  {2U,165U,47U,3499U},
  {2U,94U,777U,3500U},
  {2U,260U,903U,3501U},
  {2U,356U,289U,3502U},
  {2U,488U,859U,3503U},
  {2U,153U,897U,3504U},
  {2U,418U,234U,3505U},
  {2U,206U,892U,3506U},
  {2U,137U,833U,3507U},
  {2U,357U,718U,3508U},
  {2U,316U,654U,3509U},
  {2U,35U,952U,3510U},
  {2U,493U,186U,3511U},
  {2U,493U,151U,3512U},
  {2U,103U,373U,3513U},
  {2U,238U,237U,3514U},
  {2U,175U,733U,3515U},
  {2U,441U,606U,3516U},
  {2U,350U,649U,3517U},
  {2U,50U,601U,3518U},
  {2U,469U,363U,3519U},
  {2U,243U,602U,3520U},
  {2U,12U,203U,3521U},
  {2U,387U,331U,3522U},
  {2U,190U,514U,3523U},
  {2U,33U,15U,3524U},
  {2U,301U,905U,3525U},
  {2U,37U,803U,3526U},
  {2U,358U,463U,3527U},
  {2U,149U,352U,3528U},
  {2U,121U,828U,3529U},
  {2U,145U,420U,3530U},
  {2U,507U,728U,3531U},
  {2U,499U,386U,3532U},
  {2U,237U,646U,3533U},
  {2U,416U,85U,3534U},
  {2U,339U,45U,3535U},
  {2U,499U,415U,3536U},
  {2U,267U,684U,3537U},
  {2U,378U,577U,3538U},
  {2U,260U,517U,3539U},
  {2U,155U,71U,3540U},
  {2U,445U,835U,3541U},
  {2U,67U,790U,3542U},
  {2U,349U,47U,3543U},
  {2U,84U,802U,3544U},
  {2U,53U,553U,3545U},
  {2U,67U,761U,3546U},
  {2U,227U,969U,3547U},
  {2U,278U,462U,3548U},
  {2U,65U,848U,3549U},
  {2U,474U,623U,3550U},
  {2U,490U,717U,3551U},
  {2U,288U,107U,3552U},
  {2U,382U,544U,3553U},
  {2U,480U,236U,3554U},
  {2U,69U,517U,3555U},
  {2U,157U,229U,3556U},
  {2U,112U,477U,3557U},
  {2U,200U,295U,3558U},
  {2U,56U,577U,3559U},
  {2U,32U,528U,3560U},
  {2U,119U,542U,3561U},
  {2U,362U,952U,3562U},
  {2U,261U,208U,3563U},
  {2U,193U,112U,3564U},
  {2U,473U,281U,3565U},
  {2U,105U,134U,3566U},
  {2U,297U,315U,3567U},
  {2U,176U,597U,3568U},
  {2U,367U,775U,3569U},
  {2U,264U,257U,3570U},
  {2U,186U,526U,3571U},
  {2U,153U,878U,3572U},
  {2U,110U,268U,3573U},
  {2U,199U,144U,3574U},
  {2U,480U,904U,3575U},
  {2U,417U,981U,3576U},
  {2U,116U,576U,3577U},
  {2U,427U,555U,3578U},
  {2U,408U,407U,3579U},
  {2U,230U,109U,3580U},
  {2U,269U,430U,3581U},
  {2U,334U,427U,3582U},
  {2U,146U,339U,3583U},
  {2U,366U,430U,3584U},
  {2U,56U,410U,3585U},
  {2U,213U,719U,3586U},
  {2U,163U,181U,3587U},
  {2U,502U,571U,3588U},
  {2U,330U,684U,3589U},
  {2U,320U,769U,3590U},
  {2U,125U,12U,3591U},
  {2U,84U,879U,3592U},
  {2U,139U,439U,3593U},
  {2U,456U,924U,3594U},
  {2U,82U,891U,3595U},
  {2U,440U,578U,3596U},
  {2U,219U,949U,3597U},
  {2U,321U,223U,3598U},
  {2U,372U,616U,3599U},
  {2U,380U,218U,3600U},
  {2U,488U,487U,3601U},
  {2U,410U,450U,3602U},
  {2U,94U,473U,3603U},
  {2U,75U,614U,3604U},
  {2U,266U,930U,3605U},
  {2U,174U,923U,3606U},
  {2U,407U,766U,3607U},
  {2U,205U,832U,3608U},
  {2U,455U,767U,3609U},
  {2U,27U,219U,3610U},
  {2U,125U,54U,3611U},
  {2U,317U,227U,3612U},
  {2U,348U,974U,3613U},
  {2U,54U,751U,3614U},
  {2U,144U,86U,3615U},
  {2U,344U,474U,3616U},
  {2U,415U,969U,3617U},
  {2U,111U,654U,3618U},
  {2U,21U,745U,3619U},
  {2U,123U,607U,3620U},
  {2U,257U,955U,3621U},
  {2U,317U,253U,3622U},
  {2U,12U,544U,3623U},
  {2U,141U,179U,3624U},
  {2U,83U,799U,3625U},
  {2U,293U,299U,3626U},
  {2U,375U,129U,3627U},
  {2U,172U,444U,3628U},
  {2U,285U,586U,3629U},
  {2U,251U,494U,3630U},
  {2U,504U,614U,3631U},
  {2U,427U,702U,3632U},
  {2U,408U,997U,3633U},
  {2U,474U,541U,3634U},
  {2U,208U,903U,3635U},
  {2U,94U,176U,3636U},
  {2U,168U,264U,3637U},
  {2U,366U,353U,3638U},
  {2U,64U,810U,3639U},
  {2U,56U,717U,3640U},
  {2U,260U,693U,3641U},
  {2U,363U,950U,3642U},
  {2U,191U,568U,3643U},
  {2U,95U,163U,3644U},
  {2U,205U,238U,3645U},
  {2U,13U,172U,3646U},
  {2U,176U,613U,3647U},
  {2U,136U,644U,3648U},
  {2U,101U,475U,3649U},
  {2U,214U,667U,3650U},
  {2U,454U,44U,3651U},
  {2U,255U,268U,3652U},
  {2U,210U,831U,3653U},
  {2U,474U,471U,3654U},
  {2U,249U,498U,3655U},
  {2U,177U,581U,3656U},
  {2U,402U,885U,3657U},
  {2U,333U,601U,3658U},
  {2U,123U,661U,3659U},
  {2U,218U,363U,3660U},
  {2U,485U,961U,3661U},
  {2U,7U,317U,3662U},
  {2U,445U,264U,3663U},
  {2U,367U,498U,3664U},
  {2U,59U,116U,3665U},
  {2U,180U,847U,3666U},
  {2U,478U,980U,3667U},
  {2U,454U,326U,3668U},
  {2U,211U,934U,3669U},
  {2U,422U,998U,3670U},
  {2U,150U,363U,3671U},
  {2U,103U,986U,3672U},
  {2U,414U,252U,3673U},
  {2U,321U,224U,3674U},
  {2U,410U,298U,3675U},
  {2U,507U,240U,3676U},
  {2U,233U,639U,3677U},
  {2U,312U,922U,3678U},
  {2U,240U,265U,3679U},
  {2U,342U,845U,3680U},
  {2U,438U,554U,3681U},
  {2U,47U,246U,3682U},
  {2U,207U,228U,3683U},
  {2U,263U,362U,3684U},
  {2U,107U,921U,3685U},
  {2U,336U,32U,3686U},
  {2U,474U,614U,3687U},
  {2U,82U,374U,3688U},
  {2U,113U,153U,3689U},
  {2U,271U,391U,3690U},
  {2U,481U,634U,3691U},
  {2U,322U,701U,3692U},
  {2U,453U,588U,3693U},
  {2U,250U,823U,3694U},
  {2U,243U,876U,3695U},
  {2U,325U,954U,3696U},
  {2U,336U,789U,3697U},
  {2U,309U,595U,3698U},
  {2U,0U,774U,3699U},
  {2U,124U,730U,3700U},
  {2U,278U,252U,3701U},
  {2U,494U,214U,3702U},
  {2U,508U,251U,3703U},
  {2U,419U,157U,3704U},
  {2U,397U,945U,3705U},
  {2U,130U,844U,3706U},
  {2U,252U,540U,3707U},
  {2U,424U,900U,3708U},
  {2U,45U,35U,3709U},
  {2U,260U,194U,3710U},
  {2U,134U,148U,3711U},
  {2U,494U,501U,3712U},
  {2U,38U,602U,3713U},
  {2U,109U,880U,3714U},
  {2U,125U,563U,3715U},
  {2U,416U,174U,3716U},
  {2U,104U,496U,3717U},
  {2U,67U,184U,3718U},
  {2U,75U,617U,3719U},
  {2U,193U,523U,3720U},
  {2U,488U,938U,3721U},
  {2U,174U,937U,3722U},
  {2U,160U,815U,3723U},
  {2U,350U,343U,3724U},
  {2U,300U,29U,3725U},
  {2U,166U,504U,3726U},
  {2U,282U,554U,3727U},
  {2U,120U,595U,3728U},
  {2U,264U,51U,3729U},
  {2U,285U,968U,3730U},
  {2U,91U,897U,3731U},
  {2U,338U,856U,3732U},
  {2U,109U,202U,3733U},
  {2U,289U,818U,3734U},
  {2U,151U,248U,3735U},
  {2U,165U,109U,3736U},
  {2U,285U,908U,3737U},
  {2U,26U,373U,3738U},
  {2U,328U,997U,3739U},
  {2U,428U,807U,3740U},
  {2U,413U,103U,3741U},
  {2U,500U,761U,3742U},
  {2U,511U,920U,3743U},
  {2U,248U,679U,3744U},
  {2U,175U,857U,3745U},
  {2U,175U,505U,3746U},
  {2U,290U,505U,3747U},
  {2U,476U,456U,3748U},
  {2U,183U,180U,3749U},
  {2U,184U,502U,3750U},
  {2U,227U,178U,3751U},
  {2U,17U,936U,3752U},
  {2U,163U,227U,3753U},
  {2U,369U,941U,3754U},
  {2U,196U,19U,3755U},
  {2U,245U,885U,3756U},
  {2U,343U,278U,3757U},
  {2U,280U,656U,3758U},
  {2U,238U,175U,3759U},
  {2U,131U,830U,3760U},
  {2U,346U,30U,3761U},
  {2U,32U,281U,3762U},
  {2U,415U,120U,3763U},
  {2U,117U,178U,3764U},
  {2U,54U,693U,3765U},
  {2U,234U,420U,3766U},
  {2U,367U,300U,3767U},
  {2U,361U,819U,3768U},
  {2U,153U,736U,3769U},
  {2U,467U,923U,3770U},
  {2U,490U,320U,3771U},
  {2U,181U,324U,3772U},
  {2U,229U,702U,3773U},
  {2U,147U,18U,3774U},
  {2U,389U,85U,3775U},
  {2U,248U,713U,3776U},
  {2U,333U,639U,3777U},
  {2U,462U,490U,3778U},
  {2U,18U,155U,3779U},
  {2U,176U,126U,3780U},
  {2U,345U,948U,3781U},
  {2U,256U,813U,3782U},
  {2U,349U,427U,3783U},
  {2U,234U,368U,3784U},
  {2U,299U,560U,3785U},
  {2U,170U,421U,3786U},
  {2U,337U,422U,3787U},
  {2U,429U,785U,3788U},
  {2U,45U,731U,3789U},
  {2U,128U,533U,3790U},
  {2U,215U,255U,3791U},
  {2U,50U,918U,3792U},
  {2U,416U,387U,3793U},
  {2U,328U,99U,3794U},
  {2U,48U,616U,3795U},
  {2U,78U,756U,3796U},
  {2U,38U,325U,3797U},
  {2U,0U,38U,3798U},
  {2U,498U,399U,3799U},
  {2U,50U,656U,3800U},
  {2U,405U,976U,3801U},
  {2U,48U,556U,3802U},
  {2U,125U,916U,3803U},
  {2U,17U,360U,3804U},
  {2U,58U,221U,3805U},
  {2U,14U,690U,3806U},
  {2U,10U,56U,3807U},
  {2U,290U,644U,3808U},
  {2U,246U,747U,3809U},
  {2U,78U,183U,3810U},
  {2U,319U,485U,3811U},
  {2U,369U,928U,3812U},
  {2U,198U,92U,3813U},
  {2U,474U,675U,3814U},
  {2U,375U,903U,3815U},
  {2U,110U,973U,3816U},
  {2U,251U,947U,3817U},
  {2U,416U,826U,3818U},
  {2U,71U,754U,3819U},
  {2U,234U,556U,3820U},
  {2U,493U,109U,3821U},
  {2U,373U,855U,3822U},
  {2U,76U,584U,3823U},
  {2U,316U,639U,3824U},
  {2U,125U,767U,3825U},
  {2U,67U,829U,3826U},
  {2U,43U,281U,3827U},
  {2U,410U,259U,3828U},
  {2U,212U,28U,3829U},
  {2U,35U,841U,3830U},
  {2U,346U,971U,3831U},
  {2U,505U,81U,3832U},
  {2U,360U,286U,3833U},
  {2U,494U,935U,3834U},
  {2U,277U,557U,3835U},
  {2U,494U,913U,3836U},
  {2U,428U,631U,3837U},
  {2U,167U,131U,3838U},
  {2U,268U,13U,3839U},
  {2U,374U,409U,3840U},
  {2U,284U,349U,3841U},
  {2U,116U,872U,3842U},
  {2U,162U,922U,3843U},
  {2U,365U,209U,3844U},
  {2U,128U,796U,3845U},
  {2U,134U,31U,3846U},
  {2U,232U,848U,3847U},
  {2U,48U,118U,3848U},
  {2U,115U,413U,3849U},
  {2U,405U,765U,3850U},
  {2U,256U,95U,3851U},
  {2U,238U,981U,3852U},
  {2U,325U,510U,3853U},
  {2U,216U,881U,3854U},
  {2U,339U,124U,3855U},
  {2U,480U,509U,3856U},
  {2U,490U,859U,3857U},
  {2U,293U,848U,3858U},
  {2U,492U,787U,3859U},
  {2U,415U,222U,3860U},
  {2U,234U,545U,3861U},
  {2U,423U,151U,3862U},
  {2U,294U,895U,3863U},
  {2U,382U,288U,3864U},
  {2U,294U,950U,3865U},
  {2U,482U,120U,3866U},
  {2U,183U,17U,3867U},
  {2U,403U,727U,3868U},
  {2U,5U,216U,3869U},
  {2U,313U,901U,3870U},
  {2U,349U,628U,3871U},
  {2U,195U,610U,3872U},
  {2U,167U,479U,3873U},
  {2U,247U,240U,3874U},
  {2U,429U,337U,3875U},
  {2U,60U,533U,3876U},
  {2U,326U,985U,3877U},
  {2U,389U,102U,3878U},
  {2U,152U,891U,3879U},
  {2U,171U,563U,3880U},
  {2U,318U,938U,3881U},
  {2U,218U,647U,3882U},
  {2U,148U,891U,3883U},
  {2U,426U,924U,3884U},
  {2U,373U,307U,3885U},
  {2U,87U,266U,3886U},
  {2U,297U,161U,3887U},
  {2U,501U,59U,3888U},
  {2U,407U,386U,3889U},
  {2U,309U,703U,3890U},
  {2U,98U,432U,3891U},
  {2U,425U,925U,3892U},
  {2U,463U,193U,3893U},
  {2U,388U,151U,3894U},
  {2U,208U,956U,3895U},
  {2U,21U,769U,3896U},
  {2U,330U,243U,3897U},
  {2U,371U,22U,3898U},
  {2U,385U,824U,3899U},
  {2U,39U,944U,3900U},
  {2U,11U,165U,3901U},
  {2U,375U,327U,3902U},
  {2U,142U,238U,3903U},
  {2U,348U,917U,3904U},
  {2U,272U,533U,3905U},
  {2U,240U,820U,3906U},
  {2U,346U,738U,3907U},
  {2U,381U,557U,3908U},
  {2U,499U,995U,3909U},
  {2U,74U,866U,3910U},
  {2U,195U,29U,3911U},
  {2U,184U,298U,3912U},
  {2U,298U,800U,3913U},
  {2U,104U,602U,3914U},
  {2U,114U,34U,3915U},
  {2U,436U,854U,3916U},
  {2U,267U,702U,3917U},
  {2U,59U,494U,3918U},
  {2U,48U,343U,3919U},
  {2U,123U,855U,3920U},
  {2U,149U,246U,3921U},
  {2U,471U,297U,3922U},
  {2U,372U,923U,3923U},
  {2U,493U,152U,3924U},
  {2U,387U,258U,3925U},
  {2U,312U,824U,3926U},
  {2U,398U,75U,3927U},
  {2U,465U,504U,3928U},
  {2U,102U,93U,3929U},
  {2U,404U,993U,3930U},
  {2U,350U,547U,3931U},
  {2U,74U,729U,3932U},
  {2U,385U,652U,3933U},
  {2U,415U,71U,3934U},
  {2U,317U,683U,3935U},
  {2U,147U,982U,3936U},
  {2U,162U,903U,3937U},
  {2U,471U,307U,3938U},
  {2U,272U,824U,3939U},
  {2U,10U,588U,3940U},
  {2U,471U,739U,3941U},
  {2U,112U,447U,3942U},
  {2U,420U,530U,3943U},
  {2U,290U,570U,3944U},
  {2U,260U,909U,3945U},
  {2U,58U,278U,3946U},
  {2U,179U,437U,3947U},
  {2U,486U,662U,3948U},
  {2U,276U,958U,3949U},
  {2U,291U,251U,3950U},
  {2U,422U,719U,3951U},
  {2U,384U,140U,3952U},
  {2U,344U,909U,3953U},
  {2U,242U,256U,3954U},
  {2U,372U,558U,3955U},
  {2U,38U,197U,3956U},
  {2U,142U,609U,3957U},
  {2U,238U,820U,3958U},
  {2U,265U,419U,3959U},
  {2U,468U,467U,3960U},
  {2U,168U,300U,3961U},
  {2U,193U,174U,3962U},
  {2U,178U,828U,3963U},
  {2U,296U,103U,3964U},
  {2U,486U,98U,3965U},
  {2U,261U,959U,3966U},
  {2U,184U,788U,3967U},
  {2U,259U,32U,3968U},
  {2U,231U,593U,3969U},
  {2U,345U,998U,3970U},
  {2U,11U,115U,3971U},
  {2U,5U,461U,3972U},
  {2U,299U,13U,3973U},
  {2U,344U,933U,3974U},
  {2U,342U,826U,3975U},
  {2U,298U,881U,3976U},
  {2U,247U,687U,3977U},
  {2U,406U,446U,3978U},
  {2U,319U,664U,3979U},
  {2U,500U,820U,3980U},
  {2U,482U,520U,3981U},
  {2U,204U,32U,3982U},
  {2U,484U,171U,3983U},
  {2U,398U,453U,3984U},
  {2U,122U,732U,3985U},
  {2U,349U,201U,3986U},
  {2U,292U,932U,3987U},
  {2U,355U,934U,3988U},
  {2U,312U,153U,3989U},
  {2U,10U,47U,3990U},
  {2U,63U,485U,3991U},
  {2U,420U,751U,3992U},
  {2U,491U,449U,3993U},
  {2U,23U,489U,3994U},
  {2U,448U,449U,3995U},
  {2U,79U,273U,3996U},
  {2U,191U,923U,3997U},
  {2U,46U,803U,3998U},
  {2U,187U,487U,3999U},
  {2U,84U,962U,4000U},
  {2U,112U,178U,4001U},
  {2U,412U,246U,4002U},
  {2U,332U,974U,4003U},
  {2U,248U,755U,4004U},
  {2U,469U,184U,4005U},
  {2U,181U,315U,4006U},
  {2U,306U,153U,4007U},
  {2U,354U,488U,4008U},
  {2U,320U,228U,4009U},
  {2U,420U,927U,4010U},
  {2U,36U,549U,4011U},
  {2U,227U,556U,4012U},
  {2U,510U,967U,4013U},
  {2U,176U,503U,4014U},
  {2U,292U,582U,4015U},
  {2U,489U,335U,4016U},
  {2U,324U,603U,4017U},
  {2U,424U,289U,4018U},
  {2U,437U,662U,4019U},
  {2U,127U,895U,4020U},
  {2U,374U,811U,4021U},
  {2U,202U,349U,4022U},
  {2U,65U,272U,4023U},
  {2U,234U,850U,4024U},
  {2U,196U,683U,4025U},
  {2U,431U,207U,4026U},
  {2U,324U,885U,4027U},
  {2U,445U,698U,4028U},
  {2U,164U,626U,4029U},
  {2U,247U,816U,4030U},
  {2U,279U,279U,4031U},
  {2U,374U,883U,4032U},
  {2U,193U,463U,4033U},
  {2U,389U,785U,4034U},
  {2U,143U,637U,4035U},
  {2U,499U,911U,4036U},
  {2U,96U,881U,4037U},
  {2U,195U,998U,4038U},
  {2U,360U,657U,4039U},
  {2U,348U,584U,4040U},
  {2U,142U,664U,4041U},
  {2U,481U,166U,4042U},
  {2U,319U,570U,4043U},
  {2U,247U,814U,4044U},
  {2U,413U,243U,4045U},
  {2U,428U,273U,4046U},
  {2U,441U,487U,4047U},
  {2U,366U,798U,4048U},
  {2U,293U,717U,4049U},
  {2U,100U,81U,4050U},
  {2U,216U,522U,4051U},
  {2U,256U,545U,4052U},
  {2U,138U,922U,4053U},
  {2U,203U,465U,4054U},
  {2U,258U,382U,4055U},
  {2U,493U,985U,4056U},
  {2U,460U,326U,4057U},
  {2U,119U,913U,4058U},
  {2U,306U,321U,4059U},
  {2U,418U,289U,4060U},
  {2U,66U,623U,4061U},
  {2U,93U,981U,4062U},
  {2U,419U,165U,4063U},
  {2U,501U,768U,4064U},
  {2U,314U,224U,4065U},
  {2U,218U,293U,4066U},
  {2U,502U,137U,4067U},
  {2U,93U,526U,4068U},
  {2U,487U,416U,4069U},
  {2U,24U,17U,4070U},
  {2U,212U,68U,4071U},
  {2U,207U,326U,4072U},
  {2U,146U,728U,4073U},
  {2U,469U,219U,4074U},
  {2U,10U,347U,4075U},
  {2U,6U,946U,4076U},
  {2U,452U,950U,4077U},
  {2U,444U,637U,4078U},
  {2U,334U,673U,4079U},
  {2U,324U,138U,4080U},
  {2U,319U,935U,4081U},
  {2U,216U,444U,4082U},
  {2U,58U,286U,4083U},
  {2U,280U,123U,4084U},
  {2U,459U,228U,4085U},
  {2U,236U,126U,4086U},
  {2U,280U,811U,4087U},
  {2U,115U,836U,4088U},
  {2U,69U,649U,4089U},
  {2U,141U,709U,4090U},
  {2U,112U,233U,4091U},
  {2U,251U,680U,4092U},
  {2U,289U,832U,4093U},
  {2U,138U,976U,4094U},
  {2U,21U,555U,4095U},
  {2U,98U,622U,4096U},
  {2U,466U,336U,4097U},
  {2U,164U,613U,4098U},
  {2U,104U,851U,4099U},
  {2U,371U,957U,4100U},
  {2U,87U,30U,4101U},
  {2U,273U,117U,4102U},
  {2U,413U,26U,4103U},
  {2U,500U,738U,4104U},
  {2U,31U,762U,4105U},
  {2U,29U,513U,4106U},
  {2U,242U,805U,4107U},
  {2U,237U,364U,4108U},
  {2U,93U,533U,4109U},
  {2U,61U,129U,4110U},
  {2U,223U,308U,4111U},
  {2U,403U,86U,4112U},
  {2U,461U,977U,4113U},
  {2U,56U,925U,4114U},
  {2U,216U,773U,4115U},
  {2U,478U,693U,4116U},
  {2U,39U,575U,4117U},
  {2U,124U,129U,4118U},
  {2U,270U,823U,4119U},
  {2U,207U,271U,4120U},
  {2U,112U,680U,4121U},
  {2U,31U,10U,4122U},
  {2U,492U,28U,4123U},
  {2U,279U,849U,4124U},
  {2U,286U,722U,4125U},
  {2U,483U,169U,4126U},
  {2U,482U,392U,4127U},
  {2U,71U,676U,4128U},
  {2U,310U,275U,4129U},
  {2U,347U,412U,4130U},
  {2U,73U,57U,4131U},
  {2U,174U,678U,4132U},
  {2U,338U,556U,4133U},
  {2U,9U,168U,4134U},
  {2U,260U,361U,4135U},
  {2U,278U,951U,4136U},
  {2U,194U,999U,4137U},
  {2U,247U,775U,4138U},
  {2U,365U,688U,4139U},
  {2U,301U,413U,4140U},
  {2U,281U,261U,4141U},
  {2U,491U,147U,4142U},
  {2U,408U,572U,4143U},
  {2U,431U,636U,4144U},
  {2U,380U,341U,4145U},
  {2U,222U,93U,4146U},
  {2U,223U,881U,4147U},
  {2U,148U,41U,4148U},
  {2U,436U,56U,4149U},
  {2U,444U,585U,4150U},
  {2U,318U,938U,4151U},
  {2U,493U,757U,4152U},
  {2U,3U,490U,4153U},
  {2U,83U,122U,4154U},
  {2U,305U,864U,4155U},
  {2U,47U,549U,4156U},
  {2U,104U,639U,4157U},
  {2U,275U,184U,4158U},
  {2U,197U,392U,4159U},
  {2U,201U,580U,4160U},
  {2U,182U,558U,4161U},
  {2U,476U,808U,4162U},
  {2U,25U,92U,4163U},
  {2U,290U,452U,4164U},
  {2U,88U,989U,4165U},
  {2U,178U,698U,4166U},
  {2U,256U,515U,4167U},
  {2U,84U,646U,4168U},
  {2U,288U,240U,4169U},
  {2U,84U,36U,4170U},
  {2U,122U,592U,4171U},
  {2U,405U,153U,4172U},
  {2U,456U,412U,4173U},
  {2U,458U,145U,4174U},
  {2U,91U,774U,4175U},
  {2U,356U,450U,4176U},
  {2U,362U,230U,4177U},
  {2U,72U,614U,4178U},
  {2U,127U,80U,4179U},
  {2U,407U,124U,4180U},
  {2U,431U,995U,4181U},
  {2U,359U,47U,4182U},
  {2U,495U,532U,4183U},
  {2U,492U,552U,4184U},
  {2U,457U,870U,4185U},
  {2U,56U,133U,4186U},
  {2U,74U,532U,4187U},
  {2U,193U,588U,4188U},
  {2U,255U,638U,4189U},
  {2U,53U,613U,4190U},
  {2U,347U,130U,4191U},
  {2U,246U,967U,4192U},
  {2U,432U,495U,4193U},
  {2U,216U,397U,4194U},
  {2U,278U,893U,4195U},
  {2U,471U,109U,4196U},
  {2U,308U,566U,4197U},
  {2U,395U,171U,4198U},
  {2U,217U,691U,4199U},
  {2U,415U,918U,4200U},
  {2U,158U,34U,4201U},
  {2U,260U,72U,4202U},
  {2U,173U,415U,4203U},
  {2U,182U,787U,4204U},
  {2U,419U,430U,4205U},
  {2U,247U,400U,4206U},
  {2U,127U,502U,4207U},
  {2U,158U,631U,4208U},
  {2U,290U,769U,4209U},
  {2U,236U,931U,4210U},
  {2U,165U,393U,4211U},
  {2U,385U,819U,4212U},
  {2U,500U,219U,4213U},
  {2U,288U,469U,4214U},
  {2U,32U,920U,4215U},
  {2U,61U,349U,4216U},
  {2U,443U,970U,4217U},
  {2U,12U,388U,4218U},
  {2U,292U,575U,4219U},
  {2U,211U,449U,4220U},
  {2U,194U,349U,4221U},
  {2U,234U,459U,4222U},
  {2U,343U,156U,4223U},
  {2U,222U,356U,4224U},
  {2U,337U,153U,4225U},
  {2U,370U,493U,4226U},
  {2U,41U,461U,4227U},
  {2U,273U,547U,4228U},
  {2U,80U,36U,4229U},
  {2U,449U,782U,4230U},
  {2U,493U,110U,4231U},
  {2U,128U,107U,4232U},
  {2U,142U,173U,4233U},
  {2U,177U,588U,4234U},
  {2U,86U,331U,4235U},
  {2U,236U,411U,4236U},
  {2U,225U,957U,4237U},
  {2U,0U,488U,4238U},
  {2U,220U,22U,4239U},
  {2U,458U,620U,4240U},
  {2U,287U,751U,4241U},
  {2U,92U,649U,4242U},
  {2U,145U,245U,4243U},
  {2U,133U,366U,4244U},
  {2U,226U,842U,4245U},
  {2U,357U,864U,4246U},
  {2U,266U,373U,4247U},
  {2U,277U,927U,4248U},
  {2U,306U,156U,4249U},
  {2U,78U,696U,4250U},
  {2U,306U,924U,4251U},
  {2U,226U,696U,4252U},
  {2U,191U,931U,4253U},
  {2U,279U,726U,4254U},
  {2U,330U,770U,4255U},
  {2U,119U,824U,4256U},
  {2U,482U,387U,4257U},
  {2U,455U,314U,4258U},
  {2U,320U,82U,4259U},
  {2U,58U,633U,4260U},
  {2U,128U,182U,4261U},
  {2U,320U,764U,4262U},
  {2U,28U,732U,4263U},
  {2U,90U,695U,4264U},
  {2U,429U,213U,4265U},
  {2U,317U,798U,4266U},
  {2U,151U,206U,4267U},
  {2U,16U,476U,4268U},
  {2U,346U,716U,4269U},
  {2U,446U,690U,4270U},
  {2U,143U,694U,4271U},
  {2U,115U,620U,4272U},
  {2U,286U,684U,4273U},
  {2U,181U,235U,4274U},
  {2U,294U,591U,4275U},
  {2U,20U,158U,4276U},
  {2U,350U,828U,4277U},
  {2U,464U,558U,4278U},
  {2U,215U,42U,4279U},
  {2U,112U,408U,4280U},
  {2U,7U,520U,4281U},
  {2U,440U,559U,4282U},
  {2U,197U,137U,4283U},
  {2U,220U,853U,4284U},
  {2U,84U,405U,4285U},
  {2U,343U,282U,4286U},
  {2U,311U,716U,4287U},
  {2U,243U,997U,4288U},
  {2U,8U,513U,4289U},
  {2U,351U,681U,4290U},
  {2U,81U,400U,4291U},
  {2U,157U,49U,4292U},
  {2U,287U,414U,4293U},
  {2U,455U,895U,4294U},
  {2U,208U,423U,4295U},
  {2U,172U,747U,4296U},
  {2U,293U,894U,4297U},
  {2U,490U,122U,4298U},
  {2U,252U,869U,4299U},
  {2U,327U,693U,4300U},
  {2U,86U,475U,4301U},
  {2U,48U,392U,4302U},
  {2U,217U,685U,4303U},
  {2U,177U,161U,4304U},
  {2U,431U,399U,4305U},
  {2U,75U,786U,4306U},
  {2U,358U,184U,4307U},
  {2U,348U,359U,4308U},
  {2U,40U,785U,4309U},
  {2U,35U,140U,4310U},
  {2U,479U,115U,4311U},
  {2U,102U,41U,4312U},
  {2U,213U,417U,4313U},
  {2U,64U,147U,4314U},
  {2U,155U,934U,4315U},
  {2U,330U,759U,4316U},
  {2U,466U,238U,4317U},
  {2U,459U,11U,4318U},
  {2U,95U,633U,4319U},
  {2U,54U,272U,4320U},
  {2U,74U,883U,4321U},
  {2U,214U,386U,4322U},
  {2U,173U,103U,4323U},
  {2U,217U,109U,4324U},
  {2U,323U,284U,4325U},
  {2U,313U,481U,4326U},
  {2U,248U,840U,4327U},
  {2U,236U,504U,4328U},
  {2U,240U,244U,4329U},
  {2U,210U,207U,4330U},
  {2U,387U,461U,4331U},
  {2U,57U,978U,4332U},
  {2U,323U,212U,4333U},
  {2U,505U,159U,4334U},
  {2U,476U,644U,4335U},
  {2U,1U,68U,4336U},
  {2U,178U,394U,4337U},
  {2U,510U,178U,4338U},
  {2U,24U,567U,4339U},
  {2U,197U,896U,4340U},
  {2U,370U,576U,4341U},
  {2U,471U,985U,4342U},
  {2U,427U,987U,4343U},
  {2U,178U,698U,4344U},
  {2U,125U,542U,4345U},
  {2U,145U,512U,4346U},
  {2U,461U,363U,4347U},
  {2U,79U,264U,4348U},
  {2U,293U,683U,4349U},
  {2U,12U,908U,4350U},
  {2U,480U,8U,4351U},
  {2U,414U,502U,4352U},
  {2U,314U,480U,4353U},
  {2U,511U,394U,4354U},
  {2U,218U,880U,4355U},
  {2U,461U,395U,4356U},
  {2U,326U,369U,4357U},
  {2U,153U,975U,4358U},
  {2U,101U,42U,4359U},
  {2U,470U,534U,4360U},
  {2U,342U,608U,4361U},
  {2U,150U,874U,4362U},
  {2U,478U,207U,4363U},
  {2U,422U,954U,4364U},
  {2U,445U,429U,4365U},
  {2U,207U,575U,4366U},
  {2U,320U,750U,4367U},
  {2U,241U,12U,4368U},
  {2U,255U,100U,4369U},
  {2U,461U,817U,4370U},
  {2U,507U,506U,4371U},
  {2U,245U,661U,4372U},
  {2U,133U,162U,4373U},
  {2U,231U,361U,4374U},
  {2U,389U,482U,4375U},
  {2U,463U,528U,4376U},
  {2U,245U,410U,4377U},
  {2U,100U,939U,4378U},
  {2U,451U,492U,4379U},
  {2U,7U,437U,4380U},
  {2U,26U,375U,4381U},
  {2U,307U,420U,4382U},
  {2U,145U,161U,4383U},
  {2U,216U,614U,4384U},
  {2U,239U,745U,4385U},
  {2U,205U,603U,4386U},
  {2U,133U,773U,4387U},
  {2U,48U,659U,4388U},
  {2U,238U,161U,4389U},
  {2U,484U,766U,4390U},
  {2U,412U,953U,4391U},
  {2U,290U,298U,4392U},
  {2U,150U,605U,4393U},
  {2U,169U,432U,4394U},
  {2U,290U,377U,4395U},
  {2U,176U,502U,4396U},
  {2U,256U,806U,4397U},
  {2U,99U,668U,4398U},
  {2U,341U,575U,4399U},
  {2U,1U,353U,4400U},
  {2U,479U,799U,4401U},
  {2U,337U,191U,4402U},
  {2U,119U,899U,4403U},
  {2U,311U,854U,4404U},
  {2U,16U,598U,4405U},
  {2U,347U,642U,4406U},
  {2U,276U,339U,4407U},
  {2U,99U,428U,4408U},
  {2U,162U,766U,4409U},
  {2U,182U,849U,4410U},
  {2U,418U,640U,4411U},
  {2U,108U,565U,4412U},
  {2U,107U,349U,4413U},
  {2U,241U,71U,4414U},
  {2U,17U,461U,4415U},
  {2U,407U,997U,4416U},
  {2U,506U,464U,4417U},
  {2U,506U,377U,4418U},
  {2U,283U,554U,4419U},
  {2U,493U,227U,4420U},
  {2U,6U,786U,4421U},
  {2U,228U,403U,4422U},
  {2U,67U,153U,4423U},
  {2U,152U,266U,4424U},
  {2U,119U,769U,4425U},
  {2U,25U,855U,4426U},
  {2U,124U,166U,4427U},
  {2U,428U,985U,4428U},
  {2U,426U,841U,4429U},
  {2U,247U,458U,4430U},
  {2U,287U,155U,4431U},
  {2U,192U,829U,4432U},
  {2U,391U,8U,4433U},
  {2U,85U,515U,4434U},
  {2U,404U,253U,4435U},
  {2U,41U,788U,4436U},
  {2U,75U,804U,4437U},
  {2U,103U,433U,4438U},
  {2U,340U,916U,4439U},
  {2U,238U,581U,4440U},
  {2U,311U,370U,4441U},
  {2U,215U,59U,4442U},
  {2U,461U,112U,4443U},
  {2U,24U,714U,4444U},
  {2U,375U,20U,4445U},
  {2U,208U,322U,4446U},
  {2U,154U,46U,4447U},
  {2U,84U,99U,4448U},
  {2U,496U,668U,4449U},
  {2U,41U,405U,4450U},
  {2U,386U,213U,4451U},
  {2U,285U,78U,4452U},
  {2U,66U,620U,4453U},
  {2U,237U,907U,4454U},
  {2U,123U,606U,4455U},
  {2U,391U,942U,4456U},
  {2U,477U,662U,4457U},
  {2U,35U,347U,4458U},
  {2U,442U,978U,4459U},
  {2U,292U,958U,4460U},
  {2U,167U,831U,4461U},
  {2U,103U,796U,4462U},
  {2U,117U,354U,4463U},
  {2U,166U,686U,4464U},
  {2U,325U,356U,4465U},
  {2U,111U,181U,4466U},
  {2U,398U,889U,4467U},
  {2U,315U,368U,4468U},
  {2U,490U,826U,4469U},
  {2U,82U,794U,4470U},
  {2U,410U,492U,4471U},
  {2U,444U,772U,4472U},
  {2U,438U,175U,4473U},
  {2U,123U,391U,4474U},
  {2U,318U,851U,4475U},
  {2U,316U,853U,4476U},
  {2U,281U,120U,4477U},
  {2U,473U,837U,4478U},
  {2U,438U,568U,4479U},
  {2U,158U,402U,4480U},
  {2U,82U,243U,4481U},
  {2U,211U,704U,4482U},
  {2U,179U,683U,4483U},
  {2U,107U,73U,4484U},
  {2U,470U,370U,4485U},
  {2U,506U,633U,4486U},
  {2U,324U,839U,4487U},
  {2U,363U,212U,4488U},
  {2U,17U,330U,4489U},
  {2U,433U,169U,4490U},
  {2U,1U,616U,4491U},
  {2U,28U,124U,4492U},
  {2U,405U,74U,4493U},
  {2U,176U,459U,4494U},
  {2U,264U,429U,4495U},
  {2U,197U,270U,4496U},
  {2U,230U,505U,4497U},
  {2U,344U,505U,4498U},
  {2U,447U,76U,4499U},
  {2U,336U,438U,4500U},
  {2U,188U,784U,4501U},
  {2U,342U,523U,4502U},
  {2U,149U,925U,4503U},
  {2U,136U,201U,4504U},
  {2U,364U,802U,4505U},
  {2U,414U,36U,4506U},
  {2U,35U,173U,4507U},
  {2U,38U,569U,4508U},
  {2U,303U,824U,4509U},
  {2U,26U,554U,4510U},
  {2U,417U,378U,4511U},
  {2U,454U,439U,4512U},
  {2U,426U,31U,4513U},
  {2U,437U,446U,4514U},
  {2U,347U,608U,4515U},
  {2U,303U,712U,4516U},
  {2U,291U,626U,4517U},
  {2U,453U,128U,4518U},
  {2U,471U,202U,4519U},
  {2U,121U,28U,4520U},
  {2U,355U,941U,4521U},
  {2U,120U,513U,4522U},
  {2U,231U,703U,4523U},
  {2U,124U,83U,4524U},
  {2U,261U,353U,4525U},
  {2U,94U,163U,4526U},
  {2U,369U,924U,4527U},
  {2U,127U,158U,4528U},
  {2U,338U,373U,4529U},
  {2U,154U,512U,4530U},
  {2U,494U,280U,4531U},
  {2U,16U,515U,4532U},
  {2U,184U,63U,4533U},
  {2U,18U,726U,4534U},
  {2U,240U,848U,4535U},
  {2U,486U,551U,4536U},
  {2U,472U,154U,4537U},
  {2U,409U,102U,4538U},
  {2U,297U,483U,4539U},
  {2U,411U,89U,4540U},
  {2U,375U,359U,4541U},
  {2U,78U,503U,4542U},
  {2U,373U,152U,4543U},
  {2U,10U,524U,4544U},
  {2U,64U,195U,4545U},
  {2U,63U,596U,4546U},
  {2U,90U,208U,4547U},
  {2U,508U,533U,4548U},
  {2U,199U,444U,4549U},
  {2U,278U,585U,4550U},
  {2U,226U,955U,4551U},
  {2U,424U,680U,4552U},
  {2U,184U,440U,4553U},
  {2U,171U,197U,4554U},
  {2U,307U,603U,4555U},
  {2U,362U,755U,4556U},
  {2U,242U,441U,4557U},
  {2U,366U,969U,4558U},
  {2U,460U,262U,4559U},
  {2U,54U,21U,4560U},
  {2U,68U,623U,4561U},
  {2U,306U,819U,4562U},
  {2U,103U,957U,4563U},
  {2U,254U,775U,4564U},
  {2U,286U,355U,4565U},
  {2U,381U,814U,4566U},
  {2U,237U,631U,4567U},
  {2U,31U,9U,4568U},
  {2U,171U,36U,4569U},
  {2U,222U,180U,4570U},
  {2U,78U,922U,4571U},
  {2U,412U,914U,4572U},
  {2U,274U,316U,4573U},
  {2U,56U,357U,4574U},
  {2U,58U,674U,4575U},
  {2U,292U,717U,4576U},
  {2U,26U,969U,4577U},
  {2U,112U,904U,4578U},
  {2U,393U,93U,4579U},
  {2U,103U,346U,4580U},
  {2U,51U,56U,4581U},
  {2U,338U,584U,4582U},
  {2U,248U,662U,4583U},
  {2U,5U,886U,4584U},
  {2U,281U,902U,4585U},
  {2U,322U,243U,4586U},
  {2U,159U,686U,4587U},
  {2U,30U,925U,4588U},
  {2U,359U,135U,4589U},
  {2U,183U,970U,4590U},
  {2U,358U,328U,4591U},
  {2U,347U,76U,4592U},
  {2U,253U,257U,4593U},
  {2U,278U,490U,4594U},
  {2U,346U,997U,4595U},
  {2U,442U,478U,4596U},
  {2U,412U,627U,4597U},
  {2U,266U,212U,4598U},
  {2U,433U,36U,4599U},
  {2U,472U,234U,4600U},
  {2U,391U,711U,4601U},
  {2U,17U,501U,4602U},
  {2U,346U,57U,4603U},
  {2U,135U,386U,4604U},
  {2U,262U,806U,4605U},
  {2U,223U,518U,4606U},
  {2U,197U,432U,4607U},
  {2U,487U,871U,4608U},
  {2U,246U,212U,4609U},
  {2U,271U,550U,4610U},
  {2U,384U,462U,4611U},
  {2U,392U,62U,4612U},
  {2U,399U,925U,4613U},
  {2U,373U,789U,4614U},
  {2U,110U,307U,4615U},
  {2U,272U,180U,4616U},
  {2U,246U,164U,4617U},
  {2U,243U,875U,4618U},
  {2U,130U,39U,4619U},
  {2U,280U,507U,4620U},
  {2U,450U,463U,4621U},
  {2U,252U,482U,4622U},
  {2U,488U,871U,4623U},
  {2U,506U,111U,4624U},
  {2U,19U,624U,4625U},
  {2U,329U,713U,4626U},
  {2U,81U,827U,4627U},
  {2U,444U,780U,4628U},
  {2U,425U,626U,4629U},
  {2U,28U,976U,4630U},
  {2U,154U,355U,4631U},
  {2U,331U,210U,4632U},
  {2U,9U,59U,4633U},
  {2U,372U,308U,4634U},
  {2U,308U,904U,4635U},
  {2U,105U,946U,4636U},
  {2U,209U,997U,4637U},
  {2U,325U,880U,4638U},
  {2U,425U,259U,4639U},
  {2U,235U,677U,4640U},
  {2U,438U,818U,4641U},
  {2U,53U,649U,4642U},
  {2U,142U,388U,4643U},
  {2U,485U,538U,4644U},
  {2U,336U,106U,4645U},
  {2U,496U,234U,4646U},
  {2U,47U,928U,4647U},
  {2U,394U,680U,4648U},
  {2U,437U,172U,4649U},
  {2U,164U,346U,4650U},
  {2U,114U,903U,4651U},
  {2U,492U,26U,4652U},
  {2U,228U,516U,4653U},
  {2U,19U,182U,4654U},
  {2U,408U,288U,4655U},
  {2U,497U,938U,4656U},
  {2U,137U,442U,4657U},
  {2U,196U,256U,4658U},
  {2U,277U,227U,4659U},
  {2U,42U,75U,4660U},
  {2U,300U,343U,4661U},
  {2U,247U,703U,4662U},
  {2U,44U,280U,4663U},
  {2U,344U,292U,4664U},
  {2U,188U,922U,4665U},
  {2U,308U,418U,4666U},
  {2U,437U,186U,4667U},
  {2U,430U,978U,4668U},
  {2U,18U,594U,4669U},
  {2U,86U,741U,4670U},
  {2U,386U,414U,4671U},
  {2U,79U,424U,4672U},
  {2U,301U,425U,4673U},
  {2U,120U,397U,4674U},
  {2U,511U,722U,4675U},
  {2U,203U,344U,4676U},
  {2U,161U,959U,4677U},
  {2U,448U,570U,4678U},
  {2U,290U,869U,4679U},
  {2U,33U,919U,4680U},
  {2U,160U,231U,4681U},
  {2U,336U,286U,4682U},
  {2U,1U,681U,4683U},
  {2U,170U,334U,4684U},
  {2U,154U,678U,4685U},
  {2U,255U,346U,4686U},
  {2U,117U,969U,4687U},
  {2U,142U,376U,4688U},
  {2U,177U,418U,4689U},
  {2U,22U,183U,4690U},
  {2U,337U,429U,4691U},
  {2U,291U,621U,4692U},
  {2U,110U,156U,4693U},
  {2U,138U,970U,4694U},
  {2U,501U,95U,4695U},
  {2U,86U,984U,4696U},
  {2U,194U,788U,4697U},
  {2U,422U,255U,4698U},
  {2U,431U,749U,4699U},
  {2U,301U,392U,4700U},
  {2U,116U,377U,4701U},
  {2U,243U,394U,4702U},
  {2U,160U,144U,4703U},
  {2U,508U,865U,4704U},
  {2U,354U,779U,4705U},
  {2U,74U,26U,4706U},
  {2U,502U,68U,4707U},
  {2U,109U,152U,4708U},
  {2U,414U,93U,4709U},
  {2U,25U,541U,4710U},
  {2U,452U,615U,4711U},
  {2U,4U,426U,4712U},
  {2U,155U,176U,4713U},
  {2U,289U,64U,4714U},
  {2U,116U,558U,4715U},
  {2U,413U,373U,4716U},
  {2U,239U,876U,4717U},
  {2U,507U,789U,4718U},
  {2U,375U,548U,4719U},
  {2U,49U,448U,4720U},
  {2U,362U,714U,4721U},
  {2U,357U,519U,4722U},
  {2U,321U,811U,4723U},
  {2U,259U,48U,4724U},
  {2U,112U,919U,4725U},
  {2U,104U,446U,4726U},
  {2U,485U,36U,4727U},
  {2U,393U,680U,4728U},
  {2U,394U,832U,4729U},
  {2U,382U,324U,4730U},
  {2U,190U,324U,4731U},
  {2U,326U,537U,4732U},
  {2U,274U,791U,4733U},
  {2U,106U,375U,4734U},
  {2U,75U,950U,4735U},
  {2U,275U,990U,4736U},
  {2U,264U,214U,4737U},
  {2U,13U,582U,4738U},
  {2U,16U,428U,4739U},
  {2U,486U,135U,4740U},
  {2U,163U,388U,4741U},
  {2U,428U,390U,4742U},
  {2U,459U,882U,4743U},
  {2U,121U,495U,4744U},
  {2U,211U,601U,4745U},
  {2U,298U,326U,4746U},
  {2U,484U,436U,4747U},
  {2U,224U,488U,4748U},
  {2U,358U,98U,4749U},
  {2U,499U,751U,4750U},
  {2U,403U,68U,4751U},
  {2U,199U,846U,4752U},
  {2U,255U,17U,4753U},
  {2U,13U,8U,4754U},
  {2U,500U,670U,4755U},
  {2U,124U,275U,4756U},
  {2U,394U,333U,4757U},
  {2U,219U,747U,4758U},
  {2U,112U,94U,4759U},
  {2U,316U,42U,4760U},
  {2U,57U,732U,4761U},
  {2U,156U,578U,4762U},
  {2U,52U,126U,4763U},
  {2U,172U,522U,4764U},
  {2U,118U,922U,4765U},
  {2U,430U,335U,4766U},
  {2U,416U,987U,4767U},
  {2U,374U,827U,4768U},
  {2U,380U,22U,4769U},
  {2U,198U,498U,4770U},
  {2U,315U,25U,4771U},
  {2U,314U,696U,4772U},
  {2U,202U,709U,4773U},
  {2U,246U,682U,4774U},
  {2U,267U,97U,4775U},
  {2U,284U,269U,4776U},
  {2U,128U,667U,4777U},
  {2U,172U,17U,4778U},
  {2U,282U,922U,4779U},
  {2U,275U,809U,4780U},
  {2U,303U,207U,4781U},
  {2U,25U,556U,4782U},
  {2U,342U,989U,4783U},
  {2U,351U,180U,4784U},
  {2U,403U,783U,4785U},
  {2U,71U,581U,4786U},
  {2U,352U,853U,4787U},
  {2U,473U,997U,4788U},
  {2U,16U,103U,4789U},
  {2U,443U,677U,4790U},
  {2U,255U,126U,4791U},
  {2U,158U,264U,4792U},
  {2U,397U,580U,4793U},
  {2U,337U,514U,4794U},
  {2U,363U,797U,4795U},
  {2U,35U,714U,4796U},
  {2U,30U,881U,4797U},
  {2U,162U,902U,4798U},
  {2U,40U,503U,4799U},
  {2U,203U,231U,4800U},
  {2U,301U,488U,4801U},
  {2U,59U,848U,4802U},
  {2U,301U,616U,4803U},
  {2U,431U,951U,4804U},
  {2U,158U,333U,4805U},
  {2U,93U,963U,4806U},
  {2U,122U,778U,4807U},
  {2U,54U,758U,4808U},
  {2U,430U,281U,4809U},
  {2U,363U,822U,4810U},
  {2U,226U,982U,4811U},
  {2U,443U,270U,4812U},
  {2U,195U,334U,4813U},
  {2U,454U,230U,4814U},
  {2U,212U,36U,4815U},
  {2U,175U,848U,4816U},
  {2U,443U,988U,4817U},
  {2U,422U,428U,4818U},
  {2U,89U,407U,4819U},
  {2U,266U,343U,4820U},
  {2U,85U,642U,4821U},
  {2U,87U,495U,4822U},
  {2U,195U,684U,4823U},
  {2U,381U,142U,4824U},
  {2U,191U,597U,4825U},
  {2U,281U,963U,4826U},
  {2U,441U,985U,4827U},
  {2U,287U,852U,4828U},
  {2U,483U,922U,4829U},
  {2U,480U,835U,4830U},
  {2U,487U,200U,4831U},
  {2U,80U,21U,4832U},
  {2U,442U,748U,4833U},
  {2U,327U,323U,4834U},
  {2U,442U,225U,4835U},
  {2U,404U,386U,4836U},
  {2U,302U,374U,4837U},
  {2U,327U,996U,4838U},
  {2U,231U,278U,4839U},
  {2U,67U,614U,4840U},
  {2U,218U,668U,4841U},
  {2U,52U,663U,4842U},
  {2U,125U,196U,4843U},
  {2U,308U,219U,4844U},
  {2U,330U,200U,4845U},
  {2U,429U,97U,4846U},
  {2U,335U,917U,4847U},
  {2U,245U,740U,4848U},
  {2U,293U,804U,4849U},
  {2U,245U,504U,4850U},
  {2U,120U,705U,4851U},
  {2U,376U,287U,4852U},
  {2U,457U,489U,4853U},
  {2U,435U,964U,4854U},
  {2U,112U,187U,4855U},
  {2U,40U,917U,4856U},
  {2U,295U,171U,4857U},
  {2U,359U,182U,4858U},
  {2U,282U,465U,4859U},
  {2U,360U,66U,4860U},
  {2U,64U,186U,4861U},
  {2U,71U,994U,4862U},
  {2U,363U,291U,4863U},
  {2U,59U,844U,4864U},
  {2U,365U,923U,4865U},
  {2U,341U,728U,4866U},
  {2U,150U,295U,4867U},
  {2U,154U,215U,4868U},
  {2U,399U,352U,4869U},
  {2U,408U,876U,4870U},
  {2U,240U,987U,4871U},
  {2U,97U,506U,4872U},
  {2U,22U,358U,4873U},
  {2U,483U,354U,4874U},
  {2U,268U,738U,4875U},
  {2U,139U,270U,4876U},
  {2U,192U,132U,4877U},
  {2U,53U,953U,4878U},
  {2U,448U,278U,4879U},
  {2U,55U,755U,4880U},
  {2U,177U,665U,4881U},
  {2U,327U,877U,4882U},
  {2U,492U,240U,4883U},
  {2U,27U,227U,4884U},
  {2U,198U,602U,4885U},
  {2U,31U,922U,4886U},
  {2U,126U,504U,4887U},
  {2U,229U,414U,4888U},
  {2U,227U,301U,4889U},
  {2U,424U,999U,4890U},
  {2U,439U,399U,4891U},
  {2U,270U,840U,4892U},
  {2U,82U,429U,4893U},
  {2U,57U,904U,4894U},
  {2U,77U,611U,4895U},
  {2U,70U,361U,4896U},
  {2U,28U,826U,4897U},
  {2U,77U,914U,4898U},
  {2U,225U,223U,4899U},
  {2U,388U,46U,4900U},
  {2U,418U,635U,4901U},
  {2U,268U,16U,4902U},
  {2U,135U,423U,4903U},
  {2U,1U,326U,4904U},
  {2U,71U,827U,4905U},
  {2U,278U,362U,4906U},
  {2U,352U,144U,4907U},
  {2U,477U,346U,4908U},
  {2U,493U,630U,4909U},
  {2U,443U,691U,4910U},
  {2U,190U,894U,4911U},
  {2U,205U,109U,4912U},
  {2U,200U,689U,4913U},
  {2U,354U,758U,4914U},
  {2U,334U,758U,4915U},
  {2U,281U,811U,4916U},
  {2U,156U,394U,4917U},
  {2U,77U,391U,4918U},
  {2U,117U,719U,4919U},
  {2U,270U,239U,4920U},
  {2U,365U,949U,4921U},
  {2U,201U,987U,4922U},
  {2U,318U,709U,4923U},
  {2U,335U,24U,4924U},
  {2U,235U,364U,4925U},
  {2U,94U,584U,4926U},
  {2U,452U,145U,4927U},
  {2U,134U,108U,4928U},
  {2U,483U,823U,4929U},
  {2U,478U,319U,4930U},
  {2U,119U,776U,4931U},
  {2U,477U,617U,4932U},
  {2U,316U,563U,4933U},
  {2U,117U,149U,4934U},
  {2U,435U,917U,4935U},
  {2U,391U,740U,4936U},
  {2U,34U,683U,4937U},
  {2U,122U,991U,4938U},
  {2U,118U,285U,4939U},
  {2U,83U,37U,4940U},
  {2U,324U,61U,4941U},
  {2U,249U,539U,4942U},
  {2U,403U,551U,4943U},
  {2U,0U,929U,4944U},
  {2U,102U,161U,4945U},
  {2U,316U,982U,4946U},
  {2U,24U,100U,4947U},
  {2U,330U,907U,4948U},
  {2U,361U,869U,4949U},
  {2U,268U,656U,4950U},
  {2U,291U,266U,4951U},
  {2U,144U,124U,4952U},
  {2U,355U,83U,4953U},
  {2U,92U,939U,4954U},
  {2U,258U,110U,4955U},
  {2U,258U,42U,4956U},
  {2U,18U,52U,4957U},
  {2U,377U,708U,4958U},
  {2U,144U,127U,4959U},
  {2U,146U,276U,4960U},
  {2U,159U,854U,4961U},
  {2U,293U,184U,4962U},
  {2U,175U,747U,4963U},
  {2U,489U,881U,4964U},
  {2U,302U,186U,4965U},
  {2U,339U,589U,4966U},
  {2U,442U,251U,4967U},
  {2U,409U,59U,4968U},
  {2U,203U,822U,4969U},
  {2U,342U,98U,4970U},
  {2U,457U,315U,4971U},
  {2U,503U,207U,4972U},
  {2U,254U,675U,4973U},
  {2U,40U,186U,4974U},
  {2U,19U,454U,4975U},
  {2U,365U,502U,4976U},
  {2U,355U,661U,4977U},
  {2U,236U,842U,4978U},
  {2U,470U,673U,4979U},
  {2U,81U,263U,4980U},
  {2U,253U,749U,4981U},
  {2U,271U,461U,4982U},
  {2U,40U,303U,4983U},
  {2U,316U,292U,4984U},
  {2U,261U,989U,4985U},
  {2U,379U,752U,4986U},
  {2U,321U,262U,4987U},
  {2U,462U,718U,4988U},
  {2U,306U,930U,4989U},
  {2U,420U,227U,4990U},
  {2U,191U,851U,4991U},
  {2U,52U,459U,4992U},
  {2U,201U,523U,4993U},
  {2U,463U,159U,4994U},
  {2U,219U,820U,4995U},
  {2U,205U,794U,4996U},
  {2U,340U,126U,4997U},
  {2U,313U,404U,4998U},
  {2U,506U,651U,4999U},
  {2U,274U,70U,5000U},
  {2U,400U,150U,5001U},
  {2U,135U,855U,5002U},
  {2U,455U,100U,5003U},
  {2U,227U,141U,5004U},
  {2U,96U,414U,5005U},
  {2U,359U,611U,5006U},
  {2U,327U,541U,5007U},
  {2U,290U,355U,5008U},
  {2U,160U,365U,5009U},
  {2U,414U,159U,5010U},
  {2U,268U,443U,5011U},
  {2U,509U,675U,5012U},
  {2U,441U,639U,5013U},
  {2U,160U,158U,5014U},
  {2U,144U,255U,5015U},
  {2U,353U,862U,5016U},
  {2U,105U,40U,5017U},
  {2U,181U,496U,5018U},
  {2U,48U,122U,5019U},
  {2U,287U,237U,5020U},
  {2U,499U,462U,5021U},
  {2U,365U,399U,5022U},
  {2U,159U,871U,5023U},
  {2U,104U,742U,5024U},
  {2U,323U,645U,5025U},
  {2U,396U,119U,5026U},
  {2U,153U,82U,5027U},
  {2U,489U,538U,5028U},
  {2U,147U,659U,5029U},
  {2U,363U,651U,5030U},
  {2U,374U,918U,5031U},
  {2U,220U,900U,5032U},
  {2U,385U,188U,5033U},
  {2U,187U,627U,5034U},
  {2U,417U,260U,5035U},
  {2U,33U,863U,5036U},
  {2U,51U,713U,5037U},
  {2U,99U,813U,5038U},
  {2U,309U,833U,5039U},
  {2U,227U,596U,5040U},
  {2U,324U,251U,5041U},
  {2U,480U,381U,5042U},
  {2U,210U,51U,5043U},
  {2U,261U,101U,5044U},
  {2U,438U,876U,5045U},
  {2U,334U,707U,5046U},
  {2U,133U,815U,5047U},
  {2U,140U,508U,5048U},
  {2U,149U,430U,5049U},
  {2U,450U,956U,5050U},
  {2U,123U,398U,5051U},
  {2U,328U,484U,5052U},
  {2U,73U,183U,5053U},
  {2U,336U,77U,5054U},
  {2U,310U,847U,5055U},
  {2U,44U,495U,5056U},
  {2U,359U,802U,5057U},
  {2U,372U,682U,5058U},
  {2U,119U,916U,5059U},
  {2U,337U,153U,5060U},
  {2U,325U,999U,5061U},
  {2U,67U,662U,5062U},
  {2U,500U,241U,5063U},
  {2U,286U,927U,5064U},
  {2U,400U,36U,5065U},
  {2U,98U,552U,5066U},
  {2U,254U,222U,5067U},
  {2U,304U,267U,5068U},
  {2U,408U,441U,5069U},
  {2U,223U,624U,5070U},
  {2U,240U,732U,5071U},
  {2U,218U,804U,5072U},
  {2U,264U,72U,5073U},
  {2U,178U,184U,5074U},
  {2U,285U,617U,5075U},
  {2U,201U,411U,5076U},
  {2U,497U,989U,5077U},
  {2U,385U,907U,5078U},
  {2U,510U,288U,5079U},
  {2U,43U,843U,5080U},
  {2U,205U,715U,5081U},
  {2U,289U,828U,5082U},
  {2U,198U,904U,5083U},
  {2U,176U,574U,5084U},
  {2U,58U,811U,5085U},
  {2U,139U,629U,5086U},
  {2U,489U,593U,5087U},
  {2U,71U,764U,5088U},
  {2U,443U,415U,5089U},
  {2U,126U,135U,5090U},
  {2U,107U,49U,5091U},
  {2U,143U,175U,5092U},
  {2U,183U,398U,5093U},
  {2U,224U,488U,5094U},
  {2U,150U,13U,5095U},
  {2U,190U,542U,5096U},
  {2U,87U,287U,5097U},
  {2U,60U,645U,5098U},
  {2U,302U,98U,5099U},
  {2U,197U,575U,5100U},
  {2U,152U,947U,5101U},
  {2U,230U,918U,5102U},
  {2U,471U,773U,5103U},
  {2U,242U,168U,5104U},
  {2U,362U,417U,5105U},
  {2U,390U,796U,5106U},
  {2U,332U,899U,5107U},
  {2U,187U,336U,5108U},
  {2U,422U,777U,5109U},
  {2U,151U,976U,5110U},
  {2U,346U,31U,5111U},
  {2U,118U,697U,5112U},
  {2U,177U,425U,5113U},
  {2U,326U,902U,5114U},
  {2U,39U,894U,5115U},
  {2U,361U,957U,5116U},
  {2U,48U,410U,5117U},
  {2U,293U,535U,5118U},
  {2U,70U,462U,5119U},
  {2U,155U,448U,5120U},
};
static const unsigned paper_offsets[2] = {0U,5120U};
static const unsigned paper_work_starts[1] = {3072U};
static const unsigned paper_expected_sums[1] = {2367720U};
static const unsigned paper_expected_live[1] = {253066U};

static unsigned benchmark_live[NR_TASKLETS];
struct Snapshot { unsigned phase, round, requested, slots, reserved, empty_bytes; };
static struct Snapshot snapshots[4];
static unsigned snapshot_count;
static volatile unsigned policy_report_meta[5];
BARRIER_INIT(policy_barrier, NR_TASKLETS);
static void snapshot(unsigned phase) {
  struct Snapshot *s = &snapshots[snapshot_count++]; assert(snapshot_count <= 4);
  s->phase=phase;s->round=0;s->reserved=policy_backend.used;
  s->requested=s->slots=s->empty_bytes=0;
  for (unsigned t=0;t<NR_TASKLETS;++t) {
    s->requested+=benchmark_live[t];s->slots+=policy_stats[t].live_slots;
    s->empty_bytes+=policy_stats[t].retained*POLICY_PAGE;
  }
}
static __attribute__((noinline)) __mram_ptr void *bench_alloc(unsigned bytes) {
  BEGIN(alloc); __mram_ptr void *p=policy_malloc(bytes); END(alloc);
  assert(p); benchmark_live[me()]+=bytes;
#ifdef HOST_TEST
  host_add_interval(p,bytes);
#endif
  return p;
}
static __attribute__((noinline)) void bench_free(__mram_ptr void *p,unsigned bytes) {
#ifdef HOST_TEST
  host_remove_interval(p,bytes);
#endif
  BEGIN(free);policy_free(p);END(free);
  /* Keep conditional fault paths after the PC probes. LLVM can duplicate a
   * preceding zero-instruction BEGIN across an assertion and its branch join. */
  assert(p && benchmark_live[me()]>=bytes);benchmark_live[me()]-=bytes;
}
static __attribute__((noinline)) void init_end(void) { snapshot(0);END(init); }
static __attribute__((noinline)) void work_end(void) { snapshot(1); }
static __attribute__((noinline)) void final_report(void) {
  END(total);
  assert(!policy_backend.used && getMetadata(policy_buddy,0,0)==3);
  for(unsigned t=0;t<NR_TASKLETS;++t) { assert(!benchmark_live[t]);policy_verify(t); }
  snapshot(99);
#ifndef HOST_TEST
  policy_report_meta[0]=(unsigned)__HEAP_POINTER;
  policy_report_meta[1]=sizeof(policy_caches);policy_report_meta[2]=sizeof(policy_stats);
  policy_report_meta[3]=snapshot_count;policy_report_meta[4]=NR_TASKLETS;
  __asm__ volatile("policy_report_ready_%=:" :: "r"(policy_stats),"r"(&policy_backend),
      "r"(snapshots),"r"(policy_report_meta) : "memory");
#endif
}

struct PaperCell { uint32_t pointer; unsigned size, tag, padding; };
PAPER_STORAGE(struct PaperCell,paper_pool,NR_TASKLETS*POOL_SIZE);
static unsigned paper_sequence[NR_TASKLETS];
static struct PaperCell load_cell(unsigned i) {
  struct PaperCell c __attribute__((aligned(8)));
  mram_read(&paper_pool[me()*POOL_SIZE+i],&c,16);return c;
}
static void store_cell(unsigned i,struct PaperCell *c) { mram_write(c,&paper_pool[me()*POOL_SIZE+i],16); }
static void cell_check(struct PaperCell *c) {
  unsigned v[2] __attribute__((aligned(8)));
  mram_read((__mram_ptr void *)c->pointer,v,8);assert(v[0]==c->tag && v[1]==~c->tag);
}
static __attribute__((noinline)) void apply_op(unsigned i) {
  struct PaperOp op __attribute__((aligned(8)));input_read(&paper_ops[i],&op,16);
  assert(op.index<POOL_SIZE);
  if(op.kind==1) {
    struct PaperCell x __attribute__((aligned(8)))=load_cell(op.index);
    struct PaperCell y __attribute__((aligned(8)))=load_cell(op.value);
    store_cell(op.index,&y);store_cell(op.value,&x);return;
  }
  if(op.kind==2) {
    struct PaperCell old __attribute__((aligned(8)))=load_cell(op.index);
    cell_check(&old);bench_free((__mram_ptr void *)old.pointer,old.size);
  }
  __mram_ptr void *p=bench_alloc(op.value);
  unsigned tag=op.kind==0 ? op.index+1 : op.tag;
  struct PaperCell c __attribute__((aligned(8)))={(uint32_t)p,op.value,tag,0};store_cell(op.index,&c);
  unsigned v[2] __attribute__((aligned(8)))={tag,~tag};mram_write(v,p,8);
}
static __attribute__((noinline)) void app_check(void) {
  unsigned sum=0,bytes=0;
  for(unsigned i=0;i<POOL_SIZE;++i) {
    struct PaperCell c __attribute__((aligned(8)))=load_cell(i);cell_check(&c);sum+=c.tag;bytes+=c.size;
  }
  assert(sum==paper_expected_sums[me()] && bytes==paper_expected_live[me()]);
  assert(bytes==benchmark_live[me()]);
}
static __attribute__((noinline)) void app_destroy(void) {
  for(unsigned i=0;i<POOL_SIZE;++i) {
    struct PaperCell c __attribute__((aligned(8)))=load_cell(i);cell_check(&c);bench_free((__mram_ptr void *)c.pointer,c.size);
  }
}
/* Dedicated functions keep zero-instruction timing labels away from main's
 * conditional return joins. Compiler tail merging must not create extra ends. */
static __attribute__((noinline)) void perform_work(unsigned start,unsigned end) {
  BEGIN(work);
  for(unsigned i=start;i<end;++i)apply_op(i);
  barrier_wait(&policy_barrier);END(work);
}
static __attribute__((noinline)) void perform_drain(void) {
  BEGIN(drain);app_destroy();policy_drain(me());barrier_wait(&policy_barrier);END(drain);
}
int main(void) {
  unsigned tid=me();
  if(!tid) {
    mem_reset();BEGIN(total);BEGIN(init);policy_init(HEAP_BYTES);
#if PAPER_KIND==0
    PAPER_BIND(paper_pool);
#else
    PAPER_BIND(paper_vertices);
#endif
  }
  barrier_wait(&policy_barrier);
#if PAPER_KIND!=0
  app_init();
#endif
  for(unsigned i=paper_offsets[tid];i<paper_work_starts[tid];++i) apply_op(i);
  barrier_wait(&policy_barrier);
  if(!tid)init_end();
  barrier_wait(&policy_barrier);
  perform_work(paper_work_starts[tid],paper_offsets[tid+1]);
  if(!tid)work_end();
  barrier_wait(&policy_barrier);
  app_check();
  if(VALIDATE)policy_verify(tid);
  barrier_wait(&policy_barrier);
  perform_drain();
  if(!tid)final_report();return 0;
}

