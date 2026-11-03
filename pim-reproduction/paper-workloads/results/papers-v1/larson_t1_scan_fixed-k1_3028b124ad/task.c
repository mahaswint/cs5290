#define NR_TASKLETS 1
#define POLICY 2
#define LISTS 0
#define LOOKUP 0
#define HEAP_BYTES 33554432
#define KEEP_PAGES 1
#define MAX_KEEP 2
#define EMPTY_BUDGET 4
#define DECAY_OPS 64
#define PRESSURE_PERCENT 75
#define VALIDATE 0
#define POOL_SIZE 512
#define INPUT_ITEMS 32768
#define TRACE_SEED 4141
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
  {0U,0U,528U,0U},
  {0U,1U,456U,0U},
  {0U,2U,444U,0U},
  {0U,3U,938U,0U},
  {0U,4U,955U,0U},
  {0U,5U,652U,0U},
  {0U,6U,906U,0U},
  {0U,7U,54U,0U},
  {0U,8U,465U,0U},
  {0U,9U,828U,0U},
  {0U,10U,907U,0U},
  {0U,11U,757U,0U},
  {0U,12U,730U,0U},
  {0U,13U,582U,0U},
  {0U,14U,784U,0U},
  {0U,15U,602U,0U},
  {0U,16U,609U,0U},
  {0U,17U,382U,0U},
  {0U,18U,375U,0U},
  {0U,19U,580U,0U},
  {0U,20U,438U,0U},
  {0U,21U,492U,0U},
  {0U,22U,809U,0U},
  {0U,23U,282U,0U},
  {0U,24U,406U,0U},
  {0U,25U,813U,0U},
  {0U,26U,258U,0U},
  {0U,27U,841U,0U},
  {0U,28U,915U,0U},
  {0U,29U,609U,0U},
  {0U,30U,646U,0U},
  {0U,31U,62U,0U},
  {0U,32U,227U,0U},
  {0U,33U,788U,0U},
  {0U,34U,254U,0U},
  {0U,35U,685U,0U},
  {0U,36U,676U,0U},
  {0U,37U,653U,0U},
  {0U,38U,469U,0U},
  {0U,39U,177U,0U},
  {0U,40U,616U,0U},
  {0U,41U,188U,0U},
  {0U,42U,415U,0U},
  {0U,43U,914U,0U},
  {0U,44U,364U,0U},
  {0U,45U,914U,0U},
  {0U,46U,174U,0U},
  {0U,47U,116U,0U},
  {0U,48U,984U,0U},
  {0U,49U,555U,0U},
  {0U,50U,655U,0U},
  {0U,51U,35U,0U},
  {0U,52U,676U,0U},
  {0U,53U,860U,0U},
  {0U,54U,281U,0U},
  {0U,55U,206U,0U},
  {0U,56U,867U,0U},
  {0U,57U,119U,0U},
  {0U,58U,975U,0U},
  {0U,59U,925U,0U},
  {0U,60U,72U,0U},
  {0U,61U,25U,0U},
  {0U,62U,576U,0U},
  {0U,63U,735U,0U},
  {0U,64U,331U,0U},
  {0U,65U,108U,0U},
  {0U,66U,934U,0U},
  {0U,67U,156U,0U},
  {0U,68U,838U,0U},
  {0U,69U,428U,0U},
  {0U,70U,298U,0U},
  {0U,71U,657U,0U},
  {0U,72U,270U,0U},
  {0U,73U,686U,0U},
  {0U,74U,577U,0U},
  {0U,75U,555U,0U},
  {0U,76U,563U,0U},
  {0U,77U,976U,0U},
  {0U,78U,422U,0U},
  {0U,79U,655U,0U},
  {0U,80U,41U,0U},
  {0U,81U,845U,0U},
  {0U,82U,709U,0U},
  {0U,83U,483U,0U},
  {0U,84U,891U,0U},
  {0U,85U,388U,0U},
  {0U,86U,751U,0U},
  {0U,87U,660U,0U},
  {0U,88U,428U,0U},
  {0U,89U,169U,0U},
  {0U,90U,498U,0U},
  {0U,91U,534U,0U},
  {0U,92U,369U,0U},
  {0U,93U,975U,0U},
  {0U,94U,490U,0U},
  {0U,95U,429U,0U},
  {0U,96U,520U,0U},
  {0U,97U,461U,0U},
  {0U,98U,816U,0U},
  {0U,99U,186U,0U},
  {0U,100U,113U,0U},
  {0U,101U,775U,0U},
  {0U,102U,350U,0U},
  {0U,103U,658U,0U},
  {0U,104U,468U,0U},
  {0U,105U,423U,0U},
  {0U,106U,143U,0U},
  {0U,107U,616U,0U},
  {0U,108U,440U,0U},
  {0U,109U,264U,0U},
  {0U,110U,575U,0U},
  {0U,111U,552U,0U},
  {0U,112U,387U,0U},
  {0U,113U,454U,0U},
  {0U,114U,531U,0U},
  {0U,115U,421U,0U},
  {0U,116U,447U,0U},
  {0U,117U,874U,0U},
  {0U,118U,786U,0U},
  {0U,119U,464U,0U},
  {0U,120U,131U,0U},
  {0U,121U,917U,0U},
  {0U,122U,161U,0U},
  {0U,123U,891U,0U},
  {0U,124U,30U,0U},
  {0U,125U,541U,0U},
  {0U,126U,55U,0U},
  {0U,127U,786U,0U},
  {0U,128U,187U,0U},
  {0U,129U,626U,0U},
  {0U,130U,830U,0U},
  {0U,131U,937U,0U},
  {0U,132U,509U,0U},
  {0U,133U,166U,0U},
  {0U,134U,693U,0U},
  {0U,135U,354U,0U},
  {0U,136U,8U,0U},
  {0U,137U,76U,0U},
  {0U,138U,100U,0U},
  {0U,139U,694U,0U},
  {0U,140U,935U,0U},
  {0U,141U,510U,0U},
  {0U,142U,87U,0U},
  {0U,143U,749U,0U},
  {0U,144U,13U,0U},
  {0U,145U,398U,0U},
  {0U,146U,902U,0U},
  {0U,147U,91U,0U},
  {0U,148U,100U,0U},
  {0U,149U,66U,0U},
  {0U,150U,92U,0U},
  {0U,151U,239U,0U},
  {0U,152U,882U,0U},
  {0U,153U,241U,0U},
  {0U,154U,512U,0U},
  {0U,155U,863U,0U},
  {0U,156U,571U,0U},
  {0U,157U,651U,0U},
  {0U,158U,700U,0U},
  {0U,159U,634U,0U},
  {0U,160U,694U,0U},
  {0U,161U,627U,0U},
  {0U,162U,947U,0U},
  {0U,163U,441U,0U},
  {0U,164U,933U,0U},
  {0U,165U,853U,0U},
  {0U,166U,123U,0U},
  {0U,167U,715U,0U},
  {0U,168U,205U,0U},
  {0U,169U,30U,0U},
  {0U,170U,536U,0U},
  {0U,171U,680U,0U},
  {0U,172U,671U,0U},
  {0U,173U,537U,0U},
  {0U,174U,867U,0U},
  {0U,175U,857U,0U},
  {0U,176U,15U,0U},
  {0U,177U,794U,0U},
  {0U,178U,935U,0U},
  {0U,179U,777U,0U},
  {0U,180U,548U,0U},
  {0U,181U,543U,0U},
  {0U,182U,665U,0U},
  {0U,183U,27U,0U},
  {0U,184U,595U,0U},
  {0U,185U,318U,0U},
  {0U,186U,847U,0U},
  {0U,187U,24U,0U},
  {0U,188U,98U,0U},
  {0U,189U,977U,0U},
  {0U,190U,989U,0U},
  {0U,191U,712U,0U},
  {0U,192U,275U,0U},
  {0U,193U,527U,0U},
  {0U,194U,230U,0U},
  {0U,195U,67U,0U},
  {0U,196U,548U,0U},
  {0U,197U,527U,0U},
  {0U,198U,599U,0U},
  {0U,199U,479U,0U},
  {0U,200U,253U,0U},
  {0U,201U,20U,0U},
  {0U,202U,196U,0U},
  {0U,203U,326U,0U},
  {0U,204U,381U,0U},
  {0U,205U,728U,0U},
  {0U,206U,779U,0U},
  {0U,207U,894U,0U},
  {0U,208U,29U,0U},
  {0U,209U,828U,0U},
  {0U,210U,423U,0U},
  {0U,211U,708U,0U},
  {0U,212U,589U,0U},
  {0U,213U,336U,0U},
  {0U,214U,614U,0U},
  {0U,215U,904U,0U},
  {0U,216U,497U,0U},
  {0U,217U,395U,0U},
  {0U,218U,583U,0U},
  {0U,219U,49U,0U},
  {0U,220U,802U,0U},
  {0U,221U,721U,0U},
  {0U,222U,713U,0U},
  {0U,223U,100U,0U},
  {0U,224U,803U,0U},
  {0U,225U,878U,0U},
  {0U,226U,285U,0U},
  {0U,227U,820U,0U},
  {0U,228U,947U,0U},
  {0U,229U,818U,0U},
  {0U,230U,91U,0U},
  {0U,231U,300U,0U},
  {0U,232U,377U,0U},
  {0U,233U,240U,0U},
  {0U,234U,823U,0U},
  {0U,235U,915U,0U},
  {0U,236U,463U,0U},
  {0U,237U,897U,0U},
  {0U,238U,837U,0U},
  {0U,239U,82U,0U},
  {0U,240U,487U,0U},
  {0U,241U,161U,0U},
  {0U,242U,79U,0U},
  {0U,243U,298U,0U},
  {0U,244U,860U,0U},
  {0U,245U,561U,0U},
  {0U,246U,750U,0U},
  {0U,247U,966U,0U},
  {0U,248U,17U,0U},
  {0U,249U,969U,0U},
  {0U,250U,800U,0U},
  {0U,251U,13U,0U},
  {0U,252U,148U,0U},
  {0U,253U,815U,0U},
  {0U,254U,803U,0U},
  {0U,255U,331U,0U},
  {0U,256U,555U,0U},
  {0U,257U,786U,0U},
  {0U,258U,83U,0U},
  {0U,259U,352U,0U},
  {0U,260U,434U,0U},
  {0U,261U,953U,0U},
  {0U,262U,107U,0U},
  {0U,263U,485U,0U},
  {0U,264U,213U,0U},
  {0U,265U,445U,0U},
  {0U,266U,798U,0U},
  {0U,267U,191U,0U},
  {0U,268U,717U,0U},
  {0U,269U,13U,0U},
  {0U,270U,611U,0U},
  {0U,271U,858U,0U},
  {0U,272U,303U,0U},
  {0U,273U,809U,0U},
  {0U,274U,890U,0U},
  {0U,275U,918U,0U},
  {0U,276U,815U,0U},
  {0U,277U,35U,0U},
  {0U,278U,260U,0U},
  {0U,279U,134U,0U},
  {0U,280U,744U,0U},
  {0U,281U,709U,0U},
  {0U,282U,892U,0U},
  {0U,283U,467U,0U},
  {0U,284U,170U,0U},
  {0U,285U,104U,0U},
  {0U,286U,247U,0U},
  {0U,287U,831U,0U},
  {0U,288U,514U,0U},
  {0U,289U,827U,0U},
  {0U,290U,91U,0U},
  {0U,291U,69U,0U},
  {0U,292U,940U,0U},
  {0U,293U,955U,0U},
  {0U,294U,219U,0U},
  {0U,295U,351U,0U},
  {0U,296U,901U,0U},
  {0U,297U,135U,0U},
  {0U,298U,645U,0U},
  {0U,299U,270U,0U},
  {0U,300U,188U,0U},
  {0U,301U,902U,0U},
  {0U,302U,911U,0U},
  {0U,303U,241U,0U},
  {0U,304U,752U,0U},
  {0U,305U,110U,0U},
  {0U,306U,375U,0U},
  {0U,307U,752U,0U},
  {0U,308U,660U,0U},
  {0U,309U,433U,0U},
  {0U,310U,299U,0U},
  {0U,311U,840U,0U},
  {0U,312U,400U,0U},
  {0U,313U,363U,0U},
  {0U,314U,602U,0U},
  {0U,315U,273U,0U},
  {0U,316U,636U,0U},
  {0U,317U,866U,0U},
  {0U,318U,292U,0U},
  {0U,319U,277U,0U},
  {0U,320U,487U,0U},
  {0U,321U,476U,0U},
  {0U,322U,729U,0U},
  {0U,323U,881U,0U},
  {0U,324U,305U,0U},
  {0U,325U,836U,0U},
  {0U,326U,711U,0U},
  {0U,327U,784U,0U},
  {0U,328U,840U,0U},
  {0U,329U,879U,0U},
  {0U,330U,197U,0U},
  {0U,331U,69U,0U},
  {0U,332U,882U,0U},
  {0U,333U,212U,0U},
  {0U,334U,932U,0U},
  {0U,335U,618U,0U},
  {0U,336U,365U,0U},
  {0U,337U,976U,0U},
  {0U,338U,886U,0U},
  {0U,339U,497U,0U},
  {0U,340U,909U,0U},
  {0U,341U,757U,0U},
  {0U,342U,761U,0U},
  {0U,343U,962U,0U},
  {0U,344U,424U,0U},
  {0U,345U,846U,0U},
  {0U,346U,656U,0U},
  {0U,347U,483U,0U},
  {0U,348U,609U,0U},
  {0U,349U,852U,0U},
  {0U,350U,811U,0U},
  {0U,351U,486U,0U},
  {0U,352U,556U,0U},
  {0U,353U,804U,0U},
  {0U,354U,757U,0U},
  {0U,355U,675U,0U},
  {0U,356U,98U,0U},
  {0U,357U,435U,0U},
  {0U,358U,76U,0U},
  {0U,359U,362U,0U},
  {0U,360U,284U,0U},
  {0U,361U,761U,0U},
  {0U,362U,486U,0U},
  {0U,363U,390U,0U},
  {0U,364U,505U,0U},
  {0U,365U,591U,0U},
  {0U,366U,80U,0U},
  {0U,367U,230U,0U},
  {0U,368U,189U,0U},
  {0U,369U,942U,0U},
  {0U,370U,966U,0U},
  {0U,371U,457U,0U},
  {0U,372U,764U,0U},
  {0U,373U,829U,0U},
  {0U,374U,613U,0U},
  {0U,375U,130U,0U},
  {0U,376U,590U,0U},
  {0U,377U,413U,0U},
  {0U,378U,285U,0U},
  {0U,379U,776U,0U},
  {0U,380U,627U,0U},
  {0U,381U,977U,0U},
  {0U,382U,341U,0U},
  {0U,383U,844U,0U},
  {0U,384U,837U,0U},
  {0U,385U,736U,0U},
  {0U,386U,292U,0U},
  {0U,387U,455U,0U},
  {0U,388U,966U,0U},
  {0U,389U,629U,0U},
  {0U,390U,167U,0U},
  {0U,391U,36U,0U},
  {0U,392U,454U,0U},
  {0U,393U,653U,0U},
  {0U,394U,627U,0U},
  {0U,395U,594U,0U},
  {0U,396U,535U,0U},
  {0U,397U,874U,0U},
  {0U,398U,758U,0U},
  {0U,399U,268U,0U},
  {0U,400U,562U,0U},
  {0U,401U,772U,0U},
  {0U,402U,567U,0U},
  {0U,403U,694U,0U},
  {0U,404U,250U,0U},
  {0U,405U,205U,0U},
  {0U,406U,463U,0U},
  {0U,407U,72U,0U},
  {0U,408U,965U,0U},
  {0U,409U,307U,0U},
  {0U,410U,743U,0U},
  {0U,411U,666U,0U},
  {0U,412U,950U,0U},
  {0U,413U,999U,0U},
  {0U,414U,364U,0U},
  {0U,415U,462U,0U},
  {0U,416U,296U,0U},
  {0U,417U,849U,0U},
  {0U,418U,559U,0U},
  {0U,419U,393U,0U},
  {0U,420U,290U,0U},
  {0U,421U,360U,0U},
  {0U,422U,171U,0U},
  {0U,423U,618U,0U},
  {0U,424U,955U,0U},
  {0U,425U,971U,0U},
  {0U,426U,39U,0U},
  {0U,427U,68U,0U},
  {0U,428U,929U,0U},
  {0U,429U,148U,0U},
  {0U,430U,624U,0U},
  {0U,431U,620U,0U},
  {0U,432U,702U,0U},
  {0U,433U,591U,0U},
  {0U,434U,882U,0U},
  {0U,435U,858U,0U},
  {0U,436U,22U,0U},
  {0U,437U,453U,0U},
  {0U,438U,842U,0U},
  {0U,439U,982U,0U},
  {0U,440U,906U,0U},
  {0U,441U,393U,0U},
  {0U,442U,783U,0U},
  {0U,443U,597U,0U},
  {0U,444U,835U,0U},
  {0U,445U,452U,0U},
  {0U,446U,657U,0U},
  {0U,447U,465U,0U},
  {0U,448U,667U,0U},
  {0U,449U,370U,0U},
  {0U,450U,237U,0U},
  {0U,451U,58U,0U},
  {0U,452U,992U,0U},
  {0U,453U,522U,0U},
  {0U,454U,873U,0U},
  {0U,455U,44U,0U},
  {0U,456U,151U,0U},
  {0U,457U,589U,0U},
  {0U,458U,612U,0U},
  {0U,459U,44U,0U},
  {0U,460U,719U,0U},
  {0U,461U,264U,0U},
  {0U,462U,193U,0U},
  {0U,463U,959U,0U},
  {0U,464U,39U,0U},
  {0U,465U,380U,0U},
  {0U,466U,59U,0U},
  {0U,467U,819U,0U},
  {0U,468U,180U,0U},
  {0U,469U,744U,0U},
  {0U,470U,309U,0U},
  {0U,471U,619U,0U},
  {0U,472U,427U,0U},
  {0U,473U,814U,0U},
  {0U,474U,93U,0U},
  {0U,475U,461U,0U},
  {0U,476U,816U,0U},
  {0U,477U,261U,0U},
  {0U,478U,264U,0U},
  {0U,479U,245U,0U},
  {0U,480U,61U,0U},
  {0U,481U,911U,0U},
  {0U,482U,991U,0U},
  {0U,483U,699U,0U},
  {0U,484U,300U,0U},
  {0U,485U,303U,0U},
  {0U,486U,703U,0U},
  {0U,487U,651U,0U},
  {0U,488U,655U,0U},
  {0U,489U,653U,0U},
  {0U,490U,658U,0U},
  {0U,491U,377U,0U},
  {0U,492U,481U,0U},
  {0U,493U,970U,0U},
  {0U,494U,514U,0U},
  {0U,495U,784U,0U},
  {0U,496U,455U,0U},
  {0U,497U,698U,0U},
  {0U,498U,181U,0U},
  {0U,499U,129U,0U},
  {0U,500U,183U,0U},
  {0U,501U,349U,0U},
  {0U,502U,254U,0U},
  {0U,503U,904U,0U},
  {0U,504U,483U,0U},
  {0U,505U,324U,0U},
  {0U,506U,423U,0U},
  {0U,507U,585U,0U},
  {0U,508U,822U,0U},
  {0U,509U,442U,0U},
  {0U,510U,683U,0U},
  {0U,511U,277U,0U},
  {1U,174U,511U,0U},
  {1U,82U,510U,0U},
  {1U,242U,509U,0U},
  {1U,40U,508U,0U},
  {1U,33U,507U,0U},
  {1U,175U,506U,0U},
  {1U,113U,505U,0U},
  {1U,53U,504U,0U},
  {1U,251U,503U,0U},
  {1U,274U,502U,0U},
  {1U,301U,501U,0U},
  {1U,449U,500U,0U},
  {1U,344U,499U,0U},
  {1U,375U,498U,0U},
  {1U,367U,497U,0U},
  {1U,312U,496U,0U},
  {1U,59U,495U,0U},
  {1U,154U,494U,0U},
  {1U,438U,493U,0U},
  {1U,437U,492U,0U},
  {1U,195U,491U,0U},
  {1U,321U,490U,0U},
  {1U,114U,489U,0U},
  {1U,72U,488U,0U},
  {1U,56U,487U,0U},
  {1U,427U,486U,0U},
  {1U,219U,485U,0U},
  {1U,313U,484U,0U},
  {1U,344U,483U,0U},
  {1U,54U,482U,0U},
  {1U,87U,481U,0U},
  {1U,432U,480U,0U},
  {1U,425U,479U,0U},
  {1U,415U,478U,0U},
  {1U,256U,477U,0U},
  {1U,214U,476U,0U},
  {1U,216U,475U,0U},
  {1U,72U,474U,0U},
  {1U,370U,473U,0U},
  {1U,106U,472U,0U},
  {1U,44U,471U,0U},
  {1U,309U,470U,0U},
  {1U,362U,469U,0U},
  {1U,233U,468U,0U},
  {1U,438U,467U,0U},
  {1U,97U,466U,0U},
  {1U,220U,465U,0U},
  {1U,462U,464U,0U},
  {1U,387U,463U,0U},
  {1U,20U,462U,0U},
  {1U,311U,461U,0U},
  {1U,215U,460U,0U},
  {1U,136U,459U,0U},
  {1U,16U,458U,0U},
  {1U,204U,457U,0U},
  {1U,111U,456U,0U},
  {1U,215U,455U,0U},
  {1U,276U,454U,0U},
  {1U,171U,453U,0U},
  {1U,172U,452U,0U},
  {1U,48U,451U,0U},
  {1U,170U,450U,0U},
  {1U,194U,449U,0U},
  {1U,308U,448U,0U},
  {1U,185U,447U,0U},
  {1U,297U,446U,0U},
  {1U,315U,445U,0U},
  {1U,444U,444U,0U},
  {1U,171U,443U,0U},
  {1U,86U,442U,0U},
  {1U,167U,441U,0U},
  {1U,33U,440U,0U},
  {1U,424U,439U,0U},
  {1U,99U,438U,0U},
  {1U,317U,437U,0U},
  {1U,19U,436U,0U},
  {1U,297U,435U,0U},
  {1U,220U,434U,0U},
  {1U,360U,433U,0U},
  {1U,209U,432U,0U},
  {1U,264U,431U,0U},
  {1U,162U,430U,0U},
  {1U,82U,429U,0U},
  {1U,288U,428U,0U},
  {1U,330U,427U,0U},
  {1U,214U,426U,0U},
  {1U,233U,425U,0U},
  {1U,250U,424U,0U},
  {1U,230U,423U,0U},
  {1U,259U,422U,0U},
  {1U,356U,421U,0U},
  {1U,271U,420U,0U},
  {1U,277U,419U,0U},
  {1U,243U,418U,0U},
  {1U,234U,417U,0U},
  {1U,252U,416U,0U},
  {1U,200U,415U,0U},
  {1U,65U,414U,0U},
  {1U,191U,413U,0U},
  {1U,227U,412U,0U},
  {1U,241U,411U,0U},
  {1U,198U,410U,0U},
  {1U,336U,409U,0U},
  {1U,314U,408U,0U},
  {1U,169U,407U,0U},
  {1U,236U,406U,0U},
  {1U,378U,405U,0U},
  {1U,193U,404U,0U},
  {1U,381U,403U,0U},
  {1U,27U,402U,0U},
  {1U,277U,401U,0U},
  {1U,242U,400U,0U},
  {1U,206U,399U,0U},
  {1U,153U,398U,0U},
  {1U,117U,397U,0U},
  {1U,335U,396U,0U},
  {1U,270U,395U,0U},
  {1U,268U,394U,0U},
  {1U,387U,393U,0U},
  {1U,321U,392U,0U},
  {1U,112U,391U,0U},
  {1U,203U,390U,0U},
  {1U,60U,389U,0U},
  {1U,21U,388U,0U},
  {1U,77U,387U,0U},
  {1U,152U,386U,0U},
  {1U,158U,385U,0U},
  {1U,250U,384U,0U},
  {1U,101U,383U,0U},
  {1U,8U,382U,0U},
  {1U,331U,381U,0U},
  {1U,135U,380U,0U},
  {1U,201U,379U,0U},
  {1U,7U,378U,0U},
  {1U,264U,377U,0U},
  {1U,342U,376U,0U},
  {1U,20U,375U,0U},
  {1U,42U,374U,0U},
  {1U,312U,373U,0U},
  {1U,73U,372U,0U},
  {1U,178U,371U,0U},
  {1U,0U,370U,0U},
  {1U,335U,369U,0U},
  {1U,72U,368U,0U},
  {1U,205U,367U,0U},
  {1U,363U,366U,0U},
  {1U,159U,365U,0U},
  {1U,101U,364U,0U},
  {1U,226U,363U,0U},
  {1U,245U,362U,0U},
  {1U,174U,361U,0U},
  {1U,130U,360U,0U},
  {1U,153U,359U,0U},
  {1U,73U,358U,0U},
  {1U,289U,357U,0U},
  {1U,142U,356U,0U},
  {1U,162U,355U,0U},
  {1U,179U,354U,0U},
  {1U,233U,353U,0U},
  {1U,35U,352U,0U},
  {1U,162U,351U,0U},
  {1U,326U,350U,0U},
  {1U,315U,349U,0U},
  {1U,25U,348U,0U},
  {1U,29U,347U,0U},
  {1U,169U,346U,0U},
  {1U,215U,345U,0U},
  {1U,189U,344U,0U},
  {1U,142U,343U,0U},
  {1U,164U,342U,0U},
  {1U,253U,341U,0U},
  {1U,40U,340U,0U},
  {1U,309U,339U,0U},
  {1U,113U,338U,0U},
  {1U,99U,337U,0U},
  {1U,299U,336U,0U},
  {1U,90U,335U,0U},
  {1U,236U,334U,0U},
  {1U,240U,333U,0U},
  {1U,175U,332U,0U},
  {1U,55U,331U,0U},
  {1U,153U,330U,0U},
  {1U,151U,329U,0U},
  {1U,97U,328U,0U},
  {1U,93U,327U,0U},
  {1U,289U,326U,0U},
  {1U,157U,325U,0U},
  {1U,122U,324U,0U},
  {1U,111U,323U,0U},
  {1U,270U,322U,0U},
  {1U,239U,321U,0U},
  {1U,134U,320U,0U},
  {1U,171U,319U,0U},
  {1U,257U,318U,0U},
  {1U,103U,317U,0U},
  {1U,233U,316U,0U},
  {1U,281U,315U,0U},
  {1U,292U,314U,0U},
  {1U,162U,313U,0U},
  {1U,216U,312U,0U},
  {1U,106U,311U,0U},
  {1U,147U,310U,0U},
  {1U,98U,309U,0U},
  {1U,157U,308U,0U},
  {1U,59U,307U,0U},
  {1U,27U,306U,0U},
  {1U,101U,305U,0U},
  {1U,14U,304U,0U},
  {1U,56U,303U,0U},
  {1U,209U,302U,0U},
  {1U,14U,301U,0U},
  {1U,169U,300U,0U},
  {1U,204U,299U,0U},
  {1U,209U,298U,0U},
  {1U,142U,297U,0U},
  {1U,183U,296U,0U},
  {1U,291U,295U,0U},
  {1U,75U,294U,0U},
  {1U,206U,293U,0U},
  {1U,206U,292U,0U},
  {1U,82U,291U,0U},
  {1U,15U,290U,0U},
  {1U,171U,289U,0U},
  {1U,259U,288U,0U},
  {1U,168U,287U,0U},
  {1U,148U,286U,0U},
  {1U,58U,285U,0U},
  {1U,115U,284U,0U},
  {1U,209U,283U,0U},
  {1U,4U,282U,0U},
  {1U,168U,281U,0U},
  {1U,183U,280U,0U},
  {1U,279U,279U,0U},
  {1U,96U,278U,0U},
  {1U,208U,277U,0U},
  {1U,55U,276U,0U},
  {1U,261U,275U,0U},
  {1U,222U,274U,0U},
  {1U,41U,273U,0U},
  {1U,194U,272U,0U},
  {1U,208U,271U,0U},
  {1U,174U,270U,0U},
  {1U,37U,269U,0U},
  {1U,102U,268U,0U},
  {1U,92U,267U,0U},
  {1U,18U,266U,0U},
  {1U,172U,265U,0U},
  {1U,85U,264U,0U},
  {1U,256U,263U,0U},
  {1U,190U,262U,0U},
  {1U,121U,261U,0U},
  {1U,89U,260U,0U},
  {1U,143U,259U,0U},
  {1U,256U,258U,0U},
  {1U,82U,257U,0U},
  {1U,239U,256U,0U},
  {1U,22U,255U,0U},
  {1U,27U,254U,0U},
  {1U,25U,253U,0U},
  {1U,83U,252U,0U},
  {1U,41U,251U,0U},
  {1U,125U,250U,0U},
  {1U,0U,249U,0U},
  {1U,36U,248U,0U},
  {1U,217U,247U,0U},
  {1U,208U,246U,0U},
  {1U,62U,245U,0U},
  {1U,161U,244U,0U},
  {1U,58U,243U,0U},
  {1U,51U,242U,0U},
  {1U,236U,241U,0U},
  {1U,50U,240U,0U},
  {1U,102U,239U,0U},
  {1U,1U,238U,0U},
  {1U,89U,237U,0U},
  {1U,68U,236U,0U},
  {1U,128U,235U,0U},
  {1U,122U,234U,0U},
  {1U,133U,233U,0U},
  {1U,211U,232U,0U},
  {1U,207U,231U,0U},
  {1U,82U,230U,0U},
  {1U,183U,229U,0U},
  {1U,16U,228U,0U},
  {1U,151U,227U,0U},
  {1U,137U,226U,0U},
  {1U,66U,225U,0U},
  {1U,70U,224U,0U},
  {1U,133U,223U,0U},
  {1U,129U,222U,0U},
  {1U,131U,221U,0U},
  {1U,50U,220U,0U},
  {1U,212U,219U,0U},
  {1U,112U,218U,0U},
  {1U,140U,217U,0U},
  {1U,189U,216U,0U},
  {1U,24U,215U,0U},
  {1U,41U,214U,0U},
  {1U,203U,213U,0U},
  {1U,168U,212U,0U},
  {1U,35U,211U,0U},
  {1U,74U,210U,0U},
  {1U,158U,209U,0U},
  {1U,128U,208U,0U},
  {1U,46U,207U,0U},
  {1U,95U,206U,0U},
  {1U,142U,205U,0U},
  {1U,4U,204U,0U},
  {1U,190U,203U,0U},
  {1U,189U,202U,0U},
  {1U,131U,201U,0U},
  {1U,48U,200U,0U},
  {1U,26U,199U,0U},
  {1U,167U,198U,0U},
  {1U,173U,197U,0U},
  {1U,72U,196U,0U},
  {1U,143U,195U,0U},
  {1U,43U,194U,0U},
  {1U,47U,193U,0U},
  {1U,117U,192U,0U},
  {1U,33U,191U,0U},
  {1U,8U,190U,0U},
  {1U,146U,189U,0U},
  {1U,141U,188U,0U},
  {1U,141U,187U,0U},
  {1U,178U,186U,0U},
  {1U,13U,185U,0U},
  {1U,77U,184U,0U},
  {1U,79U,183U,0U},
  {1U,115U,182U,0U},
  {1U,14U,181U,0U},
  {1U,7U,180U,0U},
  {1U,90U,179U,0U},
  {1U,16U,178U,0U},
  {1U,102U,177U,0U},
  {1U,48U,176U,0U},
  {1U,133U,175U,0U},
  {1U,173U,174U,0U},
  {1U,50U,173U,0U},
  {1U,2U,172U,0U},
  {1U,129U,171U,0U},
  {1U,13U,170U,0U},
  {1U,8U,169U,0U},
  {1U,152U,168U,0U},
  {1U,147U,167U,0U},
  {1U,138U,166U,0U},
  {1U,41U,165U,0U},
  {1U,37U,164U,0U},
  {1U,46U,163U,0U},
  {1U,146U,162U,0U},
  {1U,7U,161U,0U},
  {1U,55U,160U,0U},
  {1U,155U,159U,0U},
  {1U,11U,158U,0U},
  {1U,126U,157U,0U},
  {1U,117U,156U,0U},
  {1U,141U,155U,0U},
  {1U,81U,154U,0U},
  {1U,151U,153U,0U},
  {1U,76U,152U,0U},
  {1U,117U,151U,0U},
  {1U,35U,150U,0U},
  {1U,19U,149U,0U},
  {1U,40U,148U,0U},
  {1U,8U,147U,0U},
  {1U,121U,146U,0U},
  {1U,56U,145U,0U},
  {1U,88U,144U,0U},
  {1U,18U,143U,0U},
  {1U,32U,142U,0U},
  {1U,23U,141U,0U},
  {1U,5U,140U,0U},
  {1U,136U,139U,0U},
  {1U,14U,138U,0U},
  {1U,49U,137U,0U},
  {1U,3U,136U,0U},
  {1U,51U,135U,0U},
  {1U,25U,134U,0U},
  {1U,79U,133U,0U},
  {1U,128U,132U,0U},
  {1U,67U,131U,0U},
  {1U,10U,130U,0U},
  {1U,47U,129U,0U},
  {1U,96U,128U,0U},
  {1U,9U,127U,0U},
  {1U,60U,126U,0U},
  {1U,4U,125U,0U},
  {1U,79U,124U,0U},
  {1U,112U,123U,0U},
  {1U,104U,122U,0U},
  {1U,106U,121U,0U},
  {1U,100U,120U,0U},
  {1U,102U,119U,0U},
  {1U,4U,118U,0U},
  {1U,115U,117U,0U},
  {1U,70U,116U,0U},
  {1U,27U,115U,0U},
  {1U,112U,114U,0U},
  {1U,80U,113U,0U},
  {1U,37U,112U,0U},
  {1U,33U,111U,0U},
  {1U,82U,110U,0U},
  {1U,49U,109U,0U},
  {1U,77U,108U,0U},
  {1U,2U,107U,0U},
  {1U,55U,106U,0U},
  {1U,29U,105U,0U},
  {1U,89U,104U,0U},
  {1U,40U,103U,0U},
  {1U,44U,102U,0U},
  {1U,18U,101U,0U},
  {1U,59U,100U,0U},
  {1U,37U,99U,0U},
  {1U,21U,98U,0U},
  {1U,80U,97U,0U},
  {1U,43U,96U,0U},
  {1U,14U,95U,0U},
  {1U,0U,94U,0U},
  {1U,67U,93U,0U},
  {1U,89U,92U,0U},
  {1U,78U,91U,0U},
  {1U,48U,90U,0U},
  {1U,18U,89U,0U},
  {1U,71U,88U,0U},
  {1U,33U,87U,0U},
  {1U,7U,86U,0U},
  {1U,49U,85U,0U},
  {1U,57U,84U,0U},
  {1U,67U,83U,0U},
  {1U,51U,82U,0U},
  {1U,45U,81U,0U},
  {1U,24U,80U,0U},
  {1U,51U,79U,0U},
  {1U,47U,78U,0U},
  {1U,16U,77U,0U},
  {1U,33U,76U,0U},
  {1U,47U,75U,0U},
  {1U,16U,74U,0U},
  {1U,0U,73U,0U},
  {1U,8U,72U,0U},
  {1U,68U,71U,0U},
  {1U,62U,70U,0U},
  {1U,50U,69U,0U},
  {1U,18U,68U,0U},
  {1U,46U,67U,0U},
  {1U,40U,66U,0U},
  {1U,45U,65U,0U},
  {1U,3U,64U,0U},
  {1U,50U,63U,0U},
  {1U,36U,62U,0U},
  {1U,9U,61U,0U},
  {1U,42U,60U,0U},
  {1U,27U,59U,0U},
  {1U,48U,58U,0U},
  {1U,6U,57U,0U},
  {1U,22U,56U,0U},
  {1U,22U,55U,0U},
  {1U,24U,54U,0U},
  {1U,31U,53U,0U},
  {1U,10U,52U,0U},
  {1U,13U,51U,0U},
  {1U,28U,50U,0U},
  {1U,32U,49U,0U},
  {1U,9U,48U,0U},
  {1U,29U,47U,0U},
  {1U,5U,46U,0U},
  {1U,12U,45U,0U},
  {1U,6U,44U,0U},
  {1U,40U,43U,0U},
  {1U,16U,42U,0U},
  {1U,32U,41U,0U},
  {1U,27U,40U,0U},
  {1U,32U,39U,0U},
  {1U,9U,38U,0U},
  {1U,36U,37U,0U},
  {1U,29U,36U,0U},
  {1U,14U,35U,0U},
  {1U,13U,34U,0U},
  {1U,33U,33U,0U},
  {1U,30U,32U,0U},
  {1U,4U,31U,0U},
  {1U,6U,30U,0U},
  {1U,9U,29U,0U},
  {1U,15U,28U,0U},
  {1U,13U,27U,0U},
  {1U,11U,26U,0U},
  {1U,16U,25U,0U},
  {1U,9U,24U,0U},
  {1U,10U,23U,0U},
  {1U,8U,22U,0U},
  {1U,15U,21U,0U},
  {1U,6U,20U,0U},
  {1U,13U,19U,0U},
  {1U,1U,18U,0U},
  {1U,0U,17U,0U},
  {1U,5U,16U,0U},
  {1U,14U,15U,0U},
  {1U,6U,14U,0U},
  {1U,0U,13U,0U},
  {1U,8U,12U,0U},
  {1U,9U,11U,0U},
  {1U,1U,10U,0U},
  {1U,5U,9U,0U},
  {1U,0U,8U,0U},
  {1U,1U,7U,0U},
  {1U,3U,6U,0U},
  {1U,1U,5U,0U},
  {1U,0U,4U,0U},
  {1U,3U,3U,0U},
  {1U,1U,2U,0U},
  {1U,0U,1U,0U},
  {1U,0U,0U,0U},
  {2U,188U,239U,1025U},
  {2U,420U,209U,1026U},
  {2U,415U,337U,1027U},
  {2U,503U,919U,1028U},
  {2U,260U,478U,1029U},
  {2U,240U,890U,1030U},
  {2U,468U,177U,1031U},
  {2U,462U,964U,1032U},
  {2U,292U,99U,1033U},
  {2U,329U,534U,1034U},
  {2U,129U,687U,1035U},
  {2U,386U,628U,1036U},
  {2U,72U,999U,1037U},
  {2U,71U,194U,1038U},
  {2U,261U,413U,1039U},
  {2U,50U,49U,1040U},
  {2U,272U,506U,1041U},
  {2U,178U,338U,1042U},
  {2U,55U,737U,1043U},
  {2U,316U,999U,1044U},
  {2U,380U,653U,1045U},
  {2U,494U,147U,1046U},
  {2U,22U,589U,1047U},
  {2U,277U,761U,1048U},
  {2U,242U,777U,1049U},
  {2U,277U,210U,1050U},
  {2U,191U,291U,1051U},
  {2U,117U,975U,1052U},
  {2U,262U,361U,1053U},
  {2U,219U,191U,1054U},
  {2U,398U,580U,1055U},
  {2U,269U,731U,1056U},
  {2U,289U,307U,1057U},
  {2U,110U,646U,1058U},
  {2U,51U,903U,1059U},
  {2U,435U,913U,1060U},
  {2U,499U,436U,1061U},
  {2U,484U,994U,1062U},
  {2U,375U,666U,1063U},
  {2U,15U,921U,1064U},
  {2U,429U,322U,1065U},
  {2U,481U,622U,1066U},
  {2U,9U,437U,1067U},
  {2U,251U,433U,1068U},
  {2U,432U,830U,1069U},
  {2U,344U,238U,1070U},
  {2U,420U,521U,1071U},
  {2U,259U,943U,1072U},
  {2U,112U,479U,1073U},
  {2U,146U,795U,1074U},
  {2U,277U,326U,1075U},
  {2U,449U,522U,1076U},
  {2U,38U,195U,1077U},
  {2U,440U,298U,1078U},
  {2U,102U,85U,1079U},
  {2U,268U,557U,1080U},
  {2U,191U,620U,1081U},
  {2U,38U,454U,1082U},
  {2U,435U,424U,1083U},
  {2U,248U,619U,1084U},
  {2U,410U,657U,1085U},
  {2U,495U,846U,1086U},
  {2U,16U,597U,1087U},
  {2U,113U,727U,1088U},
  {2U,180U,374U,1089U},
  {2U,256U,116U,1090U},
  {2U,165U,85U,1091U},
  {2U,62U,794U,1092U},
  {2U,494U,355U,1093U},
  {2U,303U,280U,1094U},
  {2U,155U,243U,1095U},
  {2U,59U,510U,1096U},
  {2U,73U,969U,1097U},
  {2U,34U,60U,1098U},
  {2U,480U,173U,1099U},
  {2U,291U,289U,1100U},
  {2U,480U,191U,1101U},
  {2U,432U,338U,1102U},
  {2U,396U,406U,1103U},
  {2U,359U,940U,1104U},
  {2U,21U,769U,1105U},
  {2U,80U,959U,1106U},
  {2U,196U,805U,1107U},
  {2U,204U,539U,1108U},
  {2U,10U,788U,1109U},
  {2U,295U,39U,1110U},
  {2U,1U,315U,1111U},
  {2U,187U,644U,1112U},
  {2U,69U,330U,1113U},
  {2U,99U,783U,1114U},
  {2U,405U,245U,1115U},
  {2U,434U,356U,1116U},
  {2U,333U,585U,1117U},
  {2U,113U,975U,1118U},
  {2U,129U,292U,1119U},
  {2U,149U,228U,1120U},
  {2U,11U,708U,1121U},
  {2U,361U,314U,1122U},
  {2U,157U,790U,1123U},
  {2U,251U,482U,1124U},
  {2U,393U,832U,1125U},
  {2U,343U,886U,1126U},
  {2U,490U,165U,1127U},
  {2U,51U,128U,1128U},
  {2U,377U,121U,1129U},
  {2U,460U,389U,1130U},
  {2U,288U,527U,1131U},
  {2U,38U,908U,1132U},
  {2U,76U,742U,1133U},
  {2U,435U,376U,1134U},
  {2U,36U,867U,1135U},
  {2U,302U,64U,1136U},
  {2U,491U,833U,1137U},
  {2U,504U,880U,1138U},
  {2U,301U,344U,1139U},
  {2U,128U,431U,1140U},
  {2U,21U,414U,1141U},
  {2U,38U,727U,1142U},
  {2U,280U,301U,1143U},
  {2U,495U,159U,1144U},
  {2U,260U,297U,1145U},
  {2U,382U,90U,1146U},
  {2U,430U,730U,1147U},
  {2U,488U,927U,1148U},
  {2U,27U,120U,1149U},
  {2U,357U,881U,1150U},
  {2U,458U,832U,1151U},
  {2U,110U,477U,1152U},
  {2U,433U,845U,1153U},
  {2U,166U,551U,1154U},
  {2U,419U,704U,1155U},
  {2U,46U,692U,1156U},
  {2U,137U,463U,1157U},
  {2U,312U,150U,1158U},
  {2U,188U,802U,1159U},
  {2U,466U,995U,1160U},
  {2U,281U,563U,1161U},
  {2U,126U,422U,1162U},
  {2U,28U,36U,1163U},
  {2U,378U,454U,1164U},
  {2U,110U,571U,1165U},
  {2U,234U,804U,1166U},
  {2U,96U,55U,1167U},
  {2U,276U,266U,1168U},
  {2U,75U,636U,1169U},
  {2U,306U,876U,1170U},
  {2U,373U,942U,1171U},
  {2U,402U,649U,1172U},
  {2U,327U,251U,1173U},
  {2U,363U,198U,1174U},
  {2U,376U,506U,1175U},
  {2U,76U,794U,1176U},
  {2U,328U,691U,1177U},
  {2U,306U,813U,1178U},
  {2U,36U,507U,1179U},
  {2U,397U,926U,1180U},
  {2U,42U,28U,1181U},
  {2U,387U,266U,1182U},
  {2U,401U,845U,1183U},
  {2U,409U,151U,1184U},
  {2U,148U,16U,1185U},
  {2U,382U,135U,1186U},
  {2U,70U,90U,1187U},
  {2U,47U,327U,1188U},
  {2U,227U,517U,1189U},
  {2U,200U,632U,1190U},
  {2U,81U,63U,1191U},
  {2U,456U,443U,1192U},
  {2U,297U,990U,1193U},
  {2U,487U,762U,1194U},
  {2U,248U,217U,1195U},
  {2U,30U,453U,1196U},
  {2U,312U,473U,1197U},
  {2U,192U,32U,1198U},
  {2U,373U,418U,1199U},
  {2U,501U,724U,1200U},
  {2U,424U,635U,1201U},
  {2U,146U,760U,1202U},
  {2U,137U,519U,1203U},
  {2U,160U,251U,1204U},
  {2U,376U,875U,1205U},
  {2U,18U,710U,1206U},
  {2U,118U,493U,1207U},
  {2U,42U,120U,1208U},
  {2U,112U,261U,1209U},
  {2U,421U,300U,1210U},
  {2U,312U,638U,1211U},
  {2U,459U,462U,1212U},
  {2U,156U,947U,1213U},
  {2U,168U,303U,1214U},
  {2U,495U,600U,1215U},
  {2U,0U,752U,1216U},
  {2U,481U,453U,1217U},
  {2U,495U,276U,1218U},
  {2U,152U,903U,1219U},
  {2U,284U,224U,1220U},
  {2U,39U,273U,1221U},
  {2U,120U,135U,1222U},
  {2U,33U,516U,1223U},
  {2U,24U,124U,1224U},
  {2U,299U,992U,1225U},
  {2U,166U,152U,1226U},
  {2U,481U,307U,1227U},
  {2U,460U,840U,1228U},
  {2U,217U,360U,1229U},
  {2U,457U,599U,1230U},
  {2U,17U,475U,1231U},
  {2U,229U,798U,1232U},
  {2U,54U,25U,1233U},
  {2U,126U,342U,1234U},
  {2U,23U,977U,1235U},
  {2U,155U,946U,1236U},
  {2U,202U,944U,1237U},
  {2U,294U,890U,1238U},
  {2U,12U,240U,1239U},
  {2U,481U,528U,1240U},
  {2U,191U,116U,1241U},
  {2U,405U,514U,1242U},
  {2U,283U,221U,1243U},
  {2U,405U,587U,1244U},
  {2U,435U,587U,1245U},
  {2U,192U,222U,1246U},
  {2U,260U,214U,1247U},
  {2U,16U,598U,1248U},
  {2U,126U,541U,1249U},
  {2U,59U,343U,1250U},
  {2U,338U,542U,1251U},
  {2U,132U,105U,1252U},
  {2U,266U,594U,1253U},
  {2U,339U,697U,1254U},
  {2U,454U,599U,1255U},
  {2U,459U,132U,1256U},
  {2U,127U,281U,1257U},
  {2U,345U,800U,1258U},
  {2U,282U,81U,1259U},
  {2U,434U,379U,1260U},
  {2U,483U,387U,1261U},
  {2U,431U,786U,1262U},
  {2U,352U,560U,1263U},
  {2U,99U,916U,1264U},
  {2U,147U,951U,1265U},
  {2U,220U,246U,1266U},
  {2U,165U,74U,1267U},
  {2U,223U,668U,1268U},
  {2U,194U,939U,1269U},
  {2U,505U,697U,1270U},
  {2U,275U,217U,1271U},
  {2U,409U,919U,1272U},
  {2U,146U,183U,1273U},
  {2U,166U,552U,1274U},
  {2U,422U,481U,1275U},
  {2U,150U,129U,1276U},
  {2U,118U,677U,1277U},
  {2U,322U,964U,1278U},
  {2U,292U,13U,1279U},
  {2U,405U,936U,1280U},
  {2U,177U,683U,1281U},
  {2U,189U,867U,1282U},
  {2U,93U,980U,1283U},
  {2U,382U,822U,1284U},
  {2U,189U,673U,1285U},
  {2U,368U,541U,1286U},
  {2U,125U,92U,1287U},
  {2U,410U,630U,1288U},
  {2U,420U,106U,1289U},
  {2U,216U,180U,1290U},
  {2U,309U,94U,1291U},
  {2U,173U,141U,1292U},
  {2U,503U,49U,1293U},
  {2U,457U,813U,1294U},
  {2U,242U,37U,1295U},
  {2U,50U,997U,1296U},
  {2U,120U,105U,1297U},
  {2U,340U,707U,1298U},
  {2U,83U,900U,1299U},
  {2U,136U,166U,1300U},
  {2U,164U,267U,1301U},
  {2U,193U,545U,1302U},
  {2U,371U,834U,1303U},
  {2U,206U,979U,1304U},
  {2U,470U,276U,1305U},
  {2U,24U,644U,1306U},
  {2U,24U,641U,1307U},
  {2U,381U,380U,1308U},
  {2U,88U,145U,1309U},
  {2U,336U,968U,1310U},
  {2U,277U,83U,1311U},
  {2U,7U,704U,1312U},
  {2U,353U,386U,1313U},
  {2U,449U,981U,1314U},
  {2U,218U,699U,1315U},
  {2U,118U,308U,1316U},
  {2U,203U,97U,1317U},
  {2U,270U,928U,1318U},
  {2U,266U,319U,1319U},
  {2U,39U,48U,1320U},
  {2U,309U,464U,1321U},
  {2U,451U,631U,1322U},
  {2U,222U,138U,1323U},
  {2U,81U,78U,1324U},
  {2U,478U,720U,1325U},
  {2U,211U,487U,1326U},
  {2U,249U,704U,1327U},
  {2U,307U,165U,1328U},
  {2U,78U,988U,1329U},
  {2U,453U,423U,1330U},
  {2U,164U,939U,1331U},
  {2U,431U,382U,1332U},
  {2U,210U,19U,1333U},
  {2U,407U,285U,1334U},
  {2U,61U,650U,1335U},
  {2U,343U,409U,1336U},
  {2U,169U,361U,1337U},
  {2U,410U,445U,1338U},
  {2U,333U,677U,1339U},
  {2U,321U,744U,1340U},
  {2U,102U,601U,1341U},
  {2U,295U,571U,1342U},
  {2U,117U,838U,1343U},
  {2U,311U,166U,1344U},
  {2U,378U,443U,1345U},
  {2U,491U,783U,1346U},
  {2U,206U,929U,1347U},
  {2U,234U,528U,1348U},
  {2U,191U,553U,1349U},
  {2U,369U,245U,1350U},
  {2U,236U,204U,1351U},
  {2U,262U,943U,1352U},
  {2U,386U,731U,1353U},
  {2U,475U,73U,1354U},
  {2U,343U,790U,1355U},
  {2U,420U,276U,1356U},
  {2U,39U,995U,1357U},
  {2U,321U,54U,1358U},
  {2U,69U,934U,1359U},
  {2U,39U,615U,1360U},
  {2U,320U,639U,1361U},
  {2U,432U,923U,1362U},
  {2U,439U,289U,1363U},
  {2U,490U,386U,1364U},
  {2U,281U,929U,1365U},
  {2U,350U,567U,1366U},
  {2U,123U,787U,1367U},
  {2U,226U,286U,1368U},
  {2U,499U,298U,1369U},
  {2U,484U,606U,1370U},
  {2U,351U,533U,1371U},
  {2U,5U,258U,1372U},
  {2U,316U,56U,1373U},
  {2U,29U,270U,1374U},
  {2U,312U,169U,1375U},
  {2U,171U,196U,1376U},
  {2U,459U,968U,1377U},
  {2U,156U,682U,1378U},
  {2U,46U,321U,1379U},
  {2U,507U,949U,1380U},
  {2U,22U,844U,1381U},
  {2U,148U,294U,1382U},
  {2U,55U,492U,1383U},
  {2U,108U,719U,1384U},
  {2U,113U,42U,1385U},
  {2U,356U,810U,1386U},
  {2U,460U,442U,1387U},
  {2U,213U,385U,1388U},
  {2U,356U,699U,1389U},
  {2U,357U,356U,1390U},
  {2U,206U,298U,1391U},
  {2U,510U,235U,1392U},
  {2U,252U,828U,1393U},
  {2U,448U,925U,1394U},
  {2U,267U,880U,1395U},
  {2U,389U,530U,1396U},
  {2U,471U,96U,1397U},
  {2U,179U,219U,1398U},
  {2U,342U,416U,1399U},
  {2U,62U,392U,1400U},
  {2U,24U,63U,1401U},
  {2U,93U,861U,1402U},
  {2U,73U,8U,1403U},
  {2U,134U,627U,1404U},
  {2U,348U,690U,1405U},
  {2U,258U,744U,1406U},
  {2U,379U,469U,1407U},
  {2U,65U,328U,1408U},
  {2U,60U,736U,1409U},
  {2U,427U,770U,1410U},
  {2U,328U,912U,1411U},
  {2U,248U,653U,1412U},
  {2U,462U,545U,1413U},
  {2U,91U,638U,1414U},
  {2U,472U,135U,1415U},
  {2U,392U,674U,1416U},
  {2U,90U,646U,1417U},
  {2U,9U,676U,1418U},
  {2U,458U,329U,1419U},
  {2U,469U,147U,1420U},
  {2U,182U,470U,1421U},
  {2U,28U,330U,1422U},
  {2U,84U,645U,1423U},
  {2U,329U,601U,1424U},
  {2U,243U,781U,1425U},
  {2U,497U,760U,1426U},
  {2U,121U,167U,1427U},
  {2U,205U,36U,1428U},
  {2U,456U,100U,1429U},
  {2U,91U,559U,1430U},
  {2U,57U,869U,1431U},
  {2U,475U,821U,1432U},
  {2U,142U,358U,1433U},
  {2U,403U,299U,1434U},
  {2U,388U,548U,1435U},
  {2U,158U,248U,1436U},
  {2U,320U,810U,1437U},
  {2U,69U,403U,1438U},
  {2U,152U,763U,1439U},
  {2U,62U,72U,1440U},
  {2U,42U,602U,1441U},
  {2U,25U,485U,1442U},
  {2U,283U,456U,1443U},
  {2U,209U,682U,1444U},
  {2U,336U,999U,1445U},
  {2U,104U,600U,1446U},
  {2U,319U,294U,1447U},
  {2U,296U,965U,1448U},
  {2U,408U,169U,1449U},
  {2U,368U,422U,1450U},
  {2U,174U,385U,1451U},
  {2U,281U,767U,1452U},
  {2U,258U,671U,1453U},
  {2U,334U,232U,1454U},
  {2U,418U,946U,1455U},
  {2U,459U,547U,1456U},
  {2U,12U,161U,1457U},
  {2U,104U,179U,1458U},
  {2U,453U,120U,1459U},
  {2U,53U,944U,1460U},
  {2U,436U,710U,1461U},
  {2U,24U,839U,1462U},
  {2U,486U,993U,1463U},
  {2U,385U,579U,1464U},
  {2U,448U,309U,1465U},
  {2U,209U,968U,1466U},
  {2U,202U,496U,1467U},
  {2U,396U,890U,1468U},
  {2U,4U,990U,1469U},
  {2U,389U,582U,1470U},
  {2U,463U,204U,1471U},
  {2U,36U,174U,1472U},
  {2U,196U,77U,1473U},
  {2U,392U,221U,1474U},
  {2U,192U,705U,1475U},
  {2U,337U,653U,1476U},
  {2U,474U,378U,1477U},
  {2U,218U,291U,1478U},
  {2U,116U,148U,1479U},
  {2U,450U,597U,1480U},
  {2U,336U,174U,1481U},
  {2U,2U,249U,1482U},
  {2U,91U,554U,1483U},
  {2U,61U,872U,1484U},
  {2U,141U,114U,1485U},
  {2U,195U,789U,1486U},
  {2U,192U,116U,1487U},
  {2U,208U,938U,1488U},
  {2U,28U,804U,1489U},
  {2U,472U,107U,1490U},
  {2U,362U,962U,1491U},
  {2U,186U,997U,1492U},
  {2U,453U,938U,1493U},
  {2U,257U,631U,1494U},
  {2U,236U,283U,1495U},
  {2U,341U,781U,1496U},
  {2U,314U,389U,1497U},
  {2U,80U,656U,1498U},
  {2U,350U,369U,1499U},
  {2U,414U,440U,1500U},
  {2U,351U,859U,1501U},
  {2U,438U,530U,1502U},
  {2U,238U,963U,1503U},
  {2U,226U,170U,1504U},
  {2U,432U,683U,1505U},
  {2U,337U,435U,1506U},
  {2U,295U,915U,1507U},
  {2U,389U,478U,1508U},
  {2U,454U,449U,1509U},
  {2U,322U,552U,1510U},
  {2U,91U,934U,1511U},
  {2U,2U,960U,1512U},
  {2U,369U,627U,1513U},
  {2U,89U,378U,1514U},
  {2U,209U,56U,1515U},
  {2U,48U,643U,1516U},
  {2U,158U,778U,1517U},
  {2U,428U,679U,1518U},
  {2U,130U,923U,1519U},
  {2U,68U,929U,1520U},
  {2U,442U,805U,1521U},
  {2U,33U,961U,1522U},
  {2U,475U,867U,1523U},
  {2U,263U,32U,1524U},
  {2U,508U,183U,1525U},
  {2U,247U,595U,1526U},
  {2U,374U,232U,1527U},
  {2U,159U,747U,1528U},
  {2U,310U,807U,1529U},
  {2U,52U,495U,1530U},
  {2U,218U,136U,1531U},
  {2U,211U,386U,1532U},
  {2U,335U,921U,1533U},
  {2U,255U,776U,1534U},
  {2U,463U,62U,1535U},
  {2U,16U,291U,1536U},
  {2U,294U,135U,1537U},
  {2U,379U,358U,1538U},
  {2U,361U,281U,1539U},
  {2U,133U,515U,1540U},
  {2U,261U,84U,1541U},
  {2U,207U,877U,1542U},
  {2U,385U,983U,1543U},
  {2U,99U,310U,1544U},
  {2U,258U,478U,1545U},
  {2U,20U,174U,1546U},
  {2U,400U,947U,1547U},
  {2U,220U,143U,1548U},
  {2U,355U,624U,1549U},
  {2U,291U,196U,1550U},
  {2U,176U,432U,1551U},
  {2U,161U,583U,1552U},
  {2U,387U,85U,1553U},
  {2U,258U,351U,1554U},
  {2U,32U,343U,1555U},
  {2U,105U,925U,1556U},
  {2U,479U,135U,1557U},
  {2U,164U,863U,1558U},
  {2U,411U,341U,1559U},
  {2U,321U,510U,1560U},
  {2U,318U,646U,1561U},
  {2U,290U,810U,1562U},
  {2U,148U,616U,1563U},
  {2U,401U,187U,1564U},
  {2U,222U,843U,1565U},
  {2U,342U,18U,1566U},
  {2U,247U,929U,1567U},
  {2U,167U,236U,1568U},
  {2U,24U,332U,1569U},
  {2U,86U,863U,1570U},
  {2U,200U,610U,1571U},
  {2U,185U,90U,1572U},
  {2U,109U,718U,1573U},
  {2U,303U,993U,1574U},
  {2U,245U,761U,1575U},
  {2U,181U,22U,1576U},
  {2U,30U,106U,1577U},
  {2U,2U,480U,1578U},
  {2U,125U,680U,1579U},
  {2U,309U,966U,1580U},
  {2U,64U,204U,1581U},
  {2U,165U,465U,1582U},
  {2U,170U,762U,1583U},
  {2U,367U,859U,1584U},
  {2U,127U,124U,1585U},
  {2U,455U,903U,1586U},
  {2U,236U,617U,1587U},
  {2U,306U,546U,1588U},
  {2U,15U,57U,1589U},
  {2U,371U,289U,1590U},
  {2U,166U,196U,1591U},
  {2U,252U,49U,1592U},
  {2U,247U,31U,1593U},
  {2U,165U,268U,1594U},
  {2U,468U,475U,1595U},
  {2U,398U,306U,1596U},
  {2U,281U,908U,1597U},
  {2U,146U,496U,1598U},
  {2U,338U,542U,1599U},
  {2U,409U,480U,1600U},
  {2U,462U,395U,1601U},
  {2U,341U,977U,1602U},
  {2U,17U,752U,1603U},
  {2U,411U,217U,1604U},
  {2U,227U,138U,1605U},
  {2U,362U,809U,1606U},
  {2U,184U,75U,1607U},
  {2U,141U,831U,1608U},
  {2U,244U,849U,1609U},
  {2U,204U,528U,1610U},
  {2U,375U,966U,1611U},
  {2U,442U,737U,1612U},
  {2U,299U,998U,1613U},
  {2U,369U,531U,1614U},
  {2U,41U,398U,1615U},
  {2U,370U,613U,1616U},
  {2U,164U,887U,1617U},
  {2U,75U,369U,1618U},
  {2U,336U,101U,1619U},
  {2U,117U,991U,1620U},
  {2U,168U,102U,1621U},
  {2U,140U,432U,1622U},
  {2U,356U,942U,1623U},
  {2U,53U,117U,1624U},
  {2U,322U,526U,1625U},
  {2U,456U,643U,1626U},
  {2U,155U,252U,1627U},
  {2U,248U,528U,1628U},
  {2U,310U,308U,1629U},
  {2U,152U,828U,1630U},
  {2U,31U,946U,1631U},
  {2U,189U,336U,1632U},
  {2U,471U,869U,1633U},
  {2U,339U,487U,1634U},
  {2U,155U,75U,1635U},
  {2U,343U,901U,1636U},
  {2U,68U,29U,1637U},
  {2U,213U,563U,1638U},
  {2U,363U,101U,1639U},
  {2U,67U,650U,1640U},
  {2U,182U,925U,1641U},
  {2U,324U,965U,1642U},
  {2U,49U,18U,1643U},
  {2U,503U,329U,1644U},
  {2U,491U,94U,1645U},
  {2U,136U,104U,1646U},
  {2U,409U,726U,1647U},
  {2U,152U,747U,1648U},
  {2U,256U,592U,1649U},
  {2U,160U,266U,1650U},
  {2U,379U,530U,1651U},
  {2U,456U,52U,1652U},
  {2U,19U,743U,1653U},
  {2U,269U,364U,1654U},
  {2U,20U,707U,1655U},
  {2U,323U,179U,1656U},
  {2U,30U,392U,1657U},
  {2U,70U,680U,1658U},
  {2U,206U,765U,1659U},
  {2U,101U,404U,1660U},
  {2U,414U,118U,1661U},
  {2U,370U,936U,1662U},
  {2U,186U,123U,1663U},
  {2U,458U,83U,1664U},
  {2U,350U,60U,1665U},
  {2U,362U,810U,1666U},
  {2U,0U,306U,1667U},
  {2U,103U,286U,1668U},
  {2U,304U,924U,1669U},
  {2U,332U,599U,1670U},
  {2U,32U,601U,1671U},
  {2U,424U,201U,1672U},
  {2U,134U,890U,1673U},
  {2U,261U,530U,1674U},
  {2U,338U,130U,1675U},
  {2U,181U,596U,1676U},
  {2U,103U,181U,1677U},
  {2U,495U,718U,1678U},
  {2U,108U,854U,1679U},
  {2U,155U,299U,1680U},
  {2U,36U,314U,1681U},
  {2U,285U,689U,1682U},
  {2U,235U,351U,1683U},
  {2U,26U,991U,1684U},
  {2U,492U,775U,1685U},
  {2U,120U,271U,1686U},
  {2U,239U,136U,1687U},
  {2U,465U,938U,1688U},
  {2U,332U,867U,1689U},
  {2U,79U,618U,1690U},
  {2U,158U,250U,1691U},
  {2U,403U,350U,1692U},
  {2U,354U,786U,1693U},
  {2U,301U,508U,1694U},
  {2U,112U,50U,1695U},
  {2U,185U,139U,1696U},
  {2U,178U,354U,1697U},
  {2U,337U,771U,1698U},
  {2U,294U,219U,1699U},
  {2U,446U,971U,1700U},
  {2U,73U,679U,1701U},
  {2U,75U,990U,1702U},
  {2U,104U,343U,1703U},
  {2U,324U,379U,1704U},
  {2U,53U,192U,1705U},
  {2U,162U,169U,1706U},
  {2U,121U,692U,1707U},
  {2U,40U,937U,1708U},
  {2U,238U,778U,1709U},
  {2U,31U,425U,1710U},
  {2U,105U,928U,1711U},
  {2U,14U,576U,1712U},
  {2U,229U,987U,1713U},
  {2U,170U,904U,1714U},
  {2U,101U,868U,1715U},
  {2U,450U,455U,1716U},
  {2U,180U,805U,1717U},
  {2U,153U,527U,1718U},
  {2U,72U,676U,1719U},
  {2U,41U,247U,1720U},
  {2U,390U,135U,1721U},
  {2U,378U,383U,1722U},
  {2U,167U,805U,1723U},
  {2U,290U,459U,1724U},
  {2U,369U,894U,1725U},
  {2U,180U,73U,1726U},
  {2U,185U,594U,1727U},
  {2U,268U,875U,1728U},
  {2U,474U,779U,1729U},
  {2U,30U,556U,1730U},
  {2U,413U,199U,1731U},
  {2U,237U,153U,1732U},
  {2U,430U,120U,1733U},
  {2U,3U,539U,1734U},
  {2U,170U,574U,1735U},
  {2U,321U,341U,1736U},
  {2U,77U,652U,1737U},
  {2U,446U,32U,1738U},
  {2U,357U,50U,1739U},
  {2U,77U,689U,1740U},
  {2U,217U,645U,1741U},
  {2U,95U,917U,1742U},
  {2U,325U,723U,1743U},
  {2U,29U,435U,1744U},
  {2U,493U,48U,1745U},
  {2U,422U,92U,1746U},
  {2U,305U,279U,1747U},
  {2U,471U,480U,1748U},
  {2U,199U,815U,1749U},
  {2U,403U,416U,1750U},
  {2U,82U,407U,1751U},
  {2U,446U,723U,1752U},
  {2U,279U,838U,1753U},
  {2U,31U,883U,1754U},
  {2U,176U,773U,1755U},
  {2U,70U,623U,1756U},
  {2U,191U,656U,1757U},
  {2U,423U,719U,1758U},
  {2U,451U,949U,1759U},
  {2U,202U,896U,1760U},
  {2U,8U,955U,1761U},
  {2U,321U,244U,1762U},
  {2U,255U,167U,1763U},
  {2U,396U,384U,1764U},
  {2U,23U,17U,1765U},
  {2U,480U,84U,1766U},
  {2U,35U,741U,1767U},
  {2U,217U,283U,1768U},
  {2U,7U,904U,1769U},
  {2U,237U,864U,1770U},
  {2U,395U,210U,1771U},
  {2U,501U,736U,1772U},
  {2U,90U,114U,1773U},
  {2U,361U,229U,1774U},
  {2U,373U,800U,1775U},
  {2U,78U,93U,1776U},
  {2U,364U,975U,1777U},
  {2U,236U,813U,1778U},
  {2U,100U,99U,1779U},
  {2U,333U,71U,1780U},
  {2U,474U,79U,1781U},
  {2U,403U,929U,1782U},
  {2U,458U,436U,1783U},
  {2U,405U,866U,1784U},
  {2U,380U,82U,1785U},
  {2U,385U,715U,1786U},
  {2U,488U,599U,1787U},
  {2U,134U,64U,1788U},
  {2U,490U,18U,1789U},
  {2U,284U,47U,1790U},
  {2U,83U,512U,1791U},
  {2U,41U,194U,1792U},
  {2U,91U,657U,1793U},
  {2U,265U,467U,1794U},
  {2U,425U,924U,1795U},
  {2U,310U,974U,1796U},
  {2U,260U,751U,1797U},
  {2U,443U,968U,1798U},
  {2U,489U,282U,1799U},
  {2U,2U,694U,1800U},
  {2U,373U,575U,1801U},
  {2U,321U,479U,1802U},
  {2U,63U,319U,1803U},
  {2U,390U,148U,1804U},
  {2U,221U,536U,1805U},
  {2U,333U,181U,1806U},
  {2U,408U,223U,1807U},
  {2U,292U,468U,1808U},
  {2U,293U,809U,1809U},
  {2U,433U,994U,1810U},
  {2U,427U,461U,1811U},
  {2U,356U,425U,1812U},
  {2U,170U,188U,1813U},
  {2U,438U,386U,1814U},
  {2U,167U,371U,1815U},
  {2U,383U,374U,1816U},
  {2U,335U,174U,1817U},
  {2U,425U,29U,1818U},
  {2U,106U,786U,1819U},
  {2U,153U,138U,1820U},
  {2U,176U,253U,1821U},
  {2U,27U,444U,1822U},
  {2U,394U,492U,1823U},
  {2U,420U,218U,1824U},
  {2U,500U,728U,1825U},
  {2U,59U,734U,1826U},
  {2U,71U,768U,1827U},
  {2U,129U,260U,1828U},
  {2U,319U,733U,1829U},
  {2U,486U,968U,1830U},
  {2U,353U,49U,1831U},
  {2U,295U,45U,1832U},
  {2U,71U,927U,1833U},
  {2U,171U,122U,1834U},
  {2U,257U,766U,1835U},
  {2U,68U,938U,1836U},
  {2U,178U,254U,1837U},
  {2U,260U,681U,1838U},
  {2U,430U,204U,1839U},
  {2U,42U,692U,1840U},
  {2U,173U,665U,1841U},
  {2U,117U,882U,1842U},
  {2U,95U,711U,1843U},
  {2U,122U,386U,1844U},
  {2U,380U,720U,1845U},
  {2U,185U,736U,1846U},
  {2U,2U,420U,1847U},
  {2U,28U,99U,1848U},
  {2U,233U,180U,1849U},
  {2U,415U,518U,1850U},
  {2U,43U,16U,1851U},
  {2U,329U,165U,1852U},
  {2U,299U,458U,1853U},
  {2U,387U,456U,1854U},
  {2U,52U,25U,1855U},
  {2U,120U,825U,1856U},
  {2U,455U,988U,1857U},
  {2U,138U,737U,1858U},
  {2U,78U,448U,1859U},
  {2U,367U,625U,1860U},
  {2U,263U,832U,1861U},
  {2U,215U,171U,1862U},
  {2U,127U,794U,1863U},
  {2U,363U,178U,1864U},
  {2U,226U,545U,1865U},
  {2U,216U,132U,1866U},
  {2U,7U,204U,1867U},
  {2U,456U,199U,1868U},
  {2U,240U,318U,1869U},
  {2U,418U,524U,1870U},
  {2U,400U,650U,1871U},
  {2U,299U,841U,1872U},
  {2U,9U,609U,1873U},
  {2U,274U,325U,1874U},
  {2U,496U,531U,1875U},
  {2U,229U,787U,1876U},
  {2U,197U,138U,1877U},
  {2U,49U,951U,1878U},
  {2U,313U,452U,1879U},
  {2U,309U,842U,1880U},
  {2U,94U,171U,1881U},
  {2U,124U,779U,1882U},
  {2U,166U,901U,1883U},
  {2U,221U,12U,1884U},
  {2U,121U,448U,1885U},
  {2U,24U,676U,1886U},
  {2U,474U,510U,1887U},
  {2U,129U,142U,1888U},
  {2U,181U,410U,1889U},
  {2U,200U,471U,1890U},
  {2U,320U,218U,1891U},
  {2U,43U,804U,1892U},
  {2U,474U,54U,1893U},
  {2U,360U,422U,1894U},
  {2U,324U,61U,1895U},
  {2U,133U,631U,1896U},
  {2U,239U,547U,1897U},
  {2U,485U,896U,1898U},
  {2U,174U,894U,1899U},
  {2U,107U,175U,1900U},
  {2U,223U,313U,1901U},
  {2U,112U,85U,1902U},
  {2U,421U,114U,1903U},
  {2U,135U,807U,1904U},
  {2U,508U,803U,1905U},
  {2U,418U,227U,1906U},
  {2U,88U,181U,1907U},
  {2U,84U,860U,1908U},
  {2U,25U,228U,1909U},
  {2U,386U,759U,1910U},
  {2U,397U,340U,1911U},
  {2U,360U,906U,1912U},
  {2U,121U,935U,1913U},
  {2U,354U,42U,1914U},
  {2U,78U,413U,1915U},
  {2U,385U,453U,1916U},
  {2U,238U,375U,1917U},
  {2U,380U,882U,1918U},
  {2U,155U,641U,1919U},
  {2U,442U,435U,1920U},
  {2U,473U,753U,1921U},
  {2U,196U,451U,1922U},
  {2U,135U,746U,1923U},
  {2U,9U,793U,1924U},
  {2U,355U,287U,1925U},
  {2U,186U,709U,1926U},
  {2U,181U,429U,1927U},
  {2U,261U,610U,1928U},
  {2U,123U,968U,1929U},
  {2U,358U,760U,1930U},
  {2U,498U,255U,1931U},
  {2U,216U,463U,1932U},
  {2U,1U,341U,1933U},
  {2U,455U,996U,1934U},
  {2U,465U,347U,1935U},
  {2U,160U,728U,1936U},
  {2U,181U,250U,1937U},
  {2U,349U,899U,1938U},
  {2U,432U,256U,1939U},
  {2U,255U,357U,1940U},
  {2U,231U,43U,1941U},
  {2U,366U,489U,1942U},
  {2U,356U,790U,1943U},
  {2U,484U,762U,1944U},
  {2U,177U,677U,1945U},
  {2U,266U,205U,1946U},
  {2U,289U,499U,1947U},
  {2U,451U,275U,1948U},
  {2U,404U,837U,1949U},
  {2U,221U,370U,1950U},
  {2U,250U,585U,1951U},
  {2U,255U,143U,1952U},
  {2U,134U,739U,1953U},
  {2U,67U,960U,1954U},
  {2U,74U,577U,1955U},
  {2U,224U,489U,1956U},
  {2U,168U,775U,1957U},
  {2U,437U,453U,1958U},
  {2U,41U,524U,1959U},
  {2U,369U,967U,1960U},
  {2U,357U,278U,1961U},
  {2U,461U,340U,1962U},
  {2U,467U,377U,1963U},
  {2U,418U,29U,1964U},
  {2U,259U,930U,1965U},
  {2U,402U,489U,1966U},
  {2U,223U,50U,1967U},
  {2U,279U,519U,1968U},
  {2U,400U,391U,1969U},
  {2U,82U,308U,1970U},
  {2U,188U,741U,1971U},
  {2U,119U,211U,1972U},
  {2U,19U,484U,1973U},
  {2U,448U,183U,1974U},
  {2U,457U,524U,1975U},
  {2U,348U,317U,1976U},
  {2U,360U,437U,1977U},
  {2U,504U,712U,1978U},
  {2U,240U,384U,1979U},
  {2U,284U,67U,1980U},
  {2U,42U,86U,1981U},
  {2U,205U,683U,1982U},
  {2U,53U,480U,1983U},
  {2U,118U,738U,1984U},
  {2U,253U,702U,1985U},
  {2U,397U,412U,1986U},
  {2U,251U,545U,1987U},
  {2U,260U,143U,1988U},
  {2U,460U,202U,1989U},
  {2U,351U,17U,1990U},
  {2U,44U,51U,1991U},
  {2U,55U,84U,1992U},
  {2U,295U,711U,1993U},
  {2U,432U,976U,1994U},
  {2U,91U,351U,1995U},
  {2U,86U,816U,1996U},
  {2U,84U,222U,1997U},
  {2U,385U,511U,1998U},
  {2U,37U,204U,1999U},
  {2U,91U,102U,2000U},
  {2U,70U,51U,2001U},
  {2U,291U,335U,2002U},
  {2U,318U,264U,2003U},
  {2U,283U,809U,2004U},
  {2U,434U,396U,2005U},
  {2U,69U,51U,2006U},
  {2U,367U,322U,2007U},
  {2U,235U,892U,2008U},
  {2U,173U,544U,2009U},
  {2U,81U,467U,2010U},
  {2U,483U,224U,2011U},
  {2U,488U,668U,2012U},
  {2U,281U,38U,2013U},
  {2U,125U,358U,2014U},
  {2U,240U,663U,2015U},
  {2U,43U,431U,2016U},
  {2U,336U,309U,2017U},
  {2U,121U,612U,2018U},
  {2U,2U,73U,2019U},
  {2U,176U,967U,2020U},
  {2U,467U,114U,2021U},
  {2U,201U,657U,2022U},
  {2U,38U,608U,2023U},
  {2U,501U,256U,2024U},
  {2U,380U,878U,2025U},
  {2U,366U,685U,2026U},
  {2U,470U,55U,2027U},
  {2U,434U,947U,2028U},
  {2U,301U,608U,2029U},
  {2U,349U,320U,2030U},
  {2U,379U,565U,2031U},
  {2U,173U,621U,2032U},
  {2U,44U,878U,2033U},
  {2U,162U,446U,2034U},
  {2U,384U,869U,2035U},
  {2U,386U,897U,2036U},
  {2U,56U,519U,2037U},
  {2U,404U,809U,2038U},
  {2U,471U,36U,2039U},
  {2U,412U,392U,2040U},
  {2U,334U,397U,2041U},
  {2U,65U,63U,2042U},
  {2U,416U,144U,2043U},
  {2U,368U,420U,2044U},
  {2U,80U,907U,2045U},
  {2U,480U,882U,2046U},
  {2U,208U,48U,2047U},
  {2U,397U,36U,2048U},
  {2U,329U,227U,2049U},
  {2U,10U,954U,2050U},
  {2U,226U,676U,2051U},
  {2U,296U,222U,2052U},
  {2U,36U,119U,2053U},
  {2U,193U,281U,2054U},
  {2U,430U,993U,2055U},
  {2U,334U,481U,2056U},
  {2U,417U,238U,2057U},
  {2U,418U,469U,2058U},
  {2U,390U,440U,2059U},
  {2U,243U,566U,2060U},
  {2U,72U,765U,2061U},
  {2U,251U,411U,2062U},
  {2U,59U,382U,2063U},
  {2U,85U,857U,2064U},
  {2U,98U,912U,2065U},
  {2U,128U,702U,2066U},
  {2U,256U,867U,2067U},
  {2U,429U,634U,2068U},
  {2U,407U,880U,2069U},
  {2U,207U,196U,2070U},
  {2U,14U,406U,2071U},
  {2U,86U,155U,2072U},
  {2U,449U,9U,2073U},
  {2U,202U,729U,2074U},
  {2U,330U,787U,2075U},
  {2U,480U,760U,2076U},
  {2U,290U,111U,2077U},
  {2U,164U,839U,2078U},
  {2U,94U,438U,2079U},
  {2U,109U,872U,2080U},
  {2U,273U,893U,2081U},
  {2U,44U,896U,2082U},
  {2U,178U,105U,2083U},
  {2U,4U,256U,2084U},
  {2U,179U,472U,2085U},
  {2U,363U,932U,2086U},
  {2U,24U,361U,2087U},
  {2U,401U,646U,2088U},
  {2U,388U,434U,2089U},
  {2U,443U,142U,2090U},
  {2U,101U,561U,2091U},
  {2U,117U,942U,2092U},
  {2U,321U,894U,2093U},
  {2U,409U,206U,2094U},
  {2U,197U,125U,2095U},
  {2U,254U,618U,2096U},
  {2U,102U,332U,2097U},
  {2U,481U,352U,2098U},
  {2U,476U,418U,2099U},
  {2U,472U,397U,2100U},
  {2U,386U,193U,2101U},
  {2U,59U,461U,2102U},
  {2U,243U,198U,2103U},
  {2U,231U,829U,2104U},
  {2U,363U,328U,2105U},
  {2U,384U,453U,2106U},
  {2U,354U,333U,2107U},
  {2U,460U,848U,2108U},
  {2U,265U,116U,2109U},
  {2U,455U,547U,2110U},
  {2U,113U,267U,2111U},
  {2U,363U,783U,2112U},
  {2U,65U,475U,2113U},
  {2U,297U,107U,2114U},
  {2U,316U,445U,2115U},
  {2U,175U,375U,2116U},
  {2U,146U,599U,2117U},
  {2U,421U,869U,2118U},
  {2U,299U,511U,2119U},
  {2U,27U,525U,2120U},
  {2U,160U,662U,2121U},
  {2U,492U,421U,2122U},
  {2U,304U,976U,2123U},
  {2U,211U,985U,2124U},
  {2U,78U,402U,2125U},
  {2U,146U,651U,2126U},
  {2U,53U,937U,2127U},
  {2U,398U,290U,2128U},
  {2U,434U,61U,2129U},
  {2U,485U,216U,2130U},
  {2U,483U,108U,2131U},
  {2U,329U,957U,2132U},
  {2U,157U,546U,2133U},
  {2U,51U,429U,2134U},
  {2U,436U,907U,2135U},
  {2U,323U,18U,2136U},
  {2U,277U,73U,2137U},
  {2U,503U,741U,2138U},
  {2U,253U,536U,2139U},
  {2U,432U,363U,2140U},
  {2U,141U,878U,2141U},
  {2U,62U,197U,2142U},
  {2U,275U,398U,2143U},
  {2U,138U,373U,2144U},
  {2U,430U,416U,2145U},
  {2U,341U,150U,2146U},
  {2U,109U,257U,2147U},
  {2U,381U,452U,2148U},
  {2U,167U,77U,2149U},
  {2U,4U,968U,2150U},
  {2U,226U,914U,2151U},
  {2U,378U,773U,2152U},
  {2U,279U,627U,2153U},
  {2U,277U,154U,2154U},
  {2U,433U,794U,2155U},
  {2U,478U,248U,2156U},
  {2U,44U,998U,2157U},
  {2U,212U,106U,2158U},
  {2U,13U,203U,2159U},
  {2U,232U,86U,2160U},
  {2U,428U,574U,2161U},
  {2U,324U,390U,2162U},
  {2U,459U,340U,2163U},
  {2U,278U,647U,2164U},
  {2U,191U,371U,2165U},
  {2U,451U,723U,2166U},
  {2U,17U,554U,2167U},
  {2U,310U,729U,2168U},
  {2U,42U,493U,2169U},
  {2U,471U,597U,2170U},
  {2U,133U,255U,2171U},
  {2U,49U,754U,2172U},
  {2U,114U,719U,2173U},
  {2U,372U,491U,2174U},
  {2U,289U,333U,2175U},
  {2U,52U,143U,2176U},
  {2U,131U,939U,2177U},
  {2U,411U,70U,2178U},
  {2U,511U,531U,2179U},
  {2U,476U,929U,2180U},
  {2U,27U,490U,2181U},
  {2U,214U,594U,2182U},
  {2U,106U,526U,2183U},
  {2U,408U,759U,2184U},
  {2U,197U,359U,2185U},
  {2U,415U,145U,2186U},
  {2U,442U,533U,2187U},
  {2U,216U,617U,2188U},
  {2U,36U,835U,2189U},
  {2U,496U,166U,2190U},
  {2U,180U,751U,2191U},
  {2U,42U,440U,2192U},
  {2U,215U,562U,2193U},
  {2U,406U,610U,2194U},
  {2U,137U,167U,2195U},
  {2U,349U,87U,2196U},
  {2U,158U,705U,2197U},
  {2U,16U,35U,2198U},
  {2U,285U,889U,2199U},
  {2U,105U,39U,2200U},
  {2U,455U,119U,2201U},
  {2U,125U,946U,2202U},
  {2U,511U,732U,2203U},
  {2U,417U,149U,2204U},
  {2U,177U,646U,2205U},
  {2U,365U,717U,2206U},
  {2U,390U,570U,2207U},
  {2U,177U,360U,2208U},
  {2U,3U,360U,2209U},
  {2U,305U,957U,2210U},
  {2U,207U,818U,2211U},
  {2U,401U,484U,2212U},
  {2U,97U,698U,2213U},
  {2U,465U,757U,2214U},
  {2U,442U,27U,2215U},
  {2U,334U,504U,2216U},
  {2U,448U,882U,2217U},
  {2U,202U,901U,2218U},
  {2U,199U,634U,2219U},
  {2U,157U,591U,2220U},
  {2U,224U,841U,2221U},
  {2U,60U,984U,2222U},
  {2U,178U,350U,2223U},
  {2U,13U,940U,2224U},
  {2U,242U,323U,2225U},
  {2U,261U,323U,2226U},
  {2U,297U,779U,2227U},
  {2U,374U,639U,2228U},
  {2U,487U,277U,2229U},
  {2U,286U,653U,2230U},
  {2U,293U,138U,2231U},
  {2U,73U,700U,2232U},
  {2U,142U,577U,2233U},
  {2U,454U,621U,2234U},
  {2U,220U,811U,2235U},
  {2U,435U,621U,2236U},
  {2U,62U,746U,2237U},
  {2U,53U,433U,2238U},
  {2U,124U,398U,2239U},
  {2U,38U,755U,2240U},
  {2U,46U,989U,2241U},
  {2U,147U,270U,2242U},
  {2U,252U,330U,2243U},
  {2U,315U,105U,2244U},
  {2U,271U,826U,2245U},
  {2U,203U,860U,2246U},
  {2U,100U,48U,2247U},
  {2U,359U,241U,2248U},
  {2U,251U,703U,2249U},
  {2U,42U,920U,2250U},
  {2U,427U,573U,2251U},
  {2U,30U,68U,2252U},
  {2U,218U,286U,2253U},
  {2U,437U,431U,2254U},
  {2U,153U,107U,2255U},
  {2U,77U,99U,2256U},
  {2U,141U,92U,2257U},
  {2U,169U,670U,2258U},
  {2U,27U,461U,2259U},
  {2U,144U,405U,2260U},
  {2U,480U,373U,2261U},
  {2U,278U,895U,2262U},
  {2U,30U,229U,2263U},
  {2U,90U,29U,2264U},
  {2U,196U,971U,2265U},
  {2U,106U,772U,2266U},
  {2U,39U,21U,2267U},
  {2U,459U,836U,2268U},
  {2U,442U,559U,2269U},
  {2U,40U,876U,2270U},
  {2U,0U,329U,2271U},
  {2U,413U,701U,2272U},
  {2U,154U,629U,2273U},
  {2U,453U,74U,2274U},
  {2U,59U,390U,2275U},
  {2U,121U,615U,2276U},
  {2U,363U,326U,2277U},
  {2U,408U,712U,2278U},
  {2U,166U,216U,2279U},
  {2U,362U,136U,2280U},
  {2U,124U,870U,2281U},
  {2U,121U,962U,2282U},
  {2U,190U,313U,2283U},
  {2U,185U,581U,2284U},
  {2U,144U,290U,2285U},
  {2U,65U,919U,2286U},
  {2U,23U,852U,2287U},
  {2U,156U,700U,2288U},
  {2U,104U,172U,2289U},
  {2U,181U,392U,2290U},
  {2U,253U,983U,2291U},
  {2U,220U,444U,2292U},
  {2U,171U,125U,2293U},
  {2U,451U,720U,2294U},
  {2U,460U,129U,2295U},
  {2U,19U,889U,2296U},
  {2U,280U,335U,2297U},
  {2U,129U,83U,2298U},
  {2U,428U,255U,2299U},
  {2U,203U,376U,2300U},
  {2U,112U,957U,2301U},
  {2U,231U,794U,2302U},
  {2U,288U,491U,2303U},
  {2U,424U,768U,2304U},
  {2U,12U,965U,2305U},
  {2U,262U,855U,2306U},
  {2U,213U,834U,2307U},
  {2U,334U,259U,2308U},
  {2U,92U,971U,2309U},
  {2U,205U,390U,2310U},
  {2U,252U,344U,2311U},
  {2U,465U,616U,2312U},
  {2U,108U,711U,2313U},
  {2U,473U,469U,2314U},
  {2U,215U,965U,2315U},
  {2U,126U,743U,2316U},
  {2U,418U,672U,2317U},
  {2U,493U,14U,2318U},
  {2U,61U,588U,2319U},
  {2U,68U,704U,2320U},
  {2U,160U,914U,2321U},
  {2U,89U,692U,2322U},
  {2U,181U,83U,2323U},
  {2U,387U,940U,2324U},
  {2U,31U,329U,2325U},
  {2U,8U,629U,2326U},
  {2U,496U,673U,2327U},
  {2U,293U,775U,2328U},
  {2U,363U,276U,2329U},
  {2U,129U,20U,2330U},
  {2U,208U,753U,2331U},
  {2U,38U,914U,2332U},
  {2U,451U,757U,2333U},
  {2U,298U,985U,2334U},
  {2U,425U,91U,2335U},
  {2U,276U,10U,2336U},
  {2U,133U,623U,2337U},
  {2U,475U,223U,2338U},
  {2U,242U,913U,2339U},
  {2U,384U,541U,2340U},
  {2U,486U,416U,2341U},
  {2U,18U,219U,2342U},
  {2U,118U,768U,2343U},
  {2U,197U,435U,2344U},
  {2U,414U,223U,2345U},
  {2U,379U,479U,2346U},
  {2U,356U,471U,2347U},
  {2U,502U,425U,2348U},
  {2U,360U,434U,2349U},
  {2U,444U,433U,2350U},
  {2U,361U,318U,2351U},
  {2U,445U,538U,2352U},
  {2U,206U,942U,2353U},
  {2U,155U,721U,2354U},
  {2U,220U,988U,2355U},
  {2U,500U,142U,2356U},
  {2U,334U,962U,2357U},
  {2U,327U,907U,2358U},
  {2U,284U,340U,2359U},
  {2U,426U,822U,2360U},
  {2U,437U,283U,2361U},
  {2U,344U,985U,2362U},
  {2U,295U,140U,2363U},
  {2U,183U,873U,2364U},
  {2U,359U,169U,2365U},
  {2U,8U,757U,2366U},
  {2U,282U,275U,2367U},
  {2U,126U,160U,2368U},
  {2U,185U,599U,2369U},
  {2U,198U,437U,2370U},
  {2U,398U,943U,2371U},
  {2U,473U,262U,2372U},
  {2U,95U,965U,2373U},
  {2U,419U,993U,2374U},
  {2U,394U,137U,2375U},
  {2U,47U,229U,2376U},
  {2U,242U,608U,2377U},
  {2U,195U,531U,2378U},
  {2U,241U,765U,2379U},
  {2U,161U,190U,2380U},
  {2U,347U,297U,2381U},
  {2U,130U,250U,2382U},
  {2U,299U,856U,2383U},
  {2U,505U,562U,2384U},
  {2U,25U,734U,2385U},
  {2U,136U,307U,2386U},
  {2U,71U,360U,2387U},
  {2U,269U,710U,2388U},
  {2U,107U,114U,2389U},
  {2U,201U,417U,2390U},
  {2U,286U,676U,2391U},
  {2U,264U,171U,2392U},
  {2U,31U,422U,2393U},
  {2U,485U,289U,2394U},
  {2U,497U,149U,2395U},
  {2U,406U,409U,2396U},
  {2U,147U,189U,2397U},
  {2U,330U,199U,2398U},
  {2U,215U,586U,2399U},
  {2U,231U,273U,2400U},
  {2U,87U,599U,2401U},
  {2U,269U,367U,2402U},
  {2U,330U,343U,2403U},
  {2U,298U,580U,2404U},
  {2U,156U,366U,2405U},
  {2U,307U,594U,2406U},
  {2U,356U,287U,2407U},
  {2U,137U,854U,2408U},
  {2U,189U,396U,2409U},
  {2U,378U,60U,2410U},
  {2U,163U,339U,2411U},
  {2U,474U,471U,2412U},
  {2U,431U,945U,2413U},
  {2U,346U,376U,2414U},
  {2U,498U,478U,2415U},
  {2U,331U,321U,2416U},
  {2U,29U,177U,2417U},
  {2U,73U,557U,2418U},
  {2U,128U,31U,2419U},
  {2U,167U,251U,2420U},
  {2U,244U,27U,2421U},
  {2U,364U,724U,2422U},
  {2U,124U,832U,2423U},
  {2U,309U,367U,2424U},
  {2U,208U,85U,2425U},
  {2U,447U,606U,2426U},
  {2U,238U,140U,2427U},
  {2U,114U,685U,2428U},
  {2U,314U,542U,2429U},
  {2U,464U,826U,2430U},
  {2U,445U,996U,2431U},
  {2U,261U,855U,2432U},
  {2U,25U,745U,2433U},
  {2U,278U,214U,2434U},
  {2U,43U,569U,2435U},
  {2U,341U,317U,2436U},
  {2U,178U,732U,2437U},
  {2U,137U,88U,2438U},
  {2U,306U,635U,2439U},
  {2U,259U,640U,2440U},
  {2U,119U,185U,2441U},
  {2U,3U,540U,2442U},
  {2U,338U,207U,2443U},
  {2U,3U,163U,2444U},
  {2U,233U,493U,2445U},
  {2U,373U,419U,2446U},
  {2U,71U,537U,2447U},
  {2U,511U,40U,2448U},
  {2U,14U,12U,2449U},
  {2U,216U,444U,2450U},
  {2U,270U,868U,2451U},
  {2U,389U,400U,2452U},
  {2U,136U,582U,2453U},
  {2U,145U,427U,2454U},
  {2U,500U,878U,2455U},
  {2U,446U,711U,2456U},
  {2U,486U,99U,2457U},
  {2U,132U,966U,2458U},
  {2U,342U,450U,2459U},
  {2U,260U,482U,2460U},
  {2U,433U,743U,2461U},
  {2U,417U,970U,2462U},
  {2U,223U,268U,2463U},
  {2U,370U,229U,2464U},
  {2U,276U,321U,2465U},
  {2U,393U,64U,2466U},
  {2U,509U,549U,2467U},
  {2U,25U,251U,2468U},
  {2U,320U,41U,2469U},
  {2U,79U,282U,2470U},
  {2U,501U,374U,2471U},
  {2U,13U,98U,2472U},
  {2U,155U,230U,2473U},
  {2U,288U,579U,2474U},
  {2U,83U,534U,2475U},
  {2U,288U,595U,2476U},
  {2U,472U,330U,2477U},
  {2U,357U,314U,2478U},
  {2U,313U,465U,2479U},
  {2U,102U,98U,2480U},
  {2U,162U,623U,2481U},
  {2U,467U,43U,2482U},
  {2U,182U,515U,2483U},
  {2U,74U,480U,2484U},
  {2U,467U,792U,2485U},
  {2U,191U,324U,2486U},
  {2U,52U,416U,2487U},
  {2U,464U,486U,2488U},
  {2U,69U,277U,2489U},
  {2U,240U,389U,2490U},
  {2U,235U,648U,2491U},
  {2U,83U,947U,2492U},
  {2U,506U,268U,2493U},
  {2U,449U,320U,2494U},
  {2U,97U,316U,2495U},
  {2U,411U,678U,2496U},
  {2U,377U,594U,2497U},
  {2U,125U,747U,2498U},
  {2U,175U,870U,2499U},
  {2U,214U,126U,2500U},
  {2U,111U,489U,2501U},
  {2U,199U,19U,2502U},
  {2U,377U,981U,2503U},
  {2U,248U,80U,2504U},
  {2U,168U,534U,2505U},
  {2U,299U,718U,2506U},
  {2U,317U,771U,2507U},
  {2U,126U,373U,2508U},
  {2U,313U,442U,2509U},
  {2U,453U,519U,2510U},
  {2U,274U,802U,2511U},
  {2U,70U,668U,2512U},
  {2U,213U,243U,2513U},
  {2U,120U,59U,2514U},
  {2U,54U,854U,2515U},
  {2U,243U,432U,2516U},
  {2U,450U,920U,2517U},
  {2U,239U,439U,2518U},
  {2U,452U,188U,2519U},
  {2U,461U,753U,2520U},
  {2U,378U,192U,2521U},
  {2U,65U,113U,2522U},
  {2U,434U,433U,2523U},
  {2U,368U,783U,2524U},
  {2U,475U,960U,2525U},
  {2U,329U,122U,2526U},
  {2U,367U,493U,2527U},
  {2U,187U,18U,2528U},
  {2U,408U,622U,2529U},
  {2U,450U,89U,2530U},
  {2U,71U,384U,2531U},
  {2U,82U,369U,2532U},
  {2U,234U,914U,2533U},
  {2U,96U,964U,2534U},
  {2U,242U,378U,2535U},
  {2U,354U,40U,2536U},
  {2U,132U,338U,2537U},
  {2U,260U,960U,2538U},
  {2U,224U,604U,2539U},
  {2U,336U,125U,2540U},
  {2U,240U,650U,2541U},
  {2U,385U,693U,2542U},
  {2U,217U,438U,2543U},
  {2U,100U,477U,2544U},
  {2U,446U,887U,2545U},
  {2U,400U,623U,2546U},
  {2U,499U,888U,2547U},
  {2U,324U,328U,2548U},
  {2U,344U,119U,2549U},
  {2U,374U,27U,2550U},
  {2U,262U,750U,2551U},
  {2U,270U,149U,2552U},
  {2U,313U,54U,2553U},
  {2U,23U,202U,2554U},
  {2U,506U,418U,2555U},
  {2U,30U,566U,2556U},
  {2U,40U,950U,2557U},
  {2U,373U,403U,2558U},
  {2U,301U,360U,2559U},
  {2U,336U,898U,2560U},
  {2U,486U,688U,2561U},
  {2U,474U,270U,2562U},
  {2U,140U,50U,2563U},
  {2U,290U,791U,2564U},
  {2U,167U,153U,2565U},
  {2U,465U,365U,2566U},
  {2U,62U,183U,2567U},
  {2U,21U,700U,2568U},
  {2U,28U,894U,2569U},
  {2U,67U,783U,2570U},
  {2U,246U,582U,2571U},
  {2U,451U,161U,2572U},
  {2U,317U,771U,2573U},
  {2U,311U,83U,2574U},
  {2U,144U,611U,2575U},
  {2U,330U,504U,2576U},
  {2U,266U,654U,2577U},
  {2U,321U,869U,2578U},
  {2U,345U,852U,2579U},
  {2U,126U,38U,2580U},
  {2U,34U,966U,2581U},
  {2U,481U,461U,2582U},
  {2U,283U,919U,2583U},
  {2U,78U,588U,2584U},
  {2U,407U,80U,2585U},
  {2U,485U,712U,2586U},
  {2U,186U,648U,2587U},
  {2U,411U,85U,2588U},
  {2U,367U,242U,2589U},
  {2U,161U,950U,2590U},
  {2U,356U,140U,2591U},
  {2U,304U,181U,2592U},
  {2U,288U,227U,2593U},
  {2U,183U,378U,2594U},
  {2U,207U,42U,2595U},
  {2U,371U,921U,2596U},
  {2U,412U,563U,2597U},
  {2U,317U,407U,2598U},
  {2U,40U,502U,2599U},
  {2U,384U,668U,2600U},
  {2U,39U,848U,2601U},
  {2U,430U,973U,2602U},
  {2U,292U,584U,2603U},
  {2U,498U,748U,2604U},
  {2U,65U,682U,2605U},
  {2U,7U,401U,2606U},
  {2U,299U,735U,2607U},
  {2U,12U,46U,2608U},
  {2U,148U,397U,2609U},
  {2U,423U,740U,2610U},
  {2U,91U,193U,2611U},
  {2U,231U,618U,2612U},
  {2U,345U,646U,2613U},
  {2U,32U,303U,2614U},
  {2U,422U,545U,2615U},
  {2U,269U,804U,2616U},
  {2U,40U,167U,2617U},
  {2U,20U,570U,2618U},
  {2U,189U,167U,2619U},
  {2U,242U,8U,2620U},
  {2U,203U,70U,2621U},
  {2U,370U,773U,2622U},
  {2U,368U,95U,2623U},
  {2U,135U,690U,2624U},
  {2U,476U,842U,2625U},
  {2U,190U,811U,2626U},
  {2U,374U,847U,2627U},
  {2U,91U,524U,2628U},
  {2U,83U,195U,2629U},
  {2U,36U,436U,2630U},
  {2U,147U,978U,2631U},
  {2U,404U,439U,2632U},
  {2U,74U,283U,2633U},
  {2U,212U,703U,2634U},
  {2U,47U,305U,2635U},
  {2U,343U,408U,2636U},
  {2U,451U,581U,2637U},
  {2U,234U,290U,2638U},
  {2U,421U,50U,2639U},
  {2U,478U,363U,2640U},
  {2U,448U,415U,2641U},
  {2U,317U,994U,2642U},
  {2U,500U,257U,2643U},
  {2U,304U,536U,2644U},
  {2U,224U,298U,2645U},
  {2U,242U,326U,2646U},
  {2U,325U,197U,2647U},
  {2U,408U,656U,2648U},
  {2U,139U,360U,2649U},
  {2U,20U,530U,2650U},
  {2U,210U,289U,2651U},
  {2U,385U,724U,2652U},
  {2U,307U,503U,2653U},
  {2U,86U,520U,2654U},
  {2U,364U,831U,2655U},
  {2U,402U,950U,2656U},
  {2U,328U,771U,2657U},
  {2U,461U,911U,2658U},
  {2U,309U,490U,2659U},
  {2U,396U,662U,2660U},
  {2U,176U,527U,2661U},
  {2U,113U,306U,2662U},
  {2U,9U,473U,2663U},
  {2U,183U,330U,2664U},
  {2U,144U,664U,2665U},
  {2U,124U,113U,2666U},
  {2U,82U,799U,2667U},
  {2U,400U,252U,2668U},
  {2U,13U,449U,2669U},
  {2U,506U,724U,2670U},
  {2U,283U,169U,2671U},
  {2U,402U,863U,2672U},
  {2U,379U,807U,2673U},
  {2U,353U,871U,2674U},
  {2U,37U,244U,2675U},
  {2U,14U,226U,2676U},
  {2U,14U,804U,2677U},
  {2U,120U,753U,2678U},
  {2U,241U,377U,2679U},
  {2U,445U,120U,2680U},
  {2U,13U,79U,2681U},
  {2U,20U,352U,2682U},
  {2U,240U,324U,2683U},
  {2U,498U,730U,2684U},
  {2U,319U,639U,2685U},
  {2U,147U,546U,2686U},
  {2U,431U,963U,2687U},
  {2U,345U,69U,2688U},
  {2U,434U,42U,2689U},
  {2U,411U,694U,2690U},
  {2U,337U,110U,2691U},
  {2U,119U,957U,2692U},
  {2U,455U,956U,2693U},
  {2U,141U,612U,2694U},
  {2U,212U,940U,2695U},
  {2U,485U,662U,2696U},
  {2U,91U,766U,2697U},
  {2U,434U,874U,2698U},
  {2U,371U,441U,2699U},
  {2U,146U,856U,2700U},
  {2U,141U,92U,2701U},
  {2U,409U,360U,2702U},
  {2U,345U,385U,2703U},
  {2U,98U,17U,2704U},
  {2U,243U,970U,2705U},
  {2U,491U,180U,2706U},
  {2U,219U,201U,2707U},
  {2U,378U,961U,2708U},
  {2U,8U,988U,2709U},
  {2U,459U,292U,2710U},
  {2U,255U,201U,2711U},
  {2U,6U,274U,2712U},
  {2U,204U,144U,2713U},
  {2U,481U,889U,2714U},
  {2U,355U,847U,2715U},
  {2U,239U,676U,2716U},
  {2U,231U,399U,2717U},
  {2U,381U,441U,2718U},
  {2U,493U,226U,2719U},
  {2U,136U,455U,2720U},
  {2U,54U,16U,2721U},
  {2U,230U,913U,2722U},
  {2U,110U,830U,2723U},
  {2U,33U,257U,2724U},
  {2U,502U,121U,2725U},
  {2U,166U,457U,2726U},
  {2U,285U,937U,2727U},
  {2U,266U,722U,2728U},
  {2U,285U,486U,2729U},
  {2U,491U,601U,2730U},
  {2U,202U,189U,2731U},
  {2U,238U,442U,2732U},
  {2U,101U,79U,2733U},
  {2U,439U,501U,2734U},
  {2U,89U,999U,2735U},
  {2U,130U,10U,2736U},
  {2U,248U,388U,2737U},
  {2U,484U,579U,2738U},
  {2U,436U,62U,2739U},
  {2U,198U,310U,2740U},
  {2U,151U,981U,2741U},
  {2U,171U,463U,2742U},
  {2U,433U,907U,2743U},
  {2U,232U,380U,2744U},
  {2U,435U,912U,2745U},
  {2U,472U,585U,2746U},
  {2U,375U,417U,2747U},
  {2U,356U,229U,2748U},
  {2U,334U,67U,2749U},
  {2U,213U,147U,2750U},
  {2U,315U,76U,2751U},
  {2U,198U,126U,2752U},
  {2U,419U,746U,2753U},
  {2U,286U,980U,2754U},
  {2U,506U,931U,2755U},
  {2U,54U,365U,2756U},
  {2U,337U,474U,2757U},
  {2U,387U,86U,2758U},
  {2U,130U,966U,2759U},
  {2U,408U,740U,2760U},
  {2U,78U,534U,2761U},
  {2U,162U,246U,2762U},
  {2U,323U,296U,2763U},
  {2U,138U,906U,2764U},
  {2U,448U,816U,2765U},
  {2U,244U,445U,2766U},
  {2U,338U,674U,2767U},
  {2U,257U,30U,2768U},
  {2U,15U,758U,2769U},
  {2U,461U,617U,2770U},
  {2U,107U,862U,2771U},
  {2U,53U,458U,2772U},
  {2U,314U,453U,2773U},
  {2U,440U,26U,2774U},
  {2U,160U,600U,2775U},
  {2U,141U,194U,2776U},
  {2U,26U,56U,2777U},
  {2U,150U,265U,2778U},
  {2U,273U,222U,2779U},
  {2U,161U,801U,2780U},
  {2U,110U,868U,2781U},
  {2U,474U,774U,2782U},
  {2U,107U,922U,2783U},
  {2U,153U,867U,2784U},
  {2U,244U,963U,2785U},
  {2U,195U,337U,2786U},
  {2U,438U,352U,2787U},
  {2U,173U,102U,2788U},
  {2U,3U,961U,2789U},
  {2U,80U,174U,2790U},
  {2U,238U,820U,2791U},
  {2U,193U,199U,2792U},
  {2U,500U,457U,2793U},
  {2U,15U,442U,2794U},
  {2U,6U,861U,2795U},
  {2U,18U,282U,2796U},
  {2U,57U,192U,2797U},
  {2U,422U,404U,2798U},
  {2U,437U,981U,2799U},
  {2U,320U,339U,2800U},
  {2U,37U,873U,2801U},
  {2U,254U,706U,2802U},
  {2U,441U,331U,2803U},
  {2U,394U,143U,2804U},
  {2U,340U,305U,2805U},
  {2U,233U,159U,2806U},
  {2U,355U,667U,2807U},
  {2U,322U,140U,2808U},
  {2U,149U,599U,2809U},
  {2U,324U,95U,2810U},
  {2U,118U,75U,2811U},
  {2U,104U,178U,2812U},
  {2U,172U,995U,2813U},
  {2U,55U,931U,2814U},
  {2U,475U,642U,2815U},
  {2U,49U,28U,2816U},
  {2U,511U,106U,2817U},
  {2U,260U,594U,2818U},
  {2U,505U,765U,2819U},
  {2U,474U,664U,2820U},
  {2U,490U,516U,2821U},
  {2U,15U,348U,2822U},
  {2U,410U,701U,2823U},
  {2U,302U,501U,2824U},
  {2U,305U,914U,2825U},
  {2U,227U,63U,2826U},
  {2U,285U,96U,2827U},
  {2U,494U,918U,2828U},
  {2U,88U,412U,2829U},
  {2U,90U,794U,2830U},
  {2U,18U,762U,2831U},
  {2U,107U,211U,2832U},
  {2U,312U,561U,2833U},
  {2U,453U,295U,2834U},
  {2U,113U,601U,2835U},
  {2U,423U,937U,2836U},
  {2U,497U,622U,2837U},
  {2U,348U,660U,2838U},
  {2U,453U,568U,2839U},
  {2U,178U,621U,2840U},
  {2U,355U,899U,2841U},
  {2U,44U,309U,2842U},
  {2U,307U,835U,2843U},
  {2U,433U,127U,2844U},
  {2U,185U,323U,2845U},
  {2U,334U,250U,2846U},
  {2U,113U,21U,2847U},
  {2U,190U,775U,2848U},
  {2U,13U,364U,2849U},
  {2U,301U,192U,2850U},
  {2U,74U,863U,2851U},
  {2U,308U,423U,2852U},
  {2U,208U,439U,2853U},
  {2U,342U,724U,2854U},
  {2U,253U,658U,2855U},
  {2U,215U,787U,2856U},
  {2U,93U,302U,2857U},
  {2U,465U,780U,2858U},
  {2U,183U,887U,2859U},
  {2U,468U,555U,2860U},
  {2U,407U,458U,2861U},
  {2U,159U,809U,2862U},
  {2U,415U,435U,2863U},
  {2U,228U,201U,2864U},
  {2U,143U,941U,2865U},
  {2U,41U,252U,2866U},
  {2U,112U,893U,2867U},
  {2U,415U,970U,2868U},
  {2U,382U,438U,2869U},
  {2U,352U,737U,2870U},
  {2U,468U,794U,2871U},
  {2U,254U,40U,2872U},
  {2U,326U,781U,2873U},
  {2U,292U,917U,2874U},
  {2U,493U,927U,2875U},
  {2U,294U,450U,2876U},
  {2U,368U,822U,2877U},
  {2U,467U,296U,2878U},
  {2U,451U,697U,2879U},
  {2U,340U,324U,2880U},
  {2U,136U,83U,2881U},
  {2U,228U,146U,2882U},
  {2U,45U,602U,2883U},
  {2U,501U,666U,2884U},
  {2U,185U,886U,2885U},
  {2U,69U,308U,2886U},
  {2U,327U,116U,2887U},
  {2U,461U,93U,2888U},
  {2U,330U,890U,2889U},
  {2U,126U,636U,2890U},
  {2U,440U,250U,2891U},
  {2U,290U,559U,2892U},
  {2U,484U,693U,2893U},
  {2U,66U,495U,2894U},
  {2U,138U,976U,2895U},
  {2U,272U,962U,2896U},
  {2U,295U,653U,2897U},
  {2U,100U,667U,2898U},
  {2U,128U,723U,2899U},
  {2U,117U,613U,2900U},
  {2U,69U,70U,2901U},
  {2U,395U,310U,2902U},
  {2U,358U,225U,2903U},
  {2U,337U,491U,2904U},
  {2U,484U,561U,2905U},
  {2U,64U,127U,2906U},
  {2U,80U,933U,2907U},
  {2U,110U,665U,2908U},
  {2U,65U,740U,2909U},
  {2U,162U,211U,2910U},
  {2U,150U,960U,2911U},
  {2U,120U,930U,2912U},
  {2U,343U,611U,2913U},
  {2U,295U,589U,2914U},
  {2U,430U,26U,2915U},
  {2U,478U,594U,2916U},
  {2U,182U,802U,2917U},
  {2U,174U,362U,2918U},
  {2U,235U,240U,2919U},
  {2U,286U,482U,2920U},
  {2U,183U,191U,2921U},
  {2U,273U,689U,2922U},
  {2U,238U,760U,2923U},
  {2U,332U,712U,2924U},
  {2U,72U,603U,2925U},
  {2U,234U,313U,2926U},
  {2U,357U,242U,2927U},
  {2U,200U,644U,2928U},
  {2U,315U,42U,2929U},
  {2U,440U,815U,2930U},
  {2U,395U,521U,2931U},
  {2U,169U,115U,2932U},
  {2U,335U,130U,2933U},
  {2U,12U,686U,2934U},
  {2U,311U,920U,2935U},
  {2U,294U,294U,2936U},
  {2U,335U,216U,2937U},
  {2U,171U,231U,2938U},
  {2U,39U,317U,2939U},
  {2U,450U,228U,2940U},
  {2U,190U,697U,2941U},
  {2U,396U,105U,2942U},
  {2U,391U,246U,2943U},
  {2U,439U,323U,2944U},
  {2U,423U,341U,2945U},
  {2U,20U,227U,2946U},
  {2U,324U,197U,2947U},
  {2U,270U,12U,2948U},
  {2U,299U,979U,2949U},
  {2U,432U,526U,2950U},
  {2U,343U,987U,2951U},
  {2U,370U,683U,2952U},
  {2U,423U,620U,2953U},
  {2U,84U,853U,2954U},
  {2U,402U,848U,2955U},
  {2U,194U,411U,2956U},
  {2U,175U,661U,2957U},
  {2U,441U,879U,2958U},
  {2U,78U,325U,2959U},
  {2U,87U,562U,2960U},
  {2U,436U,143U,2961U},
  {2U,113U,928U,2962U},
  {2U,12U,10U,2963U},
  {2U,102U,484U,2964U},
  {2U,336U,310U,2965U},
  {2U,475U,730U,2966U},
  {2U,511U,44U,2967U},
  {2U,48U,343U,2968U},
  {2U,322U,651U,2969U},
  {2U,105U,331U,2970U},
  {2U,279U,877U,2971U},
  {2U,50U,98U,2972U},
  {2U,451U,290U,2973U},
  {2U,479U,77U,2974U},
  {2U,164U,706U,2975U},
  {2U,408U,399U,2976U},
  {2U,379U,974U,2977U},
  {2U,298U,472U,2978U},
  {2U,384U,520U,2979U},
  {2U,259U,914U,2980U},
  {2U,345U,673U,2981U},
  {2U,492U,641U,2982U},
  {2U,113U,849U,2983U},
  {2U,388U,211U,2984U},
  {2U,215U,61U,2985U},
  {2U,249U,760U,2986U},
  {2U,41U,490U,2987U},
  {2U,27U,869U,2988U},
  {2U,386U,471U,2989U},
  {2U,140U,982U,2990U},
  {2U,334U,878U,2991U},
  {2U,147U,138U,2992U},
  {2U,236U,170U,2993U},
  {2U,83U,686U,2994U},
  {2U,73U,198U,2995U},
  {2U,121U,803U,2996U},
  {2U,126U,779U,2997U},
  {2U,80U,832U,2998U},
  {2U,477U,416U,2999U},
  {2U,338U,224U,3000U},
  {2U,290U,595U,3001U},
  {2U,335U,604U,3002U},
  {2U,306U,224U,3003U},
  {2U,232U,193U,3004U},
  {2U,499U,950U,3005U},
  {2U,248U,188U,3006U},
  {2U,80U,552U,3007U},
  {2U,132U,968U,3008U},
  {2U,315U,988U,3009U},
  {2U,441U,641U,3010U},
  {2U,465U,527U,3011U},
  {2U,275U,922U,3012U},
  {2U,381U,830U,3013U},
  {2U,221U,510U,3014U},
  {2U,170U,439U,3015U},
  {2U,262U,213U,3016U},
  {2U,369U,31U,3017U},
  {2U,287U,463U,3018U},
  {2U,493U,771U,3019U},
  {2U,412U,742U,3020U},
  {2U,122U,15U,3021U},
  {2U,415U,859U,3022U},
  {2U,341U,981U,3023U},
  {2U,81U,110U,3024U},
  {2U,298U,897U,3025U},
  {2U,427U,331U,3026U},
  {2U,88U,468U,3027U},
  {2U,458U,43U,3028U},
  {2U,237U,27U,3029U},
  {2U,395U,400U,3030U},
  {2U,420U,668U,3031U},
  {2U,92U,114U,3032U},
  {2U,40U,134U,3033U},
  {2U,404U,670U,3034U},
  {2U,233U,578U,3035U},
  {2U,0U,951U,3036U},
  {2U,506U,277U,3037U},
  {2U,137U,785U,3038U},
  {2U,382U,290U,3039U},
  {2U,144U,256U,3040U},
  {2U,309U,11U,3041U},
  {2U,169U,633U,3042U},
  {2U,346U,848U,3043U},
  {2U,297U,755U,3044U},
  {2U,269U,867U,3045U},
  {2U,323U,84U,3046U},
  {2U,309U,869U,3047U},
  {2U,60U,381U,3048U},
  {2U,326U,205U,3049U},
  {2U,351U,830U,3050U},
  {2U,209U,334U,3051U},
  {2U,399U,737U,3052U},
  {2U,384U,951U,3053U},
  {2U,250U,145U,3054U},
  {2U,76U,581U,3055U},
  {2U,79U,126U,3056U},
  {2U,452U,608U,3057U},
  {2U,106U,237U,3058U},
  {2U,406U,796U,3059U},
  {2U,228U,915U,3060U},
  {2U,0U,445U,3061U},
  {2U,240U,250U,3062U},
  {2U,330U,34U,3063U},
  {2U,243U,424U,3064U},
  {2U,328U,199U,3065U},
  {2U,203U,449U,3066U},
  {2U,306U,764U,3067U},
  {2U,438U,575U,3068U},
  {2U,437U,414U,3069U},
  {2U,31U,843U,3070U},
  {2U,372U,775U,3071U},
  {2U,144U,373U,3072U},
  {2U,218U,151U,3073U},
  {2U,406U,264U,3074U},
  {2U,370U,716U,3075U},
  {2U,312U,279U,3076U},
  {2U,258U,943U,3077U},
  {2U,282U,592U,3078U},
  {2U,95U,166U,3079U},
  {2U,293U,689U,3080U},
  {2U,461U,263U,3081U},
  {2U,466U,218U,3082U},
  {2U,33U,208U,3083U},
  {2U,471U,559U,3084U},
  {2U,266U,796U,3085U},
  {2U,83U,723U,3086U},
  {2U,4U,109U,3087U},
  {2U,139U,434U,3088U},
  {2U,256U,252U,3089U},
  {2U,293U,880U,3090U},
  {2U,246U,984U,3091U},
  {2U,85U,608U,3092U},
  {2U,443U,748U,3093U},
  {2U,146U,208U,3094U},
  {2U,207U,107U,3095U},
  {2U,480U,358U,3096U},
  {2U,147U,878U,3097U},
  {2U,38U,661U,3098U},
  {2U,234U,256U,3099U},
  {2U,187U,486U,3100U},
  {2U,150U,894U,3101U},
  {2U,367U,161U,3102U},
  {2U,329U,156U,3103U},
  {2U,486U,397U,3104U},
  {2U,463U,565U,3105U},
  {2U,160U,397U,3106U},
  {2U,158U,746U,3107U},
  {2U,51U,512U,3108U},
  {2U,405U,65U,3109U},
  {2U,353U,251U,3110U},
  {2U,173U,874U,3111U},
  {2U,28U,377U,3112U},
  {2U,436U,347U,3113U},
  {2U,366U,822U,3114U},
  {2U,331U,189U,3115U},
  {2U,266U,840U,3116U},
  {2U,205U,328U,3117U},
  {2U,504U,511U,3118U},
  {2U,49U,863U,3119U},
  {2U,37U,840U,3120U},
  {2U,313U,898U,3121U},
  {2U,345U,661U,3122U},
  {2U,145U,8U,3123U},
  {2U,474U,642U,3124U},
  {2U,44U,491U,3125U},
  {2U,286U,268U,3126U},
  {2U,244U,895U,3127U},
  {2U,297U,523U,3128U},
  {2U,45U,619U,3129U},
  {2U,347U,47U,3130U},
  {2U,488U,237U,3131U},
  {2U,113U,600U,3132U},
  {2U,250U,177U,3133U},
  {2U,450U,828U,3134U},
  {2U,319U,118U,3135U},
  {2U,127U,688U,3136U},
  {2U,335U,106U,3137U},
  {2U,494U,565U,3138U},
  {2U,336U,468U,3139U},
  {2U,142U,991U,3140U},
  {2U,498U,924U,3141U},
  {2U,74U,316U,3142U},
  {2U,254U,35U,3143U},
  {2U,232U,974U,3144U},
  {2U,148U,223U,3145U},
  {2U,94U,749U,3146U},
  {2U,103U,696U,3147U},
  {2U,231U,281U,3148U},
  {2U,223U,677U,3149U},
  {2U,11U,895U,3150U},
  {2U,131U,79U,3151U},
  {2U,425U,422U,3152U},
  {2U,312U,537U,3153U},
  {2U,455U,698U,3154U},
  {2U,265U,520U,3155U},
  {2U,452U,764U,3156U},
  {2U,67U,344U,3157U},
  {2U,210U,743U,3158U},
  {2U,437U,219U,3159U},
  {2U,335U,796U,3160U},
  {2U,76U,70U,3161U},
  {2U,256U,116U,3162U},
  {2U,380U,598U,3163U},
  {2U,60U,897U,3164U},
  {2U,433U,208U,3165U},
  {2U,346U,401U,3166U},
  {2U,6U,280U,3167U},
  {2U,425U,414U,3168U},
  {2U,92U,95U,3169U},
  {2U,484U,813U,3170U},
  {2U,321U,295U,3171U},
  {2U,260U,210U,3172U},
  {2U,377U,399U,3173U},
  {2U,411U,487U,3174U},
  {2U,484U,891U,3175U},
  {2U,419U,663U,3176U},
  {2U,283U,734U,3177U},
  {2U,39U,445U,3178U},
  {2U,325U,669U,3179U},
  {2U,226U,109U,3180U},
  {2U,25U,721U,3181U},
  {2U,45U,419U,3182U},
  {2U,349U,734U,3183U},
  {2U,357U,782U,3184U},
  {2U,463U,843U,3185U},
  {2U,423U,58U,3186U},
  {2U,165U,707U,3187U},
  {2U,60U,802U,3188U},
  {2U,70U,821U,3189U},
  {2U,125U,375U,3190U},
  {2U,253U,72U,3191U},
  {2U,296U,835U,3192U},
  {2U,35U,784U,3193U},
  {2U,137U,746U,3194U},
  {2U,40U,341U,3195U},
  {2U,237U,799U,3196U},
  {2U,399U,34U,3197U},
  {2U,493U,223U,3198U},
  {2U,478U,50U,3199U},
  {2U,307U,380U,3200U},
  {2U,339U,313U,3201U},
  {2U,175U,150U,3202U},
  {2U,11U,35U,3203U},
  {2U,365U,139U,3204U},
  {2U,10U,679U,3205U},
  {2U,197U,142U,3206U},
  {2U,63U,487U,3207U},
  {2U,389U,60U,3208U},
  {2U,439U,165U,3209U},
  {2U,63U,962U,3210U},
  {2U,237U,394U,3211U},
  {2U,217U,902U,3212U},
  {2U,391U,21U,3213U},
  {2U,198U,437U,3214U},
  {2U,371U,471U,3215U},
  {2U,245U,66U,3216U},
  {2U,449U,48U,3217U},
  {2U,275U,269U,3218U},
  {2U,400U,825U,3219U},
  {2U,338U,610U,3220U},
  {2U,451U,135U,3221U},
  {2U,414U,69U,3222U},
  {2U,105U,92U,3223U},
  {2U,207U,822U,3224U},
  {2U,84U,400U,3225U},
  {2U,161U,79U,3226U},
  {2U,305U,702U,3227U},
  {2U,329U,345U,3228U},
  {2U,504U,783U,3229U},
  {2U,487U,405U,3230U},
  {2U,48U,773U,3231U},
  {2U,483U,762U,3232U},
  {2U,348U,48U,3233U},
  {2U,132U,268U,3234U},
  {2U,281U,332U,3235U},
  {2U,215U,193U,3236U},
  {2U,308U,883U,3237U},
  {2U,421U,304U,3238U},
  {2U,271U,253U,3239U},
  {2U,93U,514U,3240U},
  {2U,292U,641U,3241U},
  {2U,232U,492U,3242U},
  {2U,370U,293U,3243U},
  {2U,403U,419U,3244U},
  {2U,254U,689U,3245U},
  {2U,419U,540U,3246U},
  {2U,182U,141U,3247U},
  {2U,386U,904U,3248U},
  {2U,324U,778U,3249U},
  {2U,183U,222U,3250U},
  {2U,509U,712U,3251U},
  {2U,428U,592U,3252U},
  {2U,424U,77U,3253U},
  {2U,334U,93U,3254U},
  {2U,141U,87U,3255U},
  {2U,486U,740U,3256U},
  {2U,422U,622U,3257U},
  {2U,93U,311U,3258U},
  {2U,21U,286U,3259U},
  {2U,454U,103U,3260U},
  {2U,462U,584U,3261U},
  {2U,481U,305U,3262U},
  {2U,423U,876U,3263U},
  {2U,124U,557U,3264U},
  {2U,436U,74U,3265U},
  {2U,430U,12U,3266U},
  {2U,205U,189U,3267U},
  {2U,345U,605U,3268U},
  {2U,187U,65U,3269U},
  {2U,418U,398U,3270U},
  {2U,297U,346U,3271U},
  {2U,432U,790U,3272U},
  {2U,388U,727U,3273U},
  {2U,437U,990U,3274U},
  {2U,298U,731U,3275U},
  {2U,417U,339U,3276U},
  {2U,311U,209U,3277U},
  {2U,102U,539U,3278U},
  {2U,155U,478U,3279U},
  {2U,91U,482U,3280U},
  {2U,81U,174U,3281U},
  {2U,284U,193U,3282U},
  {2U,62U,618U,3283U},
  {2U,184U,525U,3284U},
  {2U,177U,961U,3285U},
  {2U,276U,602U,3286U},
  {2U,465U,45U,3287U},
  {2U,105U,782U,3288U},
  {2U,476U,669U,3289U},
  {2U,79U,881U,3290U},
  {2U,341U,362U,3291U},
  {2U,302U,49U,3292U},
  {2U,76U,984U,3293U},
  {2U,103U,404U,3294U},
  {2U,348U,500U,3295U},
  {2U,114U,519U,3296U},
  {2U,72U,588U,3297U},
  {2U,474U,169U,3298U},
  {2U,228U,476U,3299U},
  {2U,133U,728U,3300U},
  {2U,279U,804U,3301U},
  {2U,231U,223U,3302U},
  {2U,235U,784U,3303U},
  {2U,116U,852U,3304U},
  {2U,355U,502U,3305U},
  {2U,108U,360U,3306U},
  {2U,259U,340U,3307U},
  {2U,164U,75U,3308U},
  {2U,179U,432U,3309U},
  {2U,496U,248U,3310U},
  {2U,202U,625U,3311U},
  {2U,295U,162U,3312U},
  {2U,504U,860U,3313U},
  {2U,369U,558U,3314U},
  {2U,269U,971U,3315U},
  {2U,419U,870U,3316U},
  {2U,142U,691U,3317U},
  {2U,88U,243U,3318U},
  {2U,61U,783U,3319U},
  {2U,47U,115U,3320U},
  {2U,32U,160U,3321U},
  {2U,409U,825U,3322U},
  {2U,390U,517U,3323U},
  {2U,481U,943U,3324U},
  {2U,423U,861U,3325U},
  {2U,0U,963U,3326U},
  {2U,268U,405U,3327U},
  {2U,374U,78U,3328U},
  {2U,411U,128U,3329U},
  {2U,487U,805U,3330U},
  {2U,283U,612U,3331U},
  {2U,360U,552U,3332U},
  {2U,495U,183U,3333U},
  {2U,2U,751U,3334U},
  {2U,271U,914U,3335U},
  {2U,455U,696U,3336U},
  {2U,313U,221U,3337U},
  {2U,431U,386U,3338U},
  {2U,224U,606U,3339U},
  {2U,167U,186U,3340U},
  {2U,396U,271U,3341U},
  {2U,147U,387U,3342U},
  {2U,64U,796U,3343U},
  {2U,205U,564U,3344U},
  {2U,420U,89U,3345U},
  {2U,317U,265U,3346U},
  {2U,74U,540U,3347U},
  {2U,496U,161U,3348U},
  {2U,27U,8U,3349U},
  {2U,126U,807U,3350U},
  {2U,375U,608U,3351U},
  {2U,62U,940U,3352U},
  {2U,40U,923U,3353U},
  {2U,469U,283U,3354U},
  {2U,89U,406U,3355U},
  {2U,443U,462U,3356U},
  {2U,450U,823U,3357U},
  {2U,20U,687U,3358U},
  {2U,134U,331U,3359U},
  {2U,497U,525U,3360U},
  {2U,230U,940U,3361U},
  {2U,169U,84U,3362U},
  {2U,366U,857U,3363U},
  {2U,330U,573U,3364U},
  {2U,95U,232U,3365U},
  {2U,329U,129U,3366U},
  {2U,501U,148U,3367U},
  {2U,340U,219U,3368U},
  {2U,444U,421U,3369U},
  {2U,454U,806U,3370U},
  {2U,52U,997U,3371U},
  {2U,218U,895U,3372U},
  {2U,364U,891U,3373U},
  {2U,445U,965U,3374U},
  {2U,0U,982U,3375U},
  {2U,432U,747U,3376U},
  {2U,108U,521U,3377U},
  {2U,370U,330U,3378U},
  {2U,510U,150U,3379U},
  {2U,347U,919U,3380U},
  {2U,219U,260U,3381U},
  {2U,318U,141U,3382U},
  {2U,400U,109U,3383U},
  {2U,143U,210U,3384U},
  {2U,379U,67U,3385U},
  {2U,234U,578U,3386U},
  {2U,222U,452U,3387U},
  {2U,500U,842U,3388U},
  {2U,259U,756U,3389U},
  {2U,131U,778U,3390U},
  {2U,43U,474U,3391U},
  {2U,228U,124U,3392U},
  {2U,156U,974U,3393U},
  {2U,208U,989U,3394U},
  {2U,128U,809U,3395U},
  {2U,108U,338U,3396U},
  {2U,309U,393U,3397U},
  {2U,236U,892U,3398U},
  {2U,83U,805U,3399U},
  {2U,436U,827U,3400U},
  {2U,78U,76U,3401U},
  {2U,301U,902U,3402U},
  {2U,114U,927U,3403U},
  {2U,75U,593U,3404U},
  {2U,474U,530U,3405U},
  {2U,72U,809U,3406U},
  {2U,167U,837U,3407U},
  {2U,391U,892U,3408U},
  {2U,28U,275U,3409U},
  {2U,170U,308U,3410U},
  {2U,404U,585U,3411U},
  {2U,103U,549U,3412U},
  {2U,471U,174U,3413U},
  {2U,399U,225U,3414U},
  {2U,494U,871U,3415U},
  {2U,272U,63U,3416U},
  {2U,27U,735U,3417U},
  {2U,17U,753U,3418U},
  {2U,278U,575U,3419U},
  {2U,502U,904U,3420U},
  {2U,374U,825U,3421U},
  {2U,246U,872U,3422U},
  {2U,78U,34U,3423U},
  {2U,222U,664U,3424U},
  {2U,141U,936U,3425U},
  {2U,102U,349U,3426U},
  {2U,464U,154U,3427U},
  {2U,464U,961U,3428U},
  {2U,209U,723U,3429U},
  {2U,352U,587U,3430U},
  {2U,63U,784U,3431U},
  {2U,394U,80U,3432U},
  {2U,235U,215U,3433U},
  {2U,478U,543U,3434U},
  {2U,7U,524U,3435U},
  {2U,124U,168U,3436U},
  {2U,482U,287U,3437U},
  {2U,152U,175U,3438U},
  {2U,407U,491U,3439U},
  {2U,59U,857U,3440U},
  {2U,44U,586U,3441U},
  {2U,98U,118U,3442U},
  {2U,150U,596U,3443U},
  {2U,244U,67U,3444U},
  {2U,244U,761U,3445U},
  {2U,164U,706U,3446U},
  {2U,192U,867U,3447U},
  {2U,368U,515U,3448U},
  {2U,124U,722U,3449U},
  {2U,308U,519U,3450U},
  {2U,158U,531U,3451U},
  {2U,337U,169U,3452U},
  {2U,147U,895U,3453U},
  {2U,137U,23U,3454U},
  {2U,470U,411U,3455U},
  {2U,438U,530U,3456U},
  {2U,26U,298U,3457U},
  {2U,435U,730U,3458U},
  {2U,327U,460U,3459U},
  {2U,197U,377U,3460U},
  {2U,38U,612U,3461U},
  {2U,380U,962U,3462U},
  {2U,271U,837U,3463U},
  {2U,386U,95U,3464U},
  {2U,278U,224U,3465U},
  {2U,325U,116U,3466U},
  {2U,201U,872U,3467U},
  {2U,467U,335U,3468U},
  {2U,341U,635U,3469U},
  {2U,46U,570U,3470U},
  {2U,156U,397U,3471U},
  {2U,54U,568U,3472U},
  {2U,2U,785U,3473U},
  {2U,91U,67U,3474U},
  {2U,287U,663U,3475U},
  {2U,332U,958U,3476U},
  {2U,361U,718U,3477U},
  {2U,44U,23U,3478U},
  {2U,391U,761U,3479U},
  {2U,54U,558U,3480U},
  {2U,219U,338U,3481U},
  {2U,289U,861U,3482U},
  {2U,136U,323U,3483U},
  {2U,317U,869U,3484U},
  {2U,489U,926U,3485U},
  {2U,504U,445U,3486U},
  {2U,10U,688U,3487U},
  {2U,50U,169U,3488U},
  {2U,322U,383U,3489U},
  {2U,389U,576U,3490U},
  {2U,365U,596U,3491U},
  {2U,431U,389U,3492U},
  {2U,464U,402U,3493U},
  {2U,44U,897U,3494U},
  {2U,99U,32U,3495U},
  {2U,397U,253U,3496U},
  {2U,397U,385U,3497U},
  {2U,70U,48U,3498U},
  {2U,116U,752U,3499U},
  {2U,87U,491U,3500U},
  {2U,173U,58U,3501U},
  {2U,306U,874U,3502U},
  {2U,206U,573U,3503U},
  {2U,21U,324U,3504U},
  {2U,464U,741U,3505U},
  {2U,469U,481U,3506U},
  {2U,228U,948U,3507U},
  {2U,353U,108U,3508U},
  {2U,407U,335U,3509U},
  {2U,30U,360U,3510U},
  {2U,73U,582U,3511U},
  {2U,339U,398U,3512U},
  {2U,129U,630U,3513U},
  {2U,434U,779U,3514U},
  {2U,164U,781U,3515U},
  {2U,480U,854U,3516U},
  {2U,387U,91U,3517U},
  {2U,85U,399U,3518U},
  {2U,397U,942U,3519U},
  {2U,39U,563U,3520U},
  {2U,449U,218U,3521U},
  {2U,93U,392U,3522U},
  {2U,93U,786U,3523U},
  {2U,106U,150U,3524U},
  {2U,383U,835U,3525U},
  {2U,435U,655U,3526U},
  {2U,376U,747U,3527U},
  {2U,153U,80U,3528U},
  {2U,173U,525U,3529U},
  {2U,359U,622U,3530U},
  {2U,168U,33U,3531U},
  {2U,200U,972U,3532U},
  {2U,208U,678U,3533U},
  {2U,5U,247U,3534U},
  {2U,487U,801U,3535U},
  {2U,6U,818U,3536U},
  {2U,95U,539U,3537U},
  {2U,145U,491U,3538U},
  {2U,420U,923U,3539U},
  {2U,382U,852U,3540U},
  {2U,190U,468U,3541U},
  {2U,95U,959U,3542U},
  {2U,171U,403U,3543U},
  {2U,436U,893U,3544U},
  {2U,501U,699U,3545U},
  {2U,56U,942U,3546U},
  {2U,312U,906U,3547U},
  {2U,243U,819U,3548U},
  {2U,137U,832U,3549U},
  {2U,95U,37U,3550U},
  {2U,251U,892U,3551U},
  {2U,103U,405U,3552U},
  {2U,468U,388U,3553U},
  {2U,275U,768U,3554U},
  {2U,226U,759U,3555U},
  {2U,25U,402U,3556U},
  {2U,120U,25U,3557U},
  {2U,377U,399U,3558U},
  {2U,247U,689U,3559U},
  {2U,298U,254U,3560U},
  {2U,58U,921U,3561U},
  {2U,385U,990U,3562U},
  {2U,477U,450U,3563U},
  {2U,138U,421U,3564U},
  {2U,216U,738U,3565U},
  {2U,318U,219U,3566U},
  {2U,55U,652U,3567U},
  {2U,427U,962U,3568U},
  {2U,263U,222U,3569U},
  {2U,63U,769U,3570U},
  {2U,7U,110U,3571U},
  {2U,434U,970U,3572U},
  {2U,253U,79U,3573U},
  {2U,169U,741U,3574U},
  {2U,360U,892U,3575U},
  {2U,414U,748U,3576U},
  {2U,132U,752U,3577U},
  {2U,60U,668U,3578U},
  {2U,372U,657U,3579U},
  {2U,422U,795U,3580U},
  {2U,386U,448U,3581U},
  {2U,410U,703U,3582U},
  {2U,425U,399U,3583U},
  {2U,102U,92U,3584U},
  {2U,133U,498U,3585U},
  {2U,30U,668U,3586U},
  {2U,111U,578U,3587U},
  {2U,261U,517U,3588U},
  {2U,210U,745U,3589U},
  {2U,379U,599U,3590U},
  {2U,238U,980U,3591U},
  {2U,374U,250U,3592U},
  {2U,311U,751U,3593U},
  {2U,510U,83U,3594U},
  {2U,247U,969U,3595U},
  {2U,69U,415U,3596U},
  {2U,61U,792U,3597U},
  {2U,397U,898U,3598U},
  {2U,460U,575U,3599U},
  {2U,25U,535U,3600U},
  {2U,341U,8U,3601U},
  {2U,5U,225U,3602U},
  {2U,427U,102U,3603U},
  {2U,261U,794U,3604U},
  {2U,378U,225U,3605U},
  {2U,229U,203U,3606U},
  {2U,492U,909U,3607U},
  {2U,37U,709U,3608U},
  {2U,192U,862U,3609U},
  {2U,70U,705U,3610U},
  {2U,80U,456U,3611U},
  {2U,289U,546U,3612U},
  {2U,216U,201U,3613U},
  {2U,385U,902U,3614U},
  {2U,279U,874U,3615U},
  {2U,195U,157U,3616U},
  {2U,511U,238U,3617U},
  {2U,161U,185U,3618U},
  {2U,19U,777U,3619U},
  {2U,491U,506U,3620U},
  {2U,468U,832U,3621U},
  {2U,425U,411U,3622U},
  {2U,211U,634U,3623U},
  {2U,320U,916U,3624U},
  {2U,191U,553U,3625U},
  {2U,164U,564U,3626U},
  {2U,332U,19U,3627U},
  {2U,199U,609U,3628U},
  {2U,41U,562U,3629U},
  {2U,238U,609U,3630U},
  {2U,404U,332U,3631U},
  {2U,481U,158U,3632U},
  {2U,219U,212U,3633U},
  {2U,392U,162U,3634U},
  {2U,485U,614U,3635U},
  {2U,292U,247U,3636U},
  {2U,376U,638U,3637U},
  {2U,21U,239U,3638U},
  {2U,172U,370U,3639U},
  {2U,145U,859U,3640U},
  {2U,145U,526U,3641U},
  {2U,384U,548U,3642U},
  {2U,145U,422U,3643U},
  {2U,31U,995U,3644U},
  {2U,139U,21U,3645U},
  {2U,70U,23U,3646U},
  {2U,44U,259U,3647U},
  {2U,7U,624U,3648U},
  {2U,390U,151U,3649U},
  {2U,482U,377U,3650U},
  {2U,298U,414U,3651U},
  {2U,273U,126U,3652U},
  {2U,103U,519U,3653U},
  {2U,69U,184U,3654U},
  {2U,242U,890U,3655U},
  {2U,187U,357U,3656U},
  {2U,289U,909U,3657U},
  {2U,29U,688U,3658U},
  {2U,83U,759U,3659U},
  {2U,65U,836U,3660U},
  {2U,369U,194U,3661U},
  {2U,481U,339U,3662U},
  {2U,333U,154U,3663U},
  {2U,284U,529U,3664U},
  {2U,292U,174U,3665U},
  {2U,182U,80U,3666U},
  {2U,160U,330U,3667U},
  {2U,260U,81U,3668U},
  {2U,255U,903U,3669U},
  {2U,300U,888U,3670U},
  {2U,427U,434U,3671U},
  {2U,509U,129U,3672U},
  {2U,296U,150U,3673U},
  {2U,381U,408U,3674U},
  {2U,474U,456U,3675U},
  {2U,366U,689U,3676U},
  {2U,61U,511U,3677U},
  {2U,258U,168U,3678U},
  {2U,239U,419U,3679U},
  {2U,114U,522U,3680U},
  {2U,476U,862U,3681U},
  {2U,126U,707U,3682U},
  {2U,59U,923U,3683U},
  {2U,22U,564U,3684U},
  {2U,73U,336U,3685U},
  {2U,427U,429U,3686U},
  {2U,281U,918U,3687U},
  {2U,47U,615U,3688U},
  {2U,265U,726U,3689U},
  {2U,226U,527U,3690U},
  {2U,234U,432U,3691U},
  {2U,355U,422U,3692U},
  {2U,256U,761U,3693U},
  {2U,418U,58U,3694U},
  {2U,255U,226U,3695U},
  {2U,23U,616U,3696U},
  {2U,143U,879U,3697U},
  {2U,388U,659U,3698U},
  {2U,28U,245U,3699U},
  {2U,232U,538U,3700U},
  {2U,510U,419U,3701U},
  {2U,354U,301U,3702U},
  {2U,258U,484U,3703U},
  {2U,487U,153U,3704U},
  {2U,162U,339U,3705U},
  {2U,502U,270U,3706U},
  {2U,202U,709U,3707U},
  {2U,249U,287U,3708U},
  {2U,396U,884U,3709U},
  {2U,451U,112U,3710U},
  {2U,384U,308U,3711U},
  {2U,321U,743U,3712U},
  {2U,120U,225U,3713U},
  {2U,398U,343U,3714U},
  {2U,452U,334U,3715U},
  {2U,139U,435U,3716U},
  {2U,312U,974U,3717U},
  {2U,300U,814U,3718U},
  {2U,343U,446U,3719U},
  {2U,381U,254U,3720U},
  {2U,4U,73U,3721U},
  {2U,391U,965U,3722U},
  {2U,178U,410U,3723U},
  {2U,118U,884U,3724U},
  {2U,176U,899U,3725U},
  {2U,105U,772U,3726U},
  {2U,490U,169U,3727U},
  {2U,101U,638U,3728U},
  {2U,207U,252U,3729U},
  {2U,129U,802U,3730U},
  {2U,347U,438U,3731U},
  {2U,291U,978U,3732U},
  {2U,406U,914U,3733U},
  {2U,378U,278U,3734U},
  {2U,279U,956U,3735U},
  {2U,76U,392U,3736U},
  {2U,231U,637U,3737U},
  {2U,367U,626U,3738U},
  {2U,286U,846U,3739U},
  {2U,332U,109U,3740U},
  {2U,145U,73U,3741U},
  {2U,469U,59U,3742U},
  {2U,82U,70U,3743U},
  {2U,54U,833U,3744U},
  {2U,424U,708U,3745U},
  {2U,258U,396U,3746U},
  {2U,450U,700U,3747U},
  {2U,367U,681U,3748U},
  {2U,499U,969U,3749U},
  {2U,258U,753U,3750U},
  {2U,25U,974U,3751U},
  {2U,452U,711U,3752U},
  {2U,114U,269U,3753U},
  {2U,26U,427U,3754U},
  {2U,198U,766U,3755U},
  {2U,387U,838U,3756U},
  {2U,44U,558U,3757U},
  {2U,129U,504U,3758U},
  {2U,334U,746U,3759U},
  {2U,233U,605U,3760U},
  {2U,274U,846U,3761U},
  {2U,348U,631U,3762U},
  {2U,204U,643U,3763U},
  {2U,498U,838U,3764U},
  {2U,390U,910U,3765U},
  {2U,481U,887U,3766U},
  {2U,293U,622U,3767U},
  {2U,69U,590U,3768U},
  {2U,458U,262U,3769U},
  {2U,229U,473U,3770U},
  {2U,247U,737U,3771U},
  {2U,1U,573U,3772U},
  {2U,391U,391U,3773U},
  {2U,508U,974U,3774U},
  {2U,464U,232U,3775U},
  {2U,474U,648U,3776U},
  {2U,494U,235U,3777U},
  {2U,43U,905U,3778U},
  {2U,166U,910U,3779U},
  {2U,194U,519U,3780U},
  {2U,287U,95U,3781U},
  {2U,50U,181U,3782U},
  {2U,493U,410U,3783U},
  {2U,363U,825U,3784U},
  {2U,141U,934U,3785U},
  {2U,159U,238U,3786U},
  {2U,452U,708U,3787U},
  {2U,191U,905U,3788U},
  {2U,169U,392U,3789U},
  {2U,49U,431U,3790U},
  {2U,24U,541U,3791U},
  {2U,186U,809U,3792U},
  {2U,223U,520U,3793U},
  {2U,343U,48U,3794U},
  {2U,320U,827U,3795U},
  {2U,124U,321U,3796U},
  {2U,17U,585U,3797U},
  {2U,13U,202U,3798U},
  {2U,497U,907U,3799U},
  {2U,166U,734U,3800U},
  {2U,212U,856U,3801U},
  {2U,255U,269U,3802U},
  {2U,416U,514U,3803U},
  {2U,131U,944U,3804U},
  {2U,17U,275U,3805U},
  {2U,80U,589U,3806U},
  {2U,332U,407U,3807U},
  {2U,433U,985U,3808U},
  {2U,66U,244U,3809U},
  {2U,103U,522U,3810U},
  {2U,205U,11U,3811U},
  {2U,264U,493U,3812U},
  {2U,290U,968U,3813U},
  {2U,158U,88U,3814U},
  {2U,304U,174U,3815U},
  {2U,188U,969U,3816U},
  {2U,111U,219U,3817U},
  {2U,415U,638U,3818U},
  {2U,480U,797U,3819U},
  {2U,325U,624U,3820U},
  {2U,230U,730U,3821U},
  {2U,148U,211U,3822U},
  {2U,60U,375U,3823U},
  {2U,379U,109U,3824U},
  {2U,316U,154U,3825U},
  {2U,7U,122U,3826U},
  {2U,286U,148U,3827U},
  {2U,279U,121U,3828U},
  {2U,103U,924U,3829U},
  {2U,470U,130U,3830U},
  {2U,416U,969U,3831U},
  {2U,308U,113U,3832U},
  {2U,348U,457U,3833U},
  {2U,98U,168U,3834U},
  {2U,117U,456U,3835U},
  {2U,261U,178U,3836U},
  {2U,278U,785U,3837U},
  {2U,403U,658U,3838U},
  {2U,332U,783U,3839U},
  {2U,230U,147U,3840U},
  {2U,253U,947U,3841U},
  {2U,234U,622U,3842U},
  {2U,193U,18U,3843U},
  {2U,499U,574U,3844U},
  {2U,148U,765U,3845U},
  {2U,413U,717U,3846U},
  {2U,248U,320U,3847U},
  {2U,452U,549U,3848U},
  {2U,447U,263U,3849U},
  {2U,40U,162U,3850U},
  {2U,56U,834U,3851U},
  {2U,453U,547U,3852U},
  {2U,464U,566U,3853U},
  {2U,65U,964U,3854U},
  {2U,485U,987U,3855U},
  {2U,347U,226U,3856U},
  {2U,465U,638U,3857U},
  {2U,111U,942U,3858U},
  {2U,235U,925U,3859U},
  {2U,211U,994U,3860U},
  {2U,431U,66U,3861U},
  {2U,501U,500U,3862U},
  {2U,10U,549U,3863U},
  {2U,260U,65U,3864U},
  {2U,249U,184U,3865U},
  {2U,498U,64U,3866U},
  {2U,355U,551U,3867U},
  {2U,35U,939U,3868U},
  {2U,338U,857U,3869U},
  {2U,372U,604U,3870U},
  {2U,268U,674U,3871U},
  {2U,471U,577U,3872U},
  {2U,208U,817U,3873U},
  {2U,445U,718U,3874U},
  {2U,497U,547U,3875U},
  {2U,502U,66U,3876U},
  {2U,315U,623U,3877U},
  {2U,260U,778U,3878U},
  {2U,300U,162U,3879U},
  {2U,44U,776U,3880U},
  {2U,428U,890U,3881U},
  {2U,378U,726U,3882U},
  {2U,51U,251U,3883U},
  {2U,201U,409U,3884U},
  {2U,119U,188U,3885U},
  {2U,501U,352U,3886U},
  {2U,92U,33U,3887U},
  {2U,256U,506U,3888U},
  {2U,390U,888U,3889U},
  {2U,225U,322U,3890U},
  {2U,277U,674U,3891U},
  {2U,262U,869U,3892U},
  {2U,209U,267U,3893U},
  {2U,163U,10U,3894U},
  {2U,248U,420U,3895U},
  {2U,231U,106U,3896U},
  {2U,486U,488U,3897U},
  {2U,201U,463U,3898U},
  {2U,347U,695U,3899U},
  {2U,342U,631U,3900U},
  {2U,280U,611U,3901U},
  {2U,145U,715U,3902U},
  {2U,62U,757U,3903U},
  {2U,237U,517U,3904U},
  {2U,266U,655U,3905U},
  {2U,465U,671U,3906U},
  {2U,43U,820U,3907U},
  {2U,448U,666U,3908U},
  {2U,343U,756U,3909U},
  {2U,33U,586U,3910U},
  {2U,461U,145U,3911U},
  {2U,511U,295U,3912U},
  {2U,379U,908U,3913U},
  {2U,268U,747U,3914U},
  {2U,495U,612U,3915U},
  {2U,169U,508U,3916U},
  {2U,252U,512U,3917U},
  {2U,332U,947U,3918U},
  {2U,22U,387U,3919U},
  {2U,62U,934U,3920U},
  {2U,339U,660U,3921U},
  {2U,340U,252U,3922U},
  {2U,399U,913U,3923U},
  {2U,343U,32U,3924U},
  {2U,496U,413U,3925U},
  {2U,27U,998U,3926U},
  {2U,479U,406U,3927U},
  {2U,12U,973U,3928U},
  {2U,113U,497U,3929U},
  {2U,180U,401U,3930U},
  {2U,157U,669U,3931U},
  {2U,331U,982U,3932U},
  {2U,209U,388U,3933U},
  {2U,75U,106U,3934U},
  {2U,215U,347U,3935U},
  {2U,302U,580U,3936U},
  {2U,229U,155U,3937U},
  {2U,175U,577U,3938U},
  {2U,431U,744U,3939U},
  {2U,210U,446U,3940U},
  {2U,34U,374U,3941U},
  {2U,319U,446U,3942U},
  {2U,203U,212U,3943U},
  {2U,104U,555U,3944U},
  {2U,310U,525U,3945U},
  {2U,32U,180U,3946U},
  {2U,227U,680U,3947U},
  {2U,324U,858U,3948U},
  {2U,301U,988U,3949U},
  {2U,324U,837U,3950U},
  {2U,240U,106U,3951U},
  {2U,507U,344U,3952U},
  {2U,406U,22U,3953U},
  {2U,356U,959U,3954U},
  {2U,179U,690U,3955U},
  {2U,390U,322U,3956U},
  {2U,177U,244U,3957U},
  {2U,52U,416U,3958U},
  {2U,503U,95U,3959U},
  {2U,272U,667U,3960U},
  {2U,459U,570U,3961U},
  {2U,454U,511U,3962U},
  {2U,465U,702U,3963U},
  {2U,37U,835U,3964U},
  {2U,74U,665U,3965U},
  {2U,440U,259U,3966U},
  {2U,92U,137U,3967U},
  {2U,95U,664U,3968U},
  {2U,66U,682U,3969U},
  {2U,144U,453U,3970U},
  {2U,174U,472U,3971U},
  {2U,509U,511U,3972U},
  {2U,459U,839U,3973U},
  {2U,279U,712U,3974U},
  {2U,335U,630U,3975U},
  {2U,389U,140U,3976U},
  {2U,431U,318U,3977U},
  {2U,125U,390U,3978U},
  {2U,452U,375U,3979U},
  {2U,308U,41U,3980U},
  {2U,249U,395U,3981U},
  {2U,342U,573U,3982U},
  {2U,43U,11U,3983U},
  {2U,207U,467U,3984U},
  {2U,201U,123U,3985U},
  {2U,112U,869U,3986U},
  {2U,124U,252U,3987U},
  {2U,126U,671U,3988U},
  {2U,432U,533U,3989U},
  {2U,225U,363U,3990U},
  {2U,456U,224U,3991U},
  {2U,509U,927U,3992U},
  {2U,125U,682U,3993U},
  {2U,233U,675U,3994U},
  {2U,491U,152U,3995U},
  {2U,370U,166U,3996U},
  {2U,370U,158U,3997U},
  {2U,84U,679U,3998U},
  {2U,389U,325U,3999U},
  {2U,57U,789U,4000U},
  {2U,395U,135U,4001U},
  {2U,373U,78U,4002U},
  {2U,226U,645U,4003U},
  {2U,203U,148U,4004U},
  {2U,414U,178U,4005U},
  {2U,251U,562U,4006U},
  {2U,17U,614U,4007U},
  {2U,98U,672U,4008U},
  {2U,327U,959U,4009U},
  {2U,403U,756U,4010U},
  {2U,282U,571U,4011U},
  {2U,208U,851U,4012U},
  {2U,259U,391U,4013U},
  {2U,336U,447U,4014U},
  {2U,268U,213U,4015U},
  {2U,387U,806U,4016U},
  {2U,343U,65U,4017U},
  {2U,363U,497U,4018U},
  {2U,56U,24U,4019U},
  {2U,38U,207U,4020U},
  {2U,270U,133U,4021U},
  {2U,288U,101U,4022U},
  {2U,309U,477U,4023U},
  {2U,103U,242U,4024U},
  {2U,412U,546U,4025U},
  {2U,484U,780U,4026U},
  {2U,472U,86U,4027U},
  {2U,436U,340U,4028U},
  {2U,471U,506U,4029U},
  {2U,175U,629U,4030U},
  {2U,246U,934U,4031U},
  {2U,184U,599U,4032U},
  {2U,334U,695U,4033U},
  {2U,471U,422U,4034U},
  {2U,8U,834U,4035U},
  {2U,131U,641U,4036U},
  {2U,122U,212U,4037U},
  {2U,140U,643U,4038U},
  {2U,471U,787U,4039U},
  {2U,26U,722U,4040U},
  {2U,47U,284U,4041U},
  {2U,326U,713U,4042U},
  {2U,479U,358U,4043U},
  {2U,205U,410U,4044U},
  {2U,455U,285U,4045U},
  {2U,75U,427U,4046U},
  {2U,151U,479U,4047U},
  {2U,430U,534U,4048U},
  {2U,229U,461U,4049U},
  {2U,427U,375U,4050U},
  {2U,178U,320U,4051U},
  {2U,267U,29U,4052U},
  {2U,74U,517U,4053U},
  {2U,315U,811U,4054U},
  {2U,184U,859U,4055U},
  {2U,298U,715U,4056U},
  {2U,386U,537U,4057U},
  {2U,227U,760U,4058U},
  {2U,336U,645U,4059U},
  {2U,40U,222U,4060U},
  {2U,41U,813U,4061U},
  {2U,19U,316U,4062U},
  {2U,109U,431U,4063U},
  {2U,278U,745U,4064U},
  {2U,411U,306U,4065U},
  {2U,261U,833U,4066U},
  {2U,65U,859U,4067U},
  {2U,460U,24U,4068U},
  {2U,381U,296U,4069U},
  {2U,319U,531U,4070U},
  {2U,369U,352U,4071U},
  {2U,414U,55U,4072U},
  {2U,483U,689U,4073U},
  {2U,380U,594U,4074U},
  {2U,227U,218U,4075U},
  {2U,167U,498U,4076U},
  {2U,73U,334U,4077U},
  {2U,435U,782U,4078U},
  {2U,103U,540U,4079U},
  {2U,337U,437U,4080U},
  {2U,346U,519U,4081U},
  {2U,210U,44U,4082U},
  {2U,47U,398U,4083U},
  {2U,391U,438U,4084U},
  {2U,417U,428U,4085U},
  {2U,361U,669U,4086U},
  {2U,468U,137U,4087U},
  {2U,418U,74U,4088U},
  {2U,233U,184U,4089U},
  {2U,399U,563U,4090U},
  {2U,121U,482U,4091U},
  {2U,503U,43U,4092U},
  {2U,344U,419U,4093U},
  {2U,11U,452U,4094U},
  {2U,415U,934U,4095U},
  {2U,370U,55U,4096U},
  {2U,242U,654U,4097U},
  {2U,162U,33U,4098U},
  {2U,108U,793U,4099U},
  {2U,259U,733U,4100U},
  {2U,300U,166U,4101U},
  {2U,224U,383U,4102U},
  {2U,4U,849U,4103U},
  {2U,174U,401U,4104U},
  {2U,50U,883U,4105U},
  {2U,425U,276U,4106U},
  {2U,195U,849U,4107U},
  {2U,261U,103U,4108U},
  {2U,236U,246U,4109U},
  {2U,71U,443U,4110U},
  {2U,227U,923U,4111U},
  {2U,287U,53U,4112U},
  {2U,189U,577U,4113U},
  {2U,74U,314U,4114U},
  {2U,192U,22U,4115U},
  {2U,450U,344U,4116U},
  {2U,35U,818U,4117U},
  {2U,405U,430U,4118U},
  {2U,110U,704U,4119U},
  {2U,371U,91U,4120U},
  {2U,442U,790U,4121U},
  {2U,11U,133U,4122U},
  {2U,358U,237U,4123U},
  {2U,10U,294U,4124U},
  {2U,341U,493U,4125U},
  {2U,478U,200U,4126U},
  {2U,398U,274U,4127U},
  {2U,254U,461U,4128U},
  {2U,20U,443U,4129U},
  {2U,30U,865U,4130U},
  {2U,102U,795U,4131U},
  {2U,163U,872U,4132U},
  {2U,232U,388U,4133U},
  {2U,418U,507U,4134U},
  {2U,140U,300U,4135U},
  {2U,453U,127U,4136U},
  {2U,463U,52U,4137U},
  {2U,252U,851U,4138U},
  {2U,413U,321U,4139U},
  {2U,35U,751U,4140U},
  {2U,83U,884U,4141U},
  {2U,485U,356U,4142U},
  {2U,431U,100U,4143U},
  {2U,115U,62U,4144U},
  {2U,144U,187U,4145U},
  {2U,332U,738U,4146U},
  {2U,28U,835U,4147U},
  {2U,309U,277U,4148U},
  {2U,0U,510U,4149U},
  {2U,167U,566U,4150U},
  {2U,510U,637U,4151U},
  {2U,291U,781U,4152U},
  {2U,472U,415U,4153U},
  {2U,204U,225U,4154U},
  {2U,335U,959U,4155U},
  {2U,392U,574U,4156U},
  {2U,452U,227U,4157U},
  {2U,245U,360U,4158U},
  {2U,71U,91U,4159U},
  {2U,221U,789U,4160U},
  {2U,469U,111U,4161U},
  {2U,65U,904U,4162U},
  {2U,438U,17U,4163U},
  {2U,54U,838U,4164U},
  {2U,304U,815U,4165U},
  {2U,441U,607U,4166U},
  {2U,259U,805U,4167U},
  {2U,322U,714U,4168U},
  {2U,3U,833U,4169U},
  {2U,199U,351U,4170U},
  {2U,486U,50U,4171U},
  {2U,389U,286U,4172U},
  {2U,432U,728U,4173U},
  {2U,217U,446U,4174U},
  {2U,91U,366U,4175U},
  {2U,70U,195U,4176U},
  {2U,312U,653U,4177U},
  {2U,465U,195U,4178U},
  {2U,408U,937U,4179U},
  {2U,371U,470U,4180U},
  {2U,188U,253U,4181U},
  {2U,38U,468U,4182U},
  {2U,82U,427U,4183U},
  {2U,398U,895U,4184U},
  {2U,370U,825U,4185U},
  {2U,50U,661U,4186U},
  {2U,259U,49U,4187U},
  {2U,301U,821U,4188U},
  {2U,242U,80U,4189U},
  {2U,313U,537U,4190U},
  {2U,221U,991U,4191U},
  {2U,482U,658U,4192U},
  {2U,323U,398U,4193U},
  {2U,354U,981U,4194U},
  {2U,446U,557U,4195U},
  {2U,478U,312U,4196U},
  {2U,367U,131U,4197U},
  {2U,179U,394U,4198U},
  {2U,426U,915U,4199U},
  {2U,469U,700U,4200U},
  {2U,356U,227U,4201U},
  {2U,325U,820U,4202U},
  {2U,138U,711U,4203U},
  {2U,303U,899U,4204U},
  {2U,150U,390U,4205U},
  {2U,331U,980U,4206U},
  {2U,73U,140U,4207U},
  {2U,70U,617U,4208U},
  {2U,138U,862U,4209U},
  {2U,410U,787U,4210U},
  {2U,454U,25U,4211U},
  {2U,142U,298U,4212U},
  {2U,94U,360U,4213U},
  {2U,255U,392U,4214U},
  {2U,181U,866U,4215U},
  {2U,327U,502U,4216U},
  {2U,288U,26U,4217U},
  {2U,412U,765U,4218U},
  {2U,466U,86U,4219U},
  {2U,141U,481U,4220U},
  {2U,87U,713U,4221U},
  {2U,344U,620U,4222U},
  {2U,32U,12U,4223U},
  {2U,191U,972U,4224U},
  {2U,59U,39U,4225U},
  {2U,231U,424U,4226U},
  {2U,23U,797U,4227U},
  {2U,510U,496U,4228U},
  {2U,16U,242U,4229U},
  {2U,446U,987U,4230U},
  {2U,334U,640U,4231U},
  {2U,95U,827U,4232U},
  {2U,278U,891U,4233U},
  {2U,159U,596U,4234U},
  {2U,275U,511U,4235U},
  {2U,263U,856U,4236U},
  {2U,407U,512U,4237U},
  {2U,310U,229U,4238U},
  {2U,313U,223U,4239U},
  {2U,455U,644U,4240U},
  {2U,446U,421U,4241U},
  {2U,503U,607U,4242U},
  {2U,489U,366U,4243U},
  {2U,386U,405U,4244U},
  {2U,375U,824U,4245U},
  {2U,242U,266U,4246U},
  {2U,492U,30U,4247U},
  {2U,24U,462U,4248U},
  {2U,17U,644U,4249U},
  {2U,30U,805U,4250U},
  {2U,2U,179U,4251U},
  {2U,184U,491U,4252U},
  {2U,426U,435U,4253U},
  {2U,329U,841U,4254U},
  {2U,405U,88U,4255U},
  {2U,283U,949U,4256U},
  {2U,423U,477U,4257U},
  {2U,42U,280U,4258U},
  {2U,355U,593U,4259U},
  {2U,31U,415U,4260U},
  {2U,146U,793U,4261U},
  {2U,239U,231U,4262U},
  {2U,349U,595U,4263U},
  {2U,371U,521U,4264U},
  {2U,241U,346U,4265U},
  {2U,168U,29U,4266U},
  {2U,438U,165U,4267U},
  {2U,288U,181U,4268U},
  {2U,323U,860U,4269U},
  {2U,405U,995U,4270U},
  {2U,497U,298U,4271U},
  {2U,44U,39U,4272U},
  {2U,199U,269U,4273U},
  {2U,261U,574U,4274U},
  {2U,145U,858U,4275U},
  {2U,406U,728U,4276U},
  {2U,109U,918U,4277U},
  {2U,80U,53U,4278U},
  {2U,479U,963U,4279U},
  {2U,90U,307U,4280U},
  {2U,234U,849U,4281U},
  {2U,8U,248U,4282U},
  {2U,80U,590U,4283U},
  {2U,435U,679U,4284U},
  {2U,304U,333U,4285U},
  {2U,274U,19U,4286U},
  {2U,296U,624U,4287U},
  {2U,425U,58U,4288U},
  {2U,17U,374U,4289U},
  {2U,437U,18U,4290U},
  {2U,183U,234U,4291U},
  {2U,330U,319U,4292U},
  {2U,41U,347U,4293U},
  {2U,24U,712U,4294U},
  {2U,369U,772U,4295U},
  {2U,421U,207U,4296U},
  {2U,251U,249U,4297U},
  {2U,95U,742U,4298U},
  {2U,121U,157U,4299U},
  {2U,316U,287U,4300U},
  {2U,478U,102U,4301U},
  {2U,134U,735U,4302U},
  {2U,374U,765U,4303U},
  {2U,485U,482U,4304U},
  {2U,160U,270U,4305U},
  {2U,78U,344U,4306U},
  {2U,42U,468U,4307U},
  {2U,502U,403U,4308U},
  {2U,446U,268U,4309U},
  {2U,352U,733U,4310U},
  {2U,25U,541U,4311U},
  {2U,27U,642U,4312U},
  {2U,90U,14U,4313U},
  {2U,329U,360U,4314U},
  {2U,38U,68U,4315U},
  {2U,154U,953U,4316U},
  {2U,272U,165U,4317U},
  {2U,381U,144U,4318U},
  {2U,509U,617U,4319U},
  {2U,245U,379U,4320U},
  {2U,54U,960U,4321U},
  {2U,386U,622U,4322U},
  {2U,203U,485U,4323U},
  {2U,245U,81U,4324U},
  {2U,459U,701U,4325U},
  {2U,295U,332U,4326U},
  {2U,277U,873U,4327U},
  {2U,160U,359U,4328U},
  {2U,401U,59U,4329U},
  {2U,212U,763U,4330U},
  {2U,88U,547U,4331U},
  {2U,4U,946U,4332U},
  {2U,279U,881U,4333U},
  {2U,383U,909U,4334U},
  {2U,52U,196U,4335U},
  {2U,503U,586U,4336U},
  {2U,336U,412U,4337U},
  {2U,256U,779U,4338U},
  {2U,398U,572U,4339U},
  {2U,89U,434U,4340U},
  {2U,404U,189U,4341U},
  {2U,509U,815U,4342U},
  {2U,94U,786U,4343U},
  {2U,57U,324U,4344U},
  {2U,436U,606U,4345U},
  {2U,163U,311U,4346U},
  {2U,90U,772U,4347U},
  {2U,70U,940U,4348U},
  {2U,210U,986U,4349U},
  {2U,119U,423U,4350U},
  {2U,402U,700U,4351U},
  {2U,118U,626U,4352U},
  {2U,203U,421U,4353U},
  {2U,449U,571U,4354U},
  {2U,494U,287U,4355U},
  {2U,199U,497U,4356U},
  {2U,175U,492U,4357U},
  {2U,55U,628U,4358U},
  {2U,426U,40U,4359U},
  {2U,269U,185U,4360U},
  {2U,402U,323U,4361U},
  {2U,315U,791U,4362U},
  {2U,357U,481U,4363U},
  {2U,72U,734U,4364U},
  {2U,429U,898U,4365U},
  {2U,482U,272U,4366U},
  {2U,242U,658U,4367U},
  {2U,297U,334U,4368U},
  {2U,497U,219U,4369U},
  {2U,42U,577U,4370U},
  {2U,310U,238U,4371U},
  {2U,428U,243U,4372U},
  {2U,276U,536U,4373U},
  {2U,291U,243U,4374U},
  {2U,429U,191U,4375U},
  {2U,198U,656U,4376U},
  {2U,257U,927U,4377U},
  {2U,103U,931U,4378U},
  {2U,278U,39U,4379U},
  {2U,104U,507U,4380U},
  {2U,329U,196U,4381U},
  {2U,237U,591U,4382U},
  {2U,147U,443U,4383U},
  {2U,86U,420U,4384U},
  {2U,124U,740U,4385U},
  {2U,414U,546U,4386U},
  {2U,170U,794U,4387U},
  {2U,260U,798U,4388U},
  {2U,151U,587U,4389U},
  {2U,208U,870U,4390U},
  {2U,78U,428U,4391U},
  {2U,170U,543U,4392U},
  {2U,511U,189U,4393U},
  {2U,398U,688U,4394U},
  {2U,219U,500U,4395U},
  {2U,96U,490U,4396U},
  {2U,242U,154U,4397U},
  {2U,57U,900U,4398U},
  {2U,251U,101U,4399U},
  {2U,469U,173U,4400U},
  {2U,452U,540U,4401U},
  {2U,316U,328U,4402U},
  {2U,355U,43U,4403U},
  {2U,435U,439U,4404U},
  {2U,438U,891U,4405U},
  {2U,456U,883U,4406U},
  {2U,76U,418U,4407U},
  {2U,83U,755U,4408U},
  {2U,380U,807U,4409U},
  {2U,479U,259U,4410U},
  {2U,447U,394U,4411U},
  {2U,202U,212U,4412U},
  {2U,67U,491U,4413U},
  {2U,73U,96U,4414U},
  {2U,343U,559U,4415U},
  {2U,408U,385U,4416U},
  {2U,193U,830U,4417U},
  {2U,419U,495U,4418U},
  {2U,295U,331U,4419U},
  {2U,468U,544U,4420U},
  {2U,372U,801U,4421U},
  {2U,294U,746U,4422U},
  {2U,176U,176U,4423U},
  {2U,95U,714U,4424U},
  {2U,214U,539U,4425U},
  {2U,268U,661U,4426U},
  {2U,456U,45U,4427U},
  {2U,247U,664U,4428U},
  {2U,73U,177U,4429U},
  {2U,72U,187U,4430U},
  {2U,227U,888U,4431U},
  {2U,409U,854U,4432U},
  {2U,161U,799U,4433U},
  {2U,227U,79U,4434U},
  {2U,510U,469U,4435U},
  {2U,319U,38U,4436U},
  {2U,364U,443U,4437U},
  {2U,64U,427U,4438U},
  {2U,110U,937U,4439U},
  {2U,309U,762U,4440U},
  {2U,106U,893U,4441U},
  {2U,31U,340U,4442U},
  {2U,493U,194U,4443U},
  {2U,290U,360U,4444U},
  {2U,30U,659U,4445U},
  {2U,122U,800U,4446U},
  {2U,479U,605U,4447U},
  {2U,129U,164U,4448U},
  {2U,147U,803U,4449U},
  {2U,192U,53U,4450U},
  {2U,274U,276U,4451U},
  {2U,177U,502U,4452U},
  {2U,276U,446U,4453U},
  {2U,239U,150U,4454U},
  {2U,32U,307U,4455U},
  {2U,396U,474U,4456U},
  {2U,163U,980U,4457U},
  {2U,221U,358U,4458U},
  {2U,257U,588U,4459U},
  {2U,236U,166U,4460U},
  {2U,62U,452U,4461U},
  {2U,387U,21U,4462U},
  {2U,189U,744U,4463U},
  {2U,403U,29U,4464U},
  {2U,485U,920U,4465U},
  {2U,452U,687U,4466U},
  {2U,125U,567U,4467U},
  {2U,446U,346U,4468U},
  {2U,470U,283U,4469U},
  {2U,452U,193U,4470U},
  {2U,88U,211U,4471U},
  {2U,303U,276U,4472U},
  {2U,399U,209U,4473U},
  {2U,98U,347U,4474U},
  {2U,289U,240U,4475U},
  {2U,477U,111U,4476U},
  {2U,240U,397U,4477U},
  {2U,339U,360U,4478U},
  {2U,79U,875U,4479U},
  {2U,29U,713U,4480U},
  {2U,19U,225U,4481U},
  {2U,391U,936U,4482U},
  {2U,473U,951U,4483U},
  {2U,373U,753U,4484U},
  {2U,234U,118U,4485U},
  {2U,507U,565U,4486U},
  {2U,208U,74U,4487U},
  {2U,283U,604U,4488U},
  {2U,413U,271U,4489U},
  {2U,19U,208U,4490U},
  {2U,469U,534U,4491U},
  {2U,448U,975U,4492U},
  {2U,40U,518U,4493U},
  {2U,345U,192U,4494U},
  {2U,390U,56U,4495U},
  {2U,499U,938U,4496U},
  {2U,412U,353U,4497U},
  {2U,346U,594U,4498U},
  {2U,463U,368U,4499U},
  {2U,442U,590U,4500U},
  {2U,140U,463U,4501U},
  {2U,107U,138U,4502U},
  {2U,363U,19U,4503U},
  {2U,238U,52U,4504U},
  {2U,195U,965U,4505U},
  {2U,214U,492U,4506U},
  {2U,399U,875U,4507U},
  {2U,360U,936U,4508U},
  {2U,79U,407U,4509U},
  {2U,79U,514U,4510U},
  {2U,38U,95U,4511U},
  {2U,357U,847U,4512U},
  {2U,389U,10U,4513U},
  {2U,349U,334U,4514U},
  {2U,489U,185U,4515U},
  {2U,22U,793U,4516U},
  {2U,4U,673U,4517U},
  {2U,189U,788U,4518U},
  {2U,227U,344U,4519U},
  {2U,161U,682U,4520U},
  {2U,386U,590U,4521U},
  {2U,376U,923U,4522U},
  {2U,435U,774U,4523U},
  {2U,214U,232U,4524U},
  {2U,407U,747U,4525U},
  {2U,223U,572U,4526U},
  {2U,22U,832U,4527U},
  {2U,89U,858U,4528U},
  {2U,30U,926U,4529U},
  {2U,398U,728U,4530U},
  {2U,441U,63U,4531U},
  {2U,279U,920U,4532U},
  {2U,271U,305U,4533U},
  {2U,482U,432U,4534U},
  {2U,219U,976U,4535U},
  {2U,104U,632U,4536U},
  {2U,448U,83U,4537U},
  {2U,1U,462U,4538U},
  {2U,176U,638U,4539U},
  {2U,30U,981U,4540U},
  {2U,397U,765U,4541U},
  {2U,190U,532U,4542U},
  {2U,177U,135U,4543U},
  {2U,223U,972U,4544U},
  {2U,388U,572U,4545U},
  {2U,302U,905U,4546U},
  {2U,29U,634U,4547U},
  {2U,316U,694U,4548U},
  {2U,82U,913U,4549U},
  {2U,65U,621U,4550U},
  {2U,501U,182U,4551U},
  {2U,141U,375U,4552U},
  {2U,203U,424U,4553U},
  {2U,419U,423U,4554U},
  {2U,221U,186U,4555U},
  {2U,460U,161U,4556U},
  {2U,151U,17U,4557U},
  {2U,453U,36U,4558U},
  {2U,17U,644U,4559U},
  {2U,263U,458U,4560U},
  {2U,271U,929U,4561U},
  {2U,175U,616U,4562U},
  {2U,388U,129U,4563U},
  {2U,94U,463U,4564U},
  {2U,357U,425U,4565U},
  {2U,356U,551U,4566U},
  {2U,208U,547U,4567U},
  {2U,180U,669U,4568U},
  {2U,274U,470U,4569U},
  {2U,318U,662U,4570U},
  {2U,404U,671U,4571U},
  {2U,351U,332U,4572U},
  {2U,155U,557U,4573U},
  {2U,220U,28U,4574U},
  {2U,214U,942U,4575U},
  {2U,423U,895U,4576U},
  {2U,230U,337U,4577U},
  {2U,195U,580U,4578U},
  {2U,344U,204U,4579U},
  {2U,469U,493U,4580U},
  {2U,257U,899U,4581U},
  {2U,206U,189U,4582U},
  {2U,118U,425U,4583U},
  {2U,103U,83U,4584U},
  {2U,403U,643U,4585U},
  {2U,372U,506U,4586U},
  {2U,389U,173U,4587U},
  {2U,179U,610U,4588U},
  {2U,56U,894U,4589U},
  {2U,25U,777U,4590U},
  {2U,416U,640U,4591U},
  {2U,303U,956U,4592U},
  {2U,99U,641U,4593U},
  {2U,329U,847U,4594U},
  {2U,510U,330U,4595U},
  {2U,140U,886U,4596U},
  {2U,70U,151U,4597U},
  {2U,173U,719U,4598U},
  {2U,23U,547U,4599U},
  {2U,344U,469U,4600U},
  {2U,220U,705U,4601U},
  {2U,310U,635U,4602U},
  {2U,13U,861U,4603U},
  {2U,24U,413U,4604U},
  {2U,266U,752U,4605U},
  {2U,481U,669U,4606U},
  {2U,313U,357U,4607U},
  {2U,484U,325U,4608U},
  {2U,505U,500U,4609U},
  {2U,468U,404U,4610U},
  {2U,6U,203U,4611U},
  {2U,318U,515U,4612U},
  {2U,481U,464U,4613U},
  {2U,90U,200U,4614U},
  {2U,418U,769U,4615U},
  {2U,46U,684U,4616U},
  {2U,396U,492U,4617U},
  {2U,487U,871U,4618U},
  {2U,30U,452U,4619U},
  {2U,367U,744U,4620U},
  {2U,89U,612U,4621U},
  {2U,43U,844U,4622U},
  {2U,119U,663U,4623U},
  {2U,134U,270U,4624U},
  {2U,465U,176U,4625U},
  {2U,463U,338U,4626U},
  {2U,77U,379U,4627U},
  {2U,351U,870U,4628U},
  {2U,251U,903U,4629U},
  {2U,168U,354U,4630U},
  {2U,278U,228U,4631U},
  {2U,27U,73U,4632U},
  {2U,201U,669U,4633U},
  {2U,222U,848U,4634U},
  {2U,94U,695U,4635U},
  {2U,105U,773U,4636U},
  {2U,44U,766U,4637U},
  {2U,321U,294U,4638U},
  {2U,304U,768U,4639U},
  {2U,332U,526U,4640U},
  {2U,434U,540U,4641U},
  {2U,209U,482U,4642U},
  {2U,394U,216U,4643U},
  {2U,340U,524U,4644U},
  {2U,501U,797U,4645U},
  {2U,413U,964U,4646U},
  {2U,8U,208U,4647U},
  {2U,410U,43U,4648U},
  {2U,28U,315U,4649U},
  {2U,497U,565U,4650U},
  {2U,372U,893U,4651U},
  {2U,123U,527U,4652U},
  {2U,406U,903U,4653U},
  {2U,160U,47U,4654U},
  {2U,68U,991U,4655U},
  {2U,223U,487U,4656U},
  {2U,129U,350U,4657U},
  {2U,75U,599U,4658U},
  {2U,483U,419U,4659U},
  {2U,269U,933U,4660U},
  {2U,39U,688U,4661U},
  {2U,259U,9U,4662U},
  {2U,507U,415U,4663U},
  {2U,407U,182U,4664U},
  {2U,318U,579U,4665U},
  {2U,139U,777U,4666U},
  {2U,145U,463U,4667U},
  {2U,36U,830U,4668U},
  {2U,503U,573U,4669U},
  {2U,339U,194U,4670U},
  {2U,1U,304U,4671U},
  {2U,423U,599U,4672U},
  {2U,147U,123U,4673U},
  {2U,109U,671U,4674U},
  {2U,309U,144U,4675U},
  {2U,468U,855U,4676U},
  {2U,500U,84U,4677U},
  {2U,320U,120U,4678U},
  {2U,386U,390U,4679U},
  {2U,112U,152U,4680U},
  {2U,79U,829U,4681U},
  {2U,158U,474U,4682U},
  {2U,241U,983U,4683U},
  {2U,509U,210U,4684U},
  {2U,40U,802U,4685U},
  {2U,304U,218U,4686U},
  {2U,507U,262U,4687U},
  {2U,316U,441U,4688U},
  {2U,423U,9U,4689U},
  {2U,135U,809U,4690U},
  {2U,182U,406U,4691U},
  {2U,74U,556U,4692U},
  {2U,495U,979U,4693U},
  {2U,98U,808U,4694U},
  {2U,298U,569U,4695U},
  {2U,258U,970U,4696U},
  {2U,100U,557U,4697U},
  {2U,346U,683U,4698U},
  {2U,99U,380U,4699U},
  {2U,454U,694U,4700U},
  {2U,93U,852U,4701U},
  {2U,72U,875U,4702U},
  {2U,308U,210U,4703U},
  {2U,34U,219U,4704U},
  {2U,76U,188U,4705U},
  {2U,196U,972U,4706U},
  {2U,335U,444U,4707U},
  {2U,140U,332U,4708U},
  {2U,223U,868U,4709U},
  {2U,133U,776U,4710U},
  {2U,62U,76U,4711U},
  {2U,376U,607U,4712U},
  {2U,427U,824U,4713U},
  {2U,374U,857U,4714U},
  {2U,447U,362U,4715U},
  {2U,33U,356U,4716U},
  {2U,407U,773U,4717U},
  {2U,280U,62U,4718U},
  {2U,121U,606U,4719U},
  {2U,250U,896U,4720U},
  {2U,56U,982U,4721U},
  {2U,153U,35U,4722U},
  {2U,472U,320U,4723U},
  {2U,329U,50U,4724U},
  {2U,14U,773U,4725U},
  {2U,353U,44U,4726U},
  {2U,212U,359U,4727U},
  {2U,304U,657U,4728U},
  {2U,275U,190U,4729U},
  {2U,468U,627U,4730U},
  {2U,223U,30U,4731U},
  {2U,165U,613U,4732U},
  {2U,146U,307U,4733U},
  {2U,34U,46U,4734U},
  {2U,18U,780U,4735U},
  {2U,65U,235U,4736U},
  {2U,225U,423U,4737U},
  {2U,198U,58U,4738U},
  {2U,273U,681U,4739U},
  {2U,186U,993U,4740U},
  {2U,42U,140U,4741U},
  {2U,226U,921U,4742U},
  {2U,235U,450U,4743U},
  {2U,316U,675U,4744U},
  {2U,152U,906U,4745U},
  {2U,107U,889U,4746U},
  {2U,509U,614U,4747U},
  {2U,350U,374U,4748U},
  {2U,325U,918U,4749U},
  {2U,296U,116U,4750U},
  {2U,92U,495U,4751U},
  {2U,430U,422U,4752U},
  {2U,120U,549U,4753U},
  {2U,511U,983U,4754U},
  {2U,329U,882U,4755U},
  {2U,478U,564U,4756U},
  {2U,358U,767U,4757U},
  {2U,206U,192U,4758U},
  {2U,399U,225U,4759U},
  {2U,474U,803U,4760U},
  {2U,505U,929U,4761U},
  {2U,8U,441U,4762U},
  {2U,7U,84U,4763U},
  {2U,336U,706U,4764U},
  {2U,43U,259U,4765U},
  {2U,479U,503U,4766U},
  {2U,398U,177U,4767U},
  {2U,474U,206U,4768U},
  {2U,327U,563U,4769U},
  {2U,277U,811U,4770U},
  {2U,96U,354U,4771U},
  {2U,109U,503U,4772U},
  {2U,465U,463U,4773U},
  {2U,289U,678U,4774U},
  {2U,324U,196U,4775U},
  {2U,393U,990U,4776U},
  {2U,487U,191U,4777U},
  {2U,168U,439U,4778U},
  {2U,378U,669U,4779U},
  {2U,321U,988U,4780U},
  {2U,502U,967U,4781U},
  {2U,350U,997U,4782U},
  {2U,411U,651U,4783U},
  {2U,228U,415U,4784U},
  {2U,310U,449U,4785U},
  {2U,323U,305U,4786U},
  {2U,204U,986U,4787U},
  {2U,191U,828U,4788U},
  {2U,128U,582U,4789U},
  {2U,442U,25U,4790U},
  {2U,100U,675U,4791U},
  {2U,15U,808U,4792U},
  {2U,439U,970U,4793U},
  {2U,309U,101U,4794U},
  {2U,265U,232U,4795U},
  {2U,257U,402U,4796U},
  {2U,379U,629U,4797U},
  {2U,466U,213U,4798U},
  {2U,240U,397U,4799U},
  {2U,435U,77U,4800U},
  {2U,349U,379U,4801U},
  {2U,134U,39U,4802U},
  {2U,317U,59U,4803U},
  {2U,35U,445U,4804U},
  {2U,448U,774U,4805U},
  {2U,181U,525U,4806U},
  {2U,8U,465U,4807U},
  {2U,299U,27U,4808U},
  {2U,224U,903U,4809U},
  {2U,113U,744U,4810U},
  {2U,89U,534U,4811U},
  {2U,503U,210U,4812U},
  {2U,83U,259U,4813U},
  {2U,143U,656U,4814U},
  {2U,21U,455U,4815U},
  {2U,155U,334U,4816U},
  {2U,345U,388U,4817U},
  {2U,14U,940U,4818U},
  {2U,124U,837U,4819U},
  {2U,132U,364U,4820U},
  {2U,293U,962U,4821U},
  {2U,94U,903U,4822U},
  {2U,275U,903U,4823U},
  {2U,316U,490U,4824U},
  {2U,208U,90U,4825U},
  {2U,483U,304U,4826U},
  {2U,310U,139U,4827U},
  {2U,11U,344U,4828U},
  {2U,448U,564U,4829U},
  {2U,405U,961U,4830U},
  {2U,238U,469U,4831U},
  {2U,328U,126U,4832U},
  {2U,488U,341U,4833U},
  {2U,333U,41U,4834U},
  {2U,96U,732U,4835U},
  {2U,468U,603U,4836U},
  {2U,11U,335U,4837U},
  {2U,103U,354U,4838U},
  {2U,409U,628U,4839U},
  {2U,196U,172U,4840U},
  {2U,89U,50U,4841U},
  {2U,289U,150U,4842U},
  {2U,398U,231U,4843U},
  {2U,17U,932U,4844U},
  {2U,321U,969U,4845U},
  {2U,314U,778U,4846U},
  {2U,274U,36U,4847U},
  {2U,419U,125U,4848U},
  {2U,370U,411U,4849U},
  {2U,487U,754U,4850U},
  {2U,192U,866U,4851U},
  {2U,401U,953U,4852U},
  {2U,450U,929U,4853U},
  {2U,473U,652U,4854U},
  {2U,252U,843U,4855U},
  {2U,147U,712U,4856U},
  {2U,12U,102U,4857U},
  {2U,504U,553U,4858U},
  {2U,42U,986U,4859U},
  {2U,300U,265U,4860U},
  {2U,369U,439U,4861U},
  {2U,432U,473U,4862U},
  {2U,18U,614U,4863U},
  {2U,316U,450U,4864U},
  {2U,72U,668U,4865U},
  {2U,127U,120U,4866U},
  {2U,327U,748U,4867U},
  {2U,279U,578U,4868U},
  {2U,164U,768U,4869U},
  {2U,457U,557U,4870U},
  {2U,369U,137U,4871U},
  {2U,281U,457U,4872U},
  {2U,295U,113U,4873U},
  {2U,2U,777U,4874U},
  {2U,303U,921U,4875U},
  {2U,248U,909U,4876U},
  {2U,390U,872U,4877U},
  {2U,469U,151U,4878U},
  {2U,361U,297U,4879U},
  {2U,390U,386U,4880U},
  {2U,437U,354U,4881U},
  {2U,41U,363U,4882U},
  {2U,225U,160U,4883U},
  {2U,23U,414U,4884U},
  {2U,80U,486U,4885U},
  {2U,332U,122U,4886U},
  {2U,112U,341U,4887U},
  {2U,210U,462U,4888U},
  {2U,340U,714U,4889U},
  {2U,289U,676U,4890U},
  {2U,308U,109U,4891U},
  {2U,410U,493U,4892U},
  {2U,438U,586U,4893U},
  {2U,72U,699U,4894U},
  {2U,373U,890U,4895U},
  {2U,335U,213U,4896U},
  {2U,150U,435U,4897U},
  {2U,31U,873U,4898U},
  {2U,323U,618U,4899U},
  {2U,115U,955U,4900U},
  {2U,82U,178U,4901U},
  {2U,178U,667U,4902U},
  {2U,445U,259U,4903U},
  {2U,46U,278U,4904U},
  {2U,493U,602U,4905U},
  {2U,46U,783U,4906U},
  {2U,15U,647U,4907U},
  {2U,431U,557U,4908U},
  {2U,139U,261U,4909U},
  {2U,443U,641U,4910U},
  {2U,360U,257U,4911U},
  {2U,478U,712U,4912U},
  {2U,377U,265U,4913U},
  {2U,97U,850U,4914U},
  {2U,505U,298U,4915U},
  {2U,411U,334U,4916U},
  {2U,463U,18U,4917U},
  {2U,488U,519U,4918U},
  {2U,18U,910U,4919U},
  {2U,78U,435U,4920U},
  {2U,8U,783U,4921U},
  {2U,339U,714U,4922U},
  {2U,265U,993U,4923U},
  {2U,176U,460U,4924U},
  {2U,103U,316U,4925U},
  {2U,33U,372U,4926U},
  {2U,450U,698U,4927U},
  {2U,147U,128U,4928U},
  {2U,187U,118U,4929U},
  {2U,97U,847U,4930U},
  {2U,23U,772U,4931U},
  {2U,170U,233U,4932U},
  {2U,66U,151U,4933U},
  {2U,155U,30U,4934U},
  {2U,45U,785U,4935U},
  {2U,126U,609U,4936U},
  {2U,276U,426U,4937U},
  {2U,438U,350U,4938U},
  {2U,223U,718U,4939U},
  {2U,339U,462U,4940U},
  {2U,15U,20U,4941U},
  {2U,3U,747U,4942U},
  {2U,308U,51U,4943U},
  {2U,313U,344U,4944U},
  {2U,384U,737U,4945U},
  {2U,339U,222U,4946U},
  {2U,369U,533U,4947U},
  {2U,270U,244U,4948U},
  {2U,330U,849U,4949U},
  {2U,378U,990U,4950U},
  {2U,491U,830U,4951U},
  {2U,70U,291U,4952U},
  {2U,490U,875U,4953U},
  {2U,424U,380U,4954U},
  {2U,118U,426U,4955U},
  {2U,107U,894U,4956U},
  {2U,252U,994U,4957U},
  {2U,339U,903U,4958U},
  {2U,108U,326U,4959U},
  {2U,473U,566U,4960U},
  {2U,26U,267U,4961U},
  {2U,337U,92U,4962U},
  {2U,269U,524U,4963U},
  {2U,123U,748U,4964U},
  {2U,391U,680U,4965U},
  {2U,38U,707U,4966U},
  {2U,77U,278U,4967U},
  {2U,203U,342U,4968U},
  {2U,261U,704U,4969U},
  {2U,199U,453U,4970U},
  {2U,56U,338U,4971U},
  {2U,253U,83U,4972U},
  {2U,416U,56U,4973U},
  {2U,402U,190U,4974U},
  {2U,510U,830U,4975U},
  {2U,61U,504U,4976U},
  {2U,65U,304U,4977U},
  {2U,165U,792U,4978U},
  {2U,210U,64U,4979U},
  {2U,23U,479U,4980U},
  {2U,111U,740U,4981U},
  {2U,457U,560U,4982U},
  {2U,8U,100U,4983U},
  {2U,308U,454U,4984U},
  {2U,17U,863U,4985U},
  {2U,13U,57U,4986U},
  {2U,426U,774U,4987U},
  {2U,131U,599U,4988U},
  {2U,67U,655U,4989U},
  {2U,113U,967U,4990U},
  {2U,255U,750U,4991U},
  {2U,95U,902U,4992U},
  {2U,418U,333U,4993U},
  {2U,507U,940U,4994U},
  {2U,290U,175U,4995U},
  {2U,193U,26U,4996U},
  {2U,24U,135U,4997U},
  {2U,186U,527U,4998U},
  {2U,259U,562U,4999U},
  {2U,87U,954U,5000U},
  {2U,114U,560U,5001U},
  {2U,299U,769U,5002U},
  {2U,66U,996U,5003U},
  {2U,251U,361U,5004U},
  {2U,169U,690U,5005U},
  {2U,403U,689U,5006U},
  {2U,38U,418U,5007U},
  {2U,312U,417U,5008U},
  {2U,394U,908U,5009U},
  {2U,10U,982U,5010U},
  {2U,244U,445U,5011U},
  {2U,397U,892U,5012U},
  {2U,172U,649U,5013U},
  {2U,330U,67U,5014U},
  {2U,307U,538U,5015U},
  {2U,393U,494U,5016U},
  {2U,493U,411U,5017U},
  {2U,66U,17U,5018U},
  {2U,260U,842U,5019U},
  {2U,357U,59U,5020U},
  {2U,85U,128U,5021U},
  {2U,212U,783U,5022U},
  {2U,37U,136U,5023U},
  {2U,496U,357U,5024U},
  {2U,410U,621U,5025U},
  {2U,379U,950U,5026U},
  {2U,397U,881U,5027U},
  {2U,338U,16U,5028U},
  {2U,344U,660U,5029U},
  {2U,76U,700U,5030U},
  {2U,432U,71U,5031U},
  {2U,364U,613U,5032U},
  {2U,508U,581U,5033U},
  {2U,159U,615U,5034U},
  {2U,28U,248U,5035U},
  {2U,327U,342U,5036U},
  {2U,350U,760U,5037U},
  {2U,302U,686U,5038U},
  {2U,305U,731U,5039U},
  {2U,477U,27U,5040U},
  {2U,24U,473U,5041U},
  {2U,377U,13U,5042U},
  {2U,88U,258U,5043U},
  {2U,2U,503U,5044U},
  {2U,322U,687U,5045U},
  {2U,73U,971U,5046U},
  {2U,300U,511U,5047U},
  {2U,41U,614U,5048U},
  {2U,312U,780U,5049U},
  {2U,31U,227U,5050U},
  {2U,432U,987U,5051U},
  {2U,445U,40U,5052U},
  {2U,453U,617U,5053U},
  {2U,494U,935U,5054U},
  {2U,110U,585U,5055U},
  {2U,130U,217U,5056U},
  {2U,296U,450U,5057U},
  {2U,103U,242U,5058U},
  {2U,327U,626U,5059U},
  {2U,339U,907U,5060U},
  {2U,288U,596U,5061U},
  {2U,99U,508U,5062U},
  {2U,192U,955U,5063U},
  {2U,272U,201U,5064U},
  {2U,23U,450U,5065U},
  {2U,291U,210U,5066U},
  {2U,406U,8U,5067U},
  {2U,477U,951U,5068U},
  {2U,507U,62U,5069U},
  {2U,283U,197U,5070U},
  {2U,75U,674U,5071U},
  {2U,152U,662U,5072U},
  {2U,118U,920U,5073U},
  {2U,37U,379U,5074U},
  {2U,102U,930U,5075U},
  {2U,142U,706U,5076U},
  {2U,18U,944U,5077U},
  {2U,127U,388U,5078U},
  {2U,162U,325U,5079U},
  {2U,212U,339U,5080U},
  {2U,218U,534U,5081U},
  {2U,299U,193U,5082U},
  {2U,81U,449U,5083U},
  {2U,109U,413U,5084U},
  {2U,390U,668U,5085U},
  {2U,354U,738U,5086U},
  {2U,62U,946U,5087U},
  {2U,219U,933U,5088U},
  {2U,190U,837U,5089U},
  {2U,193U,809U,5090U},
  {2U,260U,648U,5091U},
  {2U,192U,413U,5092U},
  {2U,366U,141U,5093U},
  {2U,72U,42U,5094U},
  {2U,14U,692U,5095U},
  {2U,118U,472U,5096U},
  {2U,204U,30U,5097U},
  {2U,152U,412U,5098U},
  {2U,69U,642U,5099U},
  {2U,506U,146U,5100U},
  {2U,48U,76U,5101U},
  {2U,327U,608U,5102U},
  {2U,407U,951U,5103U},
  {2U,497U,978U,5104U},
  {2U,130U,415U,5105U},
  {2U,202U,298U,5106U},
  {2U,440U,300U,5107U},
  {2U,336U,665U,5108U},
  {2U,257U,833U,5109U},
  {2U,253U,579U,5110U},
  {2U,365U,318U,5111U},
  {2U,231U,180U,5112U},
  {2U,126U,830U,5113U},
  {2U,231U,584U,5114U},
  {2U,427U,427U,5115U},
  {2U,27U,130U,5116U},
  {2U,309U,855U,5117U},
  {2U,346U,44U,5118U},
  {2U,5U,637U,5119U},
  {2U,367U,672U,5120U},
};
static const unsigned paper_offsets[2] = {0U,5120U};
static const unsigned paper_work_starts[1] = {3072U};
static const unsigned paper_expected_sums[1] = {2362757U};
static const unsigned paper_expected_live[1] = {259754U};

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
  assert(p && benchmark_live[me()]>=bytes);
