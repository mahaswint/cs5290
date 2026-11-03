#define NR_TASKLETS 4
#define POLICY 3
#define LISTS 1
#define LOOKUP 1
#define HEAP_BYTES 4194304
#define KEEP_PAGES 2
#define MAX_KEEP 2
#define EMPTY_BUDGET 64
#define DECAY_OPS 128
#define PRESSURE_PERCENT 75
#define VALIDATE 1
#define POOL_SIZE 32
#define INPUT_ITEMS 32768
#define TRACE_SEED 4141
#define REPEATS 2
#define PAPER_KIND 0
#define SLAB_BYTES 32768
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
static PAPER_INPUT struct PaperOp paper_ops[1024] = {
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
  {1U,27U,31U,0U},
  {1U,5U,30U,0U},
  {1U,20U,29U,0U},
  {1U,14U,28U,0U},
  {1U,20U,27U,0U},
  {1U,5U,26U,0U},
  {1U,25U,25U,0U},
  {1U,17U,24U,0U},
  {1U,0U,23U,0U},
  {1U,0U,22U,0U},
  {1U,7U,21U,0U},
  {1U,7U,20U,0U},
  {1U,8U,19U,0U},
  {1U,11U,18U,0U},
  {1U,14U,17U,0U},
  {1U,3U,16U,0U},
  {1U,0U,15U,0U},
  {1U,12U,14U,0U},
  {1U,7U,13U,0U},
  {1U,9U,12U,0U},
  {1U,4U,11U,0U},
  {1U,2U,10U,0U},
  {1U,3U,9U,0U},
  {1U,5U,8U,0U},
  {1U,3U,7U,0U},
  {1U,5U,6U,0U},
  {1U,1U,5U,0U},
  {1U,4U,4U,0U},
  {1U,0U,3U,0U},
  {1U,0U,2U,0U},
  {1U,0U,1U,0U},
  {1U,0U,0U,0U},
  {2U,3U,108U,65U},
  {2U,30U,156U,66U},
  {2U,30U,428U,67U},
  {2U,2U,657U,68U},
  {2U,6U,686U,69U},
  {2U,25U,555U,70U},
  {2U,11U,976U,71U},
  {2U,30U,655U,72U},
  {2U,1U,845U,73U},
  {2U,29U,483U,74U},
  {2U,19U,388U,75U},
  {2U,7U,660U,76U},
  {2U,4U,169U,77U},
  {2U,10U,534U,78U},
  {2U,9U,975U,79U},
  {2U,2U,429U,80U},
  {2U,0U,461U,81U},
  {2U,8U,186U,82U},
  {2U,9U,775U,83U},
  {2U,22U,658U,84U},
  {2U,12U,423U,85U},
  {2U,7U,616U,86U},
  {2U,16U,264U,87U},
  {2U,23U,552U,88U},
  {2U,27U,454U,89U},
  {2U,11U,421U,90U},
  {2U,23U,874U,91U},
  {2U,10U,464U,92U},
  {2U,27U,917U,93U},
  {2U,25U,891U,94U},
  {2U,22U,541U,95U},
  {2U,15U,786U,96U},
  {2U,19U,626U,97U},
  {2U,22U,937U,98U},
  {2U,21U,166U,99U},
  {2U,13U,354U,100U},
  {2U,0U,76U,101U},
  {2U,28U,694U,102U},
  {2U,31U,510U,103U},
  {2U,15U,749U,104U},
  {2U,5U,398U,105U},
  {2U,30U,91U,106U},
  {2U,28U,66U,107U},
  {2U,20U,239U,108U},
  {2U,10U,241U,109U},
  {2U,24U,863U,110U},
  {2U,19U,651U,111U},
  {2U,20U,634U,112U},
  {2U,14U,627U,113U},
  {2U,11U,441U,114U},
  {2U,29U,853U,115U},
  {2U,19U,715U,116U},
  {2U,5U,30U,117U},
  {2U,16U,680U,118U},
  {2U,23U,537U,119U},
  {2U,27U,857U,120U},
  {2U,7U,794U,121U},
  {2U,31U,777U,122U},
  {2U,28U,543U,123U},
  {2U,17U,27U,124U},
  {2U,11U,318U,125U},
  {2U,7U,24U,126U},
  {2U,26U,977U,127U},
  {2U,21U,712U,128U},
  {2U,11U,527U,129U},
  {2U,30U,67U,130U},
  {2U,28U,527U,131U},
  {2U,15U,479U,132U},
  {2U,21U,20U,133U},
  {2U,28U,326U,134U},
  {2U,21U,728U,135U},
  {2U,3U,894U,136U},
  {2U,21U,828U,137U},
  {2U,31U,708U,138U},
  {2U,5U,336U,139U},
  {2U,30U,904U,140U},
  {2U,9U,395U,141U},
  {2U,31U,49U,142U},
  {2U,26U,721U,143U},
  {2U,1U,100U,144U},
  {2U,27U,878U,145U},
  {2U,21U,820U,146U},
  {2U,11U,818U,147U},
  {2U,19U,300U,148U},
  {2U,17U,240U,149U},
  {2U,15U,915U,150U},
  {2U,7U,897U,151U},
  {2U,29U,82U,152U},
  {2U,31U,161U,153U},
  {2U,7U,298U,154U},
  {2U,20U,561U,155U},
  {2U,6U,966U,156U},
  {2U,9U,969U,157U},
  {2U,24U,13U,158U},
  {2U,12U,815U,159U},
  {2U,27U,331U,160U},
  {2U,3U,786U,161U},
  {2U,11U,352U,162U},
  {2U,10U,953U,163U},
  {2U,3U,485U,164U},
  {2U,13U,445U,165U},
  {2U,22U,191U,166U},
  {2U,5U,13U,167U},
  {2U,27U,858U,168U},
  {2U,7U,809U,169U},
  {2U,18U,918U,170U},
  {2U,7U,35U,171U},
  {2U,28U,134U,172U},
  {2U,0U,709U,173U},
  {2U,20U,467U,174U},
  {2U,2U,104U,175U},
  {2U,15U,831U,176U},
  {2U,26U,827U,177U},
  {2U,19U,69U,178U},
  {2U,4U,955U,179U},
  {2U,19U,351U,180U},
  {2U,29U,135U,181U},
  {2U,29U,270U,182U},
  {2U,20U,902U,183U},
  {2U,7U,241U,184U},
  {2U,8U,110U,185U},
  {2U,15U,752U,186U},
  {2U,12U,433U,187U},
  {2U,3U,840U,188U},
  {2U,8U,363U,189U},
  {2U,18U,273U,190U},
  {2U,20U,866U,191U},
  {2U,28U,277U,192U},
  {2U,29U,962U,193U},
  {2U,11U,417U,194U},
  {2U,6U,733U,195U},
  {2U,9U,497U,196U},
  {2U,5U,817U,197U},
  {2U,26U,960U,198U},
  {2U,20U,602U,199U},
  {2U,17U,108U,200U},
  {2U,0U,114U,201U},
  {2U,8U,474U,202U},
  {2U,12U,932U,203U},
  {2U,30U,465U,204U},
  {2U,15U,120U,205U},
  {2U,8U,343U,206U},
  {2U,22U,687U,207U},
  {2U,30U,454U,208U},
  {2U,16U,705U,209U},
  {2U,9U,30U,210U},
  {2U,28U,152U,211U},
  {2U,0U,419U,212U},
  {2U,11U,380U,213U},
  {2U,5U,602U,214U},
  {2U,29U,309U,215U},
  {2U,23U,254U,216U},
  {2U,30U,762U,217U},
  {2U,14U,862U,218U},
  {2U,0U,27U,219U},
  {2U,19U,202U,220U},
  {2U,11U,885U,221U},
  {2U,27U,866U,222U},
  {2U,0U,380U,223U},
  {2U,5U,178U,224U},
  {2U,2U,453U,225U},
  {2U,14U,68U,226U},
  {2U,22U,496U,227U},
  {2U,25U,468U,228U},
  {2U,14U,595U,229U},
  {2U,9U,434U,230U},
  {2U,22U,962U,231U},
  {2U,3U,112U,232U},
  {2U,30U,818U,233U},
  {2U,1U,843U,234U},
  {2U,25U,263U,235U},
  {2U,20U,29U,236U},
  {2U,1U,171U,237U},
  {2U,31U,919U,238U},
  {2U,24U,536U,239U},
  {2U,31U,871U,240U},
  {2U,6U,563U,241U},
  {2U,9U,152U,242U},
  {2U,23U,27U,243U},
  {2U,0U,227U,244U},
  {2U,31U,350U,245U},
  {2U,4U,777U,246U},
  {2U,4U,565U,247U},
  {2U,10U,930U,248U},
  {2U,12U,421U,249U},
  {2U,30U,908U,250U},
  {2U,7U,359U,251U},
  {2U,25U,576U,252U},
  {2U,6U,550U,253U},
  {2U,27U,842U,254U},
  {2U,30U,206U,255U},
  {2U,23U,526U,256U},
  {0U,0U,149U,0U},
  {0U,1U,276U,0U},
  {0U,2U,576U,0U},
  {0U,3U,516U,0U},
  {0U,4U,871U,0U},
  {0U,5U,98U,0U},
  {0U,6U,517U,0U},
  {0U,7U,817U,0U},
  {0U,8U,435U,0U},
  {0U,9U,248U,0U},
  {0U,10U,307U,0U},
  {0U,11U,360U,0U},
  {0U,12U,609U,0U},
  {0U,13U,735U,0U},
  {0U,14U,709U,0U},
  {0U,15U,123U,0U},
  {0U,16U,645U,0U},
  {0U,17U,691U,0U},
  {0U,18U,619U,0U},
  {0U,19U,42U,0U},
  {0U,20U,315U,0U},
  {0U,21U,98U,0U},
  {0U,22U,910U,0U},
  {0U,23U,335U,0U},
  {0U,24U,805U,0U},
  {0U,25U,823U,0U},
  {0U,26U,991U,0U},
  {0U,27U,379U,0U},
  {0U,28U,290U,0U},
  {0U,29U,614U,0U},
  {0U,30U,879U,0U},
  {0U,31U,67U,0U},
  {1U,8U,31U,0U},
  {1U,6U,30U,0U},
  {1U,10U,29U,0U},
  {1U,0U,28U,0U},
  {1U,23U,27U,0U},
  {1U,6U,26U,0U},
  {1U,9U,25U,0U},
  {1U,22U,24U,0U},
  {1U,23U,23U,0U},
  {1U,14U,22U,0U},
  {1U,20U,21U,0U},
  {1U,12U,20U,0U},
  {1U,4U,19U,0U},
  {1U,2U,18U,0U},
  {1U,1U,17U,0U},
  {1U,4U,16U,0U},
  {1U,14U,15U,0U},
  {1U,14U,14U,0U},
  {1U,0U,13U,0U},
  {1U,0U,12U,0U},
  {1U,2U,11U,0U},
  {1U,3U,10U,0U},
  {1U,8U,9U,0U},
  {1U,0U,8U,0U},
  {1U,5U,7U,0U},
  {1U,3U,6U,0U},
  {1U,1U,5U,0U},
  {1U,0U,4U,0U},
  {1U,2U,3U,0U},
  {1U,2U,2U,0U},
  {1U,0U,1U,0U},
  {1U,0U,0U,0U},
  {2U,14U,325U,65U},
  {2U,3U,510U,66U},
  {2U,3U,401U,67U},
  {2U,3U,66U,68U},
  {2U,29U,826U,69U},
  {2U,7U,365U,70U},
  {2U,16U,759U,71U},
  {2U,17U,377U,72U},
  {2U,6U,536U,73U},
  {2U,31U,119U,74U},
  {2U,6U,195U,75U},
  {2U,24U,481U,76U},
  {2U,8U,532U,77U},
  {2U,16U,924U,78U},
  {2U,22U,147U,79U},
  {2U,21U,97U,80U},
  {2U,10U,694U,81U},
  {2U,27U,203U,82U},
  {2U,26U,64U,83U},
  {2U,3U,273U,84U},
  {2U,4U,535U,85U},
  {2U,18U,480U,86U},
  {2U,21U,469U,87U},
  {2U,29U,408U,88U},
  {2U,4U,280U,89U},
  {2U,17U,540U,90U},
  {2U,0U,911U,91U},
  {2U,26U,565U,92U},
  {2U,31U,498U,93U},
  {2U,28U,571U,94U},
  {2U,12U,680U,95U},
  {2U,27U,840U,96U},
  {2U,2U,378U,97U},
  {2U,23U,57U,98U},
  {2U,8U,730U,99U},
  {2U,13U,368U,100U},
  {2U,13U,102U,101U},
  {2U,19U,584U,102U},
  {2U,22U,633U,103U},
  {2U,11U,215U,104U},
  {2U,5U,927U,105U},
  {2U,29U,257U,106U},
  {2U,1U,552U,107U},
  {2U,31U,211U,108U},
  {2U,18U,678U,109U},
  {2U,18U,877U,110U},
  {2U,28U,940U,111U},
  {2U,27U,698U,112U},
  {2U,3U,195U,113U},
  {2U,21U,959U,114U},
  {2U,23U,54U,115U},
  {2U,21U,758U,116U},
  {2U,6U,907U,117U},
  {2U,3U,882U,118U},
  {2U,13U,440U,119U},
  {2U,31U,264U,120U},
  {2U,30U,777U,121U},
  {2U,27U,983U,122U},
  {2U,8U,181U,123U},
  {2U,30U,422U,124U},
  {2U,25U,961U,125U},
  {2U,29U,203U,126U},
  {2U,23U,330U,127U},
  {2U,1U,611U,128U},
  {2U,24U,182U,129U},
  {2U,4U,569U,130U},
  {2U,8U,925U,131U},
  {2U,17U,171U,132U},
  {2U,20U,196U,133U},
  {2U,7U,624U,134U},
  {2U,22U,20U,135U},
  {2U,22U,370U,136U},
  {2U,22U,82U,137U},
  {2U,28U,678U,138U},
  {2U,22U,894U,139U},
  {2U,28U,105U,140U},
  {2U,11U,705U,141U},
  {2U,26U,452U,142U},
  {2U,22U,13U,143U},
  {2U,24U,27U,144U},
  {2U,6U,606U,145U},
  {2U,14U,452U,146U},
  {2U,20U,173U,147U},
  {2U,25U,426U,148U},
  {2U,31U,450U,149U},
  {2U,16U,849U,150U},
  {2U,30U,168U,151U},
  {2U,23U,931U,152U},
  {2U,5U,569U,153U},
  {2U,20U,422U,154U},
  {2U,26U,807U,155U},
  {2U,21U,117U,156U},
  {2U,22U,725U,157U},
  {2U,15U,233U,158U},
  {2U,10U,227U,159U},
  {2U,8U,425U,160U},
  {2U,1U,190U,161U},
  {2U,15U,750U,162U},
  {2U,20U,44U,163U},
  {2U,8U,198U,164U},
  {2U,3U,701U,165U},
  {2U,23U,324U,166U},
  {2U,22U,611U,167U},
  {2U,13U,127U,168U},
  {2U,7U,754U,169U},
  {2U,9U,648U,170U},
  {2U,4U,965U,171U},
  {2U,7U,110U,172U},
  {2U,29U,648U,173U},
  {2U,6U,482U,174U},
  {2U,22U,420U,175U},
  {2U,10U,696U,176U},
  {2U,8U,270U,177U},
  {2U,0U,394U,178U},
  {2U,27U,106U,179U},
  {2U,20U,790U,180U},
  {2U,17U,131U,181U},
  {2U,9U,467U,182U},
  {2U,9U,96U,183U},
  {2U,13U,42U,184U},
  {2U,25U,468U,185U},
  {2U,24U,412U,186U},
  {2U,29U,131U,187U},
  {2U,5U,736U,188U},
  {2U,31U,271U,189U},
  {2U,0U,322U,190U},
  {2U,22U,521U,191U},
  {2U,2U,663U,192U},
  {2U,23U,822U,193U},
  {2U,19U,514U,194U},
  {2U,8U,370U,195U},
  {2U,17U,19U,196U},
  {2U,29U,251U,197U},
  {2U,5U,645U,198U},
  {2U,13U,769U,199U},
  {2U,1U,539U,200U},
  {2U,13U,145U,201U},
  {2U,7U,182U,202U},
  {2U,14U,238U,203U},
  {2U,21U,503U,204U},
  {2U,16U,633U,205U},
  {2U,23U,520U,206U},
  {2U,25U,101U,207U},
  {2U,27U,501U,208U},
  {2U,6U,454U,209U},
  {2U,3U,265U,210U},
  {2U,20U,953U,211U},
  {2U,21U,532U,212U},
  {2U,29U,152U,213U},
  {2U,1U,262U,214U},
  {2U,25U,982U,215U},
  {2U,2U,526U,216U},
  {2U,10U,650U,217U},
  {2U,1U,662U,218U},
  {2U,25U,862U,219U},
  {2U,14U,736U,220U},
  {2U,16U,235U,221U},
  {2U,26U,281U,222U},
  {2U,30U,623U,223U},
  {2U,30U,997U,224U},
  {2U,1U,401U,225U},
  {2U,13U,347U,226U},
  {2U,27U,799U,227U},
  {2U,18U,319U,228U},
  {2U,2U,213U,229U},
  {2U,17U,799U,230U},
  {2U,7U,867U,231U},
  {2U,30U,243U,232U},
  {2U,29U,971U,233U},
  {2U,0U,418U,234U},
  {2U,12U,489U,235U},
  {2U,6U,45U,236U},
  {2U,22U,998U,237U},
  {2U,24U,96U,238U},
  {2U,27U,264U,239U},
  {2U,0U,482U,240U},
  {2U,13U,967U,241U},
  {2U,23U,904U,242U},
  {2U,23U,375U,243U},
  {2U,4U,531U,244U},
  {2U,31U,458U,245U},
  {2U,25U,250U,246U},
  {2U,16U,848U,247U},
  {2U,14U,777U,248U},
  {2U,16U,909U,249U},
  {2U,21U,796U,250U},
  {2U,15U,265U,251U},
  {2U,20U,267U,252U},
  {2U,31U,251U,253U},
  {2U,2U,495U,254U},
  {2U,30U,18U,255U},
  {2U,27U,312U,256U},
  {0U,0U,473U,0U},
  {0U,1U,149U,0U},
  {0U,2U,511U,0U},
  {0U,3U,451U,0U},
  {0U,4U,178U,0U},
  {0U,5U,698U,0U},
  {0U,6U,935U,0U},
  {0U,7U,167U,0U},
  {0U,8U,888U,0U},
  {0U,9U,125U,0U},
  {0U,10U,852U,0U},
  {0U,11U,597U,0U},
  {0U,12U,151U,0U},
  {0U,13U,468U,0U},
  {0U,14U,483U,0U},
  {0U,15U,149U,0U},
  {0U,16U,701U,0U},
  {0U,17U,580U,0U},
  {0U,18U,156U,0U},
  {0U,19U,236U,0U},
  {0U,20U,102U,0U},
  {0U,21U,598U,0U},
  {0U,22U,587U,0U},
  {0U,23U,405U,0U},
  {0U,24U,304U,0U},
  {0U,25U,461U,0U},
  {0U,26U,348U,0U},
  {0U,27U,324U,0U},
  {0U,28U,160U,0U},
  {0U,29U,58U,0U},
  {0U,30U,332U,0U},
  {0U,31U,69U,0U},
  {1U,0U,31U,0U},
  {1U,29U,30U,0U},
  {1U,15U,29U,0U},
  {1U,7U,28U,0U},
  {1U,21U,27U,0U},
  {1U,2U,26U,0U},
  {1U,11U,25U,0U},
  {1U,8U,24U,0U},
  {1U,4U,23U,0U},
  {1U,9U,22U,0U},
  {1U,19U,21U,0U},
  {1U,10U,20U,0U},
  {1U,19U,19U,0U},
  {1U,13U,18U,0U},
  {1U,13U,17U,0U},
  {1U,11U,16U,0U},
  {1U,13U,15U,0U},
  {1U,11U,14U,0U},
  {1U,9U,13U,0U},
  {1U,11U,12U,0U},
  {1U,4U,11U,0U},
  {1U,10U,10U,0U},
  {1U,3U,9U,0U},
  {1U,3U,8U,0U},
  {1U,2U,7U,0U},
  {1U,5U,6U,0U},
  {1U,1U,5U,0U},
  {1U,2U,4U,0U},
  {1U,1U,3U,0U},
  {1U,0U,2U,0U},
  {1U,0U,1U,0U},
  {1U,0U,0U,0U},
  {2U,1U,312U,65U},
  {2U,10U,537U,66U},
  {2U,21U,506U,67U},
  {2U,1U,329U,68U},
  {2U,20U,675U,69U},
  {2U,18U,37U,70U},
  {2U,28U,629U,71U},
  {2U,20U,477U,72U},
  {2U,22U,146U,73U},
  {2U,5U,901U,74U},
  {2U,31U,459U,75U},
  {2U,2U,530U,76U},
  {2U,29U,59U,77U},
  {2U,1U,263U,78U},
  {2U,14U,776U,79U},
  {2U,23U,577U,80U},
  {2U,8U,341U,81U},
  {2U,4U,313U,82U},
  {2U,19U,211U,83U},
  {2U,4U,831U,84U},
  {2U,31U,99U,85U},
  {2U,10U,507U,86U},
  {2U,17U,194U,87U},
  {2U,14U,896U,88U},
  {2U,12U,106U,89U},
  {2U,8U,831U,90U},
  {2U,9U,226U,91U},
  {2U,4U,331U,92U},
  {2U,2U,115U,93U},
  {2U,4U,946U,94U},
  {2U,4U,619U,95U},
  {2U,24U,858U,96U},
  {2U,3U,406U,97U},
  {2U,15U,608U,98U},
  {2U,20U,208U,99U},
  {2U,13U,432U,100U},
  {2U,8U,612U,101U},
  {2U,23U,390U,102U},
  {2U,6U,600U,103U},
  {2U,15U,840U,104U},
  {2U,2U,774U,105U},
  {2U,5U,469U,106U},
  {2U,23U,726U,107U},
  {2U,8U,711U,108U},
  {2U,1U,314U,109U},
  {2U,14U,430U,110U},
  {2U,21U,25U,111U},
  {2U,9U,316U,112U},
  {2U,6U,241U,113U},
  {2U,10U,49U,114U},
  {2U,22U,104U,115U},
  {2U,18U,382U,116U},
  {2U,28U,624U,117U},
  {2U,13U,844U,118U},
  {2U,5U,348U,119U},
  {2U,1U,877U,120U},
  {2U,1U,710U,121U},
  {2U,9U,438U,122U},
  {2U,21U,115U,123U},
  {2U,21U,32U,124U},
  {2U,26U,418U,125U},
  {2U,8U,226U,126U},
  {2U,29U,934U,127U},
  {2U,6U,585U,128U},
  {2U,0U,285U,129U},
  {2U,7U,374U,130U},
  {2U,17U,258U,131U},
  {2U,31U,630U,132U},
  {2U,22U,481U,133U},
  {2U,7U,61U,134U},
  {2U,29U,174U,135U},
  {2U,17U,941U,136U},
  {2U,5U,830U,137U},
  {2U,12U,427U,138U},
  {2U,26U,95U,139U},
  {2U,25U,85U,140U},
  {2U,21U,875U,141U},
  {2U,20U,263U,142U},
  {2U,22U,626U,143U},
  {2U,29U,744U,144U},
  {2U,3U,362U,145U},
  {2U,0U,930U,146U},
  {2U,4U,122U,147U},
  {2U,1U,398U,148U},
  {2U,6U,990U,149U},
  {2U,19U,846U,150U},
  {2U,16U,891U,151U},
  {2U,1U,159U,152U},
  {2U,7U,912U,153U},
  {2U,13U,442U,154U},
  {2U,6U,840U,155U},
  {2U,11U,546U,156U},
  {2U,8U,908U,157U},
  {2U,4U,98U,158U},
  {2U,1U,820U,159U},
  {2U,1U,168U,160U},
  {2U,14U,455U,161U},
  {2U,7U,829U,162U},
  {2U,19U,734U,163U},
  {2U,13U,856U,164U},
  {2U,30U,930U,165U},
  {2U,26U,287U,166U},
  {2U,27U,991U,167U},
  {2U,9U,765U,168U},
  {2U,20U,245U,169U},
  {2U,23U,638U,170U},
  {2U,23U,202U,171U},
  {2U,10U,260U,172U},
  {2U,26U,87U,173U},
  {2U,12U,479U,174U},
  {2U,14U,55U,175U},
  {2U,31U,60U,176U},
  {2U,30U,700U,177U},
  {2U,22U,877U,178U},
  {2U,23U,668U,179U},
  {2U,12U,658U,180U},
  {2U,3U,675U,181U},
  {2U,26U,462U,182U},
  {2U,20U,832U,183U},
  {2U,8U,41U,184U},
  {2U,22U,667U,185U},
  {2U,18U,887U,186U},
  {2U,8U,333U,187U},
  {2U,12U,770U,188U},
  {2U,26U,274U,189U},
  {2U,3U,664U,190U},
  {2U,23U,993U,191U},
  {2U,29U,448U,192U},
  {2U,27U,453U,193U},
  {2U,23U,105U,194U},
  {2U,28U,839U,195U},
  {2U,2U,505U,196U},
  {2U,11U,233U,197U},
  {2U,2U,517U,198U},
  {2U,18U,973U,199U},
  {2U,7U,937U,200U},
  {2U,20U,168U,201U},
  {2U,4U,701U,202U},
  {2U,11U,436U,203U},
  {2U,0U,783U,204U},
  {2U,28U,727U,205U},
  {2U,3U,68U,206U},
  {2U,19U,434U,207U},
  {2U,2U,47U,208U},
  {2U,2U,574U,209U},
  {2U,29U,66U,210U},
  {2U,1U,83U,211U},
  {2U,28U,992U,212U},
  {2U,14U,13U,213U},
  {2U,24U,91U,214U},
  {2U,24U,470U,215U},
  {2U,24U,924U,216U},
  {2U,24U,85U,217U},
  {2U,28U,504U,218U},
  {2U,23U,546U,219U},
  {2U,9U,442U,220U},
  {2U,23U,67U,221U},
  {2U,3U,157U,222U},
  {2U,12U,669U,223U},
  {2U,27U,284U,224U},
  {2U,16U,898U,225U},
  {2U,13U,690U,226U},
  {2U,12U,960U,227U},
  {2U,28U,998U,228U},
  {2U,6U,913U,229U},
  {2U,21U,332U,230U},
  {2U,19U,26U,231U},
  {2U,1U,894U,232U},
  {2U,15U,83U,233U},
  {2U,31U,243U,234U},
  {2U,31U,122U,235U},
  {2U,22U,335U,236U},
  {2U,5U,467U,237U},
  {2U,5U,828U,238U},
  {2U,23U,428U,239U},
  {2U,22U,34U,240U},
  {2U,12U,151U,241U},
  {2U,31U,624U,242U},
  {2U,23U,507U,243U},
  {2U,6U,694U,244U},
  {2U,21U,482U,245U},
  {2U,11U,99U,246U},
  {2U,5U,890U,247U},
  {2U,18U,455U,248U},
  {2U,28U,929U,249U},
  {2U,19U,752U,250U},
  {2U,26U,314U,251U},
  {2U,6U,361U,252U},
  {2U,23U,219U,253U},
  {2U,6U,466U,254U},
  {2U,26U,417U,255U},
  {2U,21U,249U,256U},
  {0U,0U,762U,0U},
  {0U,1U,680U,0U},
  {0U,2U,372U,0U},
  {0U,3U,720U,0U},
  {0U,4U,740U,0U},
  {0U,5U,49U,0U},
  {0U,6U,742U,0U},
  {0U,7U,770U,0U},
  {0U,8U,401U,0U},
  {0U,9U,163U,0U},
  {0U,10U,245U,0U},
  {0U,11U,560U,0U},
  {0U,12U,127U,0U},
  {0U,13U,527U,0U},
  {0U,14U,546U,0U},
  {0U,15U,636U,0U},
  {0U,16U,24U,0U},
  {0U,17U,485U,0U},
  {0U,18U,366U,0U},
  {0U,19U,207U,0U},
  {0U,20U,541U,0U},
  {0U,21U,670U,0U},
  {0U,22U,769U,0U},
  {0U,23U,256U,0U},
  {0U,24U,600U,0U},
  {0U,25U,925U,0U},
  {0U,26U,459U,0U},
  {0U,27U,300U,0U},
  {0U,28U,684U,0U},
  {0U,29U,667U,0U},
  {0U,30U,510U,0U},
  {0U,31U,854U,0U},
  {1U,23U,31U,0U},
  {1U,8U,30U,0U},
  {1U,10U,29U,0U},
  {1U,14U,28U,0U},
  {1U,24U,27U,0U},
  {1U,22U,26U,0U},
  {1U,12U,25U,0U},
  {1U,18U,24U,0U},
  {1U,22U,23U,0U},
  {1U,14U,22U,0U},
  {1U,9U,21U,0U},
  {1U,4U,20U,0U},
  {1U,6U,19U,0U},
  {1U,0U,18U,0U},
  {1U,14U,17U,0U},
  {1U,12U,16U,0U},
  {1U,3U,15U,0U},
  {1U,4U,14U,0U},
  {1U,6U,13U,0U},
  {1U,12U,12U,0U},
  {1U,1U,11U,0U},
  {1U,9U,10U,0U},
  {1U,4U,9U,0U},
  {1U,8U,8U,0U},
  {1U,3U,7U,0U},
  {1U,3U,6U,0U},
  {1U,5U,5U,0U},
  {1U,1U,4U,0U},
  {1U,3U,3U,0U},
  {1U,2U,2U,0U},
  {1U,1U,1U,0U},
  {1U,0U,0U,0U},
  {2U,12U,223U,65U},
  {2U,29U,64U,66U},
  {2U,27U,625U,67U},
  {2U,26U,609U,68U},
  {2U,31U,511U,69U},
  {2U,31U,603U,70U},
  {2U,19U,398U,71U},
  {2U,24U,855U,72U},
  {2U,2U,346U,73U},
  {2U,9U,754U,74U},
  {2U,15U,428U,75U},
  {2U,17U,142U,76U},
  {2U,13U,296U,77U},
  {2U,0U,254U,78U},
  {2U,24U,82U,79U},
  {2U,1U,679U,80U},
  {2U,4U,786U,81U},
  {2U,15U,671U,82U},
  {2U,28U,354U,83U},
  {2U,7U,92U,84U},
  {2U,29U,112U,85U},
  {2U,6U,860U,86U},
  {2U,29U,833U,87U},
  {2U,12U,207U,88U},
  {2U,21U,235U,89U},
  {2U,26U,832U,90U},
  {2U,31U,132U,91U},
  {2U,21U,835U,92U},
  {2U,2U,157U,93U},
  {2U,1U,452U,94U},
  {2U,17U,428U,95U},
  {2U,23U,479U,96U},
  {2U,6U,226U,97U},
  {2U,16U,677U,98U},
  {2U,2U,757U,99U},
  {2U,6U,152U,100U},
  {2U,26U,325U,101U},
  {2U,11U,499U,102U},
  {2U,22U,102U,103U},
  {2U,29U,251U,104U},
  {2U,3U,997U,105U},
  {2U,14U,609U,106U},
  {2U,25U,427U,107U},
  {2U,8U,83U,108U},
  {2U,22U,13U,109U},
  {2U,23U,681U,110U},
  {2U,29U,546U,111U},
  {2U,4U,206U,112U},
  {2U,0U,539U,113U},
  {2U,11U,946U,114U},
  {2U,12U,313U,115U},
  {2U,15U,878U,116U},
  {2U,3U,348U,117U},
  {2U,9U,818U,118U},
  {2U,11U,877U,119U},
  {2U,12U,262U,120U},
  {2U,23U,211U,121U},
  {2U,30U,693U,122U},
  {2U,18U,385U,123U},
  {2U,23U,414U,124U},
  {2U,23U,170U,125U},
  {2U,1U,413U,126U},
  {2U,30U,102U,127U},
  {2U,26U,716U,128U},
  {2U,12U,624U,129U},
  {2U,16U,349U,130U},
  {2U,18U,920U,131U},
  {2U,23U,371U,132U},
  {2U,15U,655U,133U},
  {2U,7U,335U,134U},
  {2U,10U,96U,135U},
  {2U,27U,693U,136U},
  {2U,18U,466U,137U},
  {2U,4U,575U,138U},
  {2U,15U,817U,139U},
  {2U,3U,780U,140U},
  {2U,1U,828U,141U},
  {2U,15U,54U,142U},
  {2U,11U,799U,143U},
  {2U,24U,401U,144U},
  {2U,10U,41U,145U},
  {2U,7U,965U,146U},
  {2U,14U,177U,147U},
  {2U,16U,267U,148U},
  {2U,3U,945U,149U},
  {2U,10U,394U,150U},
  {2U,14U,447U,151U},
  {2U,20U,887U,152U},
  {2U,7U,275U,153U},
  {2U,11U,323U,154U},
  {2U,12U,948U,155U},
  {2U,2U,523U,156U},
  {2U,20U,725U,157U},
  {2U,7U,33U,158U},
  {2U,29U,467U,159U},
  {2U,29U,88U,160U},
  {2U,15U,580U,161U},
  {2U,18U,9U,162U},
  {2U,30U,290U,163U},
  {2U,15U,842U,164U},
  {2U,9U,588U,165U},
  {2U,27U,890U,166U},
  {2U,21U,155U,167U},
  {2U,29U,589U,168U},
  {2U,20U,531U,169U},
  {2U,29U,637U,170U},
  {2U,2U,579U,171U},
  {2U,20U,501U,172U},
  {2U,27U,162U,173U},
  {2U,23U,640U,174U},
  {2U,29U,774U,175U},
  {2U,6U,132U,176U},
  {2U,24U,826U,177U},
  {2U,25U,452U,178U},
  {2U,26U,343U,179U},
  {2U,17U,358U,180U},
  {2U,30U,213U,181U},
  {2U,4U,523U,182U},
  {2U,16U,676U,183U},
  {2U,12U,722U,184U},
  {2U,9U,603U,185U},
  {2U,8U,528U,186U},
  {2U,25U,452U,187U},
  {2U,25U,394U,188U},
  {2U,7U,240U,189U},
  {2U,11U,572U,190U},
  {2U,15U,356U,191U},
  {2U,25U,314U,192U},
  {2U,29U,22U,193U},
  {2U,12U,753U,194U},
  {2U,19U,946U,195U},
  {2U,25U,691U,196U},
  {2U,1U,503U,197U},
  {2U,6U,27U,198U},
  {2U,5U,94U,199U},
  {2U,15U,993U,200U},
  {2U,25U,71U,201U},
  {2U,16U,766U,202U},
  {2U,10U,359U,203U},
  {2U,31U,418U,204U},
  {2U,7U,435U,205U},
  {2U,21U,178U,206U},
  {2U,29U,138U,207U},
  {2U,27U,115U,208U},
  {2U,7U,434U,209U},
  {2U,13U,368U,210U},
  {2U,31U,405U,211U},
  {2U,5U,730U,212U},
  {2U,15U,121U,213U},
  {2U,26U,356U,214U},
  {2U,0U,802U,215U},
  {2U,24U,304U,216U},
  {2U,24U,16U,217U},
  {2U,6U,549U,218U},
  {2U,17U,670U,219U},
  {2U,26U,162U,220U},
  {2U,31U,605U,221U},
  {2U,21U,808U,222U},
  {2U,14U,838U,223U},
  {2U,28U,624U,224U},
  {2U,10U,398U,225U},
  {2U,20U,324U,226U},
  {2U,17U,193U,227U},
  {2U,17U,584U,228U},
  {2U,29U,506U,229U},
  {2U,25U,553U,230U},
  {2U,30U,173U,231U},
  {2U,19U,629U,232U},
  {2U,8U,566U,233U},
  {2U,25U,491U,234U},
  {2U,24U,826U,235U},
  {2U,2U,343U,236U},
  {2U,25U,846U,237U},
  {2U,30U,222U,238U},
  {2U,16U,893U,239U},
  {2U,7U,231U,240U},
  {2U,21U,679U,241U},
  {2U,27U,644U,242U},
  {2U,18U,615U,243U},
  {2U,2U,303U,244U},
  {2U,24U,787U,245U},
  {2U,7U,402U,246U},
  {2U,22U,762U,247U},
  {2U,15U,975U,248U},
  {2U,29U,597U,249U},
  {2U,26U,877U,250U},
  {2U,19U,950U,251U},
  {2U,22U,161U,252U},
  {2U,16U,774U,253U},
  {2U,14U,813U,254U},
  {2U,7U,914U,255U},
  {2U,30U,782U,256U},
};
static const unsigned paper_offsets[5] = {0U,256U,512U,768U,1024U};
static const unsigned paper_work_starts[4] = {192U,448U,704U,960U};
static const unsigned paper_expected_sums[4] = {7235U,7275U,7031U,7203U};
static const unsigned paper_expected_live[4] = {14062U,15311U,14379U,20806U};

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

