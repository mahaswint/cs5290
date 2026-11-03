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
#define TRACE_SEED 9173
#define REPEATS 2
#define PAPER_KIND 0
#define SLAB_BYTES 65536
/* Owner-local Larson worker port. Original sizes, random victim selection,
 * Fisher-Yates preconditioning and 4*N warmup; bounded work replaces wall time.
 */
/* Slabs + indexed lists/ownership + fixed/adaptive empty-span retention.
 * Original small-class geometry and 24 B header; no payload migration.
 * retained/EMPTY_BUDGET count physical 4 KiB units, not slab count.
 */
#ifndef SLAB_BYTES
#define SLAB_BYTES 0
#endif
#ifndef LISTS
#define LISTS 1
#endif
#ifndef LOOKUP
#define LOOKUP 1
#endif
#ifndef POLICY
#define POLICY 3
#endif
#if !LISTS || !LOOKUP
#error "The stacked allocator requires indexed lists and ownership lookup"
#endif
#if SLAB_BYTES != 0 && SLAB_BYTES != 16384 && SLAB_BYTES != 32768 && SLAB_BYTES != 65536
#error "SLAB_BYTES must be 0, 16384, 32768, or 65536"
#endif
#if SLAB_BYTES == 0
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

#else
#if POLICY != 2 && POLICY != 3
#error "Slab retention supports fixed (2) and adaptive (3) policies"
#endif
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
#define POLICY_CLASSES 9
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
/* One 8 B MRAM entry per physical 4 KiB payload leaf. For slab objects,
 * tag packs tasklet/class with a 4-bit class field, and bytes stores the containing span's head
 * index. Every leaf is registered and cleared, including payload tail leaves.
 * Direct allocations register their base only; bytes is their buddy block size.
 * No absolute slab alignment is assumed: all indices use the actual heap base.
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
static unsigned policy_span(struct PolicyCache *c) {
  return c->size == 4096 ? SLAB_BYTES : POLICY_PAGE;
}
static unsigned policy_units(struct PolicyCache *c) { return policy_span(c) >> 12; }
static unsigned policy_capacity(struct PolicyCache *c) {
  unsigned n = (policy_span(c) - 24) / c->size;
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
static void policy_owner_span(__mram_ptr void *p, unsigned span, unsigned tag, unsigned head) {
  for (unsigned offset = 0; offset < span; offset += POLICY_PAGE)
    policy_owner_write(p + offset, tag, head);
}
static void policy_add_page(struct PolicyCache *c, __mram_ptr void *p, unsigned tid) {
  unsigned bitmap[4] __attribute__((aligned(8))) = {~0U, ~0U, ~0U, ~0U};
  policy_push(c, p, 1); mram_write(bitmap, p + 8, 16);
#if LOOKUP
  policy_owner_span(p, policy_span(c), 1 + (tid << 4) + (c - policy_caches[tid]), policy_page_index(p));
#endif
  c->pages++; c->empty++; policy_stats[tid].retained += policy_units(c);
}
static void policy_release_page(struct PolicyCache *c, __mram_ptr void *p, unsigned tid) {
  policy_unlink(c, p, 1);
  unsigned units = policy_units(c);
  assert(c->empty && c->pages && policy_stats[tid].retained >= units);
  c->empty--; c->pages--; policy_stats[tid].retained -= units; policy_stats[tid].returns++;
#if LOOKUP
  policy_owner_span(p, policy_span(c), 0, 0);
#endif
  assert(policy_backend_free(p) == policy_span(c));
}
static unsigned policy_trim_cache(struct PolicyCache *c, unsigned keep, unsigned tid) {
  unsigned removed = 0;
#if LISTS
  while (c->empty > keep) {
    assert(c->empty_root); policy_release_page(c, c->empty_root, tid); removed += policy_units(c);
  }
#else
  __mram_ptr void *page = c->root;
  while (page && c->empty > keep) {
    uint32_t links[2] __attribute__((aligned(8)));
    unsigned bitmap[4] __attribute__((aligned(8)));
    policy_links(page, links); mram_read(page + 8, bitmap, 16);
    if (policy_empty(bitmap)) { policy_release_page(c, page, tid); removed += policy_units(c); }
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
      __mram_ptr void *p = policy_backend_alloc(policy_span(c));
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
          assert(c->empty && s->retained >= policy_units(c)); c->empty--; s->retained -= policy_units(c);
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
    page = policy_backend_alloc(policy_span(c));
    if (!page && POLICY == 3 && s->retained) {
      unsigned before = s->retained; policy_drain(tid); s->emergency_trims += before;
      page = policy_backend_alloc(policy_span(c));
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
  assert(p >= page + 24 && p < page + policy_span(c) && offset % c->size == 0);
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
    c->empty++; s->retained += policy_units(c);
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
    unsigned owner = info.tag - 1;
    assert((owner >> 4) == tid && (owner & 15) < POLICY_CLASSES);
    struct PolicyCache *c = &policy_caches[tid][owner & 15];
    assert(info.bytes <= page_index && page_index - info.bytes < policy_units(c));
    __mram_ptr void *page = policy_buddy->mramMallocableRegionBase + (info.bytes << 12);
    policy_free_small(c, page, p, tid); return;
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
        if (p >= page + 24 && p < page + policy_span(c)) {
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
        unsigned head = policy_page_index(page);
        for (unsigned leaf = 0; leaf < policy_units(c); ++leaf) {
          struct PolicyOwner info __attribute__((aligned(8)));
          mram_read(&policy_owners[head + leaf], &info, 8);
          assert(info.tag == 1 + (tid << 4) + i && info.bytes == head);
        }
#endif
        previous = (uint32_t)page; page = (__mram_ptr void *)links[1]; assert(++pages <= c->pages);
      }
    }
    assert(pages == c->pages && empty == c->empty); total_empty += empty * policy_units(c);
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
  {0U,0U,1843U,0U},
  {0U,1U,3521U,0U},
  {0U,2U,3087U,0U},
  {0U,3U,837U,0U},
  {0U,4U,1528U,0U},
  {0U,5U,727U,0U},
  {0U,6U,165U,0U},
  {0U,7U,3164U,0U},
  {0U,8U,2262U,0U},
  {0U,9U,265U,0U},
  {0U,10U,2563U,0U},
  {0U,11U,1000U,0U},
  {0U,12U,108U,0U},
  {0U,13U,1500U,0U},
  {0U,14U,1826U,0U},
  {0U,15U,1282U,0U},
  {0U,16U,2101U,0U},
  {0U,17U,3714U,0U},
  {0U,18U,3740U,0U},
  {0U,19U,552U,0U},
  {0U,20U,3763U,0U},
  {0U,21U,21U,0U},
  {0U,22U,550U,0U},
  {0U,23U,3922U,0U},
  {0U,24U,236U,0U},
  {0U,25U,1972U,0U},
  {0U,26U,198U,0U},
  {0U,27U,2341U,0U},
  {0U,28U,3616U,0U},
  {0U,29U,542U,0U},
  {0U,30U,2267U,0U},
  {0U,31U,3507U,0U},
  {0U,32U,1118U,0U},
  {0U,33U,3729U,0U},
  {0U,34U,601U,0U},
  {0U,35U,2015U,0U},
  {0U,36U,2845U,0U},
  {0U,37U,4006U,0U},
  {0U,38U,1745U,0U},
  {0U,39U,912U,0U},
  {0U,40U,3460U,0U},
  {0U,41U,3295U,0U},
  {0U,42U,3991U,0U},
  {0U,43U,3413U,0U},
  {0U,44U,2764U,0U},
  {0U,45U,1606U,0U},
  {0U,46U,4036U,0U},
  {0U,47U,1014U,0U},
  {0U,48U,1732U,0U},
  {0U,49U,3582U,0U},
  {0U,50U,694U,0U},
  {0U,51U,2334U,0U},
  {0U,52U,3752U,0U},
  {0U,53U,4092U,0U},
  {0U,54U,30U,0U},
  {0U,55U,1502U,0U},
  {0U,56U,1473U,0U},
  {0U,57U,3617U,0U},
  {0U,58U,1724U,0U},
  {0U,59U,1393U,0U},
  {0U,60U,1256U,0U},
  {0U,61U,3855U,0U},
  {0U,62U,3406U,0U},
  {0U,63U,3611U,0U},
  {1U,39U,63U,0U},
  {1U,55U,62U,0U},
  {1U,21U,61U,0U},
  {1U,40U,60U,0U},
  {1U,24U,59U,0U},
  {1U,6U,58U,0U},
  {1U,15U,57U,0U},
  {1U,53U,56U,0U},
  {1U,18U,55U,0U},
  {1U,0U,54U,0U},
  {1U,35U,53U,0U},
  {1U,48U,52U,0U},
  {1U,11U,51U,0U},
  {1U,13U,50U,0U},
  {1U,46U,49U,0U},
  {1U,14U,48U,0U},
  {1U,43U,47U,0U},
  {1U,30U,46U,0U},
  {1U,14U,45U,0U},
  {1U,19U,44U,0U},
  {1U,21U,43U,0U},
  {1U,3U,42U,0U},
  {1U,19U,41U,0U},
  {1U,38U,40U,0U},
  {1U,12U,39U,0U},
  {1U,1U,38U,0U},
  {1U,28U,37U,0U},
  {1U,11U,36U,0U},
  {1U,10U,35U,0U},
  {1U,4U,34U,0U},
  {1U,23U,33U,0U},
  {1U,26U,32U,0U},
  {1U,14U,31U,0U},
  {1U,16U,30U,0U},
  {1U,15U,29U,0U},
  {1U,23U,28U,0U},
  {1U,27U,27U,0U},
  {1U,13U,26U,0U},
  {1U,14U,25U,0U},
  {1U,11U,24U,0U},
  {1U,23U,23U,0U},
  {1U,16U,22U,0U},
  {1U,8U,21U,0U},
  {1U,6U,20U,0U},
  {1U,8U,19U,0U},
  {1U,11U,18U,0U},
  {1U,8U,17U,0U},
  {1U,9U,16U,0U},
  {1U,15U,15U,0U},
  {1U,11U,14U,0U},
  {1U,9U,13U,0U},
  {1U,1U,12U,0U},
  {1U,10U,11U,0U},
  {1U,6U,10U,0U},
  {1U,2U,9U,0U},
  {1U,3U,8U,0U},
  {1U,1U,7U,0U},
  {1U,2U,6U,0U},
  {1U,4U,5U,0U},
  {1U,0U,4U,0U},
  {1U,2U,3U,0U},
  {1U,0U,2U,0U},
  {1U,1U,1U,0U},
  {1U,0U,0U,0U},
  {2U,20U,3410U,129U},
  {2U,42U,3426U,130U},
  {2U,25U,3291U,131U},
  {2U,1U,1774U,132U},
  {2U,18U,2104U,133U},
  {2U,43U,1771U,134U},
  {2U,0U,48U,135U},
  {2U,23U,4018U,136U},
  {2U,20U,2223U,137U},
  {2U,29U,3210U,138U},
  {2U,40U,2326U,139U},
  {2U,30U,825U,140U},
  {2U,50U,2529U,141U},
  {2U,56U,2587U,142U},
  {2U,16U,3719U,143U},
  {2U,1U,1708U,144U},
  {2U,41U,3092U,145U},
  {2U,56U,442U,146U},
  {2U,19U,3102U,147U},
  {2U,59U,3364U,148U},
  {2U,63U,1678U,149U},
  {2U,20U,2235U,150U},
  {2U,26U,2296U,151U},
  {2U,25U,744U,152U},
  {2U,32U,453U,153U},
  {2U,15U,2841U,154U},
  {2U,47U,2651U,155U},
  {2U,63U,2888U,156U},
  {2U,46U,1945U,157U},
  {2U,52U,1220U,158U},
  {2U,37U,3245U,159U},
  {2U,22U,1111U,160U},
  {2U,61U,1730U,161U},
  {2U,33U,1764U,162U},
  {2U,33U,1276U,163U},
  {2U,57U,1219U,164U},
  {2U,61U,1973U,165U},
  {2U,10U,1597U,166U},
  {2U,45U,2048U,167U},
  {2U,32U,2686U,168U},
  {2U,25U,2941U,169U},
  {2U,5U,926U,170U},
  {2U,29U,87U,171U},
  {2U,58U,708U,172U},
  {2U,10U,1232U,173U},
  {2U,23U,1067U,174U},
  {2U,19U,3100U,175U},
  {2U,52U,2211U,176U},
  {2U,50U,233U,177U},
  {2U,28U,3957U,178U},
  {2U,29U,2640U,179U},
  {2U,31U,4006U,180U},
  {2U,57U,3552U,181U},
  {2U,55U,739U,182U},
  {2U,47U,578U,183U},
  {2U,29U,1142U,184U},
  {2U,3U,2338U,185U},
  {2U,59U,2119U,186U},
  {2U,4U,912U,187U},
  {2U,6U,3989U,188U},
  {2U,46U,1469U,189U},
  {2U,39U,1279U,190U},
  {2U,37U,1314U,191U},
  {2U,18U,3834U,192U},
  {2U,27U,116U,193U},
  {2U,7U,1205U,194U},
  {2U,15U,92U,195U},
  {2U,15U,3594U,196U},
  {2U,60U,3459U,197U},
  {2U,34U,612U,198U},
  {2U,53U,2467U,199U},
  {2U,44U,2159U,200U},
  {2U,15U,1104U,201U},
  {2U,60U,1856U,202U},
  {2U,30U,1455U,203U},
  {2U,16U,3989U,204U},
  {2U,9U,1113U,205U},
  {2U,39U,3163U,206U},
  {2U,30U,2730U,207U},
  {2U,19U,3798U,208U},
  {2U,25U,2131U,209U},
  {2U,6U,1914U,210U},
  {2U,62U,1762U,211U},
  {2U,38U,3858U,212U},
  {2U,41U,3166U,213U},
  {2U,24U,1363U,214U},
  {2U,57U,3889U,215U},
  {2U,29U,1952U,216U},
  {2U,23U,3375U,217U},
  {2U,62U,2976U,218U},
  {2U,12U,2002U,219U},
  {2U,15U,1014U,220U},
  {2U,10U,4032U,221U},
  {2U,49U,3969U,222U},
  {2U,43U,2071U,223U},
  {2U,6U,3005U,224U},
  {2U,30U,4021U,225U},
  {2U,12U,3196U,226U},
  {2U,4U,3147U,227U},
  {2U,50U,3910U,228U},
  {2U,31U,2463U,229U},
  {2U,41U,2364U,230U},
  {2U,16U,3555U,231U},
  {2U,29U,3345U,232U},
  {2U,1U,3724U,233U},
  {2U,41U,2664U,234U},
  {2U,21U,4072U,235U},
  {2U,21U,2555U,236U},
  {2U,42U,3540U,237U},
  {2U,43U,3179U,238U},
  {2U,33U,793U,239U},
  {2U,9U,2005U,240U},
  {2U,47U,3670U,241U},
  {2U,5U,2736U,242U},
  {2U,15U,1744U,243U},
  {2U,8U,1369U,244U},
  {2U,17U,576U,245U},
  {2U,2U,2869U,246U},
  {2U,44U,3741U,247U},
  {2U,42U,1492U,248U},
  {2U,48U,1760U,249U},
  {2U,37U,3008U,250U},
  {2U,24U,315U,251U},
  {2U,61U,3258U,252U},
  {2U,7U,2882U,253U},
  {2U,15U,1952U,254U},
  {2U,8U,2950U,255U},
  {2U,1U,3442U,256U},
  {2U,0U,426U,257U},
  {2U,7U,1975U,258U},
  {2U,35U,2196U,259U},
  {2U,24U,1448U,260U},
  {2U,56U,2070U,261U},
  {2U,45U,439U,262U},
  {2U,46U,351U,263U},
  {2U,7U,759U,264U},
  {2U,63U,3837U,265U},
  {2U,33U,3326U,266U},
  {2U,44U,1620U,267U},
  {2U,12U,3417U,268U},
  {2U,17U,1525U,269U},
  {2U,39U,38U,270U},
  {2U,37U,2386U,271U},
  {2U,11U,1470U,272U},
  {2U,9U,2250U,273U},
  {2U,44U,1910U,274U},
  {2U,15U,3413U,275U},
  {2U,7U,2421U,276U},
  {2U,60U,2464U,277U},
  {2U,1U,1819U,278U},
  {2U,1U,3316U,279U},
  {2U,3U,1126U,280U},
  {2U,47U,2267U,281U},
  {2U,7U,1694U,282U},
  {2U,3U,409U,283U},
  {2U,31U,1346U,284U},
  {2U,26U,3561U,285U},
  {2U,56U,3384U,286U},
  {2U,18U,2189U,287U},
  {2U,34U,983U,288U},
  {2U,30U,1307U,289U},
  {2U,1U,2292U,290U},
  {2U,28U,2740U,291U},
  {2U,8U,3875U,292U},
  {2U,63U,3746U,293U},
  {2U,44U,3376U,294U},
  {2U,59U,370U,295U},
  {2U,13U,1014U,296U},
  {2U,16U,1364U,297U},
  {2U,9U,3214U,298U},
  {2U,19U,2483U,299U},
  {2U,30U,3159U,300U},
  {2U,40U,2809U,301U},
  {2U,10U,1426U,302U},
  {2U,55U,49U,303U},
  {2U,44U,509U,304U},
  {2U,63U,2792U,305U},
  {2U,3U,3011U,306U},
  {2U,21U,2863U,307U},
  {2U,54U,1592U,308U},
  {2U,57U,1885U,309U},
  {2U,57U,3903U,310U},
  {2U,41U,859U,311U},
  {2U,19U,1533U,312U},
  {2U,7U,2943U,313U},
  {2U,47U,1831U,314U},
  {2U,51U,829U,315U},
  {2U,35U,1355U,316U},
  {2U,12U,3083U,317U},
  {2U,26U,2534U,318U},
  {2U,22U,600U,319U},
  {2U,63U,760U,320U},
  {2U,0U,2591U,321U},
  {2U,49U,2376U,322U},
  {2U,11U,1722U,323U},
  {2U,0U,364U,324U},
  {2U,30U,1218U,325U},
  {2U,21U,3363U,326U},
  {2U,59U,439U,327U},
  {2U,17U,1896U,328U},
  {2U,40U,489U,329U},
  {2U,4U,2389U,330U},
  {2U,59U,3287U,331U},
  {2U,48U,2418U,332U},
  {2U,62U,47U,333U},
  {2U,62U,1672U,334U},
  {2U,51U,1353U,335U},
  {2U,63U,1806U,336U},
  {2U,6U,1736U,337U},
  {2U,26U,988U,338U},
  {2U,14U,3802U,339U},
  {2U,52U,425U,340U},
  {2U,51U,1495U,341U},
  {2U,22U,2786U,342U},
  {2U,50U,1415U,343U},
  {2U,62U,3782U,344U},
  {2U,38U,3703U,345U},
  {2U,45U,2719U,346U},
  {2U,29U,1146U,347U},
  {2U,5U,3735U,348U},
  {2U,29U,3963U,349U},
  {2U,55U,2090U,350U},
  {2U,45U,1819U,351U},
  {2U,8U,421U,352U},
  {2U,2U,2635U,353U},
  {2U,40U,3136U,354U},
  {2U,41U,1417U,355U},
  {2U,12U,3518U,356U},
  {2U,54U,2773U,357U},
  {2U,44U,1911U,358U},
  {2U,40U,3390U,359U},
  {2U,9U,683U,360U},
  {2U,17U,2336U,361U},
  {2U,13U,825U,362U},
  {2U,38U,2882U,363U},
  {2U,28U,775U,364U},
  {2U,19U,2633U,365U},
  {2U,56U,244U,366U},
  {2U,3U,3568U,367U},
  {2U,12U,1943U,368U},
  {2U,11U,1328U,369U},
  {2U,13U,2600U,370U},
  {2U,11U,2506U,371U},
  {2U,10U,816U,372U},
  {2U,51U,2120U,373U},
  {2U,51U,3812U,374U},
  {2U,63U,357U,375U},
  {2U,62U,3207U,376U},
  {2U,8U,794U,377U},
  {2U,60U,2194U,378U},
  {2U,25U,99U,379U},
  {2U,45U,54U,380U},
  {2U,29U,3797U,381U},
  {2U,0U,3441U,382U},
  {2U,15U,1115U,383U},
  {2U,60U,3272U,384U},
  {2U,2U,1573U,385U},
  {2U,38U,2867U,386U},
  {2U,42U,3119U,387U},
  {2U,40U,102U,388U},
  {2U,5U,2567U,389U},
  {2U,13U,3925U,390U},
  {2U,6U,3632U,391U},
  {2U,52U,2922U,392U},
  {2U,2U,561U,393U},
  {2U,25U,2785U,394U},
  {2U,32U,240U,395U},
  {2U,9U,3516U,396U},
  {2U,6U,3346U,397U},
  {2U,40U,2901U,398U},
  {2U,21U,2132U,399U},
  {2U,26U,3365U,400U},
  {2U,63U,3359U,401U},
  {2U,55U,3758U,402U},
  {2U,12U,2890U,403U},
  {2U,17U,1497U,404U},
  {2U,27U,2883U,405U},
  {2U,31U,326U,406U},
  {2U,19U,2288U,407U},
  {2U,8U,2066U,408U},
  {2U,4U,1532U,409U},
  {2U,52U,1468U,410U},
  {2U,61U,1945U,411U},
  {2U,21U,1691U,412U},
  {2U,49U,2051U,413U},
  {2U,10U,1846U,414U},
  {2U,34U,3301U,415U},
  {2U,5U,422U,416U},
  {2U,17U,2998U,417U},
  {2U,11U,1707U,418U},
  {2U,63U,2717U,419U},
  {2U,27U,1892U,420U},
  {2U,45U,3875U,421U},
  {2U,43U,3886U,422U},
  {2U,57U,951U,423U},
  {2U,53U,3226U,424U},
  {2U,2U,3391U,425U},
  {2U,14U,2378U,426U},
  {2U,42U,3790U,427U},
  {2U,11U,1472U,428U},
  {2U,49U,2714U,429U},
  {2U,7U,2058U,430U},
  {2U,51U,201U,431U},
  {2U,15U,3882U,432U},
  {2U,59U,1997U,433U},
  {2U,16U,3442U,434U},
  {2U,10U,1692U,435U},
  {2U,61U,3859U,436U},
  {2U,29U,3265U,437U},
  {2U,52U,964U,438U},
  {2U,45U,4092U,439U},
  {2U,53U,1786U,440U},
  {2U,4U,2359U,441U},
  {2U,18U,829U,442U},
  {2U,0U,3477U,443U},
  {2U,14U,1652U,444U},
  {2U,38U,913U,445U},
  {2U,41U,2056U,446U},
  {2U,2U,608U,447U},
  {2U,19U,1356U,448U},
  {2U,8U,3110U,449U},
  {2U,42U,624U,450U},
  {2U,57U,2227U,451U},
  {2U,16U,557U,452U},
  {2U,36U,3342U,453U},
  {2U,4U,2544U,454U},
  {2U,49U,3668U,455U},
  {2U,49U,290U,456U},
  {2U,34U,396U,457U},
  {2U,9U,1630U,458U},
  {2U,12U,3336U,459U},
  {2U,7U,1354U,460U},
  {2U,58U,2871U,461U},
  {2U,52U,3834U,462U},
  {2U,19U,408U,463U},
  {2U,63U,895U,464U},
  {2U,59U,2819U,465U},
  {2U,40U,3539U,466U},
  {2U,25U,1705U,467U},
  {2U,45U,237U,468U},
  {2U,13U,2185U,469U},
  {2U,45U,2980U,470U},
  {2U,32U,3930U,471U},
  {2U,10U,933U,472U},
  {2U,20U,2220U,473U},
  {2U,36U,2114U,474U},
  {2U,37U,2591U,475U},
  {2U,61U,3649U,476U},
  {2U,10U,3343U,477U},
  {2U,6U,3879U,478U},
  {2U,54U,3727U,479U},
  {2U,58U,3063U,480U},
  {2U,20U,2351U,481U},
  {2U,7U,2500U,482U},
  {2U,5U,1922U,483U},
  {2U,54U,1924U,484U},
  {2U,11U,3473U,485U},
  {2U,7U,3855U,486U},
  {2U,63U,504U,487U},
  {2U,53U,3409U,488U},
  {2U,46U,1118U,489U},
  {2U,61U,1072U,490U},
  {2U,59U,3440U,491U},
  {2U,16U,186U,492U},
  {2U,47U,2411U,493U},
  {2U,7U,4005U,494U},
  {2U,26U,1446U,495U},
  {2U,47U,3167U,496U},
  {2U,0U,3698U,497U},
  {2U,3U,1345U,498U},
  {2U,24U,455U,499U},
  {2U,11U,4052U,500U},
  {2U,37U,3353U,501U},
  {2U,22U,369U,502U},
  {2U,62U,2958U,503U},
  {2U,38U,3706U,504U},
  {2U,7U,3345U,505U},
  {2U,5U,1765U,506U},
  {2U,1U,3274U,507U},
  {2U,24U,1983U,508U},
  {2U,22U,621U,509U},
  {2U,2U,496U,510U},
  {2U,7U,962U,511U},
  {2U,46U,1691U,512U},
  {2U,32U,48U,513U},
  {2U,27U,3767U,514U},
  {2U,4U,2935U,515U},
  {2U,38U,2162U,516U},
  {2U,10U,2188U,517U},
  {2U,51U,3621U,518U},
  {2U,1U,2318U,519U},
  {2U,11U,2197U,520U},
  {2U,59U,1424U,521U},
  {2U,34U,4009U,522U},
  {2U,46U,1776U,523U},
  {2U,49U,702U,524U},
  {2U,20U,374U,525U},
  {2U,10U,85U,526U},
  {2U,14U,1391U,527U},
  {2U,49U,2996U,528U},
  {2U,37U,23U,529U},
  {2U,50U,3596U,530U},
  {2U,40U,1974U,531U},
  {2U,34U,488U,532U},
  {2U,58U,2262U,533U},
  {2U,46U,1735U,534U},
  {2U,63U,784U,535U},
  {2U,60U,90U,536U},
  {2U,54U,564U,537U},
  {2U,8U,3108U,538U},
  {2U,19U,1532U,539U},
  {2U,40U,2234U,540U},
  {2U,8U,3991U,541U},
  {2U,3U,3181U,542U},
  {2U,7U,2184U,543U},
  {2U,49U,3238U,544U},
  {2U,31U,3271U,545U},
  {2U,52U,3066U,546U},
  {2U,13U,397U,547U},
  {2U,6U,1295U,548U},
  {2U,21U,779U,549U},
  {2U,54U,322U,550U},
  {2U,7U,2229U,551U},
  {2U,45U,264U,552U},
  {2U,25U,3085U,553U},
  {2U,53U,919U,554U},
  {2U,15U,358U,555U},
  {2U,10U,2505U,556U},
  {2U,8U,924U,557U},
  {2U,61U,765U,558U},
  {2U,27U,1630U,559U},
  {2U,28U,2554U,560U},
  {2U,37U,1500U,561U},
  {2U,30U,889U,562U},
  {2U,50U,1514U,563U},
  {2U,29U,1548U,564U},
  {2U,42U,1823U,565U},
  {2U,36U,1508U,566U},
  {2U,36U,2581U,567U},
  {2U,61U,2785U,568U},
  {2U,8U,641U,569U},
  {2U,18U,1216U,570U},
  {2U,10U,557U,571U},
  {2U,3U,1602U,572U},
  {2U,9U,1050U,573U},
  {2U,37U,2446U,574U},
  {2U,25U,933U,575U},
  {2U,12U,3268U,576U},
  {2U,61U,1965U,577U},
  {2U,17U,2450U,578U},
  {2U,56U,2129U,579U},
  {2U,57U,2836U,580U},
  {2U,30U,1992U,581U},
  {2U,59U,3513U,582U},
  {2U,51U,1501U,583U},
  {2U,8U,149U,584U},
  {2U,53U,601U,585U},
  {2U,50U,3096U,586U},
  {2U,16U,1647U,587U},
  {2U,41U,1180U,588U},
  {2U,35U,1881U,589U},
  {2U,25U,2157U,590U},
  {2U,9U,3722U,591U},
  {2U,32U,3242U,592U},
  {2U,4U,231U,593U},
  {2U,50U,1966U,594U},
  {2U,31U,2225U,595U},
  {2U,47U,3615U,596U},
  {2U,12U,1545U,597U},
  {2U,51U,1274U,598U},
  {2U,32U,49U,599U},
  {2U,42U,2164U,600U},
  {2U,17U,417U,601U},
  {2U,10U,2167U,602U},
  {2U,11U,3890U,603U},
  {2U,63U,3519U,604U},
  {2U,49U,2802U,605U},
  {2U,25U,2076U,606U},
  {2U,25U,3556U,607U},
  {2U,12U,3897U,608U},
  {2U,34U,3046U,609U},
  {2U,1U,2184U,610U},
  {2U,19U,1377U,611U},
  {2U,60U,781U,612U},
  {2U,47U,1656U,613U},
  {2U,43U,151U,614U},
  {2U,22U,433U,615U},
  {2U,8U,389U,616U},
  {2U,37U,213U,617U},
  {2U,31U,3388U,618U},
  {2U,45U,1123U,619U},
  {2U,1U,2146U,620U},
  {2U,22U,3337U,621U},
  {2U,6U,1298U,622U},
  {2U,5U,635U,623U},
  {2U,43U,1966U,624U},
  {2U,19U,535U,625U},
  {2U,41U,1726U,626U},
  {2U,56U,2832U,627U},
  {2U,18U,2680U,628U},
  {2U,21U,3215U,629U},
  {2U,47U,1705U,630U},
  {2U,5U,1070U,631U},
  {2U,62U,1304U,632U},
  {2U,16U,2800U,633U},
  {2U,19U,3953U,634U},
  {2U,38U,2390U,635U},
  {2U,10U,3194U,636U},
  {2U,46U,2609U,637U},
  {2U,0U,2173U,638U},
  {2U,19U,3695U,639U},
  {2U,48U,111U,640U},
  {0U,0U,338U,0U},
  {0U,1U,314U,0U},
  {0U,2U,577U,0U},
  {0U,3U,3060U,0U},
  {0U,4U,2629U,0U},
  {0U,5U,2033U,0U},
  {0U,6U,3550U,0U},
  {0U,7U,3355U,0U},
  {0U,8U,2520U,0U},
  {0U,9U,3100U,0U},
  {0U,10U,2637U,0U},
  {0U,11U,1406U,0U},
  {0U,12U,1862U,0U},
  {0U,13U,1151U,0U},
  {0U,14U,3872U,0U},
  {0U,15U,3490U,0U},
  {0U,16U,2912U,0U},
  {0U,17U,357U,0U},
  {0U,18U,2433U,0U},
  {0U,19U,1386U,0U},
  {0U,20U,1583U,0U},
  {0U,21U,2807U,0U},
  {0U,22U,1395U,0U},
  {0U,23U,2581U,0U},
  {0U,24U,1828U,0U},
  {0U,25U,3228U,0U},
  {0U,26U,3884U,0U},
  {0U,27U,814U,0U},
  {0U,28U,3485U,0U},
  {0U,29U,286U,0U},
  {0U,30U,4053U,0U},
  {0U,31U,3236U,0U},
  {0U,32U,358U,0U},
  {0U,33U,2867U,0U},
  {0U,34U,1983U,0U},
  {0U,35U,2990U,0U},
  {0U,36U,620U,0U},
  {0U,37U,1342U,0U},
  {0U,38U,2081U,0U},
  {0U,39U,2143U,0U},
  {0U,40U,71U,0U},
  {0U,41U,3953U,0U},
  {0U,42U,3178U,0U},
  {0U,43U,3918U,0U},
  {0U,44U,2772U,0U},
  {0U,45U,1761U,0U},
  {0U,46U,406U,0U},
  {0U,47U,2823U,0U},
  {0U,48U,2939U,0U},
  {0U,49U,1212U,0U},
  {0U,50U,2582U,0U},
  {0U,51U,244U,0U},
  {0U,52U,2778U,0U},
  {0U,53U,2650U,0U},
  {0U,54U,2269U,0U},
  {0U,55U,3366U,0U},
  {0U,56U,1691U,0U},
  {0U,57U,3890U,0U},
  {0U,58U,1098U,0U},
  {0U,59U,116U,0U},
  {0U,60U,1570U,0U},
  {0U,61U,1540U,0U},
  {0U,62U,1402U,0U},
  {0U,63U,100U,0U},
  {1U,59U,63U,0U},
  {1U,31U,62U,0U},
  {1U,55U,61U,0U},
  {1U,45U,60U,0U},
  {1U,32U,59U,0U},
  {1U,26U,58U,0U},
  {1U,20U,57U,0U},
  {1U,9U,56U,0U},
  {1U,53U,55U,0U},
  {1U,33U,54U,0U},
  {1U,16U,53U,0U},
  {1U,43U,52U,0U},
  {1U,6U,51U,0U},
  {1U,39U,50U,0U},
  {1U,42U,49U,0U},
  {1U,0U,48U,0U},
  {1U,15U,47U,0U},
  {1U,6U,46U,0U},
  {1U,35U,45U,0U},
  {1U,10U,44U,0U},
  {1U,41U,43U,0U},
  {1U,9U,42U,0U},
  {1U,29U,41U,0U},
  {1U,3U,40U,0U},
  {1U,14U,39U,0U},
  {1U,1U,38U,0U},
  {1U,13U,37U,0U},
  {1U,30U,36U,0U},
  {1U,5U,35U,0U},
  {1U,11U,34U,0U},
  {1U,22U,33U,0U},
  {1U,30U,32U,0U},
  {1U,11U,31U,0U},
  {1U,16U,30U,0U},
  {1U,17U,29U,0U},
  {1U,6U,28U,0U},
  {1U,21U,27U,0U},
  {1U,22U,26U,0U},
  {1U,11U,25U,0U},
  {1U,22U,24U,0U},
  {1U,11U,23U,0U},
  {1U,21U,22U,0U},
  {1U,6U,21U,0U},
  {1U,10U,20U,0U},
  {1U,1U,19U,0U},
  {1U,3U,18U,0U},
  {1U,3U,17U,0U},
  {1U,12U,16U,0U},
  {1U,10U,15U,0U},
  {1U,14U,14U,0U},
  {1U,9U,13U,0U},
  {1U,9U,12U,0U},
  {1U,6U,11U,0U},
  {1U,1U,10U,0U},
  {1U,9U,9U,0U},
  {1U,2U,8U,0U},
  {1U,2U,7U,0U},
  {1U,1U,6U,0U},
  {1U,4U,5U,0U},
  {1U,0U,4U,0U},
  {1U,0U,3U,0U},
  {1U,0U,2U,0U},
  {1U,0U,1U,0U},
  {1U,0U,0U,0U},
  {2U,47U,2062U,129U},
  {2U,49U,612U,130U},
  {2U,0U,3963U,131U},
  {2U,2U,2256U,132U},
  {2U,39U,1939U,133U},
  {2U,9U,89U,134U},
  {2U,13U,2657U,135U},
  {2U,53U,517U,136U},
  {2U,4U,1129U,137U},
  {2U,61U,1286U,138U},
  {2U,60U,1540U,139U},
  {2U,11U,2565U,140U},
  {2U,54U,3710U,141U},
  {2U,26U,778U,142U},
  {2U,38U,2893U,143U},
  {2U,14U,1070U,144U},
  {2U,53U,2764U,145U},
  {2U,38U,3316U,146U},
  {2U,1U,1446U,147U},
  {2U,48U,2329U,148U},
  {2U,47U,1103U,149U},
  {2U,53U,3893U,150U},
  {2U,13U,3633U,151U},
  {2U,48U,293U,152U},
  {2U,39U,4082U,153U},
  {2U,58U,2467U,154U},
  {2U,16U,1612U,155U},
  {2U,16U,4095U,156U},
  {2U,6U,2299U,157U},
  {2U,17U,1425U,158U},
  {2U,12U,3483U,159U},
  {2U,53U,1098U,160U},
  {2U,59U,3083U,161U},
  {2U,50U,1192U,162U},
  {2U,61U,882U,163U},
  {2U,58U,3655U,164U},
  {2U,51U,824U,165U},
  {2U,41U,2203U,166U},
  {2U,37U,2119U,167U},
  {2U,1U,3021U,168U},
  {2U,41U,3144U,169U},
  {2U,23U,1941U,170U},
  {2U,52U,2433U,171U},
  {2U,44U,320U,172U},
  {2U,30U,2210U,173U},
  {2U,7U,1308U,174U},
  {2U,15U,4084U,175U},
  {2U,21U,1385U,176U},
  {2U,13U,1557U,177U},
  {2U,1U,3449U,178U},
  {2U,63U,3748U,179U},
  {2U,45U,1018U,180U},
  {2U,40U,3383U,181U},
  {2U,39U,2902U,182U},
  {2U,46U,3468U,183U},
  {2U,21U,973U,184U},
  {2U,58U,1940U,185U},
  {2U,0U,3661U,186U},
  {2U,48U,167U,187U},
  {2U,44U,2763U,188U},
  {2U,17U,1729U,189U},
  {2U,1U,274U,190U},
  {2U,47U,3535U,191U},
  {2U,54U,2422U,192U},
  {2U,3U,3205U,193U},
  {2U,39U,2965U,194U},
  {2U,9U,3839U,195U},
  {2U,21U,3868U,196U},
  {2U,40U,3750U,197U},
  {2U,18U,2141U,198U},
  {2U,49U,425U,199U},
  {2U,37U,1155U,200U},
  {2U,45U,565U,201U},
  {2U,9U,3343U,202U},
  {2U,58U,1651U,203U},
  {2U,0U,1499U,204U},
  {2U,51U,947U,205U},
  {2U,7U,3472U,206U},
  {2U,24U,2499U,207U},
  {2U,0U,2816U,208U},
  {2U,17U,2063U,209U},
  {2U,62U,1455U,210U},
  {2U,0U,3624U,211U},
  {2U,35U,1916U,212U},
  {2U,55U,1136U,213U},
  {2U,16U,3952U,214U},
  {2U,42U,1617U,215U},
  {2U,37U,3774U,216U},
  {2U,2U,3776U,217U},
  {2U,34U,1731U,218U},
  {2U,13U,3860U,219U},
  {2U,16U,1247U,220U},
  {2U,61U,223U,221U},
  {2U,56U,3638U,222U},
  {2U,39U,43U,223U},
  {2U,22U,961U,224U},
  {2U,6U,2650U,225U},
  {2U,2U,2557U,226U},
  {2U,8U,2485U,227U},
  {2U,47U,1377U,228U},
  {2U,23U,3808U,229U},
  {2U,38U,273U,230U},
  {2U,31U,3843U,231U},
  {2U,1U,2813U,232U},
  {2U,2U,2545U,233U},
  {2U,31U,3925U,234U},
  {2U,32U,993U,235U},
  {2U,58U,1500U,236U},
  {2U,35U,1123U,237U},
  {2U,38U,54U,238U},
  {2U,61U,1346U,239U},
  {2U,4U,338U,240U},
  {2U,45U,3934U,241U},
  {2U,53U,1667U,242U},
  {2U,32U,331U,243U},
  {2U,33U,1553U,244U},
  {2U,56U,1376U,245U},
  {2U,1U,3061U,246U},
  {2U,43U,1788U,247U},
  {2U,23U,1964U,248U},
  {2U,54U,3160U,249U},
  {2U,41U,1142U,250U},
  {2U,6U,1465U,251U},
  {2U,0U,192U,252U},
  {2U,1U,3798U,253U},
  {2U,7U,2344U,254U},
  {2U,40U,2053U,255U},
  {2U,47U,1281U,256U},
  {2U,12U,3609U,257U},
  {2U,39U,2151U,258U},
  {2U,15U,2518U,259U},
  {2U,53U,2566U,260U},
  {2U,19U,1594U,261U},
  {2U,44U,4063U,262U},
  {2U,31U,1805U,263U},
  {2U,17U,4080U,264U},
  {2U,1U,443U,265U},
  {2U,39U,3085U,266U},
  {2U,61U,1003U,267U},
  {2U,51U,667U,268U},
  {2U,58U,1676U,269U},
  {2U,12U,2852U,270U},
  {2U,6U,2815U,271U},
  {2U,50U,2141U,272U},
  {2U,55U,736U,273U},
  {2U,5U,2122U,274U},
  {2U,12U,2110U,275U},
  {2U,24U,1880U,276U},
  {2U,9U,2807U,277U},
  {2U,22U,532U,278U},
  {2U,34U,1888U,279U},
  {2U,13U,2385U,280U},
  {2U,37U,968U,281U},
  {2U,47U,2269U,282U},
  {2U,55U,3896U,283U},
  {2U,47U,91U,284U},
  {2U,20U,1397U,285U},
  {2U,16U,2760U,286U},
  {2U,38U,2840U,287U},
  {2U,27U,2973U,288U},
  {2U,5U,3976U,289U},
  {2U,4U,1191U,290U},
  {2U,30U,231U,291U},
  {2U,63U,2168U,292U},
  {2U,26U,2178U,293U},
  {2U,27U,1026U,294U},
  {2U,4U,2375U,295U},
  {2U,13U,1471U,296U},
  {2U,55U,2941U,297U},
  {2U,22U,2065U,298U},
  {2U,28U,1809U,299U},
  {2U,38U,3129U,300U},
  {2U,18U,2463U,301U},
  {2U,14U,3290U,302U},
  {2U,12U,65U,303U},
  {2U,4U,135U,304U},
  {2U,38U,906U,305U},
  {2U,29U,1540U,306U},
  {2U,29U,454U,307U},
  {2U,27U,3068U,308U},
  {2U,6U,1491U,309U},
  {2U,1U,3072U,310U},
  {2U,43U,2086U,311U},
  {2U,30U,3002U,312U},
  {2U,54U,4031U,313U},
  {2U,42U,2541U,314U},
  {2U,62U,481U,315U},
  {2U,0U,1307U,316U},
  {2U,63U,1475U,317U},
  {2U,14U,760U,318U},
  {2U,37U,453U,319U},
  {2U,35U,1262U,320U},
  {2U,10U,1564U,321U},
  {2U,27U,1636U,322U},
  {2U,18U,899U,323U},
  {2U,3U,2331U,324U},
  {2U,49U,2031U,325U},
  {2U,62U,1314U,326U},
  {2U,56U,1802U,327U},
  {2U,38U,1994U,328U},
  {2U,12U,3072U,329U},
  {2U,35U,3192U,330U},
  {2U,2U,202U,331U},
  {2U,43U,3405U,332U},
  {2U,30U,1627U,333U},
  {2U,43U,1396U,334U},
  {2U,27U,3364U,335U},
  {2U,62U,1893U,336U},
  {2U,40U,2326U,337U},
  {2U,12U,756U,338U},
  {2U,31U,2877U,339U},
  {2U,16U,1640U,340U},
  {2U,30U,1382U,341U},
  {2U,25U,3584U,342U},
  {2U,42U,984U,343U},
  {2U,35U,3160U,344U},
  {2U,58U,881U,345U},
  {2U,17U,682U,346U},
  {2U,35U,3019U,347U},
  {2U,37U,3620U,348U},
  {2U,51U,3113U,349U},
  {2U,62U,3429U,350U},
  {2U,47U,3278U,351U},
  {2U,17U,3790U,352U},
  {2U,47U,1343U,353U},
  {2U,32U,3890U,354U},
  {2U,33U,3500U,355U},
  {2U,14U,3872U,356U},
  {2U,9U,705U,357U},
  {2U,37U,2429U,358U},
  {2U,21U,1840U,359U},
  {2U,36U,1965U,360U},
  {2U,17U,690U,361U},
  {2U,44U,2266U,362U},
  {2U,12U,1612U,363U},
  {2U,17U,1334U,364U},
  {2U,1U,3034U,365U},
  {2U,60U,3505U,366U},
  {2U,41U,1975U,367U},
  {2U,51U,1246U,368U},
  {2U,26U,2517U,369U},
  {2U,23U,3589U,370U},
  {2U,26U,2148U,371U},
  {2U,32U,759U,372U},
  {2U,21U,166U,373U},
  {2U,52U,3771U,374U},
  {2U,50U,3179U,375U},
  {2U,33U,3822U,376U},
  {2U,39U,1235U,377U},
  {2U,24U,448U,378U},
  {2U,53U,475U,379U},
  {2U,21U,3177U,380U},
  {2U,47U,1628U,381U},
  {2U,1U,2369U,382U},
  {2U,24U,3403U,383U},
  {2U,47U,1017U,384U},
  {2U,32U,2174U,385U},
  {2U,4U,968U,386U},
  {2U,33U,167U,387U},
  {2U,61U,2409U,388U},
  {2U,23U,1609U,389U},
  {2U,15U,1935U,390U},
  {2U,61U,428U,391U},
  {2U,39U,3952U,392U},
  {2U,30U,1387U,393U},
  {2U,31U,3225U,394U},
  {2U,49U,3804U,395U},
  {2U,9U,1776U,396U},
  {2U,50U,2595U,397U},
  {2U,21U,1427U,398U},
  {2U,56U,257U,399U},
  {2U,53U,4066U,400U},
  {2U,17U,2203U,401U},
  {2U,27U,2734U,402U},
  {2U,46U,2577U,403U},
  {2U,56U,2727U,404U},
  {2U,57U,3153U,405U},
  {2U,16U,3854U,406U},
  {2U,3U,3930U,407U},
  {2U,8U,2589U,408U},
  {2U,25U,1263U,409U},
  {2U,54U,3080U,410U},
  {2U,1U,3977U,411U},
  {2U,35U,2175U,412U},
  {2U,61U,1763U,413U},
  {2U,7U,3523U,414U},
  {2U,49U,2048U,415U},
  {2U,21U,2770U,416U},
  {2U,2U,3949U,417U},
  {2U,6U,2376U,418U},
  {2U,60U,2608U,419U},
  {2U,59U,101U,420U},
  {2U,3U,211U,421U},
  {2U,40U,1380U,422U},
  {2U,50U,2074U,423U},
  {2U,46U,3012U,424U},
  {2U,11U,1727U,425U},
  {2U,18U,3926U,426U},
  {2U,18U,3075U,427U},
  {2U,38U,627U,428U},
  {2U,59U,2931U,429U},
  {2U,62U,1134U,430U},
  {2U,52U,259U,431U},
  {2U,59U,905U,432U},
  {2U,48U,1948U,433U},
  {2U,9U,3032U,434U},
  {2U,52U,4065U,435U},
  {2U,1U,2627U,436U},
  {2U,14U,3421U,437U},
  {2U,16U,756U,438U},
  {2U,20U,2942U,439U},
  {2U,14U,1029U,440U},
  {2U,17U,1459U,441U},
  {2U,45U,3178U,442U},
  {2U,19U,2462U,443U},
  {2U,22U,369U,444U},
  {2U,4U,1957U,445U},
  {2U,24U,1938U,446U},
  {2U,38U,2475U,447U},
  {2U,45U,1565U,448U},
  {2U,53U,3171U,449U},
  {2U,58U,4007U,450U},
  {2U,35U,1476U,451U},
  {2U,11U,108U,452U},
  {2U,32U,1190U,453U},
  {2U,5U,2633U,454U},
  {2U,7U,1038U,455U},
  {2U,56U,2485U,456U},
  {2U,29U,3840U,457U},
  {2U,10U,3643U,458U},
  {2U,20U,2173U,459U},
  {2U,19U,2986U,460U},
  {2U,4U,2962U,461U},
  {2U,21U,1904U,462U},
  {2U,34U,651U,463U},
  {2U,0U,1541U,464U},
  {2U,9U,555U,465U},
  {2U,7U,1667U,466U},
  {2U,37U,1425U,467U},
  {2U,52U,1090U,468U},
  {2U,48U,2598U,469U},
  {2U,9U,3514U,470U},
  {2U,37U,252U,471U},
  {2U,19U,1096U,472U},
  {2U,4U,1998U,473U},
  {2U,2U,3076U,474U},
  {2U,20U,3758U,475U},
  {2U,45U,2400U,476U},
  {2U,14U,1008U,477U},
  {2U,17U,3132U,478U},
  {2U,29U,184U,479U},
  {2U,6U,245U,480U},
  {2U,48U,637U,481U},
  {2U,25U,18U,482U},
  {2U,46U,93U,483U},
  {2U,7U,874U,484U},
  {2U,55U,2643U,485U},
  {2U,32U,1038U,486U},
  {2U,19U,3328U,487U},
  {2U,18U,3936U,488U},
  {2U,52U,198U,489U},
  {2U,57U,2903U,490U},
  {2U,38U,3687U,491U},
  {2U,17U,79U,492U},
  {2U,31U,2416U,493U},
  {2U,21U,450U,494U},
  {2U,32U,1851U,495U},
  {2U,52U,1099U,496U},
  {2U,21U,1738U,497U},
  {2U,61U,3112U,498U},
  {2U,27U,1136U,499U},
  {2U,5U,682U,500U},
  {2U,21U,3883U,501U},
  {2U,5U,2756U,502U},
  {2U,43U,1391U,503U},
  {2U,37U,1389U,504U},
  {2U,26U,3988U,505U},
  {2U,9U,2272U,506U},
  {2U,23U,3611U,507U},
  {2U,42U,3204U,508U},
  {2U,20U,2810U,509U},
  {2U,59U,278U,510U},
  {2U,4U,3444U,511U},
  {2U,22U,126U,512U},
  {2U,41U,3962U,513U},
  {2U,5U,2727U,514U},
  {2U,47U,1458U,515U},
  {2U,26U,102U,516U},
  {2U,3U,8U,517U},
  {2U,9U,2619U,518U},
  {2U,22U,587U,519U},
  {2U,17U,715U,520U},
  {2U,59U,1763U,521U},
  {2U,47U,317U,522U},
  {2U,17U,3211U,523U},
  {2U,63U,1986U,524U},
  {2U,10U,3623U,525U},
  {2U,1U,1002U,526U},
  {2U,9U,53U,527U},
  {2U,9U,2950U,528U},
  {2U,55U,2553U,529U},
  {2U,49U,1610U,530U},
  {2U,39U,3204U,531U},
  {2U,47U,2601U,532U},
  {2U,3U,1506U,533U},
  {2U,19U,646U,534U},
  {2U,4U,4043U,535U},
  {2U,1U,2889U,536U},
  {2U,5U,1793U,537U},
  {2U,22U,2120U,538U},
  {2U,50U,3580U,539U},
  {2U,37U,614U,540U},
  {2U,50U,189U,541U},
  {2U,63U,1728U,542U},
  {2U,17U,571U,543U},
  {2U,31U,838U,544U},
  {2U,1U,3554U,545U},
  {2U,15U,2846U,546U},
  {2U,19U,2770U,547U},
  {2U,16U,2544U,548U},
  {2U,35U,4063U,549U},
  {2U,15U,1652U,550U},
  {2U,36U,2340U,551U},
  {2U,59U,866U,552U},
  {2U,27U,1136U,553U},
  {2U,35U,2765U,554U},
  {2U,49U,3012U,555U},
  {2U,9U,3711U,556U},
  {2U,50U,831U,557U},
  {2U,8U,571U,558U},
  {2U,18U,398U,559U},
  {2U,19U,1047U,560U},
  {2U,52U,1369U,561U},
  {2U,39U,265U,562U},
  {2U,17U,1210U,563U},
  {2U,35U,175U,564U},
  {2U,14U,3473U,565U},
  {2U,24U,555U,566U},
  {2U,48U,2628U,567U},
  {2U,40U,2654U,568U},
  {2U,1U,3881U,569U},
  {2U,22U,1712U,570U},
  {2U,34U,2089U,571U},
  {2U,56U,1820U,572U},
  {2U,40U,3161U,573U},
  {2U,8U,3452U,574U},
  {2U,17U,1284U,575U},
  {2U,23U,4000U,576U},
  {2U,41U,13U,577U},
  {2U,55U,4009U,578U},
  {2U,31U,480U,579U},
  {2U,46U,2515U,580U},
  {2U,1U,2874U,581U},
  {2U,48U,837U,582U},
  {2U,39U,1126U,583U},
  {2U,48U,363U,584U},
  {2U,6U,3539U,585U},
  {2U,40U,1020U,586U},
  {2U,23U,3388U,587U},
  {2U,1U,3238U,588U},
  {2U,40U,3449U,589U},
  {2U,22U,3759U,590U},
  {2U,49U,1607U,591U},
  {2U,41U,1357U,592U},
  {2U,25U,2471U,593U},
  {2U,26U,2374U,594U},
  {2U,21U,2588U,595U},
  {2U,38U,3330U,596U},
  {2U,45U,3424U,597U},
  {2U,20U,3863U,598U},
  {2U,26U,1622U,599U},
  {2U,28U,3852U,600U},
  {2U,16U,3083U,601U},
  {2U,8U,206U,602U},
  {2U,58U,2093U,603U},
  {2U,15U,2192U,604U},
  {2U,62U,201U,605U},
  {2U,34U,1292U,606U},
  {2U,36U,2952U,607U},
  {2U,10U,3890U,608U},
  {2U,49U,3530U,609U},
  {2U,39U,1038U,610U},
  {2U,33U,1815U,611U},
  {2U,11U,2297U,612U},
  {2U,3U,2317U,613U},
  {2U,32U,92U,614U},
  {2U,3U,2741U,615U},
  {2U,56U,1425U,616U},
  {2U,42U,3779U,617U},
  {2U,44U,3647U,618U},
  {2U,17U,257U,619U},
  {2U,58U,1453U,620U},
  {2U,42U,1706U,621U},
  {2U,35U,1413U,622U},
  {2U,63U,1581U,623U},
  {2U,52U,716U,624U},
  {2U,9U,1310U,625U},
  {2U,47U,3647U,626U},
  {2U,28U,427U,627U},
  {2U,34U,2643U,628U},
  {2U,51U,1670U,629U},
  {2U,21U,3382U,630U},
  {2U,26U,248U,631U},
  {2U,51U,3015U,632U},
  {2U,48U,3385U,633U},
  {2U,50U,276U,634U},
  {2U,42U,1558U,635U},
  {2U,34U,1675U,636U},
  {2U,24U,3783U,637U},
  {2U,54U,810U,638U},
  {2U,57U,2823U,639U},
  {2U,47U,2138U,640U},
  {0U,0U,2301U,0U},
  {0U,1U,3889U,0U},
  {0U,2U,3832U,0U},
  {0U,3U,2775U,0U},
  {0U,4U,2172U,0U},
  {0U,5U,2921U,0U},
  {0U,6U,3190U,0U},
  {0U,7U,2433U,0U},
  {0U,8U,65U,0U},
  {0U,9U,2103U,0U},
  {0U,10U,1648U,0U},
  {0U,11U,2194U,0U},
  {0U,12U,1739U,0U},
  {0U,13U,1810U,0U},
  {0U,14U,75U,0U},
  {0U,15U,3465U,0U},
  {0U,16U,725U,0U},
  {0U,17U,1635U,0U},
  {0U,18U,2325U,0U},
  {0U,19U,2436U,0U},
  {0U,20U,2277U,0U},
  {0U,21U,1458U,0U},
  {0U,22U,2437U,0U},
  {0U,23U,987U,0U},
  {0U,24U,455U,0U},
  {0U,25U,102U,0U},
  {0U,26U,640U,0U},
  {0U,27U,398U,0U},
  {0U,28U,2451U,0U},
  {0U,29U,3581U,0U},
  {0U,30U,4008U,0U},
  {0U,31U,436U,0U},
  {0U,32U,482U,0U},
  {0U,33U,672U,0U},
  {0U,34U,423U,0U},
  {0U,35U,856U,0U},
  {0U,36U,3951U,0U},
  {0U,37U,3556U,0U},
  {0U,38U,947U,0U},
  {0U,39U,2164U,0U},
  {0U,40U,499U,0U},
  {0U,41U,4005U,0U},
  {0U,42U,341U,0U},
  {0U,43U,3579U,0U},
  {0U,44U,1916U,0U},
  {0U,45U,3903U,0U},
  {0U,46U,3753U,0U},
  {0U,47U,3846U,0U},
  {0U,48U,3986U,0U},
  {0U,49U,3944U,0U},
  {0U,50U,3419U,0U},
  {0U,51U,1549U,0U},
  {0U,52U,1818U,0U},
  {0U,53U,3587U,0U},
  {0U,54U,2518U,0U},
  {0U,55U,2834U,0U},
  {0U,56U,1006U,0U},
  {0U,57U,825U,0U},
  {0U,58U,48U,0U},
  {0U,59U,3544U,0U},
  {0U,60U,1168U,0U},
  {0U,61U,1010U,0U},
  {0U,62U,2817U,0U},
  {0U,63U,994U,0U},
  {1U,6U,63U,0U},
  {1U,44U,62U,0U},
  {1U,61U,61U,0U},
  {1U,4U,60U,0U},
  {1U,4U,59U,0U},
  {1U,32U,58U,0U},
  {1U,11U,57U,0U},
  {1U,41U,56U,0U},
  {1U,8U,55U,0U},
  {1U,26U,54U,0U},
  {1U,30U,53U,0U},
  {1U,21U,52U,0U},
  {1U,20U,51U,0U},
  {1U,35U,50U,0U},
  {1U,33U,49U,0U},
  {1U,0U,48U,0U},
  {1U,20U,47U,0U},
  {1U,35U,46U,0U},
  {1U,32U,45U,0U},
  {1U,3U,44U,0U},
  {1U,12U,43U,0U},
  {1U,30U,42U,0U},
  {1U,15U,41U,0U},
  {1U,21U,40U,0U},
  {1U,27U,39U,0U},
  {1U,8U,38U,0U},
  {1U,36U,37U,0U},
  {1U,23U,36U,0U},
  {1U,15U,35U,0U},
  {1U,0U,34U,0U},
  {1U,28U,33U,0U},
  {1U,19U,32U,0U},
  {1U,13U,31U,0U},
  {1U,11U,30U,0U},
  {1U,9U,29U,0U},
  {1U,7U,28U,0U},
  {1U,18U,27U,0U},
  {1U,11U,26U,0U},
  {1U,8U,25U,0U},
  {1U,9U,24U,0U},
  {1U,6U,23U,0U},
  {1U,22U,22U,0U},
  {1U,4U,21U,0U},
  {1U,8U,20U,0U},
  {1U,2U,19U,0U},
  {1U,6U,18U,0U},
  {1U,16U,17U,0U},
  {1U,6U,16U,0U},
  {1U,1U,15U,0U},
  {1U,7U,14U,0U},
  {1U,2U,13U,0U},
  {1U,7U,12U,0U},
  {1U,3U,11U,0U},
  {1U,6U,10U,0U},
  {1U,8U,9U,0U},
  {1U,7U,8U,0U},
  {1U,1U,7U,0U},
  {1U,0U,6U,0U},
  {1U,0U,5U,0U},
  {1U,1U,4U,0U},
  {1U,3U,3U,0U},
  {1U,2U,2U,0U},
  {1U,0U,1U,0U},
  {1U,0U,0U,0U},
  {2U,23U,1195U,129U},
  {2U,1U,1175U,130U},
  {2U,14U,365U,131U},
  {2U,51U,135U,132U},
  {2U,16U,2468U,133U},
  {2U,23U,3212U,134U},
  {2U,19U,2646U,135U},
  {2U,3U,1916U,136U},
  {2U,45U,2947U,137U},
  {2U,61U,1085U,138U},
  {2U,31U,1342U,139U},
  {2U,31U,3145U,140U},
  {2U,47U,1230U,141U},
  {2U,34U,3255U,142U},
  {2U,14U,4055U,143U},
  {2U,58U,1480U,144U},
  {2U,59U,1940U,145U},
  {2U,46U,876U,146U},
  {2U,51U,1632U,147U},
  {2U,43U,2780U,148U},
  {2U,37U,3146U,149U},
  {2U,40U,2778U,150U},
  {2U,8U,3050U,151U},
  {2U,45U,2166U,152U},
  {2U,2U,502U,153U},
  {2U,61U,2398U,154U},
  {2U,5U,3133U,155U},
  {2U,46U,442U,156U},
  {2U,62U,2568U,157U},
  {2U,25U,693U,158U},
  {2U,54U,3517U,159U},
  {2U,20U,2622U,160U},
  {2U,3U,771U,161U},
  {2U,6U,2879U,162U},
  {2U,6U,4079U,163U},
  {2U,36U,98U,164U},
  {2U,41U,1564U,165U},
  {2U,28U,537U,166U},
  {2U,19U,211U,167U},
  {2U,8U,336U,168U},
  {2U,15U,3697U,169U},
  {2U,16U,2942U,170U},
  {2U,26U,2624U,171U},
  {2U,46U,696U,172U},
  {2U,29U,3761U,173U},
  {2U,7U,2105U,174U},
  {2U,57U,1490U,175U},
  {2U,24U,1977U,176U},
  {2U,14U,1953U,177U},
  {2U,23U,3923U,178U},
  {2U,41U,3443U,179U},
  {2U,53U,3098U,180U},
  {2U,31U,3295U,181U},
  {2U,4U,2214U,182U},
  {2U,44U,3708U,183U},
  {2U,49U,1042U,184U},
  {2U,48U,2199U,185U},
  {2U,8U,2092U,186U},
  {2U,2U,16U,187U},
  {2U,5U,1757U,188U},
  {2U,46U,1887U,189U},
  {2U,49U,755U,190U},
  {2U,23U,2784U,191U},
  {2U,50U,2826U,192U},
  {2U,39U,1947U,193U},
  {2U,61U,3809U,194U},
  {2U,47U,2130U,195U},
  {2U,55U,3750U,196U},
  {2U,24U,2992U,197U},
  {2U,26U,3909U,198U},
  {2U,32U,441U,199U},
  {2U,58U,3820U,200U},
  {2U,24U,1339U,201U},
  {2U,15U,2910U,202U},
  {2U,33U,308U,203U},
  {2U,19U,2962U,204U},
  {2U,61U,3248U,205U},
  {2U,61U,3990U,206U},
  {2U,20U,1856U,207U},
  {2U,59U,1519U,208U},
  {2U,45U,3887U,209U},
  {2U,55U,1567U,210U},
  {2U,14U,1354U,211U},
  {2U,10U,1415U,212U},
  {2U,38U,1293U,213U},
  {2U,9U,711U,214U},
  {2U,27U,2105U,215U},
  {2U,2U,1029U,216U},
  {2U,19U,3778U,217U},
  {2U,30U,3728U,218U},
  {2U,1U,2430U,219U},
  {2U,4U,2549U,220U},
  {2U,7U,3666U,221U},
  {2U,63U,2975U,222U},
  {2U,8U,2086U,223U},
  {2U,5U,2400U,224U},
  {2U,43U,4070U,225U},
  {2U,59U,2508U,226U},
  {2U,31U,3057U,227U},
  {2U,52U,2607U,228U},
  {2U,57U,2866U,229U},
  {2U,35U,2640U,230U},
  {2U,62U,3675U,231U},
  {2U,32U,293U,232U},
  {2U,46U,749U,233U},
  {2U,11U,1015U,234U},
  {2U,49U,3168U,235U},
  {2U,62U,3244U,236U},
  {2U,51U,649U,237U},
  {2U,22U,1041U,238U},
  {2U,49U,2408U,239U},
  {2U,27U,1134U,240U},
  {2U,39U,1457U,241U},
  {2U,57U,3681U,242U},
  {2U,19U,1612U,243U},
  {2U,60U,1185U,244U},
  {2U,11U,2687U,245U},
  {2U,25U,341U,246U},
  {2U,18U,3062U,247U},
  {2U,27U,2342U,248U},
  {2U,45U,3266U,249U},
  {2U,44U,587U,250U},
  {2U,36U,1721U,251U},
  {2U,30U,1386U,252U},
  {2U,5U,128U,253U},
  {2U,62U,558U,254U},
  {2U,40U,640U,255U},
  {2U,0U,237U,256U},
  {2U,24U,3988U,257U},
  {2U,52U,1108U,258U},
  {2U,0U,985U,259U},
  {2U,48U,2680U,260U},
  {2U,24U,904U,261U},
  {2U,34U,3720U,262U},
  {2U,53U,578U,263U},
  {2U,43U,1382U,264U},
  {2U,21U,551U,265U},
  {2U,7U,2111U,266U},
  {2U,34U,3384U,267U},
  {2U,30U,2024U,268U},
  {2U,15U,797U,269U},
  {2U,31U,1419U,270U},
  {2U,37U,1392U,271U},
  {2U,9U,366U,272U},
  {2U,26U,2704U,273U},
  {2U,46U,3737U,274U},
  {2U,28U,2191U,275U},
  {2U,40U,1938U,276U},
  {2U,1U,959U,277U},
  {2U,48U,3009U,278U},
  {2U,6U,466U,279U},
  {2U,57U,2382U,280U},
  {2U,31U,387U,281U},
  {2U,35U,815U,282U},
  {2U,37U,3075U,283U},
  {2U,23U,894U,284U},
  {2U,34U,3005U,285U},
  {2U,19U,1228U,286U},
  {2U,62U,3358U,287U},
  {2U,22U,3668U,288U},
  {2U,51U,2306U,289U},
  {2U,49U,1488U,290U},
  {2U,7U,52U,291U},
  {2U,36U,530U,292U},
  {2U,2U,2093U,293U},
  {2U,16U,3547U,294U},
  {2U,46U,1656U,295U},
  {2U,29U,1628U,296U},
  {2U,9U,2397U,297U},
  {2U,30U,895U,298U},
  {2U,42U,1510U,299U},
  {2U,34U,1729U,300U},
  {2U,30U,1214U,301U},
  {2U,41U,2134U,302U},
  {2U,46U,556U,303U},
  {2U,9U,1452U,304U},
  {2U,48U,298U,305U},
  {2U,9U,2152U,306U},
  {2U,31U,2567U,307U},
  {2U,54U,3720U,308U},
  {2U,46U,1461U,309U},
  {2U,12U,449U,310U},
  {2U,4U,1166U,311U},
  {2U,51U,338U,312U},
  {2U,56U,1910U,313U},
  {2U,21U,1230U,314U},
  {2U,41U,1852U,315U},
  {2U,23U,1135U,316U},
  {2U,11U,3249U,317U},
  {2U,41U,2597U,318U},
  {2U,63U,3626U,319U},
  {2U,49U,2420U,320U},
  {2U,61U,3911U,321U},
  {2U,58U,1951U,322U},
  {2U,19U,2496U,323U},
  {2U,5U,1750U,324U},
  {2U,6U,2375U,325U},
  {2U,58U,1789U,326U},
  {2U,57U,2602U,327U},
  {2U,4U,4070U,328U},
  {2U,24U,967U,329U},
  {2U,23U,3918U,330U},
  {2U,47U,2996U,331U},
  {2U,35U,1434U,332U},
  {2U,33U,2348U,333U},
  {2U,63U,3322U,334U},
  {2U,60U,3247U,335U},
  {2U,19U,687U,336U},
  {2U,29U,3706U,337U},
  {2U,23U,537U,338U},
  {2U,40U,82U,339U},
  {2U,25U,2824U,340U},
  {2U,55U,1549U,341U},
  {2U,53U,682U,342U},
  {2U,32U,1149U,343U},
  {2U,52U,1628U,344U},
  {2U,4U,249U,345U},
  {2U,29U,1198U,346U},
  {2U,49U,1226U,347U},
  {2U,60U,1407U,348U},
  {2U,58U,2231U,349U},
  {2U,33U,1724U,350U},
  {2U,62U,2664U,351U},
  {2U,18U,2987U,352U},
  {2U,60U,1898U,353U},
  {2U,51U,2508U,354U},
  {2U,42U,3951U,355U},
  {2U,25U,2451U,356U},
  {2U,13U,477U,357U},
  {2U,42U,1231U,358U},
  {2U,18U,3410U,359U},
  {2U,46U,3656U,360U},
  {2U,58U,39U,361U},
  {2U,13U,3345U,362U},
  {2U,43U,3934U,363U},
  {2U,9U,2315U,364U},
  {2U,4U,2264U,365U},
  {2U,41U,3068U,366U},
  {2U,24U,3492U,367U},
  {2U,18U,3277U,368U},
  {2U,60U,597U,369U},
  {2U,34U,396U,370U},
  {2U,61U,3989U,371U},
  {2U,63U,96U,372U},
  {2U,11U,3835U,373U},
  {2U,24U,779U,374U},
  {2U,44U,3742U,375U},
  {2U,41U,1343U,376U},
  {2U,43U,3931U,377U},
  {2U,10U,3210U,378U},
  {2U,17U,2042U,379U},
  {2U,54U,134U,380U},
  {2U,46U,802U,381U},
  {2U,62U,1593U,382U},
  {2U,60U,3703U,383U},
  {2U,56U,1001U,384U},
  {2U,34U,1446U,385U},
  {2U,41U,3351U,386U},
  {2U,34U,3606U,387U},
  {2U,12U,2734U,388U},
  {2U,23U,2177U,389U},
  {2U,6U,3239U,390U},
  {2U,10U,998U,391U},
  {2U,6U,430U,392U},
  {2U,21U,16U,393U},
  {2U,4U,1070U,394U},
  {2U,21U,3968U,395U},
  {2U,16U,2923U,396U},
  {2U,42U,392U,397U},
  {2U,44U,3512U,398U},
  {2U,26U,1105U,399U},
  {2U,56U,788U,400U},
  {2U,10U,2099U,401U},
  {2U,25U,336U,402U},
  {2U,15U,1176U,403U},
  {2U,6U,656U,404U},
  {2U,14U,3153U,405U},
  {2U,38U,3682U,406U},
  {2U,47U,1955U,407U},
  {2U,3U,2797U,408U},
  {2U,20U,1850U,409U},
  {2U,28U,3858U,410U},
  {2U,33U,2679U,411U},
  {2U,22U,3239U,412U},
  {2U,62U,2624U,413U},
  {2U,3U,1080U,414U},
  {2U,21U,853U,415U},
  {2U,42U,1494U,416U},
  {2U,20U,1060U,417U},
  {2U,23U,2979U,418U},
  {2U,5U,648U,419U},
  {2U,9U,876U,420U},
  {2U,25U,119U,421U},
  {2U,42U,3228U,422U},
  {2U,9U,3923U,423U},
  {2U,28U,1761U,424U},
  {2U,11U,3786U,425U},
  {2U,44U,1697U,426U},
  {2U,14U,506U,427U},
  {2U,59U,1392U,428U},
  {2U,58U,3782U,429U},
  {2U,9U,2105U,430U},
  {2U,11U,411U,431U},
  {2U,23U,3282U,432U},
  {2U,31U,2955U,433U},
  {2U,48U,3915U,434U},
  {2U,40U,2816U,435U},
  {2U,4U,620U,436U},
  {2U,11U,859U,437U},
  {2U,32U,3836U,438U},
  {2U,35U,1349U,439U},
  {2U,24U,3415U,440U},
  {2U,54U,1237U,441U},
  {2U,39U,2297U,442U},
  {2U,49U,1280U,443U},
  {2U,51U,238U,444U},
  {2U,59U,645U,445U},
  {2U,8U,1845U,446U},
  {2U,36U,2076U,447U},
  {2U,23U,3033U,448U},
  {2U,12U,2960U,449U},
  {2U,25U,2005U,450U},
  {2U,52U,1870U,451U},
  {2U,13U,963U,452U},
  {2U,35U,599U,453U},
  {2U,17U,3981U,454U},
  {2U,6U,1563U,455U},
  {2U,24U,1524U,456U},
  {2U,21U,272U,457U},
  {2U,4U,2528U,458U},
  {2U,23U,2388U,459U},
  {2U,36U,1005U,460U},
  {2U,50U,858U,461U},
  {2U,13U,2400U,462U},
  {2U,54U,1573U,463U},
  {2U,60U,1571U,464U},
  {2U,20U,3195U,465U},
  {2U,42U,3790U,466U},
  {2U,9U,308U,467U},
  {2U,38U,1890U,468U},
  {2U,9U,709U,469U},
  {2U,7U,802U,470U},
  {2U,50U,3936U,471U},
  {2U,28U,2866U,472U},
  {2U,38U,1594U,473U},
  {2U,40U,296U,474U},
  {2U,35U,2185U,475U},
  {2U,14U,1311U,476U},
  {2U,51U,3597U,477U},
  {2U,57U,3345U,478U},
  {2U,32U,2708U,479U},
  {2U,42U,1061U,480U},
  {2U,32U,3060U,481U},
  {2U,16U,900U,482U},
  {2U,7U,2281U,483U},
  {2U,4U,2274U,484U},
  {2U,2U,1514U,485U},
  {2U,11U,1531U,486U},
  {2U,50U,3854U,487U},
  {2U,47U,2124U,488U},
  {2U,6U,1686U,489U},
  {2U,27U,3046U,490U},
  {2U,51U,2899U,491U},
  {2U,43U,2173U,492U},
  {2U,42U,2826U,493U},
  {2U,16U,3531U,494U},
  {2U,53U,1549U,495U},
  {2U,30U,2413U,496U},
  {2U,57U,3314U,497U},
  {2U,13U,3018U,498U},
  {2U,48U,2209U,499U},
  {2U,29U,727U,500U},
  {2U,35U,2794U,501U},
  {2U,14U,1139U,502U},
  {2U,29U,3375U,503U},
  {2U,28U,575U,504U},
  {2U,3U,2501U,505U},
  {2U,63U,2014U,506U},
  {2U,63U,3554U,507U},
  {2U,62U,1417U,508U},
  {2U,16U,2531U,509U},
  {2U,37U,1638U,510U},
  {2U,34U,3310U,511U},
  {2U,37U,3045U,512U},
  {2U,19U,2042U,513U},
  {2U,3U,1946U,514U},
  {2U,54U,2354U,515U},
  {2U,56U,677U,516U},
  {2U,38U,3025U,517U},
  {2U,42U,2511U,518U},
  {2U,31U,9U,519U},
  {2U,30U,1704U,520U},
  {2U,54U,3766U,521U},
  {2U,50U,614U,522U},
  {2U,16U,168U,523U},
  {2U,8U,246U,524U},
  {2U,31U,4052U,525U},
  {2U,52U,2199U,526U},
  {2U,26U,698U,527U},
  {2U,37U,1015U,528U},
  {2U,45U,1758U,529U},
  {2U,4U,2457U,530U},
  {2U,36U,922U,531U},
  {2U,22U,970U,532U},
  {2U,4U,3294U,533U},
  {2U,22U,3171U,534U},
  {2U,1U,1038U,535U},
  {2U,36U,2846U,536U},
  {2U,19U,3189U,537U},
  {2U,22U,968U,538U},
  {2U,60U,3009U,539U},
  {2U,7U,4002U,540U},
  {2U,36U,361U,541U},
  {2U,37U,3747U,542U},
  {2U,50U,1357U,543U},
  {2U,17U,3174U,544U},
  {2U,51U,356U,545U},
  {2U,9U,359U,546U},
  {2U,33U,3872U,547U},
  {2U,29U,2607U,548U},
  {2U,33U,3089U,549U},
  {2U,46U,2527U,550U},
  {2U,58U,3651U,551U},
  {2U,33U,1961U,552U},
  {2U,35U,641U,553U},
  {2U,8U,2673U,554U},
  {2U,5U,1045U,555U},
  {2U,56U,1524U,556U},
  {2U,39U,1681U,557U},
  {2U,52U,2887U,558U},
  {2U,49U,1244U,559U},
  {2U,37U,802U,560U},
  {2U,33U,3945U,561U},
  {2U,10U,3518U,562U},
  {2U,56U,1370U,563U},
  {2U,59U,2475U,564U},
  {2U,5U,559U,565U},
  {2U,16U,3312U,566U},
  {2U,25U,3644U,567U},
  {2U,15U,3490U,568U},
  {2U,30U,4084U,569U},
  {2U,6U,2634U,570U},
  {2U,41U,567U,571U},
  {2U,8U,130U,572U},
  {2U,57U,3444U,573U},
  {2U,22U,3560U,574U},
  {2U,23U,3980U,575U},
  {2U,12U,2913U,576U},
  {2U,46U,593U,577U},
  {2U,54U,1970U,578U},
  {2U,27U,4016U,579U},
  {2U,56U,886U,580U},
  {2U,58U,3522U,581U},
  {2U,11U,3229U,582U},
  {2U,37U,3543U,583U},
  {2U,43U,182U,584U},
  {2U,24U,2333U,585U},
  {2U,18U,3534U,586U},
  {2U,31U,1690U,587U},
  {2U,60U,1983U,588U},
  {2U,59U,1467U,589U},
  {2U,40U,3751U,590U},
  {2U,51U,1144U,591U},
  {2U,3U,2434U,592U},
  {2U,33U,1284U,593U},
  {2U,18U,723U,594U},
  {2U,61U,3935U,595U},
  {2U,47U,716U,596U},
  {2U,55U,2559U,597U},
  {2U,18U,2058U,598U},
  {2U,29U,2166U,599U},
  {2U,58U,2073U,600U},
  {2U,52U,1969U,601U},
  {2U,41U,1775U,602U},
  {2U,45U,3811U,603U},
  {2U,16U,3368U,604U},
  {2U,48U,1448U,605U},
  {2U,9U,1031U,606U},
  {2U,26U,2121U,607U},
  {2U,45U,1513U,608U},
  {2U,7U,3326U,609U},
  {2U,58U,2017U,610U},
  {2U,62U,3153U,611U},
  {2U,50U,3955U,612U},
  {2U,55U,2809U,613U},
  {2U,59U,1704U,614U},
  {2U,18U,1735U,615U},
  {2U,56U,757U,616U},
  {2U,40U,2108U,617U},
  {2U,44U,3481U,618U},
  {2U,54U,1355U,619U},
  {2U,43U,2778U,620U},
  {2U,41U,244U,621U},
  {2U,49U,3586U,622U},
  {2U,61U,3407U,623U},
  {2U,36U,3288U,624U},
  {2U,3U,1317U,625U},
  {2U,16U,2907U,626U},
  {2U,18U,2853U,627U},
  {2U,24U,3561U,628U},
  {2U,57U,2794U,629U},
  {2U,33U,3065U,630U},
  {2U,27U,2134U,631U},
  {2U,32U,2171U,632U},
  {2U,56U,3691U,633U},
  {2U,10U,1337U,634U},
  {2U,40U,718U,635U},
  {2U,27U,891U,636U},
  {2U,16U,2215U,637U},
  {2U,38U,2177U,638U},
  {2U,7U,1051U,639U},
  {2U,52U,1290U,640U},
  {0U,0U,1997U,0U},
  {0U,1U,3925U,0U},
  {0U,2U,1920U,0U},
  {0U,3U,3708U,0U},
  {0U,4U,1506U,0U},
  {0U,5U,1837U,0U},
  {0U,6U,3789U,0U},
  {0U,7U,3856U,0U},
  {0U,8U,891U,0U},
  {0U,9U,825U,0U},
  {0U,10U,863U,0U},
  {0U,11U,2334U,0U},
  {0U,12U,2752U,0U},
  {0U,13U,1358U,0U},
  {0U,14U,2090U,0U},
  {0U,15U,2459U,0U},
  {0U,16U,803U,0U},
  {0U,17U,681U,0U},
  {0U,18U,2769U,0U},
  {0U,19U,3720U,0U},
  {0U,20U,2183U,0U},
  {0U,21U,1522U,0U},
  {0U,22U,2205U,0U},
  {0U,23U,438U,0U},
  {0U,24U,155U,0U},
  {0U,25U,20U,0U},
  {0U,26U,735U,0U},
  {0U,27U,1268U,0U},
  {0U,28U,1272U,0U},
  {0U,29U,35U,0U},
  {0U,30U,2268U,0U},
  {0U,31U,1912U,0U},
  {0U,32U,648U,0U},
  {0U,33U,1320U,0U},
  {0U,34U,1211U,0U},
  {0U,35U,3514U,0U},
  {0U,36U,1741U,0U},
  {0U,37U,76U,0U},
  {0U,38U,172U,0U},
  {0U,39U,3003U,0U},
  {0U,40U,3067U,0U},
  {0U,41U,2360U,0U},
  {0U,42U,3511U,0U},
  {0U,43U,281U,0U},
  {0U,44U,1679U,0U},
  {0U,45U,3596U,0U},
  {0U,46U,1847U,0U},
  {0U,47U,2469U,0U},
  {0U,48U,314U,0U},
  {0U,49U,3641U,0U},
  {0U,50U,3320U,0U},
  {0U,51U,708U,0U},
  {0U,52U,3565U,0U},
  {0U,53U,1913U,0U},
  {0U,54U,3291U,0U},
  {0U,55U,1579U,0U},
  {0U,56U,319U,0U},
  {0U,57U,1864U,0U},
  {0U,58U,2973U,0U},
  {0U,59U,255U,0U},
  {0U,60U,3008U,0U},
  {0U,61U,3099U,0U},
  {0U,62U,2364U,0U},
  {0U,63U,1556U,0U},
  {1U,36U,63U,0U},
  {1U,48U,62U,0U},
  {1U,21U,61U,0U},
  {1U,45U,60U,0U},
  {1U,19U,59U,0U},
  {1U,11U,58U,0U},
  {1U,38U,57U,0U},
  {1U,4U,56U,0U},
  {1U,11U,55U,0U},
  {1U,35U,54U,0U},
  {1U,15U,53U,0U},
  {1U,38U,52U,0U},
  {1U,6U,51U,0U},
  {1U,33U,50U,0U},
  {1U,48U,49U,0U},
  {1U,3U,48U,0U},
  {1U,36U,47U,0U},
  {1U,4U,46U,0U},
  {1U,22U,45U,0U},
  {1U,7U,44U,0U},
  {1U,19U,43U,0U},
  {1U,2U,42U,0U},
  {1U,32U,41U,0U},
  {1U,25U,40U,0U},
  {1U,15U,39U,0U},
  {1U,7U,38U,0U},
  {1U,31U,37U,0U},
  {1U,29U,36U,0U},
  {1U,32U,35U,0U},
  {1U,31U,34U,0U},
  {1U,1U,33U,0U},
  {1U,12U,32U,0U},
  {1U,28U,31U,0U},
  {1U,25U,30U,0U},
  {1U,18U,29U,0U},
  {1U,16U,28U,0U},
  {1U,3U,27U,0U},
  {1U,6U,26U,0U},
  {1U,4U,25U,0U},
  {1U,1U,24U,0U},
  {1U,20U,23U,0U},
  {1U,0U,22U,0U},
  {1U,5U,21U,0U},
  {1U,5U,20U,0U},
  {1U,17U,19U,0U},
  {1U,15U,18U,0U},
  {1U,5U,17U,0U},
  {1U,9U,16U,0U},
  {1U,6U,15U,0U},
  {1U,12U,14U,0U},
  {1U,5U,13U,0U},
  {1U,8U,12U,0U},
  {1U,11U,11U,0U},
  {1U,3U,10U,0U},
  {1U,8U,9U,0U},
  {1U,7U,8U,0U},
  {1U,2U,7U,0U},
  {1U,6U,6U,0U},
  {1U,1U,5U,0U},
  {1U,1U,4U,0U},
  {1U,0U,3U,0U},
  {1U,0U,2U,0U},
  {1U,0U,1U,0U},
  {1U,0U,0U,0U},
  {2U,1U,1191U,129U},
  {2U,42U,1394U,130U},
  {2U,53U,1503U,131U},
  {2U,1U,390U,132U},
  {2U,20U,425U,133U},
  {2U,35U,3227U,134U},
  {2U,29U,2665U,135U},
  {2U,13U,1976U,136U},
  {2U,18U,2681U,137U},
  {2U,37U,256U,138U},
  {2U,35U,3458U,139U},
  {2U,24U,1152U,140U},
  {2U,51U,3984U,141U},
  {2U,51U,1193U,142U},
  {2U,20U,1891U,143U},
  {2U,8U,2324U,144U},
  {2U,3U,1715U,145U},
  {2U,10U,219U,146U},
  {2U,39U,2839U,147U},
  {2U,9U,3145U,148U},
  {2U,46U,3208U,149U},
  {2U,3U,878U,150U},
  {2U,13U,1480U,151U},
  {2U,58U,2163U,152U},
  {2U,50U,3582U,153U},
  {2U,18U,159U,154U},
  {2U,29U,2688U,155U},
  {2U,47U,398U,156U},
  {2U,41U,1198U,157U},
  {2U,12U,4005U,158U},
  {2U,59U,2153U,159U},
  {2U,43U,2332U,160U},
  {2U,10U,3569U,161U},
  {2U,36U,1944U,162U},
  {2U,41U,1599U,163U},
  {2U,54U,3860U,164U},
  {2U,48U,3133U,165U},
  {2U,7U,2040U,166U},
  {2U,8U,2117U,167U},
  {2U,46U,614U,168U},
  {2U,27U,3619U,169U},
  {2U,32U,796U,170U},
  {2U,29U,2858U,171U},
  {2U,19U,1191U,172U},
  {2U,2U,3148U,173U},
  {2U,54U,2395U,174U},
  {2U,58U,2899U,175U},
  {2U,30U,3071U,176U},
  {2U,37U,1262U,177U},
  {2U,22U,1711U,178U},
  {2U,11U,162U,179U},
  {2U,49U,3830U,180U},
  {2U,15U,3959U,181U},
  {2U,31U,755U,182U},
  {2U,51U,1779U,183U},
  {2U,7U,3712U,184U},
  {2U,1U,3001U,185U},
  {2U,15U,2257U,186U},
  {2U,41U,1225U,187U},
  {2U,63U,2797U,188U},
  {2U,15U,40U,189U},
  {2U,24U,936U,190U},
  {2U,47U,1151U,191U},
  {2U,23U,1280U,192U},
  {2U,0U,1781U,193U},
  {2U,60U,869U,194U},
  {2U,7U,1837U,195U},
  {2U,63U,2697U,196U},
  {2U,49U,2902U,197U},
  {2U,16U,2797U,198U},
  {2U,23U,3264U,199U},
  {2U,41U,3556U,200U},
  {2U,22U,2635U,201U},
  {2U,27U,564U,202U},
  {2U,43U,2491U,203U},
  {2U,24U,3984U,204U},
  {2U,15U,3702U,205U},
  {2U,9U,1540U,206U},
  {2U,2U,2908U,207U},
  {2U,46U,3891U,208U},
  {2U,18U,2585U,209U},
  {2U,29U,2380U,210U},
  {2U,53U,3290U,211U},
  {2U,54U,3926U,212U},
  {2U,19U,1642U,213U},
  {2U,20U,3274U,214U},
  {2U,5U,2007U,215U},
  {2U,29U,1569U,216U},
  {2U,30U,1642U,217U},
  {2U,48U,3215U,218U},
  {2U,7U,2329U,219U},
  {2U,45U,3615U,220U},
  {2U,48U,2796U,221U},
  {2U,31U,2849U,222U},
  {2U,26U,2095U,223U},
  {2U,2U,123U,224U},
  {2U,58U,3106U,225U},
  {2U,3U,556U,226U},
  {2U,7U,245U,227U},
  {2U,24U,1526U,228U},
  {2U,52U,876U,229U},
  {2U,33U,2705U,230U},
  {2U,40U,3089U,231U},
  {2U,11U,2354U,232U},
  {2U,0U,183U,233U},
  {2U,6U,1874U,234U},
  {2U,1U,1434U,235U},
  {2U,36U,163U,236U},
  {2U,61U,888U,237U},
  {2U,58U,229U,238U},
  {2U,6U,1418U,239U},
  {2U,23U,1087U,240U},
  {2U,42U,1935U,241U},
  {2U,44U,784U,242U},
  {2U,21U,1361U,243U},
  {2U,20U,3170U,244U},
  {2U,43U,1654U,245U},
  {2U,16U,3945U,246U},
  {2U,12U,1110U,247U},
  {2U,62U,388U,248U},
  {2U,28U,2092U,249U},
  {2U,46U,4025U,250U},
  {2U,47U,2940U,251U},
  {2U,45U,2223U,252U},
  {2U,46U,318U,253U},
  {2U,4U,1948U,254U},
  {2U,13U,56U,255U},
  {2U,52U,3688U,256U},
  {2U,46U,66U,257U},
  {2U,61U,111U,258U},
  {2U,44U,1019U,259U},
  {2U,29U,602U,260U},
  {2U,51U,1646U,261U},
  {2U,47U,3605U,262U},
  {2U,30U,99U,263U},
  {2U,52U,3969U,264U},
  {2U,14U,1196U,265U},
  {2U,51U,236U,266U},
  {2U,0U,711U,267U},
  {2U,17U,3344U,268U},
  {2U,3U,1888U,269U},
  {2U,28U,2643U,270U},
  {2U,7U,3740U,271U},
  {2U,29U,2813U,272U},
  {2U,39U,118U,273U},
  {2U,34U,1667U,274U},
  {2U,12U,1571U,275U},
  {2U,26U,3920U,276U},
  {2U,42U,3061U,277U},
  {2U,8U,1169U,278U},
  {2U,13U,3988U,279U},
  {2U,47U,1228U,280U},
  {2U,57U,1597U,281U},
  {2U,6U,1661U,282U},
  {2U,16U,2335U,283U},
  {2U,29U,1837U,284U},
  {2U,38U,2765U,285U},
  {2U,62U,1868U,286U},
  {2U,49U,46U,287U},
  {2U,47U,176U,288U},
  {2U,21U,1508U,289U},
  {2U,47U,1760U,290U},
  {2U,5U,3803U,291U},
  {2U,58U,2293U,292U},
  {2U,18U,3386U,293U},
  {2U,29U,1314U,294U},
  {2U,10U,1471U,295U},
  {2U,52U,1278U,296U},
  {2U,8U,3530U,297U},
  {2U,59U,881U,298U},
  {2U,14U,2436U,299U},
  {2U,63U,1477U,300U},
  {2U,22U,1664U,301U},
  {2U,60U,3392U,302U},
  {2U,17U,3274U,303U},
  {2U,28U,3958U,304U},
  {2U,24U,2059U,305U},
  {2U,37U,822U,306U},
  {2U,48U,1630U,307U},
  {2U,25U,322U,308U},
  {2U,13U,1742U,309U},
  {2U,10U,370U,310U},
  {2U,37U,2765U,311U},
  {2U,58U,3503U,312U},
  {2U,31U,2871U,313U},
  {2U,0U,3175U,314U},
  {2U,22U,1006U,315U},
  {2U,4U,2883U,316U},
  {2U,21U,1407U,317U},
  {2U,21U,741U,318U},
  {2U,11U,2214U,319U},
  {2U,29U,2851U,320U},
  {2U,14U,337U,321U},
  {2U,20U,2344U,322U},
  {2U,45U,3880U,323U},
  {2U,33U,2424U,324U},
  {2U,13U,2497U,325U},
  {2U,58U,2857U,326U},
  {2U,27U,629U,327U},
  {2U,33U,3926U,328U},
  {2U,31U,1314U,329U},
  {2U,9U,1550U,330U},
  {2U,25U,2623U,331U},
  {2U,37U,1413U,332U},
  {2U,34U,874U,333U},
  {2U,37U,909U,334U},
  {2U,11U,1386U,335U},
  {2U,19U,2778U,336U},
  {2U,22U,17U,337U},
  {2U,31U,480U,338U},
  {2U,62U,1740U,339U},
  {2U,15U,2264U,340U},
  {2U,62U,1653U,341U},
  {2U,38U,2002U,342U},
  {2U,44U,1715U,343U},
  {2U,49U,2810U,344U},
  {2U,63U,3672U,345U},
  {2U,1U,86U,346U},
  {2U,15U,373U,347U},
  {2U,17U,3407U,348U},
  {2U,14U,1427U,349U},
  {2U,25U,2228U,350U},
  {2U,23U,2649U,351U},
  {2U,34U,753U,352U},
  {2U,14U,3645U,353U},
  {2U,10U,3450U,354U},
  {2U,24U,2615U,355U},
  {2U,9U,2059U,356U},
  {2U,20U,1149U,357U},
  {2U,14U,1863U,358U},
  {2U,22U,907U,359U},
  {2U,53U,1502U,360U},
  {2U,57U,221U,361U},
  {2U,6U,613U,362U},
  {2U,39U,3036U,363U},
  {2U,62U,1275U,364U},
  {2U,60U,2388U,365U},
  {2U,10U,1174U,366U},
  {2U,42U,683U,367U},
  {2U,22U,3183U,368U},
  {2U,46U,1881U,369U},
  {2U,13U,3278U,370U},
  {2U,19U,790U,371U},
  {2U,45U,3741U,372U},
  {2U,45U,1465U,373U},
  {2U,58U,1880U,374U},
  {2U,12U,2863U,375U},
  {2U,39U,3135U,376U},
  {2U,30U,3463U,377U},
  {2U,20U,1543U,378U},
  {2U,6U,66U,379U},
  {2U,58U,980U,380U},
  {2U,56U,316U,381U},
  {2U,53U,1810U,382U},
  {2U,40U,64U,383U},
  {2U,23U,1544U,384U},
  {2U,10U,2804U,385U},
  {2U,58U,842U,386U},
  {2U,13U,3139U,387U},
  {2U,14U,516U,388U},
  {2U,6U,217U,389U},
  {2U,33U,3521U,390U},
  {2U,9U,1776U,391U},
  {2U,54U,103U,392U},
  {2U,8U,3183U,393U},
  {2U,29U,3436U,394U},
  {2U,12U,1062U,395U},
  {2U,34U,1191U,396U},
  {2U,27U,4046U,397U},
  {2U,2U,3083U,398U},
  {2U,48U,3003U,399U},
  {2U,9U,251U,400U},
  {2U,62U,785U,401U},
  {2U,55U,376U,402U},
  {2U,12U,2117U,403U},
  {2U,51U,2660U,404U},
  {2U,58U,834U,405U},
  {2U,31U,173U,406U},
  {2U,13U,1240U,407U},
  {2U,3U,877U,408U},
  {2U,63U,3373U,409U},
  {2U,43U,3052U,410U},
  {2U,47U,3933U,411U},
  {2U,39U,2008U,412U},
  {2U,19U,2022U,413U},
  {2U,62U,2766U,414U},
  {2U,36U,657U,415U},
  {2U,59U,528U,416U},
  {2U,23U,3127U,417U},
  {2U,6U,3560U,418U},
  {2U,51U,1503U,419U},
  {2U,56U,681U,420U},
  {2U,26U,2773U,421U},
  {2U,47U,2968U,422U},
  {2U,30U,1368U,423U},
  {2U,1U,2129U,424U},
  {2U,21U,1759U,425U},
  {2U,24U,1171U,426U},
  {2U,36U,3591U,427U},
  {2U,41U,1015U,428U},
  {2U,58U,2489U,429U},
  {2U,0U,960U,430U},
  {2U,34U,1821U,431U},
  {2U,51U,1688U,432U},
  {2U,16U,2594U,433U},
  {2U,24U,80U,434U},
  {2U,42U,57U,435U},
  {2U,35U,1833U,436U},
  {2U,6U,355U,437U},
  {2U,58U,2187U,438U},
  {2U,40U,3501U,439U},
  {2U,43U,389U,440U},
  {2U,21U,3735U,441U},
  {2U,9U,4041U,442U},
  {2U,59U,180U,443U},
  {2U,1U,3286U,444U},
  {2U,34U,3854U,445U},
  {2U,12U,1647U,446U},
  {2U,27U,1975U,447U},
  {2U,16U,1386U,448U},
  {2U,8U,1836U,449U},
  {2U,23U,685U,450U},
  {2U,23U,3775U,451U},
  {2U,56U,3918U,452U},
  {2U,56U,2459U,453U},
  {2U,5U,1578U,454U},
  {2U,56U,2650U,455U},
  {2U,5U,1445U,456U},
  {2U,16U,1931U,457U},
  {2U,39U,2347U,458U},
  {2U,25U,2310U,459U},
  {2U,39U,2844U,460U},
  {2U,14U,1495U,461U},
  {2U,57U,1294U,462U},
  {2U,48U,3952U,463U},
  {2U,19U,1867U,464U},
  {2U,15U,2501U,465U},
  {2U,2U,1713U,466U},
  {2U,48U,3774U,467U},
  {2U,60U,1335U,468U},
  {2U,24U,64U,469U},
  {2U,7U,580U,470U},
  {2U,15U,558U,471U},
  {2U,38U,3956U,472U},
  {2U,6U,3388U,473U},
  {2U,8U,1565U,474U},
  {2U,60U,582U,475U},
  {2U,38U,2083U,476U},
  {2U,55U,3984U,477U},
  {2U,2U,56U,478U},
  {2U,35U,80U,479U},
  {2U,11U,3009U,480U},
  {2U,50U,2030U,481U},
  {2U,40U,2269U,482U},
  {2U,32U,1600U,483U},
  {2U,47U,3568U,484U},
  {2U,11U,766U,485U},
  {2U,26U,2631U,486U},
  {2U,22U,347U,487U},
  {2U,2U,2728U,488U},
  {2U,62U,1351U,489U},
  {2U,5U,3616U,490U},
  {2U,51U,953U,491U},
  {2U,11U,3638U,492U},
  {2U,17U,2856U,493U},
  {2U,22U,1620U,494U},
  {2U,55U,2299U,495U},
  {2U,22U,596U,496U},
  {2U,13U,1150U,497U},
  {2U,51U,736U,498U},
  {2U,23U,3839U,499U},
  {2U,46U,2371U,500U},
  {2U,16U,2455U,501U},
  {2U,9U,1985U,502U},
  {2U,43U,133U,503U},
  {2U,36U,1527U,504U},
  {2U,2U,386U,505U},
  {2U,30U,3104U,506U},
  {2U,34U,3055U,507U},
  {2U,54U,1140U,508U},
  {2U,19U,4043U,509U},
  {2U,36U,1364U,510U},
  {2U,53U,2744U,511U},
  {2U,33U,1636U,512U},
  {2U,57U,726U,513U},
  {2U,7U,3165U,514U},
  {2U,0U,1355U,515U},
  {2U,59U,1759U,516U},
  {2U,4U,827U,517U},
  {2U,22U,331U,518U},
  {2U,15U,2383U,519U},
  {2U,57U,508U,520U},
  {2U,37U,3193U,521U},
  {2U,41U,2578U,522U},
  {2U,9U,8U,523U},
  {2U,1U,3984U,524U},
  {2U,6U,3811U,525U},
  {2U,44U,2413U,526U},
  {2U,48U,23U,527U},
  {2U,1U,3404U,528U},
  {2U,10U,2455U,529U},
  {2U,62U,3644U,530U},
  {2U,29U,1515U,531U},
  {2U,42U,3414U,532U},
  {2U,61U,900U,533U},
  {2U,7U,815U,534U},
  {2U,4U,906U,535U},
  {2U,60U,2539U,536U},
  {2U,63U,3945U,537U},
  {2U,45U,2025U,538U},
  {2U,3U,1158U,539U},
  {2U,4U,1275U,540U},
  {2U,27U,3768U,541U},
  {2U,39U,170U,542U},
  {2U,14U,744U,543U},
  {2U,24U,2649U,544U},
  {2U,26U,2097U,545U},
  {2U,33U,146U,546U},
  {2U,23U,1624U,547U},
  {2U,38U,369U,548U},
  {2U,46U,699U,549U},
  {2U,4U,3909U,550U},
  {2U,7U,3872U,551U},
  {2U,57U,2072U,552U},
  {2U,20U,1578U,553U},
  {2U,18U,1781U,554U},
  {2U,37U,2356U,555U},
  {2U,39U,1553U,556U},
  {2U,49U,4053U,557U},
  {2U,26U,3022U,558U},
  {2U,2U,1593U,559U},
  {2U,7U,2393U,560U},
  {2U,42U,3248U,561U},
  {2U,30U,2703U,562U},
  {2U,58U,1514U,563U},
  {2U,17U,1376U,564U},
  {2U,35U,549U,565U},
  {2U,60U,1469U,566U},
  {2U,12U,3252U,567U},
  {2U,44U,3315U,568U},
  {2U,39U,1497U,569U},
  {2U,45U,892U,570U},
  {2U,60U,1708U,571U},
  {2U,45U,660U,572U},
  {2U,31U,1730U,573U},
  {2U,24U,1271U,574U},
  {2U,60U,963U,575U},
  {2U,42U,2243U,576U},
  {2U,40U,2265U,577U},
  {2U,49U,707U,578U},
  {2U,18U,2070U,579U},
  {2U,52U,3698U,580U},
  {2U,36U,1794U,581U},
  {2U,2U,1012U,582U},
  {2U,45U,1512U,583U},
  {2U,39U,3483U,584U},
  {2U,35U,3052U,585U},
  {2U,15U,3949U,586U},
  {2U,5U,1286U,587U},
  {2U,24U,1096U,588U},
  {2U,3U,2805U,589U},
  {2U,3U,2451U,590U},
  {2U,46U,1504U,591U},
  {2U,50U,2861U,592U},
  {2U,38U,4060U,593U},
  {2U,51U,664U,594U},
  {2U,14U,3028U,595U},
  {2U,59U,1673U,596U},
  {2U,60U,2848U,597U},
  {2U,9U,3381U,598U},
  {2U,24U,758U,599U},
  {2U,31U,1487U,600U},
  {2U,47U,617U,601U},
  {2U,24U,2857U,602U},
  {2U,22U,2192U,603U},
  {2U,36U,2291U,604U},
  {2U,6U,3704U,605U},
  {2U,5U,2834U,606U},
  {2U,55U,3371U,607U},
  {2U,42U,784U,608U},
  {2U,23U,98U,609U},
  {2U,53U,3759U,610U},
  {2U,10U,1883U,611U},
  {2U,52U,3355U,612U},
  {2U,61U,1147U,613U},
  {2U,36U,1559U,614U},
  {2U,25U,1758U,615U},
  {2U,4U,973U,616U},
  {2U,62U,2172U,617U},
  {2U,53U,3500U,618U},
  {2U,15U,1028U,619U},
  {2U,52U,3270U,620U},
  {2U,55U,3485U,621U},
  {2U,48U,2510U,622U},
  {2U,12U,3781U,623U},
  {2U,34U,1288U,624U},
  {2U,37U,1453U,625U},
  {2U,4U,3573U,626U},
  {2U,3U,2180U,627U},
  {2U,57U,3660U,628U},
  {2U,38U,3103U,629U},
  {2U,19U,2290U,630U},
  {2U,49U,2772U,631U},
  {2U,0U,2804U,632U},
  {2U,38U,3267U,633U},
  {2U,15U,2585U,634U},
  {2U,38U,3849U,635U},
  {2U,17U,292U,636U},
  {2U,53U,273U,637U},
  {2U,4U,3844U,638U},
  {2U,49U,488U,639U},
  {2U,28U,2125U,640U},
};
static const unsigned paper_offsets[5] = {0U,640U,1280U,1920U,2560U};
static const unsigned paper_work_starts[4] = {384U,1024U,1664U,2304U};
static const unsigned paper_expected_sums[4] = {36458U,36752U,36995U,37191U};
static const unsigned paper_expected_live[4] = {128347U,131978U,141718U,140569U};

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

