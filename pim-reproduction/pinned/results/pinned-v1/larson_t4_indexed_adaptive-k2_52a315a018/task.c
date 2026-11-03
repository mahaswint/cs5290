#define BUDDY_PIN_LEVELS 0
#define NR_TASKLETS 4
#define POLICY 3
#define LISTS 1
#define LOOKUP 1
#define HEAP_BYTES 33554432
#define KEEP_PAGES 2
#define MAX_KEEP 2
#define EMPTY_BUDGET 64
#define DECAY_OPS 64
#define PRESSURE_PERCENT 75
#define VALIDATE 0
#define POOL_SIZE 64
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
static PAPER_INPUT struct PaperOp paper_ops[2560] = {
  {0U,0U,2936U,0U},
  {0U,1U,3312U,0U},
  {0U,2U,2644U,0U},
  {0U,3U,842U,0U},
  {0U,4U,3195U,0U},
  {0U,5U,3772U,0U},
  {0U,6U,802U,0U},
  {0U,7U,2926U,0U},
  {0U,8U,2817U,0U},
  {0U,9U,2212U,0U},
  {0U,10U,4059U,0U},
  {0U,11U,549U,0U},
  {0U,12U,2178U,0U},
  {0U,13U,3430U,0U},
  {0U,14U,3680U,0U},
  {0U,15U,2738U,0U},
  {0U,16U,1945U,0U},
  {0U,17U,430U,0U},
  {0U,18U,631U,0U},
  {0U,19U,2244U,0U},
  {0U,20U,3902U,0U},
  {0U,21U,2372U,0U},
  {0U,22U,153U,0U},
  {0U,23U,2386U,0U},
  {0U,24U,2246U,0U},
  {0U,25U,3869U,0U},
  {0U,26U,626U,0U},
  {0U,27U,697U,0U},
  {0U,28U,2779U,0U},
  {0U,29U,65U,0U},
  {0U,30U,2814U,0U},
  {0U,31U,2446U,0U},
  {0U,32U,1027U,0U},
  {0U,33U,1308U,0U},
  {0U,34U,2934U,0U},
  {0U,35U,2213U,0U},
  {0U,36U,364U,0U},
  {0U,37U,781U,0U},
  {0U,38U,2125U,0U},
  {0U,39U,217U,0U},
  {0U,40U,1056U,0U},
  {0U,41U,1116U,0U},
  {0U,42U,1359U,0U},
  {0U,43U,50U,0U},
  {0U,44U,1668U,0U},
  {0U,45U,3650U,0U},
  {0U,46U,2838U,0U},
  {0U,47U,1924U,0U},
  {0U,48U,440U,0U},
  {0U,49U,2459U,0U},
  {0U,50U,575U,0U},
  {0U,51U,1131U,0U},
  {0U,52U,1100U,0U},
  {0U,53U,3924U,0U},
  {0U,54U,3713U,0U},
  {0U,55U,2270U,0U},
  {0U,56U,139U,0U},
  {0U,57U,2127U,0U},
  {0U,58U,3327U,0U},
  {0U,59U,1693U,0U},
  {0U,60U,2736U,0U},
  {0U,61U,1769U,0U},
  {0U,62U,1200U,0U},
  {0U,63U,999U,0U},
  {1U,35U,63U,0U},
  {1U,22U,62U,0U},
  {1U,58U,61U,0U},
  {1U,58U,60U,0U},
  {1U,34U,59U,0U},
  {1U,41U,58U,0U},
  {1U,8U,57U,0U},
  {1U,31U,56U,0U},
  {1U,6U,55U,0U},
  {1U,33U,54U,0U},
  {1U,37U,53U,0U},
  {1U,35U,52U,0U},
  {1U,39U,51U,0U},
  {1U,7U,50U,0U},
  {1U,16U,49U,0U},
  {1U,35U,48U,0U},
  {1U,33U,47U,0U},
  {1U,29U,46U,0U},
  {1U,13U,45U,0U},
  {1U,4U,44U,0U},
  {1U,3U,43U,0U},
  {1U,2U,42U,0U},
  {1U,25U,41U,0U},
  {1U,11U,40U,0U},
  {1U,28U,39U,0U},
  {1U,23U,38U,0U},
  {1U,20U,37U,0U},
  {1U,13U,36U,0U},
  {1U,1U,35U,0U},
  {1U,5U,34U,0U},
  {1U,8U,33U,0U},
  {1U,1U,32U,0U},
  {1U,0U,31U,0U},
  {1U,19U,30U,0U},
  {1U,22U,29U,0U},
  {1U,1U,28U,0U},
  {1U,5U,27U,0U},
  {1U,18U,26U,0U},
  {1U,2U,25U,0U},
  {1U,7U,24U,0U},
  {1U,20U,23U,0U},
  {1U,11U,22U,0U},
  {1U,17U,21U,0U},
  {1U,12U,20U,0U},
  {1U,12U,19U,0U},
  {1U,1U,18U,0U},
  {1U,11U,17U,0U},
  {1U,3U,16U,0U},
  {1U,11U,15U,0U},
  {1U,8U,14U,0U},
  {1U,3U,13U,0U},
  {1U,10U,12U,0U},
  {1U,7U,11U,0U},
  {1U,10U,10U,0U},
  {1U,6U,9U,0U},
  {1U,6U,8U,0U},
  {1U,3U,7U,0U},
  {1U,2U,6U,0U},
  {1U,3U,5U,0U},
  {1U,4U,4U,0U},
  {1U,2U,3U,0U},
  {1U,0U,2U,0U},
  {1U,1U,1U,0U},
  {1U,0U,0U,0U},
  {2U,19U,3970U,129U},
  {2U,22U,1121U,130U},
  {2U,53U,3878U,131U},
  {2U,13U,2482U,132U},
  {2U,0U,1316U,133U},
  {2U,60U,462U,134U},
  {2U,31U,3742U,135U},
  {2U,15U,1261U,136U},
  {2U,5U,2934U,137U},
  {2U,30U,459U,138U},
  {2U,60U,1490U,139U},
  {2U,52U,3015U,140U},
  {2U,10U,1593U,141U},
  {2U,56U,2287U,142U},
  {2U,51U,2707U,143U},
  {2U,52U,3002U,144U},
  {2U,14U,1731U,145U},
  {2U,11U,441U,146U},
  {2U,61U,2221U,147U},
  {2U,19U,1739U,148U},
  {2U,37U,974U,149U},
  {2U,16U,2112U,150U},
  {2U,55U,2705U,151U},
  {2U,27U,1465U,152U},
  {2U,7U,2594U,153U},
  {2U,63U,2433U,154U},
  {2U,28U,3207U,155U},
  {2U,49U,3147U,156U},
  {2U,43U,2870U,157U},
  {2U,7U,488U,158U},
  {2U,58U,3057U,159U},
  {2U,21U,1840U,160U},
  {2U,11U,1487U,161U},
  {2U,62U,2883U,162U},
  {2U,60U,1855U,163U},
  {2U,15U,2527U,164U},
  {2U,53U,2772U,165U},
  {2U,60U,3918U,166U},
  {2U,53U,2784U,167U},
  {2U,35U,2134U,168U},
  {2U,53U,1028U,169U},
  {2U,63U,3268U,170U},
  {2U,5U,2680U,171U},
  {2U,30U,2560U,172U},
  {2U,41U,2027U,173U},
  {2U,31U,1273U,174U},
  {2U,58U,1489U,175U},
  {2U,33U,1852U,176U},
  {2U,59U,1518U,177U},
  {2U,21U,524U,178U},
  {2U,11U,2370U,179U},
  {2U,51U,476U,180U},
  {2U,17U,3968U,181U},
  {2U,15U,3059U,182U},
  {2U,7U,929U,183U},
  {2U,29U,2106U,184U},
  {2U,31U,3017U,185U},
  {2U,7U,1634U,186U},
  {2U,20U,2225U,187U},
  {2U,38U,1334U,188U},
  {2U,41U,2593U,189U},
  {2U,24U,3965U,190U},
  {2U,44U,2711U,191U},
  {2U,59U,659U,192U},
  {2U,3U,162U,193U},
  {2U,11U,2512U,194U},
  {2U,10U,1921U,195U},
  {2U,35U,773U,196U},
  {2U,45U,629U,197U},
  {2U,54U,2727U,198U},
  {2U,37U,2413U,199U},
  {2U,59U,618U,200U},
  {2U,39U,297U,201U},
  {2U,18U,550U,202U},
  {2U,39U,3107U,203U},
  {2U,60U,1582U,204U},
  {2U,32U,2349U,205U},
  {2U,20U,3587U,206U},
  {2U,2U,1736U,207U},
  {2U,15U,511U,208U},
  {2U,58U,1011U,209U},
  {2U,19U,525U,210U},
  {2U,4U,1643U,211U},
  {2U,19U,3551U,212U},
  {2U,61U,2967U,213U},
  {2U,29U,942U,214U},
  {2U,52U,3462U,215U},
  {2U,39U,3185U,216U},
  {2U,8U,598U,217U},
  {2U,47U,1416U,218U},
  {2U,12U,3321U,219U},
  {2U,35U,4080U,220U},
  {2U,8U,1931U,221U},
  {2U,18U,3553U,222U},
  {2U,20U,778U,223U},
  {2U,60U,213U,224U},
  {2U,31U,4060U,225U},
  {2U,17U,3817U,226U},
  {2U,41U,732U,227U},
  {2U,31U,3104U,228U},
  {2U,0U,3439U,229U},
  {2U,29U,1453U,230U},
  {2U,42U,436U,231U},
  {2U,28U,1346U,232U},
  {2U,5U,3232U,233U},
  {2U,46U,3369U,234U},
  {2U,5U,1725U,235U},
  {2U,17U,3650U,236U},
  {2U,0U,2462U,237U},
  {2U,8U,467U,238U},
  {2U,57U,1756U,239U},
  {2U,3U,1718U,240U},
  {2U,4U,2668U,241U},
  {2U,13U,2043U,242U},
  {2U,26U,3595U,243U},
  {2U,4U,1306U,244U},
  {2U,52U,841U,245U},
  {2U,30U,3862U,246U},
  {2U,17U,1911U,247U},
  {2U,40U,30U,248U},
  {2U,21U,678U,249U},
  {2U,62U,2633U,250U},
  {2U,20U,1037U,251U},
  {2U,29U,770U,252U},
  {2U,6U,301U,253U},
  {2U,53U,1664U,254U},
  {2U,11U,513U,255U},
  {2U,13U,1572U,256U},
  {2U,61U,792U,257U},
  {2U,60U,87U,258U},
  {2U,62U,1133U,259U},
  {2U,63U,492U,260U},
  {2U,30U,53U,261U},
  {2U,11U,3338U,262U},
  {2U,47U,3154U,263U},
  {2U,14U,4028U,264U},
  {2U,42U,3684U,265U},
  {2U,47U,3126U,266U},
  {2U,50U,2525U,267U},
  {2U,39U,1776U,268U},
  {2U,61U,435U,269U},
  {2U,63U,2570U,270U},
  {2U,14U,655U,271U},
  {2U,36U,902U,272U},
  {2U,0U,3441U,273U},
  {2U,39U,3433U,274U},
  {2U,58U,24U,275U},
  {2U,3U,2938U,276U},
  {2U,19U,1715U,277U},
  {2U,31U,1716U,278U},
  {2U,25U,2756U,279U},
  {2U,8U,1436U,280U},
  {2U,54U,631U,281U},
  {2U,42U,3834U,282U},
  {2U,14U,1173U,283U},
  {2U,34U,294U,284U},
  {2U,2U,3049U,285U},
  {2U,7U,4085U,286U},
  {2U,27U,2052U,287U},
  {2U,41U,1649U,288U},
  {2U,19U,3114U,289U},
  {2U,37U,2050U,290U},
  {2U,56U,178U,291U},
  {2U,33U,1196U,292U},
  {2U,47U,4045U,293U},
  {2U,28U,3668U,294U},
  {2U,7U,3936U,295U},
  {2U,57U,3967U,296U},
  {2U,31U,1828U,297U},
  {2U,19U,3947U,298U},
  {2U,12U,1040U,299U},
  {2U,45U,3747U,300U},
  {2U,3U,1246U,301U},
  {2U,53U,549U,302U},
  {2U,40U,1629U,303U},
  {2U,32U,533U,304U},
  {2U,21U,2959U,305U},
  {2U,55U,171U,306U},
  {2U,36U,3159U,307U},
  {2U,23U,1283U,308U},
  {2U,39U,453U,309U},
  {2U,10U,1377U,310U},
  {2U,57U,3074U,311U},
  {2U,26U,2016U,312U},
  {2U,63U,618U,313U},
  {2U,13U,3049U,314U},
  {2U,15U,573U,315U},
  {2U,22U,2984U,316U},
  {2U,27U,4004U,317U},
  {2U,63U,801U,318U},
  {2U,46U,3098U,319U},
  {2U,3U,789U,320U},
  {2U,46U,1112U,321U},
  {2U,50U,1742U,322U},
  {2U,17U,72U,323U},
  {2U,39U,1826U,324U},
  {2U,27U,109U,325U},
  {2U,23U,1509U,326U},
  {2U,12U,1644U,327U},
  {2U,7U,3211U,328U},
  {2U,11U,3996U,329U},
  {2U,38U,1096U,330U},
  {2U,39U,967U,331U},
  {2U,54U,2432U,332U},
  {2U,32U,1811U,333U},
  {2U,57U,447U,334U},
  {2U,12U,3303U,335U},
  {2U,43U,1426U,336U},
  {2U,9U,1408U,337U},
  {2U,12U,2481U,338U},
  {2U,8U,2099U,339U},
  {2U,54U,3029U,340U},
  {2U,20U,3886U,341U},
  {2U,36U,2537U,342U},
  {2U,50U,2279U,343U},
  {2U,6U,1626U,344U},
  {2U,35U,3515U,345U},
  {2U,55U,3680U,346U},
  {2U,8U,1238U,347U},
  {2U,30U,2690U,348U},
  {2U,47U,2223U,349U},
  {2U,53U,1807U,350U},
  {2U,36U,1720U,351U},
  {2U,20U,1861U,352U},
  {2U,57U,933U,353U},
  {2U,55U,3051U,354U},
  {2U,23U,3585U,355U},
  {2U,3U,783U,356U},
  {2U,0U,595U,357U},
  {2U,59U,2106U,358U},
  {2U,37U,3432U,359U},
  {2U,48U,525U,360U},
  {2U,56U,1029U,361U},
  {2U,0U,485U,362U},
  {2U,18U,2147U,363U},
  {2U,31U,2589U,364U},
  {2U,30U,1835U,365U},
  {2U,30U,1373U,366U},
  {2U,17U,582U,367U},
  {2U,2U,1113U,368U},
  {2U,8U,1231U,369U},
  {2U,9U,445U,370U},
  {2U,9U,2376U,371U},
  {2U,26U,3892U,372U},
  {2U,57U,1606U,373U},
  {2U,40U,820U,374U},
  {2U,37U,1810U,375U},
  {2U,1U,3579U,376U},
  {2U,62U,1561U,377U},
  {2U,1U,4058U,378U},
  {2U,14U,343U,379U},
  {2U,33U,2955U,380U},
  {2U,32U,1923U,381U},
  {2U,46U,313U,382U},
  {2U,13U,3601U,383U},
  {2U,24U,3247U,384U},
  {2U,45U,2590U,385U},
  {2U,45U,686U,386U},
  {2U,51U,3373U,387U},
  {2U,44U,2268U,388U},
  {2U,63U,3581U,389U},
  {2U,16U,1020U,390U},
  {2U,40U,3063U,391U},
  {2U,29U,829U,392U},
  {2U,13U,2399U,393U},
  {2U,27U,904U,394U},
  {2U,26U,2494U,395U},
  {2U,22U,2384U,396U},
  {2U,17U,80U,397U},
  {2U,21U,2868U,398U},
  {2U,12U,422U,399U},
  {2U,17U,3229U,400U},
  {2U,16U,1536U,401U},
  {2U,52U,687U,402U},
  {2U,38U,4049U,403U},
  {2U,55U,785U,404U},
  {2U,17U,3338U,405U},
  {2U,42U,2322U,406U},
  {2U,59U,926U,407U},
  {2U,61U,1120U,408U},
  {2U,44U,3812U,409U},
  {2U,32U,3754U,410U},
  {2U,50U,1159U,411U},
  {2U,29U,1511U,412U},
  {2U,16U,1549U,413U},
  {2U,13U,590U,414U},
  {2U,51U,2655U,415U},
  {2U,15U,1419U,416U},
  {2U,51U,977U,417U},
  {2U,34U,3486U,418U},
  {2U,33U,2649U,419U},
  {2U,26U,1375U,420U},
  {2U,62U,3474U,421U},
  {2U,8U,2014U,422U},
  {2U,62U,1477U,423U},
  {2U,57U,2953U,424U},
  {2U,36U,4002U,425U},
  {2U,59U,1616U,426U},
  {2U,56U,271U,427U},
  {2U,2U,2188U,428U},
  {2U,56U,3285U,429U},
  {2U,5U,1702U,430U},
  {2U,6U,1387U,431U},
  {2U,41U,253U,432U},
  {2U,60U,113U,433U},
  {2U,30U,2252U,434U},
  {2U,42U,2896U,435U},
  {2U,48U,3927U,436U},
  {2U,3U,3366U,437U},
  {2U,34U,2108U,438U},
  {2U,26U,1443U,439U},
  {2U,19U,1795U,440U},
  {2U,49U,2652U,441U},
  {2U,12U,626U,442U},
  {2U,14U,451U,443U},
  {2U,6U,2284U,444U},
  {2U,20U,1195U,445U},
  {2U,24U,3347U,446U},
  {2U,21U,732U,447U},
  {2U,29U,1601U,448U},
  {2U,0U,1911U,449U},
  {2U,39U,381U,450U},
  {2U,25U,2214U,451U},
  {2U,18U,3905U,452U},
  {2U,3U,4060U,453U},
  {2U,54U,3242U,454U},
  {2U,13U,950U,455U},
  {2U,48U,2634U,456U},
  {2U,2U,3006U,457U},
  {2U,42U,760U,458U},
  {2U,37U,352U,459U},
  {2U,21U,3680U,460U},
  {2U,50U,793U,461U},
  {2U,30U,2193U,462U},
  {2U,34U,3973U,463U},
  {2U,10U,765U,464U},
  {2U,59U,3268U,465U},
  {2U,51U,3739U,466U},
  {2U,10U,548U,467U},
  {2U,26U,922U,468U},
  {2U,59U,770U,469U},
  {2U,6U,1539U,470U},
  {2U,6U,1796U,471U},
  {2U,23U,3083U,472U},
  {2U,55U,3164U,473U},
  {2U,42U,1279U,474U},
  {2U,48U,191U,475U},
  {2U,34U,2708U,476U},
  {2U,56U,233U,477U},
  {2U,62U,2820U,478U},
  {2U,23U,3837U,479U},
  {2U,16U,84U,480U},
  {2U,49U,3070U,481U},
  {2U,30U,838U,482U},
  {2U,46U,3498U,483U},
  {2U,43U,3945U,484U},
  {2U,49U,1114U,485U},
  {2U,15U,4049U,486U},
  {2U,63U,1348U,487U},
  {2U,18U,3651U,488U},
  {2U,29U,1699U,489U},
  {2U,35U,1832U,490U},
  {2U,58U,1150U,491U},
  {2U,57U,1994U,492U},
  {2U,7U,3083U,493U},
  {2U,9U,2159U,494U},
  {2U,10U,3215U,495U},
  {2U,27U,1323U,496U},
  {2U,17U,2224U,497U},
  {2U,35U,923U,498U},
  {2U,46U,1442U,499U},
  {2U,31U,1721U,500U},
  {2U,51U,1907U,501U},
  {2U,56U,1865U,502U},
  {2U,47U,2976U,503U},
  {2U,57U,3089U,504U},
  {2U,39U,4015U,505U},
  {2U,42U,2060U,506U},
  {2U,46U,1663U,507U},
  {2U,15U,980U,508U},
  {2U,57U,1636U,509U},
  {2U,45U,1443U,510U},
  {2U,31U,3251U,511U},
  {2U,9U,1065U,512U},
  {2U,23U,1355U,513U},
  {2U,38U,2732U,514U},
  {2U,27U,2365U,515U},
  {2U,14U,214U,516U},
  {2U,36U,2158U,517U},
  {2U,25U,3754U,518U},
  {2U,11U,3621U,519U},
  {2U,12U,1111U,520U},
  {2U,30U,816U,521U},
  {2U,36U,2654U,522U},
  {2U,33U,387U,523U},
  {2U,54U,3035U,524U},
  {2U,37U,3724U,525U},
  {2U,7U,3413U,526U},
  {2U,4U,2588U,527U},
  {2U,54U,717U,528U},
  {2U,46U,3201U,529U},
  {2U,63U,3763U,530U},
  {2U,25U,538U,531U},
  {2U,34U,16U,532U},
  {2U,59U,2700U,533U},
  {2U,8U,1795U,534U},
  {2U,30U,1425U,535U},
  {2U,4U,1733U,536U},
  {2U,38U,4077U,537U},
  {2U,53U,3003U,538U},
  {2U,12U,3397U,539U},
  {2U,29U,1751U,540U},
  {2U,52U,570U,541U},
  {2U,41U,3129U,542U},
  {2U,26U,2687U,543U},
  {2U,49U,857U,544U},
  {2U,27U,2910U,545U},
  {2U,19U,1797U,546U},
  {2U,24U,2495U,547U},
  {2U,57U,2977U,548U},
  {2U,30U,3571U,549U},
  {2U,48U,853U,550U},
  {2U,1U,2006U,551U},
  {2U,63U,3660U,552U},
  {2U,42U,3411U,553U},
  {2U,6U,2893U,554U},
  {2U,10U,3172U,555U},
  {2U,3U,477U,556U},
  {2U,61U,34U,557U},
  {2U,32U,223U,558U},
  {2U,16U,686U,559U},
  {2U,34U,893U,560U},
  {2U,6U,207U,561U},
  {2U,22U,2577U,562U},
  {2U,63U,1751U,563U},
  {2U,31U,3850U,564U},
  {2U,50U,1991U,565U},
  {2U,8U,2804U,566U},
  {2U,45U,1184U,567U},
  {2U,40U,293U,568U},
  {2U,12U,1265U,569U},
  {2U,59U,1290U,570U},
  {2U,44U,530U,571U},
  {2U,33U,2689U,572U},
  {2U,7U,2243U,573U},
  {2U,59U,3456U,574U},
  {2U,18U,3897U,575U},
  {2U,46U,2492U,576U},
  {2U,34U,1686U,577U},
  {2U,25U,982U,578U},
  {2U,57U,2957U,579U},
  {2U,30U,1258U,580U},
  {2U,63U,3704U,581U},
  {2U,38U,1134U,582U},
  {2U,25U,1327U,583U},
  {2U,9U,2081U,584U},
  {2U,31U,1928U,585U},
  {2U,8U,3651U,586U},
  {2U,43U,676U,587U},
  {2U,12U,44U,588U},
  {2U,53U,3356U,589U},
  {2U,8U,2144U,590U},
  {2U,32U,3625U,591U},
  {2U,55U,2610U,592U},
  {2U,32U,3654U,593U},
  {2U,32U,2427U,594U},
  {2U,33U,180U,595U},
  {2U,43U,1614U,596U},
  {2U,22U,1923U,597U},
  {2U,23U,3167U,598U},
  {2U,17U,1271U,599U},
  {2U,11U,4032U,600U},
  {2U,29U,3135U,601U},
  {2U,58U,2170U,602U},
  {2U,10U,3537U,603U},
  {2U,60U,565U,604U},
  {2U,13U,3157U,605U},
  {2U,45U,1112U,606U},
  {2U,38U,1208U,607U},
  {2U,32U,3505U,608U},
  {2U,55U,1339U,609U},
  {2U,59U,2622U,610U},
  {2U,62U,2042U,611U},
  {2U,15U,2816U,612U},
  {2U,21U,195U,613U},
  {2U,63U,4070U,614U},
  {2U,60U,2289U,615U},
  {2U,24U,161U,616U},
  {2U,7U,1359U,617U},
  {2U,42U,314U,618U},
  {2U,43U,133U,619U},
  {2U,37U,532U,620U},
  {2U,30U,1454U,621U},
  {2U,19U,2685U,622U},
  {2U,29U,2470U,623U},
  {2U,33U,3065U,624U},
  {2U,8U,2286U,625U},
  {2U,56U,1038U,626U},
  {2U,10U,1601U,627U},
  {2U,8U,2291U,628U},
  {2U,20U,3515U,629U},
  {2U,41U,3349U,630U},
  {2U,51U,1339U,631U},
  {2U,0U,1902U,632U},
  {2U,13U,1657U,633U},
  {2U,58U,3036U,634U},
  {2U,32U,797U,635U},
  {2U,39U,2419U,636U},
  {2U,7U,2216U,637U},
  {2U,62U,3831U,638U},
  {2U,46U,2406U,639U},
  {2U,35U,123U,640U},
  {0U,0U,2229U,0U},
  {0U,1U,2988U,0U},
  {0U,2U,16U,0U},
  {0U,3U,772U,0U},
  {0U,4U,655U,0U},
  {0U,5U,490U,0U},
  {0U,6U,1653U,0U},
  {0U,7U,3865U,0U},
  {0U,8U,203U,0U},
  {0U,9U,3504U,0U},
  {0U,10U,1403U,0U},
  {0U,11U,1776U,0U},
  {0U,12U,2561U,0U},
  {0U,13U,3951U,0U},
  {0U,14U,1741U,0U},
  {0U,15U,1371U,0U},
  {0U,16U,1813U,0U},
  {0U,17U,1995U,0U},
  {0U,18U,3963U,0U},
  {0U,19U,2554U,0U},
  {0U,20U,1835U,0U},
  {0U,21U,2482U,0U},
  {0U,22U,1006U,0U},
  {0U,23U,2543U,0U},
  {0U,24U,2093U,0U},
  {0U,25U,2815U,0U},
  {0U,26U,3543U,0U},
  {0U,27U,2819U,0U},
  {0U,28U,1234U,0U},
  {0U,29U,1222U,0U},
  {0U,30U,2599U,0U},
  {0U,31U,819U,0U},
  {0U,32U,648U,0U},
  {0U,33U,3545U,0U},
  {0U,34U,2194U,0U},
  {0U,35U,1920U,0U},
  {0U,36U,1655U,0U},
  {0U,37U,212U,0U},
  {0U,38U,1135U,0U},
  {0U,39U,2153U,0U},
  {0U,40U,3047U,0U},
  {0U,41U,1960U,0U},
  {0U,42U,3124U,0U},
  {0U,43U,321U,0U},
  {0U,44U,652U,0U},
  {0U,45U,3655U,0U},
  {0U,46U,1443U,0U},
  {0U,47U,2664U,0U},
  {0U,48U,3678U,0U},
  {0U,49U,346U,0U},
  {0U,50U,3956U,0U},
  {0U,51U,3928U,0U},
  {0U,52U,2074U,0U},
  {0U,53U,532U,0U},
  {0U,54U,550U,0U},
  {0U,55U,3158U,0U},
  {0U,56U,1965U,0U},
  {0U,57U,2482U,0U},
  {0U,58U,1123U,0U},
  {0U,59U,2712U,0U},
  {0U,60U,2206U,0U},
  {0U,61U,1952U,0U},
  {0U,62U,3558U,0U},
  {0U,63U,2212U,0U},
  {1U,46U,63U,0U},
  {1U,44U,62U,0U},
  {1U,39U,61U,0U},
  {1U,60U,60U,0U},
  {1U,51U,59U,0U},
  {1U,39U,58U,0U},
  {1U,37U,57U,0U},
  {1U,51U,56U,0U},
  {1U,37U,55U,0U},
  {1U,4U,54U,0U},
  {1U,23U,53U,0U},
  {1U,39U,52U,0U},
  {1U,40U,51U,0U},
  {1U,32U,50U,0U},
  {1U,19U,49U,0U},
  {1U,46U,48U,0U},
  {1U,22U,47U,0U},
  {1U,12U,46U,0U},
  {1U,7U,45U,0U},
  {1U,33U,44U,0U},
  {1U,22U,43U,0U},
  {1U,13U,42U,0U},
  {1U,32U,41U,0U},
  {1U,2U,40U,0U},
  {1U,32U,39U,0U},
  {1U,38U,38U,0U},
  {1U,30U,37U,0U},
  {1U,18U,36U,0U},
  {1U,18U,35U,0U},
  {1U,3U,34U,0U},
  {1U,1U,33U,0U},
  {1U,11U,32U,0U},
  {1U,10U,31U,0U},
  {1U,4U,30U,0U},
  {1U,13U,29U,0U},
  {1U,8U,28U,0U},
  {1U,14U,27U,0U},
  {1U,8U,26U,0U},
  {1U,1U,25U,0U},
  {1U,11U,24U,0U},
  {1U,12U,23U,0U},
  {1U,21U,22U,0U},
  {1U,0U,21U,0U},
  {1U,12U,20U,0U},
  {1U,5U,19U,0U},
  {1U,3U,18U,0U},
  {1U,1U,17U,0U},
  {1U,3U,16U,0U},
  {1U,4U,15U,0U},
  {1U,6U,14U,0U},
  {1U,1U,13U,0U},
  {1U,6U,12U,0U},
  {1U,8U,11U,0U},
  {1U,1U,10U,0U},
  {1U,4U,9U,0U},
  {1U,2U,8U,0U},
  {1U,7U,7U,0U},
  {1U,4U,6U,0U},
  {1U,4U,5U,0U},
  {1U,0U,4U,0U},
  {1U,0U,3U,0U},
  {1U,0U,2U,0U},
  {1U,1U,1U,0U},
  {1U,0U,0U,0U},
  {2U,34U,1394U,129U},
  {2U,23U,2473U,130U},
  {2U,40U,946U,131U},
  {2U,45U,2016U,132U},
  {2U,45U,2206U,133U},
  {2U,19U,3240U,134U},
  {2U,54U,2825U,135U},
  {2U,43U,3695U,136U},
  {2U,5U,3463U,137U},
  {2U,61U,3921U,138U},
  {2U,33U,1888U,139U},
  {2U,31U,3227U,140U},
  {2U,50U,2758U,141U},
  {2U,18U,1813U,142U},
  {2U,60U,868U,143U},
  {2U,27U,3250U,144U},
  {2U,35U,859U,145U},
  {2U,21U,1991U,146U},
  {2U,23U,2902U,147U},
  {2U,53U,3582U,148U},
  {2U,38U,3051U,149U},
  {2U,3U,1978U,150U},
  {2U,45U,1224U,151U},
  {2U,63U,3240U,152U},
  {2U,62U,2233U,153U},
  {2U,27U,3191U,154U},
  {2U,40U,3309U,155U},
  {2U,62U,2174U,156U},
  {2U,25U,2977U,157U},
  {2U,61U,139U,158U},
  {2U,23U,3082U,159U},
  {2U,33U,2371U,160U},
  {2U,56U,2046U,161U},
  {2U,4U,1721U,162U},
  {2U,8U,221U,163U},
  {2U,49U,99U,164U},
  {2U,52U,2772U,165U},
  {2U,39U,2632U,166U},
  {2U,54U,1452U,167U},
  {2U,54U,1850U,168U},
  {2U,22U,2754U,169U},
  {2U,60U,982U,170U},
  {2U,54U,2486U,171U},
  {2U,28U,3249U,172U},
  {2U,43U,1145U,173U},
  {2U,58U,4028U,174U},
  {2U,22U,3925U,175U},
  {2U,24U,1899U,176U},
  {2U,6U,3286U,177U},
  {2U,46U,1204U,178U},
  {2U,20U,605U,179U},
  {2U,25U,226U,180U},
  {2U,31U,3850U,181U},
  {2U,16U,2241U,182U},
  {2U,30U,3184U,183U},
  {2U,55U,2227U,184U},
  {2U,37U,497U,185U},
  {2U,20U,22U,186U},
  {2U,26U,2007U,187U},
  {2U,53U,3453U,188U},
  {2U,22U,2173U,189U},
  {2U,47U,4089U,190U},
  {2U,42U,1739U,191U},
  {2U,8U,857U,192U},
  {2U,33U,3006U,193U},
  {2U,47U,2438U,194U},
  {2U,20U,972U,195U},
  {2U,40U,3670U,196U},
  {2U,3U,741U,197U},
  {2U,55U,3716U,198U},
  {2U,22U,1403U,199U},
  {2U,45U,2375U,200U},
  {2U,7U,1434U,201U},
  {2U,41U,2320U,202U},
  {2U,36U,3037U,203U},
  {2U,7U,1622U,204U},
  {2U,61U,1496U,205U},
  {2U,38U,2834U,206U},
  {2U,22U,2948U,207U},
  {2U,10U,1712U,208U},
  {2U,40U,462U,209U},
  {2U,32U,522U,210U},
  {2U,59U,1458U,211U},
  {2U,20U,1718U,212U},
  {2U,17U,2035U,213U},
  {2U,41U,3091U,214U},
  {2U,41U,3664U,215U},
  {2U,45U,42U,216U},
  {2U,57U,1708U,217U},
  {2U,24U,2356U,218U},
  {2U,29U,2467U,219U},
  {2U,5U,2552U,220U},
  {2U,31U,2847U,221U},
  {2U,32U,674U,222U},
  {2U,22U,2833U,223U},
  {2U,2U,4031U,224U},
  {2U,52U,3778U,225U},
  {2U,43U,1873U,226U},
  {2U,55U,820U,227U},
  {2U,37U,549U,228U},
  {2U,47U,557U,229U},
  {2U,43U,3974U,230U},
  {2U,48U,1410U,231U},
  {2U,27U,1447U,232U},
  {2U,24U,429U,233U},
  {2U,11U,1251U,234U},
  {2U,3U,2681U,235U},
  {2U,9U,3116U,236U},
  {2U,63U,3772U,237U},
  {2U,0U,2619U,238U},
  {2U,22U,1618U,239U},
  {2U,41U,456U,240U},
  {2U,6U,3945U,241U},
  {2U,56U,3211U,242U},
  {2U,13U,787U,243U},
  {2U,27U,882U,244U},
  {2U,2U,845U,245U},
  {2U,30U,374U,246U},
  {2U,6U,3506U,247U},
  {2U,12U,2720U,248U},
  {2U,26U,3422U,249U},
  {2U,22U,3418U,250U},
  {2U,54U,3320U,251U},
  {2U,20U,3527U,252U},
  {2U,45U,2244U,253U},
  {2U,20U,2502U,254U},
  {2U,29U,2043U,255U},
  {2U,36U,213U,256U},
  {2U,37U,3060U,257U},
  {2U,39U,1190U,258U},
  {2U,54U,1368U,259U},
  {2U,15U,3959U,260U},
  {2U,19U,3050U,261U},
  {2U,17U,2827U,262U},
  {2U,53U,1856U,263U},
  {2U,7U,3798U,264U},
  {2U,43U,2871U,265U},
  {2U,60U,2104U,266U},
  {2U,58U,4022U,267U},
  {2U,55U,3634U,268U},
  {2U,33U,1844U,269U},
  {2U,32U,2504U,270U},
  {2U,40U,982U,271U},
  {2U,29U,606U,272U},
  {2U,61U,1786U,273U},
  {2U,28U,2190U,274U},
  {2U,37U,659U,275U},
  {2U,55U,533U,276U},
  {2U,53U,2400U,277U},
  {2U,32U,1150U,278U},
  {2U,7U,2895U,279U},
  {2U,33U,2665U,280U},
  {2U,46U,3383U,281U},
  {2U,26U,2844U,282U},
  {2U,47U,1953U,283U},
  {2U,32U,3124U,284U},
  {2U,16U,3615U,285U},
  {2U,50U,1667U,286U},
  {2U,28U,2165U,287U},
  {2U,10U,933U,288U},
  {2U,28U,1202U,289U},
  {2U,35U,3572U,290U},
  {2U,61U,3410U,291U},
  {2U,53U,1312U,292U},
  {2U,31U,2378U,293U},
  {2U,21U,246U,294U},
  {2U,38U,1478U,295U},
  {2U,25U,2629U,296U},
  {2U,5U,1695U,297U},
  {2U,61U,2533U,298U},
  {2U,39U,912U,299U},
  {2U,14U,1526U,300U},
  {2U,53U,1920U,301U},
  {2U,15U,1737U,302U},
  {2U,21U,3701U,303U},
  {2U,14U,1489U,304U},
  {2U,12U,3708U,305U},
  {2U,55U,3994U,306U},
  {2U,1U,883U,307U},
  {2U,14U,201U,308U},
  {2U,19U,3012U,309U},
  {2U,59U,2306U,310U},
  {2U,49U,2993U,311U},
  {2U,57U,2814U,312U},
  {2U,42U,1822U,313U},
  {2U,37U,3304U,314U},
  {2U,29U,470U,315U},
  {2U,25U,2070U,316U},
  {2U,52U,3111U,317U},
  {2U,12U,2334U,318U},
  {2U,21U,2561U,319U},
  {2U,41U,2197U,320U},
  {2U,45U,1912U,321U},
  {2U,24U,2769U,322U},
  {2U,0U,3684U,323U},
  {2U,35U,1714U,324U},
  {2U,30U,1440U,325U},
  {2U,53U,2181U,326U},
  {2U,29U,2051U,327U},
  {2U,20U,3368U,328U},
  {2U,23U,3988U,329U},
  {2U,34U,2741U,330U},
  {2U,47U,2255U,331U},
  {2U,11U,1004U,332U},
  {2U,32U,2387U,333U},
  {2U,10U,1618U,334U},
  {2U,2U,825U,335U},
  {2U,15U,3235U,336U},
  {2U,61U,959U,337U},
  {2U,44U,1713U,338U},
  {2U,23U,318U,339U},
  {2U,4U,2431U,340U},
  {2U,30U,1005U,341U},
  {2U,31U,3831U,342U},
  {2U,13U,2105U,343U},
  {2U,32U,3232U,344U},
  {2U,18U,3991U,345U},
  {2U,18U,3760U,346U},
  {2U,10U,2907U,347U},
  {2U,15U,353U,348U},
  {2U,26U,3085U,349U},
  {2U,42U,1306U,350U},
  {2U,24U,1616U,351U},
  {2U,37U,3127U,352U},
  {2U,34U,2596U,353U},
  {2U,47U,1566U,354U},
  {2U,27U,146U,355U},
  {2U,54U,2021U,356U},
  {2U,62U,3565U,357U},
  {2U,32U,349U,358U},
  {2U,45U,1351U,359U},
  {2U,26U,1681U,360U},
  {2U,7U,2179U,361U},
  {2U,54U,3434U,362U},
  {2U,4U,3579U,363U},
  {2U,8U,15U,364U},
  {2U,62U,1122U,365U},
  {2U,22U,2749U,366U},
  {2U,53U,495U,367U},
  {2U,16U,1738U,368U},
  {2U,33U,1661U,369U},
  {2U,15U,850U,370U},
  {2U,40U,4082U,371U},
  {2U,47U,79U,372U},
  {2U,45U,4022U,373U},
  {2U,43U,2378U,374U},
  {2U,33U,155U,375U},
  {2U,46U,2874U,376U},
  {2U,16U,3896U,377U},
  {2U,6U,429U,378U},
  {2U,49U,4006U,379U},
  {2U,58U,1646U,380U},
  {2U,20U,3567U,381U},
  {2U,13U,1058U,382U},
  {2U,6U,1346U,383U},
  {2U,10U,3652U,384U},
  {2U,52U,3847U,385U},
  {2U,4U,1740U,386U},
  {2U,60U,3552U,387U},
  {2U,22U,2915U,388U},
  {2U,41U,2153U,389U},
  {2U,43U,518U,390U},
  {2U,37U,2432U,391U},
  {2U,26U,1207U,392U},
  {2U,42U,320U,393U},
  {2U,43U,3066U,394U},
  {2U,63U,3502U,395U},
  {2U,18U,1623U,396U},
  {2U,34U,3337U,397U},
  {2U,56U,1090U,398U},
  {2U,3U,699U,399U},
  {2U,59U,2476U,400U},
  {2U,50U,1122U,401U},
  {2U,4U,2601U,402U},
  {2U,29U,3462U,403U},
  {2U,15U,280U,404U},
  {2U,42U,3318U,405U},
  {2U,23U,2423U,406U},
  {2U,5U,3727U,407U},
  {2U,41U,2679U,408U},
  {2U,46U,3895U,409U},
  {2U,50U,1211U,410U},
  {2U,63U,1643U,411U},
  {2U,25U,382U,412U},
  {2U,20U,1841U,413U},
  {2U,9U,2238U,414U},
  {2U,54U,4014U,415U},
  {2U,43U,3796U,416U},
  {2U,40U,740U,417U},
  {2U,18U,2676U,418U},
  {2U,56U,2075U,419U},
  {2U,25U,1994U,420U},
  {2U,30U,2832U,421U},
  {2U,34U,1395U,422U},
  {2U,22U,1274U,423U},
  {2U,12U,20U,424U},
  {2U,8U,856U,425U},
  {2U,13U,272U,426U},
  {2U,60U,2575U,427U},
  {2U,58U,196U,428U},
  {2U,15U,3246U,429U},
  {2U,54U,204U,430U},
  {2U,16U,2065U,431U},
  {2U,45U,3697U,432U},
  {2U,34U,265U,433U},
  {2U,63U,259U,434U},
  {2U,21U,1641U,435U},
  {2U,0U,3535U,436U},
  {2U,30U,1582U,437U},
  {2U,50U,2457U,438U},
  {2U,30U,3181U,439U},
  {2U,29U,1873U,440U},
  {2U,46U,1805U,441U},
  {2U,14U,165U,442U},
  {2U,50U,2026U,443U},
  {2U,33U,3029U,444U},
  {2U,41U,2582U,445U},
  {2U,24U,1059U,446U},
  {2U,34U,2372U,447U},
  {2U,45U,2392U,448U},
  {2U,50U,1738U,449U},
  {2U,1U,2263U,450U},
  {2U,32U,2687U,451U},
  {2U,15U,1229U,452U},
  {2U,34U,932U,453U},
  {2U,58U,4050U,454U},
  {2U,17U,2096U,455U},
  {2U,32U,583U,456U},
  {2U,24U,3109U,457U},
  {2U,28U,3691U,458U},
  {2U,43U,913U,459U},
  {2U,45U,2799U,460U},
  {2U,53U,395U,461U},
  {2U,26U,838U,462U},
  {2U,34U,2945U,463U},
  {2U,14U,1472U,464U},
  {2U,10U,3012U,465U},
  {2U,9U,350U,466U},
  {2U,27U,200U,467U},
  {2U,13U,3802U,468U},
  {2U,61U,2800U,469U},
  {2U,5U,2324U,470U},
  {2U,23U,4011U,471U},
  {2U,3U,1154U,472U},
  {2U,40U,1162U,473U},
  {2U,44U,2033U,474U},
  {2U,44U,1859U,475U},
  {2U,24U,3578U,476U},
  {2U,4U,723U,477U},
  {2U,60U,4033U,478U},
  {2U,36U,422U,479U},
  {2U,8U,1617U,480U},
  {2U,57U,2776U,481U},
  {2U,24U,3809U,482U},
  {2U,16U,2711U,483U},
  {2U,26U,2670U,484U},
  {2U,24U,1921U,485U},
  {2U,3U,2523U,486U},
  {2U,49U,886U,487U},
  {2U,8U,1996U,488U},
  {2U,32U,1853U,489U},
  {2U,41U,2448U,490U},
  {2U,22U,2812U,491U},
  {2U,32U,3337U,492U},
  {2U,47U,1976U,493U},
  {2U,41U,1762U,494U},
  {2U,40U,209U,495U},
  {2U,0U,1739U,496U},
  {2U,36U,308U,497U},
  {2U,9U,863U,498U},
  {2U,25U,1335U,499U},
  {2U,21U,3877U,500U},
  {2U,21U,2188U,501U},
  {2U,34U,2162U,502U},
  {2U,48U,3123U,503U},
  {2U,14U,3567U,504U},
  {2U,29U,1854U,505U},
  {2U,25U,814U,506U},
  {2U,63U,2895U,507U},
  {2U,6U,2554U,508U},
  {2U,37U,2835U,509U},
  {2U,32U,2902U,510U},
  {2U,35U,3606U,511U},
  {2U,22U,3673U,512U},
  {2U,14U,2073U,513U},
  {2U,61U,2016U,514U},
  {2U,19U,2206U,515U},
  {2U,56U,2199U,516U},
  {2U,53U,4062U,517U},
  {2U,54U,1220U,518U},
  {2U,52U,3792U,519U},
  {2U,60U,2959U,520U},
  {2U,49U,1402U,521U},
  {2U,30U,208U,522U},
  {2U,20U,2183U,523U},
  {2U,21U,702U,524U},
  {2U,17U,670U,525U},
  {2U,6U,3547U,526U},
  {2U,38U,740U,527U},
  {2U,12U,1746U,528U},
  {2U,31U,2783U,529U},
  {2U,53U,3992U,530U},
  {2U,45U,3368U,531U},
  {2U,37U,3776U,532U},
  {2U,23U,1119U,533U},
  {2U,55U,157U,534U},
  {2U,25U,2467U,535U},
  {2U,37U,2223U,536U},
  {2U,61U,30U,537U},
  {2U,12U,3401U,538U},
  {2U,17U,3643U,539U},
  {2U,44U,1786U,540U},
  {2U,42U,3409U,541U},
  {2U,4U,3591U,542U},
  {2U,63U,992U,543U},
  {2U,61U,3998U,544U},
  {2U,62U,895U,545U},
  {2U,29U,3492U,546U},
  {2U,18U,1304U,547U},
  {2U,57U,3676U,548U},
  {2U,27U,1890U,549U},
  {2U,4U,2741U,550U},
  {2U,37U,2957U,551U},
  {2U,19U,2507U,552U},
  {2U,36U,65U,553U},
  {2U,26U,2705U,554U},
  {2U,3U,3118U,555U},
  {2U,51U,664U,556U},
  {2U,50U,1643U,557U},
  {2U,52U,2052U,558U},
  {2U,49U,465U,559U},
  {2U,59U,2175U,560U},
  {2U,35U,2289U,561U},
  {2U,51U,3691U,562U},
  {2U,28U,2097U,563U},
  {2U,57U,3665U,564U},
  {2U,34U,1950U,565U},
  {2U,1U,748U,566U},
  {2U,23U,1583U,567U},
  {2U,57U,1323U,568U},
  {2U,48U,2463U,569U},
  {2U,24U,359U,570U},
  {2U,51U,2430U,571U},
  {2U,3U,3731U,572U},
  {2U,48U,3087U,573U},
  {2U,0U,3747U,574U},
  {2U,13U,598U,575U},
  {2U,13U,833U,576U},
  {2U,54U,4020U,577U},
  {2U,43U,1533U,578U},
  {2U,21U,798U,579U},
  {2U,21U,3389U,580U},
  {2U,10U,2611U,581U},
  {2U,37U,1872U,582U},
  {2U,24U,1256U,583U},
  {2U,0U,1828U,584U},
  {2U,63U,2418U,585U},
  {2U,48U,2159U,586U},
  {2U,12U,1883U,587U},
  {2U,28U,2335U,588U},
  {2U,20U,4024U,589U},
  {2U,49U,3436U,590U},
  {2U,3U,1061U,591U},
  {2U,23U,2436U,592U},
  {2U,23U,1965U,593U},
  {2U,53U,1661U,594U},
  {2U,36U,1498U,595U},
  {2U,18U,58U,596U},
  {2U,6U,3946U,597U},
  {2U,50U,3830U,598U},
  {2U,55U,1696U,599U},
  {2U,8U,2993U,600U},
  {2U,42U,1049U,601U},
  {2U,37U,3814U,602U},
  {2U,14U,1259U,603U},
  {2U,57U,2574U,604U},
  {2U,44U,1077U,605U},
  {2U,9U,2471U,606U},
  {2U,50U,3538U,607U},
  {2U,8U,2673U,608U},
  {2U,23U,2623U,609U},
  {2U,59U,842U,610U},
  {2U,4U,3741U,611U},
  {2U,13U,973U,612U},
  {2U,49U,2717U,613U},
  {2U,44U,1218U,614U},
  {2U,24U,3303U,615U},
  {2U,24U,358U,616U},
  {2U,9U,2575U,617U},
  {2U,39U,632U,618U},
  {2U,56U,2383U,619U},
  {2U,61U,2078U,620U},
  {2U,31U,2836U,621U},
  {2U,31U,3224U,622U},
  {2U,35U,4063U,623U},
  {2U,55U,1046U,624U},
  {2U,7U,3296U,625U},
  {2U,48U,2588U,626U},
  {2U,58U,369U,627U},
  {2U,27U,2237U,628U},
  {2U,10U,3989U,629U},
  {2U,44U,1718U,630U},
  {2U,16U,1741U,631U},
  {2U,11U,2544U,632U},
  {2U,20U,2819U,633U},
  {2U,1U,926U,634U},
  {2U,63U,3108U,635U},
  {2U,59U,81U,636U},
  {2U,46U,3240U,637U},
  {2U,25U,2578U,638U},
  {2U,23U,2482U,639U},
  {2U,28U,1393U,640U},
  {0U,0U,89U,0U},
  {0U,1U,1029U,0U},
  {0U,2U,3663U,0U},
  {0U,3U,1819U,0U},
  {0U,4U,1506U,0U},
  {0U,5U,738U,0U},
  {0U,6U,1695U,0U},
  {0U,7U,855U,0U},
  {0U,8U,2112U,0U},
  {0U,9U,205U,0U},
  {0U,10U,3748U,0U},
  {0U,11U,1661U,0U},
  {0U,12U,1815U,0U},
  {0U,13U,652U,0U},
  {0U,14U,1627U,0U},
  {0U,15U,2301U,0U},
  {0U,16U,37U,0U},
  {0U,17U,1940U,0U},
  {0U,18U,252U,0U},
  {0U,19U,484U,0U},
  {0U,20U,2894U,0U},
  {0U,21U,3790U,0U},
  {0U,22U,835U,0U},
  {0U,23U,325U,0U},
  {0U,24U,2544U,0U},
  {0U,25U,2813U,0U},
  {0U,26U,2324U,0U},
  {0U,27U,3092U,0U},
  {0U,28U,696U,0U},
  {0U,29U,442U,0U},
  {0U,30U,3844U,0U},
  {0U,31U,1445U,0U},
  {0U,32U,904U,0U},
  {0U,33U,237U,0U},
  {0U,34U,2373U,0U},
  {0U,35U,3619U,0U},
  {0U,36U,2269U,0U},
  {0U,37U,2850U,0U},
  {0U,38U,149U,0U},
  {0U,39U,3060U,0U},
  {0U,40U,1972U,0U},
  {0U,41U,1204U,0U},
  {0U,42U,2191U,0U},
  {0U,43U,1152U,0U},
  {0U,44U,1319U,0U},
  {0U,45U,2138U,0U},
  {0U,46U,3131U,0U},
  {0U,47U,2174U,0U},
  {0U,48U,1261U,0U},
  {0U,49U,2560U,0U},
  {0U,50U,2663U,0U},
  {0U,51U,35U,0U},
  {0U,52U,3560U,0U},
  {0U,53U,1502U,0U},
  {0U,54U,2781U,0U},
  {0U,55U,1657U,0U},
  {0U,56U,2242U,0U},
  {0U,57U,132U,0U},
  {0U,58U,1669U,0U},
  {0U,59U,3827U,0U},
  {0U,60U,1325U,0U},
  {0U,61U,2009U,0U},
  {0U,62U,632U,0U},
  {0U,63U,1558U,0U},
  {1U,1U,63U,0U},
  {1U,19U,62U,0U},
  {1U,52U,61U,0U},
  {1U,4U,60U,0U},
  {1U,13U,59U,0U},
  {1U,23U,58U,0U},
  {1U,25U,57U,0U},
  {1U,9U,56U,0U},
  {1U,44U,55U,0U},
  {1U,10U,54U,0U},
  {1U,12U,53U,0U},
  {1U,30U,52U,0U},
  {1U,20U,51U,0U},
  {1U,1U,50U,0U},
  {1U,48U,49U,0U},
  {1U,30U,48U,0U},
  {1U,22U,47U,0U},
  {1U,9U,46U,0U},
  {1U,29U,45U,0U},
  {1U,40U,44U,0U},
  {1U,11U,43U,0U},
  {1U,27U,42U,0U},
  {1U,14U,41U,0U},
  {1U,6U,40U,0U},
  {1U,37U,39U,0U},
  {1U,31U,38U,0U},
  {1U,25U,37U,0U},
  {1U,11U,36U,0U},
  {1U,34U,35U,0U},
  {1U,21U,34U,0U},
  {1U,29U,33U,0U},
  {1U,13U,32U,0U},
  {1U,8U,31U,0U},
  {1U,23U,30U,0U},
  {1U,20U,29U,0U},
  {1U,6U,28U,0U},
  {1U,27U,27U,0U},
  {1U,10U,26U,0U},
  {1U,18U,25U,0U},
  {1U,13U,24U,0U},
  {1U,23U,23U,0U},
  {1U,8U,22U,0U},
  {1U,18U,21U,0U},
  {1U,18U,20U,0U},
  {1U,17U,19U,0U},
  {1U,0U,18U,0U},
  {1U,0U,17U,0U},
  {1U,2U,16U,0U},
  {1U,12U,15U,0U},
  {1U,5U,14U,0U},
  {1U,4U,13U,0U},
  {1U,2U,12U,0U},
  {1U,5U,11U,0U},
  {1U,4U,10U,0U},
  {1U,6U,9U,0U},
  {1U,6U,8U,0U},
  {1U,2U,7U,0U},
  {1U,5U,6U,0U},
  {1U,0U,5U,0U},
  {1U,4U,4U,0U},
  {1U,0U,3U,0U},
  {1U,2U,2U,0U},
  {1U,0U,1U,0U},
  {1U,0U,0U,0U},
  {2U,35U,870U,129U},
  {2U,47U,3920U,130U},
  {2U,20U,3128U,131U},
  {2U,45U,2536U,132U},
  {2U,40U,1340U,133U},
  {2U,23U,30U,134U},
  {2U,6U,3880U,135U},
  {2U,47U,456U,136U},
  {2U,34U,1294U,137U},
  {2U,37U,2485U,138U},
  {2U,23U,3566U,139U},
  {2U,40U,1439U,140U},
  {2U,1U,2698U,141U},
  {2U,14U,3510U,142U},
  {2U,21U,1809U,143U},
  {2U,41U,1428U,144U},
  {2U,6U,921U,145U},
  {2U,10U,617U,146U},
  {2U,54U,920U,147U},
  {2U,18U,470U,148U},
  {2U,28U,2128U,149U},
  {2U,45U,2588U,150U},
  {2U,5U,1060U,151U},
  {2U,33U,3557U,152U},
  {2U,1U,534U,153U},
  {2U,9U,2590U,154U},
  {2U,53U,3243U,155U},
  {2U,53U,1208U,156U},
  {2U,26U,2970U,157U},
  {2U,8U,4074U,158U},
  {2U,29U,326U,159U},
  {2U,6U,945U,160U},
  {2U,32U,4093U,161U},
  {2U,39U,2382U,162U},
  {2U,49U,2066U,163U},
  {2U,31U,422U,164U},
  {2U,54U,281U,165U},
  {2U,39U,2293U,166U},
  {2U,61U,3158U,167U},
  {2U,49U,3349U,168U},
  {2U,5U,1430U,169U},
  {2U,12U,1491U,170U},
  {2U,26U,151U,171U},
  {2U,57U,3685U,172U},
  {2U,21U,3507U,173U},
  {2U,52U,3463U,174U},
  {2U,54U,2402U,175U},
  {2U,61U,1800U,176U},
  {2U,35U,1586U,177U},
  {2U,32U,2970U,178U},
  {2U,36U,3674U,179U},
  {2U,1U,2374U,180U},
  {2U,38U,3150U,181U},
  {2U,19U,1910U,182U},
  {2U,48U,2547U,183U},
  {2U,1U,3663U,184U},
  {2U,7U,1544U,185U},
  {2U,13U,2122U,186U},
  {2U,38U,2712U,187U},
  {2U,43U,1642U,188U},
  {2U,40U,3948U,189U},
  {2U,4U,2490U,190U},
  {2U,1U,2748U,191U},
  {2U,1U,2512U,192U},
  {2U,46U,1007U,193U},
  {2U,7U,373U,194U},
  {2U,19U,2862U,195U},
  {2U,13U,1640U,196U},
  {2U,62U,810U,197U},
  {2U,26U,2687U,198U},
  {2U,27U,2087U,199U},
  {2U,41U,645U,200U},
  {2U,20U,733U,201U},
  {2U,23U,3230U,202U},
  {2U,23U,402U,203U},
  {2U,42U,1412U,204U},
  {2U,58U,4023U,205U},
  {2U,44U,3007U,206U},
  {2U,46U,3287U,207U},
  {2U,63U,2116U,208U},
  {2U,62U,3628U,209U},
  {2U,54U,3149U,210U},
  {2U,55U,3788U,211U},
  {2U,12U,1218U,212U},
  {2U,3U,2083U,213U},
  {2U,58U,3718U,214U},
  {2U,52U,1592U,215U},
  {2U,40U,2417U,216U},
  {2U,54U,3507U,217U},
  {2U,18U,3359U,218U},
  {2U,8U,2757U,219U},
  {2U,44U,3730U,220U},
  {2U,26U,2738U,221U},
  {2U,35U,136U,222U},
  {2U,23U,1385U,223U},
  {2U,29U,2952U,224U},
  {2U,18U,3105U,225U},
  {2U,56U,2919U,226U},
  {2U,58U,2076U,227U},
  {2U,48U,2002U,228U},
  {2U,48U,1725U,229U},
  {2U,57U,574U,230U},
  {2U,3U,3498U,231U},
  {2U,44U,1672U,232U},
  {2U,34U,792U,233U},
  {2U,24U,183U,234U},
  {2U,15U,68U,235U},
  {2U,60U,2717U,236U},
  {2U,37U,265U,237U},
  {2U,25U,219U,238U},
  {2U,34U,947U,239U},
  {2U,57U,1007U,240U},
  {2U,34U,621U,241U},
  {2U,5U,2907U,242U},
  {2U,34U,2231U,243U},
  {2U,16U,961U,244U},
  {2U,5U,2806U,245U},
  {2U,10U,685U,246U},
  {2U,14U,765U,247U},
  {2U,54U,3588U,248U},
  {2U,48U,282U,249U},
  {2U,46U,1398U,250U},
  {2U,31U,51U,251U},
  {2U,38U,1545U,252U},
  {2U,16U,2673U,253U},
  {2U,38U,3853U,254U},
  {2U,62U,3738U,255U},
  {2U,11U,249U,256U},
  {2U,58U,841U,257U},
  {2U,24U,1735U,258U},
  {2U,40U,3325U,259U},
  {2U,17U,1748U,260U},
  {2U,19U,1934U,261U},
  {2U,33U,3904U,262U},
  {2U,22U,3462U,263U},
  {2U,21U,2510U,264U},
  {2U,60U,1778U,265U},
  {2U,59U,974U,266U},
  {2U,62U,516U,267U},
  {2U,62U,2801U,268U},
  {2U,36U,2835U,269U},
  {2U,0U,3103U,270U},
  {2U,42U,1276U,271U},
  {2U,56U,2903U,272U},
  {2U,14U,1832U,273U},
  {2U,45U,1200U,274U},
  {2U,41U,337U,275U},
  {2U,37U,2466U,276U},
  {2U,39U,367U,277U},
  {2U,29U,1934U,278U},
  {2U,63U,2807U,279U},
  {2U,55U,1398U,280U},
  {2U,17U,1920U,281U},
  {2U,23U,2231U,282U},
  {2U,33U,3763U,283U},
  {2U,52U,1935U,284U},
  {2U,47U,1775U,285U},
  {2U,61U,65U,286U},
  {2U,11U,1123U,287U},
  {2U,16U,957U,288U},
  {2U,59U,2972U,289U},
  {2U,26U,1657U,290U},
  {2U,21U,2889U,291U},
  {2U,27U,2632U,292U},
  {2U,63U,589U,293U},
  {2U,3U,3022U,294U},
  {2U,16U,2623U,295U},
  {2U,5U,2478U,296U},
  {2U,52U,3636U,297U},
  {2U,33U,1571U,298U},
  {2U,20U,2221U,299U},
  {2U,22U,429U,300U},
  {2U,24U,2881U,301U},
  {2U,35U,591U,302U},
  {2U,6U,1280U,303U},
  {2U,49U,1020U,304U},
  {2U,4U,629U,305U},
  {2U,61U,3743U,306U},
  {2U,0U,2837U,307U},
  {2U,19U,3358U,308U},
  {2U,55U,2808U,309U},
  {2U,62U,2586U,310U},
  {2U,3U,4030U,311U},
  {2U,1U,1321U,312U},
  {2U,61U,3520U,313U},
  {2U,28U,1373U,314U},
  {2U,33U,1562U,315U},
  {2U,10U,3705U,316U},
  {2U,2U,562U,317U},
  {2U,13U,2083U,318U},
  {2U,12U,2143U,319U},
  {2U,13U,2412U,320U},
  {2U,15U,3994U,321U},
  {2U,43U,2251U,322U},
  {2U,43U,3100U,323U},
  {2U,41U,79U,324U},
  {2U,4U,861U,325U},
  {2U,63U,3605U,326U},
  {2U,28U,3400U,327U},
  {2U,59U,2525U,328U},
  {2U,11U,60U,329U},
  {2U,57U,709U,330U},
  {2U,54U,2176U,331U},
  {2U,23U,2912U,332U},
  {2U,13U,736U,333U},
  {2U,58U,1752U,334U},
  {2U,59U,2211U,335U},
  {2U,54U,3835U,336U},
  {2U,39U,4051U,337U},
  {2U,4U,3908U,338U},
  {2U,40U,3617U,339U},
  {2U,9U,1736U,340U},
  {2U,9U,366U,341U},
  {2U,43U,886U,342U},
  {2U,11U,2140U,343U},
  {2U,23U,3681U,344U},
  {2U,61U,3696U,345U},
  {2U,47U,253U,346U},
  {2U,34U,3692U,347U},
  {2U,36U,3246U,348U},
  {2U,2U,1884U,349U},
  {2U,36U,3042U,350U},
  {2U,54U,1514U,351U},
  {2U,60U,2476U,352U},
  {2U,12U,2562U,353U},
  {2U,27U,1398U,354U},
  {2U,19U,431U,355U},
  {2U,30U,2059U,356U},
  {2U,41U,3644U,357U},
  {2U,23U,2902U,358U},
  {2U,36U,346U,359U},
  {2U,5U,3201U,360U},
  {2U,46U,3066U,361U},
  {2U,51U,1366U,362U},
  {2U,16U,318U,363U},
  {2U,30U,1459U,364U},
  {2U,8U,619U,365U},
  {2U,20U,1663U,366U},
  {2U,42U,1343U,367U},
  {2U,56U,1364U,368U},
  {2U,55U,2023U,369U},
  {2U,53U,385U,370U},
  {2U,54U,286U,371U},
  {2U,59U,1445U,372U},
  {2U,18U,2752U,373U},
  {2U,45U,1717U,374U},
  {2U,24U,255U,375U},
  {2U,50U,339U,376U},
  {2U,19U,3054U,377U},
  {2U,39U,2897U,378U},
  {2U,49U,2239U,379U},
  {2U,0U,3346U,380U},
  {2U,15U,3049U,381U},
  {2U,52U,205U,382U},
  {2U,51U,1369U,383U},
  {2U,49U,2586U,384U},
  {2U,3U,4051U,385U},
  {2U,34U,2501U,386U},
  {2U,31U,2173U,387U},
  {2U,16U,614U,388U},
  {2U,9U,3018U,389U},
  {2U,29U,2607U,390U},
  {2U,36U,1793U,391U},
  {2U,60U,1351U,392U},
  {2U,9U,2899U,393U},
  {2U,1U,1822U,394U},
  {2U,51U,168U,395U},
  {2U,31U,1417U,396U},
  {2U,30U,1571U,397U},
  {2U,48U,990U,398U},
  {2U,49U,802U,399U},
  {2U,50U,1531U,400U},
  {2U,56U,71U,401U},
  {2U,63U,2148U,402U},
  {2U,41U,1946U,403U},
  {2U,6U,2866U,404U},
  {2U,34U,3747U,405U},
  {2U,57U,3838U,406U},
  {2U,26U,1970U,407U},
  {2U,27U,3651U,408U},
  {2U,49U,3104U,409U},
  {2U,9U,1045U,410U},
  {2U,44U,1556U,411U},
  {2U,34U,2109U,412U},
  {2U,63U,3936U,413U},
  {2U,25U,922U,414U},
  {2U,33U,3747U,415U},
  {2U,36U,2042U,416U},
  {2U,2U,2258U,417U},
  {2U,14U,1038U,418U},
  {2U,11U,246U,419U},
  {2U,31U,3543U,420U},
  {2U,59U,3166U,421U},
  {2U,60U,3141U,422U},
  {2U,54U,2166U,423U},
  {2U,31U,2475U,424U},
  {2U,59U,1660U,425U},
  {2U,6U,3174U,426U},
  {2U,56U,2907U,427U},
  {2U,15U,1690U,428U},
  {2U,19U,3616U,429U},
  {2U,14U,3366U,430U},
  {2U,40U,1594U,431U},
  {2U,3U,3368U,432U},
  {2U,22U,2400U,433U},
  {2U,46U,1524U,434U},
  {2U,25U,393U,435U},
  {2U,61U,2087U,436U},
  {2U,7U,2550U,437U},
  {2U,5U,3969U,438U},
  {2U,52U,1833U,439U},
  {2U,30U,1983U,440U},
  {2U,31U,220U,441U},
  {2U,26U,490U,442U},
  {2U,16U,2835U,443U},
  {2U,8U,2026U,444U},
  {2U,31U,2809U,445U},
  {2U,7U,3980U,446U},
  {2U,33U,1370U,447U},
  {2U,24U,3867U,448U},
  {2U,28U,3872U,449U},
  {2U,37U,1096U,450U},
  {2U,23U,2393U,451U},
  {2U,25U,567U,452U},
  {2U,33U,3649U,453U},
  {2U,16U,1093U,454U},
  {2U,19U,3229U,455U},
  {2U,45U,3256U,456U},
  {2U,36U,3766U,457U},
  {2U,46U,3755U,458U},
  {2U,16U,3601U,459U},
  {2U,60U,862U,460U},
  {2U,37U,96U,461U},
  {2U,63U,2913U,462U},
  {2U,32U,3288U,463U},
  {2U,56U,2342U,464U},
  {2U,30U,3999U,465U},
  {2U,4U,1448U,466U},
  {2U,23U,1969U,467U},
  {2U,11U,2329U,468U},
  {2U,43U,3811U,469U},
  {2U,27U,512U,470U},
  {2U,38U,3935U,471U},
  {2U,53U,1031U,472U},
  {2U,37U,2739U,473U},
  {2U,9U,3290U,474U},
  {2U,9U,2887U,475U},
  {2U,1U,1782U,476U},
  {2U,61U,1570U,477U},
  {2U,33U,3518U,478U},
  {2U,1U,331U,479U},
  {2U,34U,321U,480U},
  {2U,18U,1311U,481U},
  {2U,33U,3328U,482U},
  {2U,56U,1704U,483U},
  {2U,43U,182U,484U},
  {2U,47U,3595U,485U},
  {2U,29U,130U,486U},
  {2U,45U,3025U,487U},
  {2U,7U,162U,488U},
  {2U,25U,300U,489U},
  {2U,58U,1941U,490U},
  {2U,29U,901U,491U},
  {2U,4U,2649U,492U},
  {2U,41U,2764U,493U},
  {2U,3U,3163U,494U},
  {2U,18U,2722U,495U},
  {2U,34U,2086U,496U},
  {2U,42U,2171U,497U},
  {2U,58U,3326U,498U},
  {2U,23U,802U,499U},
  {2U,22U,3312U,500U},
  {2U,17U,3189U,501U},
  {2U,45U,3860U,502U},
  {2U,27U,1495U,503U},
  {2U,44U,4016U,504U},
  {2U,30U,1934U,505U},
  {2U,46U,1306U,506U},
  {2U,61U,2073U,507U},
  {2U,10U,900U,508U},
  {2U,17U,1248U,509U},
  {2U,19U,716U,510U},
  {2U,1U,959U,511U},
  {2U,40U,2121U,512U},
  {2U,8U,2145U,513U},
  {2U,19U,754U,514U},
  {2U,43U,1216U,515U},
  {2U,34U,2853U,516U},
  {2U,41U,1448U,517U},
  {2U,16U,2789U,518U},
  {2U,26U,1585U,519U},
  {2U,49U,471U,520U},
  {2U,51U,3017U,521U},
  {2U,14U,2485U,522U},
  {2U,4U,361U,523U},
  {2U,2U,1051U,524U},
  {2U,21U,3153U,525U},
  {2U,19U,3944U,526U},
  {2U,57U,2058U,527U},
  {2U,46U,1184U,528U},
  {2U,49U,3759U,529U},
  {2U,14U,3274U,530U},
  {2U,36U,1180U,531U},
  {2U,57U,2434U,532U},
  {2U,30U,574U,533U},
  {2U,54U,2990U,534U},
  {2U,9U,2751U,535U},
  {2U,35U,2275U,536U},
  {2U,3U,579U,537U},
  {2U,37U,853U,538U},
  {2U,48U,1600U,539U},
  {2U,13U,1912U,540U},
  {2U,43U,1363U,541U},
  {2U,61U,503U,542U},
  {2U,10U,3117U,543U},
  {2U,20U,1290U,544U},
  {2U,1U,1485U,545U},
  {2U,4U,3490U,546U},
  {2U,52U,624U,547U},
  {2U,24U,848U,548U},
  {2U,40U,3537U,549U},
  {2U,41U,1126U,550U},
  {2U,10U,2177U,551U},
  {2U,38U,2429U,552U},
  {2U,16U,2160U,553U},
  {2U,22U,757U,554U},
  {2U,25U,1282U,555U},
  {2U,60U,1576U,556U},
  {2U,50U,315U,557U},
  {2U,47U,2603U,558U},
  {2U,9U,738U,559U},
  {2U,19U,1296U,560U},
  {2U,39U,548U,561U},
  {2U,10U,3444U,562U},
  {2U,31U,636U,563U},
  {2U,23U,800U,564U},
  {2U,54U,1735U,565U},
  {2U,8U,3822U,566U},
  {2U,14U,547U,567U},
  {2U,50U,1568U,568U},
  {2U,5U,3955U,569U},
  {2U,50U,1263U,570U},
  {2U,22U,1472U,571U},
  {2U,63U,2191U,572U},
  {2U,53U,3825U,573U},
  {2U,61U,3487U,574U},
  {2U,13U,1832U,575U},
  {2U,0U,345U,576U},
  {2U,18U,3599U,577U},
  {2U,47U,999U,578U},
  {2U,44U,2189U,579U},
  {2U,51U,3618U,580U},
  {2U,23U,943U,581U},
  {2U,55U,3757U,582U},
  {2U,13U,2291U,583U},
  {2U,2U,3215U,584U},
  {2U,57U,300U,585U},
  {2U,13U,2999U,586U},
  {2U,24U,1430U,587U},
  {2U,28U,2245U,588U},
  {2U,19U,132U,589U},
  {2U,9U,364U,590U},
  {2U,1U,1360U,591U},
  {2U,0U,2912U,592U},
  {2U,46U,618U,593U},
  {2U,29U,1024U,594U},
  {2U,9U,1510U,595U},
  {2U,20U,3730U,596U},
  {2U,52U,1781U,597U},
  {2U,4U,3318U,598U},
  {2U,20U,51U,599U},
  {2U,0U,2950U,600U},
  {2U,39U,2633U,601U},
  {2U,2U,3760U,602U},
  {2U,28U,1569U,603U},
  {2U,37U,946U,604U},
  {2U,7U,3407U,605U},
  {2U,36U,1306U,606U},
  {2U,33U,3643U,607U},
  {2U,52U,3866U,608U},
  {2U,12U,366U,609U},
  {2U,13U,1209U,610U},
  {2U,58U,2200U,611U},
  {2U,35U,3288U,612U},
  {2U,62U,1216U,613U},
  {2U,46U,252U,614U},
  {2U,1U,1264U,615U},
  {2U,27U,1227U,616U},
  {2U,56U,2001U,617U},
  {2U,47U,1973U,618U},
  {2U,41U,1091U,619U},
  {2U,29U,2519U,620U},
  {2U,37U,3834U,621U},
  {2U,53U,568U,622U},
  {2U,32U,3519U,623U},
  {2U,54U,223U,624U},
  {2U,2U,161U,625U},
  {2U,30U,3579U,626U},
  {2U,7U,722U,627U},
  {2U,47U,2272U,628U},
  {2U,0U,771U,629U},
  {2U,22U,210U,630U},
  {2U,59U,3817U,631U},
  {2U,20U,3706U,632U},
  {2U,19U,882U,633U},
  {2U,28U,1810U,634U},
  {2U,20U,2531U,635U},
  {2U,57U,1851U,636U},
  {2U,54U,3575U,637U},
  {2U,42U,682U,638U},
  {2U,46U,3262U,639U},
  {2U,3U,2560U,640U},
  {0U,0U,714U,0U},
  {0U,1U,40U,0U},
  {0U,2U,2068U,0U},
  {0U,3U,3768U,0U},
  {0U,4U,3004U,0U},
  {0U,5U,1281U,0U},
  {0U,6U,1734U,0U},
  {0U,7U,1522U,0U},
  {0U,8U,2049U,0U},
  {0U,9U,371U,0U},
  {0U,10U,3229U,0U},
  {0U,11U,48U,0U},
  {0U,12U,1847U,0U},
  {0U,13U,527U,0U},
  {0U,14U,1482U,0U},
  {0U,15U,3948U,0U},
  {0U,16U,2208U,0U},
  {0U,17U,837U,0U},
  {0U,18U,2294U,0U},
  {0U,19U,351U,0U},
  {0U,20U,3861U,0U},
  {0U,21U,2718U,0U},
  {0U,22U,353U,0U},
  {0U,23U,2192U,0U},
  {0U,24U,3464U,0U},
  {0U,25U,917U,0U},
  {0U,26U,851U,0U},
  {0U,27U,308U,0U},
  {0U,28U,2372U,0U},
  {0U,29U,1227U,0U},
  {0U,30U,4006U,0U},
  {0U,31U,3150U,0U},
  {0U,32U,2871U,0U},
  {0U,33U,2505U,0U},
  {0U,34U,3664U,0U},
  {0U,35U,1390U,0U},
  {0U,36U,3588U,0U},
  {0U,37U,667U,0U},
  {0U,38U,2266U,0U},
  {0U,39U,1718U,0U},
  {0U,40U,1926U,0U},
  {0U,41U,2619U,0U},
  {0U,42U,57U,0U},
  {0U,43U,3988U,0U},
  {0U,44U,1118U,0U},
  {0U,45U,3047U,0U},
  {0U,46U,2384U,0U},
  {0U,47U,2390U,0U},
  {0U,48U,1995U,0U},
  {0U,49U,3794U,0U},
  {0U,50U,1736U,0U},
  {0U,51U,1122U,0U},
  {0U,52U,13U,0U},
  {0U,53U,291U,0U},
  {0U,54U,2884U,0U},
  {0U,55U,2413U,0U},
  {0U,56U,171U,0U},
  {0U,57U,1103U,0U},
  {0U,58U,4095U,0U},
  {0U,59U,1641U,0U},
  {0U,60U,1083U,0U},
  {0U,61U,2433U,0U},
  {0U,62U,889U,0U},
  {0U,63U,15U,0U},
  {1U,44U,63U,0U},
  {1U,51U,62U,0U},
  {1U,39U,61U,0U},
  {1U,60U,60U,0U},
  {1U,7U,59U,0U},
  {1U,55U,58U,0U},
  {1U,20U,57U,0U},
  {1U,39U,56U,0U},
  {1U,55U,55U,0U},
  {1U,28U,54U,0U},
  {1U,9U,53U,0U},
  {1U,19U,52U,0U},
  {1U,19U,51U,0U},
  {1U,28U,50U,0U},
  {1U,42U,49U,0U},
  {1U,39U,48U,0U},
  {1U,18U,47U,0U},
  {1U,3U,46U,0U},
  {1U,25U,45U,0U},
  {1U,41U,44U,0U},
  {1U,43U,43U,0U},
  {1U,29U,42U,0U},
  {1U,27U,41U,0U},
  {1U,11U,40U,0U},
  {1U,29U,39U,0U},
  {1U,29U,38U,0U},
  {1U,28U,37U,0U},
  {1U,4U,36U,0U},
  {1U,0U,35U,0U},
  {1U,12U,34U,0U},
  {1U,23U,33U,0U},
  {1U,9U,32U,0U},
  {1U,4U,31U,0U},
  {1U,3U,30U,0U},
  {1U,17U,29U,0U},
  {1U,15U,28U,0U},
  {1U,20U,27U,0U},
  {1U,9U,26U,0U},
  {1U,7U,25U,0U},
  {1U,18U,24U,0U},
  {1U,21U,23U,0U},
  {1U,3U,22U,0U},
  {1U,18U,21U,0U},
  {1U,14U,20U,0U},
  {1U,13U,19U,0U},
  {1U,17U,18U,0U},
  {1U,12U,17U,0U},
  {1U,13U,16U,0U},
  {1U,5U,15U,0U},
  {1U,7U,14U,0U},
  {1U,8U,13U,0U},
  {1U,3U,12U,0U},
  {1U,3U,11U,0U},
  {1U,1U,10U,0U},
  {1U,5U,9U,0U},
  {1U,7U,8U,0U},
  {1U,2U,7U,0U},
  {1U,6U,6U,0U},
  {1U,1U,5U,0U},
  {1U,3U,4U,0U},
  {1U,1U,3U,0U},
  {1U,0U,2U,0U},
  {1U,1U,1U,0U},
  {1U,0U,0U,0U},
  {2U,38U,2922U,129U},
  {2U,48U,2933U,130U},
  {2U,2U,2413U,131U},
  {2U,6U,2664U,132U},
  {2U,58U,2853U,133U},
  {2U,11U,4027U,134U},
  {2U,22U,3750U,135U},
  {2U,29U,1387U,136U},
  {2U,3U,3109U,137U},
  {2U,46U,2225U,138U},
  {2U,25U,1691U,139U},
  {2U,40U,243U,140U},
  {2U,22U,3389U,141U},
  {2U,55U,3641U,142U},
  {2U,29U,3258U,143U},
  {2U,4U,3494U,144U},
  {2U,0U,2611U,145U},
  {2U,11U,2810U,146U},
  {2U,12U,2041U,147U},
  {2U,47U,1334U,148U},
  {2U,3U,708U,149U},
  {2U,41U,1666U,150U},
  {2U,43U,397U,151U},
  {2U,44U,1182U,152U},
  {2U,55U,2779U,153U},
  {2U,62U,781U,154U},
  {2U,18U,841U,155U},
  {2U,23U,214U,156U},
  {2U,23U,3290U,157U},
  {2U,1U,2013U,158U},
  {2U,30U,4094U,159U},
  {2U,26U,2892U,160U},
  {2U,12U,1256U,161U},
  {2U,48U,157U,162U},
  {2U,18U,1144U,163U},
  {2U,55U,2179U,164U},
  {2U,15U,3023U,165U},
  {2U,39U,847U,166U},
  {2U,10U,3224U,167U},
  {2U,59U,1933U,168U},
  {2U,50U,1218U,169U},
  {2U,4U,383U,170U},
  {2U,47U,2865U,171U},
  {2U,35U,812U,172U},
  {2U,33U,356U,173U},
  {2U,15U,2350U,174U},
  {2U,43U,3631U,175U},
  {2U,56U,3137U,176U},
  {2U,42U,313U,177U},
  {2U,39U,3213U,178U},
  {2U,14U,665U,179U},
  {2U,16U,3083U,180U},
  {2U,35U,1161U,181U},
  {2U,10U,490U,182U},
  {2U,14U,2007U,183U},
  {2U,20U,2663U,184U},
  {2U,39U,467U,185U},
  {2U,11U,1979U,186U},
  {2U,12U,1252U,187U},
  {2U,34U,3987U,188U},
  {2U,52U,1717U,189U},
  {2U,7U,3385U,190U},
  {2U,61U,1915U,191U},
  {2U,29U,1528U,192U},
  {2U,47U,2596U,193U},
  {2U,50U,473U,194U},
  {2U,30U,1946U,195U},
  {2U,15U,202U,196U},
  {2U,41U,3124U,197U},
  {2U,59U,2026U,198U},
  {2U,21U,3979U,199U},
  {2U,29U,2853U,200U},
  {2U,20U,691U,201U},
  {2U,61U,3093U,202U},
  {2U,2U,1531U,203U},
  {2U,20U,3693U,204U},
  {2U,59U,4058U,205U},
  {2U,23U,2440U,206U},
  {2U,29U,1326U,207U},
  {2U,38U,2788U,208U},
  {2U,56U,2154U,209U},
  {2U,57U,3316U,210U},
  {2U,58U,1863U,211U},
  {2U,49U,2262U,212U},
  {2U,62U,357U,213U},
  {2U,36U,875U,214U},
  {2U,48U,4052U,215U},
  {2U,12U,1418U,216U},
  {2U,9U,1299U,217U},
  {2U,40U,1280U,218U},
  {2U,25U,1716U,219U},
  {2U,57U,1418U,220U},
  {2U,7U,64U,221U},
  {2U,11U,2092U,222U},
  {2U,15U,1628U,223U},
  {2U,25U,506U,224U},
  {2U,29U,1416U,225U},
  {2U,33U,2579U,226U},
  {2U,63U,935U,227U},
  {2U,5U,3987U,228U},
  {2U,19U,2920U,229U},
  {2U,25U,3120U,230U},
  {2U,50U,1229U,231U},
  {2U,35U,1591U,232U},
  {2U,20U,562U,233U},
  {2U,43U,3478U,234U},
  {2U,0U,2735U,235U},
  {2U,37U,13U,236U},
  {2U,28U,212U,237U},
  {2U,2U,597U,238U},
  {2U,50U,942U,239U},
  {2U,52U,3767U,240U},
  {2U,39U,3761U,241U},
  {2U,10U,1820U,242U},
  {2U,25U,2605U,243U},
  {2U,49U,1689U,244U},
  {2U,44U,3065U,245U},
  {2U,43U,3381U,246U},
  {2U,25U,2306U,247U},
  {2U,7U,110U,248U},
  {2U,15U,1146U,249U},
  {2U,7U,2482U,250U},
  {2U,54U,3047U,251U},
  {2U,44U,925U,252U},
  {2U,39U,2943U,253U},
  {2U,34U,3535U,254U},
  {2U,20U,3579U,255U},
  {2U,27U,538U,256U},
  {2U,3U,2517U,257U},
  {2U,61U,296U,258U},
  {2U,61U,1071U,259U},
  {2U,63U,3052U,260U},
  {2U,22U,1554U,261U},
  {2U,4U,2655U,262U},
  {2U,52U,2726U,263U},
  {2U,42U,3328U,264U},
  {2U,31U,3361U,265U},
  {2U,24U,2903U,266U},
  {2U,10U,1971U,267U},
  {2U,62U,700U,268U},
  {2U,48U,923U,269U},
  {2U,55U,492U,270U},
  {2U,47U,2384U,271U},
  {2U,14U,2311U,272U},
  {2U,32U,2960U,273U},
  {2U,53U,1641U,274U},
  {2U,13U,3106U,275U},
  {2U,17U,265U,276U},
  {2U,63U,2866U,277U},
  {2U,18U,1222U,278U},
  {2U,31U,4090U,279U},
  {2U,34U,2726U,280U},
  {2U,28U,3387U,281U},
  {2U,20U,3807U,282U},
  {2U,9U,3939U,283U},
  {2U,26U,1195U,284U},
  {2U,17U,2549U,285U},
  {2U,45U,2075U,286U},
  {2U,18U,735U,287U},
  {2U,25U,2508U,288U},
  {2U,31U,2729U,289U},
  {2U,47U,224U,290U},
  {2U,3U,1536U,291U},
  {2U,53U,1162U,292U},
  {2U,8U,2271U,293U},
  {2U,15U,3250U,294U},
  {2U,63U,618U,295U},
  {2U,14U,3527U,296U},
  {2U,40U,1306U,297U},
  {2U,42U,2044U,298U},
  {2U,48U,438U,299U},
  {2U,45U,1648U,300U},
  {2U,34U,2244U,301U},
  {2U,58U,2371U,302U},
  {2U,33U,1508U,303U},
  {2U,19U,2158U,304U},
  {2U,45U,2411U,305U},
  {2U,44U,27U,306U},
  {2U,11U,2655U,307U},
  {2U,49U,1466U,308U},
  {2U,24U,1995U,309U},
  {2U,56U,3277U,310U},
  {2U,0U,915U,311U},
  {2U,2U,1116U,312U},
  {2U,32U,1931U,313U},
  {2U,54U,3696U,314U},
  {2U,49U,2837U,315U},
  {2U,32U,1229U,316U},
  {2U,58U,1624U,317U},
  {2U,39U,2548U,318U},
  {2U,55U,3950U,319U},
  {2U,30U,3970U,320U},
  {2U,11U,131U,321U},
  {2U,16U,2849U,322U},
  {2U,1U,1883U,323U},
  {2U,1U,555U,324U},
  {2U,50U,3377U,325U},
  {2U,44U,683U,326U},
  {2U,29U,1704U,327U},
  {2U,26U,1952U,328U},
  {2U,48U,1633U,329U},
  {2U,3U,2441U,330U},
  {2U,13U,3462U,331U},
  {2U,58U,3968U,332U},
  {2U,2U,3260U,333U},
  {2U,61U,2523U,334U},
  {2U,53U,2850U,335U},
  {2U,9U,4026U,336U},
  {2U,51U,3078U,337U},
  {2U,32U,38U,338U},
  {2U,56U,2417U,339U},
  {2U,19U,1756U,340U},
  {2U,51U,2818U,341U},
  {2U,60U,3507U,342U},
  {2U,9U,3276U,343U},
  {2U,10U,1750U,344U},
  {2U,60U,2994U,345U},
  {2U,39U,2097U,346U},
  {2U,6U,4002U,347U},
  {2U,13U,2363U,348U},
  {2U,11U,3186U,349U},
  {2U,17U,3340U,350U},
  {2U,1U,2733U,351U},
  {2U,55U,1509U,352U},
  {2U,20U,3247U,353U},
  {2U,13U,2748U,354U},
  {2U,34U,92U,355U},
  {2U,18U,3389U,356U},
  {2U,23U,3445U,357U},
  {2U,22U,921U,358U},
  {2U,21U,3095U,359U},
  {2U,39U,3736U,360U},
  {2U,29U,240U,361U},
  {2U,9U,1966U,362U},
  {2U,30U,985U,363U},
  {2U,34U,329U,364U},
  {2U,3U,1266U,365U},
  {2U,14U,1075U,366U},
  {2U,23U,4095U,367U},
  {2U,59U,2061U,368U},
  {2U,9U,24U,369U},
  {2U,10U,3075U,370U},
  {2U,23U,481U,371U},
  {2U,37U,192U,372U},
  {2U,24U,2996U,373U},
  {2U,18U,3008U,374U},
  {2U,11U,3446U,375U},
  {2U,14U,2135U,376U},
  {2U,42U,1367U,377U},
  {2U,31U,2841U,378U},
  {2U,41U,2248U,379U},
  {2U,21U,3962U,380U},
  {2U,25U,2889U,381U},
  {2U,52U,1763U,382U},
  {2U,30U,2604U,383U},
  {2U,16U,4014U,384U},
  {2U,8U,3060U,385U},
  {2U,44U,618U,386U},
  {2U,3U,2750U,387U},
  {2U,10U,2176U,388U},
  {2U,48U,1614U,389U},
  {2U,17U,2772U,390U},
  {2U,62U,3543U,391U},
  {2U,40U,380U,392U},
  {2U,27U,3091U,393U},
  {2U,62U,112U,394U},
  {2U,41U,1101U,395U},
  {2U,48U,1512U,396U},
  {2U,19U,3416U,397U},
  {2U,11U,1931U,398U},
  {2U,32U,2146U,399U},
  {2U,22U,3355U,400U},
  {2U,56U,882U,401U},
  {2U,6U,3890U,402U},
  {2U,62U,3593U,403U},
  {2U,38U,727U,404U},
  {2U,39U,726U,405U},
  {2U,14U,2019U,406U},
  {2U,34U,4033U,407U},
  {2U,5U,2842U,408U},
  {2U,8U,2457U,409U},
  {2U,8U,211U,410U},
  {2U,49U,2443U,411U},
  {2U,18U,2396U,412U},
  {2U,37U,694U,413U},
  {2U,22U,2574U,414U},
  {2U,6U,2870U,415U},
  {2U,56U,3940U,416U},
  {2U,34U,3920U,417U},
  {2U,62U,18U,418U},
  {2U,28U,3961U,419U},
  {2U,44U,787U,420U},
  {2U,54U,577U,421U},
  {2U,32U,1455U,422U},
  {2U,58U,2755U,423U},
  {2U,10U,3545U,424U},
  {2U,25U,46U,425U},
  {2U,47U,3134U,426U},
  {2U,30U,2295U,427U},
  {2U,56U,3515U,428U},
  {2U,38U,3061U,429U},
  {2U,0U,1854U,430U},
  {2U,9U,3100U,431U},
  {2U,24U,1335U,432U},
  {2U,19U,1326U,433U},
  {2U,23U,3354U,434U},
  {2U,55U,249U,435U},
  {2U,32U,2649U,436U},
  {2U,58U,2294U,437U},
  {2U,22U,187U,438U},
  {2U,32U,1612U,439U},
  {2U,56U,280U,440U},
  {2U,49U,3722U,441U},
  {2U,59U,1140U,442U},
  {2U,34U,972U,443U},
  {2U,42U,1733U,444U},
  {2U,29U,3743U,445U},
  {2U,19U,3069U,446U},
  {2U,1U,2183U,447U},
  {2U,12U,1546U,448U},
  {2U,55U,518U,449U},
  {2U,6U,2038U,450U},
  {2U,21U,765U,451U},
  {2U,38U,197U,452U},
  {2U,11U,3917U,453U},
  {2U,48U,3063U,454U},
  {2U,14U,3250U,455U},
  {2U,26U,1734U,456U},
  {2U,15U,242U,457U},
  {2U,41U,3815U,458U},
  {2U,13U,408U,459U},
  {2U,26U,3229U,460U},
  {2U,61U,2288U,461U},
  {2U,2U,824U,462U},
  {2U,60U,3720U,463U},
  {2U,54U,635U,464U},
  {2U,46U,663U,465U},
  {2U,52U,898U,466U},
  {2U,49U,1554U,467U},
  {2U,18U,463U,468U},
  {2U,63U,2080U,469U},
  {2U,15U,208U,470U},
  {2U,54U,1503U,471U},
  {2U,19U,2467U,472U},
  {2U,58U,3799U,473U},
  {2U,24U,3028U,474U},
  {2U,4U,734U,475U},
  {2U,46U,2272U,476U},
  {2U,5U,959U,477U},
  {2U,17U,1663U,478U},
  {2U,29U,1468U,479U},
  {2U,21U,240U,480U},
  {2U,49U,1679U,481U},
  {2U,7U,3498U,482U},
  {2U,39U,3765U,483U},
  {2U,37U,1023U,484U},
  {2U,27U,2262U,485U},
  {2U,19U,4000U,486U},
  {2U,19U,841U,487U},
  {2U,57U,497U,488U},
  {2U,39U,2339U,489U},
  {2U,3U,3840U,490U},
  {2U,45U,2166U,491U},
  {2U,1U,3917U,492U},
  {2U,62U,1824U,493U},
  {2U,59U,2579U,494U},
  {2U,11U,2095U,495U},
  {2U,57U,4059U,496U},
  {2U,27U,3853U,497U},
  {2U,40U,67U,498U},
  {2U,34U,1542U,499U},
  {2U,28U,871U,500U},
  {2U,36U,2007U,501U},
  {2U,31U,2663U,502U},
  {2U,5U,1774U,503U},
  {2U,16U,2257U,504U},
  {2U,47U,1863U,505U},
  {2U,55U,2150U,506U},
  {2U,42U,647U,507U},
  {2U,22U,2132U,508U},
  {2U,54U,890U,509U},
  {2U,35U,979U,510U},
  {2U,57U,4001U,511U},
  {2U,31U,3559U,512U},
  {2U,63U,2598U,513U},
  {2U,6U,2347U,514U},
  {2U,22U,2405U,515U},
  {2U,4U,3767U,516U},
  {2U,49U,1814U,517U},
  {2U,11U,2788U,518U},
  {2U,27U,2524U,519U},
  {2U,21U,2410U,520U},
  {2U,6U,2578U,521U},
  {2U,43U,1920U,522U},
  {2U,52U,3341U,523U},
  {2U,5U,3664U,524U},
  {2U,7U,626U,525U},
  {2U,22U,1752U,526U},
  {2U,36U,806U,527U},
  {2U,54U,572U,528U},
  {2U,13U,778U,529U},
  {2U,48U,2205U,530U},
  {2U,56U,871U,531U},
  {2U,11U,2638U,532U},
  {2U,63U,2757U,533U},
  {2U,48U,860U,534U},
  {2U,23U,2035U,535U},
  {2U,30U,1123U,536U},
  {2U,53U,2282U,537U},
  {2U,8U,885U,538U},
  {2U,21U,3453U,539U},
  {2U,40U,1031U,540U},
  {2U,36U,278U,541U},
  {2U,15U,1392U,542U},
  {2U,23U,274U,543U},
  {2U,33U,3409U,544U},
  {2U,5U,1396U,545U},
  {2U,35U,3959U,546U},
  {2U,27U,2089U,547U},
  {2U,36U,3043U,548U},
  {2U,36U,791U,549U},
  {2U,44U,156U,550U},
  {2U,37U,3239U,551U},
  {2U,41U,1955U,552U},
  {2U,8U,636U,553U},
  {2U,51U,2670U,554U},
  {2U,59U,1785U,555U},
  {2U,46U,1233U,556U},
  {2U,57U,1372U,557U},
  {2U,7U,2681U,558U},
  {2U,61U,2119U,559U},
  {2U,14U,2935U,560U},
  {2U,52U,2348U,561U},
  {2U,6U,2525U,562U},
  {2U,20U,256U,563U},
  {2U,20U,3423U,564U},
  {2U,41U,3215U,565U},
  {2U,14U,3370U,566U},
  {2U,16U,1619U,567U},
  {2U,48U,3909U,568U},
  {2U,57U,1208U,569U},
  {2U,36U,2617U,570U},
  {2U,33U,2314U,571U},
  {2U,27U,1469U,572U},
  {2U,27U,3872U,573U},
  {2U,55U,871U,574U},
  {2U,22U,2116U,575U},
  {2U,26U,86U,576U},
  {2U,57U,1404U,577U},
  {2U,27U,619U,578U},
  {2U,62U,186U,579U},
  {2U,14U,3547U,580U},
  {2U,23U,3277U,581U},
  {2U,0U,2519U,582U},
  {2U,12U,3324U,583U},
  {2U,26U,422U,584U},
  {2U,32U,3753U,585U},
  {2U,5U,1940U,586U},
  {2U,60U,456U,587U},
  {2U,59U,2128U,588U},
  {2U,11U,53U,589U},
  {2U,22U,934U,590U},
  {2U,21U,3964U,591U},
  {2U,39U,3227U,592U},
  {2U,26U,1258U,593U},
  {2U,35U,1926U,594U},
  {2U,4U,1030U,595U},
  {2U,58U,389U,596U},
  {2U,51U,1047U,597U},
  {2U,34U,1258U,598U},
  {2U,17U,3092U,599U},
  {2U,9U,1298U,600U},
  {2U,29U,2273U,601U},
  {2U,9U,3487U,602U},
  {2U,12U,1775U,603U},
  {2U,4U,2310U,604U},
  {2U,8U,521U,605U},
  {2U,22U,2250U,606U},
  {2U,55U,2325U,607U},
  {2U,4U,2056U,608U},
  {2U,37U,1888U,609U},
  {2U,37U,1521U,610U},
  {2U,7U,515U,611U},
  {2U,13U,1765U,612U},
  {2U,3U,3882U,613U},
  {2U,41U,2773U,614U},
  {2U,48U,1331U,615U},
  {2U,1U,1743U,616U},
  {2U,19U,1424U,617U},
  {2U,3U,1362U,618U},
  {2U,29U,181U,619U},
  {2U,51U,331U,620U},
  {2U,56U,2813U,621U},
  {2U,16U,2619U,622U},
  {2U,1U,988U,623U},
  {2U,44U,3626U,624U},
  {2U,7U,222U,625U},
  {2U,41U,3446U,626U},
  {2U,42U,349U,627U},
  {2U,4U,1547U,628U},
  {2U,60U,822U,629U},
  {2U,63U,3937U,630U},
  {2U,20U,612U,631U},
  {2U,40U,1694U,632U},
  {2U,19U,2727U,633U},
  {2U,48U,2781U,634U},
  {2U,39U,3301U,635U},
  {2U,12U,1931U,636U},
  {2U,50U,2216U,637U},
  {2U,44U,3086U,638U},
  {2U,12U,2502U,639U},
  {2U,29U,2665U,640U},
};
static const unsigned paper_offsets[5] = {0U,640U,1280U,1920U,2560U};
static const unsigned paper_work_starts[4] = {384U,1024U,1664U,2304U};
static const unsigned paper_expected_sums[4] = {37395U,37154U,37407U,36900U};
static const unsigned paper_expected_live[4] = {116425U,139561U,141742U,122023U};

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

