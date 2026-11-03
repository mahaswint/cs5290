#define BUDDY_PIN_LEVELS 0
#define NR_TASKLETS 4
#define POLICY 2
#define LISTS 0
#define LOOKUP 0
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
#define PAPER_KIND 4
/* Variable-size LRU write-cache replay. Metadata stays outside buffer payloads.
 * Exact source request lengths are retained, including direct-bypass requests.
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
static PAPER_INPUT struct PaperOp paper_ops[384]={
  {1U,42932747U,512U,3U},
  {1U,31954535U,6144U,5U},
  {1U,6238199U,57344U,6U},
  {1U,26185655U,8192U,10U},
  {1U,6238311U,4096U,12U},
  {1U,1313768U,3584U,14U},
  {1U,1329916U,4096U,16U},
  {1U,1329924U,1536U,17U},
  {1U,3362311U,4096U,22U},
  {1U,3345071U,4096U,24U},
  {1U,3345071U,16384U,25U},
  {1U,3362287U,16384U,26U},
  {1U,3345079U,4096U,27U},
  {1U,3345071U,16384U,28U},
  {1U,3345071U,4096U,29U},
  {1U,3345071U,4096U,30U},
  {1U,31954543U,5120U,31U},
  {1U,31954551U,1536U,32U},
  {1U,31954551U,2048U,34U},
  {1U,31954551U,5120U,36U},
  {1U,6238319U,4096U,37U},
  {1U,31954559U,3072U,41U},
  {1U,31954559U,4096U,42U},
  {1U,31954559U,5120U,43U},
  {1U,31954567U,1024U,44U},
  {1U,6238327U,53248U,45U},
  {1U,33544823U,4096U,46U},
  {1U,33544239U,4096U,49U},
  {1U,33544295U,4096U,50U},
  {1U,31954567U,5120U,51U},
  {1U,31954575U,2048U,52U},
  {1U,31954575U,3584U,54U},
  {1U,31954575U,4096U,56U},
  {1U,31954575U,4096U,57U},
  {1U,6238431U,4096U,58U},
  {1U,6238439U,4096U,59U},
  {1U,31954575U,4608U,61U},
  {1U,31954583U,1536U,63U},
  {1U,31954583U,2560U,64U},
  {1U,31954583U,3072U,65U},
  {1U,6238447U,49152U,66U},
  {1U,31954583U,5120U,69U},
  {1U,31954591U,2560U,71U},
  {1U,31954591U,3072U,74U},
  {1U,31954591U,5120U,75U},
  {1U,31954599U,2048U,76U},
  {1U,31954599U,8192U,79U},
  {1U,6238543U,57344U,80U},
  {1U,42932755U,512U,81U},
  {1U,42932759U,512U,87U},
  {1U,31954607U,11264U,91U},
  {1U,26185663U,12288U,95U},
  {1U,6238655U,4096U,96U},
  {1U,31954623U,4608U,98U},
  {1U,6238663U,4096U,100U},
  {1U,1313768U,3584U,102U},
  {1U,1329916U,4096U,104U},
  {1U,1329924U,1536U,105U},
  {1U,3362311U,4096U,110U},
  {1U,3345071U,4096U,112U},
  {1U,3345071U,16384U,113U},
  {1U,3362287U,16384U,114U},
  {1U,3345079U,4096U,115U},
  {1U,3345071U,16384U,116U},
  {1U,3345071U,4096U,117U},
  {1U,3345071U,4096U,118U},
  {1U,31954631U,1536U,127U},
  {1U,31954631U,3072U,129U},
  {1U,6238671U,53248U,130U},
  {1U,31954631U,4608U,138U},
  {1U,31954639U,1024U,139U},
  {1U,6238775U,49152U,140U},
  {1U,6238871U,4096U,144U},
  {1U,1313768U,3584U,146U},
  {1U,1329916U,4096U,148U},
  {1U,1329924U,1536U,149U},
  {1U,3363695U,4096U,157U},
  {1U,3364879U,4096U,158U},
  {1U,3365911U,4096U,159U},
  {1U,3345071U,4096U,161U},
  {1U,3345071U,16384U,162U},
  {1U,3364879U,16384U,163U},
  {1U,3345079U,4096U,164U},
  {1U,3345071U,16384U,165U},
  {1U,3363695U,16384U,166U},
  {1U,3365903U,8192U,167U},
  {1U,1297879U,8192U,168U},
  {1U,3345071U,4096U,169U},
  {1U,3345071U,4096U,170U},
  {1U,31954639U,1536U,172U},
  {1U,31954639U,3584U,173U},
  {1U,31954639U,4096U,174U},
  {1U,6238879U,4096U,176U},
  {1U,6238887U,53248U,184U},
  {1U,42932763U,512U,193U},
  {1U,42932767U,512U,198U},
  {1U,42932771U,512U,203U},
  {1U,6238991U,57344U,206U},
  {1U,6239103U,4096U,212U},
  {1U,1313768U,3584U,214U},
  {1U,1329916U,4096U,216U},
  {1U,1329924U,1536U,217U},
  {1U,3362311U,4096U,222U},
  {1U,3345071U,4096U,224U},
  {1U,3345071U,16384U,225U},
  {1U,3362287U,16384U,226U},
  {1U,3345079U,4096U,227U},
  {1U,3345071U,16384U,228U},
  {1U,3345071U,4096U,229U},
  {1U,3345071U,4096U,230U},
  {1U,26185679U,8192U,231U},
  {1U,6239111U,57344U,242U},
  {1U,6239223U,53248U,254U},
  {1U,6239327U,4096U,260U},
  {1U,42932775U,512U,273U},
  {1U,42932779U,512U,278U},
  {1U,6239335U,53248U,280U},
  {1U,26185687U,12288U,289U},
  {1U,6239439U,4096U,290U},
  {1U,6239447U,12288U,294U},
  {1U,1313768U,3584U,296U},
  {1U,1329916U,4096U,298U},
  {1U,1329924U,1536U,299U},
  {1U,1366436U,1536U,306U},
  {1U,1346692U,1536U,308U},
  {1U,1329012U,1536U,310U},
  {1U,1324132U,1536U,312U},
  {1U,1319676U,1536U,314U},
  {1U,1317676U,1536U,316U},
  {1U,1290327U,2560U,317U},
  {1U,1274719U,2560U,319U},
  {1U,6239471U,4096U,322U},
  {1U,3349263U,4096U,323U},
  {1U,3351823U,4096U,324U},
  {1U,3362311U,4096U,325U},
  {1U,3363559U,4096U,326U},
  {1U,3363735U,4096U,327U},
  {1U,3363855U,4096U,328U},
  {1U,3365727U,4096U,329U},
  {1U,3345071U,4096U,333U},
  {1U,3345071U,16384U,334U},
  {1U,3362287U,16384U,335U},
  {1U,3345079U,4096U,336U},
  {1U,3345071U,16384U,337U},
  {1U,3351823U,16384U,339U},
  {1U,3362159U,16384U,341U},
  {1U,3349263U,16384U,342U},
  {1U,3363535U,16384U,343U},
  {1U,3363727U,16384U,344U},
  {1U,3363855U,16384U,345U},
  {1U,3365711U,16384U,347U},
  {1U,3345071U,4096U,348U},
  {1U,3345071U,4096U,349U},
  {1U,6239479U,4096U,356U},
  {1U,42932783U,512U,361U},
  {1U,6239487U,53248U,363U},
  {1U,1313768U,3584U,370U},
  {1U,1329916U,4096U,372U},
  {1U,1329924U,1536U,373U},
  {1U,3363695U,4096U,381U},
  {1U,3364879U,4096U,382U},
  {1U,3365911U,4096U,383U},
  {1U,42932746U,512U,2U},
  {1U,6160447U,4096U,7U},
  {1U,6160431U,4096U,8U},
  {1U,42600911U,2048U,9U},
  {1U,6160455U,4096U,13U},
  {1U,6160447U,4096U,19U},
  {1U,6160455U,4096U,21U},
  {1U,6160447U,4096U,23U},
  {1U,6160455U,4096U,38U},
  {1U,6160447U,4096U,47U},
  {1U,6160439U,4096U,48U},
  {1U,42932750U,1024U,62U},
  {1U,6160455U,4096U,67U},
  {1U,6160431U,4096U,68U},
  {1U,42932754U,512U,78U},
  {1U,6160447U,4096U,82U},
  {1U,6160439U,4096U,83U},
  {1U,42932758U,512U,86U},
  {1U,42600911U,3072U,89U},
  {1U,42600911U,3072U,92U},
  {1U,6160455U,4096U,101U},
  {1U,6160447U,4096U,107U},
  {1U,6160455U,4096U,109U},
  {1U,6160447U,4096U,111U},
  {1U,6160455U,4096U,119U},
  {1U,6160447U,4096U,131U},
  {1U,6160431U,4096U,132U},
  {1U,23516023U,4096U,136U},
  {1U,6160455U,4096U,141U},
  {1U,6160439U,4096U,142U},
  {1U,6160447U,4096U,145U},
  {1U,1386820U,4096U,151U},
  {1U,1386828U,1536U,152U},
  {1U,6160455U,4096U,154U},
  {1U,6160447U,4096U,156U},
  {1U,6160455U,4096U,160U},
  {1U,42932762U,512U,171U},
  {1U,34224959U,512U,175U},
  {1U,6160447U,4096U,177U},
  {1U,34224959U,1024U,179U},
  {1U,34224959U,1536U,180U},
  {1U,34224959U,2560U,181U},
  {1U,34224959U,4096U,182U},
  {1U,34224959U,4608U,183U},
  {1U,6160455U,4096U,185U},
  {1U,6160431U,4096U,186U},
  {1U,34224967U,1024U,188U},
  {1U,34224967U,1024U,189U},
  {1U,34224967U,3072U,190U},
  {1U,34224967U,3584U,192U},
  {1U,42600911U,3072U,195U},
  {1U,42932766U,512U,197U},
  {1U,34224967U,8704U,201U},
  {1U,42932770U,512U,202U},
  {1U,6160447U,4096U,207U},
  {1U,6160439U,4096U,208U},
  {1U,42600911U,4096U,209U},
  {1U,34224983U,5120U,210U},
  {1U,6160455U,4096U,213U},
  {1U,6160447U,4096U,219U},
  {1U,6160455U,4096U,221U},
  {1U,6160447U,4096U,223U},
  {1U,34224991U,1024U,232U},
  {1U,34224991U,1536U,233U},
  {1U,34224991U,5632U,234U},
  {1U,34224999U,4608U,238U},
  {1U,34225007U,512U,239U},
  {1U,34225007U,1536U,240U},
  {1U,34225007U,3072U,241U},
  {1U,6160455U,4096U,243U},
  {1U,6160431U,4096U,244U},
  {1U,34225007U,3072U,245U},
  {1U,34225007U,3072U,246U},
  {1U,34225007U,3584U,248U},
  {1U,34225007U,5120U,249U},
  {1U,34225015U,1536U,250U},
  {1U,34225015U,1536U,251U},
  {1U,34225015U,3584U,253U},
  {1U,6160447U,4096U,255U},
  {1U,6160439U,4096U,256U},
  {1U,34225015U,4096U,257U},
  {1U,34225015U,4608U,263U},
  {1U,34225023U,512U,264U},
  {1U,34225023U,5120U,265U},
  {1U,6160455U,4096U,266U},
  {1U,23516023U,4096U,267U},
  {1U,42932774U,512U,268U},
  {1U,34225031U,2048U,270U},
  {1U,34225031U,4608U,271U},
  {1U,34225039U,2560U,272U},
  {1U,42932778U,512U,277U},
  {1U,34225039U,8192U,279U},
  {1U,6160447U,4096U,281U},
  {1U,6160431U,4096U,282U},
  {1U,42932782U,512U,285U},
  {1U,42600911U,4608U,286U},
  {1U,34225047U,9728U,288U},
  {1U,6160455U,4096U,291U},
  {1U,6160447U,4096U,295U},
  {1U,1386820U,4096U,301U},
  {1U,1386828U,1536U,302U},
  {1U,1389228U,1536U,304U},
  {1U,6160455U,4096U,320U},
  {1U,6160447U,4096U,332U},
  {1U,34225063U,1536U,351U},
  {1U,34225063U,2048U,353U},
  {1U,34225063U,3584U,354U},
  {1U,6160455U,4096U,357U},
  {1U,23516023U,4096U,358U},
  {1U,34225063U,4096U,360U},
  {1U,34225063U,4608U,362U},
  {1U,6160447U,4096U,364U},
  {1U,6160439U,4096U,365U},
  {1U,34225071U,3072U,366U},
  {1U,6160455U,4096U,369U},
  {1U,1386820U,4096U,375U},
  {1U,1386828U,1536U,376U},
  {1U,6160447U,4096U,378U},
  {1U,6160455U,4096U,380U},
  {1U,6160447U,4096U,384U},
  {1U,42932745U,512U,1U},
  {1U,1386815U,2560U,18U},
  {1U,42932749U,512U,55U},
  {1U,42932753U,512U,77U},
  {1U,42932757U,512U,85U},
  {1U,42932761U,512U,94U},
  {1U,1386815U,2560U,106U},
  {1U,6357623U,4096U,123U},
  {1U,14530183U,2048U,134U},
  {1U,22899119U,4096U,137U},
  {1U,1386815U,2560U,150U},
  {1U,1389223U,2560U,153U},
  {1U,42932765U,512U,196U},
  {1U,42932769U,512U,200U},
  {1U,1386815U,2560U,218U},
  {1U,42932773U,512U,252U},
  {1U,6357623U,4096U,259U},
  {1U,42932777U,512U,276U},
  {1U,42932781U,512U,284U},
  {1U,22899119U,4096U,292U},
  {1U,1386815U,2560U,300U},
  {1U,1389223U,2560U,303U},
  {1U,17934903U,4096U,330U},
  {1U,17934903U,16384U,338U},
  {1U,17935031U,16384U,340U},
  {1U,12706335U,512U,352U},
  {1U,22899119U,4096U,359U},
  {1U,1386815U,2560U,374U},
  {1U,1389223U,2560U,377U},
  {1U,40409911U,6656U,4U},
  {1U,1313767U,512U,11U},
  {1U,1329911U,2560U,15U},
  {1U,1313767U,512U,20U},
  {1U,40409919U,2560U,33U},
  {1U,42932748U,512U,35U},
  {1U,6292031U,4096U,39U},
  {1U,40409919U,5632U,40U},
  {1U,40409927U,2048U,53U},
  {1U,6313479U,4096U,60U},
  {1U,40409927U,2048U,70U},
  {1U,6320583U,4096U,72U},
  {1U,42932752U,512U,73U},
  {1U,42932756U,512U,84U},
  {1U,40409927U,4608U,88U},
  {1U,42932760U,512U,90U},
  {1U,40409935U,1024U,93U},
  {1U,6298647U,4096U,97U},
  {1U,1313767U,512U,99U},
  {1U,1329911U,2560U,103U},
  {1U,1313767U,512U,108U},
  {1U,6320583U,4096U,120U},
  {1U,6322327U,4096U,121U},
  {1U,6322343U,4096U,122U},
  {1U,6292031U,4096U,124U},
  {1U,6298647U,4096U,125U},
  {1U,6313479U,4096U,126U},
  {1U,1313783U,4096U,128U},
  {1U,20241375U,4096U,133U},
  {1U,17838687U,4096U,135U},
  {1U,1313767U,512U,143U},
  {1U,1329911U,2560U,147U},
  {1U,1313767U,512U,155U},
  {1U,6292031U,4096U,178U},
  {1U,40409935U,2048U,187U},
  {1U,6320583U,4096U,191U},
  {1U,42932764U,512U,194U},
  {1U,42932768U,512U,199U},
  {1U,40409935U,4096U,204U},
  {1U,40409303U,1024U,205U},
  {1U,1313767U,512U,211U},
  {1U,1329911U,2560U,215U},
  {1U,1313767U,512U,220U},
  {1U,6298647U,4096U,235U},
  {1U,42932772U,512U,236U},
  {1U,40409303U,1024U,237U},
  {1U,1313783U,4096U,247U},
  {1U,6320583U,4096U,258U},
  {1U,6292031U,4096U,261U},
  {1U,6313479U,4096U,262U},
  {1U,40409303U,1536U,269U},
  {1U,40409303U,2048U,274U},
  {1U,42932776U,512U,275U},
  {1U,42932780U,512U,283U},
  {1U,40409303U,4608U,287U},
  {1U,1313767U,512U,293U},
  {1U,1329911U,2560U,297U},
  {1U,1366431U,2560U,305U},
  {1U,1346687U,2560U,307U},
  {1U,1329007U,2560U,309U},
  {1U,1324127U,2560U,311U},
  {1U,1319671U,2560U,313U},
  {1U,1317671U,2560U,315U},
  {1U,1290332U,1536U,318U},
  {1U,1313767U,512U,321U},
  {1U,21809351U,4096U,331U},
  {1U,21809351U,16384U,346U},
  {1U,40409311U,512U,350U},
  {1U,17838687U,4096U,355U},
  {1U,1313783U,4096U,367U},
  {1U,1313767U,512U,368U},
  {1U,1329911U,2560U,371U},
  {1U,1313767U,512U,379U},
};
static const unsigned paper_offsets[5]={0U,162U,282U,311U,384U};
static const unsigned paper_work_starts[4]={67U,187U,290U,339U};
static const unsigned cache_expected_allocations[4]={121U,59U,19U,42U};
static const unsigned cache_expected_frees[4]={57U,27U,1U,8U};
static const unsigned cache_expected_hits[4]={41U,61U,10U,31U};
static const unsigned cache_expected_work_allocations[4]={66U,49U,12U,23U};
static const unsigned cache_expected_work_frees[4]={39U,26U,1U,6U};
static const unsigned cache_expected_work_hits[4]={29U,46U,9U,22U};
static const unsigned cache_expected_live[4]={64U,32U,18U,34U};
static const struct PaperOp cache_expected_final[148]={
  {0,6238543U,57344U,80U},
  {0,42932755U,512U,81U},
  {0,42932759U,512U,87U},
  {0,31954607U,11264U,91U},
  {0,26185663U,12288U,95U},
  {0,6238655U,4096U,96U},
  {0,31954623U,4608U,98U},
  {0,6238663U,4096U,100U},
  {0,6238671U,53248U,130U},
  {0,31954631U,4608U,138U},
  {0,6238775U,49152U,140U},
  {0,6238871U,4096U,144U},
  {0,3365903U,8192U,167U},
  {0,1297879U,8192U,168U},
  {0,31954639U,4096U,174U},
  {0,6238879U,4096U,176U},
  {0,6238887U,53248U,184U},
  {0,42932763U,512U,193U},
  {0,42932767U,512U,198U},
  {0,42932771U,512U,203U},
  {0,6238991U,57344U,206U},
  {0,6239103U,4096U,212U},
  {0,26185679U,8192U,231U},
  {0,6239111U,57344U,242U},
  {0,6239223U,53248U,254U},
  {0,6239327U,4096U,260U},
  {0,42932775U,512U,273U},
  {0,42932779U,512U,278U},
  {0,6239335U,53248U,280U},
  {0,26185687U,12288U,289U},
  {0,6239439U,4096U,290U},
  {0,6239447U,12288U,294U},
  {0,1366436U,1536U,306U},
  {0,1346692U,1536U,308U},
  {0,1329012U,1536U,310U},
  {0,1324132U,1536U,312U},
  {0,1319676U,1536U,314U},
  {0,1317676U,1536U,316U},
  {0,1290327U,2560U,317U},
  {0,1274719U,2560U,319U},
  {0,6239471U,4096U,322U},
  {0,3362311U,4096U,325U},
  {0,3363559U,4096U,326U},
  {0,3363735U,4096U,327U},
  {0,3365727U,4096U,329U},
  {0,3362287U,16384U,335U},
  {0,3345079U,4096U,336U},
  {0,3351823U,16384U,339U},
  {0,3362159U,16384U,341U},
  {0,3349263U,16384U,342U},
  {0,3363535U,16384U,343U},
  {0,3363727U,16384U,344U},
  {0,3363855U,16384U,345U},
  {0,3365711U,16384U,347U},
  {0,3345071U,4096U,349U},
  {0,6239479U,4096U,356U},
  {0,42932783U,512U,361U},
  {0,6239487U,53248U,363U},
  {0,1313768U,3584U,370U},
  {0,1329916U,4096U,372U},
  {0,1329924U,1536U,373U},
  {0,3363695U,4096U,381U},
  {0,3364879U,4096U,382U},
  {0,3365911U,4096U,383U},
  {0,42932746U,512U,2U},
  {0,42932750U,1024U,62U},
  {0,42932754U,512U,78U},
  {0,42932758U,512U,86U},
  {0,42932762U,512U,171U},
  {0,34224959U,4608U,183U},
  {0,42932766U,512U,197U},
  {0,34224967U,8704U,201U},
  {0,42932770U,512U,202U},
  {0,34224983U,5120U,210U},
  {0,34224991U,5632U,234U},
  {0,34224999U,4608U,238U},
  {0,34225007U,5120U,249U},
  {0,34225015U,4608U,263U},
  {0,34225023U,5120U,265U},
  {0,42932774U,512U,268U},
  {0,34225031U,4608U,271U},
  {0,42932778U,512U,277U},
  {0,34225039U,8192U,279U},
  {0,6160431U,4096U,282U},
  {0,42932782U,512U,285U},
  {0,42600911U,4608U,286U},
  {0,34225047U,9728U,288U},
  {0,1389228U,1536U,304U},
  {0,23516023U,4096U,358U},
  {0,34225063U,4608U,362U},
  {0,6160439U,4096U,365U},
  {0,34225071U,3072U,366U},
  {0,1386820U,4096U,375U},
  {0,1386828U,1536U,376U},
  {0,6160455U,4096U,380U},
  {0,6160447U,4096U,384U},
  {0,42932745U,512U,1U},
  {0,42932749U,512U,55U},
  {0,42932753U,512U,77U},
  {0,42932757U,512U,85U},
  {0,42932761U,512U,94U},
  {0,14530183U,2048U,134U},
  {0,42932765U,512U,196U},
  {0,42932769U,512U,200U},
  {0,42932773U,512U,252U},
  {0,6357623U,4096U,259U},
  {0,42932777U,512U,276U},
  {0,42932781U,512U,284U},
  {0,17934903U,16384U,338U},
  {0,17935031U,16384U,340U},
  {0,12706335U,512U,352U},
  {0,22899119U,4096U,359U},
  {0,1386815U,2560U,374U},
  {0,1389223U,2560U,377U},
  {0,40409911U,6656U,4U},
  {0,42932748U,512U,35U},
  {0,40409919U,5632U,40U},
  {0,42932752U,512U,73U},
  {0,42932756U,512U,84U},
  {0,40409927U,4608U,88U},
  {0,42932760U,512U,90U},
  {0,6322327U,4096U,121U},
  {0,6322343U,4096U,122U},
  {0,20241375U,4096U,133U},
  {0,42932764U,512U,194U},
  {0,42932768U,512U,199U},
  {0,40409935U,4096U,204U},
  {0,6298647U,4096U,235U},
  {0,42932772U,512U,236U},
  {0,6320583U,4096U,258U},
  {0,6292031U,4096U,261U},
  {0,6313479U,4096U,262U},
  {0,42932776U,512U,275U},
  {0,42932780U,512U,283U},
  {0,40409303U,4608U,287U},
  {0,1366431U,2560U,305U},
  {0,1346687U,2560U,307U},
  {0,1329007U,2560U,309U},
  {0,1324127U,2560U,311U},
  {0,1319671U,2560U,313U},
  {0,1317671U,2560U,315U},
  {0,1290332U,1536U,318U},
  {0,21809351U,16384U,346U},
  {0,40409311U,512U,350U},
  {0,17838687U,4096U,355U},
  {0,1313783U,4096U,367U},
  {0,1329911U,2560U,371U},
  {0,1313767U,512U,379U},
};
static const unsigned cache_expected_offsets[5]={0U,64U,96U,114U,148U};

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

struct CacheEntry { uint32_t pointer;unsigned key,size,tag,touch; };
static struct CacheEntry cache_entries[NR_TASKLETS][POOL_SIZE];
static unsigned cache_clock[NR_TASKLETS],cache_counts[NR_TASKLETS][6];
static unsigned payload_word(unsigned key,unsigned tag,unsigned index) {
  return key^tag^(index*0x9e3779b1U);
}
static void payload_write(struct CacheEntry *e) {
  unsigned buffer[64] __attribute__((aligned(8)));
  for(unsigned offset=0;offset<e->size;offset+=256) {
    unsigned n=e->size-offset;if(n>256)n=256;
    for(unsigned j=0;j<n/4;++j)buffer[j]=payload_word(e->key,e->tag,offset/4+j);
    mram_write(buffer,(__mram_ptr void *)e->pointer+offset,n);
  }
}
static void payload_check(struct CacheEntry *e) {
  unsigned buffer[64] __attribute__((aligned(8)));
  for(unsigned offset=0;offset<e->size;offset+=256) {
    unsigned n=e->size-offset;if(n>256)n=256;
    mram_read((__mram_ptr void *)e->pointer+offset,buffer,n);
    for(unsigned j=0;j<n/4;++j)assert(buffer[j]==payload_word(e->key,e->tag,offset/4+j));
  }
}
static void cache_release(struct CacheEntry *e,unsigned timed) {
  payload_check(e);bench_free((__mram_ptr void *)e->pointer,e->size);e->pointer=0;
  cache_counts[me()][1]++;if(timed)cache_counts[me()][4]++;
}
static __attribute__((noinline)) void apply_op(unsigned i) {
  unsigned tid=me(),timed=i>=paper_work_starts[tid];
  struct PaperOp op __attribute__((aligned(8)));input_read(&paper_ops[i],&op,16);
  assert(((op.index^(op.index>>16))%NR_TASKLETS)==tid);
  struct CacheEntry *entry=0,*empty=0,*oldest=0;
  for(unsigned j=0;j<POOL_SIZE;++j) {
    struct CacheEntry *e=&cache_entries[tid][j];
    if(!e->pointer){empty=e;continue;}
    if(e->key==op.index)entry=e;
    if(!oldest || e->touch<oldest->touch)oldest=e;
  }
  if(entry && entry->size!=op.value){cache_release(entry,timed);empty=entry;entry=0;}
  if(entry) {
    payload_check(entry);cache_counts[tid][2]++;if(timed)cache_counts[tid][5]++;
    if(op.kind){entry->tag=op.tag;payload_write(entry);}else assert(entry->tag==op.tag);
  } else {
    entry=empty ? empty : oldest;assert(entry);
    if(entry->pointer)cache_release(entry,timed);
    entry->pointer=(uint32_t)bench_alloc(op.value);entry->key=op.index;
    entry->size=op.value;entry->tag=op.tag;payload_write(entry);
    cache_counts[tid][0]++;if(timed)cache_counts[tid][3]++;
  }
  entry->touch=++cache_clock[tid];
}
static __attribute__((noinline)) void app_check(void) {
  unsigned tid=me(),live=0;
  assert(cache_counts[tid][0]==cache_expected_allocations[tid]);
  assert(cache_counts[tid][1]==cache_expected_frees[tid]);
  assert(cache_counts[tid][2]==cache_expected_hits[tid]);
  assert(cache_counts[tid][3]==cache_expected_work_allocations[tid]);
  assert(cache_counts[tid][4]==cache_expected_work_frees[tid]);
  assert(cache_counts[tid][5]==cache_expected_work_hits[tid]);
  for(unsigned j=0;j<POOL_SIZE;++j) {
    struct CacheEntry *e=&cache_entries[tid][j];if(!e->pointer)continue;
    payload_check(e);unsigned found=0;
    for(unsigned k=cache_expected_offsets[tid];k<cache_expected_offsets[tid+1];++k) {
      struct PaperOp expected=cache_expected_final[k];
      if(expected.index==e->key){assert(expected.value==e->size && expected.tag==e->tag);found++;}
    }
    assert(found==1);live++;
    for(unsigned k=0;k<j;++k)assert(!cache_entries[tid][k].pointer || cache_entries[tid][k].key!=e->key);
  }
  assert(live==cache_expected_live[tid]);
}
static void app_destroy(void) {
  for(unsigned j=0;j<POOL_SIZE;++j) {
    struct CacheEntry *e=&cache_entries[me()][j];
    if(e->pointer){payload_check(e);bench_free((__mram_ptr void *)e->pointer,e->size);e->pointer=0;}
  }
}
static __attribute__((noinline)) void perform_work(unsigned start,unsigned end) {
  BEGIN(work);for(unsigned i=start;i<end;++i)apply_op(i);
  barrier_wait(&policy_barrier);END(work);
}
static __attribute__((noinline)) void perform_drain(void) {
  BEGIN(drain);app_destroy();policy_drain(me());barrier_wait(&policy_barrier);END(drain);
}
int main(void) {
  unsigned tid=me();
  if(!tid){mem_reset();BEGIN(total);BEGIN(init);policy_init(HEAP_BYTES);}
  barrier_wait(&policy_barrier);
  for(unsigned i=paper_offsets[tid];i<paper_work_starts[tid];++i)apply_op(i);
  barrier_wait(&policy_barrier);if(!tid)init_end();barrier_wait(&policy_barrier);
  perform_work(paper_work_starts[tid],paper_offsets[tid+1]);
  if(!tid)work_end();barrier_wait(&policy_barrier);app_check();
  if(VALIDATE)policy_verify(tid);barrier_wait(&policy_barrier);
  perform_drain();if(!tid)final_report();return 0;
}