#ifdef HOST_TEST
  host_remove_interval(p,bytes);
#endif
  BEGIN(free);policy_free(p);END(free);benchmark_live[me()]-=bytes;
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
static void apply_op(unsigned i) {
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
static void app_check(void) {
  unsigned sum=0,bytes=0;
  for(unsigned i=0;i<POOL_SIZE;++i) {
    struct PaperCell c __attribute__((aligned(8)))=load_cell(i);cell_check(&c);sum+=c.tag;bytes+=c.size;
  }
  assert(sum==paper_expected_sums[me()] && bytes==paper_expected_live[me()]);
  assert(bytes==benchmark_live[me()]);
}
static void app_destroy(void) {
  for(unsigned i=0;i<POOL_SIZE;++i) {
    struct PaperCell c __attribute__((aligned(8)))=load_cell(i);cell_check(&c);bench_free((__mram_ptr void *)c.pointer,c.size);
  }
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
  BEGIN(work);
  for(unsigned i=paper_work_starts[tid];i<paper_offsets[tid+1];++i)apply_op(i);
  barrier_wait(&policy_barrier);
  END(work);
  if(!tid)work_end();
  barrier_wait(&policy_barrier);
  app_check();
  if(VALIDATE)policy_verify(tid);
  barrier_wait(&policy_barrier);
  BEGIN(drain);app_destroy();policy_drain(tid);barrier_wait(&policy_barrier);END(drain);
  if(!tid)final_report();return 0;
}

