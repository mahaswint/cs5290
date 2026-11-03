#define NR_TASKLETS 1
#define POLICY 2
#define LISTS 1
#define LOOKUP 1
#define HEAP_BYTES 33554432
#define KEEP_PAGES 2
#define MAX_KEEP 2
#define EMPTY_BUDGET 64
#define DECAY_OPS 128
#define PRESSURE_PERCENT 75
#define VALIDATE 0
#define POOL_SIZE 512
#define INPUT_ITEMS 32768
#define TRACE_SEED 4141
#define REPEATS 2
#define PAPER_KIND 2
#define SLAB_BYTES 65536
/* Paper-style paged adjacency / growing power-of-two arrays. The faimGraph
 * variant additionally performs arbitrary-edge deletion with tail compaction.
 * Graph structure lives in MRAM; only aligned DMA buffers live in WRAM.
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
#define PAPER_VERTICES 5372
static PAPER_INPUT struct PaperOp paper_ops[6144] = {
  {3U,570U,621U,0U},
  {3U,621U,570U,0U},
  {3U,4118U,4184U,0U},
  {3U,4184U,4118U,0U},
  {3U,2002U,2407U,0U},
  {3U,2407U,2002U,0U},
  {3U,2983U,4917U,0U},
  {3U,4917U,2983U,0U},
  {3U,4679U,4680U,0U},
  {3U,4680U,4679U,0U},
  {3U,3857U,4526U,0U},
  {3U,4526U,3857U,0U},
  {3U,322U,1857U,0U},
  {3U,1857U,322U,0U},
  {3U,711U,2034U,0U},
  {3U,2034U,711U,0U},
  {3U,338U,1401U,0U},
  {3U,1401U,338U,0U},
  {3U,493U,1894U,0U},
  {3U,1894U,493U,0U},
  {3U,56U,958U,0U},
  {3U,958U,56U,0U},
  {3U,2826U,3044U,0U},
  {3U,3044U,2826U,0U},
  {3U,1099U,2436U,0U},
  {3U,2436U,1099U,0U},
  {3U,1049U,1072U,0U},
  {3U,1072U,1049U,0U},
  {3U,914U,2139U,0U},
  {3U,2139U,914U,0U},
  {3U,263U,3089U,0U},
  {3U,3089U,263U,0U},
  {3U,2998U,3185U,0U},
  {3U,3185U,2998U,0U},
  {3U,791U,2238U,0U},
  {3U,2238U,791U,0U},
  {3U,293U,2126U,0U},
  {3U,2126U,293U,0U},
  {3U,4285U,4308U,0U},
  {3U,4308U,4285U,0U},
  {3U,1538U,4176U,0U},
  {3U,4176U,1538U,0U},
  {3U,4899U,4956U,0U},
  {3U,4956U,4899U,0U},
  {3U,104U,500U,0U},
  {3U,500U,104U,0U},
  {3U,3969U,3970U,0U},
  {3U,3970U,3969U,0U},
  {3U,884U,1126U,0U},
  {3U,1126U,884U,0U},
  {3U,2728U,2735U,0U},
  {3U,2735U,2728U,0U},
  {3U,2344U,2906U,0U},
  {3U,2906U,2344U,0U},
  {3U,4315U,4836U,0U},
  {3U,4836U,4315U,0U},
  {3U,4495U,5256U,0U},
  {3U,5256U,4495U,0U},
  {3U,3440U,3988U,0U},
  {3U,3988U,3440U,0U},
  {3U,316U,1740U,0U},
  {3U,1740U,316U,0U},
  {3U,2497U,3281U,0U},
  {3U,3281U,2497U,0U},
  {3U,1422U,2458U,0U},
  {3U,2458U,1422U,0U},
  {3U,4061U,4253U,0U},
  {3U,4253U,4061U,0U},
  {3U,1671U,4460U,0U},
  {3U,4460U,1671U,0U},
  {3U,1487U,1925U,0U},
  {3U,1925U,1487U,0U},
  {3U,4470U,4471U,0U},
  {3U,4471U,4470U,0U},
  {3U,3243U,4081U,0U},
  {3U,4081U,3243U,0U},
  {3U,100U,2124U,0U},
  {3U,2124U,100U,0U},
  {3U,627U,1812U,0U},
  {3U,1812U,627U,0U},
  {3U,2918U,3615U,0U},
  {3U,3615U,2918U,0U},
  {3U,1263U,3292U,0U},
  {3U,3292U,1263U,0U},
  {3U,1181U,3961U,0U},
  {3U,3961U,1181U,0U},
  {3U,53U,1713U,0U},
  {3U,1713U,53U,0U},
  {3U,836U,3654U,0U},
  {3U,3654U,836U,0U},
  {3U,3361U,5009U,0U},
  {3U,5009U,3361U,0U},
  {3U,186U,1496U,0U},
  {3U,1496U,186U,0U},
  {3U,952U,1228U,0U},
  {3U,1228U,952U,0U},
  {3U,2837U,3816U,0U},
  {3U,3816U,2837U,0U},
  {3U,612U,2625U,0U},
  {3U,2625U,612U,0U},
  {3U,2386U,3746U,0U},
  {3U,3746U,2386U,0U},
  {3U,1295U,1667U,0U},
  {3U,1667U,1295U,0U},
  {3U,1165U,2438U,0U},
  {3U,2438U,1165U,0U},
  {3U,366U,520U,0U},
  {3U,520U,366U,0U},
  {3U,5292U,5333U,0U},
  {3U,5333U,5292U,0U},
  {3U,5265U,5266U,0U},
  {3U,5266U,5265U,0U},
  {3U,623U,2769U,0U},
  {3U,2769U,623U,0U},
  {3U,2353U,4747U,0U},
  {3U,4747U,2353U,0U},
  {3U,4884U,5172U,0U},
  {3U,5172U,4884U,0U},
  {3U,3810U,4350U,0U},
  {3U,4350U,3810U,0U},
  {3U,4604U,5118U,0U},
  {3U,5118U,4604U,0U},
  {3U,1259U,4005U,0U},
  {3U,4005U,1259U,0U},
  {3U,2913U,4428U,0U},
  {3U,4428U,2913U,0U},
  {3U,504U,2203U,0U},
  {3U,2203U,504U,0U},
  {3U,5142U,5217U,0U},
  {3U,5217U,5142U,0U},
  {3U,1578U,4259U,0U},
  {3U,4259U,1578U,0U},
  {3U,4343U,4856U,0U},
  {3U,4856U,4343U,0U},
  {3U,27U,187U,0U},
  {3U,187U,27U,0U},
  {3U,672U,2005U,0U},
  {3U,2005U,672U,0U},
  {3U,559U,1491U,0U},
  {3U,1491U,559U,0U},
  {3U,2166U,4092U,0U},
  {3U,4092U,2166U,0U},
  {3U,4924U,5328U,0U},
  {3U,5328U,4924U,0U},
  {3U,2783U,3193U,0U},
  {3U,3193U,2783U,0U},
  {3U,2127U,3322U,0U},
  {3U,3322U,2127U,0U},
  {3U,1562U,2507U,0U},
  {3U,2507U,1562U,0U},
  {3U,174U,2677U,0U},
  {3U,2677U,174U,0U},
  {3U,1779U,2928U,0U},
  {3U,2928U,1779U,0U},
  {3U,1597U,2053U,0U},
  {3U,2053U,1597U,0U},
  {3U,1071U,1889U,0U},
  {3U,1889U,1071U,0U},
  {3U,2501U,4783U,0U},
  {3U,4783U,2501U,0U},
  {3U,3192U,3468U,0U},
  {3U,3468U,3192U,0U},
  {3U,1570U,2500U,0U},
  {3U,2500U,1570U,0U},
  {3U,71U,3239U,0U},
  {3U,3239U,71U,0U},
  {3U,3347U,5002U,0U},
  {3U,5002U,3347U,0U},
  {3U,171U,2647U,0U},
  {3U,2647U,171U,0U},
  {3U,1216U,1223U,0U},
  {3U,1223U,1216U,0U},
  {3U,241U,1112U,0U},
  {3U,1112U,241U,0U},
  {3U,81U,1946U,0U},
  {3U,1946U,81U,0U},
  {3U,1617U,4328U,0U},
  {3U,4328U,1617U,0U},
  {3U,3641U,3976U,0U},
  {3U,3976U,3641U,0U},
  {3U,385U,1381U,0U},
  {3U,1381U,385U,0U},
  {3U,2748U,3458U,0U},
  {3U,3458U,2748U,0U},
  {3U,1047U,1068U,0U},
  {3U,1068U,1047U,0U},
  {3U,3158U,3477U,0U},
  {3U,3477U,3158U,0U},
  {3U,2511U,3685U,0U},
  {3U,3685U,2511U,0U},
  {3U,10U,680U,0U},
  {3U,680U,10U,0U},
  {3U,32U,1220U,0U},
  {3U,1220U,32U,0U},
  {3U,2548U,4185U,0U},
  {3U,4185U,2548U,0U},
  {3U,4244U,4489U,0U},
  {3U,4489U,4244U,0U},
  {3U,571U,1489U,0U},
  {3U,1489U,571U,0U},
  {3U,5021U,5126U,0U},
  {3U,5126U,5021U,0U},
  {3U,2232U,4707U,0U},
  {3U,4707U,2232U,0U},
  {3U,275U,914U,0U},
  {3U,914U,275U,0U},
  {3U,2855U,2858U,0U},
  {3U,2858U,2855U,0U},
  {3U,806U,1819U,0U},
  {3U,1819U,806U,0U},
  {3U,1914U,4608U,0U},
  {3U,4608U,1914U,0U},
  {3U,53U,1649U,0U},
  {3U,1649U,53U,0U},
  {3U,745U,3766U,0U},
  {3U,3766U,745U,0U},
  {3U,3865U,4676U,0U},
  {3U,4676U,3865U,0U},
  {3U,877U,3709U,0U},
  {3U,3709U,877U,0U},
  {3U,1631U,3355U,0U},
  {3U,3355U,1631U,0U},
  {3U,852U,3804U,0U},
  {3U,3804U,852U,0U},
  {3U,283U,633U,0U},
  {3U,633U,283U,0U},
  {3U,3349U,4903U,0U},
  {3U,4903U,3349U,0U},
  {3U,120U,2334U,0U},
  {3U,2334U,120U,0U},
  {3U,476U,890U,0U},
  {3U,890U,476U,0U},
  {3U,4304U,5218U,0U},
  {3U,5218U,4304U,0U},
  {3U,976U,2039U,0U},
  {3U,2039U,976U,0U},
  {3U,763U,1304U,0U},
  {3U,1304U,763U,0U},
  {3U,120U,446U,0U},
  {3U,446U,120U,0U},
  {3U,76U,1905U,0U},
  {3U,1905U,76U,0U},
  {3U,726U,2003U,0U},
  {3U,2003U,726U,0U},
  {3U,4355U,4870U,0U},
  {3U,4870U,4355U,0U},
  {3U,255U,3030U,0U},
  {3U,3030U,255U,0U},
  {3U,1684U,4475U,0U},
  {3U,4475U,1684U,0U},
  {3U,1090U,3123U,0U},
  {3U,3123U,1090U,0U},
  {3U,5313U,5357U,0U},
  {3U,5357U,5313U,0U},
  {3U,2554U,2826U,0U},
  {3U,2826U,2554U,0U},
  {3U,2038U,2700U,0U},
  {3U,2700U,2038U,0U},
  {3U,226U,1286U,0U},
  {3U,1286U,226U,0U},
  {3U,2842U,3222U,0U},
  {3U,3222U,2842U,0U},
  {3U,371U,2883U,0U},
  {3U,2883U,371U,0U},
  {3U,2158U,3423U,0U},
  {3U,3423U,2158U,0U},
  {3U,4220U,5182U,0U},
  {3U,5182U,4220U,0U},
  {3U,122U,2354U,0U},
  {3U,2354U,122U,0U},
  {3U,238U,2986U,0U},
  {3U,2986U,238U,0U},
  {3U,232U,411U,0U},
  {3U,411U,232U,0U},
  {3U,4135U,4136U,0U},
  {3U,4136U,4135U,0U},
  {3U,4172U,4822U,0U},
  {3U,4822U,4172U,0U},
  {3U,2017U,2019U,0U},
  {3U,2019U,2017U,0U},
  {3U,1292U,1299U,0U},
  {3U,1299U,1292U,0U},
  {3U,3733U,5083U,0U},
  {3U,5083U,3733U,0U},
  {3U,53U,1774U,0U},
  {3U,1774U,53U,0U},
  {3U,1550U,4191U,0U},
  {3U,4191U,1550U,0U},
  {3U,554U,725U,0U},
  {3U,725U,554U,0U},
  {3U,3134U,3241U,0U},
  {3U,3241U,3134U,0U},
  {3U,635U,746U,0U},
  {3U,746U,635U,0U},
  {3U,120U,1101U,0U},
  {3U,1101U,120U,0U},
  {3U,137U,844U,0U},
  {3U,844U,137U,0U},
  {3U,2635U,3160U,0U},
  {3U,3160U,2635U,0U},
  {3U,71U,3855U,0U},
  {3U,3855U,71U,0U},
  {3U,264U,977U,0U},
  {3U,977U,264U,0U},
  {3U,192U,2135U,0U},
  {3U,2135U,192U,0U},
  {3U,186U,2390U,0U},
  {3U,2390U,186U,0U},
  {3U,4751U,5291U,0U},
  {3U,5291U,4751U,0U},
  {3U,1464U,2817U,0U},
  {3U,2817U,1464U,0U},
  {3U,244U,1461U,0U},
  {3U,1461U,244U,0U},
  {3U,3020U,3310U,0U},
  {3U,3310U,3020U,0U},
  {3U,3014U,3704U,0U},
  {3U,3704U,3014U,0U},
  {3U,911U,962U,0U},
  {3U,962U,911U,0U},
  {3U,2464U,3951U,0U},
  {3U,3951U,2464U,0U},
  {3U,1772U,3026U,0U},
  {3U,3026U,1772U,0U},
  {3U,1479U,2355U,0U},
  {3U,2355U,1479U,0U},
  {3U,2360U,3735U,0U},
  {3U,3735U,2360U,0U},
  {3U,2648U,3849U,0U},
  {3U,3849U,2648U,0U},
  {3U,78U,1924U,0U},
  {3U,1924U,78U,0U},
  {3U,2592U,4056U,0U},
  {3U,4056U,2592U,0U},
  {3U,1383U,3950U,0U},
  {3U,3950U,1383U,0U},
  {3U,1545U,2979U,0U},
  {3U,2979U,1545U,0U},
  {3U,173U,2672U,0U},
  {3U,2672U,173U,0U},
  {3U,699U,1346U,0U},
  {3U,1346U,699U,0U},
  {3U,3724U,3973U,0U},
  {3U,3973U,3724U,0U},
  {3U,2563U,2676U,0U},
  {3U,2676U,2563U,0U},
  {3U,1244U,2014U,0U},
  {3U,2014U,1244U,0U},
  {3U,181U,1837U,0U},
  {3U,1837U,181U,0U},
  {3U,341U,3253U,0U},
  {3U,3253U,341U,0U},
  {3U,98U,2032U,0U},
  {3U,2032U,98U,0U},
  {3U,1409U,1817U,0U},
  {3U,1817U,1409U,0U},
  {3U,3540U,3541U,0U},
  {3U,3541U,3540U,0U},
  {3U,4173U,5177U,0U},
  {3U,5177U,4173U,0U},
  {3U,918U,1318U,0U},
  {3U,1318U,918U,0U},
  {3U,3486U,3762U,0U},
  {3U,3762U,3486U,0U},
  {3U,234U,402U,0U},
  {3U,402U,234U,0U},
  {3U,120U,1456U,0U},
  {3U,1456U,120U,0U},
  {3U,1328U,1329U,0U},
  {3U,1329U,1328U,0U},
  {3U,5231U,5330U,0U},
  {3U,5330U,5231U,0U},
  {3U,394U,3512U,0U},
  {3U,3512U,394U,0U},
  {3U,1006U,3277U,0U},
  {3U,3277U,1006U,0U},
  {3U,4458U,4542U,0U},
  {3U,4542U,4458U,0U},
  {3U,465U,489U,0U},
  {3U,489U,465U,0U},
  {3U,1241U,1475U,0U},
  {3U,1475U,1241U,0U},
  {3U,1014U,1853U,0U},
  {3U,1853U,1014U,0U},
  {3U,789U,1790U,0U},
  {3U,1790U,789U,0U},
  {3U,34U,1280U,0U},
  {3U,1280U,34U,0U},
  {3U,3509U,3979U,0U},
  {3U,3979U,3509U,0U},
  {3U,236U,2847U,0U},
  {3U,2847U,236U,0U},
  {3U,632U,2001U,0U},
  {3U,2001U,632U,0U},
  {3U,1762U,3811U,0U},
  {3U,3811U,1762U,0U},
  {3U,1705U,4349U,0U},
  {3U,4349U,1705U,0U},
  {3U,18U,780U,0U},
  {3U,780U,18U,0U},
  {3U,352U,594U,0U},
  {3U,594U,352U,0U},
  {3U,863U,1418U,0U},
  {3U,1418U,863U,0U},
  {3U,4462U,5253U,0U},
  {3U,5253U,4462U,0U},
  {3U,1154U,3844U,0U},
  {3U,3844U,1154U,0U},
  {3U,2792U,4853U,0U},
  {3U,4853U,2792U,0U},
  {3U,1170U,1908U,0U},
  {3U,1908U,1170U,0U},
  {3U,1233U,1284U,0U},
  {3U,1284U,1233U,0U},
  {3U,555U,565U,0U},
  {3U,565U,555U,0U},
  {3U,4546U,4859U,0U},
  {3U,4859U,4546U,0U},
  {3U,2473U,4603U,0U},
  {3U,4603U,2473U,0U},
  {3U,1394U,3452U,0U},
  {3U,3452U,1394U,0U},
  {3U,5242U,5312U,0U},
  {3U,5312U,5242U,0U},
  {3U,2111U,3891U,0U},
  {3U,3891U,2111U,0U},
  {3U,2079U,4677U,0U},
  {3U,4677U,2079U,0U},
  {3U,2208U,2846U,0U},
  {3U,2846U,2208U,0U},
  {3U,343U,2900U,0U},
  {3U,2900U,343U,0U},
  {3U,1662U,4023U,0U},
  {3U,4023U,1662U,0U},
  {3U,1317U,3848U,0U},
  {3U,3848U,1317U,0U},
  {3U,2908U,4501U,0U},
  {3U,4501U,2908U,0U},
  {3U,3247U,3394U,0U},
  {3U,3394U,3247U,0U},
  {3U,1180U,1677U,0U},
  {3U,1677U,1180U,0U},
  {3U,1054U,2426U,0U},
  {3U,2426U,1054U,0U},
  {3U,3692U,5073U,0U},
  {3U,5073U,3692U,0U},
  {3U,2502U,4624U,0U},
  {3U,4624U,2502U,0U},
  {3U,383U,3487U,0U},
  {3U,3487U,383U,0U},
  {3U,707U,3748U,0U},
  {3U,3748U,707U,0U},
  {3U,192U,228U,0U},
  {3U,228U,192U,0U},
  {3U,721U,1292U,0U},
  {3U,1292U,721U,0U},
  {3U,3352U,3765U,0U},
  {3U,3765U,3352U,0U},
  {3U,934U,1525U,0U},
  {3U,1525U,934U,0U},
  {3U,1486U,2641U,0U},
  {3U,2641U,1486U,0U},
  {3U,2725U,3358U,0U},
  {3U,3358U,2725U,0U},
  {3U,995U,3454U,0U},
  {3U,3454U,995U,0U},
  {3U,210U,2832U,0U},
  {3U,2832U,210U,0U},
  {3U,150U,311U,0U},
  {3U,311U,150U,0U},
  {3U,937U,1529U,0U},
  {3U,1529U,937U,0U},
  {3U,2433U,4768U,0U},
  {3U,4768U,2433U,0U},
  {3U,956U,1793U,0U},
  {3U,1793U,956U,0U},
  {3U,1356U,3928U,0U},
  {3U,3928U,1356U,0U},
  {3U,2536U,3779U,0U},
  {3U,3779U,2536U,0U},
  {3U,233U,1129U,0U},
  {3U,1129U,233U,0U},
  {3U,1365U,2415U,0U},
  {3U,2415U,1365U,0U},
  {3U,1556U,2151U,0U},
  {3U,2151U,1556U,0U},
  {3U,458U,3599U,0U},
  {3U,3599U,458U,0U},
  {3U,146U,1776U,0U},
  {3U,1776U,146U,0U},
  {3U,316U,1391U,0U},
  {3U,1391U,316U,0U},
  {3U,439U,1163U,0U},
  {3U,1163U,439U,0U},
  {3U,4322U,4860U,0U},
  {3U,4860U,4322U,0U},
  {3U,3118U,3274U,0U},
  {3U,3274U,3118U,0U},
  {3U,999U,2823U,0U},
  {3U,2823U,999U,0U},
  {3U,2782U,3190U,0U},
  {3U,3190U,2782U,0U},
  {3U,3462U,4096U,0U},
  {3U,4096U,3462U,0U},
  {3U,5343U,5369U,0U},
  {3U,5369U,5343U,0U},
  {3U,4009U,4983U,0U},
  {3U,4983U,4009U,0U},
  {3U,3212U,4013U,0U},
  {3U,4013U,3212U,0U},
  {3U,2613U,3758U,0U},
  {3U,3758U,2613U,0U},
  {3U,3474U,4513U,0U},
  {3U,4513U,3474U,0U},
  {3U,1632U,3410U,0U},
  {3U,3410U,1632U,0U},
  {3U,3750U,4037U,0U},
  {3U,4037U,3750U,0U},
  {3U,327U,1584U,0U},
  {3U,1584U,327U,0U},
  {3U,32U,1219U,0U},
  {3U,1219U,32U,0U},
  {3U,487U,2219U,0U},
  {3U,2219U,487U,0U},
  {3U,932U,1498U,0U},
  {3U,1498U,932U,0U},
  {3U,5258U,5278U,0U},
  {3U,5278U,5258U,0U},
  {3U,835U,2788U,0U},
  {3U,2788U,835U,0U},
  {3U,660U,1873U,0U},
  {3U,1873U,660U,0U},
  {3U,235U,2975U,0U},
  {3U,2975U,235U,0U},
  {3U,1007U,2122U,0U},
  {3U,2122U,1007U,0U},
  {3U,4288U,4289U,0U},
  {3U,4289U,4288U,0U},
  {3U,2335U,3298U,0U},
  {3U,3298U,2335U,0U},
  {3U,192U,1176U,0U},
  {3U,1176U,192U,0U},
  {3U,409U,2362U,0U},
  {3U,2362U,409U,0U},
  {3U,528U,2132U,0U},
  {3U,2132U,528U,0U},
  {3U,382U,1875U,0U},
  {3U,1875U,382U,0U},
  {3U,432U,567U,0U},
  {3U,567U,432U,0U},
  {3U,2568U,2655U,0U},
  {3U,2655U,2568U,0U},
  {3U,4762U,5301U,0U},
  {3U,5301U,4762U,0U},
  {3U,422U,2099U,0U},
  {3U,2099U,422U,0U},
  {3U,2184U,2873U,0U},
  {3U,2873U,2184U,0U},
  {3U,120U,477U,0U},
  {3U,477U,120U,0U},
  {3U,3750U,4054U,0U},
  {3U,4054U,3750U,0U},
  {3U,2307U,4720U,0U},
  {3U,4720U,2307U,0U},
  {3U,2156U,3889U,0U},
  {3U,3889U,2156U,0U},
  {3U,4284U,5206U,0U},
  {3U,5206U,4284U,0U},
  {3U,120U,2312U,0U},
  {3U,2312U,120U,0U},
  {3U,53U,1676U,0U},
  {3U,1676U,53U,0U},
  {3U,2449U,4769U,0U},
  {3U,4769U,2449U,0U},
  {3U,2595U,2596U,0U},
  {3U,2596U,2595U,0U},
  {3U,53U,412U,0U},
  {3U,412U,53U,0U},
  {3U,243U,1730U,0U},
  {3U,1730U,243U,0U},
  {3U,1923U,2083U,0U},
  {3U,2083U,1923U,0U},
  {3U,3642U,3775U,0U},
  {3U,3775U,3642U,0U},
  {3U,170U,199U,0U},
  {3U,199U,170U,0U},
  {3U,714U,3031U,0U},
  {3U,3031U,714U,0U},
  {3U,105U,2248U,0U},
  {3U,2248U,105U,0U},
  {3U,48U,205U,0U},
  {3U,205U,48U,0U},
  {3U,589U,3681U,0U},
  {3U,3681U,589U,0U},
  {3U,1189U,1975U,0U},
  {3U,1975U,1189U,0U},
  {3U,4982U,5146U,0U},
  {3U,5146U,4982U,0U},
  {3U,464U,2342U,0U},
  {3U,2342U,464U,0U},
  {3U,409U,2078U,0U},
  {3U,2078U,409U,0U},
  {3U,3414U,5025U,0U},
  {3U,5025U,3414U,0U},
  {3U,82U,741U,0U},
  {3U,741U,82U,0U},
  {3U,280U,2829U,0U},
  {3U,2829U,280U,0U},
  {3U,53U,1722U,0U},
  {3U,1722U,53U,0U},
  {3U,5065U,5345U,0U},
  {3U,5345U,5065U,0U},
  {3U,2866U,3893U,0U},
  {3U,3893U,2866U,0U},
  {3U,485U,1396U,0U},
  {3U,1396U,485U,0U},
  {3U,3255U,4985U,0U},
  {3U,4985U,3255U,0U},
  {3U,547U,597U,0U},
  {3U,597U,547U,0U},
  {3U,2041U,4654U,0U},
  {3U,4654U,2041U,0U},
  {3U,2477U,3124U,0U},
  {3U,3124U,2477U,0U},
  {3U,1884U,3121U,0U},
  {3U,3121U,1884U,0U},
  {3U,1382U,2882U,0U},
  {3U,2882U,1382U,0U},
  {3U,3018U,3042U,0U},
  {3U,3042U,3018U,0U},
  {3U,1933U,3897U,0U},
  {3U,3897U,1933U,0U},
  {3U,4446U,5046U,0U},
  {3U,5046U,4446U,0U},
  {3U,985U,1012U,0U},
  {3U,1012U,985U,0U},
  {3U,697U,3744U,0U},
  {3U,3744U,697U,0U},
  {3U,1044U,1070U,0U},
  {3U,1070U,1044U,0U},
  {3U,595U,702U,0U},
  {3U,702U,595U,0U},
  {3U,1823U,2376U,0U},
  {3U,2376U,1823U,0U},
  {3U,1534U,1599U,0U},
  {3U,1599U,1534U,0U},
  {3U,2351U,4745U,0U},
  {3U,4745U,2351U,0U},
  {3U,218U,1307U,0U},
  {3U,1307U,218U,0U},
  {3U,428U,532U,0U},
  {3U,532U,428U,0U},
  {3U,2876U,3395U,0U},
  {3U,3395U,2876U,0U},
  {3U,3801U,4648U,0U},
  {3U,4648U,3801U,0U},
  {3U,3219U,3630U,0U},
  {3U,3630U,3219U,0U},
  {3U,2U,396U,0U},
  {3U,396U,2U,0U},
  {3U,13U,535U,0U},
  {3U,535U,13U,0U},
  {3U,519U,3097U,0U},
  {3U,3097U,519U,0U},
  {3U,1707U,1888U,0U},
  {3U,1888U,1707U,0U},
  {3U,557U,990U,0U},
  {3U,990U,557U,0U},
  {3U,3527U,5047U,0U},
  {3U,5047U,3527U,0U},
  {3U,2510U,3114U,0U},
  {3U,3114U,2510U,0U},
  {3U,309U,2146U,0U},
  {3U,2146U,309U,0U},
  {3U,3600U,5063U,0U},
  {3U,5063U,3600U,0U},
  {3U,117U,1860U,0U},
  {3U,1860U,117U,0U},
  {3U,4980U,5285U,0U},
  {3U,5285U,4980U,0U},
  {3U,97U,649U,0U},
  {3U,649U,97U,0U},
  {3U,2604U,4390U,0U},
  {3U,4390U,2604U,0U},
  {3U,3958U,3959U,0U},
  {3U,3959U,3958U,0U},
  {3U,1560U,3206U,0U},
  {3U,3206U,1560U,0U},
  {3U,3780U,3912U,0U},
  {3U,3912U,3780U,0U},
  {3U,2816U,4602U,0U},
  {3U,4602U,2816U,0U},
  {3U,1029U,3737U,0U},
  {3U,3737U,1029U,0U},
  {3U,3188U,4152U,0U},
  {3U,4152U,3188U,0U},
  {3U,834U,3793U,0U},
  {3U,3793U,834U,0U},
  {3U,4105U,5168U,0U},
  {3U,5168U,4105U,0U},
  {3U,1784U,4583U,0U},
  {3U,4583U,1784U,0U},
  {3U,3127U,3133U,0U},
  {3U,3133U,3127U,0U},
  {3U,734U,2207U,0U},
  {3U,2207U,734U,0U},
  {3U,1387U,2479U,0U},
  {3U,2479U,1387U,0U},
  {3U,308U,3287U,0U},
  {3U,3287U,308U,0U},
  {3U,1502U,2441U,0U},
  {3U,2441U,1502U,0U},
  {3U,3716U,3846U,0U},
  {3U,3846U,3716U,0U},
  {3U,4947U,5317U,0U},
  {3U,5317U,4947U,0U},
  {3U,3178U,3919U,0U},
  {3U,3919U,3178U,0U},
  {3U,1573U,4249U,0U},
  {3U,4249U,1573U,0U},
  {3U,1769U,4576U,0U},
  {3U,4576U,1769U,0U},
  {3U,266U,1053U,0U},
  {3U,1053U,266U,0U},
  {3U,4850U,4945U,0U},
  {3U,4945U,4850U,0U},
  {3U,1097U,2462U,0U},
  {3U,2462U,1097U,0U},
  {3U,2211U,2697U,0U},
  {3U,2697U,2211U,0U},
  {3U,1978U,2614U,0U},
  {3U,2614U,1978U,0U},
  {3U,1708U,3264U,0U},
  {3U,3264U,1708U,0U},
  {3U,1288U,3820U,0U},
  {3U,3820U,1288U,0U},
  {3U,801U,2218U,0U},
  {3U,2218U,801U,0U},
  {3U,148U,2552U,0U},
  {3U,2552U,148U,0U},
  {3U,2093U,2618U,0U},
  {3U,2618U,2093U,0U},
  {3U,1689U,4227U,0U},
  {3U,4227U,1689U,0U},
  {3U,53U,1750U,0U},
  {3U,1750U,53U,0U},
  {3U,526U,825U,0U},
  {3U,825U,526U,0U},
  {3U,318U,3165U,0U},
  {3U,3165U,318U,0U},
  {3U,856U,2165U,0U},
  {3U,2165U,856U,0U},
  {3U,987U,2673U,0U},
  {3U,2673U,987U,0U},
  {3U,2238U,2636U,0U},
  {3U,2636U,2238U,0U},
  {3U,3582U,5060U,0U},
  {3U,5060U,3582U,0U},
  {3U,2336U,4473U,0U},
  {3U,4473U,2336U,0U},
  {3U,2575U,4801U,0U},
  {3U,4801U,2575U,0U},
  {3U,671U,1810U,0U},
  {3U,1810U,671U,0U},
  {3U,622U,989U,0U},
  {3U,989U,622U,0U},
  {3U,4070U,5020U,0U},
  {3U,5020U,4070U,0U},
  {3U,5360U,5361U,0U},
  {3U,5361U,5360U,0U},
  {3U,1103U,1211U,0U},
  {3U,1211U,1103U,0U},
  {3U,2114U,4688U,0U},
  {3U,4688U,2114U,0U},
  {3U,4320U,4321U,0U},
  {3U,4321U,4320U,0U},
  {3U,1U,201U,0U},
  {3U,201U,1U,0U},
  {3U,104U,1139U,0U},
  {3U,1139U,104U,0U},
  {3U,4219U,4267U,0U},
  {3U,4267U,4219U,0U},
  {3U,442U,2107U,0U},
  {3U,2107U,442U,0U},
  {3U,53U,1656U,0U},
  {3U,1656U,53U,0U},
  {3U,58U,1075U,0U},
  {3U,1075U,58U,0U},
  {3U,2437U,4678U,0U},
  {3U,4678U,2437U,0U},
  {3U,2337U,3678U,0U},
  {3U,3678U,2337U,0U},
  {3U,9U,673U,0U},
  {3U,673U,9U,0U},
  {3U,2654U,2691U,0U},
  {3U,2691U,2654U,0U},
  {3U,1060U,1203U,0U},
  {3U,1203U,1060U,0U},
  {3U,71U,773U,0U},
  {3U,773U,71U,0U},
  {3U,1766U,1785U,0U},
  {3U,1785U,1766U,0U},
  {3U,4317U,4838U,0U},
  {3U,4838U,4317U,0U},
  {3U,3053U,4187U,0U},
  {3U,4187U,3053U,0U},
  {3U,4387U,4426U,0U},
  {3U,4426U,4387U,0U},
  {3U,5134U,5144U,0U},
  {3U,5144U,5134U,0U},
  {3U,213U,1345U,0U},
  {3U,1345U,213U,0U},
  {3U,5264U,5344U,0U},
  {3U,5344U,5264U,0U},
  {3U,2096U,4684U,0U},
  {3U,4684U,2096U,0U},
  {3U,1364U,4086U,0U},
  {3U,4086U,1364U,0U},
  {3U,4868U,4953U,0U},
  {3U,4953U,4868U,0U},
  {3U,3984U,4430U,0U},
  {3U,4430U,3984U,0U},
  {3U,3753U,3924U,0U},
  {3U,3924U,3753U,0U},
  {3U,2359U,4093U,0U},
  {3U,4093U,2359U,0U},
  {3U,4933U,5331U,0U},
  {3U,5331U,4933U,0U},
  {3U,264U,421U,0U},
  {3U,421U,264U,0U},
  {3U,722U,758U,0U},
  {3U,758U,722U,0U},
  {3U,321U,3247U,0U},
  {3U,3247U,321U,0U},
  {3U,873U,3491U,0U},
  {3U,3491U,873U,0U},
  {3U,0U,36U,0U},
  {3U,36U,0U,0U},
  {3U,901U,1574U,0U},
  {3U,1574U,901U,0U},
  {3U,500U,3288U,0U},
  {3U,3288U,500U,0U},
  {3U,282U,451U,0U},
  {3U,451U,282U,0U},
  {3U,4011U,5148U,0U},
  {3U,5148U,4011U,0U},
  {3U,1509U,1510U,0U},
  {3U,1510U,1509U,0U},
  {3U,1193U,3140U,0U},
  {3U,3140U,1193U,0U},
  {3U,4612U,5080U,0U},
  {3U,5080U,4612U,0U},
  {3U,1157U,2018U,0U},
  {3U,2018U,1157U,0U},
  {3U,1341U,2046U,0U},
  {3U,2046U,1341U,0U},
  {3U,968U,1098U,0U},
  {3U,1098U,968U,0U},
  {3U,4384U,4385U,0U},
  {3U,4385U,4384U,0U},
  {3U,2745U,2932U,0U},
  {3U,2932U,2745U,0U},
  {3U,2903U,3360U,0U},
  {3U,3360U,2903U,0U},
  {3U,288U,3174U,0U},
  {3U,3174U,288U,0U},
  {3U,192U,2127U,0U},
  {3U,2127U,192U,0U},
  {3U,2026U,2972U,0U},
  {3U,2972U,2026U,0U},
  {3U,45U,951U,0U},
  {3U,951U,45U,0U},
  {3U,3813U,4206U,0U},
  {3U,4206U,3813U,0U},
  {3U,2172U,3263U,0U},
  {3U,3263U,2172U,0U},
  {3U,2941U,2970U,0U},
  {3U,2970U,2941U,0U},
  {3U,944U,2319U,0U},
  {3U,2319U,944U,0U},
  {3U,2600U,3335U,0U},
  {3U,3335U,2600U,0U},
  {3U,5170U,5226U,0U},
  {3U,5226U,5170U,0U},
  {3U,1709U,4510U,0U},
  {3U,4510U,1709U,0U},
  {3U,762U,2035U,0U},
  {3U,2035U,762U,0U},
  {3U,4432U,4434U,0U},
  {3U,4434U,4432U,0U},
  {3U,3948U,5131U,0U},
  {3U,5131U,3948U,0U},
  {3U,3822U,5076U,0U},
  {3U,5076U,3822U,0U},
  {3U,213U,1101U,0U},
  {3U,1101U,213U,0U},
  {3U,2461U,3311U,0U},
  {3U,3311U,2461U,0U},
  {3U,4948U,5332U,0U},
  {3U,5332U,4948U,0U},
  {3U,98U,1415U,0U},
  {3U,1415U,98U,0U},
  {3U,2206U,4702U,0U},
  {3U,4702U,2206U,0U},
  {3U,5219U,5225U,0U},
  {3U,5225U,5219U,0U},
  {3U,1727U,2978U,0U},
  {3U,2978U,1727U,0U},
  {3U,3368U,5010U,0U},
  {3U,5010U,3368U,0U},
  {3U,140U,2444U,0U},
  {3U,2444U,140U,0U},
  {3U,3601U,3602U,0U},
  {3U,3602U,3601U,0U},
  {3U,1435U,1441U,0U},
  {3U,1441U,1435U,0U},
  {3U,1634U,4347U,0U},
  {3U,4347U,1634U,0U},
  {3U,323U,3214U,0U},
  {3U,3214U,323U,0U},
  {3U,722U,2821U,0U},
  {3U,2821U,722U,0U},
  {3U,382U,1164U,0U},
  {3U,1164U,382U,0U},
  {3U,1430U,1431U,0U},
  {3U,1431U,1430U,0U},
  {3U,1257U,1331U,0U},
  {3U,1331U,1257U,0U},
  {3U,382U,608U,0U},
  {3U,608U,382U,0U},
  {3U,3558U,4144U,0U},
  {3U,4144U,3558U,0U},
  {3U,337U,539U,0U},
  {3U,539U,337U,0U},
  {3U,1536U,4169U,0U},
  {3U,4169U,1536U,0U},
  {3U,496U,1440U,0U},
  {3U,1440U,496U,0U},
  {3U,510U,1449U,0U},
  {3U,1449U,510U,0U},
  {3U,153U,2588U,0U},
  {3U,2588U,153U,0U},
  {3U,631U,636U,0U},
  {3U,636U,631U,0U},
  {3U,2141U,3594U,0U},
  {3U,3594U,2141U,0U},
  {3U,20U,1092U,0U},
  {3U,1092U,20U,0U},
  {3U,230U,733U,0U},
  {3U,733U,230U,0U},
  {3U,3840U,5110U,0U},
  {3U,5110U,3840U,0U},
  {3U,43U,83U,0U},
  {3U,83U,43U,0U},
  {3U,2980U,4915U,0U},
  {3U,4915U,2980U,0U},
  {3U,797U,2171U,0U},
  {3U,2171U,797U,0U},
  {3U,4737U,5296U,0U},
  {3U,5296U,4737U,0U},
  {3U,4124U,4531U,0U},
  {3U,4531U,4124U,0U},
  {3U,3519U,4001U,0U},
  {3U,4001U,3519U,0U},
  {3U,2558U,3854U,0U},
  {3U,3854U,2558U,0U},
  {3U,4006U,4052U,0U},
  {3U,4052U,4006U,0U},
  {3U,2667U,4443U,0U},
  {3U,4443U,2667U,0U},
  {3U,399U,2875U,0U},
  {3U,2875U,399U,0U},
  {3U,1137U,3898U,0U},
  {3U,3898U,1137U,0U},
  {3U,3500U,3888U,0U},
  {3U,3888U,3500U,0U},
  {3U,2403U,3054U,0U},
  {3U,3054U,2403U,0U},
  {3U,722U,731U,0U},
  {3U,731U,722U,0U},
  {3U,376U,892U,0U},
  {3U,892U,376U,0U},
  {3U,4351U,4901U,0U},
  {3U,4901U,4351U,0U},
  {3U,502U,3635U,0U},
  {3U,3635U,502U,0U},
  {3U,4755U,5299U,0U},
  {3U,5299U,4755U,0U},
  {3U,44U,641U,0U},
  {3U,641U,44U,0U},
  {3U,2893U,2935U,0U},
  {3U,2935U,2893U,0U},
  {3U,979U,1185U,0U},
  {3U,1185U,979U,0U},
  {3U,3469U,4451U,0U},
  {3U,4451U,3469U,0U},
  {3U,339U,698U,0U},
  {3U,698U,339U,0U},
  {3U,366U,1141U,0U},
  {3U,1141U,366U,0U},
  {3U,4411U,4556U,0U},
  {3U,4556U,4411U,0U},
  {3U,3064U,3078U,0U},
  {3U,3078U,3064U,0U},
  {3U,1250U,3261U,0U},
  {3U,3261U,1250U,0U},
  {3U,1840U,3598U,0U},
  {3U,3598U,1840U,0U},
  {3U,666U,3466U,0U},
  {3U,3466U,666U,0U},
  {3U,3409U,5023U,0U},
  {3U,5023U,3409U,0U},
  {3U,1495U,2294U,0U},
  {3U,2294U,1495U,0U},
  {3U,324U,2621U,0U},
  {3U,2621U,324U,0U},
  {3U,1032U,2006U,0U},
  {3U,2006U,1032U,0U},
  {3U,3417U,3562U,0U},
  {3U,3562U,3417U,0U},
  {3U,535U,804U,0U},
  {3U,804U,535U,0U},
  {3U,4633U,4955U,0U},
  {3U,4955U,4633U,0U},
  {3U,3388U,4274U,0U},
  {3U,4274U,3388U,0U},
  {3U,1733U,4540U,0U},
  {3U,4540U,1733U,0U},
  {3U,2660U,2665U,0U},
  {3U,2665U,2660U,0U},
  {3U,2881U,4416U,0U},
  {3U,4416U,2881U,0U},
  {3U,514U,600U,0U},
  {3U,600U,514U,0U},
  {3U,3342U,4018U,0U},
  {3U,4018U,3342U,0U},
  {3U,729U,3707U,0U},
  {3U,3707U,729U,0U},
  {3U,1270U,1279U,0U},
  {3U,1279U,1270U,0U},
  {3U,5139U,5239U,0U},
  {3U,5239U,5139U,0U},
  {3U,42U,247U,0U},
  {3U,247U,42U,0U},
  {3U,516U,2091U,0U},
  {3U,2091U,516U,0U},
  {3U,2943U,4907U,0U},
  {3U,4907U,2943U,0U},
  {3U,2559U,4182U,0U},
  {3U,4182U,2559U,0U},
  {3U,1539U,3060U,0U},
  {3U,3060U,1539U,0U},
  {3U,1872U,3706U,0U},
  {3U,3706U,1872U,0U},
  {3U,3883U,5116U,0U},
  {3U,5116U,3883U,0U},
  {3U,1035U,3900U,0U},
  {3U,3900U,1035U,0U},
  {3U,53U,349U,0U},
  {3U,349U,53U,0U},
  {3U,4727U,4728U,0U},
  {3U,4728U,4727U,0U},
  {3U,786U,1141U,0U},
  {3U,1141U,786U,0U},
  {3U,1994U,3510U,0U},
  {3U,3510U,1994U,0U},
  {3U,4499U,5008U,0U},
  {3U,5008U,4499U,0U},
  {3U,1037U,1145U,0U},
  {3U,1145U,1037U,0U},
  {3U,2160U,3783U,0U},
  {3U,3783U,2160U,0U},
  {3U,1668U,1679U,0U},
  {3U,1679U,1668U,0U},
  {3U,4455U,5250U,0U},
  {3U,5250U,4455U,0U},
  {3U,2205U,4701U,0U},
  {3U,4701U,2205U,0U},
  {3U,4529U,4788U,0U},
  {3U,4788U,4529U,0U},
  {3U,102U,1145U,0U},
  {3U,1145U,102U,0U},
  {3U,76U,700U,0U},
  {3U,700U,76U,0U},
  {3U,1190U,1481U,0U},
  {3U,1481U,1190U,0U},
  {3U,3515U,4671U,0U},
  {3U,4671U,3515U,0U},
  {3U,2763U,4848U,0U},
  {3U,4848U,2763U,0U},
  {3U,162U,2300U,0U},
  {3U,2300U,162U,0U},
  {3U,2506U,2889U,0U},
  {3U,2889U,2506U,0U},
  {3U,74U,1886U,0U},
  {3U,1886U,74U,0U},
  {3U,100U,543U,0U},
  {3U,543U,100U,0U},
  {3U,4060U,4629U,0U},
  {3U,4629U,4060U,0U},
  {3U,347U,732U,0U},
  {3U,732U,347U,0U},
  {3U,71U,3683U,0U},
  {3U,3683U,71U,0U},
  {3U,2523U,3066U,0U},
  {3U,3066U,2523U,0U},
  {3U,4332U,4699U,0U},
  {3U,4699U,4332U,0U},
  {3U,1582U,4269U,0U},
  {3U,4269U,1582U,0U},
  {3U,4883U,5273U,0U},
  {3U,5273U,4883U,0U},
  {3U,685U,689U,0U},
  {3U,689U,685U,0U},
  {3U,4739U,4740U,0U},
  {3U,4740U,4739U,0U},
  {3U,2814U,4863U,0U},
  {3U,4863U,2814U,0U},
  {3U,220U,642U,0U},
  {3U,642U,220U,0U},
  {3U,4525U,5260U,0U},
  {3U,5260U,4525U,0U},
  {3U,873U,1928U,0U},
  {3U,1928U,873U,0U},
  {3U,2844U,2870U,0U},
  {3U,2870U,2844U,0U},
  {3U,541U,2424U,0U},
  {3U,2424U,541U,0U},
  {3U,756U,769U,0U},
  {3U,769U,756U,0U},
  {3U,2724U,4826U,0U},
  {3U,4826U,2724U,0U},
  {3U,4567U,4750U,0U},
  {3U,4750U,4567U,0U},
  {3U,380U,3169U,0U},
  {3U,3169U,380U,0U},
  {3U,3397U,4043U,0U},
  {3U,4043U,3397U,0U},
  {3U,1738U,3183U,0U},
  {3U,3183U,1738U,0U},
  {3U,2927U,3077U,0U},
  {3U,3077U,2927U,0U},
  {3U,4490U,4491U,0U},
  {3U,4491U,4490U,0U},
  {3U,4341U,4342U,0U},
  {3U,4342U,4341U,0U},
  {3U,1904U,1972U,0U},
  {3U,1972U,1904U,0U},
  {3U,4391U,4819U,0U},
  {3U,4819U,4391U,0U},
  {3U,3588U,4362U,0U},
  {3U,4362U,3588U,0U},
  {3U,104U,2243U,0U},
  {3U,2243U,104U,0U},
  {3U,434U,3567U,0U},
  {3U,3567U,434U,0U},
  {3U,5270U,5315U,0U},
  {3U,5315U,5270U,0U},
  {3U,3086U,3166U,0U},
  {3U,3166U,3086U,0U},
  {3U,4906U,5001U,0U},
  {3U,5001U,4906U,0U},
  {3U,22U,1105U,0U},
  {3U,1105U,22U,0U},
  {3U,287U,2804U,0U},
  {3U,2804U,287U,0U},
  {3U,824U,826U,0U},
  {3U,826U,824U,0U},
  {3U,3721U,4565U,0U},
  {3U,4565U,3721U,0U},
  {3U,2446U,2711U,0U},
  {3U,2711U,2446U,0U},
  {3U,316U,1406U,0U},
  {3U,1406U,316U,0U},
  {3U,2822U,4865U,0U},
  {3U,4865U,2822U,0U},
  {3U,2569U,2655U,0U},
  {3U,2655U,2569U,0U},
  {3U,98U,1445U,0U},
  {3U,1445U,98U,0U},
  {3U,118U,961U,0U},
  {3U,961U,118U,0U},
  {3U,4162U,5174U,0U},
  {3U,5174U,4162U,0U},
  {3U,822U,2699U,0U},
  {3U,2699U,822U,0U},
  {3U,1542U,3059U,0U},
  {3U,3059U,1542U,0U},
  {3U,1869U,2471U,0U},
  {3U,2471U,1869U,0U},
  {3U,1160U,3778U,0U},
  {3U,3778U,1160U,0U},
  {3U,666U,3366U,0U},
  {3U,3366U,666U,0U},
  {3U,841U,3799U,0U},
  {3U,3799U,841U,0U},
  {3U,1315U,3798U,0U},
  {3U,3798U,1315U,0U},
  {3U,440U,1079U,0U},
  {3U,1079U,440U,0U},
  {3U,494U,2221U,0U},
  {3U,2221U,494U,0U},
  {3U,1728U,1805U,0U},
  {3U,1805U,1728U,0U},
  {3U,484U,1397U,0U},
  {3U,1397U,484U,0U},
  {3U,4637U,5284U,0U},
  {3U,5284U,4637U,0U},
  {3U,5308U,5365U,0U},
  {3U,5365U,5308U,0U},
  {3U,2985U,4920U,0U},
  {3U,4920U,2985U,0U},
  {3U,53U,1763U,0U},
  {3U,1763U,53U,0U},
  {3U,688U,3743U,0U},
  {3U,3743U,688U,0U},
  {3U,3443U,4159U,0U},
  {3U,4159U,3443U,0U},
  {3U,2996U,4921U,0U},
  {3U,4921U,2996U,0U},
  {3U,4379U,4837U,0U},
  {3U,4837U,4379U,0U},
  {3U,313U,516U,0U},
  {3U,516U,313U,0U},
  {3U,521U,3653U,0U},
  {3U,3653U,521U,0U},
  {3U,488U,1910U,0U},
  {3U,1910U,488U,0U},
  {3U,790U,798U,0U},
  {3U,798U,790U,0U},
  {3U,977U,2120U,0U},
  {3U,2120U,977U,0U},
  {3U,3443U,5029U,0U},
  {3U,5029U,3443U,0U},
  {3U,71U,3843U,0U},
  {3U,3843U,71U,0U},
  {3U,71U,3420U,0U},
  {3U,3420U,71U,0U},
  {3U,2448U,2715U,0U},
  {3U,2715U,2448U,0U},
  {3U,590U,2498U,0U},
  {3U,2498U,590U,0U},
  {3U,4925U,5140U,0U},
  {3U,5140U,4925U,0U},
  {3U,3242U,4977U,0U},
  {3U,4977U,3242U,0U},
  {3U,4228U,4785U,0U},
  {3U,4785U,4228U,0U},
  {3U,2246U,2247U,0U},
  {3U,2247U,2246U,0U},
  {3U,1348U,3640U,0U},
  {3U,3640U,1348U,0U},
  {3U,140U,310U,0U},
  {3U,310U,140U,0U},
  {3U,4272U,5203U,0U},
  {3U,5203U,4272U,0U},
  {3U,1232U,3318U,0U},
  {3U,3318U,1232U,0U},
  {3U,3346U,5000U,0U},
  {3U,5000U,3346U,0U},
  {3U,626U,2646U,0U},
  {3U,2646U,626U,0U},
  {3U,982U,2256U,0U},
  {3U,2256U,982U,0U},
  {3U,71U,4592U,0U},
  {3U,4592U,71U,0U},
  {3U,4260U,5199U,0U},
  {3U,5199U,4260U,0U},
  {3U,2328U,2713U,0U},
  {3U,2713U,2328U,0U},
  {3U,1428U,1429U,0U},
  {3U,1429U,1428U,0U},
  {3U,4030U,4453U,0U},
  {3U,4453U,4030U,0U},
  {3U,1260U,1281U,0U},
  {3U,1281U,1260U,0U},
  {3U,113U,1831U,0U},
  {3U,1831U,113U,0U},
  {3U,143U,190U,0U},
  {3U,190U,143U,0U},
  {3U,69U,495U,0U},
  {3U,495U,69U,0U},
  {3U,291U,1313U,0U},
  {3U,1313U,291U,0U},
  {3U,407U,3350U,0U},
  {3U,3350U,407U,0U},
  {3U,2010U,2841U,0U},
  {3U,2841U,2010U,0U},
  {3U,2159U,2974U,0U},
  {3U,2974U,2159U,0U},
  {3U,10U,690U,0U},
  {3U,690U,10U,0U},
  {3U,413U,580U,0U},
  {3U,580U,413U,0U},
  {3U,3998U,4036U,0U},
  {3U,4036U,3998U,0U},
  {3U,2123U,4690U,0U},
  {3U,4690U,2123U,0U},
  {3U,2240U,4710U,0U},
  {3U,4710U,2240U,0U},
  {3U,2274U,2775U,0U},
  {3U,2775U,2274U,0U},
  {3U,2499U,2744U,0U},
  {3U,2744U,2499U,0U},
  {3U,17U,561U,0U},
  {3U,561U,17U,0U},
  {3U,449U,1369U,0U},
  {3U,1369U,449U,0U},
  {3U,912U,2869U,0U},
  {3U,2869U,912U,0U},
  {3U,5018U,5341U,0U},
  {3U,5341U,5018U,0U},
  {3U,3407U,5022U,0U},
  {3U,5022U,3407U,0U},
  {3U,437U,2037U,0U},
  {3U,2037U,437U,0U},
  {3U,2997U,4693U,0U},
  {3U,4693U,2997U,0U},
  {3U,466U,1095U,0U},
  {3U,1095U,466U,0U},
  {3U,112U,2268U,0U},
  {3U,2268U,112U,0U},
  {3U,4894U,4895U,0U},
  {3U,4895U,4894U,0U},
  {3U,393U,904U,0U},
  {3U,904U,393U,0U},
  {3U,3088U,4941U,0U},
  {3U,4941U,3088U,0U},
  {3U,925U,3837U,0U},
  {3U,3837U,925U,0U},
  {3U,269U,1621U,0U},
  {3U,1621U,269U,0U},
  {3U,781U,1046U,0U},
  {3U,1046U,781U,0U},
  {3U,674U,1859U,0U},
  {3U,1859U,674U,0U},
  {3U,2047U,4658U,0U},
  {3U,4658U,2047U,0U},
  {3U,1557U,4202U,0U},
  {3U,4202U,1557U,0U},
  {3U,336U,3385U,0U},
  {3U,3385U,336U,0U},
  {3U,3220U,4364U,0U},
  {3U,4364U,3220U,0U},
  {3U,17U,983U,0U},
  {3U,983U,17U,0U},
  {3U,681U,1051U,0U},
  {3U,1051U,681U,0U},
  {3U,264U,855U,0U},
  {3U,855U,264U,0U},
  {3U,2578U,2845U,0U},
  {3U,2845U,2578U,0U},
  {3U,3455U,5033U,0U},
  {3U,5033U,3455U,0U},
  {3U,98U,2025U,0U},
  {3U,2025U,98U,0U},
  {3U,2732U,2740U,0U},
  {3U,2740U,2732U,0U},
  {3U,266U,1613U,0U},
  {3U,1613U,266U,0U},
  {3U,2723U,4839U,0U},
  {3U,4839U,2723U,0U},
  {3U,382U,653U,0U},
  {3U,653U,382U,0U},
  {3U,5171U,5335U,0U},
  {3U,5335U,5171U,0U},
  {3U,1096U,1735U,0U},
  {3U,1735U,1096U,0U},
  {3U,345U,3408U,0U},
  {3U,3408U,345U,0U},
  {3U,682U,1192U,0U},
  {3U,1192U,682U,0U},
  {3U,420U,3240U,0U},
  {3U,3240U,420U,0U},
  {3U,3504U,4033U,0U},
  {3U,4033U,3504U,0U},
  {3U,2538U,4120U,0U},
  {3U,4120U,2538U,0U},
  {3U,4397U,4398U,0U},
  {3U,4398U,4397U,0U},
  {3U,2619U,2623U,0U},
  {3U,2623U,2619U,0U},
  {3U,4936U,4937U,0U},
  {3U,4937U,4936U,0U},
  {3U,2556U,3075U,0U},
  {3U,3075U,2556U,0U},
  {3U,71U,4040U,0U},
  {3U,4040U,71U,0U},
  {3U,2357U,4749U,0U},
  {3U,4749U,2357U,0U},
  {3U,3107U,3861U,0U},
  {3U,3861U,3107U,0U},
  {3U,854U,978U,0U},
  {3U,978U,854U,0U},
  {3U,1523U,1528U,0U},
  {3U,1528U,1523U,0U},
  {3U,1504U,1530U,0U},
  {3U,1530U,1504U,0U},
  {3U,2151U,2484U,0U},
  {3U,2484U,2151U,0U},
  {3U,1258U,1327U,0U},
  {3U,1327U,1258U,0U},
  {3U,2310U,3971U,0U},
  {3U,3971U,2310U,0U},
  {3U,4123U,5101U,0U},
  {3U,5101U,4123U,0U},
  {3U,1135U,1900U,0U},
  {3U,1900U,1135U,0U},
  {3U,4247U,5356U,0U},
  {3U,5356U,4247U,0U},
  {3U,1870U,1887U,0U},
  {3U,1887U,1870U,0U},
  {3U,1572U,4245U,0U},
  {3U,4245U,1572U,0U},
  {3U,360U,3432U,0U},
  {3U,3432U,360U,0U},
  {3U,3701U,4942U,0U},
  {3U,4942U,3701U,0U},
  {3U,1749U,4550U,0U},
  {3U,4550U,1749U,0U},
  {3U,736U,2262U,0U},
  {3U,2262U,736U,0U},
  {3U,3396U,5019U,0U},
  {3U,5019U,3396U,0U},
  {3U,1330U,4058U,0U},
  {3U,4058U,1330U,0U},
  {3U,194U,2777U,0U},
  {3U,2777U,194U,0U},
  {3U,2235U,4107U,0U},
  {3U,4107U,2235U,0U},
  {3U,267U,2397U,0U},
  {3U,2397U,267U,0U},
  {3U,4305U,5133U,0U},
  {3U,5133U,4305U,0U},
  {3U,800U,3722U,0U},
  {3U,3722U,800U,0U},
  {3U,400U,1371U,0U},
  {3U,1371U,400U,0U},
  {3U,2229U,4102U,0U},
  {3U,4102U,2229U,0U},
  {3U,996U,1439U,0U},
  {3U,1439U,996U,0U},
  {3U,2990U,4610U,0U},
  {3U,4610U,2990U,0U},
  {3U,425U,455U,0U},
  {3U,455U,425U,0U},
  {3U,1442U,1638U,0U},
  {3U,1638U,1442U,0U},
  {3U,2580U,3389U,0U},
  {3U,3389U,2580U,0U},
  {3U,2738U,4373U,0U},
  {3U,4373U,2738U,0U},
  {3U,966U,1174U,0U},
  {3U,1174U,966U,0U},
  {3U,456U,2463U,0U},
  {3U,2463U,456U,0U},
  {3U,3103U,4669U,0U},
  {3U,4669U,3103U,0U},
  {3U,2073U,4401U,0U},
  {3U,4401U,2073U,0U},
  {3U,4354U,4486U,0U},
  {3U,4486U,4354U,0U},
  {3U,917U,1798U,0U},
  {3U,1798U,917U,0U},
  {3U,2871U,4125U,0U},
  {3U,4125U,2871U,0U},
  {3U,646U,2302U,0U},
  {3U,2302U,646U,0U},
  {3U,1861U,3825U,0U},
  {3U,3825U,1861U,0U},
  {3U,263U,2151U,0U},
  {3U,2151U,263U,0U},
  {3U,3265U,3268U,0U},
  {3U,3268U,3265U,0U},
  {3U,3974U,5132U,0U},
  {3U,5132U,3974U,0U},
  {3U,4266U,4311U,0U},
  {3U,4311U,4266U,0U},
  {3U,423U,3372U,0U},
  {3U,3372U,423U,0U},
  {3U,143U,2472U,0U},
  {3U,2472U,143U,0U},
  {3U,237U,1096U,0U},
  {3U,1096U,237U,0U},
  {3U,299U,2418U,0U},
  {3U,2418U,299U,0U},
  {3U,2474U,3036U,0U},
  {3U,3036U,2474U,0U},
  {3U,4703U,5064U,0U},
  {3U,5064U,4703U,0U},
  {3U,3935U,4733U,0U},
  {3U,4733U,3935U,0U},
  {3U,1612U,3451U,0U},
  {3U,3451U,1612U,0U},
  {3U,189U,2189U,0U},
  {3U,2189U,189U,0U},
  {3U,3986U,4412U,0U},
  {3U,4412U,3986U,0U},
  {3U,483U,866U,0U},
  {3U,866U,483U,0U},
  {3U,2231U,4706U,0U},
  {3U,4706U,2231U,0U},
  {3U,4569U,4570U,0U},
  {3U,4570U,4569U,0U},
  {3U,1325U,4042U,0U},
  {3U,4042U,1325U,0U},
  {3U,330U,3377U,0U},
  {3U,3377U,330U,0U},
  {3U,2599U,4810U,0U},
  {3U,4810U,2599U,0U},
  {3U,1124U,1815U,0U},
  {3U,1815U,1124U,0U},
  {3U,404U,1414U,0U},
  {3U,1414U,404U,0U},
  {3U,1245U,3978U,0U},
  {3U,3978U,1245U,0U},
  {3U,1322U,3968U,0U},
  {3U,3968U,1322U,0U},
  {3U,1262U,1977U,0U},
  {3U,1977U,1262U,0U},
  {3U,2152U,3483U,0U},
  {3U,3483U,2152U,0U},
  {3U,753U,761U,0U},
  {3U,761U,753U,0U},
  {3U,1024U,3890U,0U},
  {3U,3890U,1024U,0U},
  {3U,169U,1082U,0U},
  {3U,1082U,169U,0U},
  {3U,101U,358U,0U},
  {3U,358U,101U,0U},
  {3U,1354U,2282U,0U},
  {3U,2282U,1354U,0U},
  {3U,4734U,4735U,0U},
  {3U,4735U,4734U,0U},
  {3U,634U,645U,0U},
  {3U,645U,634U,0U},
  {3U,338U,1760U,0U},
  {3U,1760U,338U,0U},
  {3U,2928U,4719U,0U},
  {3U,4719U,2928U,0U},
  {3U,1205U,1207U,0U},
  {3U,1207U,1205U,0U},
  {3U,2949U,3034U,0U},
  {3U,3034U,2949U,0U},
  {3U,1309U,2917U,0U},
  {3U,2917U,1309U,0U},
  {3U,2759U,3252U,0U},
  {3U,3252U,2759U,0U},
  {3U,3936U,5125U,0U},
  {3U,5125U,3936U,0U},
  {3U,3559U,5045U,0U},
  {3U,5045U,3559U,0U},
  {3U,625U,1927U,0U},
  {3U,1927U,625U,0U},
  {3U,2785U,3189U,0U},
  {3U,3189U,2785U,0U},
  {3U,341U,3248U,0U},
  {3U,3248U,341U,0U},
  {3U,1367U,1818U,0U},
  {3U,1818U,1367U,0U},
  {3U,524U,593U,0U},
  {3U,593U,524U,0U},
  {3U,472U,2404U,0U},
  {3U,2404U,472U,0U},
  {3U,2209U,2228U,0U},
  {3U,2228U,2209U,0U},
  {3U,3116U,4731U,0U},
  {3U,4731U,3116U,0U},
  {3U,2572U,3282U,0U},
  {3U,3282U,2572U,0U},
  {3U,1014U,1099U,0U},
  {3U,1099U,1014U,0U},
  {3U,85U,1249U,0U},
  {3U,1249U,85U,0U},
  {3U,1968U,2331U,0U},
  {3U,2331U,1968U,0U},
  {3U,3296U,4115U,0U},
  {3U,4115U,3296U,0U},
  {3U,4110U,4796U,0U},
  {3U,4796U,4110U,0U},
  {3U,4205U,4313U,0U},
  {3U,4313U,4205U,0U},
  {3U,3083U,3622U,0U},
  {3U,3622U,3083U,0U},
  {3U,4074U,5044U,0U},
  {3U,5044U,4074U,0U},
  {3U,4479U,5119U,0U},
  {3U,5119U,4479U,0U},
  {3U,587U,3686U,0U},
  {3U,3686U,587U,0U},
  {3U,797U,1384U,0U},
  {3U,1384U,797U,0U},
  {3U,344U,2055U,0U},
  {3U,2055U,344U,0U},
  {3U,4132U,4331U,0U},
  {3U,4331U,4132U,0U},
  {3U,553U,554U,0U},
  {3U,554U,553U,0U},
  {3U,2263U,3320U,0U},
  {3U,3320U,2263U,0U},
  {3U,2509U,2597U,0U},
  {3U,2597U,2509U,0U},
  {3U,2445U,3612U,0U},
  {3U,3612U,2445U,0U},
  {3U,4043U,5158U,0U},
  {3U,5158U,4043U,0U},
  {3U,3387U,3388U,0U},
  {3U,3388U,3387U,0U},
  {3U,5041U,5180U,0U},
  {3U,5180U,5041U,0U},
  {3U,4299U,4419U,0U},
  {3U,4419U,4299U,0U},
  {3U,712U,3314U,0U},
  {3U,3314U,712U,0U},
  {3U,26U,915U,0U},
  {3U,915U,26U,0U},
  {3U,3009U,3932U,0U},
  {3U,3932U,3009U,0U},
  {3U,582U,879U,0U},
  {3U,879U,582U,0U},
  {3U,5130U,5279U,0U},
  {3U,5279U,5130U,0U},
  {3U,2831U,3909U,0U},
  {3U,3909U,2831U,0U},
  {3U,2729U,2731U,0U},
  {3U,2731U,2729U,0U},
  {3U,2753U,4605U,0U},
  {3U,4605U,2753U,0U},
  {3U,343U,2802U,0U},
  {3U,2802U,343U,0U},
  {3U,436U,3356U,0U},
  {3U,3356U,436U,0U},
  {3U,3529U,3901U,0U},
  {3U,3901U,3529U,0U},
  {3U,4223U,4548U,0U},
  {3U,4548U,4223U,0U},
  {3U,922U,3763U,0U},
  {3U,3763U,922U,0U},
  {3U,3373U,3873U,0U},
  {3U,3873U,3373U,0U},
  {3U,2041U,2059U,0U},
  {3U,2059U,2041U,0U},
  {3U,4949U,4950U,0U},
  {3U,4950U,4949U,0U},
  {3U,5004U,5338U,0U},
  {3U,5338U,5004U,0U},
  {3U,53U,1702U,0U},
  {3U,1702U,53U,0U},
  {3U,4209U,4997U,0U},
  {3U,4997U,4209U,0U},
  {3U,2496U,4754U,0U},
  {3U,4754U,2496U,0U},
  {3U,4681U,5074U,0U},
  {3U,5074U,4681U,0U},
  {3U,3128U,3464U,0U},
  {3U,3464U,3128U,0U},
  {3U,3148U,3238U,0U},
  {3U,3238U,3148U,0U},
  {3U,1768U,2653U,0U},
  {3U,2653U,1768U,0U},
  {3U,4946U,5049U,0U},
  {3U,5049U,4946U,0U},
  {3U,138U,2434U,0U},
  {3U,2434U,138U,0U},
  {3U,1944U,3113U,0U},
  {3U,3113U,1944U,0U},
  {3U,1775U,4577U,0U},
  {3U,4577U,1775U,0U},
  {3U,3581U,3917U,0U},
  {3U,3917U,3581U,0U},
  {3U,1363U,2080U,0U},
  {3U,2080U,1363U,0U},
  {3U,98U,2103U,0U},
  {3U,2103U,98U,0U},
  {3U,2716U,3016U,0U},
  {3U,3016U,2716U,0U},
  {3U,4784U,5303U,0U},
  {3U,5303U,4784U,0U},
  {3U,3076U,4167U,0U},
  {3U,4167U,3076U,0U},
  {3U,5141U,5217U,0U},
  {3U,5217U,5141U,0U},
  {3U,3902U,5081U,0U},
  {3U,5081U,3902U,0U},
  {3U,2551U,3045U,0U},
  {3U,3045U,2551U,0U},
  {3U,1036U,3791U,0U},
  {3U,3791U,1036U,0U},
  {3U,19U,1087U,0U},
  {3U,1087U,19U,0U},
  {3U,3U,452U,0U},
  {3U,452U,3U,0U},
  {3U,90U,602U,0U},
  {3U,602U,90U,0U},
  {3U,4306U,4376U,0U},
  {3U,4376U,4306U,0U},
  {3U,1335U,3999U,0U},
  {3U,3999U,1335U,0U},
  {3U,53U,1547U,0U},
  {3U,1547U,53U,0U},
  {3U,3938U,4157U,0U},
  {3U,4157U,3938U,0U},
  {3U,2015U,2442U,0U},
  {3U,2442U,2015U,0U},
  {3U,405U,875U,0U},
  {3U,875U,405U,0U},
  {3U,2454U,3715U,0U},
  {3U,3715U,2454U,0U},
  {3U,695U,1296U,0U},
  {3U,1296U,695U,0U},
  {3U,4183U,4199U,0U},
  {3U,4199U,4183U,0U},
  {3U,4057U,4797U,0U},
  {3U,4797U,4057U,0U},
  {3U,1643U,4410U,0U},
  {3U,4410U,1643U,0U},
  {3U,754U,1942U,0U},
  {3U,1942U,754U,0U},
  {3U,3323U,3549U,0U},
  {3U,3549U,3323U,0U},
  {3U,487U,2157U,0U},
  {3U,2157U,487U,0U},
  {3U,240U,3003U,0U},
  {3U,3003U,240U,0U},
  {3U,4002U,4357U,0U},
  {3U,4357U,4002U,0U},
  {3U,1678U,4200U,0U},
  {3U,4200U,1678U,0U},
  {3U,1501U,3649U,0U},
  {3U,3649U,1501U,0U},
  {3U,1454U,3375U,0U},
  {3U,3375U,1454U,0U},
  {3U,2879U,3591U,0U},
  {3U,3591U,2879U,0U},
  {3U,1474U,4143U,0U},
  {3U,4143U,1474U,0U},
  {3U,1134U,2905U,0U},
  {3U,2905U,1134U,0U},
  {3U,3040U,4194U,0U},
  {3U,4194U,3040U,0U},
  {3U,4217U,5181U,0U},
  {3U,5181U,4217U,0U},
  {3U,8U,45U,0U},
  {3U,45U,8U,0U},
  {3U,3629U,4094U,0U},
  {3U,4094U,3629U,0U},
  {3U,709U,1135U,0U},
  {3U,1135U,709U,0U},
  {3U,1755U,4021U,0U},
  {3U,4021U,1755U,0U},
  {3U,2460U,4634U,0U},
  {3U,4634U,2460U,0U},
  {3U,3896U,4035U,0U},
  {3U,4035U,3896U,0U},
  {3U,39U,838U,0U},
  {3U,838U,39U,0U},
  {3U,1626U,2896U,0U},
  {3U,2896U,1626U,0U},
  {3U,2947U,4909U,0U},
  {3U,4909U,2947U,0U},
  {3U,2471U,2717U,0U},
  {3U,2717U,2471U,0U},
  {3U,1169U,3954U,0U},
  {3U,3954U,1169U,0U},
  {3U,147U,616U,0U},
  {3U,616U,147U,0U},
  {3U,172U,2662U,0U},
  {3U,2662U,172U,0U},
  {3U,3061U,3072U,0U},
  {3U,3072U,3061U,0U},
  {3U,2217U,2409U,0U},
  {3U,2409U,2217U,0U},
  {3U,5237U,5238U,0U},
  {3U,5238U,5237U,0U},
  {3U,2149U,4695U,0U},
  {3U,4695U,2149U,0U},
  {3U,4487U,4646U,0U},
  {3U,4646U,4487U,0U},
  {3U,2109U,2668U,0U},
  {3U,2668U,2109U,0U},
  {3U,4995U,5337U,0U},
  {3U,5337U,4995U,0U},
  {3U,5230U,5283U,0U},
  {3U,5283U,5230U,0U},
  {3U,120U,2321U,0U},
  {3U,2321U,120U,0U},
  {3U,126U,167U,0U},
  {3U,167U,126U,0U},
  {3U,2389U,2513U,0U},
  {3U,2513U,2389U,0U},
  {3U,367U,3354U,0U},
  {3U,3354U,367U,0U},
  {3U,3399U,4400U,0U},
  {3U,4400U,3399U,0U},
  {3U,3521U,4464U,0U},
  {3U,4464U,3521U,0U},
  {3U,4263U,4436U,0U},
  {3U,4436U,4263U,0U},
  {3U,264U,2052U,0U},
  {3U,2052U,264U,0U},
  {3U,3125U,4951U,0U},
  {3U,4951U,3125U,0U},
  {3U,1963U,4276U,0U},
  {3U,4276U,1963U,0U},
  {3U,898U,921U,0U},
  {3U,921U,898U,0U},
  {3U,3992U,4967U,0U},
  {3U,4967U,3992U,0U},
  {3U,1310U,2514U,0U},
  {3U,2514U,1310U,0U},
  {3U,366U,1971U,0U},
  {3U,1971U,366U,0U},
  {3U,1U,184U,0U},
  {3U,184U,1U,0U},
  {3U,4022U,4193U,0U},
  {3U,4193U,4022U,0U},
  {3U,1110U,3937U,0U},
  {3U,3937U,1110U,0U},
  {3U,969U,2184U,0U},
  {3U,2184U,969U,0U},
  {3U,71U,2069U,0U},
  {3U,2069U,71U,0U},
  {3U,5191U,5246U,0U},
  {3U,5246U,5191U,0U},
  {3U,76U,1898U,0U},
  {3U,1898U,76U,0U},
  {3U,5077U,5349U,0U},
  {3U,5349U,5077U,0U},
  {3U,111U,860U,0U},
  {3U,860U,111U,0U},
  {3U,533U,3660U,0U},
  {3U,3660U,533U,0U},
  {3U,1254U,1277U,0U},
  {3U,1277U,1254U,0U},
  {3U,895U,1453U,0U},
  {3U,1453U,895U,0U},
  {3U,2253U,3200U,0U},
  {3U,3200U,2253U,0U},
  {3U,50U,1492U,0U},
  {3U,1492U,50U,0U},
  {3U,858U,1066U,0U},
  {3U,1066U,858U,0U},
  {3U,3439U,3863U,0U},
  {3U,3863U,3439U,0U},
  {3U,3344U,3345U,0U},
  {3U,3345U,3344U,0U},
  {3U,4584U,4975U,0U},
  {3U,4975U,4584U,0U},
  {3U,308U,3280U,0U},
  {3U,3280U,308U,0U},
  {3U,971U,2530U,0U},
  {3U,2530U,971U,0U},
  {3U,4514U,4779U,0U},
  {3U,4779U,4514U,0U},
  {3U,344U,1402U,0U},
  {3U,1402U,344U,0U},
  {3U,28U,1195U,0U},
  {3U,1195U,28U,0U},
  {3U,53U,1694U,0U},
  {3U,1694U,53U,0U},
  {3U,708U,2946U,0U},
  {3U,2946U,708U,0U},
  {3U,771U,3085U,0U},
  {3U,3085U,771U,0U},
  {3U,2393U,4153U,0U},
  {3U,4153U,2393U,0U},
  {3U,1188U,3963U,0U},
  {3U,3963U,1188U,0U},
  {3U,2642U,3043U,0U},
  {3U,3043U,2642U,0U},
  {3U,490U,950U,0U},
  {3U,950U,490U,0U},
  {3U,2250U,2930U,0U},
  {3U,2930U,2250U,0U},
  {3U,4478U,4832U,0U},
  {3U,4832U,4478U,0U},
  {3U,3520U,5005U,0U},
  {3U,5005U,3520U,0U},
  {3U,924U,926U,0U},
  {3U,926U,924U,0U},
  {3U,1108U,2289U,0U},
  {3U,2289U,1108U,0U},
  {3U,1372U,2090U,0U},
  {3U,2090U,1372U,0U},
  {3U,106U,1360U,0U},
  {3U,1360U,106U,0U},
  {3U,124U,2058U,0U},
  {3U,2058U,124U,0U},
  {3U,3867U,4030U,0U},
  {3U,4030U,3867U,0U},
  {3U,939U,1109U,0U},
  {3U,1109U,939U,0U},
  {3U,343U,2730U,0U},
  {3U,2730U,343U,0U},
  {3U,3096U,3644U,0U},
  {3U,3644U,3096U,0U},
  {3U,345U,2168U,0U},
  {3U,2168U,345U,0U},
  {3U,498U,1814U,0U},
  {3U,1814U,498U,0U},
  {3U,1598U,4291U,0U},
  {3U,4291U,1598U,0U},
  {3U,905U,1880U,0U},
  {3U,1880U,905U,0U},
  {3U,141U,774U,0U},
  {3U,774U,141U,0U},
  {3U,4758U,4922U,0U},
  {3U,4922U,4758U,0U},
  {3U,71U,3814U,0U},
  {3U,3814U,71U,0U},
  {3U,802U,1229U,0U},
  {3U,1229U,802U,0U},
  {3U,2872U,2882U,0U},
  {3U,2882U,2872U,0U},
  {3U,4121U,4952U,0U},
  {3U,4952U,4121U,0U},
  {3U,1162U,1417U,0U},
  {3U,1417U,1162U,0U},
  {3U,1276U,2591U,0U},
  {3U,2591U,1276U,0U},
  {3U,1236U,2230U,0U},
  {3U,2230U,1236U,0U},
  {3U,2054U,4663U,0U},
  {3U,4663U,2054U,0U},
  {3U,568U,828U,0U},
  {3U,828U,568U,0U},
  {3U,143U,2467U,0U},
  {3U,2467U,143U,0U},
  {3U,4415U,5213U,0U},
  {3U,5213U,4415U,0U},
  {3U,3154U,4957U,0U},
  {3U,4957U,3154U,0U},
  {3U,5305U,5340U,0U},
  {3U,5340U,5305U,0U},
  {3U,4741U,5297U,0U},
  {3U,5297U,4741U,0U},
  {3U,2739U,3405U,0U},
  {3U,3405U,2739U,0U},
  {3U,2288U,3278U,0U},
  {3U,3278U,2288U,0U},
  {3U,326U,2466U,0U},
  {3U,2466U,326U,0U},
  {3U,295U,3217U,0U},
  {3U,3217U,295U,0U},
  {3U,4459U,5251U,0U},
  {3U,5251U,4459U,0U},
  {3U,3484U,5038U,0U},
  {3U,5038U,3484U,0U},
  {3U,2671U,2674U,0U},
  {3U,2674U,2671U,0U},
  {3U,4732U,4752U,0U},
  {3U,4752U,4732U,0U},
  {3U,2957U,2959U,0U},
  {3U,2959U,2957U,0U},
  {3U,120U,392U,0U},
  {3U,392U,120U,0U},
  {3U,2505U,3223U,0U},
  {3U,3223U,2505U,0U},
  {3U,2133U,2341U,0U},
  {3U,2341U,2133U,0U},
  {3U,5190U,5327U,0U},
  {3U,5327U,5190U,0U},
  {3U,652U,1323U,0U},
  {3U,1323U,652U,0U},
  {3U,383U,3090U,0U},
  {3U,3090U,383U,0U},
  {3U,146U,280U,0U},
  {3U,280U,146U,0U},
  {3U,382U,547U,0U},
  {3U,547U,382U,0U},
  {3U,4087U,4405U,0U},
  {3U,4405U,4087U,0U},
  {3U,157U,257U,0U},
  {3U,257U,157U,0U},
  {3U,1787U,2457U,0U},
  {3U,2457U,1787U,0U},
  {3U,1107U,1574U,0U},
  {3U,1574U,1107U,0U},
  {3U,1614U,4325U,0U},
  {3U,4325U,1614U,0U},
  {3U,1116U,1377U,0U},
  {3U,1377U,1116U,0U},
  {3U,2387U,2555U,0U},
  {3U,2555U,2387U,0U},
  {3U,1499U,4158U,0U},
  {3U,4158U,1499U,0U},
  {3U,433U,784U,0U},
  {3U,784U,433U,0U},
  {3U,659U,692U,0U},
  {3U,692U,659U,0U},
  {3U,874U,2864U,0U},
  {3U,2864U,874U,0U},
  {3U,2480U,4780U,0U},
  {3U,4780U,2480U,0U},
  {3U,218U,2924U,0U},
  {3U,2924U,218U,0U},
  {3U,4015U,4221U,0U},
  {3U,4221U,4015U,0U},
  {3U,2534U,4793U,0U},
  {3U,4793U,2534U,0U},
  {3U,2163U,4075U,0U},
  {3U,4075U,2163U,0U},
  {3U,621U,2818U,0U},
  {3U,2818U,621U,0U},
  {3U,980U,2583U,0U},
  {3U,2583U,980U,0U},
  {3U,715U,3120U,0U},
  {3U,3120U,715U,0U},
  {3U,562U,596U,0U},
  {3U,596U,562U,0U},
  {3U,1340U,4073U,0U},
  {3U,4073U,1340U,0U},
  {3U,1732U,3633U,0U},
  {3U,3633U,1732U,0U},
  {3U,722U,2628U,0U},
  {3U,2628U,722U,0U},
  {3U,2722U,3401U,0U},
  {3U,3401U,2722U,0U},
  {3U,2222U,2963U,0U},
  {3U,2963U,2222U,0U},
  {3U,134U,1001U,0U},
  {3U,1001U,134U,0U},
  {3U,768U,3382U,0U},
  {3U,3382U,768U,0U},
  {3U,247U,3012U,0U},
  {3U,3012U,247U,0U},
  {3U,2854U,3877U,0U},
  {3U,3877U,2854U,0U},
  {3U,4504U,4840U,0U},
  {3U,4840U,4504U,0U},
  {3U,4278U,5150U,0U},
  {3U,5150U,4278U,0U},
  {3U,169U,1488U,0U},
  {3U,1488U,169U,0U},
  {3U,4614U,4615U,0U},
  {3U,4615U,4614U,0U},
  {3U,1833U,1915U,0U},
  {3U,1915U,1833U,0U},
  {3U,71U,5117U,0U},
  {3U,5117U,71U,0U},
  {3U,2456U,3162U,0U},
  {3U,3162U,2456U,0U},
  {3U,1751U,3595U,0U},
  {3U,3595U,1751U,0U},
  {3U,1786U,4585U,0U},
  {3U,4585U,1786U,0U},
  {3U,744U,2361U,0U},
  {3U,2361U,744U,0U},
  {3U,3627U,4869U,0U},
  {3U,4869U,3627U,0U},
  {3U,1118U,2583U,0U},
  {3U,2583U,1118U,0U},
  {3U,4918U,4919U,0U},
  {3U,4919U,4918U,0U},
  {3U,3617U,3972U,0U},
  {3U,3972U,3617U,0U},
  {3U,1868U,2125U,0U},
  {3U,2125U,1868U,0U},
  {3U,3180U,4959U,0U},
  {3U,4959U,3180U,0U},
  {3U,1081U,3472U,0U},
  {3U,3472U,1081U,0U},
  {3U,3879U,5115U,0U},
  {3U,5115U,3879U,0U},
  {3U,736U,2260U,0U},
  {3U,2260U,736U,0U},
  {3U,2989U,4913U,0U},
  {3U,4913U,2989U,0U},
  {3U,2683U,4827U,0U},
  {3U,4827U,2683U,0U},
  {3U,1544U,1553U,0U},
  {3U,1553U,1544U,0U},
  {3U,569U,2306U,0U},
  {3U,2306U,569U,0U},
  {3U,1885U,2251U,0U},
  {3U,2251U,1885U,0U},
  {3U,1002U,2626U,0U},
  {3U,2626U,1002U,0U},
  {3U,914U,1962U,0U},
  {3U,1962U,914U,0U},
  {3U,2216U,3639U,0U},
  {3U,3639U,2216U,0U},
  {3U,264U,307U,0U},
  {3U,307U,264U,0U},
  {3U,1252U,3237U,0U},
  {3U,3237U,1252U,0U},
  {3U,37U,1300U,0U},
  {3U,1300U,37U,0U},
  {3U,166U,1583U,0U},
  {3U,1583U,166U,0U},
  {3U,166U,882U,0U},
  {3U,882U,166U,0U},
  {3U,303U,3272U,0U},
  {3U,3272U,303U,0U},
  {3U,27U,1179U,0U},
  {3U,1179U,27U,0U},
  {3U,929U,3682U,0U},
  {3U,3682U,929U,0U},
  {3U,688U,792U,0U},
  {3U,792U,688U,0U},
  {3U,1253U,2600U,0U},
  {3U,2600U,1253U,0U},
  {3U,4536U,4829U,0U},
  {3U,4829U,4536U,0U},
  {3U,916U,3832U,0U},
  {3U,3832U,916U,0U},
  {3U,120U,1947U,0U},
  {3U,1947U,120U,0U},
  {3U,3479U,3480U,0U},
  {3U,3480U,3479U,0U},
  {3U,1568U,1569U,0U},
  {3U,1569U,1568U,0U},
  {3U,4344U,5165U,0U},
  {3U,5165U,4344U,0U},
  {3U,3333U,3334U,0U},
  {3U,3334U,3333U,0U},
  {3U,793U,1088U,0U},
  {3U,1088U,793U,0U},
  {3U,4468U,4469U,0U},
  {3U,4469U,4468U,0U},
  {3U,356U,1293U,0U},
  {3U,1293U,356U,0U},
  {3U,3915U,4704U,0U},
  {3U,4704U,3915U,0U},
  {3U,886U,997U,0U},
  {3U,997U,886U,0U},
  {3U,3964U,4808U,0U},
  {3U,4808U,3964U,0U},
  {3U,1595U,4286U,0U},
  {3U,4286U,1595U,0U},
  {3U,2529U,3047U,0U},
  {3U,3047U,2529U,0U},
  {3U,3659U,4034U,0U},
  {3U,4034U,3659U,0U},
  {3U,4566U,4849U,0U},
  {3U,4849U,4566U,0U},
  {3U,864U,1970U,0U},
  {3U,1970U,864U,0U},
  {3U,275U,584U,0U},
  {3U,584U,275U,0U},
  {3U,1472U,4141U,0U},
  {3U,4141U,1472U,0U},
  {3U,4055U,5097U,0U},
  {3U,5097U,4055U,0U},
  {3U,420U,1704U,0U},
  {3U,1704U,420U,0U},
  {3U,14U,2401U,0U},
  {3U,2401U,14U,0U},
  {3U,620U,740U,0U},
  {3U,740U,620U,0U},
  {3U,2112U,4598U,0U},
  {3U,4598U,2112U,0U},
  {3U,3U,474U,0U},
  {3U,474U,3U,0U},
  {3U,4168U,4935U,0U},
  {3U,4935U,4168U,0U},
  {3U,903U,1188U,0U},
  {3U,1188U,903U,0U},
  {3U,245U,1056U,0U},
  {3U,1056U,245U,0U},
  {3U,5051U,5052U,0U},
  {3U,5052U,5051U,0U},
  {3U,2969U,4138U,0U},
  {3U,4138U,2969U,0U},
  {3U,5141U,5235U,0U},
  {3U,5235U,5141U,0U},
  {3U,319U,1809U,0U},
  {3U,1809U,319U,0U},
  {3U,53U,1699U,0U},
  {3U,1699U,53U,0U},
  {3U,499U,1412U,0U},
  {3U,1412U,499U,0U},
  {3U,1535U,3577U,0U},
  {3U,3577U,1535U,0U},
  {3U,1045U,2564U,0U},
  {3U,2564U,1045U,0U},
  {3U,2153U,3078U,0U},
  {3U,3078U,2153U,0U},
  {3U,4871U,5321U,0U},
  {3U,5321U,4871U,0U},
  {3U,1866U,3317U,0U},
  {3U,3317U,1866U,0U},
  {3U,539U,3150U,0U},
  {3U,3150U,539U,0U},
  {3U,113U,1878U,0U},
  {3U,1878U,113U,0U},
  {3U,357U,973U,0U},
  {3U,973U,357U,0U},
  {3U,2546U,3055U,0U},
  {3U,3055U,2546U,0U},
  {3U,217U,2616U,0U},
  {3U,2616U,217U,0U},
  {3U,431U,2712U,0U},
  {3U,2712U,431U,0U},
  {3U,638U,2367U,0U},
  {3U,2367U,638U,0U},
  {3U,4079U,4080U,0U},
  {3U,4080U,4079U,0U},
  {3U,68U,591U,0U},
  {3U,591U,68U,0U},
  {3U,632U,2147U,0U},
  {3U,2147U,632U,0U},
  {3U,1903U,2212U,0U},
  {3U,2212U,1903U,0U},
  {3U,1985U,2718U,0U},
  {3U,2718U,1985U,0U},
  {3U,4579U,4667U,0U},
  {3U,4667U,4579U,0U},
  {3U,321U,1128U,0U},
  {3U,1128U,321U,0U},
  {3U,120U,2318U,0U},
  {3U,2318U,120U,0U},
  {3U,1581U,1737U,0U},
  {3U,1737U,1581U,0U},
  {3U,4599U,5091U,0U},
  {3U,5091U,4599U,0U},
  {3U,307U,2134U,0U},
  {3U,2134U,307U,0U},
  {3U,4440U,4441U,0U},
  {3U,4441U,4440U,0U},
  {3U,4365U,4507U,0U},
  {3U,4507U,4365U,0U},
  {3U,857U,1965U,0U},
  {3U,1965U,857U,0U},
  {3U,665U,3168U,0U},
  {3U,3168U,665U,0U},
  {3U,332U,3379U,0U},
  {3U,3379U,332U,0U},
  {3U,1524U,1852U,0U},
  {3U,1852U,1524U,0U},
  {3U,518U,728U,0U},
  {3U,728U,518U,0U},
  {3U,4713U,4785U,0U},
  {3U,4785U,4713U,0U},
  {3U,4422U,5195U,0U},
  {3U,5195U,4422U,0U},
  {3U,295U,3216U,0U},
  {3U,3216U,295U,0U},
  {3U,1890U,3170U,0U},
  {3U,3170U,1890U,0U},
  {3U,1640U,4232U,0U},
  {3U,4232U,1640U,0U},
  {3U,3151U,3152U,0U},
  {3U,3152U,3151U,0U},
  {3U,2304U,2695U,0U},
  {3U,2695U,2304U,0U},
  {3U,4145U,4709U,0U},
  {3U,4709U,4145U,0U},
  {3U,1542U,1778U,0U},
  {3U,1778U,1542U,0U},
  {3U,3135U,4380U,0U},
  {3U,4380U,3135U,0U},
  {3U,2004U,4644U,0U},
  {3U,4644U,2004U,0U},
  {3U,1480U,1754U,0U},
  {3U,1754U,1480U,0U},
  {3U,2749U,3795U,0U},
  {3U,3795U,2749U,0U},
  {3U,1663U,2885U,0U},
  {3U,2885U,1663U,0U},
  {3U,2623U,4103U,0U},
  {3U,4103U,2623U,0U},
  {3U,1447U,2670U,0U},
  {3U,2670U,1447U,0U},
  {3U,516U,2438U,0U},
  {3U,2438U,516U,0U},
  {3U,1424U,3422U,0U},
  {3U,3422U,1424U,0U},
  {3U,2661U,2999U,0U},
  {3U,2999U,2661U,0U},
  {3U,1443U,2258U,0U},
  {3U,2258U,1443U,0U},
  {3U,2898U,4890U,0U},
  {3U,4890U,2898U,0U},
  {3U,1334U,4065U,0U},
  {3U,4065U,1334U,0U},
  {3U,1347U,4076U,0U},
  {3U,4076U,1347U,0U},
  {3U,2515U,2916U,0U},
  {3U,2916U,2515U,0U},
  {3U,2891U,4626U,0U},
  {3U,4626U,2891U,0U},
  {3U,1909U,3179U,0U},
  {3U,3179U,1909U,0U},
  {3U,1948U,2778U,0U},
  {3U,2778U,1948U,0U},
  {3U,124U,1507U,0U},
  {3U,1507U,124U,0U},
  {3U,1043U,1556U,0U},
  {3U,1556U,1043U,0U},
  {3U,124U,571U,0U},
  {3U,571U,124U,0U},
  {3U,3926U,5124U,0U},
  {3U,5124U,3926U,0U},
  {3U,1999U,2760U,0U},
  {3U,2760U,1999U,0U},
  {3U,888U,1231U,0U},
  {3U,1231U,888U,0U},
  {3U,3210U,4287U,0U},
  {3U,4287U,3210U,0U},
  {3U,4414U,4835U,0U},
  {3U,4835U,4414U,0U},
  {3U,4420U,5200U,0U},
  {3U,5200U,4420U,0U},
  {3U,995U,3105U,0U},
  {3U,3105U,995U,0U},
  {3U,1993U,4622U,0U},
  {3U,4622U,1993U,0U},
  {3U,5163U,5353U,0U},
  {3U,5353U,5163U,0U},
  {3U,1395U,2169U,0U},
  {3U,2169U,1395U,0U},
  {3U,267U,1227U,0U},
  {3U,1227U,267U,0U},
  {3U,1281U,2606U,0U},
  {3U,2606U,1281U,0U},
  {3U,880U,1516U,0U},
  {3U,1516U,880U,0U},
  {3U,2675U,2907U,0U},
  {3U,2907U,2675U,0U},
  {3U,1413U,3719U,0U},
  {3U,3719U,1413U,0U},
  {3U,743U,923U,0U},
  {3U,923U,743U,0U},
  {3U,93U,207U,0U},
  {3U,207U,93U,0U},
  {3U,516U,2225U,0U},
  {3U,2225U,516U,0U},
  {3U,1332U,2056U,0U},
  {3U,2056U,1332U,0U},
  {3U,41U,318U,0U},
  {3U,318U,41U,0U},
  {3U,1466U,1548U,0U},
  {3U,1548U,1466U,0U},
  {3U,321U,1981U,0U},
  {3U,1981U,321U,0U},
  {3U,113U,719U,0U},
  {3U,719U,113U,0U},
  {3U,964U,1829U,0U},
  {3U,1829U,964U,0U},
  {3U,4715U,4846U,0U},
  {3U,4846U,4715U,0U},
  {3U,71U,4687U,0U},
  {3U,4687U,71U,0U},
  {3U,1531U,2422U,0U},
  {3U,2422U,1531U,0U},
  {3U,53U,1408U,0U},
  {3U,1408U,53U,0U},
  {3U,5067U,5348U,0U},
  {3U,5348U,5067U,0U},
  {3U,337U,795U,0U},
  {3U,795U,337U,0U},
  {3U,4898U,4954U,0U},
  {3U,4954U,4898U,0U},
  {3U,2631U,2939U,0U},
  {3U,2939U,2631U,0U},
  {3U,120U,2327U,0U},
  {3U,2327U,120U,0U},
  {3U,100U,315U,0U},
  {3U,315U,100U,0U},
  {3U,1851U,4596U,0U},
  {3U,4596U,1851U,0U},
  {3U,335U,1349U,0U},
  {3U,1349U,335U,0U},
  {3U,168U,831U,0U},
  {3U,831U,168U,0U},
  {3U,1278U,4029U,0U},
  {3U,4029U,1278U,0U},
  {3U,931U,2399U,0U},
  {3U,2399U,931U,0U},
  {3U,218U,2585U,0U},
  {3U,2585U,218U,0U},
  {3U,268U,1901U,0U},
  {3U,1901U,268U,0U},
  {3U,2901U,4892U,0U},
  {3U,4892U,2901U,0U},
  {3U,4825U,5314U,0U},
  {3U,5314U,4825U,0U},
  {3U,3081U,4149U,0U},
  {3U,4149U,3081U,0U},
  {3U,39U,1316U,0U},
  {3U,1316U,39U,0U},
  {3U,445U,1197U,0U},
  {3U,1197U,445U,0U},
  {3U,34U,1256U,0U},
  {3U,1256U,34U,0U},
  {3U,497U,870U,0U},
  {3U,870U,497U,0U},
  {3U,4265U,4438U,0U},
  {3U,4438U,4265U,0U},
  {3U,4493U,5233U,0U},
  {3U,5233U,4493U,0U},
  {3U,1748U,4549U,0U},
  {3U,4549U,1748U,0U},
  {3U,171U,371U,0U},
  {3U,371U,171U,0U},
  {3U,793U,1089U,0U},
  {3U,1089U,793U,0U},
  {3U,654U,1376U,0U},
  {3U,1376U,654U,0U},
  {3U,398U,2601U,0U},
  {3U,2601U,398U,0U},
  {3U,1729U,4537U,0U},
  {3U,4537U,1729U,0U},
  {3U,2895U,4885U,0U},
  {3U,4885U,2895U,0U},
  {3U,637U,1263U,0U},
  {3U,1263U,637U,0U},
  {3U,71U,1932U,0U},
  {3U,1932U,71U,0U},
  {3U,3559U,4084U,0U},
  {3U,4084U,3559U,0U},
  {3U,896U,2346U,0U},
  {3U,2346U,896U,0U},
  {3U,540U,948U,0U},
  {3U,948U,540U,0U},
  {3U,257U,1187U,0U},
  {3U,1187U,257U,0U},
  {3U,273U,1283U,0U},
  {3U,1283U,273U,0U},
  {3U,598U,1949U,0U},
  {3U,1949U,598U,0U},
  {3U,1390U,3768U,0U},
  {3U,3768U,1390U,0U},
  {3U,1646U,2733U,0U},
  {3U,2733U,1646U,0U},
  {3U,3215U,4181U,0U},
  {3U,4181U,3215U,0U},
  {3U,218U,2116U,0U},
  {3U,2116U,218U,0U},
  {3U,839U,3313U,0U},
  {3U,3313U,839U,0U},
  {3U,2060U,3221U,0U},
  {3U,3221U,2060U,0U},
  {3U,3989U,3990U,0U},
  {3U,3990U,3989U,0U},
  {3U,1388U,1797U,0U},
  {3U,1797U,1388U,0U},
  {3U,511U,2094U,0U},
  {3U,2094U,511U,0U},
  {3U,3092U,3134U,0U},
  {3U,3134U,3092U,0U},
  {3U,4962U,5096U,0U},
  {3U,5096U,4962U,0U},
  {3U,2800U,3084U,0U},
  {3U,3084U,2800U,0U},
  {3U,1876U,2343U,0U},
  {3U,2343U,1876U,0U},
  {3U,4324U,5223U,0U},
  {3U,5223U,4324U,0U},
  {3U,2487U,3741U,0U},
  {3U,3741U,2487U,0U},
  {3U,481U,1210U,0U},
  {3U,1210U,481U,0U},
  {3U,2764U,3029U,0U},
  {3U,3029U,2764U,0U},
  {3U,2581U,2608U,0U},
  {3U,2608U,2581U,0U},
  {3U,1961U,2756U,0U},
  {3U,2756U,1961U,0U},
  {3U,4017U,4071U,0U},
  {3U,4071U,4017U,0U},
  {3U,4520U,4617U,0U},
  {3U,4617U,4520U,0U},
  {3U,1459U,4129U,0U},
  {3U,4129U,1459U,0U},
  {3U,764U,3770U,0U},
  {3U,3770U,764U,0U},
  {3U,2011U,2611U,0U},
  {3U,2611U,2011U,0U},
  {3U,556U,578U,0U},
  {3U,578U,556U,0U},
  {3U,2721U,4297U,0U},
  {3U,4297U,2721U,0U},
  {3U,471U,843U,0U},
  {3U,843U,471U,0U},
  {3U,76U,551U,0U},
  {3U,551U,76U,0U},
  {3U,1163U,3384U,0U},
  {3U,3384U,1163U,0U},
  {3U,4753U,5298U,0U},
  {3U,5298U,4753U,0U},
  {3U,2720U,4842U,0U},
  {3U,4842U,2720U,0U},
  {3U,47U,1434U,0U},
  {3U,1434U,47U,0U},
  {3U,5035U,5227U,0U},
  {3U,5227U,5035U,0U},
  {3U,48U,209U,0U},
  {3U,209U,48U,0U},
  {3U,4477U,4505U,0U},
  {3U,4505U,4477U,0U},
  {3U,2780U,2781U,0U},
  {3U,2781U,2780U,0U},
  {3U,4934U,5055U,0U},
  {3U,5055U,4934U,0U},
  {3U,1916U,4611U,0U},
  {3U,4611U,1916U,0U},
  {3U,2413U,4082U,0U},
  {3U,4082U,2413U,0U},
  {3U,710U,2064U,0U},
  {3U,2064U,710U,0U},
  {3U,379U,1404U,0U},
  {3U,1404U,379U,0U},
  {3U,1608U,2532U,0U},
  {3U,2532U,1608U,0U},
  {3U,663U,2987U,0U},
  {3U,2987U,663U,0U},
  {3U,3424U,3859U,0U},
  {3U,3859U,3424U,0U},
  {3U,195U,2358U,0U},
  {3U,2358U,195U,0U},
  {3U,4518U,4851U,0U},
  {3U,4851U,4518U,0U},
  {3U,181U,261U,0U},
  {3U,261U,181U,0U},
  {3U,2751U,4226U,0U},
  {3U,4226U,2751U,0U},
  {3U,158U,1744U,0U},
  {3U,1744U,158U,0U},
  {3U,2077U,2430U,0U},
  {3U,2430U,2077U,0U},
  {3U,286U,2867U,0U},
  {3U,2867U,286U,0U},
  {3U,151U,2565U,0U},
  {3U,2565U,151U,0U},
  {3U,4356U,5147U,0U},
  {3U,5147U,4356U,0U},
  {3U,17U,143U,0U},
  {3U,143U,17U,0U},
  {3U,416U,2234U,0U},
  {3U,2234U,416U,0U},
  {3U,536U,845U,0U},
  {3U,845U,536U,0U},
  {3U,3943U,5128U,0U},
  {3U,5128U,3943U,0U},
  {3U,1099U,1199U,0U},
  {3U,1199U,1099U,0U},
  {3U,370U,971U,0U},
  {3U,971U,370U,0U},
  {3U,4814U,5053U,0U},
  {3U,5053U,4814U,0U},
  {3U,67U,457U,0U},
  {3U,457U,67U,0U},
  {3U,1799U,2480U,0U},
  {3U,2480U,1799U,0U},
  {3U,990U,1121U,0U},
  {3U,1121U,990U,0U},
  {3U,2958U,2960U,0U},
  {3U,2960U,2958U,0U},
  {3U,632U,2081U,0U},
  {3U,2081U,632U,0U},
  {3U,324U,2204U,0U},
  {3U,2204U,324U,0U},
  {3U,558U,1835U,0U},
  {3U,1835U,558U,0U},
  {3U,382U,3167U,0U},
  {3U,3167U,382U,0U},
  {3U,365U,868U,0U},
  {3U,868U,365U,0U},
  {3U,269U,2119U,0U},
  {3U,2119U,269U,0U},
  {3U,1084U,2505U,0U},
  {3U,2505U,1084U,0U},
  {3U,1159U,3940U,0U},
  {3U,3940U,1159U,0U},
  {3U,499U,2584U,0U},
  {3U,2584U,499U,0U},
  {3U,2379U,3100U,0U},
  {3U,3100U,2379U,0U},
  {3U,1639U,3415U,0U},
  {3U,3415U,1639U,0U},
  {3U,417U,2793U,0U},
  {3U,2793U,417U,0U},
  {3U,500U,2518U,0U},
  {3U,2518U,500U,0U},
  {3U,500U,735U,0U},
  {3U,735U,500U,0U},
  {3U,872U,1078U,0U},
  {3U,1078U,872U,0U},
  {3U,3267U,3392U,0U},
  {3U,3392U,3267U,0U},
  {3U,1579U,2922U,0U},
  {3U,2922U,1579U,0U},
  {3U,290U,2213U,0U},
  {3U,2213U,290U,0U},
  {3U,942U,2308U,0U},
  {3U,2308U,942U,0U},
  {3U,1521U,1526U,0U},
  {3U,1526U,1521U,0U},
  {3U,243U,2184U,0U},
  {3U,2184U,243U,0U},
  {3U,2000U,4641U,0U},
  {3U,4641U,2000U,0U},
  {3U,1407U,2620U,0U},
  {3U,2620U,1407U,0U},
  {3U,713U,797U,0U},
  {3U,797U,713U,0U},
  {3U,3496U,5042U,0U},
  {3U,5042U,3496U,0U},
  {3U,1661U,4450U,0U},
  {3U,4450U,1661U,0U},
  {3U,4502U,4503U,0U},
  {3U,4503U,4502U,0U},
  {3U,167U,626U,0U},
  {3U,626U,167U,0U},
  {3U,2955U,4910U,0U},
  {3U,4910U,2955U,0U},
  {3U,3693U,4041U,0U},
  {3U,4041U,3693U,0U},
  {3U,71U,5371U,0U},
  {3U,5371U,71U,0U},
  {3U,4562U,4971U,0U},
  {3U,4971U,4562U,0U},
  {3U,644U,1365U,0U},
  {3U,1365U,644U,0U},
  {3U,4377U,4640U,0U},
  {3U,4640U,4377U,0U},
  {3U,847U,2685U,0U},
  {3U,2685U,847U,0U},
  {3U,2330U,3731U,0U},
  {3U,3731U,2330U,0U},
  {3U,80U,103U,0U},
  {3U,103U,80U,0U},
  {3U,556U,3668U,0U},
  {3U,3668U,556U,0U},
  {3U,98U,2097U,0U},
  {3U,2097U,98U,0U},
  {3U,2919U,3511U,0U},
  {3U,3511U,2919U,0U},
  {3U,2719U,3882U,0U},
  {3U,3882U,2719U,0U},
  {3U,4696U,4700U,0U},
  {3U,4700U,4696U,0U},
  {3U,841U,2453U,0U},
  {3U,2453U,841U,0U},
  {3U,1575U,3505U,0U},
  {3U,3505U,1575U,0U},
  {3U,3378U,3631U,0U},
  {3U,3631U,3378U,0U},
  {3U,230U,239U,0U},
  {3U,239U,230U,0U},
  {3U,2427U,4230U,0U},
  {3U,4230U,2427U,0U},
  {3U,3528U,3551U,0U},
  {3U,3551U,3528U,0U},
  {3U,1132U,1225U,0U},
  {3U,1225U,1132U,0U},
  {3U,561U,3670U,0U},
  {3U,3670U,561U,0U},
  {3U,3236U,4004U,0U},
  {3U,4004U,3236U,0U},
  {3U,5229U,5243U,0U},
  {3U,5243U,5229U,0U},
  {3U,1269U,1565U,0U},
  {3U,1565U,1269U,0U},
  {3U,4652U,5113U,0U},
  {3U,5113U,4652U,0U},
  {3U,3947U,4944U,0U},
  {3U,4944U,3947U,0U},
  {3U,71U,3871U,0U},
  {3U,3871U,71U,0U},
  {3U,3614U,5068U,0U},
  {3U,5068U,3614U,0U},
  {3U,644U,1795U,0U},
  {3U,1795U,644U,0U},
  {3U,3437U,3438U,0U},
  {3U,3438U,3437U,0U},
  {3U,163U,766U,0U},
  {3U,766U,163U,0U},
  {3U,253U,1217U,0U},
  {3U,1217U,253U,0U},
  {3U,42U,473U,0U},
  {3U,473U,42U,0U},
  {3U,1021U,1446U,0U},
  {3U,1446U,1021U,0U},
  {3U,3612U,3967U,0U},
  {3U,3967U,3612U,0U},
  {3U,4423U,4511U,0U},
  {3U,4511U,4423U,0U},
  {3U,755U,759U,0U},
  {3U,759U,755U,0U},
  {3U,266U,1848U,0U},
  {3U,1848U,266U,0U},
  {3U,3696U,5075U,0U},
  {3U,5075U,3696U,0U},
  {3U,2868U,4873U,0U},
  {3U,4873U,2868U,0U},
  {3U,3419U,3544U,0U},
  {3U,3544U,3419U,0U},
  {3U,3233U,3330U,0U},
  {3U,3330U,3233U,0U},
  {3U,1773U,2508U,0U},
  {3U,2508U,1773U,0U},
  {3U,4294U,5208U,0U},
  {3U,5208U,4294U,0U},
  {3U,1891U,4600U,0U},
  {3U,4600U,1891U,0U},
  {3U,2823U,3115U,0U},
  {3U,3115U,2823U,0U},
  {3U,4186U,5178U,0U},
  {3U,5178U,4186U,0U},
  {3U,699U,2516U,0U},
  {3U,2516U,699U,0U},
  {3U,581U,2177U,0U},
  {3U,2177U,581U,0U},
  {3U,548U,1493U,0U},
  {3U,1493U,548U,0U},
  {3U,1791U,3695U,0U},
  {3U,3695U,1791U,0U},
  {3U,3294U,4534U,0U},
  {3U,4534U,3294U,0U},
  {3U,807U,950U,0U},
  {3U,950U,807U,0U},
  {3U,4724U,5294U,0U},
  {3U,5294U,4724U,0U},
  {3U,342U,1344U,0U},
  {3U,1344U,342U,0U},
  {3U,341U,1743U,0U},
  {3U,1743U,341U,0U},
  {3U,1172U,3956U,0U},
  {3U,3956U,1172U,0U},
  {3U,4239U,5088U,0U},
  {3U,5088U,4239U,0U},
  {3U,186U,2395U,0U},
  {3U,2395U,186U,0U},
  {3U,2910U,3234U,0U},
  {3U,3234U,2910U,0U},
  {3U,120U,197U,0U},
  {3U,197U,120U,0U},
  {3U,3365U,4201U,0U},
  {3U,4201U,3365U,0U},
  {3U,553U,1410U,0U},
  {3U,1410U,553U,0U},
  {3U,2477U,2812U,0U},
  {3U,2812U,2477U,0U},
  {3U,4019U,4133U,0U},
  {3U,4133U,4019U,0U},
  {3U,1302U,1827U,0U},
  {3U,1827U,1302U,0U},
  {3U,1601U,3339U,0U},
  {3U,3339U,1601U,0U},
  {3U,787U,2539U,0U},
  {3U,2539U,787U,0U},
  {3U,624U,1004U,0U},
  {3U,1004U,624U,0U},
  {3U,4756U,4757U,0U},
  {3U,4757U,4756U,0U},
  {3U,4494U,4843U,0U},
  {3U,4843U,4494U,0U},
  {3U,5154U,5249U,0U},
  {3U,5249U,5154U,0U},
  {3U,1004U,3881U,0U},
  {3U,3881U,1004U,0U},
  {3U,4716U,5293U,0U},
  {3U,5293U,4716U,0U},
  {3U,3336U,4981U,0U},
  {3U,4981U,3336U,0U},
  {3U,550U,2432U,0U},
  {3U,2432U,550U,0U},
  {3U,760U,1511U,0U},
  {3U,1511U,760U,0U},
  {3U,2933U,3676U,0U},
  {3U,3676U,2933U,0U},
  {3U,192U,2768U,0U},
  {3U,2768U,192U,0U},
  {3U,2292U,3618U,0U},
  {3U,3618U,2292U,0U},
  {3U,4104U,4708U,0U},
  {3U,4708U,4104U,0U},
  {3U,497U,3553U,0U},
  {3U,3553U,497U,0U},
  {3U,333U,1762U,0U},
  {3U,1762U,333U,0U},
  {3U,100U,652U,0U},
  {3U,652U,100U,0U},
  {3U,123U,748U,0U},
  {3U,748U,123U,0U},
  {3U,1483U,4155U,0U},
  {3U,4155U,1483U,0U},
  {3U,5136U,5145U,0U},
  {3U,5145U,5136U,0U},
  {3U,3163U,3774U,0U},
  {3U,3774U,3163U,0U},
  {3U,4160U,4271U,0U},
  {3U,4271U,4160U,0U},
  {3U,3110U,3301U,0U},
  {3U,3301U,3110U,0U},
  {3U,83U,1378U,0U},
  {3U,1378U,83U,0U},
  {3U,116U,1519U,0U},
  {3U,1519U,116U,0U},
  {3U,3545U,5048U,0U},
  {3U,5048U,3545U,0U},
  {3U,922U,3835U,0U},
  {3U,3835U,922U,0U},
  {3U,3351U,3380U,0U},
  {3U,3380U,3351U,0U},
  {3U,553U,622U,0U},
  {3U,622U,553U,0U},
  {3U,342U,1320U,0U},
  {3U,1320U,342U,0U},
  {3U,3689U,4077U,0U},
  {3U,4077U,3689U,0U},
  {3U,2429U,3826U,0U},
  {3U,3826U,2429U,0U},
  {3U,1697U,3708U,0U},
  {3U,3708U,1697U,0U},
  {3U,1559U,4213U,0U},
  {3U,4213U,1559U,0U},
  {3U,156U,1469U,0U},
  {3U,1469U,156U,0U},
  {3U,2874U,4368U,0U},
  {3U,4368U,2874U,0U},
  {3U,329U,389U,0U},
  {3U,389U,329U,0U},
  {3U,2057U,4666U,0U},
  {3U,4666U,2057U,0U},
  {3U,2734U,2736U,0U},
  {3U,2736U,2734U,0U},
  {3U,1615U,3790U,0U},
  {3U,3790U,1615U,0U},
  {3U,354U,2622U,0U},
  {3U,2622U,354U,0U},
  {3U,2068U,4674U,0U},
  {3U,4674U,2068U,0U},
  {3U,539U,628U,0U},
  {3U,628U,539U,0U},
  {3U,4U,591U,0U},
  {3U,591U,4U,0U},
  {3U,3858U,4101U,0U},
  {3U,4101U,3858U,0U},
  {3U,27U,154U,0U},
  {3U,154U,27U,0U},
  {3U,1399U,4100U,0U},
  {3U,4100U,1399U,0U},
  {3U,414U,530U,0U},
  {3U,530U,414U,0U},
  {3U,120U,1359U,0U},
  {3U,1359U,120U,0U},
  {3U,811U,2952U,0U},
  {3U,2952U,811U,0U},
  {3U,238U,2981U,0U},
  {3U,2981U,238U,0U},
  {3U,1518U,4164U,0U},
  {3U,4164U,1518U,0U},
  {3U,53U,1567U,0U},
  {3U,1567U,53U,0U},
  {3U,4045U,4046U,0U},
  {3U,4046U,4045U,0U},
  {3U,2266U,3478U,0U},
  {3U,3478U,2266U,0U},
  {3U,991U,3548U,0U},
  {3U,3548U,991U,0U},
  {3U,119U,1091U,0U},
  {3U,1091U,119U,0U},
  {3U,3007U,4742U,0U},
  {3U,4742U,3007U,0U},
  {3U,4986U,5179U,0U},
  {3U,5179U,4986U,0U},
  {3U,59U,779U,0U},
  {3U,779U,59U,0U},
  {3U,1030U,2119U,0U},
  {3U,2119U,1030U,0U},
  {3U,98U,1467U,0U},
  {3U,1467U,98U,0U},
  {3U,325U,592U,0U},
  {3U,592U,325U,0U},
  {3U,1966U,2241U,0U},
  {3U,2241U,1966U,0U},
  {3U,258U,265U,0U},
  {3U,265U,258U,0U},
  {3U,4314U,4413U,0U},
  {3U,4413U,4314U,0U},
  {3U,4530U,5263U,0U},
  {3U,5263U,4530U,0U},
  {3U,285U,2450U,0U},
  {3U,2450U,285U,0U},
  {3U,4976U,5028U,0U},
  {3U,5028U,4976U,0U},
  {3U,4880U,5323U,0U},
  {3U,5323U,4880U,0U},
  {3U,759U,3391U,0U},
  {3U,3391U,759U,0U},
  {3U,718U,3752U,0U},
  {3U,3752U,718U,0U},
  {3U,150U,2562U,0U},
  {3U,2562U,150U,0U},
  {3U,2920U,3171U,0U},
  {3U,3171U,2920U,0U},
  {3U,2106U,4686U,0U},
  {3U,4686U,2106U,0U},
  {3U,1976U,3714U,0U},
  {3U,3714U,1976U,0U},
  {3U,975U,3534U,0U},
  {3U,3534U,975U,0U},
  {3U,3674U,4689U,0U},
  {3U,4689U,3674U,0U},
  {3U,677U,3740U,0U},
  {3U,3740U,677U,0U},
  {3U,361U,1863U,0U},
  {3U,1863U,361U,0U},
  {3U,269U,1834U,0U},
  {3U,1834U,269U,0U},
  {3U,1921U,2738U,0U},
  {3U,2738U,1921U,0U},
  {3U,4559U,4560U,0U},
  {3U,4560U,4559U,0U},
  {3U,441U,3573U,0U},
  {3U,3573U,441U,0U},
  {3U,817U,1230U,0U},
  {3U,1230U,817U,0U},
  {3U,3620U,3914U,0U},
  {3U,3914U,3620U,0U},
  {3U,4114U,4586U,0U},
  {3U,4586U,4114U,0U},
  {3U,572U,871U,0U},
  {3U,871U,572U,0U},
  {3U,615U,716U,0U},
  {3U,716U,615U,0U},
  {3U,1653U,4439U,0U},
  {3U,4439U,1653U,0U},
  {3U,2315U,4725U,0U},
  {3U,4725U,2315U,0U},
  {3U,562U,849U,0U},
  {3U,849U,562U,0U},
  {3U,3885U,4156U,0U},
  {3U,4156U,3885U,0U},
  {3U,142U,2030U,0U},
  {3U,2030U,142U,0U},
  {3U,730U,737U,0U},
  {3U,737U,730U,0U},
  {3U,2594U,2603U,0U},
  {3U,2603U,2594U,0U},
  {3U,606U,1782U,0U},
  {3U,1782U,606U,0U},
  {3U,18U,1052U,0U},
  {3U,1052U,18U,0U},
  {3U,4192U,5159U,0U},
  {3U,5159U,4192U,0U},
  {3U,4251U,4252U,0U},
  {3U,4252U,4251U,0U},
  {3U,289U,3182U,0U},
  {3U,3182U,289U,0U},
  {3U,3546U,4424U,0U},
  {3U,4424U,3546U,0U},
  {3U,1115U,2170U,0U},
  {3U,2170U,1115U,0U},
  {3U,975U,3828U,0U},
  {3U,3828U,975U,0U},
  {3U,253U,846U,0U},
  {3U,846U,253U,0U},
  {3U,2184U,3218U,0U},
  {3U,3218U,2184U,0U},
  {3U,186U,1692U,0U},
  {3U,1692U,186U,0U},
  {3U,4088U,4151U,0U},
  {3U,4151U,4088U,0U},
  {3U,258U,1672U,0U},
  {3U,1672U,258U,0U},
  {3U,1542U,3058U,0U},
  {3U,3058U,1542U,0U},
  {3U,382U,992U,0U},
  {3U,992U,382U,0U},
  {3U,145U,438U,0U},
  {3U,438U,145U,0U},
  {3U,3448U,3610U,0U},
  {3U,3610U,3448U,0U},
  {3U,3000U,4923U,0U},
  {3U,4923U,3000U,0U},
  {3U,320U,1411U,0U},
  {3U,1411U,320U,0U},
  {3U,53U,306U,0U},
  {3U,306U,53U,0U},
  {3U,1637U,3522U,0U},
  {3U,3522U,1637U,0U},
  {3U,262U,1020U,0U},
  {3U,1020U,262U,0U},
  {3U,1628U,3024U,0U},
  {3U,3024U,1628U,0U},
  {3U,507U,516U,0U},
  {3U,516U,507U,0U},
  {3U,120U,839U,0U},
  {3U,839U,120U,0U},
  {3U,4620U,5280U,0U},
  {3U,5280U,4620U,0U},
  {3U,55U,1800U,0U},
  {3U,1800U,55U,0U},
  {3U,2187U,2416U,0U},
  {3U,2416U,2187U,0U},
  {3U,4204U,4897U,0U},
  {3U,4897U,4204U,0U},
  {3U,46U,1393U,0U},
  {3U,1393U,46U,0U},
  {3U,3362U,4039U,0U},
  {3U,4039U,3362U,0U},
  {3U,86U,1974U,0U},
  {3U,1974U,86U,0U},
  {3U,2066U,3304U,0U},
  {3U,3304U,2066U,0U},
  {3U,215U,304U,0U},
  {3U,304U,215U,0U},
  {3U,4449U,4638U,0U},
  {3U,4638U,4449U,0U},
  {3U,3428U,3535U,0U},
  {3U,3535U,3428U,0U},
  {3U,336U,2201U,0U},
  {3U,2201U,336U,0U},
  {3U,2087U,3406U,0U},
  {3U,3406U,2087U,0U},
  {3U,224U,2577U,0U},
  {3U,2577U,224U,0U},
  {3U,98U,2022U,0U},
  {3U,2022U,98U,0U},
  {3U,5066U,5103U,0U},
  {3U,5103U,5066U,0U},
  {3U,4131U,4158U,0U},
  {3U,4158U,4131U,0U},
  {3U,1079U,3513U,0U},
  {3U,3513U,1079U,0U},
  {3U,611U,2470U,0U},
  {3U,2470U,611U,0U},
  {3U,720U,1457U,0U},
  {3U,1457U,720U,0U},
  {3U,1272U,3343U,0U},
  {3U,3343U,1272U,0U},
  {3U,1235U,1822U,0U},
  {3U,1822U,1235U,0U},
  {3U,517U,771U,0U},
  {3U,771U,517U,0U},
  {3U,302U,3271U,0U},
  {3U,3271U,302U,0U},
  {3U,1594U,3851U,0U},
  {3U,3851U,1594U,0U},
  {3U,4914U,5079U,0U},
  {3U,5079U,4914U,0U},
  {3U,1351U,3665U,0U},
  {3U,3665U,1351U,0U},
  {3U,53U,1085U,0U},
  {3U,1085U,53U,0U},
  {3U,373U,3459U,0U},
  {3U,3459U,373U,0U},
  {3U,592U,3321U,0U},
  {3U,3321U,592U,0U},
  {3U,2388U,3906U,0U},
  {3U,3906U,2388U,0U},
  {3U,3297U,4545U,0U},
  {3U,4545U,3297U,0U},
  {3U,3013U,4928U,0U},
  {3U,4928U,3013U,0U},
  {3U,3398U,4573U,0U},
  {3U,4573U,3398U,0U},
  {3U,3965U,4358U,0U},
  {3U,4358U,3965U,0U},
  {3U,2965U,3657U,0U},
  {3U,3657U,2965U,0U},
  {3U,247U,2843U,0U},
  {3U,2843U,247U,0U},
  {3U,5027U,5342U,0U},
  {3U,5342U,5027U,0U},
  {3U,4224U,4791U,0U},
  {3U,4791U,4224U,0U},
  {3U,953U,2020U,0U},
  {3U,2020U,953U,0U},
  {3U,3435U,3815U,0U},
  {3U,3815U,3435U,0U},
  {3U,1985U,2899U,0U},
  {3U,2899U,1985U,0U},
  {3U,3688U,4961U,0U},
  {3U,4961U,3688U,0U},
  {3U,5173U,5322U,0U},
  {3U,5322U,5173U,0U},
  {3U,3153U,4816U,0U},
  {3U,4816U,3153U,0U},
  {3U,2525U,2557U,0U},
  {3U,2557U,2525U,0U},
  {3U,919U,920U,0U},
  {3U,920U,919U,0U},
  {3U,537U,869U,0U},
  {3U,869U,537U,0U},
  {3U,2352U,2953U,0U},
  {3U,2953U,2352U,0U},
  {3U,987U,3126U,0U},
  {3U,3126U,987U,0U},
  {3U,2573U,3441U,0U},
  {3U,3441U,2573U,0U},
  {3U,2727U,2742U,0U},
  {3U,2742U,2727U,0U},
  {3U,549U,858U,0U},
  {3U,858U,549U,0U},
  {3U,837U,2698U,0U},
  {3U,2698U,837U,0U},
  {3U,132U,957U,0U},
  {3U,957U,132U,0U},
  {3U,380U,2175U,0U},
  {3U,2175U,380U,0U},
  {3U,359U,2098U,0U},
  {3U,2098U,359U,0U},
  {3U,1265U,1731U,0U},
  {3U,1731U,1265U,0U},
  {3U,2U,384U,0U},
  {3U,384U,2U,0U},
  {3U,4250U,5194U,0U},
  {3U,5194U,4250U,0U},
  {3U,2912U,4137U,0U},
  {3U,4137U,2912U,0U},
  {3U,222U,2940U,0U},
  {3U,2940U,222U,0U},
  {3U,658U,3556U,0U},
  {3U,3556U,658U,0U},
  {3U,1436U,3981U,0U},
  {3U,3981U,1436U,0U},
  {3U,344U,1813U,0U},
  {3U,1813U,344U,0U},
  {3U,1802U,4590U,0U},
  {3U,4590U,1802U,0U},
  {3U,1208U,1306U,0U},
  {3U,1306U,1208U,0U},
  {3U,2961U,4911U,0U},
  {3U,4911U,2961U,0U},
  {3U,3777U,3927U,0U},
  {3U,3927U,3777U,0U},
  {3U,1554U,4189U,0U},
  {3U,4189U,1554U,0U},
  {3U,4371U,4372U,0U},
  {3U,4372U,4371U,0U},
  {3U,1939U,3803U,0U},
  {3U,3803U,1939U,0U},
  {3U,317U,517U,0U},
  {3U,517U,317U,0U},
  {3U,1576U,4256U,0U},
  {3U,4256U,1576U,0U},
  {3U,1665U,4134U,0U},
  {3U,4134U,1665U,0U},
  {3U,3812U,5104U,0U},
  {3U,5104U,3812U,0U},
  {3U,1542U,3062U,0U},
  {3U,3062U,1542U,0U},
  {3U,844U,1096U,0U},
  {3U,1096U,844U,0U},
  {3U,4811U,5309U,0U},
  {3U,5309U,4811U,0U},
  {3U,2182U,3141U,0U},
  {3U,3141U,2182U,0U},
  {3U,2656U,3475U,0U},
  {3U,3475U,2656U,0U},
  {3U,3712U,5078U,0U},
  {3U,5078U,3712U,0U},
  {3U,53U,1783U,0U},
  {3U,1783U,53U,0U},
  {3U,2198U,2349U,0U},
  {3U,2349U,2198U,0U},
  {3U,553U,1127U,0U},
  {3U,1127U,553U,0U},
  {3U,104U,2161U,0U},
  {3U,2161U,104U,0U},
  {3U,1753U,4553U,0U},
  {3U,4553U,1753U,0U},
  {3U,54U,1173U,0U},
  {3U,1173U,54U,0U},
  {3U,1238U,2320U,0U},
  {3U,2320U,1238U,0U},
  {3U,53U,1690U,0U},
  {3U,1690U,53U,0U},
  {3U,112U,1485U,0U},
  {3U,1485U,112U,0U},
  {3U,3555U,4452U,0U},
  {3U,4452U,3555U,0U},
  {3U,3997U,4989U,0U},
  {3U,4989U,3997U,0U},
  {3U,3945U,5112U,0U},
  {3U,5112U,3945U,0U},
  {3U,1095U,3925U,0U},
  {3U,3925U,1095U,0U},
  {3U,178U,2688U,0U},
  {3U,2688U,178U,0U},
  {3U,4210U,4861U,0U},
  {3U,4861U,4210U,0U},
  {3U,4404U,4875U,0U},
  {3U,4875U,4404U,0U},
  {3U,1706U,1716U,0U},
  {3U,1716U,1706U,0U},
  {3U,148U,2550U,0U},
  {3U,2550U,148U,0U},
  {3U,276U,2710U,0U},
  {3U,2710U,276U,0U},
  {3U,1625U,4352U,0U},
  {3U,4352U,1625U,0U},
  {3U,1198U,2311U,0U},
  {3U,2311U,1198U,0U},
  {3U,631U,1148U,0U},
  {3U,1148U,631U,0U},
  {3U,31U,1209U,0U},
  {3U,1209U,31U,0U},
  {3U,3208U,3269U,0U},
  {3U,3269U,3208U,0U},
  {3U,2488U,3301U,0U},
  {3U,3301U,2488U,0U},
  {3U,16U,967U,0U},
  {3U,967U,16U,0U},
  {3U,2629U,2632U,0U},
  {3U,2632U,2629U,0U},
  {3U,2300U,3794U,0U},
  {3U,3794U,2300U,0U},
  {3U,3651U,3652U,0U},
  {3U,3652U,3651U,0U},
  {3U,218U,2277U,0U},
  {3U,2277U,218U,0U},
  {3U,479U,2419U,0U},
  {3U,2419U,479U,0U},
  {3U,3302U,3307U,0U},
  {3U,3307U,3302U,0U},
  {3U,76U,1893U,0U},
  {3U,1893U,76U,0U},
  {3U,4772U,5302U,0U},
  {3U,5302U,4772U,0U},
  {3U,4862U,5320U,0U},
  {3U,5320U,4862U,0U},
  {3U,867U,3809U,0U},
  {3U,3809U,867U,0U},
  {3U,104U,1167U,0U},
  {3U,1167U,104U,0U},
  {3U,3993U,4211U,0U},
  {3U,4211U,3993U,0U},
  {3U,2071U,2092U,0U},
  {3U,2092U,2071U,0U},
  {3U,2452U,4771U,0U},
  {3U,4771U,2452U,0U},
  {3U,1627U,3289U,0U},
  {3U,3289U,1627U,0U},
  {3U,2U,387U,0U},
  {3U,387U,2U,0U},
  {3U,2481U,2482U,0U},
  {3U,2482U,2481U,0U},
  {3U,4229U,5186U,0U},
  {3U,5186U,4229U,0U},
  {3U,3093U,3710U,0U},
  {3U,3710U,3093U,0U},
  {3U,573U,599U,0U},
  {3U,599U,573U,0U},
  {3U,1324U,2246U,0U},
  {3U,2246U,1324U,0U},
  {3U,2269U,2679U,0U},
  {3U,2679U,2269U,0U},
  {3U,52U,1497U,0U},
  {3U,1497U,52U,0U},
  {3U,704U,5062U,0U},
  {3U,5062U,704U,0U},
  {3U,2838U,3526U,0U},
  {3U,3526U,2838U,0U},
  {3U,2117U,2333U,0U},
  {3U,2333U,2117U,0U},
  {3U,1117U,2704U,0U},
  {3U,2704U,1117U,0U},
  {3U,3465U,4369U,0U},
  {3U,4369U,3465U,0U},
  {3U,1161U,3952U,0U},
  {3U,3952U,1161U,0U},
  {3U,4929U,4978U,0U},
  {3U,4978U,4929U,0U},
  {3U,2534U,3560U,0U},
  {3U,3560U,2534U,0U},
  {3U,2842U,4642U,0U},
  {3U,4642U,2842U,0U},
  {3U,1143U,2065U,0U},
  {3U,2065U,1143U,0U},
  {3U,143U,2296U,0U},
  {3U,2296U,143U,0U},
  {3U,401U,3195U,0U},
  {3U,3195U,401U,0U},
  {3U,3138U,4692U,0U},
  {3U,4692U,3138U,0U},
  {3U,127U,504U,0U},
  {3U,504U,127U,0U},
  {3U,1100U,3931U,0U},
  {3U,3931U,1100U,0U},
  {3U,3205U,4116U,0U},
  {3U,4116U,3205U,0U},
  {3U,604U,1282U,0U},
  {3U,1282U,604U,0U},
  {3U,984U,1247U,0U},
  {3U,1247U,984U,0U},
  {3U,4472U,5071U,0U},
  {3U,5071U,4472U,0U},
  {3U,4238U,5183U,0U},
  {3U,5183U,4238U,0U},
  {3U,5209U,5210U,0U},
  {3U,5210U,5209U,0U},
  {3U,4558U,5161U,0U},
  {3U,5161U,4558U,0U},
  {3U,881U,936U,0U},
  {3U,936U,881U,0U},
  {3U,367U,2377U,0U},
  {3U,2377U,367U,0U},
  {3U,4382U,5236U,0U},
  {3U,5236U,4382U,0U},
  {3U,3497U,4097U,0U},
  {3U,4097U,3497U,0U},
  {3U,3632U,4128U,0U},
  {3U,4128U,3632U,0U},
  {3U,300U,1273U,0U},
  {3U,1273U,300U,0U},
  {3U,2U,334U,0U},
  {3U,334U,2U,0U},
  {3U,333U,959U,0U},
  {3U,959U,333U,0U},
  {3U,10U,679U,0U},
  {3U,679U,10U,0U},
  {3U,2423U,4765U,0U},
  {3U,4765U,2423U,0U},
  {3U,1008U,1441U,0U},
  {3U,1441U,1008U,0U},
  {3U,1096U,2372U,0U},
  {3U,2372U,1096U,0U},
  {3U,1685U,4482U,0U},
  {3U,4482U,1685U,0U},
  {3U,2561U,3149U,0U},
  {3U,3149U,2561U,0U},
  {3U,2317U,2350U,0U},
  {3U,2350U,2317U,0U},
  {3U,2084U,2560U,0U},
  {3U,2560U,2084U,0U},
  {3U,3721U,4905U,0U},
  {3U,4905U,3721U,0U},
  {3U,1398U,2101U,0U},
  {3U,2101U,1398U,0U},
  {3U,442U,1883U,0U},
  {3U,1883U,442U,0U},
  {3U,363U,1094U,0U},
  {3U,1094U,363U,0U},
  {3U,3370U,3806U,0U},
  {3U,3806U,3370U,0U},
  {3U,2104U,2143U,0U},
  {3U,2143U,2104U,0U},
  {3U,124U,1426U,0U},
  {3U,1426U,124U,0U},
  {3U,546U,1144U,0U},
  {3U,1144U,546U,0U},
  {3U,4831U,4845U,0U},
  {3U,4845U,4831U,0U},
  {3U,4990U,5336U,0U},
  {3U,5336U,4990U,0U},
  {3U,1275U,3547U,0U},
  {3U,3547U,1275U,0U},
  {3U,53U,1558U,0U},
  {3U,1558U,53U,0U},
  {3U,3887U,4726U,0U},
  {3U,4726U,3887U,0U},
  {3U,336U,3386U,0U},
  {3U,3386U,336U,0U},
  {3U,927U,3319U,0U},
  {3U,3319U,927U,0U},
  {3U,95U,809U,0U},
  {3U,809U,95U,0U},
  {3U,247U,462U,0U},
  {3U,462U,247U,0U},
  {3U,4292U,4616U,0U},
  {3U,4616U,4292U,0U},
  {3U,40U,1320U,0U},
  {3U,1320U,40U,0U},
  {3U,778U,840U,0U},
  {3U,840U,778U,0U},
  {3U,53U,389U,0U},
  {3U,389U,53U,0U},
  {3U,2515U,2726U,0U},
  {3U,2726U,2515U,0U},
  {3U,777U,3139U,0U},
  {3U,3139U,777U,0U},
  {3U,1667U,2652U,0U},
  {3U,2652U,1667U,0U},
  {3U,1041U,1845U,0U},
  {3U,1845U,1041U,0U},
  {3U,1074U,1564U,0U},
  {3U,1564U,1074U,0U},
  {3U,3545U,4545U,0U},
  {3U,4545U,3545U,0U},
  {3U,803U,887U,0U},
  {3U,887U,803U,0U},
  {3U,274U,1240U,0U},
  {3U,1240U,274U,0U},
  {3U,3606U,3607U,0U},
  {3U,3607U,3606U,0U},
  {3U,1807U,2385U,0U},
  {3U,2385U,1807U,0U},
  {3U,504U,628U,0U},
  {3U,628U,504U,0U},
  {3U,4047U,4904U,0U},
  {3U,4904U,4047U,0U},
  {3U,2051U,4659U,0U},
  {3U,4659U,2051U,0U},
  {3U,71U,4631U,0U},
  {3U,4631U,71U,0U},
  {3U,298U,3258U,0U},
  {3U,3258U,298U,0U},
  {3U,6U,132U,0U},
  {3U,132U,6U,0U},
  {3U,3866U,5111U,0U},
  {3U,5111U,3866U,0U},
  {3U,271U,3132U,0U},
  {3U,3132U,271U,0U},
  {3U,1460U,3703U,0U},
  {3U,3703U,1460U,0U},
  {3U,971U,2524U,0U},
  {3U,2524U,971U,0U},
  {3U,4650U,5108U,0U},
  {3U,5108U,4650U,0U},
  {3U,4258U,4872U,0U},
  {3U,4872U,4258U,0U},
  {3U,4363U,4557U,0U},
  {3U,4557U,4363U,0U},
  {3U,62U,64U,0U},
  {3U,64U,62U,0U},
  {3U,4290U,4300U,0U},
  {3U,4300U,4290U,0U},
  {3U,260U,3065U,0U},
  {3U,3065U,260U,0U},
  {3U,4551U,5082U,0U},
  {3U,5082U,4551U,0U},
  {3U,331U,2773U,0U},
  {3U,2773U,331U,0U},
  {3U,586U,3104U,0U},
  {3U,3104U,586U,0U},
  {3U,35U,772U,0U},
  {3U,772U,35U,0U},
  {3U,1789U,2313U,0U},
  {3U,2313U,1789U,0U},
  {3U,5212U,5248U,0U},
  {3U,5248U,5212U,0U},
  {3U,2993U,2995U,0U},
  {3U,2995U,2993U,0U},
  {3U,18U,129U,0U},
  {3U,129U,18U,0U},
  {3U,23U,1113U,0U},
  {3U,1113U,23U,0U},
  {3U,2197U,2737U,0U},
  {3U,2737U,2197U,0U},
  {3U,2865U,3542U,0U},
  {3U,3542U,2865U,0U},
  {3U,573U,2414U,0U},
  {3U,2414U,573U,0U},
  {3U,1624U,4348U,0U},
  {3U,4348U,1624U,0U},
  {3U,2945U,4908U,0U},
  {3U,4908U,2945U,0U},
  {3U,94U,1849U,0U},
  {3U,1849U,94U,0U},
  {3U,1156U,1432U,0U},
  {3U,1432U,1156U,0U},
  {3U,4628U,5282U,0U},
  {3U,5282U,4628U,0U},
  {3U,1796U,2257U,0U},
  {3U,2257U,1796U,0U},
  {3U,4026U,4072U,0U},
  {3U,4072U,4026U,0U},
  {3U,4694U,5290U,0U},
  {3U,5290U,4694U,0U},
  {3U,281U,696U,0U},
  {3U,696U,281U,0U},
  {3U,816U,3725U,0U},
  {3U,3725U,816U,0U},
  {3U,2145U,3645U,0U},
  {3U,3645U,2145U,0U},
  {3U,542U,858U,0U},
  {3U,858U,542U,0U},
  {3U,639U,842U,0U},
  {3U,842U,639U,0U},
  {3U,3342U,4062U,0U},
  {3U,4062U,3342U,0U},
  {3U,4656U,5287U,0U},
  {3U,5287U,4656U,0U},
  {3U,2897U,4888U,0U},
  {3U,4888U,2897U,0U},
  {3U,155U,2593U,0U},
  {3U,2593U,155U,0U},
  {3U,124U,1189U,0U},
  {3U,1189U,124U,0U},
  {3U,4454U,5164U,0U},
  {3U,5164U,4454U,0U},
  {3U,4775U,5050U,0U},
  {3U,5050U,4775U,0U},
  {3U,463U,3146U,0U},
  {3U,3146U,463U,0U},
  {3U,2706U,3669U,0U},
  {3U,3669U,2706U,0U},
  {3U,185U,1155U,0U},
  {3U,1155U,185U,0U},
  {3U,2590U,4625U,0U},
  {3U,4625U,2590U,0U},
  {3U,1698U,3471U,0U},
  {3U,3471U,1698U,0U},
  {3U,364U,1458U,0U},
  {3U,1458U,364U,0U},
  {3U,4972U,4973U,0U},
  {3U,4973U,4972U,0U},
  {3U,1311U,2663U,0U},
  {3U,2663U,1311U,0U},
  {3U,3694U,5031U,0U},
  {3U,5031U,3694U,0U},
  {3U,3033U,4039U,0U},
  {3U,4039U,3033U,0U},
  {3U,1973U,3655U,0U},
  {3U,3655U,1973U,0U},
  {3U,3010U,4927U,0U},
  {3U,4927U,3010U,0U},
  {3U,2384U,3305U,0U},
  {3U,3305U,2384U,0U},
  {3U,87U,1979U,0U},
  {3U,1979U,87U,0U},
  {3U,4855U,5114U,0U},
  {3U,5114U,4855U,0U},
  {3U,2743U,3403U,0U},
  {3U,3403U,2743U,0U},
  {3U,450U,2610U,0U},
  {3U,2610U,450U,0U},
  {3U,176U,2682U,0U},
  {3U,2682U,176U,0U},
  {3U,714U,2412U,0U},
  {3U,2412U,714U,0U},
  {3U,880U,931U,0U},
  {3U,931U,880U,0U},
  {3U,2172U,3225U,0U},
  {3U,3225U,2172U,0U},
  {3U,1085U,2185U,0U},
  {3U,2185U,1085U,0U},
  {3U,105U,1846U,0U},
  {3U,1846U,105U,0U},
  {3U,1980U,4630U,0U},
  {3U,4630U,1980U,0U},
  {3U,544U,1419U,0U},
  {3U,1419U,544U,0U},
  {3U,848U,2369U,0U},
  {3U,2369U,848U,0U},
  {3U,1484U,1596U,0U},
  {3U,1596U,1484U,0U},
  {3U,2040U,4653U,0U},
  {3U,4653U,2040U,0U},
  {3U,1362U,4085U,0U},
  {3U,4085U,1362U,0U},
  {3U,2540U,3079U,0U},
  {3U,3079U,2540U,0U},
  {3U,1588U,4236U,0U},
  {3U,4236U,1588U,0U},
  {3U,1268U,1337U,0U},
  {3U,1337U,1268U,0U},
  {3U,1712U,4466U,0U},
  {3U,4466U,1712U,0U},
  {3U,2835U,3673U,0U},
  {3U,3673U,2835U,0U},
  {3U,1293U,2967U,0U},
  {3U,2967U,1293U,0U},
  {3U,270U,1063U,0U},
  {3U,1063U,270U,0U},
  {3U,444U,3575U,0U},
  {3U,3575U,444U,0U},
  {3U,577U,1899U,0U},
  {3U,1899U,577U,0U},
  {3U,3056U,3069U,0U},
  {3U,3069U,3056U,0U},
  {3U,1943U,4619U,0U},
  {3U,4619U,1943U,0U},
  {3U,4881U,4887U,0U},
  {3U,4887U,4881U,0U},
  {3U,5276U,5277U,0U},
  {3U,5277U,5276U,0U},
  {3U,4319U,5192U,0U},
  {3U,5192U,4319U,0U},
  {3U,1691U,4492U,0U},
  {3U,4492U,1691U,0U},
  {3U,3131U,4789U,0U},
  {3U,4789U,3131U,0U},
  {3U,100U,908U,0U},
  {3U,908U,100U,0U},
  {3U,225U,846U,0U},
  {3U,846U,225U,0U},
  {3U,2776U,3299U,0U},
  {3U,3299U,2776U,0U},
  {3U,2650U,3456U,0U},
  {3U,3456U,2650U,0U},
  {3U,2237U,2786U,0U},
  {3U,2786U,2237U,0U},
  {3U,2638U,2643U,0U},
  {3U,2643U,2638U,0U},
  {3U,4461U,5252U,0U},
  {3U,5252U,4461U,0U},
  {3U,3235U,4969U,0U},
  {3U,4969U,3235U,0U},
  {3U,312U,1956U,0U},
  {3U,1956U,312U,0U},
  {3U,2U,256U,0U},
  {3U,256U,2U,0U},
  {3U,668U,1081U,0U},
  {3U,1081U,668U,0U},
  {3U,1658U,4437U,0U},
  {3U,4437U,1658U,0U},
  {3U,522U,1811U,0U},
  {3U,1811U,522U,0U},
  {3U,2757U,4127U,0U},
  {3U,4127U,2757U,0U},
  {3U,4833U,4837U,0U},
  {3U,4837U,4833U,0U},
  {3U,963U,3011U,0U},
  {3U,3011U,963U,0U},
  {3U,425U,2192U,0U},
  {3U,2192U,425U,0U},
  {3U,746U,819U,0U},
  {3U,819U,746U,0U},
  {3U,93U,234U,0U},
  {3U,234U,93U,0U},
  {3U,3143U,3687U,0U},
  {3U,3687U,3143U,0U},
  {3U,2162U,2570U,0U},
  {3U,2570U,2162U,0U},
  {3U,1191U,2188U,0U},
  {3U,2188U,1191U,0U},
  {3U,4930U,4931U,0U},
  {3U,4931U,4930U,0U},
  {3U,947U,3821U,0U},
  {3U,3821U,947U,0U},
  {3U,2115U,2944U,0U},
  {3U,2944U,2115U,0U},
  {3U,1222U,1830U,0U},
  {3U,1830U,1222U,0U},
  {3U,1623U,4177U,0U},
  {3U,4177U,1623U,0U},
  {3U,1027U,3892U,0U},
  {3U,3892U,1027U,0U},
  {3U,2984U,4777U,0U},
  {3U,4777U,2984U,0U},
  {3U,491U,3450U,0U},
  {3U,3450U,491U,0U},
  {3U,2758U,3211U,0U},
  {3U,3211U,2758U,0U},
  {3U,5267U,5310U,0U},
  {3U,5310U,5267U,0U},
  {3U,70U,375U,0U},
  {3U,375U,70U,0U},
  {3U,2074U,4675U,0U},
  {3U,4675U,2074U,0U},
  {3U,1048U,2973U,0U},
  {3U,2973U,1048U,0U},
  {3U,248U,259U,0U},
  {3U,259U,248U,0U},
  {3U,564U,599U,0U},
  {3U,599U,564U,0U},
  {3U,2794U,4854U,0U},
  {3U,4854U,2794U,0U},
  {3U,1645U,4421U,0U},
  {3U,4421U,1645U,0U},
  {3U,3291U,4988U,0U},
  {3U,4988U,3291U,0U},
  {3U,3823U,4609U,0U},
  {3U,4609U,3823U,0U},
  {3U,3295U,4484U,0U},
  {3U,4484U,3295U,0U},
  {3U,2178U,3232U,0U},
  {3U,3232U,2178U,0U},
  {3U,115U,1234U,0U},
  {3U,1234U,115U,0U},
  {3U,203U,2807U,0U},
  {3U,2807U,203U,0U},
  {3U,1133U,1801U,0U},
  {3U,1801U,1133U,0U},
  {3U,172U,1717U,0U},
  {3U,1717U,172U,0U},
  {3U,1770U,4112U,0U},
  {3U,4112U,1770U,0U},
  {3U,4823U,4874U,0U},
  {3U,4874U,4823U,0U},
  {3U,329U,592U,0U},
  {3U,592U,329U,0U},
  {3U,4225U,5185U,0U},
  {3U,5185U,4225U,0U},
  {3U,2291U,3004U,0U},
  {3U,3004U,2291U,0U},
  {3U,2036U,3818U,0U},
  {3U,3818U,2036U,0U},
  {3U,4500U,5257U,0U},
  {3U,5257U,4500U,0U},
  {3U,1683U,2490U,0U},
  {3U,2490U,1683U,0U},
  {3U,1305U,1477U,0U},
  {3U,1477U,1305U,0U},
  {3U,3930U,4773U,0U},
  {3U,4773U,3930U,0U},
  {3U,381U,851U,0U},
  {3U,851U,381U,0U},
  {3U,675U,1202U,0U},
  {3U,1202U,675U,0U},
  {3U,767U,1064U,0U},
  {3U,1064U,767U,0U},
  {3U,1125U,3729U,0U},
  {3U,3729U,1125U,0U},
  {3U,943U,1073U,0U},
  {3U,1073U,943U,0U},
  {3U,1213U,2609U,0U},
  {3U,2609U,1213U,0U},
  {3U,71U,1988U,0U},
  {3U,1988U,71U,0U},
  {3U,974U,3700U,0U},
  {3U,3700U,974U,0U},
  {3U,4432U,4433U,0U},
  {3U,4433U,4432U,0U},
  {3U,225U,1204U,0U},
  {3U,1204U,225U,0U},
  {3U,3362U,3363U,0U},
  {3U,3363U,3362U,0U},
  {3U,2340U,3933U,0U},
  {3U,3933U,2340U,0U},
  {3U,2805U,2806U,0U},
  {3U,2806U,2805U,0U},
  {3U,1821U,2007U,0U},
  {3U,2007U,1821U,0U},
  {3U,120U,460U,0U},
  {3U,460U,120U,0U},
  {3U,4346U,4512U,0U},
  {3U,4512U,4346U,0U},
  {3U,3754U,3776U,0U},
  {3U,3776U,3754U,0U},
  {3U,88U,749U,0U},
  {3U,749U,88U,0U},
  {3U,2543U,3057U,0U},
  {3U,3057U,2543U,0U},
  {3U,43U,1323U,0U},
  {3U,1323U,43U,0U},
  {3U,4171U,4175U,0U},
  {3U,4175U,4171U,0U},
  {3U,705U,2062U,0U},
  {3U,2062U,705U,0U},
  {3U,2615U,4399U,0U},
  {3U,4399U,2615U,0U},
  {3U,799U,3781U,0U},
  {3U,3781U,799U,0U},
  {3U,4555U,5214U,0U},
  {3U,5214U,4555U,0U},
  {3U,1660U,4448U,0U},
  {3U,4448U,1660U,0U},
  {3U,1741U,2651U,0U},
  {3U,2651U,1741U,0U},
  {3U,1463U,3628U,0U},
  {3U,3628U,1463U,0U},
  {3U,4682U,5289U,0U},
  {3U,5289U,4682U,0U},
  {3U,2504U,3108U,0U},
  {3U,3108U,2504U,0U},
  {3U,2521U,3068U,0U},
  {3U,3068U,2521U,0U},
  {3U,2373U,4645U,0U},
  {3U,4645U,2373U,0U},
  {3U,1287U,2128U,0U},
  {3U,2128U,1287U,0U},
  {3U,53U,1673U,0U},
  {3U,1673U,53U,0U},
  {3U,814U,2254U,0U},
  {3U,2254U,814U,0U},
  {3U,1424U,3634U,0U},
  {3U,3634U,1424U,0U},
  {3U,603U,2752U,0U},
  {3U,2752U,603U,0U},
  {3U,1083U,3922U,0U},
  {3U,3922U,1083U,0U},
  {3U,1855U,3675U,0U},
  {3U,3675U,1855U,0U},
  {3U,4277U,5204U,0U},
  {3U,5204U,4277U,0U},
  {3U,1042U,3904U,0U},
  {3U,3904U,1042U,0U},
  {3U,2537U,3038U,0U},
  {3U,3038U,2537U,0U},
  {3U,419U,3359U,0U},
  {3U,3359U,419U,0U},
  {3U,751U,3767U,0U},
  {3U,3767U,751U,0U},
  {3U,1986U,3983U,0U},
  {3U,3983U,1986U,0U},
  {3U,4541U,5268U,0U},
  {3U,5268U,4541U,0U},
  {3U,3111U,4099U,0U},
  {3U,4099U,3111U,0U},
  {3U,266U,1912U,0U},
  {3U,1912U,266U,0U},
  {3U,3545U,4050U,0U},
  {3U,4050U,3545U,0U},
  {3U,1616U,4327U,0U},
  {3U,4327U,1616U,0U},
  {3U,183U,200U,0U},
  {3U,200U,183U,0U},
  {3U,538U,2118U,0U},
  {3U,2118U,538U,0U},
  {3U,1847U,3796U,0U},
  {3U,3796U,1847U,0U},
  {3U,1563U,1803U,0U},
  {3U,1803U,1563U,0U},
  {3U,247U,832U,0U},
  {3U,832U,247U,0U},
  {3U,1202U,2286U,0U},
  {3U,2286U,1202U,0U},
  {3U,2634U,2644U,0U},
  {3U,2644U,2634U,0U},
  {3U,425U,2281U,0U},
  {3U,2281U,425U,0U},
  {3U,2140U,2233U,0U},
  {3U,2233U,2140U,0U},
  {3U,3490U,3586U,0U},
  {3U,3586U,3490U,0U},
  {3U,4523U,5259U,0U},
  {3U,5259U,4523U,0U},
  {3U,1585U,4273U,0U},
  {3U,4273U,1585U,0U},
  {3U,3142U,3532U,0U},
  {3U,3532U,3142U,0U},
  {3U,53U,1681U,0U},
  {3U,1681U,53U,0U},
  {3U,1370U,4089U,0U},
  {3U,4089U,1370U,0U},
  {3U,4532U,4891U,0U},
  {3U,4891U,4532U,0U},
  {3U,4388U,4389U,0U},
  {3U,4389U,4388U,0U},
  {3U,71U,2150U,0U},
  {3U,2150U,71U,0U},
  {3U,1077U,2275U,0U},
  {3U,2275U,1077U,0U},
  {3U,3102U,4820U,0U},
  {3U,4820U,3102U,0U},
  {3U,2337U,3095U,0U},
  {3U,3095U,2337U,0U},
  {3U,492U,825U,0U},
  {3U,825U,492U,0U},
  {3U,2009U,4098U,0U},
  {3U,4098U,2009U,0U},
  {3U,3461U,3661U,0U},
  {3U,3661U,3461U,0U},
  {3U,4912U,4994U,0U},
  {3U,4994U,4912U,0U},
  {3U,2923U,4824U,0U},
  {3U,4824U,2923U,0U},
  {3U,3046U,3063U,0U},
  {3U,3063U,3046U,0U},
  {3U,2381U,3539U,0U},
  {3U,3539U,2381U,0U},
  {3U,3987U,5093U,0U},
  {3U,5093U,3987U,0U},
  {3U,667U,724U,0U},
  {3U,724U,667U,0U},
  {3U,357U,2223U,0U},
  {3U,2223U,357U,0U},
  {3U,5346U,5347U,0U},
  {3U,5347U,5346U,0U},
  {3U,48U,1470U,0U},
  {3U,1470U,48U,0U},
  {3U,2533U,3050U,0U},
  {3U,3050U,2533U,0U},
  {3U,2497U,4366U,0U},
  {3U,4366U,2497U,0U},
  {3U,11U,717U,0U},
  {3U,717U,11U,0U},
  {3U,5274U,5275U,0U},
  {3U,5275U,5274U,0U},
  {3U,2994U,4668U,0U},
  {3U,4668U,2994U,0U},
  {3U,351U,3413U,0U},
  {3U,3413U,351U,0U},
  {3U,2825U,4519U,0U},
  {3U,4519U,2825U,0U},
  {3U,2551U,4794U,0U},
  {3U,4794U,2551U,0U},
  {3U,728U,3608U,0U},
  {3U,3608U,728U,0U},
  {3U,1379U,4091U,0U},
  {3U,4091U,1379U,0U},
  {3U,2934U,3273U,0U},
  {3U,3273U,2934U,0U},
  {3U,71U,128U,0U},
  {3U,128U,71U,0U},
  {3U,1433U,1758U,0U},
  {3U,1758U,1433U,0U},
  {3U,827U,833U,0U},
  {3U,833U,827U,0U},
  {3U,4248U,5193U,0U},
  {3U,5193U,4248U,0U},
  {3U,1680U,4467U,0U},
  {3U,4467U,1680U,0U},
  {3U,164U,2553U,0U},
  {3U,2553U,164U,0U},
  {3U,3910U,4275U,0U},
  {3U,4275U,3910U,0U},
  {3U,2324U,3194U,0U},
  {3U,3194U,2324U,0U},
  {3U,325U,1952U,0U},
  {3U,1952U,325U,0U},
  {3U,272U,611U,0U},
  {3U,611U,272U,0U},
  {3U,3230U,4966U,0U},
  {3U,4966U,3230U,0U},
  {3U,3303U,3306U,0U},
  {3U,3306U,3303U,0U},
  {3U,2291U,3376U,0U},
  {3U,3376U,2291U,0U},
  {3U,1389U,2971U,0U},
  {3U,2971U,1389U,0U},
  {3U,1403U,3196U,0U},
  {3U,3196U,1403U,0U},
  {3U,2339U,3587U,0U},
  {3U,3587U,2339U,0U},
  {3U,293U,419U,0U},
  {3U,419U,293U,0U},
  {3U,3049U,4195U,0U},
  {3U,4195U,3049U,0U},
  {3U,2707U,4830U,0U},
  {3U,4830U,2707U,0U},
  {3U,3592U,4007U,0U},
  {3U,4007U,3592U,0U},
  {3U,4293U,5207U,0U},
  {3U,5207U,4293U,0U},
  {3U,552U,592U,0U},
  {3U,592U,552U,0U},
  {3U,1522U,2801U,0U},
  {3U,2801U,1522U,0U},
  {3U,2520U,3072U,0U},
  {3U,3072U,2520U,0U},
  {3U,3284U,3717U,0U},
  {3U,3717U,3284U,0U},
  {3U,1361U,1497U,0U},
  {3U,1497U,1361U,0U},
  {3U,166U,2627U,0U},
  {3U,2627U,166U,0U},
  {3U,261U,1416U,0U},
  {3U,1416U,261U,0U},
  {3U,1827U,2338U,0U},
  {3U,2338U,1827U,0U},
  {3U,1688U,1989U,0U},
  {3U,1989U,1688U,0U},
  {3U,3523U,5234U,0U},
  {3U,5234U,3523U,0U},
  {3U,2703U,3485U,0U},
  {3U,3485U,2703U,0U},
  {3U,4U,486U,0U},
  {3U,486U,4U,0U},
  {3U,146U,430U,0U},
  {3U,430U,146U,0U},
  {3U,26U,132U,0U},
  {3U,132U,26U,0U},
  {3U,4442U,5247U,0U},
  {3U,5247U,4442U,0U},
  {3U,1995U,3624U,0U},
  {3U,3624U,1995U,0U},
  {3U,1720U,1892U,0U},
  {3U,1892U,1720U,0U},
  {3U,3156U,3842U,0U},
  {3U,3842U,3156U,0U},
  {3U,3769U,5100U,0U},
  {3U,5100U,3769U,0U},
  {3U,1494U,2421U,0U},
  {3U,2421U,1494U,0U},
  {3U,1062U,1991U,0U},
  {3U,1991U,1062U,0U},
  {3U,1437U,3838U,0U},
  {3U,3838U,1437U,0U},
  {3U,2259U,3434U,0U},
  {3U,3434U,2259U,0U},
  {3U,418U,601U,0U},
  {3U,601U,418U,0U},
  {3U,120U,475U,0U},
  {3U,475U,120U,0U},
  {3U,4736U,5157U,0U},
  {3U,5157U,4736U,0U},
  {3U,527U,583U,0U},
  {3U,583U,527U,0U},
  {3U,426U,783U,0U},
  {3U,783U,426U,0U},
  {3U,143U,691U,0U},
  {3U,691U,143U,0U},
  {3U,664U,3367U,0U},
  {3U,3367U,664U,0U},
  {3U,1118U,1425U,0U},
  {3U,1425U,1118U,0U},
  {3U,4280U,4902U,0U},
  {3U,4902U,4280U,0U},
  {3U,1400U,1746U,0U},
  {3U,1746U,1400U,0U},
  {3U,843U,3327U,0U},
  {3U,3327U,843U,0U},
  {3U,907U,3130U,0U},
  {3U,3130U,907U,0U},
  {3U,2936U,3426U,0U},
  {3U,3426U,2936U,0U},
  {3U,3638U,3662U,0U},
  {3U,3662U,3638U,0U},
  {3U,3827U,3829U,0U},
  {3U,3829U,3827U,0U},
  {3U,414U,2363U,0U},
  {3U,2363U,414U,0U},
  {3U,538U,605U,0U},
  {3U,605U,538U,0U},
  {3U,221U,1409U,0U},
  {3U,1409U,221U,0U},
  {3U,3017U,3038U,0U},
  {3U,3038U,3017U,0U},
  {3U,52U,1532U,0U},
  {3U,1532U,52U,0U},
  {3U,1468U,1652U,0U},
  {3U,1652U,1468U,0U},
  {3U,5017U,5197U,0U},
  {3U,5197U,5017U,0U},
  {3U,250U,3019U,0U},
  {3U,3019U,250U,0U},
  {3U,801U,810U,0U},
  {3U,810U,801U,0U},
  {3U,1447U,1619U,0U},
  {3U,1619U,1447U,0U},
  {3U,1011U,3371U,0U},
  {3U,3371U,1011U,0U},
  {3U,4964U,5325U,0U},
  {3U,5325U,4964U,0U},
  {3U,1983U,4635U,0U},
  {3U,4635U,1983U,0U},
  {3U,3251U,4984U,0U},
  {3U,4984U,3251U,0U},
  {3U,2391U,2392U,0U},
  {3U,2392U,2391U,0U},
  {3U,1937U,1938U,0U},
  {3U,1938U,1937U,0U},
  {3U,1334U,1980U,0U},
  {3U,1980U,1334U,0U},
  {3U,2218U,3671U,0U},
  {3U,3671U,2218U,0U},
  {3U,4770U,4974U,0U},
  {3U,4974U,4770U,0U},
  {3U,678U,1373U,0U},
  {3U,1373U,678U,0U},
  {3U,1761U,4521U,0U},
  {3U,4521U,1761U,0U},
  {3U,1079U,2370U,0U},
  {3U,2370U,1079U,0U},
  {3U,3596U,3597U,0U},
  {3U,3597U,3596U,0U},
  {3U,66U,249U,0U},
  {3U,249U,66U,0U},
  {3U,909U,1114U,0U},
  {3U,1114U,909U,0U},
  {3U,815U,2076U,0U},
  {3U,2076U,815U,0U},
  {3U,4066U,4386U,0U},
  {3U,4386U,4066U,0U},
  {3U,3718U,4309U,0U},
  {3U,4309U,3718U,0U},
  {3U,1752U,4552U,0U},
  {3U,4552U,1752U,0U},
  {3U,175U,729U,0U},
  {3U,729U,175U,0U},
  {3U,2535U,2538U,0U},
  {3U,2538U,2535U,0U},
  {3U,2U,390U,0U},
  {3U,390U,2U,0U},
  {3U,3564U,5058U,0U},
  {3U,5058U,3564U,0U},
  {3U,508U,3637U,0U},
  {3U,3637U,508U,0U},
  {3U,2783U,3453U,0U},
  {3U,3453U,2783U,0U},
  {3U,71U,1039U,0U},
  {3U,1039U,71U,0U},
  {3U,4126U,4231U,0U},
  {3U,4231U,4126U,0U},
  {3U,160U,2617U,0U},
  {3U,2617U,160U,0U},
  {3U,2705U,3771U,0U},
  {3U,3771U,2705U,0U},
  {3U,75U,553U,0U},
  {3U,553U,75U,0U},
  {3U,2937U,3498U,0U},
  {3U,3498U,2937U,0U},
  {3U,714U,886U,0U},
  {3U,886U,714U,0U},
  {3U,2121U,2486U,0U},
  {3U,2486U,2121U,0U},
  {3U,630U,977U,0U},
  {3U,977U,630U,0U},
  {3U,705U,4759U,0U},
  {3U,4759U,705U,0U},
  {3U,1629U,4370U,0U},
  {3U,4370U,1629U,0U},
  {3U,186U,448U,0U},
  {3U,448U,186U,0U},
  {3U,405U,765U,0U},
  {3U,765U,405U,0U},
  {3U,619U,3698U,0U},
  {3U,3698U,619U,0U},
  {3U,4627U,5281U,0U},
  {3U,5281U,4627U,0U},
  {3U,1715U,4524U,0U},
  {3U,4524U,1715U,0U},
  {3U,722U,2255U,0U},
  {3U,2255U,722U,0U},
  {3U,4833U,4834U,0U},
  {3U,4834U,4833U,0U},
  {3U,4958U,5121U,0U},
  {3U,5121U,4958U,0U},
  {3U,166U,994U,0U},
  {3U,994U,166U,0U},
  {3U,1478U,4147U,0U},
  {3U,4147U,1478U,0U},
  {3U,284U,3031U,0U},
  {3U,3031U,284U,0U},
  {3U,216U,2857U,0U},
  {3U,2857U,216U,0U},
  {3U,2323U,4572U,0U},
  {3U,4572U,2323U,0U},
  {3U,3094U,4943U,0U},
  {3U,4943U,3094U,0U},
  {3U,104U,1969U,0U},
  {3U,1969U,104U,0U},
  {3U,3757U,5098U,0U},
  {3U,5098U,3757U,0U},
  {3U,505U,3636U,0U},
  {3U,3636U,505U,0U},
  {3U,1034U,2049U,0U},
  {3U,2049U,1034U,0U},
  {3U,1022U,2517U,0U},
  {3U,2517U,1022U,0U},
  {3U,1031U,3895U,0U},
  {3U,3895U,1031U,0U},
  {3U,906U,1793U,0U},
  {3U,1793U,906U,0U},
  {3U,3869U,4014U,0U},
  {3U,4014U,3869U,0U},
  {3U,672U,3736U,0U},
  {3U,3736U,672U,0U},
  {3U,4307U,5221U,0U},
  {3U,5221U,4307U,0U},
  {3U,1003U,2137U,0U},
  {3U,2137U,1003U,0U},
  {3U,4876U,4877U,0U},
  {3U,4877U,4876U,0U},
  {3U,226U,1719U,0U},
  {3U,1719U,226U,0U},
  {3U,2326U,2992U,0U},
  {3U,2992U,2326U,0U},
  {3U,588U,1040U,0U},
  {3U,1040U,588U,0U},
  {3U,2512U,2799U,0U},
  {3U,2799U,2512U,0U},
  {3U,4456U,5232U,0U},
  {3U,5232U,4456U,0U},
  {3U,71U,5364U,0U},
  {3U,5364U,71U,0U},
  {3U,1540U,2531U,0U},
  {3U,2531U,1540U,0U},
  {3U,1550U,1603U,0U},
  {3U,1603U,1550U,0U},
  {3U,218U,1177U,0U},
  {3U,1177U,218U,0U},
  {3U,1858U,1951U,0U},
  {3U,1951U,1858U,0U},
  {3U,72U,1065U,0U},
  {3U,1065U,72U,0U},
  {3U,970U,3853U,0U},
  {3U,3853U,970U,0U},
  {3U,71U,1815U,0U},
  {3U,1815U,71U,0U},
  {3U,686U,2411U,0U},
  {3U,2411U,686U,0U},
  {3U,61U,1057U,0U},
  {3U,1057U,61U,0U},
  {3U,1264U,1271U,0U},
  {3U,1271U,1264U,0U},
  {3U,70U,1879U,0U},
  {3U,1879U,70U,0U},
  {3U,461U,1655U,0U},
  {3U,1655U,461U,0U},
  {3U,1165U,1438U,0U},
  {3U,1438U,1165U,0U},
  {3U,143U,2476U,0U},
  {3U,2476U,143U,0U},
  {3U,423U,3552U,0U},
  {3U,3552U,423U,0U},
  {3U,4068U,4069U,0U},
  {3U,4069U,4068U,0U},
  {3U,2008U,2264U,0U},
  {3U,2264U,2008U,0U},
  {3U,5153U,5326U,0U},
  {3U,5326U,5153U,0U},
  {3U,482U,3402U,0U},
  {3U,3402U,482U,0U},
  {3U,362U,2810U,0U},
  {3U,2810U,362U,0U},
  {3U,720U,988U,0U},
  {3U,988U,720U,0U},
  {3U,2455U,3894U,0U},
  {3U,3894U,2455U,0U},
  {3U,2420U,4764U,0U},
  {3U,4764U,2420U,0U},
  {3U,3557U,5054U,0U},
  {3U,5054U,3557U,0U},
  {3U,3755U,5094U,0U},
  {3U,5094U,3755U,0U},
  {3U,57U,1618U,0U},
  {3U,1618U,57U,0U},
  {3U,922U,3836U,0U},
  {3U,3836U,922U,0U},
  {3U,4318U,4841U,0U},
  {3U,4841U,4318U,0U},
  {3U,213U,2195U,0U},
  {3U,2195U,213U,0U},
  {3U,3571U,4623U,0U},
  {3U,4623U,3571U,0U},
  {3U,981U,2766U,0U},
  {3U,2766U,981U,0U},
  {3U,4554U,4851U,0U},
  {3U,4851U,4554U,0U},
  {3U,99U,2113U,0U},
  {3U,2113U,99U,0U},
  {3U,3U,193U,0U},
  {3U,193U,3U,0U},
  {3U,350U,3412U,0U},
  {3U,3412U,350U,0U},
  {3U,1380U,1392U,0U},
  {3U,1392U,1380U,0U},
  {3U,4296U,5211U,0U},
  {3U,5211U,4296U,0U},
  {3U,1877U,4597U,0U},
  {3U,4597U,1877U,0U},
  {3U,1338U,3340U,0U},
  {3U,3340U,1338U,0U},
  {3U,42U,96U,0U},
  {3U,96U,42U,0U},
  {3U,629U,3711U,0U},
  {3U,3711U,629U,0U},
  {3U,2439U,3875U,0U},
  {3U,3875U,2439U,0U},
  {3U,26U,135U,0U},
  {3U,135U,26U,0U},
  {3U,934U,1319U,0U},
  {3U,1319U,934U,0U},
  {3U,2494U,2861U,0U},
  {3U,2861U,2494U,0U},
  {3U,1050U,1931U,0U},
  {3U,1931U,1050U,0U},
  {3U,2305U,2863U,0U},
  {3U,2863U,2305U,0U},
  {3U,2890U,4130U,0U},
  {3U,4130U,2890U,0U},
  {3U,3032U,3874U,0U},
  {3U,3874U,3032U,0U},
  {3U,2425U,4767U,0U},
  {3U,4767U,2425U,0U},
  {3U,517U,525U,0U},
  {3U,525U,517U,0U},
  {3U,252U,3022U,0U},
  {3U,3022U,252U,0U},
  {3U,2129U,4568U,0U},
  {3U,4568U,2129U,0U},
  {3U,1911U,2567U,0U},
  {3U,2567U,1911U,0U},
  {3U,4712U,5095U,0U},
  {3U,5095U,4712U,0U},
  {3U,4760U,4932U,0U},
  {3U,4932U,4760U,0U},
  {3U,1246U,2850U,0U},
  {3U,2850U,1246U,0U},
  {3U,859U,3473U,0U},
  {3U,3473U,859U,0U},
  {3U,2065U,4670U,0U},
  {3U,4670U,2065U,0U},
  {3U,3554U,3977U,0U},
  {3U,3977U,3554U,0U},
  {3U,1505U,2779U,0U},
  {3U,2779U,1505U,0U},
  {3U,1788U,1990U,0U},
  {3U,1990U,1788U,0U},
  {3U,2041U,3720U,0U},
  {3U,3720U,2041U,0U},
  {3U,1998U,4161U,0U},
  {3U,4161U,1998U,0U},
  {3U,666U,1051U,0U},
  {3U,1051U,666U,0U},
  {3U,4786U,4807U,0U},
  {3U,4807U,4786U,0U},
  {3U,107U,823U,0U},
  {3U,823U,107U,0U},
  {3U,1136U,1757U,0U},
  {3U,1757U,1136U,0U},
  {3U,2140U,2220U,0U},
  {3U,2220U,2140U,0U},
  {3U,3374U,5012U,0U},
  {3U,5012U,3374U,0U},
  {3U,3726U,3903U,0U},
  {3U,3903U,3726U,0U},
  {3U,1000U,3747U,0U},
  {3U,3747U,1000U,0U},
  {3U,1342U,1500U,0U},
  {3U,1500U,1342U,0U},
  {3U,5006U,5007U,0U},
  {3U,5007U,5006U,0U},
  {3U,949U,2297U,0U},
  {3U,2297U,949U,0U},
  {3U,2492U,3957U,0U},
  {3U,3957U,2492U,0U},
  {3U,1791U,1844U,0U},
  {3U,1844U,1791U,0U},
  {3U,353U,767U,0U},
  {3U,767U,353U,0U},
  {3U,143U,200U,0U},
  {3U,200U,143U,0U},
  {3U,2828U,3418U,0U},
  {3U,3418U,2828U,0U},
  {3U,4298U,4435U,0U},
  {3U,4435U,4298U,0U},
  {3U,1423U,1427U,0U},
  {3U,1427U,1423U,0U},
  {3U,2375U,3514U,0U},
  {3U,3514U,2375U,0U},
  {3U,4339U,4340U,0U},
  {3U,4340U,4339U,0U},
  {3U,53U,410U,0U},
  {3U,410U,53U,0U},
  {3U,5363U,5370U,0U},
  {3U,5370U,5363U,0U},
  {3U,2582U,3001U,0U},
  {3U,3001U,2582U,0U},
  {3U,186U,2236U,0U},
  {3U,2236U,186U,0U},
  {3U,1178U,3960U,0U},
  {3U,3960U,1178U,0U},
  {3U,2048U,2762U,0U},
  {3U,2762U,2048U,0U},
  {3U,238U,723U,0U},
  {3U,723U,238U,0U},
  {3U,1577U,4257U,0U},
  {3U,4257U,1577U,0U},
  {3U,204U,2701U,0U},
  {3U,2701U,204U,0U},
  {3U,2033U,2962U,0U},
  {3U,2962U,2033U,0U},
  {3U,2761U,4847U,0U},
  {3U,4847U,2761U,0U},
  {3U,321U,3266U,0U},
  {3U,3266U,321U,0U},
  {3U,978U,2186U,0U},
  {3U,2186U,978U,0U},
  {3U,4476U,5216U,0U},
  {3U,5216U,4476U,0U},
  {3U,98U,1919U,0U},
  {3U,1919U,98U,0U},
  {3U,1131U,1871U,0U},
  {3U,1871U,1131U,0U},
  {3U,1106U,3578U,0U},
  {3U,3578U,1106U,0U},
  {3U,71U,3326U,0U},
  {3U,3326U,71U,0U},
  {3U,2836U,3646U,0U},
  {3U,3646U,2836U,0U},
  {3U,1642U,1654U,0U},
  {3U,1654U,1642U,0U},
  {3U,420U,3245U,0U},
  {3U,3245U,420U,0U},
  {3U,3U,229U,0U},
  {3U,229U,3U,0U},
  {3U,138U,2284U,0U},
  {3U,2284U,138U,0U},
  {3U,940U,1420U,0U},
  {3U,1420U,940U,0U},
  {3U,1028U,1158U,0U},
  {3U,1158U,1028U,0U},
  {3U,380U,1471U,0U},
  {3U,1471U,380U,0U},
  {3U,1146U,3021U,0U},
  {3U,3021U,1146U,0U},
  {3U,2174U,4697U,0U},
  {3U,4697U,2174U,0U},
  {3U,2681U,2931U,0U},
  {3U,2931U,2681U,0U},
  {3U,830U,1168U,0U},
  {3U,1168U,830U,0U},
  {3U,955U,1386U,0U},
  {3U,1386U,955U,0U},
  {3U,2605U,4813U,0U},
  {3U,4813U,2605U,0U},
  {3U,4334U,4544U,0U},
  {3U,4544U,4334U,0U},
  {3U,1194U,3966U,0U},
  {3U,3966U,1194U,0U},
  {3U,285U,3164U,0U},
  {3U,3164U,285U,0U},
  {3U,138U,2428U,0U},
  {3U,2428U,138U,0U},
  {3U,325U,1018U,0U},
  {3U,1018U,325U,0U},
  {3U,2167U,2709U,0U},
  {3U,2709U,2167U,0U},
  {3U,2597U,3270U,0U},
  {3U,3270U,2597U,0U},
  {3U,4563U,5272U,0U},
  {3U,5272U,4563U,0U},
  {3U,3786U,4691U,0U},
  {3U,4691U,3786U,0U},
  {3U,1804U,4591U,0U},
  {3U,4591U,1804U,0U},
  {3U,1674U,1675U,0U},
  {3U,1675U,1674U,0U},
  {3U,3655U,3656U,0U},
  {3U,3656U,3655U,0U},
  {3U,1200U,3605U,0U},
  {3U,3605U,1200U,0U},
  {3U,4139U,4798U,0U},
  {3U,4798U,4139U,0U},
  {3U,3734U,5084U,0U},
  {3U,5084U,3734U,0U},
  {3U,139U,219U,0U},
  {3U,219U,139U,0U},
  {3U,3488U,3489U,0U},
  {3U,3489U,3488U,0U},
  {3U,858U,2179U,0U},
  {3U,2179U,858U,0U},
  {3U,124U,2322U,0U},
  {3U,2322U,124U,0U},
  {3U,1940U,4618U,0U},
  {3U,4618U,1940U,0U},
  {3U,4594U,4595U,0U},
  {3U,4595U,4594U,0U},
  {3U,271U,3129U,0U},
  {3U,3129U,271U,0U},
  {3U,3259U,3381U,0U},
  {3U,3381U,3259U,0U},
  {3U,4353U,4392U,0U},
  {3U,4392U,4353U,0U},
  {3U,3506U,5043U,0U},
  {3U,5043U,3506U,0U},
  {3U,296U,1339U,0U},
  {3U,1339U,296U,0U},
  {3U,3052U,4795U,0U},
  {3U,4795U,3052U,0U},
  {3U,2808U,3946U,0U},
  {3U,3946U,2808U,0U},
  {3U,2545U,3071U,0U},
  {3U,3071U,2545U,0U},
  {3U,1303U,4032U,0U},
  {3U,4032U,1303U,0U},
  {3U,3944U,4023U,0U},
  {3U,4023U,3944U,0U},
  {3U,862U,2440U,0U},
  {3U,2440U,862U,0U},
  {3U,2489U,2541U,0U},
  {3U,2541U,2489U,0U},
  {3U,937U,3279U,0U},
  {3U,3279U,937U,0U},
  {3U,4197U,4198U,0U},
  {3U,4198U,4197U,0U},
  {3U,1512U,1515U,0U},
  {3U,1515U,1512U,0U},
  {3U,3091U,3749U,0U},
  {3U,3749U,3091U,0U},
  {3U,506U,574U,0U},
  {3U,574U,506U,0U},
  {3U,4170U,4991U,0U},
  {3U,4991U,4170U,0U},
  {3U,2027U,4312U,0U},
  {3U,4312U,2027U,0U},
  {3U,4330U,5156U,0U},
  {3U,5156U,4330U,0U},
  {3U,580U,878U,0U},
  {3U,878U,580U,0U},
  {3U,3421U,3425U,0U},
  {3U,3425U,3421U,0U},
  {3U,1593U,4179U,0U},
  {3U,4179U,1593U,0U},
  {3U,2U,313U,0U},
  {3U,313U,2U,0U},
  {3U,3447U,3886U,0U},
  {3U,3886U,3447U,0U},
  {3U,354U,3416U,0U},
  {3U,3416U,354U,0U},
  {3U,5040U,5271U,0U},
  {3U,5271U,5040U,0U},
  {3U,5255U,5362U,0U},
  {3U,5362U,5255U,0U},
  {3U,88U,750U,0U},
  {3U,750U,88U,0U},
  {3U,0U,114U,0U},
  {3U,114U,0U,0U},
  {3U,1968U,2173U,0U},
  {3U,2173U,1968U,0U},
  {3U,196U,2789U,0U},
  {3U,2789U,196U,0U},
  {3U,2279U,3037U,0U},
  {3U,3037U,2279U,0U},
  {3U,2729U,2741U,0U},
  {3U,2741U,2729U,0U},
  {3U,3507U,3585U,0U},
  {3U,3585U,3507U,0U},
  {3U,1186U,1239U,0U},
  {3U,1239U,1186U,0U},
  {3U,865U,2316U,0U},
  {3U,2316U,865U,0U},
  {3U,2790U,2859U,0U},
  {3U,2859U,2790U,0U},
  {3U,1111U,3939U,0U},
  {3U,3939U,1111U,0U},
  {3U,3076U,3080U,0U},
  {3U,3080U,3076U,0U},
  {3U,3920U,3962U,0U},
  {3U,3962U,3920U,0U},
  {3U,2954U,2956U,0U},
  {3U,2956U,2954U,0U},
  {3U,3677U,3980U,0U},
  {3U,3980U,3677U,0U},
  {3U,2725U,3808U,0U},
  {3U,3808U,2725U,0U},
  {3U,3819U,5105U,0U},
  {3U,5105U,3819U,0U},
  {3U,267U,304U,0U},
  {3U,304U,267U,0U},
  {3U,930U,3593U,0U},
  {3U,3593U,930U,0U},
  {3U,1967U,2848U,0U},
  {3U,2848U,1967U,0U},
  {3U,261U,1291U,0U},
  {3U,1291U,261U,0U},
  {3U,1333U,4064U,0U},
  {3U,4064U,1333U,0U},
  {3U,2227U,3679U,0U},
  {3U,3679U,2227U,0U},
  {3U,71U,2950U,0U},
  {3U,2950U,71U,0U},
  {3U,3209U,4338U,0U},
  {3U,4338U,3209U,0U},
  {3U,580U,1321U,0U},
  {3U,1321U,580U,0U},
  {3U,1393U,3099U,0U},
  {3U,3099U,1393U,0U},
  {3U,3254U,3802U,0U},
  {3U,3802U,3254U,0U},
  {3U,4463U,5254U,0U},
  {3U,5254U,4463U,0U},
  {3U,3184U,4960U,0U},
  {3U,4960U,3184U,0U},
  {3U,4403U,5241U,0U},
  {3U,5241U,4403U,0U},
  {3U,1608U,5304U,0U},
  {3U,5304U,1608U,0U},
  {3U,1991U,3860U,0U},
  {3U,3860U,1991U,0U},
  {3U,211U,212U,0U},
  {3U,212U,211U,0U},
  {3U,1876U,2750U,0U},
  {3U,2750U,1876U,0U},
  {3U,715U,1843U,0U},
  {3U,1843U,715U,0U},
  {3U,3341U,4993U,0U},
  {3U,4993U,3341U,0U},
  {3U,1686U,3204U,0U},
  {3U,3204U,1686U,0U},
  {3U,1936U,3705U,0U},
  {3U,3705U,1936U,0U},
  {3U,2755U,3501U,0U},
  {3U,3501U,2755U,0U},
  {3U,3106U,3109U,0U},
  {3U,3109U,3106U,0U},
  {3U,5176U,5202U,0U},
  {3U,5202U,5176U,0U},
  {3U,1586U,4279U,0U},
  {3U,4279U,1586U,0U},
  {3U,4457U,4711U,0U},
  {3U,4711U,4457U,0U},
  {3U,71U,3008U,0U},
  {3U,3008U,71U,0U},
  {3U,2136U,2138U,0U},
  {3U,2138U,2136U,0U},
  {3U,1549U,4190U,0U},
  {3U,4190U,1549U,0U},
  {3U,1149U,1600U,0U},
  {3U,1600U,1149U,0U},
  {3U,488U,2314U,0U},
  {3U,2314U,488U,0U},
  {3U,1171U,3955U,0U},
  {3U,3955U,1171U,0U},
  {3U,2085U,4589U,0U},
  {3U,4589U,2085U,0U},
  {3U,152U,406U,0U},
  {3U,406U,152U,0U},
  {3U,3112U,3199U,0U},
  {3U,3199U,3112U,0U},
  {3U,1140U,3847U,0U},
  {3U,3847U,1140U,0U},
  {3U,4336U,4337U,0U},
  {3U,4337U,4336U,0U},
  {3U,4543U,5269U,0U},
  {3U,5269U,4543U,0U},
  {3U,592U,805U,0U},
  {3U,805U,592U,0U},
  {3U,4268U,5201U,0U},
  {3U,5201U,4268U,0U},
  {3U,503U,529U,0U},
  {3U,529U,503U,0U},
  {3U,1636U,1982U,0U},
  {3U,1982U,1636U,0U},
  {3U,2332U,3082U,0U},
  {3U,3082U,2332U,0U},
  {3U,3224U,3817U,0U},
  {3U,3817U,3224U,0U},
  {3U,226U,322U,0U},
  {3U,322U,226U,0U},
  {3U,4672U,4673U,0U},
  {3U,4673U,4672U,0U},
  {3U,4547U,5262U,0U},
  {3U,5262U,4547U,0U},
  {3U,1725U,2833U,0U},
  {3U,2833U,1725U,0U},
  {3U,2021U,3467U,0U},
  {3U,3467U,2021U,0U},
  {3U,1506U,1513U,0U},
  {3U,1513U,1506U,0U},
  {3U,2447U,2546U,0U},
  {3U,2546U,2447U,0U},
  {3U,1182U,1633U,0U},
  {3U,1633U,1182U,0U},
  {3U,1290U,3583U,0U},
  {3U,3583U,1290U,0U},
  {3U,2519U,3017U,0U},
  {3U,3017U,2519U,0U},
  {3U,1225U,3308U,0U},
  {3U,3308U,1225U,0U},
  {3U,261U,3035U,0U},
  {3U,3035U,261U,0U},
  {3U,1657U,3745U,0U},
  {3U,3745U,1657U,0U},
  {3U,2184U,3226U,0U},
  {3U,3226U,2184U,0U},
  {3U,1759U,4561U,0U},
  {3U,4561U,1759U,0U},
  {3U,1455U,4010U,0U},
  {3U,4010U,1455U,0U},
  {3U,4165U,5175U,0U},
  {3U,5175U,4165U,0U},
  {3U,4427U,4580U,0U},
  {3U,4580U,4427U,0U},
  {3U,1163U,3953U,0U},
  {3U,3953U,1163U,0U},
  {3U,191U,2612U,0U},
  {3U,2612U,191U,0U},
  {3U,1476U,4146U,0U},
  {3U,4146U,1476U,0U},
  {3U,191U,314U,0U},
  {3U,314U,191U,0U},
  {3U,1745U,3880U,0U},
  {3U,3880U,1745U,0U},
  {3U,1882U,2210U,0U},
  {3U,2210U,1882U,0U},
  {3U,1444U,2012U,0U},
  {3U,2012U,1444U,0U},
  {3U,4110U,4940U,0U},
  {3U,4940U,4110U,0U},
  {3U,401U,2280U,0U},
  {3U,2280U,401U,0U},
  {3U,3117U,3119U,0U},
  {3U,3119U,3117U,0U},
  {3U,2856U,3312U,0U},
  {3U,3312U,2856U,0U},
  {3U,2662U,3027U,0U},
  {3U,3027U,2662U,0U},
  {3U,1711U,3730U,0U},
  {3U,3730U,1711U,0U},
  {3U,53U,251U,0U},
  {3U,251U,53U,0U},
  {3U,682U,3155U,0U},
  {3U,3155U,682U,0U},
  {3U,1646U,3404U,0U},
  {3U,3404U,1646U,0U},
  {3U,2872U,3213U,0U},
  {3U,3213U,2872U,0U},
  {3U,3868U,4815U,0U},
  {3U,4815U,3868U,0U},
  {3U,3203U,4481U,0U},
  {3U,4481U,3203U,0U},
  {3U,1175U,2309U,0U},
  {3U,2309U,1175U,0U},
  {3U,4264U,4317U,0U},
  {3U,4317U,4264U,0U},
  {3U,3227U,3293U,0U},
  {3U,3293U,3227U,0U},
  {3U,13U,848U,0U},
  {3U,848U,13U,0U},
  {3U,4316U,4857U,0U},
  {3U,4857U,4316U,0U},
  {3U,1997U,2394U,0U},
  {3U,2394U,1997U,0U},
  {3U,200U,1703U,0U},
  {3U,1703U,200U,0U},
  {3U,515U,3647U,0U},
  {3U,3647U,515U,0U},
  {3U,1069U,2915U,0U},
  {3U,2915U,1069U,0U},
  {3U,71U,2239U,0U},
  {3U,2239U,71U,0U},
  {3U,38U,1301U,0U},
  {3U,1301U,38U,0U},
  {3U,380U,1431U,0U},
  {3U,1431U,380U,0U},
  {3U,2U,161U,0U},
  {3U,161U,2U,0U},
  {3U,340U,355U,0U},
  {3U,355U,340U,0U},
  {3U,3493U,4800U,0U},
  {3U,4800U,3493U,0U},
  {3U,15U,883U,0U},
  {3U,883U,15U,0U},
  {3U,2082U,4655U,0U},
  {3U,4655U,2082U,0U},
  {3U,3172U,3173U,0U},
  {3U,3173U,3172U,0U},
  {3U,4232U,5090U,0U},
  {3U,5090U,4232U,0U},
  {3U,4383U,4418U,0U},
  {3U,4418U,4383U,0U},
  {3U,4858U,4968U,0U},
  {3U,4968U,4858U,0U},
  {3U,747U,818U,0U},
  {3U,818U,747U,0U},
  {3U,2024U,2630U,0U},
  {3U,2630U,2024U,0U},
  {3U,2190U,3841U,0U},
  {3U,3841U,2190U,0U},
  {3U,3764U,5099U,0U},
  {3U,5099U,3764U,0U},
  {3U,4852U,5319U,0U},
  {3U,5319U,4852U,0U},
  {3U,4049U,4665U,0U},
  {3U,4665U,4049U,0U},
  {3U,2813U,2815U,0U},
  {3U,2815U,2813U,0U},
  {3U,263U,566U,0U},
  {3U,566U,263U,0U},
  {3U,4150U,4360U,0U},
  {3U,4360U,4150U,0U},
  {3U,722U,3756U,0U},
  {3U,3756U,722U,0U},
  {3U,4216U,4255U,0U},
  {3U,4255U,4216U,0U},
  {3U,4864U,5092U,0U},
  {3U,5092U,4864U,0U},
  {3U,231U,2396U,0U},
  {3U,2396U,231U,0U},
  {3U,5366U,5367U,0U},
  {3U,5367U,5366U,0U},
  {3U,4417U,5244U,0U},
  {3U,5244U,4417U,0U},
  {3U,2287U,2417U,0U},
  {3U,2417U,2287U,0U},
  {3U,512U,2196U,0U},
  {3U,2196U,512U,0U},
  {3U,488U,2226U,0U},
  {3U,2226U,488U,0U},
  {3U,3492U,4303U,0U},
  {3U,4303U,3492U,0U},
  {3U,3020U,3309U,0U},
  {3U,3309U,3020U,0U},
  {3U,292U,321U,0U},
  {3U,321U,292U,0U},
  {3U,614U,2267U,0U},
  {3U,2267U,614U,0U},
  {3U,1368U,1385U,0U},
  {3U,1385U,1368U,0U},
  {3U,1421U,1827U,0U},
  {3U,1827U,1421U,0U},
  {3U,501U,819U,0U},
  {3U,819U,501U,0U},
  {3U,3433U,3709U,0U},
  {3U,3709U,3433U,0U},
  {3U,386U,2475U,0U},
  {3U,2475U,386U,0U},
  {3U,2942U,3476U,0U},
  {3U,3476U,2942U,0U},
  {3U,3400U,3985U,0U},
  {3U,3985U,3400U,0U},
  {3U,424U,1587U,0U},
  {3U,1587U,424U,0U},
  {3U,5334U,5368U,0U},
  {3U,5368U,5334U,0U},
  {3U,65U,1850U,0U},
  {3U,1850U,65U,0U},
  {3U,142U,1970U,0U},
  {3U,1970U,142U,0U},
  {3U,1918U,3533U,0U},
  {3U,3533U,1918U,0U},
  {3U,2465U,4774U,0U},
  {3U,4774U,2465U,0U},
  {3U,3383U,5014U,0U},
  {3U,5014U,3383U,0U},
  {3U,301U,2589U,0U},
  {3U,2589U,301U,0U},
  {3U,2852U,3878U,0U},
  {3U,3878U,2852U,0U},
  {3U,388U,390U,0U},
  {3U,390U,388U,0U},
  {3U,1151U,2878U,0U},
  {3U,2878U,1151U,0U},
  {3U,165U,2624U,0U},
  {3U,2624U,165U,0U},
  {3U,368U,776U,0U},
  {3U,776U,368U,0U},
  {3U,2774U,3611U,0U},
  {3U,3611U,2774U,0U},
  {3U,3831U,5107U,0U},
  {3U,5107U,3831U,0U},
  {3U,640U,1930U,0U},
  {3U,1930U,640U,0U},
  {3U,701U,1215U,0U},
  {3U,1215U,701U,0U},
  {3U,301U,2374U,0U},
  {3U,2374U,301U,0U},
  {3U,1076U,2070U,0U},
  {3U,2070U,1076U,0U},
  {3U,2244U,4335U,0U},
  {3U,4335U,2244U,0U},
  {3U,374U,984U,0U},
  {3U,984U,374U,0U},
  {3U,2180U,3864U,0U},
  {3U,3864U,2180U,0U},
  {3U,214U,2273U,0U},
  {3U,2273U,214U,0U},
  {3U,882U,3411U,0U},
  {3U,3411U,882U,0U},
  {3U,1864U,1867U,0U},
  {3U,1867U,1864U,0U},
  {3U,2527U,3560U,0U},
  {3U,3560U,2527U,0U},
  {3U,4333U,4378U,0U},
  {3U,4378U,4333U,0U},
  {3U,470U,3147U,0U},
  {3U,3147U,470U,0U},
  {3U,143U,149U,0U},
  {3U,149U,143U,0U},
  {3U,53U,1610U,0U},
  {3U,1610U,53U,0U},
  {3U,4235U,4889U,0U},
  {3U,4889U,4235U,0U},
  {3U,1583U,2579U,0U},
  {3U,2579U,1583U,0U},
  {3U,1739U,4543U,0U},
  {3U,4543U,1739U,0U},
  {3U,2246U,3517U,0U},
  {3U,3517U,2246U,0U},
  {3U,2478U,2991U,0U},
  {3U,2991U,2478U,0U},
  {3U,367U,1602U,0U},
  {3U,1602U,367U,0U},
  {3U,1895U,3643U,0U},
  {3U,3643U,1895U,0U},
  {3U,889U,891U,0U},
  {3U,891U,889U,0U},
  {3U,3244U,3250U,0U},
  {3U,3250U,3244U,0U},
  {3U,1605U,1701U,0U},
  {3U,1701U,1605U,0U},
  {3U,3446U,5030U,0U},
  {3U,5030U,3446U,0U},
  {3U,60U,121U,0U},
  {3U,121U,60U,0U},
  {3U,2443U,3797U,0U},
  {3U,3797U,2443U,0U},
  {3U,4U,213U,0U},
  {3U,213U,4U,0U},
  {3U,4996U,5318U,0U},
  {3U,5318U,4996U,0U},
  {3U,2522U,4790U,0U},
  {3U,4790U,2522U,0U},
  {3U,1055U,3784U,0U},
  {3U,3784U,1055U,0U},
  {3U,1922U,3982U,0U},
  {3U,3982U,1922U,0U},
  {3U,1842U,2747U,0U},
  {3U,2747U,1842U,0U},
  {3U,4761U,5300U,0U},
  {3U,5300U,4761U,0U},
  {3U,3994U,5135U,0U},
  {3U,5135U,3994U,0U},
  {3U,1248U,3664U,0U},
  {3U,3664U,1248U,0U},
  {3U,27U,648U,0U},
  {3U,648U,27U,0U},
  {3U,797U,2215U,0U},
  {3U,2215U,797U,0U},
  {3U,1896U,3159U,0U},
  {3U,3159U,1896U,0U},
  {3U,71U,1527U,0U},
  {3U,1527U,71U,0U},
  {3U,71U,4782U,0U},
  {3U,4782U,71U,0U},
  {3U,1482U,3561U,0U},
  {3U,3561U,1482U,0U},
  {3U,1375U,4090U,0U},
  {3U,4090U,1375U,0U},
  {3U,147U,2345U,0U},
  {3U,2345U,147U,0U},
  {3U,3663U,5069U,0U},
  {3U,5069U,3663U,0U},
  {3U,2982U,4916U,0U},
  {3U,4916U,2982U,0U},
  {3U,893U,3824U,0U},
  {3U,3824U,893U,0U},
  {3U,3051U,4188U,0U},
  {3U,4188U,3051U,0U},
  {3U,4367U,5152U,0U},
  {3U,5152U,4367U,0U},
  {3U,42U,1350U,0U},
  {3U,1350U,42U,0U},
  {3U,5026U,5032U,0U},
  {3U,5032U,5026U,0U},
  {3U,2888U,4882U,0U},
  {3U,4882U,2888U,0U},
  {3U,1152U,1153U,0U},
  {3U,1153U,1152U,0U},
  {3U,1987U,3691U,0U},
  {3U,3691U,1987U,0U},
  {3U,1102U,3934U,0U},
  {3U,3934U,1102U,0U},
  {3U,1808U,2366U,0U},
  {3U,2366U,1808U,0U},
  {3U,2052U,4662U,0U},
  {3U,4662U,2052U,0U},
  {3U,1700U,3137U,0U},
  {3U,3137U,1700U,0U},
  {3U,4012U,4345U,0U},
  {3U,4345U,4012U,0U},
  {3U,1352U,1353U,0U},
  {3U,1353U,1352U,0U},
  {3U,328U,459U,0U},
  {3U,459U,328U,0U},
  {3U,4926U,5329U,0U},
  {3U,5329U,4926U,0U},
  {3U,73U,796U,0U},
  {3U,796U,73U,0U},
  {3U,2245U,3619U,0U},
  {3U,3619U,2245U,0U},
  {3U,2576U,4802U,0U},
  {3U,4802U,2576U,0U},
  {3U,2906U,4893U,0U},
  {3U,4893U,2906U,0U},
  {3U,761U,2272U,0U},
  {3U,2272U,761U,0U},
  {3U,965U,1756U,0U},
  {3U,1756U,965U,0U},
  {3U,2784U,3908U,0U},
  {3U,3908U,2784U,0U},
  {3U,1724U,4402U,0U},
  {3U,4402U,1724U,0U},
  {3U,3805U,4113U,0U},
  {3U,4113U,3805U,0U},
  {3U,467U,3603U,0U},
  {3U,3603U,467U,0U},
  {3U,788U,3332U,0U},
  {3U,3332U,788U,0U},
  {3U,5056U,5057U,0U},
  {3U,5057U,5056U,0U},
  {3U,401U,470U,0U},
  {3U,470U,401U,0U},
  {3U,109U,913U,0U},
  {3U,913U,109U,0U},
  {3U,3260U,4896U,0U},
  {3U,4896U,3260U,0U},
  {3U,108U,159U,0U},
  {3U,159U,108U,0U},
  {3U,3228U,3845U,0U},
  {3U,3845U,3228U,0U},
  {3U,53U,180U,0U},
  {3U,180U,53U,0U},
  {3U,710U,1824U,0U},
  {3U,1824U,710U,0U},
  {3U,2181U,4698U,0U},
  {3U,4698U,2181U,0U},
  {3U,98U,2063U,0U},
  {3U,2063U,98U,0U},
  {3U,104U,2224U,0U},
  {3U,2224U,104U,0U},
  {3U,296U,2899U,0U},
  {3U,2899U,296U,0U},
  {3U,2075U,3850U,0U},
  {3U,3850U,2075U,0U},
  {3U,1862U,2356U,0U},
  {3U,2356U,1862U,0U},
  {3U,382U,2378U,0U},
  {3U,2378U,382U,0U},
  {3U,136U,159U,0U},
  {3U,159U,136U,0U},
  {3U,3U,469U,0U},
  {3U,469U,3U,0U},
  {3U,1571U,2976U,0U},
  {3U,2976U,1571U,0U},
  {3U,2542U,3067U,0U},
  {3U,3067U,2542U,0U},
  {3U,53U,1664U,0U},
  {3U,1664U,53U,0U},
  {3U,1387U,3626U,0U},
  {3U,3626U,1387U,0U},
  {3U,2830U,3495U,0U},
  {3U,3495U,2830U,0U},
  {3U,272U,1086U,0U},
  {3U,1086U,272U,0U},
  {3U,1929U,2803U,0U},
  {3U,2803U,1929U,0U},
  {3U,124U,1358U,0U},
  {3U,1358U,124U,0U},
  {3U,1552U,3041U,0U},
  {3U,3041U,1552U,0U},
  {3U,1955U,2303U,0U},
  {3U,2303U,1955U,0U},
  {3U,952U,2410U,0U},
  {3U,2410U,952U,0U},
  {3U,945U,3325U,0U},
  {3U,3325U,945U,0U},
  {3U,1902U,2348U,0U},
  {3U,2348U,1902U,0U},
  {3U,2U,369U,0U},
  {3U,369U,2U,0U},
  {3U,4538U,4539U,0U},
  {3U,4539U,4538U,0U},
  {3U,3873U,4763U,0U},
  {3U,4763U,3873U,0U},
  {3U,389U,1490U,0U},
  {3U,1490U,389U,0U},
  {3U,4095U,5039U,0U},
  {3U,5039U,4095U,0U},
  {3U,84U,1958U,0U},
  {3U,1958U,84U,0U},
  {3U,5166U,5167U,0U},
  {3U,5167U,5166U,0U},
  {3U,2951U,3604U,0U},
  {3U,3604U,2951U,0U},
  {3U,2278U,2966U,0U},
  {3U,2966U,2278U,0U},
  {3U,3830U,4805U,0U},
  {3U,4805U,3830U,0U},
  {3U,810U,2144U,0U},
  {3U,2144U,810U,0U},
  {3U,1147U,1214U,0U},
  {3U,1214U,1147U,0U},
  {3U,242U,2271U,0U},
  {3U,2271U,242U,0U},
  {3U,1314U,4038U,0U},
  {3U,4038U,1314U,0U},
  {3U,897U,2636U,0U},
  {3U,2636U,897U,0U},
  {3U,4408U,4409U,0U},
  {3U,4409U,4408U,0U},
  {3U,2968U,3942U,0U},
  {3U,3942U,2968U,0U},
  {3U,1959U,1960U,0U},
  {3U,1960U,1959U,0U},
  {3U,4394U,4395U,0U},
  {3U,4395U,4394U,0U},
  {3U,4302U,4632U,0U},
  {3U,4632U,4302U,0U},
  {3U,877U,2193U,0U},
  {3U,2193U,877U,0U},
  {3U,902U,1326U,0U},
  {3U,1326U,902U,0U},
  {3U,2574U,2607U,0U},
  {3U,2607U,2574U,0U},
  {3U,4140U,4778U,0U},
  {3U,4778U,4140U,0U},
  {3U,70U,1881U,0U},
  {3U,1881U,70U,0U},
  {3U,2100U,4685U,0U},
  {3U,4685U,2100U,0U},
  {3U,1016U,2102U,0U},
  {3U,2102U,1016U,0U},
  {3U,1641U,4407U,0U},
  {3U,4407U,1641U,0U},
  {3U,774U,785U,0U},
  {3U,785U,774U,0U},
  {3U,899U,900U,0U},
  {3U,900U,899U,0U},
  {3U,4429U,5245U,0U},
  {3U,5245U,4429U,0U},
  {3U,954U,1826U,0U},
  {3U,1826U,954U,0U},
  {3U,53U,1590U,0U},
  {3U,1590U,53U,0U},
  {3U,2468U,4776U,0U},
  {3U,4776U,2468U,0U},
  {3U,682U,1926U,0U},
  {3U,1926U,682U,0U},
  {3U,278U,1765U,0U},
  {3U,1765U,278U,0U},
  {3U,4660U,4661U,0U},
  {3U,4661U,4660U,0U},
  {3U,1005U,1019U,0U},
  {3U,1019U,1005U,0U},
  {3U,1357U,4083U,0U},
  {3U,4083U,1357U,0U},
  {3U,528U,894U,0U},
  {3U,894U,528U,0U},
  {3U,2708U,3136U,0U},
  {3U,3136U,2708U,0U},
  {3U,71U,3161U,0U},
  {3U,3161U,71U,0U},
  {3U,215U,414U,0U},
  {3U,414U,215U,0U},
  {3U,3697U,4047U,0U},
  {3U,4047U,3697U,0U},
  {3U,3005U,3929U,0U},
  {3U,3929U,3005U,0U},
  {3U,1050U,2067U,0U},
  {3U,2067U,1050U,0U},
  {3U,425U,2202U,0U},
  {3U,2202U,425U,0U},
  {3U,2483U,2485U,0U},
  {3U,2485U,2483U,0U},
  {3U,25U,112U,0U},
  {3U,112U,25U,0U},
  {3U,2657U,4588U,0U},
  {3U,4588U,2657U,0U},
  {3U,2484U,2544U,0U},
  {3U,2544U,2484U,0U},
  {3U,279U,1854U,0U},
  {3U,1854U,279U,0U},
  {3U,5358U,5359U,0U},
  {3U,5359U,5358U,0U},
  {3U,4721U,4722U,0U},
  {3U,4722U,4721U,0U},
  {3U,2514U,2659U,0U},
  {3U,2659U,2514U,0U},
  {3U,425U,3457U,0U},
  {3U,3457U,425U,0U},
  {3U,125U,1934U,0U},
  {3U,1934U,125U,0U},
  {3U,693U,1780U,0U},
  {3U,1780U,693U,0U},
  {3U,3460U,5034U,0U},
  {3U,5034U,3460U,0U},
  {3U,39U,1130U,0U},
  {3U,1130U,39U,0U},
  {3U,120U,1591U,0U},
  {3U,1591U,120U,0U},
  {3U,3482U,4781U,0U},
  {3U,4781U,3482U,0U},
  {3U,53U,1747U,0U},
  {3U,1747U,53U,0U},
  {3U,213U,517U,0U},
  {3U,517U,213U,0U},
  {3U,534U,576U,0U},
  {3U,576U,534U,0U},
  {3U,4621U,5169U,0U},
  {3U,5169U,4621U,0U},
  {3U,2164U,2183U,0U},
  {3U,2183U,2164U,0U},
  {3U,1336U,2770U,0U},
  {3U,2770U,1336U,0U},
  {3U,418U,585U,0U},
  {3U,585U,418U,0U},
  {3U,367U,3449U,0U},
  {3U,3449U,367U,0U},
  {3U,2666U,3028U,0U},
  {3U,3028U,2666U,0U},
  {3U,1670U,2635U,0U},
  {3U,2635U,1670U,0U},
  {3U,4240U,4445U,0U},
  {3U,4445U,4240U,0U},
  {3U,1067U,3913U,0U},
  {3U,3913U,1067U,0U},
  {3U,1158U,1243U,0U},
  {3U,1243U,1158U,0U},
  {3U,3536U,3537U,0U},
  {3U,3537U,3536U,0U},
  {3U,146U,1288U,0U},
  {3U,1288U,146U,0U},
  {3U,960U,3580U,0U},
  {3U,3580U,960U,0U},
  {3U,4254U,4359U,0U},
  {3U,4359U,4254U,0U},
  {3U,2276U,2633U,0U},
  {3U,2633U,2276U,0U},
  {3U,933U,935U,0U},
  {3U,935U,933U,0U},
  {3U,1013U,3884U,0U},
  {3U,3884U,1013U,0U},
  {3U,2130U,2754U,0U},
  {3U,2754U,2130U,0U},
  {3U,110U,2261U,0U},
  {3U,2261U,110U,0U},
  {3U,277U,427U,0U},
  {3U,427U,277U,0U},
  {3U,1777U,2459U,0U},
  {3U,2459U,1777U,0U},
  {3U,2347U,4743U,0U},
  {3U,4743U,2347U,0U},
  {3U,669U,2929U,0U},
  {3U,2929U,669U,0U},
  {3U,2526U,4178U,0U},
  {3U,4178U,2526U,0U},
  {3U,17U,972U,0U},
  {3U,972U,17U,0U},
  {3U,1581U,4262U,0U},
  {3U,4262U,1581U,0U},
  {3U,3275U,3353U,0U},
  {3U,3353U,3275U,0U},
  {3U,519U,1297U,0U},
  {3U,1297U,519U,0U},
  {3U,2088U,3680U,0U},
  {3U,3680U,2088U,0U},
  {3U,531U,3658U,0U},
  {3U,3658U,531U,0U},
  {3U,3702U,4474U,0U},
  {3U,4474U,3702U,0U},
  {3U,1666U,4261U,0U},
  {3U,4261U,1666U,0U},
  {3U,179U,2692U,0U},
  {3U,2692U,179U,0U},
  {3U,1693U,3177U,0U},
  {3U,3177U,1693U,0U},
  {3U,1514U,4163U,0U},
  {3U,4163U,1514U,0U},
  {3U,1920U,2199U,0U},
  {3U,2199U,1920U,0U},
  {3U,611U,1183U,0U},
  {3U,1183U,611U,0U},
  {3U,5129U,5352U,0U},
  {3U,5352U,5129U,0U},
  {3U,24U,77U,0U},
  {3U,77U,24U,0U},
  {3U,511U,2772U,0U},
  {3U,2772U,511U,0U},
  {3U,675U,1465U,0U},
  {3U,1465U,675U,0U},
  {3U,420U,1620U,0U},
  {3U,1620U,420U,0U},
  {3U,1723U,2511U,0U},
  {3U,2511U,1723U,0U},
  {3U,1450U,2214U,0U},
  {3U,2214U,1450U,0U},
  {3U,202U,2364U,0U},
  {3U,2364U,202U,0U},
  {3U,124U,2382U,0U},
  {3U,2382U,124U,0U},
  {3U,993U,3592U,0U},
  {3U,3592U,993U,0U},
  {3U,643U,792U,0U},
  {3U,792U,643U,0U},
  {3U,4243U,4522U,0U},
  {3U,4522U,4243U,0U},
  {3U,1651U,4431U,0U},
  {3U,4431U,1651U,0U},
  {3U,812U,3785U,0U},
  {3U,3785U,812U,0U},
  {3U,4323U,5222U,0U},
  {3U,5222U,4323U,0U},
  {3U,2851U,2853U,0U},
  {3U,2853U,2851U,0U},
  {3U,3331U,4992U,0U},
  {3U,4992U,3331U,0U},
  {3U,1355U,2714U,0U},
  {3U,2714U,1355U,0U},
  {3U,1251U,1980U,0U},
  {3U,1980U,1251U,0U},
  {3U,4241U,4886U,0U},
  {3U,4886U,4241U,0U},
  {3U,1919U,4606U,0U},
  {3U,4606U,1919U,0U},
  {3U,1295U,3623U,0U},
  {3U,3623U,1295U,0U},
  {3U,2862U,3181U,0U},
  {3U,3181U,2862U,0U},
  {3U,3995U,3996U,0U},
  {3U,3996U,3995U,0U},
  {3U,3246U,3337U,0U},
  {3U,3337U,3246U,0U},
  {3U,380U,1250U,0U},
  {3U,1250U,380U,0U},
  {3U,76U,1906U,0U},
  {3U,1906U,76U,0U},
  {3U,3856U,4705U,0U},
  {3U,4705U,3856U,0U},
  {3U,4508U,4509U,0U},
  {3U,4509U,4508U,0U},
  {3U,325U,687U,0U},
  {3U,687U,325U,0U},
  {3U,5220U,5306U,0U},
  {3U,5306U,5220U,0U},
  {3U,3002U,3249U,0U},
  {3U,3249U,3002U,0U},
  {3U,3229U,4817U,0U},
  {3U,4817U,3229U,0U},
  {3U,1630U,4375U,0U},
  {3U,4375U,1630U,0U},
  {3U,2405U,2406U,0U},
  {3U,2406U,2405U,0U},
  {3U,1150U,3916U,0U},
  {3U,3916U,1150U,0U},
  {3U,1954U,3538U,0U},
  {3U,3538U,1954U,0U},
  {3U,676U,3739U,0U},
  {3U,3739U,676U,0U},
  {3U,2988U,3949U,0U},
  {3U,3949U,2988U,0U},
  {3U,517U,1157U,0U},
  {3U,1157U,517U,0U},
  {3U,990U,3870U,0U},
  {3U,3870U,990U,0U},
  {3U,91U,1992U,0U},
  {3U,1992U,91U,0U},
  {3U,4078U,4106U,0U},
  {3U,4106U,4078U,0U},
  {3U,3666U,5070U,0U},
  {3U,5070U,3666U,0U},
  {3U,1452U,4122U,0U},
  {3U,4122U,1452U,0U},
  {3U,2909U,2926U,0U},
  {3U,2926U,2909U,0U},
  {3U,2796U,2797U,0U},
  {3U,2797U,2796U,0U},
  {3U,1850U,3713U,0U},
  {3U,3713U,1850U,0U},
  {3U,2977U,3197U,0U},
  {3U,3197U,2977U,0U},
  {3U,133U,1138U,0U},
  {3U,1138U,133U,0U},
  {3U,4601U,4879U,0U},
  {3U,4879U,4601U,0U},
  {3U,3286U,3503U,0U},
  {3U,3503U,3286U,0U},
  {3U,4374U,4564U,0U},
  {3U,4564U,4374U,0U},
  {3U,4581U,4582U,0U},
  {3U,4582U,4581U,0U},
  {3U,2765U,3524U,0U},
  {3U,3524U,2765U,0U},
  {3U,4900U,5143U,0U},
  {3U,5143U,4900U,0U},
  {3U,4059U,5151U,0U},
  {3U,5151U,4059U,0U},
  {3U,2696U,3684U,0U},
  {3U,3684U,2696U,0U},
  {3U,4664U,5288U,0U},
  {3U,5288U,4664U,0U},
  {3U,4730U,5295U,0U},
  {3U,5295U,4730U,0U},
  {3U,1266U,3338U,0U},
  {3U,3338U,1266U,0U},
  {3U,3834U,4649U,0U},
  {3U,4649U,3834U,0U},
  {3U,480U,1237U,0U},
  {3U,1237U,480U,0U},
  {3U,4496U,5155U,0U},
  {3U,5155U,4496U,0U},
  {3U,2425U,4766U,0U},
  {3U,4766U,2425U,0U},
  {3U,246U,2148U,0U},
  {3U,2148U,246U,0U},
  {3U,399U,1648U,0U},
  {3U,1648U,399U,0U},
  {3U,1838U,3436U,0U},
  {3U,3436U,1838U,0U},
  {3U,3565U,3566U,0U},
  {3U,3566U,3565U,0U},
  {3U,1119U,2176U,0U},
  {3U,2176U,1119U,0U},
  {3U,4301U,5215U,0U},
  {3U,5215U,4301U,0U},
  {3U,3589U,3650U,0U},
  {3U,3650U,3589U,0U},
  {3U,1274U,4020U,0U},
  {3U,4020U,1274U,0U},
  {3U,2690U,4729U,0U},
  {3U,4729U,2690U,0U},
  {3U,71U,2435U,0U},
  {3U,2435U,71U,0U},
  {3U,447U,1293U,0U},
  {3U,1293U,447U,0U},
  {3U,2839U,2840U,0U},
  {3U,2840U,2839U,0U},
  {3U,1267U,1977U,0U},
  {3U,1977U,1267U,0U},
  {3U,468U,513U,0U},
  {3U,513U,468U,0U},
  {3U,2155U,3773U,0U},
  {3U,3773U,2155U,0U},
  {3U,3742U,5085U,0U},
  {3U,5085U,3742U,0U},
  {3U,4488U,5109U,0U},
  {3U,5109U,4488U,0U},
  {3U,2431U,2662U,0U},
  {3U,2662U,2431U,0U},
  {3U,4578U,4970U,0U},
  {3U,4970U,4578U,0U},
  {3U,1312U,3329U,0U},
  {3U,3329U,1312U,0U},
  {3U,677U,1935U,0U},
  {3U,1935U,677U,0U},
  {3U,5138U,5324U,0U},
  {3U,5324U,5138U,0U},
  {3U,885U,3324U,0U},
  {3U,3324U,885U,0U},
  {3U,294U,1721U,0U},
  {3U,1721U,294U,0U},
  {3U,453U,3543U,0U},
  {3U,3543U,453U,0U},
  {3U,374U,1917U,0U},
  {3U,1917U,374U,0U},
  {3U,2152U,3590U,0U},
  {3U,3590U,2152U,0U},
  {3U,2511U,4787U,0U},
  {3U,4787U,2511U,0U},
  {3U,2669U,3256U,0U},
  {3U,3256U,2669U,0U},
  {3U,2528U,2549U,0U},
  {3U,2549U,2528U,0U},
  {3U,85U,607U,0U},
  {3U,607U,85U,0U},
  {3U,4048U,4063U,0U},
  {3U,4063U,4048U,0U},
  {3U,1964U,2050U,0U},
  {3U,2050U,1964U,0U},
  {3U,2398U,4718U,0U},
  {3U,4718U,2398U,0U},
  {3U,1736U,2809U,0U},
  {3U,2809U,1736U,0U},
  {3U,2746U,3025U,0U},
  {3U,3025U,2746U,0U},
  {3U,434U,3568U,0U},
  {3U,3568U,434U,0U},
  {3U,810U,1898U,0U},
  {3U,1898U,810U,0U},
  {3U,4154U,4809U,0U},
  {3U,4809U,4154U,0U},
  {3U,853U,3470U,0U},
  {3U,3470U,853U,0U},
  {3U,782U,1166U,0U},
  {3U,1166U,782U,0U},
  {3U,1023U,3807U,0U},
  {3U,3807U,1023U,0U},
  {3U,4607U,4806U,0U},
  {3U,4806U,4607U,0U},
  {3U,2892U,4821U,0U},
  {3U,4821U,2892U,0U},
  {3U,3572U,5059U,0U},
  {3U,5059U,3572U,0U},
  {3U,941U,946U,0U},
  {3U,946U,941U,0U},
  {3U,4283U,5205U,0U},
  {3U,5205U,4283U,0U},
  {3U,2787U,3186U,0U},
  {3U,3186U,2787U,0U},
  {3U,500U,2072U,0U},
  {3U,2072U,500U,0U},
  {3U,2061U,2649U,0U},
  {3U,2649U,2061U,0U},
  {3U,395U,2798U,0U},
  {3U,2798U,395U,0U},
  {3U,2402U,3563U,0U},
  {3U,3563U,2402U,0U},
  {3U,4574U,4575U,0U},
  {3U,4575U,4574U,0U},
  {3U,497U,1038U,0U},
  {3U,1038U,497U,0U},
  {3U,71U,3175U,0U},
  {3U,3175U,71U,0U},
  {3U,656U,1026U,0U},
  {3U,1026U,656U,0U},
  {3U,3772U,4148U,0U},
  {3U,4148U,3772U,0U},
  {3U,403U,728U,0U},
  {3U,728U,403U,0U},
  {3U,98U,2028U,0U},
  {3U,2028U,98U,0U},
  {3U,131U,2400U,0U},
  {3U,2400U,131U,0U},
  {3U,191U,415U,0U},
  {3U,415U,191U,0U},
  {3U,2325U,4214U,0U},
  {3U,4214U,2325U,0U},
  {3U,89U,258U,0U},
  {3U,258U,89U,0U},
  {3U,618U,1503U,0U},
  {3U,1503U,618U,0U},
  {3U,5188U,5355U,0U},
  {3U,5355U,5188U,0U},
  {3U,2242U,3231U,0U},
  {3U,3231U,2242U,0U},
  {3U,5187U,5311U,0U},
  {3U,5311U,5187U,0U},
  {3U,867U,2639U,0U},
  {3U,2639U,867U,0U},
  {3U,138U,2295U,0U},
  {3U,2295U,138U,0U},
  {3U,2964U,3023U,0U},
  {3U,3023U,2964U,0U},
  {3U,1U,206U,0U},
  {3U,206U,1U,0U},
  {3U,2283U,3788U,0U},
  {3U,3788U,2283U,0U},
  {3U,53U,1561U,0U},
  {3U,1561U,53U,0U},
  {3U,1546U,3039U,0U},
  {3U,3039U,1546U,0U},
  {3U,2887U,4237U,0U},
  {3U,4237U,2887U,0U},
  {3U,1695U,4326U,0U},
  {3U,4326U,1695U,0U},
  {3U,3262U,4425U,0U},
  {3U,4425U,3262U,0U},
  {3U,367U,2013U,0U},
  {3U,2013U,367U,0U},
  {3U,1580U,1740U,0U},
  {3U,1740U,1580U,0U},
  {3U,71U,3690U,0U},
  {3U,3690U,71U,0U},
  {3U,4444U,5196U,0U},
  {3U,5196U,4444U,0U},
  {3U,770U,3430U,0U},
  {3U,3430U,770U,0U},
  {3U,3672U,4804U,0U},
  {3U,4804U,3672U,0U},
  {3U,382U,1950U,0U},
  {3U,1950U,382U,0U},
  {3U,144U,216U,0U},
  {3U,216U,144U,0U},
  {3U,4593U,5013U,0U},
  {3U,5013U,4593U,0U},
  {3U,1825U,3975U,0U},
  {3U,3975U,1825U,0U},
  {3U,68U,1865U,0U},
  {3U,1865U,68U,0U},
  {3U,2680U,3144U,0U},
  {3U,3144U,2680U,0U},
  {3U,3995U,4480U,0U},
  {3U,4480U,3995U,0U},
  {3U,2469U,3918U,0U},
  {3U,3918U,2469U,0U},
  {3U,1957U,3571U,0U},
  {3U,3571U,1957U,0U},
  {3U,1907U,2495U,0U},
  {3U,2495U,1907U,0U},
  {3U,218U,2914U,0U},
  {3U,2914U,218U,0U},
  {3U,1462U,2611U,0U},
  {3U,2611U,1462U,0U},
  {3U,1298U,4031U,0U},
  {3U,4031U,1298U,0U},
  {3U,3101U,3283U,0U},
  {3U,3283U,3101U,0U},
  {3U,1533U,2921U,0U},
  {3U,2921U,1533U,0U},
  {3U,1710U,4108U,0U},
  {3U,4108U,1710U,0U},
  {3U,2834U,4866U,0U},
  {3U,4866U,2834U,0U},
  {3U,2686U,4828U,0U},
  {3U,4828U,2686U,0U},
  {3U,208U,2824U,0U},
  {3U,2824U,208U,0U},
  {3U,3348U,5003U,0U},
  {3U,5003U,3348U,0U},
  {3U,71U,5024U,0U},
  {3U,5024U,71U,0U},
  {3U,4651U,5286U,0U},
  {3U,5286U,4651U,0U},
  {3U,5228U,5354U,0U},
  {3U,5354U,5228U,0U},
  {3U,2811U,3923U,0U},
  {3U,3923U,2811U,0U},
  {3U,408U,3494U,0U},
  {3U,3494U,408U,0U},
  {3U,509U,870U,0U},
  {3U,870U,509U,0U},
  {3U,92U,1996U,0U},
  {3U,1996U,92U,0U},
  {3U,655U,2678U,0U},
  {3U,2678U,655U,0U},
  {3U,3074U,4498U,0U},
  {3U,4498U,3074U,0U},
  {3U,5261U,5350U,0U},
  {3U,5350U,5261U,0U},
  {3U,2877U,4878U,0U},
  {3U,4878U,2877U,0U},
  {3U,1385U,1816U,0U},
  {3U,1816U,1385U,0U},
  {3U,2493U,2503U,0U},
  {3U,2503U,2493U,0U},
  {3U,829U,2702U,0U},
  {3U,2702U,829U,0U},
  {3U,4044U,5149U,0U},
  {3U,5149U,4044U,0U},
  {3U,1659U,4447U,0U},
  {3U,4447U,1659U,0U},
  {3U,4329U,5224U,0U},
  {3U,5224U,4329U,0U},
  {3U,443U,3574U,0U},
  {3U,3574U,443U,0U},
  {3U,632U,739U,0U},
  {3U,739U,632U,0U},
  {3U,4803U,5307U,0U},
  {3U,5307U,4803U,0U},
  {3U,1059U,3911U,0U},
  {3U,3911U,1059U,0U},
  {3U,610U,3518U,0U},
  {3U,3518U,610U,0U},
  {3U,563U,910U,0U},
  {3U,910U,563U,0U},
  {3U,4212U,4406U,0U},
  {3U,4406U,4212U,0U},
  {3U,120U,2200U,0U},
  {3U,2200U,120U,0U},
  {3U,198U,684U,0U},
  {3U,684U,198U,0U},
  {3U,2849U,4867U,0U},
  {3U,4867U,2849U,0U},
  {3U,332U,3290U,0U},
  {3U,3290U,332U,0U},
  {3U,398U,727U,0U},
  {3U,727U,398U,0U},
  {3U,377U,575U,0U},
  {3U,575U,377U,0U},
  {3U,4483U,4485U,0U},
  {3U,4485U,4483U,0U},
  {3U,3789U,4643U,0U},
  {3U,4643U,3789U,0U},
  {3U,2031U,3759U,0U},
  {3U,3759U,2031U,0U},
  {3U,4242U,4831U,0U},
  {3U,4831U,4242U,0U},
  {3U,2948U,3616U,0U},
  {3U,3616U,2948U,0U},
  {3U,2925U,4203U,0U},
  {3U,4203U,2925U,0U},
  {3U,3431U,4746U,0U},
  {3U,4746U,3431U,0U},
  {3U,397U,2521U,0U},
  {3U,2521U,397U,0U},
  {3U,1206U,2365U,0U},
  {3U,2365U,1206U,0U},
  {3U,812U,2658U,0U},
  {3U,2658U,812U,0U},
  {3U,1061U,3899U,0U},
  {3U,3899U,1061U,0U},
  {3U,2095U,2285U,0U},
  {3U,2285U,2095U,0U},
  {3U,120U,1285U,0U},
  {3U,1285U,120U,0U},
  {3U,2640U,4276U,0U},
  {3U,4276U,2640U,0U},
  {3U,3569U,3570U,0U},
  {3U,3570U,3569U,0U},
  {3U,7U,650U,0U},
  {3U,650U,7U,0U},
  {3U,2042U,2108U,0U},
  {3U,2108U,2042U,0U},
  {3U,0U,5U,0U},
  {3U,5U,0U,0U},
  {3U,357U,1897U,0U},
  {3U,1897U,357U,0U},
  {3U,1080U,1792U,0U},
  {3U,1792U,1080U,0U},
  {3U,573U,1157U,0U},
  {3U,1157U,573U,0U},
  {3U,5006U,5189U,0U},
  {3U,5189U,5006U,0U},
  {3U,42U,1366U,0U},
  {3U,1366U,42U,0U},
  {3U,1184U,2298U,0U},
  {3U,2298U,1184U,0U},
  {3U,49U,1208U,0U},
  {3U,1208U,49U,0U},
  {3U,244U,3006U,0U},
  {3U,3006U,244U,0U},
  {3U,4998U,5137U,0U},
  {3U,5137U,4998U,0U},
  {3U,2105U,3751U,0U},
  {3U,3751U,2105U,0U},
  {3U,757U,2142U,0U},
  {3U,2142U,757U,0U},
  {3U,53U,1551U,0U},
  {3U,1551U,53U,0U},
  {3U,528U,1856U,0U},
  {3U,1856U,528U,0U},
  {3U,662U,3732U,0U},
  {3U,3732U,662U,0U},
  {3U,3905U,5120U,0U},
  {3U,5120U,3905U,0U},
  {3U,1541U,1543U,0U},
  {3U,1543U,1541U,0U},
  {3U,3070U,4938U,0U},
  {3U,4938U,3070U,0U},
  {3U,4234U,4979U,0U},
  {3U,4979U,4234U,0U},
  {3U,1781U,3833U,0U},
  {3U,3833U,1781U,0U},
  {3U,382U,1874U,0U},
  {3U,1874U,382U,0U},
  {3U,1095U,1212U,0U},
  {3U,1212U,1095U,0U},
  {3U,122U,692U,0U},
  {3U,692U,122U,0U},
  {3U,3862U,4270U,0U},
  {3U,4270U,3862U,0U},
  {3U,64U,1839U,0U},
  {3U,1839U,64U,0U},
  {3U,4025U,4028U,0U},
  {3U,4028U,4025U,0U},
  {3U,4008U,4818U,0U},
  {3U,4818U,4008U,0U},
  {3U,254U,1794U,0U},
  {3U,1794U,254U,0U},
  {3U,1718U,4528U,0U},
  {3U,4528U,1718U,0U},
  {3U,264U,1767U,0U},
  {3U,1767U,264U,0U},
  {3U,1590U,3699U,0U},
  {3U,3699U,1590U,0U},
  {3U,141U,770U,0U},
  {3U,770U,141U,0U},
  {3U,346U,3316U,0U},
  {3U,3316U,346U,0U},
  {3U,4738U,4744U,0U},
  {3U,4744U,4738U,0U},
  {3U,4344U,4987U,0U},
  {3U,4987U,4344U,0U},
  {3U,2637U,4639U,0U},
  {3U,4639U,2637U,0U},
  {3U,2023U,2894U,0U},
  {3U,2894U,2023U,0U},
  {3U,138U,1667U,0U},
  {3U,1667U,138U,0U},
  {3U,2514U,3427U,0U},
  {3U,3427U,2514U,0U},
  {3U,323U,3293U,0U},
  {3U,3293U,323U,0U},
  {3U,391U,2194U,0U},
  {3U,2194U,391U,0U},
  {3U,136U,368U,0U},
  {3U,368U,136U,0U},
  {3U,2184U,3285U,0U},
  {3U,3285U,2184U,0U},
  {3U,1122U,1613U,0U},
  {3U,1613U,1122U,0U},
  {3U,3444U,3625U,0U},
  {3U,3625U,3444U,0U},
  {3U,223U,556U,0U},
  {3U,556U,223U,0U},
  {3U,1566U,4233U,0U},
  {3U,4233U,1566U,0U},
  {3U,2301U,2795U,0U},
  {3U,2795U,2301U,0U},
  {3U,1537U,4174U,0U},
  {3U,4174U,1537U,0U},
  {3U,4003U,4844U,0U},
  {3U,4844U,4003U,0U},
  {3U,2044U,4657U,0U},
  {3U,4657U,2044U,0U},
  {3U,1255U,1764U,0U},
  {3U,1764U,1255U,0U},
  {3U,1015U,3508U,0U},
  {3U,3508U,1015U,0U},
  {3U,2886U,4027U,0U},
  {3U,4027U,2886U,0U},
  {3U,2819U,2827U,0U},
  {3U,2827U,2819U,0U},
  {3U,3357U,4506U,0U},
  {3U,4506U,3357U,0U},
  {3U,1289U,2131U,0U},
  {3U,2131U,1289U,0U},
  {3U,1806U,3738U,0U},
  {3U,3738U,1806U,0U},
  {3U,348U,1344U,0U},
  {3U,1344U,348U,0U},
  {3U,305U,3276U,0U},
  {3U,3276U,305U,0U},
  {3U,273U,2684U,0U},
  {3U,2684U,273U,0U},
  {3U,5122U,5123U,0U},
  {3U,5123U,5122U,0U},
  {3U,4111U,4166U,0U},
  {3U,4166U,4111U,0U},
  {3U,4024U,4799U,0U},
  {3U,4799U,4024U,0U},
  {3U,1941U,2791U,0U},
  {3U,2791U,1941U,0U},
  {3U,1033U,3315U,0U},
  {3U,3315U,1033U,0U},
  {3U,651U,3364U,0U},
  {3U,3364U,651U,0U},
  {3U,33U,1242U,0U},
  {3U,1242U,33U,0U},
  {3U,952U,2408U,0U},
  {3U,2408U,952U,0U},
  {3U,5106U,5351U,0U},
  {3U,5351U,5106U,0U},
  {3U,178U,2689U,0U},
  {3U,2689U,178U,0U},
  {3U,3516U,3798U,0U},
  {3U,3798U,3516U,0U},
  {3U,1696U,3369U,0U},
  {3U,3369U,1696U,0U},
  {3U,850U,1807U,0U},
  {3U,1807U,850U,0U},
  {3U,12U,821U,0U},
  {3U,821U,12U,0U},
  {3U,4465U,4965U,0U},
  {3U,4965U,4465U,0U},
  {3U,2293U,4717U,0U},
  {3U,4717U,2293U,0U},
  {3U,3198U,3800U,0U},
  {3U,3800U,3198U,0U},
  {3U,4067U,5162U,0U},
  {3U,5162U,4067U,0U},
  {3U,2043U,2880U,0U},
  {3U,2880U,2043U,0U},
  {3U,5198U,5316U,0U},
  {3U,5316U,5198U,0U},
  {3U,350U,1524U,0U},
  {3U,1524U,350U,0U},
  {3U,3390U,5015U,0U},
  {3U,5015U,3390U,0U},
  {3U,3463U,3502U,0U},
  {3U,3502U,3463U,0U},
  {3U,4051U,4218U,0U},
  {3U,4218U,4051U,0U},
  {3U,4361U,4587U,0U},
  {3U,4587U,4361U,0U},
  {3U,4118U,4119U,0U},
  {3U,4119U,4118U,0U},
  {3U,454U,1606U,0U},
  {3U,1606U,454U,0U},
  {3U,706U,813U,0U},
  {3U,813U,706U,0U},
  {3U,3584U,5061U,0U},
  {3U,5061U,3584U,0U},
  {3U,1682U,4053U,0U},
  {3U,4053U,1682U,0U},
  {3U,515U,1221U,0U},
  {3U,1221U,515U,0U},
  {3U,24U,130U,0U},
  {3U,130U,24U,0U},
  {3U,20U,1093U,0U},
  {3U,1093U,20U,0U},
  {3U,998U,3876U,0U},
  {3U,3876U,998U,0U},
  {3U,1535U,3576U,0U},
  {3U,3576U,1535U,0U},
  {3U,218U,2902U,0U},
  {3U,2902U,218U,0U},
  {3U,1714U,4497U,0U},
  {3U,4497U,1714U,0U},
  {3U,1644U,4215U,0U},
  {3U,4215U,1644U,0U},
  {3U,2571U,3621U,0U},
  {3U,3621U,2571U,0U},
  {3U,1913U,2154U,0U},
  {3U,2154U,1913U,0U},
  {3U,2860U,4571U,0U},
  {3U,4571U,2860U,0U},
  {3U,29U,861U,0U},
  {3U,861U,29U,0U},
  {3U,1123U,1218U,0U},
  {3U,1218U,1123U,0U},
  {3U,1374U,3609U,0U},
  {3U,3609U,1374U,0U},
  {3U,3038U,3048U,0U},
  {3U,3048U,3038U,0U},
  {3U,617U,2016U,0U},
  {3U,2016U,617U,0U},
  {3U,2566U,3157U,0U},
  {3U,3157U,2566U,0U},
  {3U,1017U,2664U,0U},
  {3U,2664U,1017U,0U},
  {3U,21U,683U,0U},
  {3U,683U,21U,0U},
  {3U,810U,1104U,0U},
  {3U,1104U,810U,0U},
  {3U,545U,2252U,0U},
  {3U,2252U,545U,0U},
  {3U,538U,545U,0U},
  {3U,545U,538U,0U},
  {3U,523U,579U,0U},
  {3U,579U,523U,0U},
  {3U,1980U,2605U,0U},
  {3U,2605U,1980U,0U},
  {3U,1592U,4282U,0U},
  {3U,4282U,1592U,0U},
  {3U,76U,694U,0U},
  {3U,694U,76U,0U},
  {3U,1945U,2086U,0U},
  {3U,2086U,1945U,0U},
  {3U,657U,1841U,0U},
  {3U,1841U,657U,0U},
  {3U,752U,2270U,0U},
  {3U,2270U,752U,0U},
  {3U,3393U,5016U,0U},
  {3U,5016U,3393U,0U},
  {3U,1589U,4246U,0U},
  {3U,4246U,1589U,0U},
  {3U,1261U,1280U,0U},
  {3U,1280U,1261U,0U},
  {3U,28U,647U,0U},
  {3U,647U,28U,0U},
  {3U,4516U,4517U,0U},
  {3U,4517U,4516U,0U},
  {3U,3839U,4714U,0U},
  {3U,4714U,3839U,0U},
  {3U,5089U,5160U,0U},
  {3U,5160U,5089U,0U},
  {3U,907U,1202U,0U},
  {3U,1202U,907U,0U},
  {3U,1832U,2299U,0U},
  {3U,2299U,1832U,0U},
  {3U,3135U,4180U,0U},
  {3U,4180U,3135U,0U},
  {3U,5011U,5339U,0U},
  {3U,5339U,5011U,0U},
  {3U,1635U,1646U,0U},
  {3U,1646U,1635U,0U},
  {3U,1611U,4310U,0U},
  {3U,4310U,1611U,0U},
  {3U,4381U,4834U,0U},
  {3U,4834U,4381U,0U},
  {3U,2771U,3207U,0U},
  {3U,3207U,2771U,0U},
  {3U,2265U,3613U,0U},
  {3U,3613U,2265U,0U},
  {3U,34U,1259U,0U},
  {3U,1259U,34U,0U},
  {3U,120U,2329U,0U},
  {3U,2329U,120U,0U},
  {3U,1622U,4295U,0U},
  {3U,4295U,1622U,0U},
  {3U,25U,609U,0U},
  {3U,609U,25U,0U},
  {3U,2290U,2380U,0U},
  {3U,2380U,2290U,0U},
  {3U,1188U,3201U,0U},
  {3U,3201U,1188U,0U},
  {3U,414U,613U,0U},
  {3U,613U,414U,0U},
  {3U,98U,875U,0U},
  {3U,875U,98U,0U},
  {3U,1058U,2191U,0U},
  {3U,2191U,1058U,0U},
  {3U,3727U,3728U,0U},
  {3U,3728U,3727U,0U},
  {3U,0U,30U,0U},
  {3U,30U,0U,0U},
  {3U,3429U,3499U,0U},
  {3U,3499U,3429U,0U},
  {3U,3481U,4860U,0U},
  {3U,4860U,3481U,0U},
  {3U,382U,1010U,0U},
  {3U,1010U,382U,0U},
  {3U,1499U,3257U,0U},
  {3U,3257U,1499U,0U},
  {3U,1508U,1517U,0U},
  {3U,1517U,1508U,0U},
  {3U,3187U,3191U,0U},
  {3U,3191U,3187U,0U},
  {3U,3941U,5127U,0U},
  {3U,5127U,3941U,0U},
  {3U,2694U,2767U,0U},
  {3U,2767U,2694U,0U},
  {3U,536U,2383U,0U},
  {3U,2383U,536U,0U},
  {3U,1405U,3530U,0U},
  {3U,3530U,1405U,0U},
  {3U,3991U,4999U,0U},
  {3U,4999U,3991U,0U},
  {3U,372U,1836U,0U},
  {3U,1836U,372U,0U},
  {3U,4222U,5184U,0U},
  {3U,5184U,4222U,0U},
  {3U,2249U,3145U,0U},
  {3U,3145U,2249U,0U},
  {3U,986U,3328U,0U},
  {3U,3328U,986U,0U},
  {3U,4533U,4812U,0U},
  {3U,4812U,4533U,0U},
  {3U,624U,1009U,0U},
  {3U,1009U,624U,0U},
  {3U,435U,2820U,0U},
  {3U,2820U,435U,0U},
  {3U,1041U,2371U,0U},
  {3U,2371U,1041U,0U},
  {3U,2029U,4647U,0U},
  {3U,4647U,2029U,0U},
  {3U,2368U,3525U,0U},
  {3U,3525U,2368U,0U},
  {3U,4207U,4208U,0U},
  {3U,4208U,4207U,0U},
  {3U,1274U,3202U,0U},
  {3U,3202U,1274U,0U},
  {3U,1953U,3792U,0U},
  {3U,3792U,1953U,0U},
  {3U,1555U,1687U,0U},
  {3U,1687U,1555U,0U},
  {3U,71U,378U,0U},
  {3U,378U,71U,0U},
  {3U,1111U,2491U,0U},
  {3U,2491U,1111U,0U},
  {3U,4196U,4792U,0U},
  {3U,4792U,4196U,0U},
  {3U,3442U,4016U,0U},
  {3U,4016U,3442U,0U},
  {3U,186U,2701U,0U},
  {3U,2701U,186U,0U},
  {3U,1820U,3760U,0U},
  {3U,3760U,1820U,0U},
  {3U,328U,3300U,0U},
  {3U,3300U,328U,0U},
  {3U,177U,2687U,0U},
  {3U,2687U,177U,0U},
  {3U,4393U,4963U,0U},
  {3U,4963U,4393U,0U},
  {3U,4613U,4723U,0U},
  {3U,4723U,4613U,0U},
  {3U,264U,3098U,0U},
  {3U,3098U,264U,0U},
  {3U,71U,429U,0U},
  {3U,429U,71U,0U},
  {3U,927U,1224U,0U},
  {3U,1224U,927U,0U},
  {3U,3440U,3445U,0U},
  {3U,3445U,3440U,0U},
  {3U,1726U,4535U,0U},
  {3U,4535U,1726U,0U},
  {3U,3017U,4109U,0U},
  {3U,4109U,3017U,0U},
  {3U,3852U,4515U,0U},
  {3U,4515U,3852U,0U},
  {3U,3782U,5102U,0U},
  {3U,5102U,3782U,0U},
  {3U,661U,3550U,0U},
  {3U,3550U,661U,0U},
  {3U,820U,2451U,0U},
  {3U,2451U,820U,0U},
  {3U,1448U,4117U,0U},
  {3U,4117U,1448U,0U},
  {3U,994U,3872U,0U},
  {3U,3872U,994U,0U},
  {3U,2911U,4527U,0U},
  {3U,4527U,2911U,0U},
  {3U,703U,3667U,0U},
  {3U,3667U,703U,0U},
  {3U,79U,1025U,0U},
  {3U,1025U,79U,0U},
  {3U,1308U,3199U,0U},
  {3U,3199U,1308U,0U},
  {3U,938U,1142U,0U},
  {3U,1142U,938U,0U},
  {3U,478U,500U,0U},
  {3U,500U,478U,0U},
  {3U,1255U,3015U,0U},
  {3U,3015U,1255U,0U},
  {3U,1609U,1650U,0U},
  {3U,1650U,1609U,0U},
  {3U,2586U,2587U,0U},
  {3U,2587U,2586U,0U},
  {3U,1254U,4000U,0U},
  {3U,4000U,1254U,0U},
  {3U,626U,808U,0U},
  {3U,808U,626U,0U},
  {3U,1451U,1647U,0U},
  {3U,1647U,1451U,0U},
  {3U,794U,844U,0U},
  {3U,844U,794U,0U},
  {3U,3122U,3723U,0U},
  {3U,3723U,3122U,0U},
  {3U,98U,2045U,0U},
  {3U,2045U,98U,0U},
  {3U,1275U,2598U,0U},
  {3U,2598U,1275U,0U},
  {3U,76U,795U,0U},
  {3U,795U,76U,0U},
  {3U,2089U,4683U,0U},
  {3U,4683U,2089U,0U},
  {3U,670U,1226U,0U},
  {3U,1226U,670U,0U},
  {3U,502U,3531U,0U},
  {3U,3531U,502U,0U},
  {3U,324U,2034U,0U},
  {3U,2034U,324U,0U},
  {3U,5036U,5037U,0U},
  {3U,5037U,5036U,0U},
  {3U,1473U,4142U,0U},
  {3U,4142U,1473U,0U},
  {3U,63U,1828U,0U},
  {3U,1828U,63U,0U},
  {3U,738U,3761U,0U},
  {3U,3761U,738U,0U},
  {3U,1984U,4636U,0U},
  {3U,4636U,1984U,0U},
  {3U,560U,2013U,0U},
  {3U,2013U,560U,0U},
  {3U,2110U,3787U,0U},
  {3U,3787U,2110U,0U},
  {3U,3907U,3921U,0U},
  {3U,3921U,3907U,0U},
  {3U,51U,188U,0U},
  {3U,188U,51U,0U},
  {3U,4396U,5240U,0U},
  {3U,5240U,4396U,0U},
  {3U,1201U,1520U,0U},
  {3U,1520U,1201U,0U},
  {3U,179U,2693U,0U},
  {3U,2693U,179U,0U},
  {3U,1604U,1734U,0U},
  {3U,1734U,1604U,0U},
  {3U,182U,2695U,0U},
  {3U,2695U,182U,0U},
  {3U,742U,928U,0U},
  {3U,928U,742U,0U},
  {3U,297U,397U,0U},
  {3U,397U,297U,0U},
  {3U,1607U,3176U,0U},
  {3U,3176U,1607U,0U},
  {3U,2884U,2904U,0U},
  {3U,2904U,2884U,0U},
  {3U,227U,1196U,0U},
  {3U,1196U,227U,0U},
  {3U,632U,775U,0U},
  {3U,775U,632U,0U},
  {3U,856U,1120U,0U},
  {3U,1120U,856U,0U},
  {3U,1742U,2938U,0U},
  {3U,2938U,1742U,0U},
  {3U,53U,1669U,0U},
  {3U,1669U,53U,0U},
  {3U,2872U,4281U,0U},
  {3U,4281U,2872U,0U},
  {3U,1771U,2645U,0U},
  {3U,2645U,1771U,0U},
  {3U,876U,1294U,0U},
  {3U,1294U,876U,0U},
  {3U,148U,2547U,0U},
  {3U,2547U,148U,0U},
  {3U,120U,1343U,0U},
  {3U,1343U,120U,0U},
  {3U,341U,2602U,0U},
  {3U,2602U,341U,0U},
  {3U,516U,3648U,0U},
  {3U,3648U,516U,0U},
  {3U,3087U,3579U,0U},
  {3U,3579U,3087U,0U},
  {3U,4748U,5072U,0U},
  {3U,5072U,4748U,0U},
  {3U,5086U,5087U,0U},
  {3U,5087U,5086U,0U},
  {3U,3073U,4939U,0U},
  {3U,4939U,3073U,0U},
};
static const unsigned paper_offsets[2] = {0U,6144U};
static const unsigned paper_work_starts[1] = {4096U};
static const unsigned paper_expected_sums[1] = {3359389020U};
static const unsigned paper_expected_edges[1] = {6144U};
static PAPER_INPUT uint64_t paper_expected_degrees[5372] = {4,3,9,5,3,1,1,1,1,1,3,1,1,2,1,1,1,4,3,1,2,1,1,1,2,2,3,4,2,1,1,1,2,1,3,1,1,1,1,3,1,1,5,2,1,2,1,1,3,1,1,1,2,34,1,1,1,1,1,1,1,1,1,1,2,1,1,1,2,1,3,35,1,1,1,1,8,1,1,1,1,1,1,2,1,2,1,1,2,1,1,1,1,2,1,1,1,1,13,1,5,1,1,1,7,2,1,1,1,1,1,1,3,3,1,1,1,1,1,1,21,1,2,1,8,1,1,1,1,1,1,1,3,1,1,1,2,1,5,1,2,2,2,9,1,1,4,2,3,1,2,1,1,1,1,1,1,1,1,2,1,1,1,1,1,1,4,2,1,2,1,2,2,1,1,1,1,1,2,2,1,2,1,1,1,1,7,1,1,1,1,3,5,1,1,1,1,1,1,1,3,1,1,1,1,1,1,1,1,1,1,1,1,5,1,2,2,1,8,1,1,1,1,1,1,2,3,1,1,1,2,1,1,1,2,1,1,1,3,1,1,1,1,2,2,1,1,5,1,1,1,1,1,2,1,1,1,2,3,1,1,4,1,3,7,1,4,3,1,3,1,2,2,2,1,2,1,1,1,1,2,1,1,1,1,2,1,1,1,1,1,1,1,2,1,2,2,1,1,1,1,2,1,1,2,1,1,2,2,1,1,1,1,2,1,1,3,1,2,1,1,5,2,2,3,4,1,1,2,2,1,1,2,2,1,1,3,2,2,1,1,4,2,3,3,2,1,1,1,1,2,1,1,1,2,1,1,3,1,1,1,1,1,1,1,1,3,5,2,1,1,2,1,1,2,1,1,1,1,1,5,1,11,2,1,1,1,1,1,3,2,1,1,1,1,1,1,2,2,2,1,3,1,1,1,2,1,1,1,2,1,1,1,1,4,1,1,1,2,2,4,1,1,2,1,5,1,1,1,1,1,1,1,1,2,1,1,1,1,1,1,1,2,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,2,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,2,3,1,1,1,1,1,1,1,1,3,1,2,6,1,2,1,3,1,1,1,1,1,1,2,1,1,1,2,6,5,1,2,1,1,1,1,1,1,1,1,3,1,1,1,1,1,1,2,2,1,3,3,1,1,1,1,1,2,1,2,1,1,1,1,1,5,2,1,3,1,1,1,1,2,2,1,1,1,1,1,1,1,1,2,1,3,1,1,1,1,1,1,3,1,1,1,1,1,1,1,1,1,1,2,5,1,1,1,1,1,1,2,1,1,1,1,1,1,1,1,1,1,1,3,1,1,1,1,1,1,1,1,1,2,2,1,2,1,3,1,2,1,1,2,5,1,1,1,1,1,1,1,1,1,1,1,2,1,1,1,1,1,1,1,2,1,1,1,1,1,1,1,1,1,1,1,1,1,3,1,1,1,1,1,2,1,1,2,1,2,1,1,1,1,3,1,1,1,1,1,2,1,1,1,2,1,1,1,1,1,1,2,1,1,1,1,1,2,1,1,1,1,2,1,1,1,3,2,1,1,1,1,2,1,6,1,1,1,1,1,3,2,1,1,1,1,1,1,2,1,1,1,1,1,1,1,1,1,2,1,1,1,1,1,1,1,1,1,1,1,1,2,1,2,1,1,1,1,1,2,1,1,2,2,1,1,2,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,2,2,1,2,1,4,1,1,1,2,1,1,1,1,1,1,1,1,4,1,2,1,1,1,1,1,1,2,1,1,1,1,1,2,1,1,1,1,1,1,1,1,1,1,1,1,1,2,1,2,1,2,3,1,2,1,2,1,1,1,1,1,1,1,2,1,4,1,1,1,1,1,1,1,1,2,1,1,2,1,1,2,1,2,1,2,1,1,2,1,2,1,1,1,2,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,2,1,1,1,1,1,1,3,1,1,1,1,1,1,1,3,1,1,1,1,2,1,1,1,2,1,1,2,1,1,2,1,1,1,1,1,1,1,1,1,1,1,1,2,1,3,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,3,1,1,1,2,1,3,2,1,1,1,1,1,2,1,1,2,1,1,3,1,1,1,2,2,1,1,1,1,1,1,1,1,2,1,1,1,1,1,1,1,1,1,2,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,2,1,1,1,1,1,1,1,1,2,2,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,3,1,2,1,1,1,2,1,1,1,1,1,1,1,1,1,3,4,1,1,3,1,2,1,1,1,1,1,1,1,1,1,2,1,1,1,1,1,1,2,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,2,1,1,1,1,1,2,1,1,1,2,1,1,1,1,1,1,1,1,1,1,1,3,2,1,1,1,1,3,1,2,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,3,2,1,1,1,1,1,1,1,1,1,1,1,1,3,1,1,1,1,1,2,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,2,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,2,1,1,1,2,2,1,1,1,2,1,1,1,2,1,1,1,1,1,1,1,1,1,1,2,2,1,1,1,1,2,2,1,1,1,1,1,1,2,1,1,1,2,3,1,2,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,2,1,1,2,1,1,1,1,1,1,1,1,1,1,2,1,1,1,1,1,1,1,1,1,2,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,2,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,2,1,2,1,1,1,1,1,2,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,2,1,1,1,1,1,1,1,1,1,1,1,1,1,1,2,1,1,1,1,1,1,2,1,1,1,1,1,1,1,1,1,2,1,1,1,1,1,2,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,2,1,2,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,2,1,1,1,1,1,1,1,1,1,1,2,1,1,1,1,1,1,4,1,1,1,1,1,1,1,2,1,1,1,1,1,2,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,2,1,1,1,1,1,1,2,1,2,1,1,1,1,1,1,2,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,2,1,1,1,1,2,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,3,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,3,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,2,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,2,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,2,1,2,1,1,1,1,1,1,1,1,1,1,1,1,1,2,1,1,1,1,1,1,1,2,1,1,1,1,1,1,1,1,1,1,1,3,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,2,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,2,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,2,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,2,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,2,1,2,1,1,1,1,1,1,2,1,1,4,1,1,1,1,2,1,1,1,1,1,2,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,2,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,2,1,1,1,1,1,1,3,1,1,1,1,1,1,1,1,1,1,2,1,1,1,1,1,1,1,1,1,1,1,1,2,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,2,1,1,1,1,1,1,1,2,1,1,1,1,1,1,1,1,1,1,1,1,2,1,1,1,1,1,1,1,1,1,1,3,2,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,2,1,1,1,1,1,1,1,1,1,1,1,6,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,2,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,2,1,1,1,1,1,1,1,3,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,2,1,1,1,1,1,1,1,1,2,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,2,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,2,1,1,1,1,1,1,1,1,1,1,1,1,2,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,2,1,1,1,1,1,2,1,1,2,1,1,1,2,1,1,1,1,1,1,1,1,1,1,1,1,2,1,1,1,1,1,1,1,2,1,1,1,1,1,3,1,1,3,2,1,1,1,1,1,2,1,1,1,1,1,1,1,1,1,1,1,1,2,1,1,1,2,1,1,1,1,1,1,1,2,1,1,1,1,2,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,2,1,1,1,1,1,1,1,1,1,1,1,1,1,2,1,1,2,1,1,1,1,2,1,1,1,1,1,2,1,1,1,1,1,1,1,1,1,1,1,2,1,1,1,1,1,1,1,1,1,1,1,2,2,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,2,1,1,1,1,1,1,3,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,2,1,1,1,1,1,2,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,2,1,1,1,2,1,1,1,1,1,1,1,1,2,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,2,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,2,1,1,2,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,2,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,3,1,1,1,1,1,1,1,1,1,2,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,2,1,1,1,1,1,1,2,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,2,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,3,1,1,2,1,1,1,1,1,1,1,1,1,1,2,1,1,1,1,1,1,3,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,2,1,1,1,2,1,2,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,2,2,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,2,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,2,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,2,1,1,1,1,1,1,1,2,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,2,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,2,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,2,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,2,1,1,2,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,3,1,1,1,1,1,1,1,1,1,1,1,1,1,2,2,1,1,1,1,1,1,1,1,1,1,2,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,2,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,2,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,2,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,2,1,1,1,1,1,1,1,1,1,1,1,2,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,2,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,2,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,2,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,2,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,2,1,1,1,1,1,1,2,1,1,1,1,1,1,1,1,2,1,1,1,2,1,1,1,2,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,2,1,1,1,1,1,1,1,2,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,2,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,2,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,2,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,2,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,2,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,2,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,2,1,2,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,2,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,2,1,2,2,1,1,2,1,1,1,1,1,1,1,1,1,1,1,1,1,2,1,1,1,1,1,1,1,1,2,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,2,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,2,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,2,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1};
#ifdef HOST_TEST
static PAPER_INPUT struct PaperOp paper_exact_edges[6144] = {
  {0U,0U,5U,0U},
  {0U,0U,30U,0U},
  {0U,0U,36U,0U},
  {0U,0U,114U,0U},
  {0U,1U,184U,0U},
  {0U,1U,201U,0U},
  {0U,1U,206U,0U},
  {0U,2U,161U,0U},
  {0U,2U,256U,0U},
  {0U,2U,313U,0U},
  {0U,2U,334U,0U},
  {0U,2U,369U,0U},
  {0U,2U,384U,0U},
  {0U,2U,387U,0U},
  {0U,2U,390U,0U},
  {0U,2U,396U,0U},
  {0U,3U,193U,0U},
  {0U,3U,229U,0U},
  {0U,3U,452U,0U},
  {0U,3U,469U,0U},
  {0U,3U,474U,0U},
  {0U,4U,213U,0U},
  {0U,4U,486U,0U},
  {0U,4U,591U,0U},
  {0U,5U,0U,0U},
  {0U,6U,132U,0U},
  {0U,7U,650U,0U},
  {0U,8U,45U,0U},
  {0U,9U,673U,0U},
  {0U,10U,679U,0U},
  {0U,10U,680U,0U},
  {0U,10U,690U,0U},
  {0U,11U,717U,0U},
  {0U,12U,821U,0U},
  {0U,13U,535U,0U},
  {0U,13U,848U,0U},
  {0U,14U,2401U,0U},
  {0U,15U,883U,0U},
  {0U,16U,967U,0U},
  {0U,17U,143U,0U},
  {0U,17U,561U,0U},
  {0U,17U,972U,0U},
  {0U,17U,983U,0U},
  {0U,18U,129U,0U},
  {0U,18U,780U,0U},
  {0U,18U,1052U,0U},
  {0U,19U,1087U,0U},
  {0U,20U,1092U,0U},
  {0U,20U,1093U,0U},
  {0U,21U,683U,0U},
  {0U,22U,1105U,0U},
  {0U,23U,1113U,0U},
  {0U,24U,77U,0U},
  {0U,24U,130U,0U},
  {0U,25U,112U,0U},
  {0U,25U,609U,0U},
  {0U,26U,132U,0U},
  {0U,26U,135U,0U},
  {0U,26U,915U,0U},
  {0U,27U,154U,0U},
  {0U,27U,187U,0U},
  {0U,27U,648U,0U},
  {0U,27U,1179U,0U},
  {0U,28U,647U,0U},
  {0U,28U,1195U,0U},
  {0U,29U,861U,0U},
  {0U,30U,0U,0U},
  {0U,31U,1209U,0U},
  {0U,32U,1219U,0U},
  {0U,32U,1220U,0U},
  {0U,33U,1242U,0U},
  {0U,34U,1256U,0U},
  {0U,34U,1259U,0U},
  {0U,34U,1280U,0U},
  {0U,35U,772U,0U},
  {0U,36U,0U,0U},
  {0U,37U,1300U,0U},
  {0U,38U,1301U,0U},
  {0U,39U,838U,0U},
  {0U,39U,1130U,0U},
  {0U,39U,1316U,0U},
  {0U,40U,1320U,0U},
  {0U,41U,318U,0U},
  {0U,42U,96U,0U},
  {0U,42U,247U,0U},
  {0U,42U,473U,0U},
  {0U,42U,1350U,0U},
  {0U,42U,1366U,0U},
  {0U,43U,83U,0U},
  {0U,43U,1323U,0U},
  {0U,44U,641U,0U},
  {0U,45U,8U,0U},
  {0U,45U,951U,0U},
  {0U,46U,1393U,0U},
  {0U,47U,1434U,0U},
  {0U,48U,205U,0U},
  {0U,48U,209U,0U},
  {0U,48U,1470U,0U},
  {0U,49U,1208U,0U},
  {0U,50U,1492U,0U},
  {0U,51U,188U,0U},
  {0U,52U,1497U,0U},
  {0U,52U,1532U,0U},
  {0U,53U,180U,0U},
  {0U,53U,251U,0U},
  {0U,53U,306U,0U},
  {0U,53U,349U,0U},
  {0U,53U,389U,0U},
  {0U,53U,410U,0U},
  {0U,53U,412U,0U},
  {0U,53U,1085U,0U},
  {0U,53U,1408U,0U},
  {0U,53U,1547U,0U},
  {0U,53U,1551U,0U},
  {0U,53U,1558U,0U},
  {0U,53U,1561U,0U},
  {0U,53U,1567U,0U},
  {0U,53U,1590U,0U},
  {0U,53U,1610U,0U},
  {0U,53U,1649U,0U},
  {0U,53U,1656U,0U},
  {0U,53U,1664U,0U},
  {0U,53U,1669U,0U},
  {0U,53U,1673U,0U},
  {0U,53U,1676U,0U},
  {0U,53U,1681U,0U},
  {0U,53U,1690U,0U},
  {0U,53U,1694U,0U},
  {0U,53U,1699U,0U},
  {0U,53U,1702U,0U},
  {0U,53U,1713U,0U},
  {0U,53U,1722U,0U},
  {0U,53U,1747U,0U},
  {0U,53U,1750U,0U},
  {0U,53U,1763U,0U},
  {0U,53U,1774U,0U},
  {0U,53U,1783U,0U},
  {0U,54U,1173U,0U},
  {0U,55U,1800U,0U},
  {0U,56U,958U,0U},
  {0U,57U,1618U,0U},
  {0U,58U,1075U,0U},
  {0U,59U,779U,0U},
  {0U,60U,121U,0U},
  {0U,61U,1057U,0U},
  {0U,62U,64U,0U},
  {0U,63U,1828U,0U},
  {0U,64U,62U,0U},
  {0U,64U,1839U,0U},
  {0U,65U,1850U,0U},
  {0U,66U,249U,0U},
  {0U,67U,457U,0U},
  {0U,68U,591U,0U},
  {0U,68U,1865U,0U},
  {0U,69U,495U,0U},
  {0U,70U,375U,0U},
  {0U,70U,1879U,0U},
  {0U,70U,1881U,0U},
  {0U,71U,128U,0U},
  {0U,71U,378U,0U},
  {0U,71U,429U,0U},
  {0U,71U,773U,0U},
  {0U,71U,1039U,0U},
  {0U,71U,1527U,0U},
  {0U,71U,1815U,0U},
  {0U,71U,1932U,0U},
  {0U,71U,1988U,0U},
  {0U,71U,2069U,0U},
  {0U,71U,2150U,0U},
  {0U,71U,2239U,0U},
  {0U,71U,2435U,0U},
  {0U,71U,2950U,0U},
  {0U,71U,3008U,0U},
  {0U,71U,3161U,0U},
  {0U,71U,3175U,0U},
  {0U,71U,3239U,0U},
  {0U,71U,3326U,0U},
  {0U,71U,3420U,0U},
  {0U,71U,3683U,0U},
  {0U,71U,3690U,0U},
  {0U,71U,3814U,0U},
  {0U,71U,3843U,0U},
  {0U,71U,3855U,0U},
  {0U,71U,3871U,0U},
  {0U,71U,4040U,0U},
  {0U,71U,4592U,0U},
  {0U,71U,4631U,0U},
  {0U,71U,4687U,0U},
  {0U,71U,4782U,0U},
  {0U,71U,5024U,0U},
  {0U,71U,5117U,0U},
  {0U,71U,5364U,0U},
  {0U,71U,5371U,0U},
  {0U,72U,1065U,0U},
  {0U,73U,796U,0U},
  {0U,74U,1886U,0U},
  {0U,75U,553U,0U},
  {0U,76U,551U,0U},
  {0U,76U,694U,0U},
  {0U,76U,700U,0U},
  {0U,76U,795U,0U},
  {0U,76U,1893U,0U},
  {0U,76U,1898U,0U},
  {0U,76U,1905U,0U},
  {0U,76U,1906U,0U},
  {0U,77U,24U,0U},
  {0U,78U,1924U,0U},
  {0U,79U,1025U,0U},
  {0U,80U,103U,0U},
  {0U,81U,1946U,0U},
  {0U,82U,741U,0U},
  {0U,83U,43U,0U},
  {0U,83U,1378U,0U},
  {0U,84U,1958U,0U},
  {0U,85U,607U,0U},
  {0U,85U,1249U,0U},
  {0U,86U,1974U,0U},
  {0U,87U,1979U,0U},
  {0U,88U,749U,0U},
  {0U,88U,750U,0U},
  {0U,89U,258U,0U},
  {0U,90U,602U,0U},
  {0U,91U,1992U,0U},
  {0U,92U,1996U,0U},
  {0U,93U,207U,0U},
  {0U,93U,234U,0U},
  {0U,94U,1849U,0U},
  {0U,95U,809U,0U},
  {0U,96U,42U,0U},
  {0U,97U,649U,0U},
  {0U,98U,875U,0U},
  {0U,98U,1415U,0U},
  {0U,98U,1445U,0U},
  {0U,98U,1467U,0U},
  {0U,98U,1919U,0U},
  {0U,98U,2022U,0U},
  {0U,98U,2025U,0U},
  {0U,98U,2028U,0U},
  {0U,98U,2032U,0U},
  {0U,98U,2045U,0U},
  {0U,98U,2063U,0U},
  {0U,98U,2097U,0U},
  {0U,98U,2103U,0U},
  {0U,99U,2113U,0U},
  {0U,100U,315U,0U},
  {0U,100U,543U,0U},
  {0U,100U,652U,0U},
  {0U,100U,908U,0U},
  {0U,100U,2124U,0U},
  {0U,101U,358U,0U},
  {0U,102U,1145U,0U},
  {0U,103U,80U,0U},
  {0U,104U,500U,0U},
  {0U,104U,1139U,0U},
  {0U,104U,1167U,0U},
  {0U,104U,1969U,0U},
  {0U,104U,2161U,0U},
  {0U,104U,2224U,0U},
  {0U,104U,2243U,0U},
  {0U,105U,1846U,0U},
  {0U,105U,2248U,0U},
  {0U,106U,1360U,0U},
  {0U,107U,823U,0U},
  {0U,108U,159U,0U},
  {0U,109U,913U,0U},
  {0U,110U,2261U,0U},
  {0U,111U,860U,0U},
  {0U,112U,25U,0U},
  {0U,112U,1485U,0U},
  {0U,112U,2268U,0U},
  {0U,113U,719U,0U},
  {0U,113U,1831U,0U},
  {0U,113U,1878U,0U},
  {0U,114U,0U,0U},
  {0U,115U,1234U,0U},
  {0U,116U,1519U,0U},
  {0U,117U,1860U,0U},
  {0U,118U,961U,0U},
  {0U,119U,1091U,0U},
  {0U,120U,197U,0U},
  {0U,120U,392U,0U},
  {0U,120U,446U,0U},
  {0U,120U,460U,0U},
  {0U,120U,475U,0U},
  {0U,120U,477U,0U},
  {0U,120U,839U,0U},
  {0U,120U,1101U,0U},
  {0U,120U,1285U,0U},
  {0U,120U,1343U,0U},
  {0U,120U,1359U,0U},
  {0U,120U,1456U,0U},
  {0U,120U,1591U,0U},
  {0U,120U,1947U,0U},
  {0U,120U,2200U,0U},
  {0U,120U,2312U,0U},
  {0U,120U,2318U,0U},
  {0U,120U,2321U,0U},
  {0U,120U,2327U,0U},
  {0U,120U,2329U,0U},
  {0U,120U,2334U,0U},
  {0U,121U,60U,0U},
  {0U,122U,692U,0U},
  {0U,122U,2354U,0U},
  {0U,123U,748U,0U},
  {0U,124U,571U,0U},
  {0U,124U,1189U,0U},
  {0U,124U,1358U,0U},
  {0U,124U,1426U,0U},
  {0U,124U,1507U,0U},
  {0U,124U,2058U,0U},
  {0U,124U,2322U,0U},
  {0U,124U,2382U,0U},
  {0U,125U,1934U,0U},
  {0U,126U,167U,0U},
  {0U,127U,504U,0U},
  {0U,128U,71U,0U},
  {0U,129U,18U,0U},
  {0U,130U,24U,0U},
  {0U,131U,2400U,0U},
  {0U,132U,6U,0U},
  {0U,132U,26U,0U},
  {0U,132U,957U,0U},
  {0U,133U,1138U,0U},
  {0U,134U,1001U,0U},
  {0U,135U,26U,0U},
  {0U,136U,159U,0U},
  {0U,136U,368U,0U},
  {0U,137U,844U,0U},
  {0U,138U,1667U,0U},
  {0U,138U,2284U,0U},
  {0U,138U,2295U,0U},
  {0U,138U,2428U,0U},
  {0U,138U,2434U,0U},
  {0U,139U,219U,0U},
  {0U,140U,310U,0U},
  {0U,140U,2444U,0U},
  {0U,141U,770U,0U},
  {0U,141U,774U,0U},
  {0U,142U,1970U,0U},
  {0U,142U,2030U,0U},
  {0U,143U,17U,0U},
  {0U,143U,149U,0U},
  {0U,143U,190U,0U},
  {0U,143U,200U,0U},
  {0U,143U,691U,0U},
  {0U,143U,2296U,0U},
  {0U,143U,2467U,0U},
  {0U,143U,2472U,0U},
  {0U,143U,2476U,0U},
  {0U,144U,216U,0U},
  {0U,145U,438U,0U},
  {0U,146U,280U,0U},
  {0U,146U,430U,0U},
  {0U,146U,1288U,0U},
  {0U,146U,1776U,0U},
  {0U,147U,616U,0U},
  {0U,147U,2345U,0U},
  {0U,148U,2547U,0U},
  {0U,148U,2550U,0U},
  {0U,148U,2552U,0U},
  {0U,149U,143U,0U},
  {0U,150U,311U,0U},
  {0U,150U,2562U,0U},
  {0U,151U,2565U,0U},
  {0U,152U,406U,0U},
  {0U,153U,2588U,0U},
  {0U,154U,27U,0U},
  {0U,155U,2593U,0U},
  {0U,156U,1469U,0U},
  {0U,157U,257U,0U},
  {0U,158U,1744U,0U},
  {0U,159U,108U,0U},
  {0U,159U,136U,0U},
  {0U,160U,2617U,0U},
  {0U,161U,2U,0U},
  {0U,162U,2300U,0U},
  {0U,163U,766U,0U},
  {0U,164U,2553U,0U},
  {0U,165U,2624U,0U},
  {0U,166U,882U,0U},
  {0U,166U,994U,0U},
  {0U,166U,1583U,0U},
  {0U,166U,2627U,0U},
  {0U,167U,126U,0U},
  {0U,167U,626U,0U},
  {0U,168U,831U,0U},
  {0U,169U,1082U,0U},
  {0U,169U,1488U,0U},
  {0U,170U,199U,0U},
  {0U,171U,371U,0U},
  {0U,171U,2647U,0U},
  {0U,172U,1717U,0U},
  {0U,172U,2662U,0U},
  {0U,173U,2672U,0U},
  {0U,174U,2677U,0U},
  {0U,175U,729U,0U},
  {0U,176U,2682U,0U},
  {0U,177U,2687U,0U},
  {0U,178U,2688U,0U},
  {0U,178U,2689U,0U},
  {0U,179U,2692U,0U},
  {0U,179U,2693U,0U},
  {0U,180U,53U,0U},
  {0U,181U,261U,0U},
  {0U,181U,1837U,0U},
  {0U,182U,2695U,0U},
  {0U,183U,200U,0U},
  {0U,184U,1U,0U},
  {0U,185U,1155U,0U},
  {0U,186U,448U,0U},
  {0U,186U,1496U,0U},
  {0U,186U,1692U,0U},
  {0U,186U,2236U,0U},
  {0U,186U,2390U,0U},
  {0U,186U,2395U,0U},
  {0U,186U,2701U,0U},
  {0U,187U,27U,0U},
  {0U,188U,51U,0U},
  {0U,189U,2189U,0U},
  {0U,190U,143U,0U},
  {0U,191U,314U,0U},
  {0U,191U,415U,0U},
  {0U,191U,2612U,0U},
  {0U,192U,228U,0U},
  {0U,192U,1176U,0U},
  {0U,192U,2127U,0U},
  {0U,192U,2135U,0U},
  {0U,192U,2768U,0U},
  {0U,193U,3U,0U},
  {0U,194U,2777U,0U},
  {0U,195U,2358U,0U},
  {0U,196U,2789U,0U},
  {0U,197U,120U,0U},
  {0U,198U,684U,0U},
  {0U,199U,170U,0U},
  {0U,200U,143U,0U},
  {0U,200U,183U,0U},
  {0U,200U,1703U,0U},
  {0U,201U,1U,0U},
  {0U,202U,2364U,0U},
  {0U,203U,2807U,0U},
  {0U,204U,2701U,0U},
  {0U,205U,48U,0U},
  {0U,206U,1U,0U},
  {0U,207U,93U,0U},
  {0U,208U,2824U,0U},
  {0U,209U,48U,0U},
  {0U,210U,2832U,0U},
  {0U,211U,212U,0U},
  {0U,212U,211U,0U},
  {0U,213U,4U,0U},
  {0U,213U,517U,0U},
  {0U,213U,1101U,0U},
  {0U,213U,1345U,0U},
  {0U,213U,2195U,0U},
  {0U,214U,2273U,0U},
  {0U,215U,304U,0U},
  {0U,215U,414U,0U},
  {0U,216U,144U,0U},
  {0U,216U,2857U,0U},
  {0U,217U,2616U,0U},
  {0U,218U,1177U,0U},
  {0U,218U,1307U,0U},
  {0U,218U,2116U,0U},
  {0U,218U,2277U,0U},
  {0U,218U,2585U,0U},
  {0U,218U,2902U,0U},
  {0U,218U,2914U,0U},
  {0U,218U,2924U,0U},
  {0U,219U,139U,0U},
  {0U,220U,642U,0U},
  {0U,221U,1409U,0U},
  {0U,222U,2940U,0U},
  {0U,223U,556U,0U},
  {0U,224U,2577U,0U},
  {0U,225U,846U,0U},
  {0U,225U,1204U,0U},
  {0U,226U,322U,0U},
  {0U,226U,1286U,0U},
  {0U,226U,1719U,0U},
  {0U,227U,1196U,0U},
  {0U,228U,192U,0U},
  {0U,229U,3U,0U},
  {0U,230U,239U,0U},
  {0U,230U,733U,0U},
  {0U,231U,2396U,0U},
  {0U,232U,411U,0U},
  {0U,233U,1129U,0U},
  {0U,234U,93U,0U},
  {0U,234U,402U,0U},
  {0U,235U,2975U,0U},
  {0U,236U,2847U,0U},
  {0U,237U,1096U,0U},
  {0U,238U,723U,0U},
  {0U,238U,2981U,0U},
  {0U,238U,2986U,0U},
  {0U,239U,230U,0U},
  {0U,240U,3003U,0U},
  {0U,241U,1112U,0U},
  {0U,242U,2271U,0U},
  {0U,243U,1730U,0U},
  {0U,243U,2184U,0U},
  {0U,244U,1461U,0U},
  {0U,244U,3006U,0U},
  {0U,245U,1056U,0U},
  {0U,246U,2148U,0U},
  {0U,247U,42U,0U},
  {0U,247U,462U,0U},
  {0U,247U,832U,0U},
  {0U,247U,2843U,0U},
  {0U,247U,3012U,0U},
  {0U,248U,259U,0U},
  {0U,249U,66U,0U},
  {0U,250U,3019U,0U},
  {0U,251U,53U,0U},
  {0U,252U,3022U,0U},
  {0U,253U,846U,0U},
  {0U,253U,1217U,0U},
  {0U,254U,1794U,0U},
  {0U,255U,3030U,0U},
  {0U,256U,2U,0U},
  {0U,257U,157U,0U},
  {0U,257U,1187U,0U},
  {0U,258U,89U,0U},
  {0U,258U,265U,0U},
  {0U,258U,1672U,0U},
  {0U,259U,248U,0U},
  {0U,260U,3065U,0U},
  {0U,261U,181U,0U},
  {0U,261U,1291U,0U},
  {0U,261U,1416U,0U},
  {0U,261U,3035U,0U},
  {0U,262U,1020U,0U},
  {0U,263U,566U,0U},
  {0U,263U,2151U,0U},
  {0U,263U,3089U,0U},
  {0U,264U,307U,0U},
  {0U,264U,421U,0U},
  {0U,264U,855U,0U},
  {0U,264U,977U,0U},
  {0U,264U,1767U,0U},
  {0U,264U,2052U,0U},
  {0U,264U,3098U,0U},
  {0U,265U,258U,0U},
  {0U,266U,1053U,0U},
  {0U,266U,1613U,0U},
  {0U,266U,1848U,0U},
  {0U,266U,1912U,0U},
  {0U,267U,304U,0U},
  {0U,267U,1227U,0U},
  {0U,267U,2397U,0U},
  {0U,268U,1901U,0U},
  {0U,269U,1621U,0U},
  {0U,269U,1834U,0U},
  {0U,269U,2119U,0U},
  {0U,270U,1063U,0U},
  {0U,271U,3129U,0U},
  {0U,271U,3132U,0U},
  {0U,272U,611U,0U},
  {0U,272U,1086U,0U},
  {0U,273U,1283U,0U},
  {0U,273U,2684U,0U},
  {0U,274U,1240U,0U},
  {0U,275U,584U,0U},
  {0U,275U,914U,0U},
  {0U,276U,2710U,0U},
  {0U,277U,427U,0U},
  {0U,278U,1765U,0U},
  {0U,279U,1854U,0U},
  {0U,280U,146U,0U},
  {0U,280U,2829U,0U},
  {0U,281U,696U,0U},
  {0U,282U,451U,0U},
  {0U,283U,633U,0U},
  {0U,284U,3031U,0U},
  {0U,285U,2450U,0U},
  {0U,285U,3164U,0U},
  {0U,286U,2867U,0U},
  {0U,287U,2804U,0U},
  {0U,288U,3174U,0U},
  {0U,289U,3182U,0U},
  {0U,290U,2213U,0U},
  {0U,291U,1313U,0U},
  {0U,292U,321U,0U},
  {0U,293U,419U,0U},
  {0U,293U,2126U,0U},
  {0U,294U,1721U,0U},
  {0U,295U,3216U,0U},
  {0U,295U,3217U,0U},
  {0U,296U,1339U,0U},
  {0U,296U,2899U,0U},
  {0U,297U,397U,0U},
  {0U,298U,3258U,0U},
  {0U,299U,2418U,0U},
  {0U,300U,1273U,0U},
  {0U,301U,2374U,0U},
  {0U,301U,2589U,0U},
  {0U,302U,3271U,0U},
  {0U,303U,3272U,0U},
  {0U,304U,215U,0U},
  {0U,304U,267U,0U},
  {0U,305U,3276U,0U},
  {0U,306U,53U,0U},
  {0U,307U,264U,0U},
  {0U,307U,2134U,0U},
  {0U,308U,3280U,0U},
  {0U,308U,3287U,0U},
  {0U,309U,2146U,0U},
  {0U,310U,140U,0U},
  {0U,311U,150U,0U},
  {0U,312U,1956U,0U},
  {0U,313U,2U,0U},
  {0U,313U,516U,0U},
  {0U,314U,191U,0U},
  {0U,315U,100U,0U},
  {0U,316U,1391U,0U},
  {0U,316U,1406U,0U},
  {0U,316U,1740U,0U},
  {0U,317U,517U,0U},
  {0U,318U,41U,0U},
  {0U,318U,3165U,0U},
  {0U,319U,1809U,0U},
  {0U,320U,1411U,0U},
  {0U,321U,292U,0U},
  {0U,321U,1128U,0U},
  {0U,321U,1981U,0U},
  {0U,321U,3247U,0U},
  {0U,321U,3266U,0U},
  {0U,322U,226U,0U},
  {0U,322U,1857U,0U},
  {0U,323U,3214U,0U},
  {0U,323U,3293U,0U},
  {0U,324U,2034U,0U},
  {0U,324U,2204U,0U},
  {0U,324U,2621U,0U},
  {0U,325U,592U,0U},
  {0U,325U,687U,0U},
  {0U,325U,1018U,0U},
  {0U,325U,1952U,0U},
  {0U,326U,2466U,0U},
  {0U,327U,1584U,0U},
  {0U,328U,459U,0U},
  {0U,328U,3300U,0U},
  {0U,329U,389U,0U},
  {0U,329U,592U,0U},
  {0U,330U,3377U,0U},
  {0U,331U,2773U,0U},
  {0U,332U,3290U,0U},
  {0U,332U,3379U,0U},
  {0U,333U,959U,0U},
  {0U,333U,1762U,0U},
  {0U,334U,2U,0U},
  {0U,335U,1349U,0U},
  {0U,336U,2201U,0U},
  {0U,336U,3385U,0U},
  {0U,336U,3386U,0U},
  {0U,337U,539U,0U},
  {0U,337U,795U,0U},
  {0U,338U,1401U,0U},
  {0U,338U,1760U,0U},
  {0U,339U,698U,0U},
  {0U,340U,355U,0U},
  {0U,341U,1743U,0U},
  {0U,341U,2602U,0U},
  {0U,341U,3248U,0U},
  {0U,341U,3253U,0U},
  {0U,342U,1320U,0U},
  {0U,342U,1344U,0U},
  {0U,343U,2730U,0U},
  {0U,343U,2802U,0U},
  {0U,343U,2900U,0U},
  {0U,344U,1402U,0U},
  {0U,344U,1813U,0U},
  {0U,344U,2055U,0U},
  {0U,345U,2168U,0U},
  {0U,345U,3408U,0U},
  {0U,346U,3316U,0U},
  {0U,347U,732U,0U},
  {0U,348U,1344U,0U},
  {0U,349U,53U,0U},
  {0U,350U,1524U,0U},
  {0U,350U,3412U,0U},
  {0U,351U,3413U,0U},
  {0U,352U,594U,0U},
  {0U,353U,767U,0U},
  {0U,354U,2622U,0U},
  {0U,354U,3416U,0U},
  {0U,355U,340U,0U},
  {0U,356U,1293U,0U},
  {0U,357U,973U,0U},
  {0U,357U,1897U,0U},
  {0U,357U,2223U,0U},
  {0U,358U,101U,0U},
  {0U,359U,2098U,0U},
  {0U,360U,3432U,0U},
  {0U,361U,1863U,0U},
  {0U,362U,2810U,0U},
  {0U,363U,1094U,0U},
  {0U,364U,1458U,0U},
  {0U,365U,868U,0U},
  {0U,366U,520U,0U},
  {0U,366U,1141U,0U},
  {0U,366U,1971U,0U},
  {0U,367U,1602U,0U},
  {0U,367U,2013U,0U},
  {0U,367U,2377U,0U},
  {0U,367U,3354U,0U},
  {0U,367U,3449U,0U},
  {0U,368U,136U,0U},
  {0U,368U,776U,0U},
  {0U,369U,2U,0U},
  {0U,370U,971U,0U},
  {0U,371U,171U,0U},
  {0U,371U,2883U,0U},
  {0U,372U,1836U,0U},
  {0U,373U,3459U,0U},
  {0U,374U,984U,0U},
  {0U,374U,1917U,0U},
  {0U,375U,70U,0U},
  {0U,376U,892U,0U},
  {0U,377U,575U,0U},
  {0U,378U,71U,0U},
  {0U,379U,1404U,0U},
  {0U,380U,1250U,0U},
  {0U,380U,1431U,0U},
  {0U,380U,1471U,0U},
  {0U,380U,2175U,0U},
  {0U,380U,3169U,0U},
  {0U,381U,851U,0U},
  {0U,382U,547U,0U},
  {0U,382U,608U,0U},
  {0U,382U,653U,0U},
  {0U,382U,992U,0U},
  {0U,382U,1010U,0U},
  {0U,382U,1164U,0U},
  {0U,382U,1874U,0U},
  {0U,382U,1875U,0U},
  {0U,382U,1950U,0U},
  {0U,382U,2378U,0U},
  {0U,382U,3167U,0U},
  {0U,383U,3090U,0U},
  {0U,383U,3487U,0U},
  {0U,384U,2U,0U},
  {0U,385U,1381U,0U},
  {0U,386U,2475U,0U},
  {0U,387U,2U,0U},
  {0U,388U,390U,0U},
  {0U,389U,53U,0U},
  {0U,389U,329U,0U},
  {0U,389U,1490U,0U},
  {0U,390U,2U,0U},
  {0U,390U,388U,0U},
  {0U,391U,2194U,0U},
  {0U,392U,120U,0U},
  {0U,393U,904U,0U},
  {0U,394U,3512U,0U},
  {0U,395U,2798U,0U},
  {0U,396U,2U,0U},
  {0U,397U,297U,0U},
  {0U,397U,2521U,0U},
  {0U,398U,727U,0U},
  {0U,398U,2601U,0U},
  {0U,399U,1648U,0U},
  {0U,399U,2875U,0U},
  {0U,400U,1371U,0U},
  {0U,401U,470U,0U},
  {0U,401U,2280U,0U},
  {0U,401U,3195U,0U},
  {0U,402U,234U,0U},
  {0U,403U,728U,0U},
  {0U,404U,1414U,0U},
  {0U,405U,765U,0U},
  {0U,405U,875U,0U},
  {0U,406U,152U,0U},
  {0U,407U,3350U,0U},
  {0U,408U,3494U,0U},
  {0U,409U,2078U,0U},
  {0U,409U,2362U,0U},
  {0U,410U,53U,0U},
  {0U,411U,232U,0U},
  {0U,412U,53U,0U},
  {0U,413U,580U,0U},
  {0U,414U,215U,0U},
  {0U,414U,530U,0U},
  {0U,414U,613U,0U},
  {0U,414U,2363U,0U},
  {0U,415U,191U,0U},
  {0U,416U,2234U,0U},
  {0U,417U,2793U,0U},
  {0U,418U,585U,0U},
  {0U,418U,601U,0U},
  {0U,419U,293U,0U},
  {0U,419U,3359U,0U},
  {0U,420U,1620U,0U},
  {0U,420U,1704U,0U},
  {0U,420U,3240U,0U},
  {0U,420U,3245U,0U},
  {0U,421U,264U,0U},
  {0U,422U,2099U,0U},
  {0U,423U,3372U,0U},
  {0U,423U,3552U,0U},
  {0U,424U,1587U,0U},
  {0U,425U,455U,0U},
  {0U,425U,2192U,0U},
  {0U,425U,2202U,0U},
  {0U,425U,2281U,0U},
  {0U,425U,3457U,0U},
  {0U,426U,783U,0U},
  {0U,427U,277U,0U},
  {0U,428U,532U,0U},
  {0U,429U,71U,0U},
  {0U,430U,146U,0U},
  {0U,431U,2712U,0U},
  {0U,432U,567U,0U},
  {0U,433U,784U,0U},
  {0U,434U,3567U,0U},
  {0U,434U,3568U,0U},
  {0U,435U,2820U,0U},
  {0U,436U,3356U,0U},
  {0U,437U,2037U,0U},
  {0U,438U,145U,0U},
  {0U,439U,1163U,0U},
  {0U,440U,1079U,0U},
  {0U,441U,3573U,0U},
  {0U,442U,1883U,0U},
  {0U,442U,2107U,0U},
  {0U,443U,3574U,0U},
  {0U,444U,3575U,0U},
  {0U,445U,1197U,0U},
  {0U,446U,120U,0U},
  {0U,447U,1293U,0U},
  {0U,448U,186U,0U},
  {0U,449U,1369U,0U},
  {0U,450U,2610U,0U},
  {0U,451U,282U,0U},
  {0U,452U,3U,0U},
  {0U,453U,3543U,0U},
  {0U,454U,1606U,0U},
  {0U,455U,425U,0U},
  {0U,456U,2463U,0U},
  {0U,457U,67U,0U},
  {0U,458U,3599U,0U},
  {0U,459U,328U,0U},
  {0U,460U,120U,0U},
  {0U,461U,1655U,0U},
  {0U,462U,247U,0U},
  {0U,463U,3146U,0U},
  {0U,464U,2342U,0U},
  {0U,465U,489U,0U},
  {0U,466U,1095U,0U},
  {0U,467U,3603U,0U},
  {0U,468U,513U,0U},
  {0U,469U,3U,0U},
  {0U,470U,401U,0U},
  {0U,470U,3147U,0U},
  {0U,471U,843U,0U},
  {0U,472U,2404U,0U},
  {0U,473U,42U,0U},
  {0U,474U,3U,0U},
  {0U,475U,120U,0U},
  {0U,476U,890U,0U},
  {0U,477U,120U,0U},
  {0U,478U,500U,0U},
  {0U,479U,2419U,0U},
  {0U,480U,1237U,0U},
  {0U,481U,1210U,0U},
  {0U,482U,3402U,0U},
  {0U,483U,866U,0U},
  {0U,484U,1397U,0U},
  {0U,485U,1396U,0U},
  {0U,486U,4U,0U},
  {0U,487U,2157U,0U},
  {0U,487U,2219U,0U},
  {0U,488U,1910U,0U},
  {0U,488U,2226U,0U},
  {0U,488U,2314U,0U},
  {0U,489U,465U,0U},
  {0U,490U,950U,0U},
  {0U,491U,3450U,0U},
  {0U,492U,825U,0U},
  {0U,493U,1894U,0U},
  {0U,494U,2221U,0U},
  {0U,495U,69U,0U},
  {0U,496U,1440U,0U},
  {0U,497U,870U,0U},
  {0U,497U,1038U,0U},
  {0U,497U,3553U,0U},
  {0U,498U,1814U,0U},
  {0U,499U,1412U,0U},
  {0U,499U,2584U,0U},
  {0U,500U,104U,0U},
  {0U,500U,478U,0U},
  {0U,500U,735U,0U},
  {0U,500U,2072U,0U},
  {0U,500U,2518U,0U},
  {0U,500U,3288U,0U},
  {0U,501U,819U,0U},
  {0U,502U,3531U,0U},
  {0U,502U,3635U,0U},
  {0U,503U,529U,0U},
  {0U,504U,127U,0U},
  {0U,504U,628U,0U},
  {0U,504U,2203U,0U},
  {0U,505U,3636U,0U},
  {0U,506U,574U,0U},
  {0U,507U,516U,0U},
  {0U,508U,3637U,0U},
  {0U,509U,870U,0U},
  {0U,510U,1449U,0U},
  {0U,511U,2094U,0U},
  {0U,511U,2772U,0U},
  {0U,512U,2196U,0U},
  {0U,513U,468U,0U},
  {0U,514U,600U,0U},
  {0U,515U,1221U,0U},
  {0U,515U,3647U,0U},
  {0U,516U,313U,0U},
  {0U,516U,507U,0U},
  {0U,516U,2091U,0U},
  {0U,516U,2225U,0U},
  {0U,516U,2438U,0U},
  {0U,516U,3648U,0U},
  {0U,517U,213U,0U},
  {0U,517U,317U,0U},
  {0U,517U,525U,0U},
  {0U,517U,771U,0U},
  {0U,517U,1157U,0U},
  {0U,518U,728U,0U},
  {0U,519U,1297U,0U},
  {0U,519U,3097U,0U},
  {0U,520U,366U,0U},
  {0U,521U,3653U,0U},
  {0U,522U,1811U,0U},
  {0U,523U,579U,0U},
  {0U,524U,593U,0U},
  {0U,525U,517U,0U},
  {0U,526U,825U,0U},
  {0U,527U,583U,0U},
  {0U,528U,894U,0U},
  {0U,528U,1856U,0U},
  {0U,528U,2132U,0U},
  {0U,529U,503U,0U},
  {0U,530U,414U,0U},
  {0U,531U,3658U,0U},
  {0U,532U,428U,0U},
  {0U,533U,3660U,0U},
  {0U,534U,576U,0U},
  {0U,535U,13U,0U},
  {0U,535U,804U,0U},
  {0U,536U,845U,0U},
  {0U,536U,2383U,0U},
  {0U,537U,869U,0U},
  {0U,538U,545U,0U},
  {0U,538U,605U,0U},
  {0U,538U,2118U,0U},
  {0U,539U,337U,0U},
  {0U,539U,628U,0U},
  {0U,539U,3150U,0U},
  {0U,540U,948U,0U},
  {0U,541U,2424U,0U},
  {0U,542U,858U,0U},
  {0U,543U,100U,0U},
  {0U,544U,1419U,0U},
  {0U,545U,538U,0U},
  {0U,545U,2252U,0U},
  {0U,546U,1144U,0U},
  {0U,547U,382U,0U},
  {0U,547U,597U,0U},
  {0U,548U,1493U,0U},
  {0U,549U,858U,0U},
  {0U,550U,2432U,0U},
  {0U,551U,76U,0U},
  {0U,552U,592U,0U},
  {0U,553U,75U,0U},
  {0U,553U,554U,0U},
  {0U,553U,622U,0U},
  {0U,553U,1127U,0U},
  {0U,553U,1410U,0U},
  {0U,554U,553U,0U},
  {0U,554U,725U,0U},
  {0U,555U,565U,0U},
  {0U,556U,223U,0U},
  {0U,556U,578U,0U},
  {0U,556U,3668U,0U},
  {0U,557U,990U,0U},
  {0U,558U,1835U,0U},
  {0U,559U,1491U,0U},
  {0U,560U,2013U,0U},
  {0U,561U,17U,0U},
  {0U,561U,3670U,0U},
  {0U,562U,596U,0U},
  {0U,562U,849U,0U},
  {0U,563U,910U,0U},
  {0U,564U,599U,0U},
  {0U,565U,555U,0U},
  {0U,566U,263U,0U},
  {0U,567U,432U,0U},
  {0U,568U,828U,0U},
  {0U,569U,2306U,0U},
  {0U,570U,621U,0U},
  {0U,571U,124U,0U},
  {0U,571U,1489U,0U},
  {0U,572U,871U,0U},
  {0U,573U,599U,0U},
  {0U,573U,1157U,0U},
  {0U,573U,2414U,0U},
  {0U,574U,506U,0U},
  {0U,575U,377U,0U},
  {0U,576U,534U,0U},
  {0U,577U,1899U,0U},
  {0U,578U,556U,0U},
  {0U,579U,523U,0U},
  {0U,580U,413U,0U},
  {0U,580U,878U,0U},
  {0U,580U,1321U,0U},
  {0U,581U,2177U,0U},
  {0U,582U,879U,0U},
  {0U,583U,527U,0U},
  {0U,584U,275U,0U},
  {0U,585U,418U,0U},
  {0U,586U,3104U,0U},
  {0U,587U,3686U,0U},
  {0U,588U,1040U,0U},
  {0U,589U,3681U,0U},
  {0U,590U,2498U,0U},
  {0U,591U,4U,0U},
  {0U,591U,68U,0U},
  {0U,592U,325U,0U},
  {0U,592U,329U,0U},
  {0U,592U,552U,0U},
  {0U,592U,805U,0U},
  {0U,592U,3321U,0U},
  {0U,593U,524U,0U},
  {0U,594U,352U,0U},
  {0U,595U,702U,0U},
  {0U,596U,562U,0U},
  {0U,597U,547U,0U},
  {0U,598U,1949U,0U},
  {0U,599U,564U,0U},
  {0U,599U,573U,0U},
  {0U,600U,514U,0U},
  {0U,601U,418U,0U},
  {0U,602U,90U,0U},
  {0U,603U,2752U,0U},
  {0U,604U,1282U,0U},
  {0U,605U,538U,0U},
  {0U,606U,1782U,0U},
  {0U,607U,85U,0U},
  {0U,608U,382U,0U},
  {0U,609U,25U,0U},
  {0U,610U,3518U,0U},
  {0U,611U,272U,0U},
  {0U,611U,1183U,0U},
  {0U,611U,2470U,0U},
  {0U,612U,2625U,0U},
  {0U,613U,414U,0U},
  {0U,614U,2267U,0U},
  {0U,615U,716U,0U},
  {0U,616U,147U,0U},
  {0U,617U,2016U,0U},
  {0U,618U,1503U,0U},
  {0U,619U,3698U,0U},
  {0U,620U,740U,0U},
  {0U,621U,570U,0U},
  {0U,621U,2818U,0U},
  {0U,622U,553U,0U},
  {0U,622U,989U,0U},
  {0U,623U,2769U,0U},
  {0U,624U,1004U,0U},
  {0U,624U,1009U,0U},
  {0U,625U,1927U,0U},
  {0U,626U,167U,0U},
  {0U,626U,808U,0U},
  {0U,626U,2646U,0U},
  {0U,627U,1812U,0U},
  {0U,628U,504U,0U},
  {0U,628U,539U,0U},
  {0U,629U,3711U,0U},
  {0U,630U,977U,0U},
  {0U,631U,636U,0U},
  {0U,631U,1148U,0U},
  {0U,632U,739U,0U},
  {0U,632U,775U,0U},
  {0U,632U,2001U,0U},
  {0U,632U,2081U,0U},
  {0U,632U,2147U,0U},
  {0U,633U,283U,0U},
  {0U,634U,645U,0U},
  {0U,635U,746U,0U},
  {0U,636U,631U,0U},
  {0U,637U,1263U,0U},
  {0U,638U,2367U,0U},
  {0U,639U,842U,0U},
  {0U,640U,1930U,0U},
  {0U,641U,44U,0U},
  {0U,642U,220U,0U},
  {0U,643U,792U,0U},
  {0U,644U,1365U,0U},
  {0U,644U,1795U,0U},
  {0U,645U,634U,0U},
  {0U,646U,2302U,0U},
  {0U,647U,28U,0U},
  {0U,648U,27U,0U},
  {0U,649U,97U,0U},
  {0U,650U,7U,0U},
  {0U,651U,3364U,0U},
  {0U,652U,100U,0U},
  {0U,652U,1323U,0U},
  {0U,653U,382U,0U},
  {0U,654U,1376U,0U},
  {0U,655U,2678U,0U},
  {0U,656U,1026U,0U},
  {0U,657U,1841U,0U},
  {0U,658U,3556U,0U},
  {0U,659U,692U,0U},
  {0U,660U,1873U,0U},
  {0U,661U,3550U,0U},
  {0U,662U,3732U,0U},
  {0U,663U,2987U,0U},
  {0U,664U,3367U,0U},
  {0U,665U,3168U,0U},
  {0U,666U,1051U,0U},
  {0U,666U,3366U,0U},
  {0U,666U,3466U,0U},
  {0U,667U,724U,0U},
  {0U,668U,1081U,0U},
  {0U,669U,2929U,0U},
  {0U,670U,1226U,0U},
  {0U,671U,1810U,0U},
  {0U,672U,2005U,0U},
  {0U,672U,3736U,0U},
  {0U,673U,9U,0U},
  {0U,674U,1859U,0U},
  {0U,675U,1202U,0U},
  {0U,675U,1465U,0U},
  {0U,676U,3739U,0U},
  {0U,677U,1935U,0U},
  {0U,677U,3740U,0U},
  {0U,678U,1373U,0U},
  {0U,679U,10U,0U},
  {0U,680U,10U,0U},
  {0U,681U,1051U,0U},
  {0U,682U,1192U,0U},
  {0U,682U,1926U,0U},
  {0U,682U,3155U,0U},
  {0U,683U,21U,0U},
  {0U,684U,198U,0U},
  {0U,685U,689U,0U},
  {0U,686U,2411U,0U},
  {0U,687U,325U,0U},
  {0U,688U,792U,0U},
  {0U,688U,3743U,0U},
  {0U,689U,685U,0U},
  {0U,690U,10U,0U},
  {0U,691U,143U,0U},
  {0U,692U,122U,0U},
  {0U,692U,659U,0U},
  {0U,693U,1780U,0U},
  {0U,694U,76U,0U},
  {0U,695U,1296U,0U},
  {0U,696U,281U,0U},
  {0U,697U,3744U,0U},
  {0U,698U,339U,0U},
  {0U,699U,1346U,0U},
  {0U,699U,2516U,0U},
  {0U,700U,76U,0U},
  {0U,701U,1215U,0U},
  {0U,702U,595U,0U},
  {0U,703U,3667U,0U},
  {0U,704U,5062U,0U},
  {0U,705U,2062U,0U},
  {0U,705U,4759U,0U},
  {0U,706U,813U,0U},
  {0U,707U,3748U,0U},
  {0U,708U,2946U,0U},
  {0U,709U,1135U,0U},
  {0U,710U,1824U,0U},
  {0U,710U,2064U,0U},
  {0U,711U,2034U,0U},
  {0U,712U,3314U,0U},
  {0U,713U,797U,0U},
  {0U,714U,886U,0U},
  {0U,714U,2412U,0U},
  {0U,714U,3031U,0U},
  {0U,715U,1843U,0U},
  {0U,715U,3120U,0U},
  {0U,716U,615U,0U},
  {0U,717U,11U,0U},
  {0U,718U,3752U,0U},
  {0U,719U,113U,0U},
  {0U,720U,988U,0U},
  {0U,720U,1457U,0U},
  {0U,721U,1292U,0U},
  {0U,722U,731U,0U},
  {0U,722U,758U,0U},
  {0U,722U,2255U,0U},
  {0U,722U,2628U,0U},
  {0U,722U,2821U,0U},
  {0U,722U,3756U,0U},
  {0U,723U,238U,0U},
  {0U,724U,667U,0U},
  {0U,725U,554U,0U},
  {0U,726U,2003U,0U},
  {0U,727U,398U,0U},
  {0U,728U,403U,0U},
  {0U,728U,518U,0U},
  {0U,728U,3608U,0U},
  {0U,729U,175U,0U},
  {0U,729U,3707U,0U},
  {0U,730U,737U,0U},
  {0U,731U,722U,0U},
  {0U,732U,347U,0U},
  {0U,733U,230U,0U},
  {0U,734U,2207U,0U},
  {0U,735U,500U,0U},
  {0U,736U,2260U,0U},
  {0U,736U,2262U,0U},
  {0U,737U,730U,0U},
  {0U,738U,3761U,0U},
  {0U,739U,632U,0U},
  {0U,740U,620U,0U},
  {0U,741U,82U,0U},
  {0U,742U,928U,0U},
  {0U,743U,923U,0U},
  {0U,744U,2361U,0U},
  {0U,745U,3766U,0U},
  {0U,746U,635U,0U},
  {0U,746U,819U,0U},
  {0U,747U,818U,0U},
  {0U,748U,123U,0U},
  {0U,749U,88U,0U},
  {0U,750U,88U,0U},
  {0U,751U,3767U,0U},
  {0U,752U,2270U,0U},
  {0U,753U,761U,0U},
  {0U,754U,1942U,0U},
  {0U,755U,759U,0U},
  {0U,756U,769U,0U},
  {0U,757U,2142U,0U},
  {0U,758U,722U,0U},
  {0U,759U,755U,0U},
  {0U,759U,3391U,0U},
  {0U,760U,1511U,0U},
  {0U,761U,753U,0U},
  {0U,761U,2272U,0U},
  {0U,762U,2035U,0U},
  {0U,763U,1304U,0U},
  {0U,764U,3770U,0U},
  {0U,765U,405U,0U},
  {0U,766U,163U,0U},
  {0U,767U,353U,0U},
  {0U,767U,1064U,0U},
  {0U,768U,3382U,0U},
  {0U,769U,756U,0U},
  {0U,770U,141U,0U},
  {0U,770U,3430U,0U},
  {0U,771U,517U,0U},
  {0U,771U,3085U,0U},
  {0U,772U,35U,0U},
  {0U,773U,71U,0U},
  {0U,774U,141U,0U},
  {0U,774U,785U,0U},
  {0U,775U,632U,0U},
  {0U,776U,368U,0U},
  {0U,777U,3139U,0U},
  {0U,778U,840U,0U},
  {0U,779U,59U,0U},
  {0U,780U,18U,0U},
  {0U,781U,1046U,0U},
  {0U,782U,1166U,0U},
  {0U,783U,426U,0U},
  {0U,784U,433U,0U},
  {0U,785U,774U,0U},
  {0U,786U,1141U,0U},
  {0U,787U,2539U,0U},
  {0U,788U,3332U,0U},
  {0U,789U,1790U,0U},
  {0U,790U,798U,0U},
  {0U,791U,2238U,0U},
  {0U,792U,643U,0U},
  {0U,792U,688U,0U},
  {0U,793U,1088U,0U},
  {0U,793U,1089U,0U},
  {0U,794U,844U,0U},
  {0U,795U,76U,0U},
  {0U,795U,337U,0U},
  {0U,796U,73U,0U},
  {0U,797U,713U,0U},
  {0U,797U,1384U,0U},
  {0U,797U,2171U,0U},
  {0U,797U,2215U,0U},
  {0U,798U,790U,0U},
  {0U,799U,3781U,0U},
  {0U,800U,3722U,0U},
  {0U,801U,810U,0U},
  {0U,801U,2218U,0U},
  {0U,802U,1229U,0U},
  {0U,803U,887U,0U},
  {0U,804U,535U,0U},
  {0U,805U,592U,0U},
  {0U,806U,1819U,0U},
  {0U,807U,950U,0U},
  {0U,808U,626U,0U},
  {0U,809U,95U,0U},
  {0U,810U,801U,0U},
  {0U,810U,1104U,0U},
  {0U,810U,1898U,0U},
  {0U,810U,2144U,0U},
  {0U,811U,2952U,0U},
  {0U,812U,2658U,0U},
  {0U,812U,3785U,0U},
  {0U,813U,706U,0U},
  {0U,814U,2254U,0U},
  {0U,815U,2076U,0U},
  {0U,816U,3725U,0U},
  {0U,817U,1230U,0U},
  {0U,818U,747U,0U},
  {0U,819U,501U,0U},
  {0U,819U,746U,0U},
  {0U,820U,2451U,0U},
  {0U,821U,12U,0U},
  {0U,822U,2699U,0U},
  {0U,823U,107U,0U},
  {0U,824U,826U,0U},
  {0U,825U,492U,0U},
  {0U,825U,526U,0U},
  {0U,826U,824U,0U},
  {0U,827U,833U,0U},
  {0U,828U,568U,0U},
  {0U,829U,2702U,0U},
  {0U,830U,1168U,0U},
  {0U,831U,168U,0U},
  {0U,832U,247U,0U},
  {0U,833U,827U,0U},
  {0U,834U,3793U,0U},
  {0U,835U,2788U,0U},
  {0U,836U,3654U,0U},
  {0U,837U,2698U,0U},
  {0U,838U,39U,0U},
  {0U,839U,120U,0U},
  {0U,839U,3313U,0U},
  {0U,840U,778U,0U},
  {0U,841U,2453U,0U},
  {0U,841U,3799U,0U},
  {0U,842U,639U,0U},
  {0U,843U,471U,0U},
  {0U,843U,3327U,0U},
  {0U,844U,137U,0U},
  {0U,844U,794U,0U},
  {0U,844U,1096U,0U},
  {0U,845U,536U,0U},
  {0U,846U,225U,0U},
  {0U,846U,253U,0U},
  {0U,847U,2685U,0U},
  {0U,848U,13U,0U},
  {0U,848U,2369U,0U},
  {0U,849U,562U,0U},
  {0U,850U,1807U,0U},
  {0U,851U,381U,0U},
  {0U,852U,3804U,0U},
  {0U,853U,3470U,0U},
  {0U,854U,978U,0U},
  {0U,855U,264U,0U},
  {0U,856U,1120U,0U},
  {0U,856U,2165U,0U},
  {0U,857U,1965U,0U},
  {0U,858U,542U,0U},
  {0U,858U,549U,0U},
  {0U,858U,1066U,0U},
  {0U,858U,2179U,0U},
  {0U,859U,3473U,0U},
  {0U,860U,111U,0U},
  {0U,861U,29U,0U},
  {0U,862U,2440U,0U},
  {0U,863U,1418U,0U},
  {0U,864U,1970U,0U},
  {0U,865U,2316U,0U},
  {0U,866U,483U,0U},
  {0U,867U,2639U,0U},
  {0U,867U,3809U,0U},
  {0U,868U,365U,0U},
  {0U,869U,537U,0U},
  {0U,870U,497U,0U},
  {0U,870U,509U,0U},
  {0U,871U,572U,0U},
  {0U,872U,1078U,0U},
  {0U,873U,1928U,0U},
  {0U,873U,3491U,0U},
  {0U,874U,2864U,0U},
  {0U,875U,98U,0U},
  {0U,875U,405U,0U},
  {0U,876U,1294U,0U},
  {0U,877U,2193U,0U},
  {0U,877U,3709U,0U},
  {0U,878U,580U,0U},
  {0U,879U,582U,0U},
  {0U,880U,931U,0U},
  {0U,880U,1516U,0U},
  {0U,881U,936U,0U},
  {0U,882U,166U,0U},
  {0U,882U,3411U,0U},
  {0U,883U,15U,0U},
  {0U,884U,1126U,0U},
  {0U,885U,3324U,0U},
  {0U,886U,714U,0U},
  {0U,886U,997U,0U},
  {0U,887U,803U,0U},
  {0U,888U,1231U,0U},
  {0U,889U,891U,0U},
  {0U,890U,476U,0U},
  {0U,891U,889U,0U},
  {0U,892U,376U,0U},
  {0U,893U,3824U,0U},
  {0U,894U,528U,0U},
  {0U,895U,1453U,0U},
  {0U,896U,2346U,0U},
  {0U,897U,2636U,0U},
  {0U,898U,921U,0U},
  {0U,899U,900U,0U},
  {0U,900U,899U,0U},
  {0U,901U,1574U,0U},
  {0U,902U,1326U,0U},
  {0U,903U,1188U,0U},
  {0U,904U,393U,0U},
  {0U,905U,1880U,0U},
  {0U,906U,1793U,0U},
  {0U,907U,1202U,0U},
  {0U,907U,3130U,0U},
  {0U,908U,100U,0U},
  {0U,909U,1114U,0U},
  {0U,910U,563U,0U},
  {0U,911U,962U,0U},
  {0U,912U,2869U,0U},
  {0U,913U,109U,0U},
  {0U,914U,275U,0U},
  {0U,914U,1962U,0U},
  {0U,914U,2139U,0U},
  {0U,915U,26U,0U},
  {0U,916U,3832U,0U},
  {0U,917U,1798U,0U},
  {0U,918U,1318U,0U},
  {0U,919U,920U,0U},
  {0U,920U,919U,0U},
  {0U,921U,898U,0U},
  {0U,922U,3763U,0U},
  {0U,922U,3835U,0U},
  {0U,922U,3836U,0U},
  {0U,923U,743U,0U},
  {0U,924U,926U,0U},
  {0U,925U,3837U,0U},
  {0U,926U,924U,0U},
  {0U,927U,1224U,0U},
  {0U,927U,3319U,0U},
  {0U,928U,742U,0U},
  {0U,929U,3682U,0U},
  {0U,930U,3593U,0U},
  {0U,931U,880U,0U},
  {0U,931U,2399U,0U},
  {0U,932U,1498U,0U},
  {0U,933U,935U,0U},
  {0U,934U,1319U,0U},
  {0U,934U,1525U,0U},
  {0U,935U,933U,0U},
  {0U,936U,881U,0U},
  {0U,937U,1529U,0U},
  {0U,937U,3279U,0U},
  {0U,938U,1142U,0U},
  {0U,939U,1109U,0U},
  {0U,940U,1420U,0U},
  {0U,941U,946U,0U},
  {0U,942U,2308U,0U},
  {0U,943U,1073U,0U},
  {0U,944U,2319U,0U},
  {0U,945U,3325U,0U},
  {0U,946U,941U,0U},
  {0U,947U,3821U,0U},
  {0U,948U,540U,0U},
  {0U,949U,2297U,0U},
  {0U,950U,490U,0U},
  {0U,950U,807U,0U},
  {0U,951U,45U,0U},
  {0U,952U,1228U,0U},
  {0U,952U,2408U,0U},
  {0U,952U,2410U,0U},
  {0U,953U,2020U,0U},
  {0U,954U,1826U,0U},
  {0U,955U,1386U,0U},
  {0U,956U,1793U,0U},
  {0U,957U,132U,0U},
  {0U,958U,56U,0U},
  {0U,959U,333U,0U},
  {0U,960U,3580U,0U},
  {0U,961U,118U,0U},
  {0U,962U,911U,0U},
  {0U,963U,3011U,0U},
  {0U,964U,1829U,0U},
  {0U,965U,1756U,0U},
  {0U,966U,1174U,0U},
  {0U,967U,16U,0U},
  {0U,968U,1098U,0U},
  {0U,969U,2184U,0U},
  {0U,970U,3853U,0U},
  {0U,971U,370U,0U},
  {0U,971U,2524U,0U},
  {0U,971U,2530U,0U},
  {0U,972U,17U,0U},
  {0U,973U,357U,0U},
  {0U,974U,3700U,0U},
  {0U,975U,3534U,0U},
  {0U,975U,3828U,0U},
  {0U,976U,2039U,0U},
  {0U,977U,264U,0U},
  {0U,977U,630U,0U},
  {0U,977U,2120U,0U},
  {0U,978U,854U,0U},
  {0U,978U,2186U,0U},
  {0U,979U,1185U,0U},
  {0U,980U,2583U,0U},
  {0U,981U,2766U,0U},
  {0U,982U,2256U,0U},
  {0U,983U,17U,0U},
  {0U,984U,374U,0U},
  {0U,984U,1247U,0U},
  {0U,985U,1012U,0U},
  {0U,986U,3328U,0U},
  {0U,987U,2673U,0U},
  {0U,987U,3126U,0U},
  {0U,988U,720U,0U},
  {0U,989U,622U,0U},
  {0U,990U,557U,0U},
  {0U,990U,1121U,0U},
  {0U,990U,3870U,0U},
  {0U,991U,3548U,0U},
  {0U,992U,382U,0U},
  {0U,993U,3592U,0U},
  {0U,994U,166U,0U},
  {0U,994U,3872U,0U},
  {0U,995U,3105U,0U},
  {0U,995U,3454U,0U},
  {0U,996U,1439U,0U},
  {0U,997U,886U,0U},
  {0U,998U,3876U,0U},
  {0U,999U,2823U,0U},
  {0U,1000U,3747U,0U},
  {0U,1001U,134U,0U},
  {0U,1002U,2626U,0U},
  {0U,1003U,2137U,0U},
  {0U,1004U,624U,0U},
  {0U,1004U,3881U,0U},
  {0U,1005U,1019U,0U},
  {0U,1006U,3277U,0U},
  {0U,1007U,2122U,0U},
  {0U,1008U,1441U,0U},
  {0U,1009U,624U,0U},
  {0U,1010U,382U,0U},
  {0U,1011U,3371U,0U},
  {0U,1012U,985U,0U},
  {0U,1013U,3884U,0U},
  {0U,1014U,1099U,0U},
  {0U,1014U,1853U,0U},
  {0U,1015U,3508U,0U},
  {0U,1016U,2102U,0U},
  {0U,1017U,2664U,0U},
  {0U,1018U,325U,0U},
  {0U,1019U,1005U,0U},
  {0U,1020U,262U,0U},
  {0U,1021U,1446U,0U},
  {0U,1022U,2517U,0U},
  {0U,1023U,3807U,0U},
  {0U,1024U,3890U,0U},
  {0U,1025U,79U,0U},
  {0U,1026U,656U,0U},
  {0U,1027U,3892U,0U},
  {0U,1028U,1158U,0U},
  {0U,1029U,3737U,0U},
  {0U,1030U,2119U,0U},
  {0U,1031U,3895U,0U},
  {0U,1032U,2006U,0U},
  {0U,1033U,3315U,0U},
  {0U,1034U,2049U,0U},
  {0U,1035U,3900U,0U},
  {0U,1036U,3791U,0U},
  {0U,1037U,1145U,0U},
  {0U,1038U,497U,0U},
  {0U,1039U,71U,0U},
  {0U,1040U,588U,0U},
  {0U,1041U,1845U,0U},
  {0U,1041U,2371U,0U},
  {0U,1042U,3904U,0U},
  {0U,1043U,1556U,0U},
  {0U,1044U,1070U,0U},
  {0U,1045U,2564U,0U},
  {0U,1046U,781U,0U},
  {0U,1047U,1068U,0U},
  {0U,1048U,2973U,0U},
  {0U,1049U,1072U,0U},
  {0U,1050U,1931U,0U},
  {0U,1050U,2067U,0U},
  {0U,1051U,666U,0U},
  {0U,1051U,681U,0U},
  {0U,1052U,18U,0U},
  {0U,1053U,266U,0U},
  {0U,1054U,2426U,0U},
  {0U,1055U,3784U,0U},
  {0U,1056U,245U,0U},
  {0U,1057U,61U,0U},
  {0U,1058U,2191U,0U},
  {0U,1059U,3911U,0U},
  {0U,1060U,1203U,0U},
  {0U,1061U,3899U,0U},
  {0U,1062U,1991U,0U},
  {0U,1063U,270U,0U},
  {0U,1064U,767U,0U},
  {0U,1065U,72U,0U},
  {0U,1066U,858U,0U},
  {0U,1067U,3913U,0U},
  {0U,1068U,1047U,0U},
  {0U,1069U,2915U,0U},
  {0U,1070U,1044U,0U},
  {0U,1071U,1889U,0U},
  {0U,1072U,1049U,0U},
  {0U,1073U,943U,0U},
  {0U,1074U,1564U,0U},
  {0U,1075U,58U,0U},
  {0U,1076U,2070U,0U},
  {0U,1077U,2275U,0U},
  {0U,1078U,872U,0U},
  {0U,1079U,440U,0U},
  {0U,1079U,2370U,0U},
  {0U,1079U,3513U,0U},
  {0U,1080U,1792U,0U},
  {0U,1081U,668U,0U},
  {0U,1081U,3472U,0U},
  {0U,1082U,169U,0U},
  {0U,1083U,3922U,0U},
  {0U,1084U,2505U,0U},
  {0U,1085U,53U,0U},
  {0U,1085U,2185U,0U},
  {0U,1086U,272U,0U},
  {0U,1087U,19U,0U},
  {0U,1088U,793U,0U},
  {0U,1089U,793U,0U},
  {0U,1090U,3123U,0U},
  {0U,1091U,119U,0U},
  {0U,1092U,20U,0U},
  {0U,1093U,20U,0U},
  {0U,1094U,363U,0U},
  {0U,1095U,466U,0U},
  {0U,1095U,1212U,0U},
  {0U,1095U,3925U,0U},
  {0U,1096U,237U,0U},
  {0U,1096U,844U,0U},
  {0U,1096U,1735U,0U},
  {0U,1096U,2372U,0U},
  {0U,1097U,2462U,0U},
  {0U,1098U,968U,0U},
  {0U,1099U,1014U,0U},
  {0U,1099U,1199U,0U},
  {0U,1099U,2436U,0U},
  {0U,1100U,3931U,0U},
  {0U,1101U,120U,0U},
  {0U,1101U,213U,0U},
  {0U,1102U,3934U,0U},
  {0U,1103U,1211U,0U},
  {0U,1104U,810U,0U},
  {0U,1105U,22U,0U},
  {0U,1106U,3578U,0U},
  {0U,1107U,1574U,0U},
  {0U,1108U,2289U,0U},
  {0U,1109U,939U,0U},
  {0U,1110U,3937U,0U},
  {0U,1111U,2491U,0U},
  {0U,1111U,3939U,0U},
  {0U,1112U,241U,0U},
  {0U,1113U,23U,0U},
  {0U,1114U,909U,0U},
  {0U,1115U,2170U,0U},
  {0U,1116U,1377U,0U},
  {0U,1117U,2704U,0U},
  {0U,1118U,1425U,0U},
  {0U,1118U,2583U,0U},
  {0U,1119U,2176U,0U},
  {0U,1120U,856U,0U},
  {0U,1121U,990U,0U},
  {0U,1122U,1613U,0U},
  {0U,1123U,1218U,0U},
  {0U,1124U,1815U,0U},
  {0U,1125U,3729U,0U},
  {0U,1126U,884U,0U},
  {0U,1127U,553U,0U},
  {0U,1128U,321U,0U},
  {0U,1129U,233U,0U},
  {0U,1130U,39U,0U},
  {0U,1131U,1871U,0U},
  {0U,1132U,1225U,0U},
  {0U,1133U,1801U,0U},
  {0U,1134U,2905U,0U},
  {0U,1135U,709U,0U},
  {0U,1135U,1900U,0U},
  {0U,1136U,1757U,0U},
  {0U,1137U,3898U,0U},
  {0U,1138U,133U,0U},
  {0U,1139U,104U,0U},
  {0U,1140U,3847U,0U},
  {0U,1141U,366U,0U},
  {0U,1141U,786U,0U},
  {0U,1142U,938U,0U},
  {0U,1143U,2065U,0U},
  {0U,1144U,546U,0U},
  {0U,1145U,102U,0U},
  {0U,1145U,1037U,0U},
  {0U,1146U,3021U,0U},
  {0U,1147U,1214U,0U},
  {0U,1148U,631U,0U},
  {0U,1149U,1600U,0U},
  {0U,1150U,3916U,0U},
  {0U,1151U,2878U,0U},
  {0U,1152U,1153U,0U},
  {0U,1153U,1152U,0U},
  {0U,1154U,3844U,0U},
  {0U,1155U,185U,0U},
  {0U,1156U,1432U,0U},
  {0U,1157U,517U,0U},
  {0U,1157U,573U,0U},
  {0U,1157U,2018U,0U},
  {0U,1158U,1028U,0U},
  {0U,1158U,1243U,0U},
  {0U,1159U,3940U,0U},
  {0U,1160U,3778U,0U},
  {0U,1161U,3952U,0U},
  {0U,1162U,1417U,0U},
  {0U,1163U,439U,0U},
  {0U,1163U,3384U,0U},
  {0U,1163U,3953U,0U},
  {0U,1164U,382U,0U},
  {0U,1165U,1438U,0U},
  {0U,1165U,2438U,0U},
  {0U,1166U,782U,0U},
  {0U,1167U,104U,0U},
  {0U,1168U,830U,0U},
  {0U,1169U,3954U,0U},
  {0U,1170U,1908U,0U},
  {0U,1171U,3955U,0U},
  {0U,1172U,3956U,0U},
  {0U,1173U,54U,0U},
  {0U,1174U,966U,0U},
  {0U,1175U,2309U,0U},
  {0U,1176U,192U,0U},
  {0U,1177U,218U,0U},
  {0U,1178U,3960U,0U},
  {0U,1179U,27U,0U},
  {0U,1180U,1677U,0U},
  {0U,1181U,3961U,0U},
  {0U,1182U,1633U,0U},
  {0U,1183U,611U,0U},
  {0U,1184U,2298U,0U},
  {0U,1185U,979U,0U},
  {0U,1186U,1239U,0U},
  {0U,1187U,257U,0U},
  {0U,1188U,903U,0U},
  {0U,1188U,3201U,0U},
  {0U,1188U,3963U,0U},
  {0U,1189U,124U,0U},
  {0U,1189U,1975U,0U},
  {0U,1190U,1481U,0U},
  {0U,1191U,2188U,0U},
  {0U,1192U,682U,0U},
  {0U,1193U,3140U,0U},
  {0U,1194U,3966U,0U},
  {0U,1195U,28U,0U},
  {0U,1196U,227U,0U},
  {0U,1197U,445U,0U},
  {0U,1198U,2311U,0U},
  {0U,1199U,1099U,0U},
  {0U,1200U,3605U,0U},
  {0U,1201U,1520U,0U},
  {0U,1202U,675U,0U},
  {0U,1202U,907U,0U},
  {0U,1202U,2286U,0U},
  {0U,1203U,1060U,0U},
  {0U,1204U,225U,0U},
  {0U,1205U,1207U,0U},
  {0U,1206U,2365U,0U},
  {0U,1207U,1205U,0U},
  {0U,1208U,49U,0U},
  {0U,1208U,1306U,0U},
  {0U,1209U,31U,0U},
  {0U,1210U,481U,0U},
  {0U,1211U,1103U,0U},
  {0U,1212U,1095U,0U},
  {0U,1213U,2609U,0U},
  {0U,1214U,1147U,0U},
  {0U,1215U,701U,0U},
  {0U,1216U,1223U,0U},
  {0U,1217U,253U,0U},
  {0U,1218U,1123U,0U},
  {0U,1219U,32U,0U},
  {0U,1220U,32U,0U},
  {0U,1221U,515U,0U},
  {0U,1222U,1830U,0U},
  {0U,1223U,1216U,0U},
  {0U,1224U,927U,0U},
  {0U,1225U,1132U,0U},
  {0U,1225U,3308U,0U},
  {0U,1226U,670U,0U},
  {0U,1227U,267U,0U},
  {0U,1228U,952U,0U},
  {0U,1229U,802U,0U},
  {0U,1230U,817U,0U},
  {0U,1231U,888U,0U},
  {0U,1232U,3318U,0U},
  {0U,1233U,1284U,0U},
  {0U,1234U,115U,0U},
  {0U,1235U,1822U,0U},
  {0U,1236U,2230U,0U},
  {0U,1237U,480U,0U},
  {0U,1238U,2320U,0U},
  {0U,1239U,1186U,0U},
  {0U,1240U,274U,0U},
  {0U,1241U,1475U,0U},
  {0U,1242U,33U,0U},
  {0U,1243U,1158U,0U},
  {0U,1244U,2014U,0U},
  {0U,1245U,3978U,0U},
  {0U,1246U,2850U,0U},
  {0U,1247U,984U,0U},
  {0U,1248U,3664U,0U},
  {0U,1249U,85U,0U},
  {0U,1250U,380U,0U},
  {0U,1250U,3261U,0U},
  {0U,1251U,1980U,0U},
  {0U,1252U,3237U,0U},
  {0U,1253U,2600U,0U},
  {0U,1254U,1277U,0U},
  {0U,1254U,4000U,0U},
  {0U,1255U,1764U,0U},
  {0U,1255U,3015U,0U},
  {0U,1256U,34U,0U},
  {0U,1257U,1331U,0U},
  {0U,1258U,1327U,0U},
  {0U,1259U,34U,0U},
  {0U,1259U,4005U,0U},
  {0U,1260U,1281U,0U},
  {0U,1261U,1280U,0U},
  {0U,1262U,1977U,0U},
  {0U,1263U,637U,0U},
  {0U,1263U,3292U,0U},
  {0U,1264U,1271U,0U},
  {0U,1265U,1731U,0U},
  {0U,1266U,3338U,0U},
  {0U,1267U,1977U,0U},
  {0U,1268U,1337U,0U},
  {0U,1269U,1565U,0U},
  {0U,1270U,1279U,0U},
  {0U,1271U,1264U,0U},
  {0U,1272U,3343U,0U},
  {0U,1273U,300U,0U},
  {0U,1274U,3202U,0U},
  {0U,1274U,4020U,0U},
  {0U,1275U,2598U,0U},
  {0U,1275U,3547U,0U},
  {0U,1276U,2591U,0U},
  {0U,1277U,1254U,0U},
  {0U,1278U,4029U,0U},
  {0U,1279U,1270U,0U},
  {0U,1280U,34U,0U},
  {0U,1280U,1261U,0U},
  {0U,1281U,1260U,0U},
  {0U,1281U,2606U,0U},
  {0U,1282U,604U,0U},
  {0U,1283U,273U,0U},
  {0U,1284U,1233U,0U},
  {0U,1285U,120U,0U},
  {0U,1286U,226U,0U},
  {0U,1287U,2128U,0U},
  {0U,1288U,146U,0U},
  {0U,1288U,3820U,0U},
  {0U,1289U,2131U,0U},
  {0U,1290U,3583U,0U},
  {0U,1291U,261U,0U},
  {0U,1292U,721U,0U},
  {0U,1292U,1299U,0U},
  {0U,1293U,356U,0U},
  {0U,1293U,447U,0U},
  {0U,1293U,2967U,0U},
  {0U,1294U,876U,0U},
  {0U,1295U,1667U,0U},
  {0U,1295U,3623U,0U},
  {0U,1296U,695U,0U},
  {0U,1297U,519U,0U},
  {0U,1298U,4031U,0U},
  {0U,1299U,1292U,0U},
  {0U,1300U,37U,0U},
  {0U,1301U,38U,0U},
  {0U,1302U,1827U,0U},
  {0U,1303U,4032U,0U},
  {0U,1304U,763U,0U},
  {0U,1305U,1477U,0U},
  {0U,1306U,1208U,0U},
  {0U,1307U,218U,0U},
  {0U,1308U,3199U,0U},
  {0U,1309U,2917U,0U},
  {0U,1310U,2514U,0U},
  {0U,1311U,2663U,0U},
  {0U,1312U,3329U,0U},
  {0U,1313U,291U,0U},
  {0U,1314U,4038U,0U},
  {0U,1315U,3798U,0U},
  {0U,1316U,39U,0U},
  {0U,1317U,3848U,0U},
  {0U,1318U,918U,0U},
  {0U,1319U,934U,0U},
  {0U,1320U,40U,0U},
  {0U,1320U,342U,0U},
  {0U,1321U,580U,0U},
  {0U,1322U,3968U,0U},
  {0U,1323U,43U,0U},
  {0U,1323U,652U,0U},
  {0U,1324U,2246U,0U},
  {0U,1325U,4042U,0U},
  {0U,1326U,902U,0U},
  {0U,1327U,1258U,0U},
  {0U,1328U,1329U,0U},
  {0U,1329U,1328U,0U},
  {0U,1330U,4058U,0U},
  {0U,1331U,1257U,0U},
  {0U,1332U,2056U,0U},
  {0U,1333U,4064U,0U},
  {0U,1334U,1980U,0U},
  {0U,1334U,4065U,0U},
  {0U,1335U,3999U,0U},
  {0U,1336U,2770U,0U},
  {0U,1337U,1268U,0U},
  {0U,1338U,3340U,0U},
  {0U,1339U,296U,0U},
  {0U,1340U,4073U,0U},
  {0U,1341U,2046U,0U},
  {0U,1342U,1500U,0U},
  {0U,1343U,120U,0U},
  {0U,1344U,342U,0U},
  {0U,1344U,348U,0U},
  {0U,1345U,213U,0U},
  {0U,1346U,699U,0U},
  {0U,1347U,4076U,0U},
  {0U,1348U,3640U,0U},
  {0U,1349U,335U,0U},
  {0U,1350U,42U,0U},
  {0U,1351U,3665U,0U},
  {0U,1352U,1353U,0U},
  {0U,1353U,1352U,0U},
  {0U,1354U,2282U,0U},
  {0U,1355U,2714U,0U},
  {0U,1356U,3928U,0U},
  {0U,1357U,4083U,0U},
  {0U,1358U,124U,0U},
  {0U,1359U,120U,0U},
  {0U,1360U,106U,0U},
  {0U,1361U,1497U,0U},
  {0U,1362U,4085U,0U},
  {0U,1363U,2080U,0U},
  {0U,1364U,4086U,0U},
  {0U,1365U,644U,0U},
  {0U,1365U,2415U,0U},
  {0U,1366U,42U,0U},
  {0U,1367U,1818U,0U},
  {0U,1368U,1385U,0U},
  {0U,1369U,449U,0U},
  {0U,1370U,4089U,0U},
  {0U,1371U,400U,0U},
  {0U,1372U,2090U,0U},
  {0U,1373U,678U,0U},
  {0U,1374U,3609U,0U},
  {0U,1375U,4090U,0U},
  {0U,1376U,654U,0U},
  {0U,1377U,1116U,0U},
  {0U,1378U,83U,0U},
  {0U,1379U,4091U,0U},
  {0U,1380U,1392U,0U},
  {0U,1381U,385U,0U},
  {0U,1382U,2882U,0U},
  {0U,1383U,3950U,0U},
  {0U,1384U,797U,0U},
  {0U,1385U,1368U,0U},
  {0U,1385U,1816U,0U},
  {0U,1386U,955U,0U},
  {0U,1387U,2479U,0U},
  {0U,1387U,3626U,0U},
  {0U,1388U,1797U,0U},
  {0U,1389U,2971U,0U},
  {0U,1390U,3768U,0U},
  {0U,1391U,316U,0U},
  {0U,1392U,1380U,0U},
  {0U,1393U,46U,0U},
  {0U,1393U,3099U,0U},
  {0U,1394U,3452U,0U},
  {0U,1395U,2169U,0U},
  {0U,1396U,485U,0U},
  {0U,1397U,484U,0U},
  {0U,1398U,2101U,0U},
  {0U,1399U,4100U,0U},
  {0U,1400U,1746U,0U},
  {0U,1401U,338U,0U},
  {0U,1402U,344U,0U},
  {0U,1403U,3196U,0U},
  {0U,1404U,379U,0U},
  {0U,1405U,3530U,0U},
  {0U,1406U,316U,0U},
  {0U,1407U,2620U,0U},
  {0U,1408U,53U,0U},
  {0U,1409U,221U,0U},
  {0U,1409U,1817U,0U},
  {0U,1410U,553U,0U},
  {0U,1411U,320U,0U},
  {0U,1412U,499U,0U},
  {0U,1413U,3719U,0U},
  {0U,1414U,404U,0U},
  {0U,1415U,98U,0U},
  {0U,1416U,261U,0U},
  {0U,1417U,1162U,0U},
  {0U,1418U,863U,0U},
  {0U,1419U,544U,0U},
  {0U,1420U,940U,0U},
  {0U,1421U,1827U,0U},
  {0U,1422U,2458U,0U},
  {0U,1423U,1427U,0U},
  {0U,1424U,3422U,0U},
  {0U,1424U,3634U,0U},
  {0U,1425U,1118U,0U},
  {0U,1426U,124U,0U},
  {0U,1427U,1423U,0U},
  {0U,1428U,1429U,0U},
  {0U,1429U,1428U,0U},
  {0U,1430U,1431U,0U},
  {0U,1431U,380U,0U},
  {0U,1431U,1430U,0U},
  {0U,1432U,1156U,0U},
  {0U,1433U,1758U,0U},
  {0U,1434U,47U,0U},
  {0U,1435U,1441U,0U},
  {0U,1436U,3981U,0U},
  {0U,1437U,3838U,0U},
  {0U,1438U,1165U,0U},
  {0U,1439U,996U,0U},
  {0U,1440U,496U,0U},
  {0U,1441U,1008U,0U},
  {0U,1441U,1435U,0U},
  {0U,1442U,1638U,0U},
  {0U,1443U,2258U,0U},
  {0U,1444U,2012U,0U},
  {0U,1445U,98U,0U},
  {0U,1446U,1021U,0U},
  {0U,1447U,1619U,0U},
  {0U,1447U,2670U,0U},
  {0U,1448U,4117U,0U},
  {0U,1449U,510U,0U},
  {0U,1450U,2214U,0U},
  {0U,1451U,1647U,0U},
  {0U,1452U,4122U,0U},
  {0U,1453U,895U,0U},
  {0U,1454U,3375U,0U},
  {0U,1455U,4010U,0U},
  {0U,1456U,120U,0U},
  {0U,1457U,720U,0U},
  {0U,1458U,364U,0U},
  {0U,1459U,4129U,0U},
  {0U,1460U,3703U,0U},
  {0U,1461U,244U,0U},
  {0U,1462U,2611U,0U},
  {0U,1463U,3628U,0U},
  {0U,1464U,2817U,0U},
  {0U,1465U,675U,0U},
  {0U,1466U,1548U,0U},
  {0U,1467U,98U,0U},
  {0U,1468U,1652U,0U},
  {0U,1469U,156U,0U},
  {0U,1470U,48U,0U},
  {0U,1471U,380U,0U},
  {0U,1472U,4141U,0U},
  {0U,1473U,4142U,0U},
  {0U,1474U,4143U,0U},
  {0U,1475U,1241U,0U},
  {0U,1476U,4146U,0U},
  {0U,1477U,1305U,0U},
  {0U,1478U,4147U,0U},
  {0U,1479U,2355U,0U},
  {0U,1480U,1754U,0U},
  {0U,1481U,1190U,0U},
  {0U,1482U,3561U,0U},
  {0U,1483U,4155U,0U},
  {0U,1484U,1596U,0U},
  {0U,1485U,112U,0U},
  {0U,1486U,2641U,0U},
  {0U,1487U,1925U,0U},
  {0U,1488U,169U,0U},
  {0U,1489U,571U,0U},
  {0U,1490U,389U,0U},
  {0U,1491U,559U,0U},
  {0U,1492U,50U,0U},
  {0U,1493U,548U,0U},
  {0U,1494U,2421U,0U},
  {0U,1495U,2294U,0U},
  {0U,1496U,186U,0U},
  {0U,1497U,52U,0U},
  {0U,1497U,1361U,0U},
  {0U,1498U,932U,0U},
  {0U,1499U,3257U,0U},
  {0U,1499U,4158U,0U},
  {0U,1500U,1342U,0U},
  {0U,1501U,3649U,0U},
  {0U,1502U,2441U,0U},
  {0U,1503U,618U,0U},
  {0U,1504U,1530U,0U},
  {0U,1505U,2779U,0U},
  {0U,1506U,1513U,0U},
  {0U,1507U,124U,0U},
  {0U,1508U,1517U,0U},
  {0U,1509U,1510U,0U},
  {0U,1510U,1509U,0U},
  {0U,1511U,760U,0U},
  {0U,1512U,1515U,0U},
  {0U,1513U,1506U,0U},
  {0U,1514U,4163U,0U},
  {0U,1515U,1512U,0U},
  {0U,1516U,880U,0U},
  {0U,1517U,1508U,0U},
  {0U,1518U,4164U,0U},
  {0U,1519U,116U,0U},
  {0U,1520U,1201U,0U},
  {0U,1521U,1526U,0U},
  {0U,1522U,2801U,0U},
  {0U,1523U,1528U,0U},
  {0U,1524U,350U,0U},
  {0U,1524U,1852U,0U},
  {0U,1525U,934U,0U},
  {0U,1526U,1521U,0U},
  {0U,1527U,71U,0U},
  {0U,1528U,1523U,0U},
  {0U,1529U,937U,0U},
  {0U,1530U,1504U,0U},
  {0U,1531U,2422U,0U},
  {0U,1532U,52U,0U},
  {0U,1533U,2921U,0U},
  {0U,1534U,1599U,0U},
  {0U,1535U,3576U,0U},
  {0U,1535U,3577U,0U},
  {0U,1536U,4169U,0U},
  {0U,1537U,4174U,0U},
  {0U,1538U,4176U,0U},
  {0U,1539U,3060U,0U},
  {0U,1540U,2531U,0U},
  {0U,1541U,1543U,0U},
  {0U,1542U,1778U,0U},
  {0U,1542U,3058U,0U},
  {0U,1542U,3059U,0U},
  {0U,1542U,3062U,0U},
  {0U,1543U,1541U,0U},
  {0U,1544U,1553U,0U},
  {0U,1545U,2979U,0U},
  {0U,1546U,3039U,0U},
  {0U,1547U,53U,0U},
  {0U,1548U,1466U,0U},
  {0U,1549U,4190U,0U},
  {0U,1550U,1603U,0U},
  {0U,1550U,4191U,0U},
  {0U,1551U,53U,0U},
  {0U,1552U,3041U,0U},
  {0U,1553U,1544U,0U},
  {0U,1554U,4189U,0U},
  {0U,1555U,1687U,0U},
  {0U,1556U,1043U,0U},
  {0U,1556U,2151U,0U},
  {0U,1557U,4202U,0U},
  {0U,1558U,53U,0U},
  {0U,1559U,4213U,0U},
  {0U,1560U,3206U,0U},
  {0U,1561U,53U,0U},
  {0U,1562U,2507U,0U},
  {0U,1563U,1803U,0U},
  {0U,1564U,1074U,0U},
  {0U,1565U,1269U,0U},
  {0U,1566U,4233U,0U},
  {0U,1567U,53U,0U},
  {0U,1568U,1569U,0U},
  {0U,1569U,1568U,0U},
  {0U,1570U,2500U,0U},
  {0U,1571U,2976U,0U},
  {0U,1572U,4245U,0U},
  {0U,1573U,4249U,0U},
  {0U,1574U,901U,0U},
  {0U,1574U,1107U,0U},
  {0U,1575U,3505U,0U},
  {0U,1576U,4256U,0U},
  {0U,1577U,4257U,0U},
  {0U,1578U,4259U,0U},
  {0U,1579U,2922U,0U},
  {0U,1580U,1740U,0U},
  {0U,1581U,1737U,0U},
  {0U,1581U,4262U,0U},
  {0U,1582U,4269U,0U},
  {0U,1583U,166U,0U},
  {0U,1583U,2579U,0U},
  {0U,1584U,327U,0U},
  {0U,1585U,4273U,0U},
  {0U,1586U,4279U,0U},
  {0U,1587U,424U,0U},
  {0U,1588U,4236U,0U},
  {0U,1589U,4246U,0U},
  {0U,1590U,53U,0U},
  {0U,1590U,3699U,0U},
  {0U,1591U,120U,0U},
  {0U,1592U,4282U,0U},
  {0U,1593U,4179U,0U},
  {0U,1594U,3851U,0U},
  {0U,1595U,4286U,0U},
  {0U,1596U,1484U,0U},
  {0U,1597U,2053U,0U},
  {0U,1598U,4291U,0U},
  {0U,1599U,1534U,0U},
  {0U,1600U,1149U,0U},
  {0U,1601U,3339U,0U},
  {0U,1602U,367U,0U},
  {0U,1603U,1550U,0U},
  {0U,1604U,1734U,0U},
  {0U,1605U,1701U,0U},
  {0U,1606U,454U,0U},
  {0U,1607U,3176U,0U},
  {0U,1608U,2532U,0U},
  {0U,1608U,5304U,0U},
  {0U,1609U,1650U,0U},
  {0U,1610U,53U,0U},
  {0U,1611U,4310U,0U},
  {0U,1612U,3451U,0U},
  {0U,1613U,266U,0U},
  {0U,1613U,1122U,0U},
  {0U,1614U,4325U,0U},
  {0U,1615U,3790U,0U},
  {0U,1616U,4327U,0U},
  {0U,1617U,4328U,0U},
  {0U,1618U,57U,0U},
  {0U,1619U,1447U,0U},
  {0U,1620U,420U,0U},
  {0U,1621U,269U,0U},
  {0U,1622U,4295U,0U},
  {0U,1623U,4177U,0U},
  {0U,1624U,4348U,0U},
  {0U,1625U,4352U,0U},
  {0U,1626U,2896U,0U},
  {0U,1627U,3289U,0U},
  {0U,1628U,3024U,0U},
  {0U,1629U,4370U,0U},
  {0U,1630U,4375U,0U},
  {0U,1631U,3355U,0U},
  {0U,1632U,3410U,0U},
  {0U,1633U,1182U,0U},
  {0U,1634U,4347U,0U},
  {0U,1635U,1646U,0U},
  {0U,1636U,1982U,0U},
  {0U,1637U,3522U,0U},
  {0U,1638U,1442U,0U},
  {0U,1639U,3415U,0U},
  {0U,1640U,4232U,0U},
  {0U,1641U,4407U,0U},
  {0U,1642U,1654U,0U},
  {0U,1643U,4410U,0U},
  {0U,1644U,4215U,0U},
  {0U,1645U,4421U,0U},
  {0U,1646U,1635U,0U},
  {0U,1646U,2733U,0U},
  {0U,1646U,3404U,0U},
  {0U,1647U,1451U,0U},
  {0U,1648U,399U,0U},
  {0U,1649U,53U,0U},
  {0U,1650U,1609U,0U},
  {0U,1651U,4431U,0U},
  {0U,1652U,1468U,0U},
  {0U,1653U,4439U,0U},
  {0U,1654U,1642U,0U},
  {0U,1655U,461U,0U},
  {0U,1656U,53U,0U},
  {0U,1657U,3745U,0U},
  {0U,1658U,4437U,0U},
  {0U,1659U,4447U,0U},
  {0U,1660U,4448U,0U},
  {0U,1661U,4450U,0U},
  {0U,1662U,4023U,0U},
  {0U,1663U,2885U,0U},
  {0U,1664U,53U,0U},
  {0U,1665U,4134U,0U},
  {0U,1666U,4261U,0U},
  {0U,1667U,138U,0U},
  {0U,1667U,1295U,0U},
  {0U,1667U,2652U,0U},
  {0U,1668U,1679U,0U},
  {0U,1669U,53U,0U},
  {0U,1670U,2635U,0U},
  {0U,1671U,4460U,0U},
  {0U,1672U,258U,0U},
  {0U,1673U,53U,0U},
  {0U,1674U,1675U,0U},
  {0U,1675U,1674U,0U},
  {0U,1676U,53U,0U},
  {0U,1677U,1180U,0U},
  {0U,1678U,4200U,0U},
  {0U,1679U,1668U,0U},
  {0U,1680U,4467U,0U},
  {0U,1681U,53U,0U},
  {0U,1682U,4053U,0U},
  {0U,1683U,2490U,0U},
  {0U,1684U,4475U,0U},
  {0U,1685U,4482U,0U},
  {0U,1686U,3204U,0U},
  {0U,1687U,1555U,0U},
  {0U,1688U,1989U,0U},
  {0U,1689U,4227U,0U},
  {0U,1690U,53U,0U},
  {0U,1691U,4492U,0U},
  {0U,1692U,186U,0U},
  {0U,1693U,3177U,0U},
  {0U,1694U,53U,0U},
  {0U,1695U,4326U,0U},
  {0U,1696U,3369U,0U},
  {0U,1697U,3708U,0U},
  {0U,1698U,3471U,0U},
  {0U,1699U,53U,0U},
  {0U,1700U,3137U,0U},
  {0U,1701U,1605U,0U},
  {0U,1702U,53U,0U},
  {0U,1703U,200U,0U},
  {0U,1704U,420U,0U},
  {0U,1705U,4349U,0U},
  {0U,1706U,1716U,0U},
  {0U,1707U,1888U,0U},
  {0U,1708U,3264U,0U},
  {0U,1709U,4510U,0U},
  {0U,1710U,4108U,0U},
  {0U,1711U,3730U,0U},
  {0U,1712U,4466U,0U},
  {0U,1713U,53U,0U},
  {0U,1714U,4497U,0U},
  {0U,1715U,4524U,0U},
  {0U,1716U,1706U,0U},
  {0U,1717U,172U,0U},
  {0U,1718U,4528U,0U},
  {0U,1719U,226U,0U},
  {0U,1720U,1892U,0U},
  {0U,1721U,294U,0U},
  {0U,1722U,53U,0U},
  {0U,1723U,2511U,0U},
  {0U,1724U,4402U,0U},
  {0U,1725U,2833U,0U},
  {0U,1726U,4535U,0U},
  {0U,1727U,2978U,0U},
  {0U,1728U,1805U,0U},
  {0U,1729U,4537U,0U},
  {0U,1730U,243U,0U},
  {0U,1731U,1265U,0U},
  {0U,1732U,3633U,0U},
  {0U,1733U,4540U,0U},
  {0U,1734U,1604U,0U},
  {0U,1735U,1096U,0U},
  {0U,1736U,2809U,0U},
  {0U,1737U,1581U,0U},
  {0U,1738U,3183U,0U},
  {0U,1739U,4543U,0U},
  {0U,1740U,316U,0U},
  {0U,1740U,1580U,0U},
  {0U,1741U,2651U,0U},
  {0U,1742U,2938U,0U},
  {0U,1743U,341U,0U},
  {0U,1744U,158U,0U},
  {0U,1745U,3880U,0U},
  {0U,1746U,1400U,0U},
  {0U,1747U,53U,0U},
  {0U,1748U,4549U,0U},
  {0U,1749U,4550U,0U},
  {0U,1750U,53U,0U},
  {0U,1751U,3595U,0U},
  {0U,1752U,4552U,0U},
  {0U,1753U,4553U,0U},
  {0U,1754U,1480U,0U},
  {0U,1755U,4021U,0U},
  {0U,1756U,965U,0U},
  {0U,1757U,1136U,0U},
  {0U,1758U,1433U,0U},
  {0U,1759U,4561U,0U},
  {0U,1760U,338U,0U},
  {0U,1761U,4521U,0U},
  {0U,1762U,333U,0U},
  {0U,1762U,3811U,0U},
  {0U,1763U,53U,0U},
  {0U,1764U,1255U,0U},
  {0U,1765U,278U,0U},
  {0U,1766U,1785U,0U},
  {0U,1767U,264U,0U},
  {0U,1768U,2653U,0U},
  {0U,1769U,4576U,0U},
  {0U,1770U,4112U,0U},
  {0U,1771U,2645U,0U},
  {0U,1772U,3026U,0U},
  {0U,1773U,2508U,0U},
  {0U,1774U,53U,0U},
  {0U,1775U,4577U,0U},
  {0U,1776U,146U,0U},
  {0U,1777U,2459U,0U},
  {0U,1778U,1542U,0U},
  {0U,1779U,2928U,0U},
  {0U,1780U,693U,0U},
  {0U,1781U,3833U,0U},
  {0U,1782U,606U,0U},
  {0U,1783U,53U,0U},
  {0U,1784U,4583U,0U},
  {0U,1785U,1766U,0U},
  {0U,1786U,4585U,0U},
  {0U,1787U,2457U,0U},
  {0U,1788U,1990U,0U},
  {0U,1789U,2313U,0U},
  {0U,1790U,789U,0U},
  {0U,1791U,1844U,0U},
  {0U,1791U,3695U,0U},
  {0U,1792U,1080U,0U},
  {0U,1793U,906U,0U},
  {0U,1793U,956U,0U},
  {0U,1794U,254U,0U},
  {0U,1795U,644U,0U},
  {0U,1796U,2257U,0U},
  {0U,1797U,1388U,0U},
  {0U,1798U,917U,0U},
  {0U,1799U,2480U,0U},
  {0U,1800U,55U,0U},
  {0U,1801U,1133U,0U},
  {0U,1802U,4590U,0U},
  {0U,1803U,1563U,0U},
  {0U,1804U,4591U,0U},
  {0U,1805U,1728U,0U},
  {0U,1806U,3738U,0U},
  {0U,1807U,850U,0U},
  {0U,1807U,2385U,0U},
  {0U,1808U,2366U,0U},
  {0U,1809U,319U,0U},
  {0U,1810U,671U,0U},
  {0U,1811U,522U,0U},
  {0U,1812U,627U,0U},
  {0U,1813U,344U,0U},
  {0U,1814U,498U,0U},
  {0U,1815U,71U,0U},
  {0U,1815U,1124U,0U},
  {0U,1816U,1385U,0U},
  {0U,1817U,1409U,0U},
  {0U,1818U,1367U,0U},
  {0U,1819U,806U,0U},
  {0U,1820U,3760U,0U},
  {0U,1821U,2007U,0U},
  {0U,1822U,1235U,0U},
  {0U,1823U,2376U,0U},
  {0U,1824U,710U,0U},
  {0U,1825U,3975U,0U},
  {0U,1826U,954U,0U},
  {0U,1827U,1302U,0U},
  {0U,1827U,1421U,0U},
  {0U,1827U,2338U,0U},
  {0U,1828U,63U,0U},
  {0U,1829U,964U,0U},
  {0U,1830U,1222U,0U},
  {0U,1831U,113U,0U},
  {0U,1832U,2299U,0U},
  {0U,1833U,1915U,0U},
  {0U,1834U,269U,0U},
  {0U,1835U,558U,0U},
  {0U,1836U,372U,0U},
  {0U,1837U,181U,0U},
  {0U,1838U,3436U,0U},
  {0U,1839U,64U,0U},
  {0U,1840U,3598U,0U},
  {0U,1841U,657U,0U},
  {0U,1842U,2747U,0U},
  {0U,1843U,715U,0U},
  {0U,1844U,1791U,0U},
  {0U,1845U,1041U,0U},
  {0U,1846U,105U,0U},
  {0U,1847U,3796U,0U},
  {0U,1848U,266U,0U},
  {0U,1849U,94U,0U},
  {0U,1850U,65U,0U},
  {0U,1850U,3713U,0U},
  {0U,1851U,4596U,0U},
  {0U,1852U,1524U,0U},
  {0U,1853U,1014U,0U},
  {0U,1854U,279U,0U},
  {0U,1855U,3675U,0U},
  {0U,1856U,528U,0U},
  {0U,1857U,322U,0U},
  {0U,1858U,1951U,0U},
  {0U,1859U,674U,0U},
  {0U,1860U,117U,0U},
  {0U,1861U,3825U,0U},
  {0U,1862U,2356U,0U},
  {0U,1863U,361U,0U},
  {0U,1864U,1867U,0U},
  {0U,1865U,68U,0U},
  {0U,1866U,3317U,0U},
  {0U,1867U,1864U,0U},
  {0U,1868U,2125U,0U},
  {0U,1869U,2471U,0U},
  {0U,1870U,1887U,0U},
  {0U,1871U,1131U,0U},
  {0U,1872U,3706U,0U},
  {0U,1873U,660U,0U},
  {0U,1874U,382U,0U},
  {0U,1875U,382U,0U},
  {0U,1876U,2343U,0U},
  {0U,1876U,2750U,0U},
  {0U,1877U,4597U,0U},
  {0U,1878U,113U,0U},
  {0U,1879U,70U,0U},
  {0U,1880U,905U,0U},
  {0U,1881U,70U,0U},
  {0U,1882U,2210U,0U},
  {0U,1883U,442U,0U},
  {0U,1884U,3121U,0U},
  {0U,1885U,2251U,0U},
  {0U,1886U,74U,0U},
  {0U,1887U,1870U,0U},
  {0U,1888U,1707U,0U},
  {0U,1889U,1071U,0U},
  {0U,1890U,3170U,0U},
  {0U,1891U,4600U,0U},
  {0U,1892U,1720U,0U},
  {0U,1893U,76U,0U},
  {0U,1894U,493U,0U},
  {0U,1895U,3643U,0U},
  {0U,1896U,3159U,0U},
  {0U,1897U,357U,0U},
  {0U,1898U,76U,0U},
  {0U,1898U,810U,0U},
  {0U,1899U,577U,0U},
  {0U,1900U,1135U,0U},
  {0U,1901U,268U,0U},
  {0U,1902U,2348U,0U},
  {0U,1903U,2212U,0U},
  {0U,1904U,1972U,0U},
  {0U,1905U,76U,0U},
  {0U,1906U,76U,0U},
  {0U,1907U,2495U,0U},
  {0U,1908U,1170U,0U},
  {0U,1909U,3179U,0U},
  {0U,1910U,488U,0U},
  {0U,1911U,2567U,0U},
  {0U,1912U,266U,0U},
  {0U,1913U,2154U,0U},
  {0U,1914U,4608U,0U},
  {0U,1915U,1833U,0U},
  {0U,1916U,4611U,0U},
  {0U,1917U,374U,0U},
  {0U,1918U,3533U,0U},
  {0U,1919U,98U,0U},
  {0U,1919U,4606U,0U},
  {0U,1920U,2199U,0U},
  {0U,1921U,2738U,0U},
  {0U,1922U,3982U,0U},
  {0U,1923U,2083U,0U},
  {0U,1924U,78U,0U},
  {0U,1925U,1487U,0U},
  {0U,1926U,682U,0U},
  {0U,1927U,625U,0U},
  {0U,1928U,873U,0U},
  {0U,1929U,2803U,0U},
  {0U,1930U,640U,0U},
  {0U,1931U,1050U,0U},
  {0U,1932U,71U,0U},
  {0U,1933U,3897U,0U},
  {0U,1934U,125U,0U},
  {0U,1935U,677U,0U},
  {0U,1936U,3705U,0U},
  {0U,1937U,1938U,0U},
  {0U,1938U,1937U,0U},
  {0U,1939U,3803U,0U},
  {0U,1940U,4618U,0U},
  {0U,1941U,2791U,0U},
  {0U,1942U,754U,0U},
  {0U,1943U,4619U,0U},
  {0U,1944U,3113U,0U},
  {0U,1945U,2086U,0U},
  {0U,1946U,81U,0U},
  {0U,1947U,120U,0U},
  {0U,1948U,2778U,0U},
  {0U,1949U,598U,0U},
  {0U,1950U,382U,0U},
  {0U,1951U,1858U,0U},
  {0U,1952U,325U,0U},
  {0U,1953U,3792U,0U},
  {0U,1954U,3538U,0U},
  {0U,1955U,2303U,0U},
  {0U,1956U,312U,0U},
  {0U,1957U,3571U,0U},
  {0U,1958U,84U,0U},
  {0U,1959U,1960U,0U},
  {0U,1960U,1959U,0U},
  {0U,1961U,2756U,0U},
  {0U,1962U,914U,0U},
  {0U,1963U,4276U,0U},
  {0U,1964U,2050U,0U},
  {0U,1965U,857U,0U},
  {0U,1966U,2241U,0U},
  {0U,1967U,2848U,0U},
  {0U,1968U,2173U,0U},
  {0U,1968U,2331U,0U},
  {0U,1969U,104U,0U},
  {0U,1970U,142U,0U},
  {0U,1970U,864U,0U},
  {0U,1971U,366U,0U},
  {0U,1972U,1904U,0U},
  {0U,1973U,3655U,0U},
  {0U,1974U,86U,0U},
  {0U,1975U,1189U,0U},
  {0U,1976U,3714U,0U},
  {0U,1977U,1262U,0U},
  {0U,1977U,1267U,0U},
  {0U,1978U,2614U,0U},
  {0U,1979U,87U,0U},
  {0U,1980U,1251U,0U},
  {0U,1980U,1334U,0U},
  {0U,1980U,2605U,0U},
  {0U,1980U,4630U,0U},
  {0U,1981U,321U,0U},
  {0U,1982U,1636U,0U},
  {0U,1983U,4635U,0U},
  {0U,1984U,4636U,0U},
  {0U,1985U,2718U,0U},
  {0U,1985U,2899U,0U},
  {0U,1986U,3983U,0U},
  {0U,1987U,3691U,0U},
  {0U,1988U,71U,0U},
  {0U,1989U,1688U,0U},
  {0U,1990U,1788U,0U},
  {0U,1991U,1062U,0U},
  {0U,1991U,3860U,0U},
  {0U,1992U,91U,0U},
  {0U,1993U,4622U,0U},
  {0U,1994U,3510U,0U},
  {0U,1995U,3624U,0U},
  {0U,1996U,92U,0U},
  {0U,1997U,2394U,0U},
  {0U,1998U,4161U,0U},
  {0U,1999U,2760U,0U},
  {0U,2000U,4641U,0U},
  {0U,2001U,632U,0U},
  {0U,2002U,2407U,0U},
  {0U,2003U,726U,0U},
  {0U,2004U,4644U,0U},
  {0U,2005U,672U,0U},
  {0U,2006U,1032U,0U},
  {0U,2007U,1821U,0U},
  {0U,2008U,2264U,0U},
  {0U,2009U,4098U,0U},
  {0U,2010U,2841U,0U},
  {0U,2011U,2611U,0U},
  {0U,2012U,1444U,0U},
  {0U,2013U,367U,0U},
  {0U,2013U,560U,0U},
  {0U,2014U,1244U,0U},
  {0U,2015U,2442U,0U},
  {0U,2016U,617U,0U},
  {0U,2017U,2019U,0U},
  {0U,2018U,1157U,0U},
  {0U,2019U,2017U,0U},
  {0U,2020U,953U,0U},
  {0U,2021U,3467U,0U},
  {0U,2022U,98U,0U},
  {0U,2023U,2894U,0U},
  {0U,2024U,2630U,0U},
  {0U,2025U,98U,0U},
  {0U,2026U,2972U,0U},
  {0U,2027U,4312U,0U},
  {0U,2028U,98U,0U},
  {0U,2029U,4647U,0U},
  {0U,2030U,142U,0U},
  {0U,2031U,3759U,0U},
  {0U,2032U,98U,0U},
  {0U,2033U,2962U,0U},
  {0U,2034U,324U,0U},
  {0U,2034U,711U,0U},
  {0U,2035U,762U,0U},
  {0U,2036U,3818U,0U},
  {0U,2037U,437U,0U},
  {0U,2038U,2700U,0U},
  {0U,2039U,976U,0U},
  {0U,2040U,4653U,0U},
  {0U,2041U,2059U,0U},
  {0U,2041U,3720U,0U},
  {0U,2041U,4654U,0U},
  {0U,2042U,2108U,0U},
  {0U,2043U,2880U,0U},
  {0U,2044U,4657U,0U},
  {0U,2045U,98U,0U},
  {0U,2046U,1341U,0U},
  {0U,2047U,4658U,0U},
  {0U,2048U,2762U,0U},
  {0U,2049U,1034U,0U},
  {0U,2050U,1964U,0U},
  {0U,2051U,4659U,0U},
  {0U,2052U,264U,0U},
  {0U,2052U,4662U,0U},
  {0U,2053U,1597U,0U},
  {0U,2054U,4663U,0U},
  {0U,2055U,344U,0U},
  {0U,2056U,1332U,0U},
  {0U,2057U,4666U,0U},
  {0U,2058U,124U,0U},
  {0U,2059U,2041U,0U},
  {0U,2060U,3221U,0U},
  {0U,2061U,2649U,0U},
  {0U,2062U,705U,0U},
  {0U,2063U,98U,0U},
  {0U,2064U,710U,0U},
  {0U,2065U,1143U,0U},
  {0U,2065U,4670U,0U},
  {0U,2066U,3304U,0U},
  {0U,2067U,1050U,0U},
  {0U,2068U,4674U,0U},
  {0U,2069U,71U,0U},
  {0U,2070U,1076U,0U},
  {0U,2071U,2092U,0U},
  {0U,2072U,500U,0U},
  {0U,2073U,4401U,0U},
  {0U,2074U,4675U,0U},
  {0U,2075U,3850U,0U},
  {0U,2076U,815U,0U},
  {0U,2077U,2430U,0U},
  {0U,2078U,409U,0U},
  {0U,2079U,4677U,0U},
  {0U,2080U,1363U,0U},
  {0U,2081U,632U,0U},
  {0U,2082U,4655U,0U},
  {0U,2083U,1923U,0U},
  {0U,2084U,2560U,0U},
  {0U,2085U,4589U,0U},
  {0U,2086U,1945U,0U},
  {0U,2087U,3406U,0U},
  {0U,2088U,3680U,0U},
  {0U,2089U,4683U,0U},
  {0U,2090U,1372U,0U},
  {0U,2091U,516U,0U},
  {0U,2092U,2071U,0U},
  {0U,2093U,2618U,0U},
  {0U,2094U,511U,0U},
  {0U,2095U,2285U,0U},
  {0U,2096U,4684U,0U},
  {0U,2097U,98U,0U},
  {0U,2098U,359U,0U},
  {0U,2099U,422U,0U},
  {0U,2100U,4685U,0U},
  {0U,2101U,1398U,0U},
  {0U,2102U,1016U,0U},
  {0U,2103U,98U,0U},
  {0U,2104U,2143U,0U},
  {0U,2105U,3751U,0U},
  {0U,2106U,4686U,0U},
  {0U,2107U,442U,0U},
  {0U,2108U,2042U,0U},
  {0U,2109U,2668U,0U},
  {0U,2110U,3787U,0U},
  {0U,2111U,3891U,0U},
  {0U,2112U,4598U,0U},
  {0U,2113U,99U,0U},
  {0U,2114U,4688U,0U},
  {0U,2115U,2944U,0U},
  {0U,2116U,218U,0U},
  {0U,2117U,2333U,0U},
  {0U,2118U,538U,0U},
  {0U,2119U,269U,0U},
  {0U,2119U,1030U,0U},
  {0U,2120U,977U,0U},
  {0U,2121U,2486U,0U},
  {0U,2122U,1007U,0U},
  {0U,2123U,4690U,0U},
  {0U,2124U,100U,0U},
  {0U,2125U,1868U,0U},
  {0U,2126U,293U,0U},
  {0U,2127U,192U,0U},
  {0U,2127U,3322U,0U},
  {0U,2128U,1287U,0U},
  {0U,2129U,4568U,0U},
  {0U,2130U,2754U,0U},
  {0U,2131U,1289U,0U},
  {0U,2132U,528U,0U},
  {0U,2133U,2341U,0U},
  {0U,2134U,307U,0U},
  {0U,2135U,192U,0U},
  {0U,2136U,2138U,0U},
  {0U,2137U,1003U,0U},
  {0U,2138U,2136U,0U},
  {0U,2139U,914U,0U},
  {0U,2140U,2220U,0U},
  {0U,2140U,2233U,0U},
  {0U,2141U,3594U,0U},
  {0U,2142U,757U,0U},
  {0U,2143U,2104U,0U},
  {0U,2144U,810U,0U},
  {0U,2145U,3645U,0U},
  {0U,2146U,309U,0U},
  {0U,2147U,632U,0U},
  {0U,2148U,246U,0U},
  {0U,2149U,4695U,0U},
  {0U,2150U,71U,0U},
  {0U,2151U,263U,0U},
  {0U,2151U,1556U,0U},
  {0U,2151U,2484U,0U},
  {0U,2152U,3483U,0U},
  {0U,2152U,3590U,0U},
  {0U,2153U,3078U,0U},
  {0U,2154U,1913U,0U},
  {0U,2155U,3773U,0U},
  {0U,2156U,3889U,0U},
  {0U,2157U,487U,0U},
  {0U,2158U,3423U,0U},
  {0U,2159U,2974U,0U},
  {0U,2160U,3783U,0U},
  {0U,2161U,104U,0U},
  {0U,2162U,2570U,0U},
  {0U,2163U,4075U,0U},
  {0U,2164U,2183U,0U},
  {0U,2165U,856U,0U},
  {0U,2166U,4092U,0U},
  {0U,2167U,2709U,0U},
  {0U,2168U,345U,0U},
  {0U,2169U,1395U,0U},
  {0U,2170U,1115U,0U},
  {0U,2171U,797U,0U},
  {0U,2172U,3225U,0U},
  {0U,2172U,3263U,0U},
  {0U,2173U,1968U,0U},
  {0U,2174U,4697U,0U},
  {0U,2175U,380U,0U},
  {0U,2176U,1119U,0U},
  {0U,2177U,581U,0U},
  {0U,2178U,3232U,0U},
  {0U,2179U,858U,0U},
  {0U,2180U,3864U,0U},
  {0U,2181U,4698U,0U},
  {0U,2182U,3141U,0U},
  {0U,2183U,2164U,0U},
  {0U,2184U,243U,0U},
  {0U,2184U,969U,0U},
  {0U,2184U,2873U,0U},
  {0U,2184U,3218U,0U},
  {0U,2184U,3226U,0U},
  {0U,2184U,3285U,0U},
  {0U,2185U,1085U,0U},
  {0U,2186U,978U,0U},
  {0U,2187U,2416U,0U},
  {0U,2188U,1191U,0U},
  {0U,2189U,189U,0U},
  {0U,2190U,3841U,0U},
  {0U,2191U,1058U,0U},
  {0U,2192U,425U,0U},
  {0U,2193U,877U,0U},
  {0U,2194U,391U,0U},
  {0U,2195U,213U,0U},
  {0U,2196U,512U,0U},
  {0U,2197U,2737U,0U},
  {0U,2198U,2349U,0U},
  {0U,2199U,1920U,0U},
  {0U,2200U,120U,0U},
  {0U,2201U,336U,0U},
  {0U,2202U,425U,0U},
  {0U,2203U,504U,0U},
  {0U,2204U,324U,0U},
  {0U,2205U,4701U,0U},
  {0U,2206U,4702U,0U},
  {0U,2207U,734U,0U},
  {0U,2208U,2846U,0U},
  {0U,2209U,2228U,0U},
  {0U,2210U,1882U,0U},
  {0U,2211U,2697U,0U},
  {0U,2212U,1903U,0U},
  {0U,2213U,290U,0U},
  {0U,2214U,1450U,0U},
  {0U,2215U,797U,0U},
  {0U,2216U,3639U,0U},
  {0U,2217U,2409U,0U},
  {0U,2218U,801U,0U},
  {0U,2218U,3671U,0U},
  {0U,2219U,487U,0U},
  {0U,2220U,2140U,0U},
  {0U,2221U,494U,0U},
  {0U,2222U,2963U,0U},
  {0U,2223U,357U,0U},
  {0U,2224U,104U,0U},
  {0U,2225U,516U,0U},
  {0U,2226U,488U,0U},
  {0U,2227U,3679U,0U},
  {0U,2228U,2209U,0U},
  {0U,2229U,4102U,0U},
  {0U,2230U,1236U,0U},
  {0U,2231U,4706U,0U},
  {0U,2232U,4707U,0U},
  {0U,2233U,2140U,0U},
  {0U,2234U,416U,0U},
  {0U,2235U,4107U,0U},
  {0U,2236U,186U,0U},
  {0U,2237U,2786U,0U},
  {0U,2238U,791U,0U},
  {0U,2238U,2636U,0U},
  {0U,2239U,71U,0U},
  {0U,2240U,4710U,0U},
  {0U,2241U,1966U,0U},
  {0U,2242U,3231U,0U},
  {0U,2243U,104U,0U},
  {0U,2244U,4335U,0U},
  {0U,2245U,3619U,0U},
  {0U,2246U,1324U,0U},
  {0U,2246U,2247U,0U},
  {0U,2246U,3517U,0U},
  {0U,2247U,2246U,0U},
  {0U,2248U,105U,0U},
  {0U,2249U,3145U,0U},
  {0U,2250U,2930U,0U},
  {0U,2251U,1885U,0U},
  {0U,2252U,545U,0U},
  {0U,2253U,3200U,0U},
  {0U,2254U,814U,0U},
  {0U,2255U,722U,0U},
  {0U,2256U,982U,0U},
  {0U,2257U,1796U,0U},
  {0U,2258U,1443U,0U},
  {0U,2259U,3434U,0U},
  {0U,2260U,736U,0U},
  {0U,2261U,110U,0U},
  {0U,2262U,736U,0U},
  {0U,2263U,3320U,0U},
  {0U,2264U,2008U,0U},
  {0U,2265U,3613U,0U},
  {0U,2266U,3478U,0U},
  {0U,2267U,614U,0U},
  {0U,2268U,112U,0U},
  {0U,2269U,2679U,0U},
  {0U,2270U,752U,0U},
  {0U,2271U,242U,0U},
  {0U,2272U,761U,0U},
  {0U,2273U,214U,0U},
  {0U,2274U,2775U,0U},
  {0U,2275U,1077U,0U},
  {0U,2276U,2633U,0U},
  {0U,2277U,218U,0U},
  {0U,2278U,2966U,0U},
  {0U,2279U,3037U,0U},
  {0U,2280U,401U,0U},
  {0U,2281U,425U,0U},
  {0U,2282U,1354U,0U},
  {0U,2283U,3788U,0U},
  {0U,2284U,138U,0U},
  {0U,2285U,2095U,0U},
  {0U,2286U,1202U,0U},
  {0U,2287U,2417U,0U},
  {0U,2288U,3278U,0U},
  {0U,2289U,1108U,0U},
  {0U,2290U,2380U,0U},
  {0U,2291U,3004U,0U},
  {0U,2291U,3376U,0U},
  {0U,2292U,3618U,0U},
  {0U,2293U,4717U,0U},
  {0U,2294U,1495U,0U},
  {0U,2295U,138U,0U},
  {0U,2296U,143U,0U},
  {0U,2297U,949U,0U},
  {0U,2298U,1184U,0U},
  {0U,2299U,1832U,0U},
  {0U,2300U,162U,0U},
  {0U,2300U,3794U,0U},
  {0U,2301U,2795U,0U},
  {0U,2302U,646U,0U},
  {0U,2303U,1955U,0U},
  {0U,2304U,2695U,0U},
  {0U,2305U,2863U,0U},
  {0U,2306U,569U,0U},
  {0U,2307U,4720U,0U},
  {0U,2308U,942U,0U},
  {0U,2309U,1175U,0U},
  {0U,2310U,3971U,0U},
  {0U,2311U,1198U,0U},
  {0U,2312U,120U,0U},
  {0U,2313U,1789U,0U},
  {0U,2314U,488U,0U},
  {0U,2315U,4725U,0U},
  {0U,2316U,865U,0U},
  {0U,2317U,2350U,0U},
  {0U,2318U,120U,0U},
  {0U,2319U,944U,0U},
  {0U,2320U,1238U,0U},
  {0U,2321U,120U,0U},
  {0U,2322U,124U,0U},
  {0U,2323U,4572U,0U},
  {0U,2324U,3194U,0U},
  {0U,2325U,4214U,0U},
  {0U,2326U,2992U,0U},
  {0U,2327U,120U,0U},
  {0U,2328U,2713U,0U},
  {0U,2329U,120U,0U},
  {0U,2330U,3731U,0U},
  {0U,2331U,1968U,0U},
  {0U,2332U,3082U,0U},
  {0U,2333U,2117U,0U},
  {0U,2334U,120U,0U},
  {0U,2335U,3298U,0U},
  {0U,2336U,4473U,0U},
  {0U,2337U,3095U,0U},
  {0U,2337U,3678U,0U},
  {0U,2338U,1827U,0U},
  {0U,2339U,3587U,0U},
  {0U,2340U,3933U,0U},
  {0U,2341U,2133U,0U},
  {0U,2342U,464U,0U},
  {0U,2343U,1876U,0U},
  {0U,2344U,2906U,0U},
  {0U,2345U,147U,0U},
  {0U,2346U,896U,0U},
  {0U,2347U,4743U,0U},
  {0U,2348U,1902U,0U},
  {0U,2349U,2198U,0U},
  {0U,2350U,2317U,0U},
  {0U,2351U,4745U,0U},
  {0U,2352U,2953U,0U},
  {0U,2353U,4747U,0U},
  {0U,2354U,122U,0U},
  {0U,2355U,1479U,0U},
  {0U,2356U,1862U,0U},
  {0U,2357U,4749U,0U},
  {0U,2358U,195U,0U},
  {0U,2359U,4093U,0U},
  {0U,2360U,3735U,0U},
  {0U,2361U,744U,0U},
  {0U,2362U,409U,0U},
  {0U,2363U,414U,0U},
  {0U,2364U,202U,0U},
  {0U,2365U,1206U,0U},
  {0U,2366U,1808U,0U},
  {0U,2367U,638U,0U},
  {0U,2368U,3525U,0U},
  {0U,2369U,848U,0U},
  {0U,2370U,1079U,0U},
  {0U,2371U,1041U,0U},
  {0U,2372U,1096U,0U},
  {0U,2373U,4645U,0U},
  {0U,2374U,301U,0U},
  {0U,2375U,3514U,0U},
  {0U,2376U,1823U,0U},
  {0U,2377U,367U,0U},
  {0U,2378U,382U,0U},
  {0U,2379U,3100U,0U},
  {0U,2380U,2290U,0U},
  {0U,2381U,3539U,0U},
  {0U,2382U,124U,0U},
  {0U,2383U,536U,0U},
  {0U,2384U,3305U,0U},
  {0U,2385U,1807U,0U},
  {0U,2386U,3746U,0U},
  {0U,2387U,2555U,0U},
  {0U,2388U,3906U,0U},
  {0U,2389U,2513U,0U},
  {0U,2390U,186U,0U},
  {0U,2391U,2392U,0U},
  {0U,2392U,2391U,0U},
  {0U,2393U,4153U,0U},
  {0U,2394U,1997U,0U},
  {0U,2395U,186U,0U},
  {0U,2396U,231U,0U},
  {0U,2397U,267U,0U},
  {0U,2398U,4718U,0U},
  {0U,2399U,931U,0U},
  {0U,2400U,131U,0U},
  {0U,2401U,14U,0U},
  {0U,2402U,3563U,0U},
  {0U,2403U,3054U,0U},
  {0U,2404U,472U,0U},
  {0U,2405U,2406U,0U},
  {0U,2406U,2405U,0U},
  {0U,2407U,2002U,0U},
  {0U,2408U,952U,0U},
  {0U,2409U,2217U,0U},
  {0U,2410U,952U,0U},
  {0U,2411U,686U,0U},
  {0U,2412U,714U,0U},
  {0U,2413U,4082U,0U},
  {0U,2414U,573U,0U},
  {0U,2415U,1365U,0U},
  {0U,2416U,2187U,0U},
  {0U,2417U,2287U,0U},
  {0U,2418U,299U,0U},
  {0U,2419U,479U,0U},
  {0U,2420U,4764U,0U},
  {0U,2421U,1494U,0U},
  {0U,2422U,1531U,0U},
  {0U,2423U,4765U,0U},
  {0U,2424U,541U,0U},
  {0U,2425U,4766U,0U},
  {0U,2425U,4767U,0U},
  {0U,2426U,1054U,0U},
  {0U,2427U,4230U,0U},
  {0U,2428U,138U,0U},
  {0U,2429U,3826U,0U},
  {0U,2430U,2077U,0U},
  {0U,2431U,2662U,0U},
  {0U,2432U,550U,0U},
  {0U,2433U,4768U,0U},
  {0U,2434U,138U,0U},
  {0U,2435U,71U,0U},
  {0U,2436U,1099U,0U},
  {0U,2437U,4678U,0U},
  {0U,2438U,516U,0U},
  {0U,2438U,1165U,0U},
  {0U,2439U,3875U,0U},
  {0U,2440U,862U,0U},
  {0U,2441U,1502U,0U},
  {0U,2442U,2015U,0U},
  {0U,2443U,3797U,0U},
  {0U,2444U,140U,0U},
  {0U,2445U,3612U,0U},
  {0U,2446U,2711U,0U},
  {0U,2447U,2546U,0U},
  {0U,2448U,2715U,0U},
  {0U,2449U,4769U,0U},
  {0U,2450U,285U,0U},
  {0U,2451U,820U,0U},
  {0U,2452U,4771U,0U},
  {0U,2453U,841U,0U},
  {0U,2454U,3715U,0U},
  {0U,2455U,3894U,0U},
  {0U,2456U,3162U,0U},
  {0U,2457U,1787U,0U},
  {0U,2458U,1422U,0U},
  {0U,2459U,1777U,0U},
  {0U,2460U,4634U,0U},
  {0U,2461U,3311U,0U},
  {0U,2462U,1097U,0U},
  {0U,2463U,456U,0U},
  {0U,2464U,3951U,0U},
  {0U,2465U,4774U,0U},
  {0U,2466U,326U,0U},
  {0U,2467U,143U,0U},
  {0U,2468U,4776U,0U},
  {0U,2469U,3918U,0U},
  {0U,2470U,611U,0U},
  {0U,2471U,1869U,0U},
  {0U,2471U,2717U,0U},
  {0U,2472U,143U,0U},
  {0U,2473U,4603U,0U},
  {0U,2474U,3036U,0U},
  {0U,2475U,386U,0U},
  {0U,2476U,143U,0U},
  {0U,2477U,2812U,0U},
  {0U,2477U,3124U,0U},
  {0U,2478U,2991U,0U},
  {0U,2479U,1387U,0U},
  {0U,2480U,1799U,0U},
  {0U,2480U,4780U,0U},
  {0U,2481U,2482U,0U},
  {0U,2482U,2481U,0U},
  {0U,2483U,2485U,0U},
  {0U,2484U,2151U,0U},
  {0U,2484U,2544U,0U},
  {0U,2485U,2483U,0U},
  {0U,2486U,2121U,0U},
  {0U,2487U,3741U,0U},
  {0U,2488U,3301U,0U},
  {0U,2489U,2541U,0U},
  {0U,2490U,1683U,0U},
  {0U,2491U,1111U,0U},
  {0U,2492U,3957U,0U},
  {0U,2493U,2503U,0U},
  {0U,2494U,2861U,0U},
  {0U,2495U,1907U,0U},
  {0U,2496U,4754U,0U},
  {0U,2497U,3281U,0U},
  {0U,2497U,4366U,0U},
  {0U,2498U,590U,0U},
  {0U,2499U,2744U,0U},
  {0U,2500U,1570U,0U},
  {0U,2501U,4783U,0U},
  {0U,2502U,4624U,0U},
  {0U,2503U,2493U,0U},
  {0U,2504U,3108U,0U},
  {0U,2505U,1084U,0U},
  {0U,2505U,3223U,0U},
  {0U,2506U,2889U,0U},
  {0U,2507U,1562U,0U},
  {0U,2508U,1773U,0U},
  {0U,2509U,2597U,0U},
  {0U,2510U,3114U,0U},
  {0U,2511U,1723U,0U},
  {0U,2511U,3685U,0U},
  {0U,2511U,4787U,0U},
  {0U,2512U,2799U,0U},
  {0U,2513U,2389U,0U},
  {0U,2514U,1310U,0U},
  {0U,2514U,2659U,0U},
  {0U,2514U,3427U,0U},
  {0U,2515U,2726U,0U},
  {0U,2515U,2916U,0U},
  {0U,2516U,699U,0U},
  {0U,2517U,1022U,0U},
  {0U,2518U,500U,0U},
  {0U,2519U,3017U,0U},
  {0U,2520U,3072U,0U},
  {0U,2521U,397U,0U},
  {0U,2521U,3068U,0U},
  {0U,2522U,4790U,0U},
  {0U,2523U,3066U,0U},
  {0U,2524U,971U,0U},
  {0U,2525U,2557U,0U},
  {0U,2526U,4178U,0U},
  {0U,2527U,3560U,0U},
  {0U,2528U,2549U,0U},
  {0U,2529U,3047U,0U},
  {0U,2530U,971U,0U},
  {0U,2531U,1540U,0U},
  {0U,2532U,1608U,0U},
  {0U,2533U,3050U,0U},
  {0U,2534U,3560U,0U},
  {0U,2534U,4793U,0U},
  {0U,2535U,2538U,0U},
  {0U,2536U,3779U,0U},
  {0U,2537U,3038U,0U},
  {0U,2538U,2535U,0U},
  {0U,2538U,4120U,0U},
  {0U,2539U,787U,0U},
  {0U,2540U,3079U,0U},
  {0U,2541U,2489U,0U},
  {0U,2542U,3067U,0U},
  {0U,2543U,3057U,0U},
  {0U,2544U,2484U,0U},
  {0U,2545U,3071U,0U},
  {0U,2546U,2447U,0U},
  {0U,2546U,3055U,0U},
  {0U,2547U,148U,0U},
  {0U,2548U,4185U,0U},
  {0U,2549U,2528U,0U},
  {0U,2550U,148U,0U},
  {0U,2551U,3045U,0U},
  {0U,2551U,4794U,0U},
  {0U,2552U,148U,0U},
  {0U,2553U,164U,0U},
  {0U,2554U,2826U,0U},
  {0U,2555U,2387U,0U},
  {0U,2556U,3075U,0U},
  {0U,2557U,2525U,0U},
  {0U,2558U,3854U,0U},
  {0U,2559U,4182U,0U},
  {0U,2560U,2084U,0U},
  {0U,2561U,3149U,0U},
  {0U,2562U,150U,0U},
  {0U,2563U,2676U,0U},
  {0U,2564U,1045U,0U},
  {0U,2565U,151U,0U},
  {0U,2566U,3157U,0U},
  {0U,2567U,1911U,0U},
  {0U,2568U,2655U,0U},
  {0U,2569U,2655U,0U},
  {0U,2570U,2162U,0U},
  {0U,2571U,3621U,0U},
  {0U,2572U,3282U,0U},
  {0U,2573U,3441U,0U},
  {0U,2574U,2607U,0U},
  {0U,2575U,4801U,0U},
  {0U,2576U,4802U,0U},
  {0U,2577U,224U,0U},
  {0U,2578U,2845U,0U},
  {0U,2579U,1583U,0U},
  {0U,2580U,3389U,0U},
  {0U,2581U,2608U,0U},
  {0U,2582U,3001U,0U},
  {0U,2583U,980U,0U},
  {0U,2583U,1118U,0U},
  {0U,2584U,499U,0U},
  {0U,2585U,218U,0U},
  {0U,2586U,2587U,0U},
  {0U,2587U,2586U,0U},
  {0U,2588U,153U,0U},
  {0U,2589U,301U,0U},
  {0U,2590U,4625U,0U},
  {0U,2591U,1276U,0U},
  {0U,2592U,4056U,0U},
  {0U,2593U,155U,0U},
  {0U,2594U,2603U,0U},
  {0U,2595U,2596U,0U},
  {0U,2596U,2595U,0U},
  {0U,2597U,2509U,0U},
  {0U,2597U,3270U,0U},
  {0U,2598U,1275U,0U},
  {0U,2599U,4810U,0U},
  {0U,2600U,1253U,0U},
  {0U,2600U,3335U,0U},
  {0U,2601U,398U,0U},
  {0U,2602U,341U,0U},
  {0U,2603U,2594U,0U},
  {0U,2604U,4390U,0U},
  {0U,2605U,1980U,0U},
  {0U,2605U,4813U,0U},
  {0U,2606U,1281U,0U},
  {0U,2607U,2574U,0U},
  {0U,2608U,2581U,0U},
  {0U,2609U,1213U,0U},
  {0U,2610U,450U,0U},
  {0U,2611U,1462U,0U},
  {0U,2611U,2011U,0U},
  {0U,2612U,191U,0U},
  {0U,2613U,3758U,0U},
  {0U,2614U,1978U,0U},
  {0U,2615U,4399U,0U},
  {0U,2616U,217U,0U},
  {0U,2617U,160U,0U},
  {0U,2618U,2093U,0U},
  {0U,2619U,2623U,0U},
  {0U,2620U,1407U,0U},
  {0U,2621U,324U,0U},
  {0U,2622U,354U,0U},
  {0U,2623U,2619U,0U},
  {0U,2623U,4103U,0U},
  {0U,2624U,165U,0U},
  {0U,2625U,612U,0U},
  {0U,2626U,1002U,0U},
  {0U,2627U,166U,0U},
  {0U,2628U,722U,0U},
  {0U,2629U,2632U,0U},
  {0U,2630U,2024U,0U},
  {0U,2631U,2939U,0U},
  {0U,2632U,2629U,0U},
  {0U,2633U,2276U,0U},
  {0U,2634U,2644U,0U},
  {0U,2635U,1670U,0U},
  {0U,2635U,3160U,0U},
  {0U,2636U,897U,0U},
  {0U,2636U,2238U,0U},
  {0U,2637U,4639U,0U},
  {0U,2638U,2643U,0U},
  {0U,2639U,867U,0U},
  {0U,2640U,4276U,0U},
  {0U,2641U,1486U,0U},
  {0U,2642U,3043U,0U},
  {0U,2643U,2638U,0U},
  {0U,2644U,2634U,0U},
  {0U,2645U,1771U,0U},
  {0U,2646U,626U,0U},
  {0U,2647U,171U,0U},
  {0U,2648U,3849U,0U},
  {0U,2649U,2061U,0U},
  {0U,2650U,3456U,0U},
  {0U,2651U,1741U,0U},
  {0U,2652U,1667U,0U},
  {0U,2653U,1768U,0U},
  {0U,2654U,2691U,0U},
  {0U,2655U,2568U,0U},
  {0U,2655U,2569U,0U},
  {0U,2656U,3475U,0U},
  {0U,2657U,4588U,0U},
  {0U,2658U,812U,0U},
  {0U,2659U,2514U,0U},
  {0U,2660U,2665U,0U},
  {0U,2661U,2999U,0U},
  {0U,2662U,172U,0U},
  {0U,2662U,2431U,0U},
  {0U,2662U,3027U,0U},
  {0U,2663U,1311U,0U},
  {0U,2664U,1017U,0U},
  {0U,2665U,2660U,0U},
  {0U,2666U,3028U,0U},
  {0U,2667U,4443U,0U},
  {0U,2668U,2109U,0U},
  {0U,2669U,3256U,0U},
  {0U,2670U,1447U,0U},
  {0U,2671U,2674U,0U},
  {0U,2672U,173U,0U},
  {0U,2673U,987U,0U},
  {0U,2674U,2671U,0U},
  {0U,2675U,2907U,0U},
  {0U,2676U,2563U,0U},
  {0U,2677U,174U,0U},
  {0U,2678U,655U,0U},
  {0U,2679U,2269U,0U},
  {0U,2680U,3144U,0U},
  {0U,2681U,2931U,0U},
  {0U,2682U,176U,0U},
  {0U,2683U,4827U,0U},
  {0U,2684U,273U,0U},
  {0U,2685U,847U,0U},
  {0U,2686U,4828U,0U},
  {0U,2687U,177U,0U},
  {0U,2688U,178U,0U},
  {0U,2689U,178U,0U},
  {0U,2690U,4729U,0U},
  {0U,2691U,2654U,0U},
  {0U,2692U,179U,0U},
  {0U,2693U,179U,0U},
  {0U,2694U,2767U,0U},
  {0U,2695U,182U,0U},
  {0U,2695U,2304U,0U},
  {0U,2696U,3684U,0U},
  {0U,2697U,2211U,0U},
  {0U,2698U,837U,0U},
  {0U,2699U,822U,0U},
  {0U,2700U,2038U,0U},
  {0U,2701U,186U,0U},
  {0U,2701U,204U,0U},
  {0U,2702U,829U,0U},
  {0U,2703U,3485U,0U},
  {0U,2704U,1117U,0U},
  {0U,2705U,3771U,0U},
  {0U,2706U,3669U,0U},
  {0U,2707U,4830U,0U},
  {0U,2708U,3136U,0U},
  {0U,2709U,2167U,0U},
  {0U,2710U,276U,0U},
  {0U,2711U,2446U,0U},
  {0U,2712U,431U,0U},
  {0U,2713U,2328U,0U},
  {0U,2714U,1355U,0U},
  {0U,2715U,2448U,0U},
  {0U,2716U,3016U,0U},
  {0U,2717U,2471U,0U},
  {0U,2718U,1985U,0U},
  {0U,2719U,3882U,0U},
  {0U,2720U,4842U,0U},
  {0U,2721U,4297U,0U},
  {0U,2722U,3401U,0U},
  {0U,2723U,4839U,0U},
  {0U,2724U,4826U,0U},
  {0U,2725U,3358U,0U},
  {0U,2725U,3808U,0U},
  {0U,2726U,2515U,0U},
  {0U,2727U,2742U,0U},
  {0U,2728U,2735U,0U},
  {0U,2729U,2731U,0U},
  {0U,2729U,2741U,0U},
  {0U,2730U,343U,0U},
  {0U,2731U,2729U,0U},
  {0U,2732U,2740U,0U},
  {0U,2733U,1646U,0U},
  {0U,2734U,2736U,0U},
  {0U,2735U,2728U,0U},
  {0U,2736U,2734U,0U},
  {0U,2737U,2197U,0U},
  {0U,2738U,1921U,0U},
  {0U,2738U,4373U,0U},
  {0U,2739U,3405U,0U},
  {0U,2740U,2732U,0U},
  {0U,2741U,2729U,0U},
  {0U,2742U,2727U,0U},
  {0U,2743U,3403U,0U},
  {0U,2744U,2499U,0U},
  {0U,2745U,2932U,0U},
  {0U,2746U,3025U,0U},
  {0U,2747U,1842U,0U},
  {0U,2748U,3458U,0U},
  {0U,2749U,3795U,0U},
  {0U,2750U,1876U,0U},
  {0U,2751U,4226U,0U},
  {0U,2752U,603U,0U},
  {0U,2753U,4605U,0U},
  {0U,2754U,2130U,0U},
  {0U,2755U,3501U,0U},
  {0U,2756U,1961U,0U},
  {0U,2757U,4127U,0U},
  {0U,2758U,3211U,0U},
  {0U,2759U,3252U,0U},
  {0U,2760U,1999U,0U},
  {0U,2761U,4847U,0U},
  {0U,2762U,2048U,0U},
  {0U,2763U,4848U,0U},
  {0U,2764U,3029U,0U},
  {0U,2765U,3524U,0U},
  {0U,2766U,981U,0U},
  {0U,2767U,2694U,0U},
  {0U,2768U,192U,0U},
  {0U,2769U,623U,0U},
  {0U,2770U,1336U,0U},
  {0U,2771U,3207U,0U},
  {0U,2772U,511U,0U},
  {0U,2773U,331U,0U},
  {0U,2774U,3611U,0U},
  {0U,2775U,2274U,0U},
  {0U,2776U,3299U,0U},
  {0U,2777U,194U,0U},
  {0U,2778U,1948U,0U},
  {0U,2779U,1505U,0U},
  {0U,2780U,2781U,0U},
  {0U,2781U,2780U,0U},
  {0U,2782U,3190U,0U},
  {0U,2783U,3193U,0U},
  {0U,2783U,3453U,0U},
  {0U,2784U,3908U,0U},
  {0U,2785U,3189U,0U},
  {0U,2786U,2237U,0U},
  {0U,2787U,3186U,0U},
  {0U,2788U,835U,0U},
  {0U,2789U,196U,0U},
  {0U,2790U,2859U,0U},
  {0U,2791U,1941U,0U},
  {0U,2792U,4853U,0U},
  {0U,2793U,417U,0U},
  {0U,2794U,4854U,0U},
  {0U,2795U,2301U,0U},
  {0U,2796U,2797U,0U},
  {0U,2797U,2796U,0U},
  {0U,2798U,395U,0U},
  {0U,2799U,2512U,0U},
  {0U,2800U,3084U,0U},
  {0U,2801U,1522U,0U},
  {0U,2802U,343U,0U},
  {0U,2803U,1929U,0U},
  {0U,2804U,287U,0U},
  {0U,2805U,2806U,0U},
  {0U,2806U,2805U,0U},
  {0U,2807U,203U,0U},
  {0U,2808U,3946U,0U},
  {0U,2809U,1736U,0U},
  {0U,2810U,362U,0U},
  {0U,2811U,3923U,0U},
  {0U,2812U,2477U,0U},
  {0U,2813U,2815U,0U},
  {0U,2814U,4863U,0U},
  {0U,2815U,2813U,0U},
  {0U,2816U,4602U,0U},
  {0U,2817U,1464U,0U},
  {0U,2818U,621U,0U},
  {0U,2819U,2827U,0U},
  {0U,2820U,435U,0U},
  {0U,2821U,722U,0U},
  {0U,2822U,4865U,0U},
  {0U,2823U,999U,0U},
  {0U,2823U,3115U,0U},
  {0U,2824U,208U,0U},
  {0U,2825U,4519U,0U},
  {0U,2826U,2554U,0U},
  {0U,2826U,3044U,0U},
  {0U,2827U,2819U,0U},
  {0U,2828U,3418U,0U},
  {0U,2829U,280U,0U},
  {0U,2830U,3495U,0U},
  {0U,2831U,3909U,0U},
  {0U,2832U,210U,0U},
  {0U,2833U,1725U,0U},
  {0U,2834U,4866U,0U},
  {0U,2835U,3673U,0U},
  {0U,2836U,3646U,0U},
  {0U,2837U,3816U,0U},
  {0U,2838U,3526U,0U},
  {0U,2839U,2840U,0U},
  {0U,2840U,2839U,0U},
  {0U,2841U,2010U,0U},
  {0U,2842U,3222U,0U},
  {0U,2842U,4642U,0U},
  {0U,2843U,247U,0U},
  {0U,2844U,2870U,0U},
  {0U,2845U,2578U,0U},
  {0U,2846U,2208U,0U},
  {0U,2847U,236U,0U},
  {0U,2848U,1967U,0U},
  {0U,2849U,4867U,0U},
  {0U,2850U,1246U,0U},
  {0U,2851U,2853U,0U},
  {0U,2852U,3878U,0U},
  {0U,2853U,2851U,0U},
  {0U,2854U,3877U,0U},
  {0U,2855U,2858U,0U},
  {0U,2856U,3312U,0U},
  {0U,2857U,216U,0U},
  {0U,2858U,2855U,0U},
  {0U,2859U,2790U,0U},
  {0U,2860U,4571U,0U},
  {0U,2861U,2494U,0U},
  {0U,2862U,3181U,0U},
  {0U,2863U,2305U,0U},
  {0U,2864U,874U,0U},
  {0U,2865U,3542U,0U},
  {0U,2866U,3893U,0U},
  {0U,2867U,286U,0U},
  {0U,2868U,4873U,0U},
  {0U,2869U,912U,0U},
  {0U,2870U,2844U,0U},
  {0U,2871U,4125U,0U},
  {0U,2872U,2882U,0U},
  {0U,2872U,3213U,0U},
  {0U,2872U,4281U,0U},
  {0U,2873U,2184U,0U},
  {0U,2874U,4368U,0U},
  {0U,2875U,399U,0U},
  {0U,2876U,3395U,0U},
  {0U,2877U,4878U,0U},
  {0U,2878U,1151U,0U},
  {0U,2879U,3591U,0U},
  {0U,2880U,2043U,0U},
  {0U,2881U,4416U,0U},
  {0U,2882U,1382U,0U},
  {0U,2882U,2872U,0U},
  {0U,2883U,371U,0U},
  {0U,2884U,2904U,0U},
  {0U,2885U,1663U,0U},
  {0U,2886U,4027U,0U},
  {0U,2887U,4237U,0U},
  {0U,2888U,4882U,0U},
  {0U,2889U,2506U,0U},
  {0U,2890U,4130U,0U},
  {0U,2891U,4626U,0U},
  {0U,2892U,4821U,0U},
  {0U,2893U,2935U,0U},
  {0U,2894U,2023U,0U},
  {0U,2895U,4885U,0U},
  {0U,2896U,1626U,0U},
  {0U,2897U,4888U,0U},
  {0U,2898U,4890U,0U},
  {0U,2899U,296U,0U},
  {0U,2899U,1985U,0U},
  {0U,2900U,343U,0U},
  {0U,2901U,4892U,0U},
  {0U,2902U,218U,0U},
  {0U,2903U,3360U,0U},
  {0U,2904U,2884U,0U},
  {0U,2905U,1134U,0U},
  {0U,2906U,2344U,0U},
  {0U,2906U,4893U,0U},
  {0U,2907U,2675U,0U},
  {0U,2908U,4501U,0U},
  {0U,2909U,2926U,0U},
  {0U,2910U,3234U,0U},
  {0U,2911U,4527U,0U},
  {0U,2912U,4137U,0U},
  {0U,2913U,4428U,0U},
  {0U,2914U,218U,0U},
  {0U,2915U,1069U,0U},
  {0U,2916U,2515U,0U},
  {0U,2917U,1309U,0U},
  {0U,2918U,3615U,0U},
  {0U,2919U,3511U,0U},
  {0U,2920U,3171U,0U},
  {0U,2921U,1533U,0U},
  {0U,2922U,1579U,0U},
  {0U,2923U,4824U,0U},
  {0U,2924U,218U,0U},
  {0U,2925U,4203U,0U},
  {0U,2926U,2909U,0U},
  {0U,2927U,3077U,0U},
  {0U,2928U,1779U,0U},
  {0U,2928U,4719U,0U},
  {0U,2929U,669U,0U},
  {0U,2930U,2250U,0U},
  {0U,2931U,2681U,0U},
  {0U,2932U,2745U,0U},
  {0U,2933U,3676U,0U},
  {0U,2934U,3273U,0U},
  {0U,2935U,2893U,0U},
  {0U,2936U,3426U,0U},
  {0U,2937U,3498U,0U},
  {0U,2938U,1742U,0U},
  {0U,2939U,2631U,0U},
  {0U,2940U,222U,0U},
  {0U,2941U,2970U,0U},
  {0U,2942U,3476U,0U},
  {0U,2943U,4907U,0U},
  {0U,2944U,2115U,0U},
  {0U,2945U,4908U,0U},
  {0U,2946U,708U,0U},
  {0U,2947U,4909U,0U},
  {0U,2948U,3616U,0U},
  {0U,2949U,3034U,0U},
  {0U,2950U,71U,0U},
  {0U,2951U,3604U,0U},
  {0U,2952U,811U,0U},
  {0U,2953U,2352U,0U},
  {0U,2954U,2956U,0U},
  {0U,2955U,4910U,0U},
  {0U,2956U,2954U,0U},
  {0U,2957U,2959U,0U},
  {0U,2958U,2960U,0U},
  {0U,2959U,2957U,0U},
  {0U,2960U,2958U,0U},
  {0U,2961U,4911U,0U},
  {0U,2962U,2033U,0U},
  {0U,2963U,2222U,0U},
  {0U,2964U,3023U,0U},
  {0U,2965U,3657U,0U},
  {0U,2966U,2278U,0U},
  {0U,2967U,1293U,0U},
  {0U,2968U,3942U,0U},
  {0U,2969U,4138U,0U},
  {0U,2970U,2941U,0U},
  {0U,2971U,1389U,0U},
  {0U,2972U,2026U,0U},
  {0U,2973U,1048U,0U},
  {0U,2974U,2159U,0U},
  {0U,2975U,235U,0U},
  {0U,2976U,1571U,0U},
  {0U,2977U,3197U,0U},
  {0U,2978U,1727U,0U},
  {0U,2979U,1545U,0U},
  {0U,2980U,4915U,0U},
  {0U,2981U,238U,0U},
  {0U,2982U,4916U,0U},
  {0U,2983U,4917U,0U},
  {0U,2984U,4777U,0U},
  {0U,2985U,4920U,0U},
  {0U,2986U,238U,0U},
  {0U,2987U,663U,0U},
  {0U,2988U,3949U,0U},
  {0U,2989U,4913U,0U},
  {0U,2990U,4610U,0U},
  {0U,2991U,2478U,0U},
  {0U,2992U,2326U,0U},
  {0U,2993U,2995U,0U},
  {0U,2994U,4668U,0U},
  {0U,2995U,2993U,0U},
  {0U,2996U,4921U,0U},
  {0U,2997U,4693U,0U},
  {0U,2998U,3185U,0U},
  {0U,2999U,2661U,0U},
  {0U,3000U,4923U,0U},
  {0U,3001U,2582U,0U},
  {0U,3002U,3249U,0U},
  {0U,3003U,240U,0U},
  {0U,3004U,2291U,0U},
  {0U,3005U,3929U,0U},
  {0U,3006U,244U,0U},
  {0U,3007U,4742U,0U},
  {0U,3008U,71U,0U},
  {0U,3009U,3932U,0U},
  {0U,3010U,4927U,0U},
  {0U,3011U,963U,0U},
  {0U,3012U,247U,0U},
  {0U,3013U,4928U,0U},
  {0U,3014U,3704U,0U},
  {0U,3015U,1255U,0U},
  {0U,3016U,2716U,0U},
  {0U,3017U,2519U,0U},
  {0U,3017U,3038U,0U},
  {0U,3017U,4109U,0U},
  {0U,3018U,3042U,0U},
  {0U,3019U,250U,0U},
  {0U,3020U,3309U,0U},
  {0U,3020U,3310U,0U},
  {0U,3021U,1146U,0U},
  {0U,3022U,252U,0U},
  {0U,3023U,2964U,0U},
  {0U,3024U,1628U,0U},
  {0U,3025U,2746U,0U},
  {0U,3026U,1772U,0U},
  {0U,3027U,2662U,0U},
  {0U,3028U,2666U,0U},
  {0U,3029U,2764U,0U},
  {0U,3030U,255U,0U},
  {0U,3031U,284U,0U},
  {0U,3031U,714U,0U},
  {0U,3032U,3874U,0U},
  {0U,3033U,4039U,0U},
  {0U,3034U,2949U,0U},
  {0U,3035U,261U,0U},
  {0U,3036U,2474U,0U},
  {0U,3037U,2279U,0U},
  {0U,3038U,2537U,0U},
  {0U,3038U,3017U,0U},
  {0U,3038U,3048U,0U},
  {0U,3039U,1546U,0U},
  {0U,3040U,4194U,0U},
  {0U,3041U,1552U,0U},
  {0U,3042U,3018U,0U},
  {0U,3043U,2642U,0U},
  {0U,3044U,2826U,0U},
  {0U,3045U,2551U,0U},
  {0U,3046U,3063U,0U},
  {0U,3047U,2529U,0U},
  {0U,3048U,3038U,0U},
  {0U,3049U,4195U,0U},
  {0U,3050U,2533U,0U},
  {0U,3051U,4188U,0U},
  {0U,3052U,4795U,0U},
  {0U,3053U,4187U,0U},
  {0U,3054U,2403U,0U},
  {0U,3055U,2546U,0U},
  {0U,3056U,3069U,0U},
  {0U,3057U,2543U,0U},
  {0U,3058U,1542U,0U},
  {0U,3059U,1542U,0U},
  {0U,3060U,1539U,0U},
  {0U,3061U,3072U,0U},
  {0U,3062U,1542U,0U},
  {0U,3063U,3046U,0U},
  {0U,3064U,3078U,0U},
  {0U,3065U,260U,0U},
  {0U,3066U,2523U,0U},
  {0U,3067U,2542U,0U},
  {0U,3068U,2521U,0U},
  {0U,3069U,3056U,0U},
  {0U,3070U,4938U,0U},
  {0U,3071U,2545U,0U},
  {0U,3072U,2520U,0U},
  {0U,3072U,3061U,0U},
  {0U,3073U,4939U,0U},
  {0U,3074U,4498U,0U},
  {0U,3075U,2556U,0U},
  {0U,3076U,3080U,0U},
  {0U,3076U,4167U,0U},
  {0U,3077U,2927U,0U},
  {0U,3078U,2153U,0U},
  {0U,3078U,3064U,0U},
  {0U,3079U,2540U,0U},
  {0U,3080U,3076U,0U},
  {0U,3081U,4149U,0U},
  {0U,3082U,2332U,0U},
  {0U,3083U,3622U,0U},
  {0U,3084U,2800U,0U},
  {0U,3085U,771U,0U},
  {0U,3086U,3166U,0U},
  {0U,3087U,3579U,0U},
  {0U,3088U,4941U,0U},
  {0U,3089U,263U,0U},
  {0U,3090U,383U,0U},
  {0U,3091U,3749U,0U},
  {0U,3092U,3134U,0U},
  {0U,3093U,3710U,0U},
  {0U,3094U,4943U,0U},
  {0U,3095U,2337U,0U},
  {0U,3096U,3644U,0U},
  {0U,3097U,519U,0U},
  {0U,3098U,264U,0U},
  {0U,3099U,1393U,0U},
  {0U,3100U,2379U,0U},
  {0U,3101U,3283U,0U},
  {0U,3102U,4820U,0U},
  {0U,3103U,4669U,0U},
  {0U,3104U,586U,0U},
  {0U,3105U,995U,0U},
  {0U,3106U,3109U,0U},
  {0U,3107U,3861U,0U},
  {0U,3108U,2504U,0U},
  {0U,3109U,3106U,0U},
  {0U,3110U,3301U,0U},
  {0U,3111U,4099U,0U},
  {0U,3112U,3199U,0U},
  {0U,3113U,1944U,0U},
  {0U,3114U,2510U,0U},
  {0U,3115U,2823U,0U},
  {0U,3116U,4731U,0U},
  {0U,3117U,3119U,0U},
  {0U,3118U,3274U,0U},
  {0U,3119U,3117U,0U},
  {0U,3120U,715U,0U},
  {0U,3121U,1884U,0U},
  {0U,3122U,3723U,0U},
  {0U,3123U,1090U,0U},
  {0U,3124U,2477U,0U},
  {0U,3125U,4951U,0U},
  {0U,3126U,987U,0U},
  {0U,3127U,3133U,0U},
  {0U,3128U,3464U,0U},
  {0U,3129U,271U,0U},
  {0U,3130U,907U,0U},
  {0U,3131U,4789U,0U},
  {0U,3132U,271U,0U},
  {0U,3133U,3127U,0U},
  {0U,3134U,3092U,0U},
  {0U,3134U,3241U,0U},
  {0U,3135U,4180U,0U},
  {0U,3135U,4380U,0U},
  {0U,3136U,2708U,0U},
  {0U,3137U,1700U,0U},
  {0U,3138U,4692U,0U},
  {0U,3139U,777U,0U},
  {0U,3140U,1193U,0U},
  {0U,3141U,2182U,0U},
  {0U,3142U,3532U,0U},
  {0U,3143U,3687U,0U},
  {0U,3144U,2680U,0U},
  {0U,3145U,2249U,0U},
  {0U,3146U,463U,0U},
  {0U,3147U,470U,0U},
  {0U,3148U,3238U,0U},
  {0U,3149U,2561U,0U},
  {0U,3150U,539U,0U},
  {0U,3151U,3152U,0U},
  {0U,3152U,3151U,0U},
  {0U,3153U,4816U,0U},
  {0U,3154U,4957U,0U},
  {0U,3155U,682U,0U},
  {0U,3156U,3842U,0U},
  {0U,3157U,2566U,0U},
  {0U,3158U,3477U,0U},
  {0U,3159U,1896U,0U},
  {0U,3160U,2635U,0U},
  {0U,3161U,71U,0U},
  {0U,3162U,2456U,0U},
  {0U,3163U,3774U,0U},
  {0U,3164U,285U,0U},
  {0U,3165U,318U,0U},
  {0U,3166U,3086U,0U},
  {0U,3167U,382U,0U},
  {0U,3168U,665U,0U},
  {0U,3169U,380U,0U},
  {0U,3170U,1890U,0U},
  {0U,3171U,2920U,0U},
  {0U,3172U,3173U,0U},
  {0U,3173U,3172U,0U},
  {0U,3174U,288U,0U},
  {0U,3175U,71U,0U},
  {0U,3176U,1607U,0U},
  {0U,3177U,1693U,0U},
  {0U,3178U,3919U,0U},
  {0U,3179U,1909U,0U},
  {0U,3180U,4959U,0U},
  {0U,3181U,2862U,0U},
  {0U,3182U,289U,0U},
  {0U,3183U,1738U,0U},
  {0U,3184U,4960U,0U},
  {0U,3185U,2998U,0U},
  {0U,3186U,2787U,0U},
  {0U,3187U,3191U,0U},
  {0U,3188U,4152U,0U},
  {0U,3189U,2785U,0U},
  {0U,3190U,2782U,0U},
  {0U,3191U,3187U,0U},
  {0U,3192U,3468U,0U},
  {0U,3193U,2783U,0U},
  {0U,3194U,2324U,0U},
  {0U,3195U,401U,0U},
  {0U,3196U,1403U,0U},
  {0U,3197U,2977U,0U},
  {0U,3198U,3800U,0U},
  {0U,3199U,1308U,0U},
  {0U,3199U,3112U,0U},
  {0U,3200U,2253U,0U},
  {0U,3201U,1188U,0U},
  {0U,3202U,1274U,0U},
  {0U,3203U,4481U,0U},
  {0U,3204U,1686U,0U},
  {0U,3205U,4116U,0U},
  {0U,3206U,1560U,0U},
  {0U,3207U,2771U,0U},
  {0U,3208U,3269U,0U},
  {0U,3209U,4338U,0U},
  {0U,3210U,4287U,0U},
  {0U,3211U,2758U,0U},
  {0U,3212U,4013U,0U},
  {0U,3213U,2872U,0U},
  {0U,3214U,323U,0U},
  {0U,3215U,4181U,0U},
  {0U,3216U,295U,0U},
  {0U,3217U,295U,0U},
  {0U,3218U,2184U,0U},
  {0U,3219U,3630U,0U},
  {0U,3220U,4364U,0U},
  {0U,3221U,2060U,0U},
  {0U,3222U,2842U,0U},
  {0U,3223U,2505U,0U},
  {0U,3224U,3817U,0U},
  {0U,3225U,2172U,0U},
  {0U,3226U,2184U,0U},
  {0U,3227U,3293U,0U},
  {0U,3228U,3845U,0U},
  {0U,3229U,4817U,0U},
  {0U,3230U,4966U,0U},
  {0U,3231U,2242U,0U},
  {0U,3232U,2178U,0U},
  {0U,3233U,3330U,0U},
  {0U,3234U,2910U,0U},
  {0U,3235U,4969U,0U},
  {0U,3236U,4004U,0U},
  {0U,3237U,1252U,0U},
  {0U,3238U,3148U,0U},
  {0U,3239U,71U,0U},
  {0U,3240U,420U,0U},
  {0U,3241U,3134U,0U},
  {0U,3242U,4977U,0U},
  {0U,3243U,4081U,0U},
  {0U,3244U,3250U,0U},
  {0U,3245U,420U,0U},
  {0U,3246U,3337U,0U},
  {0U,3247U,321U,0U},
  {0U,3247U,3394U,0U},
  {0U,3248U,341U,0U},
  {0U,3249U,3002U,0U},
  {0U,3250U,3244U,0U},
  {0U,3251U,4984U,0U},
  {0U,3252U,2759U,0U},
  {0U,3253U,341U,0U},
  {0U,3254U,3802U,0U},
  {0U,3255U,4985U,0U},
  {0U,3256U,2669U,0U},
  {0U,3257U,1499U,0U},
  {0U,3258U,298U,0U},
  {0U,3259U,3381U,0U},
  {0U,3260U,4896U,0U},
  {0U,3261U,1250U,0U},
  {0U,3262U,4425U,0U},
  {0U,3263U,2172U,0U},
  {0U,3264U,1708U,0U},
  {0U,3265U,3268U,0U},
  {0U,3266U,321U,0U},
  {0U,3267U,3392U,0U},
  {0U,3268U,3265U,0U},
  {0U,3269U,3208U,0U},
  {0U,3270U,2597U,0U},
  {0U,3271U,302U,0U},
  {0U,3272U,303U,0U},
  {0U,3273U,2934U,0U},
  {0U,3274U,3118U,0U},
  {0U,3275U,3353U,0U},
  {0U,3276U,305U,0U},
  {0U,3277U,1006U,0U},
  {0U,3278U,2288U,0U},
  {0U,3279U,937U,0U},
  {0U,3280U,308U,0U},
  {0U,3281U,2497U,0U},
  {0U,3282U,2572U,0U},
  {0U,3283U,3101U,0U},
  {0U,3284U,3717U,0U},
  {0U,3285U,2184U,0U},
  {0U,3286U,3503U,0U},
  {0U,3287U,308U,0U},
  {0U,3288U,500U,0U},
  {0U,3289U,1627U,0U},
  {0U,3290U,332U,0U},
  {0U,3291U,4988U,0U},
  {0U,3292U,1263U,0U},
  {0U,3293U,323U,0U},
  {0U,3293U,3227U,0U},
  {0U,3294U,4534U,0U},
  {0U,3295U,4484U,0U},
  {0U,3296U,4115U,0U},
  {0U,3297U,4545U,0U},
  {0U,3298U,2335U,0U},
  {0U,3299U,2776U,0U},
  {0U,3300U,328U,0U},
  {0U,3301U,2488U,0U},
  {0U,3301U,3110U,0U},
  {0U,3302U,3307U,0U},
  {0U,3303U,3306U,0U},
  {0U,3304U,2066U,0U},
  {0U,3305U,2384U,0U},
  {0U,3306U,3303U,0U},
  {0U,3307U,3302U,0U},
  {0U,3308U,1225U,0U},
  {0U,3309U,3020U,0U},
  {0U,3310U,3020U,0U},
  {0U,3311U,2461U,0U},
  {0U,3312U,2856U,0U},
  {0U,3313U,839U,0U},
  {0U,3314U,712U,0U},
  {0U,3315U,1033U,0U},
  {0U,3316U,346U,0U},
  {0U,3317U,1866U,0U},
  {0U,3318U,1232U,0U},
  {0U,3319U,927U,0U},
  {0U,3320U,2263U,0U},
  {0U,3321U,592U,0U},
  {0U,3322U,2127U,0U},
  {0U,3323U,3549U,0U},
  {0U,3324U,885U,0U},
  {0U,3325U,945U,0U},
  {0U,3326U,71U,0U},
  {0U,3327U,843U,0U},
  {0U,3328U,986U,0U},
  {0U,3329U,1312U,0U},
  {0U,3330U,3233U,0U},
  {0U,3331U,4992U,0U},
  {0U,3332U,788U,0U},
  {0U,3333U,3334U,0U},
  {0U,3334U,3333U,0U},
  {0U,3335U,2600U,0U},
  {0U,3336U,4981U,0U},
  {0U,3337U,3246U,0U},
  {0U,3338U,1266U,0U},
  {0U,3339U,1601U,0U},
  {0U,3340U,1338U,0U},
  {0U,3341U,4993U,0U},
  {0U,3342U,4018U,0U},
  {0U,3342U,4062U,0U},
  {0U,3343U,1272U,0U},
  {0U,3344U,3345U,0U},
  {0U,3345U,3344U,0U},
  {0U,3346U,5000U,0U},
  {0U,3347U,5002U,0U},
  {0U,3348U,5003U,0U},
  {0U,3349U,4903U,0U},
  {0U,3350U,407U,0U},
  {0U,3351U,3380U,0U},
  {0U,3352U,3765U,0U},
  {0U,3353U,3275U,0U},
  {0U,3354U,367U,0U},
  {0U,3355U,1631U,0U},
  {0U,3356U,436U,0U},
  {0U,3357U,4506U,0U},
  {0U,3358U,2725U,0U},
  {0U,3359U,419U,0U},
  {0U,3360U,2903U,0U},
  {0U,3361U,5009U,0U},
  {0U,3362U,3363U,0U},
  {0U,3362U,4039U,0U},
  {0U,3363U,3362U,0U},
  {0U,3364U,651U,0U},
  {0U,3365U,4201U,0U},
  {0U,3366U,666U,0U},
  {0U,3367U,664U,0U},
  {0U,3368U,5010U,0U},
  {0U,3369U,1696U,0U},
  {0U,3370U,3806U,0U},
  {0U,3371U,1011U,0U},
  {0U,3372U,423U,0U},
  {0U,3373U,3873U,0U},
  {0U,3374U,5012U,0U},
  {0U,3375U,1454U,0U},
  {0U,3376U,2291U,0U},
  {0U,3377U,330U,0U},
  {0U,3378U,3631U,0U},
  {0U,3379U,332U,0U},
  {0U,3380U,3351U,0U},
  {0U,3381U,3259U,0U},
  {0U,3382U,768U,0U},
  {0U,3383U,5014U,0U},
  {0U,3384U,1163U,0U},
  {0U,3385U,336U,0U},
  {0U,3386U,336U,0U},
  {0U,3387U,3388U,0U},
  {0U,3388U,3387U,0U},
  {0U,3388U,4274U,0U},
  {0U,3389U,2580U,0U},
  {0U,3390U,5015U,0U},
  {0U,3391U,759U,0U},
  {0U,3392U,3267U,0U},
  {0U,3393U,5016U,0U},
  {0U,3394U,3247U,0U},
  {0U,3395U,2876U,0U},
  {0U,3396U,5019U,0U},
  {0U,3397U,4043U,0U},
  {0U,3398U,4573U,0U},
  {0U,3399U,4400U,0U},
  {0U,3400U,3985U,0U},
  {0U,3401U,2722U,0U},
  {0U,3402U,482U,0U},
  {0U,3403U,2743U,0U},
  {0U,3404U,1646U,0U},
  {0U,3405U,2739U,0U},
  {0U,3406U,2087U,0U},
  {0U,3407U,5022U,0U},
  {0U,3408U,345U,0U},
  {0U,3409U,5023U,0U},
  {0U,3410U,1632U,0U},
  {0U,3411U,882U,0U},
  {0U,3412U,350U,0U},
  {0U,3413U,351U,0U},
  {0U,3414U,5025U,0U},
  {0U,3415U,1639U,0U},
  {0U,3416U,354U,0U},
  {0U,3417U,3562U,0U},
  {0U,3418U,2828U,0U},
  {0U,3419U,3544U,0U},
  {0U,3420U,71U,0U},
  {0U,3421U,3425U,0U},
  {0U,3422U,1424U,0U},
  {0U,3423U,2158U,0U},
  {0U,3424U,3859U,0U},
  {0U,3425U,3421U,0U},
  {0U,3426U,2936U,0U},
  {0U,3427U,2514U,0U},
  {0U,3428U,3535U,0U},
  {0U,3429U,3499U,0U},
  {0U,3430U,770U,0U},
  {0U,3431U,4746U,0U},
  {0U,3432U,360U,0U},
  {0U,3433U,3709U,0U},
  {0U,3434U,2259U,0U},
  {0U,3435U,3815U,0U},
  {0U,3436U,1838U,0U},
  {0U,3437U,3438U,0U},
  {0U,3438U,3437U,0U},
  {0U,3439U,3863U,0U},
  {0U,3440U,3445U,0U},
  {0U,3440U,3988U,0U},
  {0U,3441U,2573U,0U},
  {0U,3442U,4016U,0U},
  {0U,3443U,4159U,0U},
  {0U,3443U,5029U,0U},
  {0U,3444U,3625U,0U},
  {0U,3445U,3440U,0U},
  {0U,3446U,5030U,0U},
  {0U,3447U,3886U,0U},
  {0U,3448U,3610U,0U},
  {0U,3449U,367U,0U},
  {0U,3450U,491U,0U},
  {0U,3451U,1612U,0U},
  {0U,3452U,1394U,0U},
  {0U,3453U,2783U,0U},
  {0U,3454U,995U,0U},
  {0U,3455U,5033U,0U},
  {0U,3456U,2650U,0U},
  {0U,3457U,425U,0U},
  {0U,3458U,2748U,0U},
  {0U,3459U,373U,0U},
  {0U,3460U,5034U,0U},
  {0U,3461U,3661U,0U},
  {0U,3462U,4096U,0U},
  {0U,3463U,3502U,0U},
  {0U,3464U,3128U,0U},
  {0U,3465U,4369U,0U},
  {0U,3466U,666U,0U},
  {0U,3467U,2021U,0U},
  {0U,3468U,3192U,0U},
  {0U,3469U,4451U,0U},
  {0U,3470U,853U,0U},
  {0U,3471U,1698U,0U},
  {0U,3472U,1081U,0U},
  {0U,3473U,859U,0U},
  {0U,3474U,4513U,0U},
  {0U,3475U,2656U,0U},
  {0U,3476U,2942U,0U},
  {0U,3477U,3158U,0U},
  {0U,3478U,2266U,0U},
  {0U,3479U,3480U,0U},
  {0U,3480U,3479U,0U},
  {0U,3481U,4860U,0U},
  {0U,3482U,4781U,0U},
  {0U,3483U,2152U,0U},
  {0U,3484U,5038U,0U},
  {0U,3485U,2703U,0U},
  {0U,3486U,3762U,0U},
  {0U,3487U,383U,0U},
  {0U,3488U,3489U,0U},
  {0U,3489U,3488U,0U},
  {0U,3490U,3586U,0U},
  {0U,3491U,873U,0U},
  {0U,3492U,4303U,0U},
  {0U,3493U,4800U,0U},
  {0U,3494U,408U,0U},
  {0U,3495U,2830U,0U},
  {0U,3496U,5042U,0U},
  {0U,3497U,4097U,0U},
  {0U,3498U,2937U,0U},
  {0U,3499U,3429U,0U},
  {0U,3500U,3888U,0U},
  {0U,3501U,2755U,0U},
  {0U,3502U,3463U,0U},
  {0U,3503U,3286U,0U},
  {0U,3504U,4033U,0U},
  {0U,3505U,1575U,0U},
  {0U,3506U,5043U,0U},
  {0U,3507U,3585U,0U},
  {0U,3508U,1015U,0U},
  {0U,3509U,3979U,0U},
  {0U,3510U,1994U,0U},
  {0U,3511U,2919U,0U},
  {0U,3512U,394U,0U},
  {0U,3513U,1079U,0U},
  {0U,3514U,2375U,0U},
  {0U,3515U,4671U,0U},
  {0U,3516U,3798U,0U},
  {0U,3517U,2246U,0U},
  {0U,3518U,610U,0U},
  {0U,3519U,4001U,0U},
  {0U,3520U,5005U,0U},
  {0U,3521U,4464U,0U},
  {0U,3522U,1637U,0U},
  {0U,3523U,5234U,0U},
  {0U,3524U,2765U,0U},
  {0U,3525U,2368U,0U},
  {0U,3526U,2838U,0U},
  {0U,3527U,5047U,0U},
  {0U,3528U,3551U,0U},
  {0U,3529U,3901U,0U},
  {0U,3530U,1405U,0U},
  {0U,3531U,502U,0U},
  {0U,3532U,3142U,0U},
  {0U,3533U,1918U,0U},
  {0U,3534U,975U,0U},
  {0U,3535U,3428U,0U},
  {0U,3536U,3537U,0U},
  {0U,3537U,3536U,0U},
  {0U,3538U,1954U,0U},
  {0U,3539U,2381U,0U},
  {0U,3540U,3541U,0U},
  {0U,3541U,3540U,0U},
  {0U,3542U,2865U,0U},
  {0U,3543U,453U,0U},
  {0U,3544U,3419U,0U},
  {0U,3545U,4050U,0U},
  {0U,3545U,4545U,0U},
  {0U,3545U,5048U,0U},
  {0U,3546U,4424U,0U},
  {0U,3547U,1275U,0U},
  {0U,3548U,991U,0U},
  {0U,3549U,3323U,0U},
  {0U,3550U,661U,0U},
  {0U,3551U,3528U,0U},
  {0U,3552U,423U,0U},
  {0U,3553U,497U,0U},
  {0U,3554U,3977U,0U},
  {0U,3555U,4452U,0U},
  {0U,3556U,658U,0U},
  {0U,3557U,5054U,0U},
  {0U,3558U,4144U,0U},
  {0U,3559U,4084U,0U},
  {0U,3559U,5045U,0U},
  {0U,3560U,2527U,0U},
  {0U,3560U,2534U,0U},
  {0U,3561U,1482U,0U},
  {0U,3562U,3417U,0U},
  {0U,3563U,2402U,0U},
  {0U,3564U,5058U,0U},
  {0U,3565U,3566U,0U},
  {0U,3566U,3565U,0U},
  {0U,3567U,434U,0U},
  {0U,3568U,434U,0U},
  {0U,3569U,3570U,0U},
  {0U,3570U,3569U,0U},
  {0U,3571U,1957U,0U},
  {0U,3571U,4623U,0U},
  {0U,3572U,5059U,0U},
  {0U,3573U,441U,0U},
  {0U,3574U,443U,0U},
  {0U,3575U,444U,0U},
  {0U,3576U,1535U,0U},
  {0U,3577U,1535U,0U},
  {0U,3578U,1106U,0U},
  {0U,3579U,3087U,0U},
  {0U,3580U,960U,0U},
  {0U,3581U,3917U,0U},
  {0U,3582U,5060U,0U},
  {0U,3583U,1290U,0U},
  {0U,3584U,5061U,0U},
  {0U,3585U,3507U,0U},
  {0U,3586U,3490U,0U},
  {0U,3587U,2339U,0U},
  {0U,3588U,4362U,0U},
  {0U,3589U,3650U,0U},
  {0U,3590U,2152U,0U},
  {0U,3591U,2879U,0U},
  {0U,3592U,993U,0U},
  {0U,3592U,4007U,0U},
  {0U,3593U,930U,0U},
  {0U,3594U,2141U,0U},
  {0U,3595U,1751U,0U},
  {0U,3596U,3597U,0U},
  {0U,3597U,3596U,0U},
  {0U,3598U,1840U,0U},
  {0U,3599U,458U,0U},
  {0U,3600U,5063U,0U},
  {0U,3601U,3602U,0U},
  {0U,3602U,3601U,0U},
  {0U,3603U,467U,0U},
  {0U,3604U,2951U,0U},
  {0U,3605U,1200U,0U},
  {0U,3606U,3607U,0U},
  {0U,3607U,3606U,0U},
  {0U,3608U,728U,0U},
  {0U,3609U,1374U,0U},
  {0U,3610U,3448U,0U},
  {0U,3611U,2774U,0U},
  {0U,3612U,2445U,0U},
  {0U,3612U,3967U,0U},
  {0U,3613U,2265U,0U},
  {0U,3614U,5068U,0U},
  {0U,3615U,2918U,0U},
  {0U,3616U,2948U,0U},
  {0U,3617U,3972U,0U},
  {0U,3618U,2292U,0U},
  {0U,3619U,2245U,0U},
  {0U,3620U,3914U,0U},
  {0U,3621U,2571U,0U},
  {0U,3622U,3083U,0U},
  {0U,3623U,1295U,0U},
  {0U,3624U,1995U,0U},
  {0U,3625U,3444U,0U},
  {0U,3626U,1387U,0U},
  {0U,3627U,4869U,0U},
  {0U,3628U,1463U,0U},
  {0U,3629U,4094U,0U},
  {0U,3630U,3219U,0U},
  {0U,3631U,3378U,0U},
  {0U,3632U,4128U,0U},
  {0U,3633U,1732U,0U},
  {0U,3634U,1424U,0U},
  {0U,3635U,502U,0U},
  {0U,3636U,505U,0U},
  {0U,3637U,508U,0U},
  {0U,3638U,3662U,0U},
  {0U,3639U,2216U,0U},
  {0U,3640U,1348U,0U},
  {0U,3641U,3976U,0U},
  {0U,3642U,3775U,0U},
  {0U,3643U,1895U,0U},
  {0U,3644U,3096U,0U},
  {0U,3645U,2145U,0U},
  {0U,3646U,2836U,0U},
  {0U,3647U,515U,0U},
  {0U,3648U,516U,0U},
  {0U,3649U,1501U,0U},
  {0U,3650U,3589U,0U},
  {0U,3651U,3652U,0U},
  {0U,3652U,3651U,0U},
  {0U,3653U,521U,0U},
  {0U,3654U,836U,0U},
  {0U,3655U,1973U,0U},
  {0U,3655U,3656U,0U},
  {0U,3656U,3655U,0U},
  {0U,3657U,2965U,0U},
  {0U,3658U,531U,0U},
  {0U,3659U,4034U,0U},
  {0U,3660U,533U,0U},
  {0U,3661U,3461U,0U},
  {0U,3662U,3638U,0U},
  {0U,3663U,5069U,0U},
  {0U,3664U,1248U,0U},
  {0U,3665U,1351U,0U},
  {0U,3666U,5070U,0U},
  {0U,3667U,703U,0U},
  {0U,3668U,556U,0U},
  {0U,3669U,2706U,0U},
  {0U,3670U,561U,0U},
  {0U,3671U,2218U,0U},
  {0U,3672U,4804U,0U},
  {0U,3673U,2835U,0U},
  {0U,3674U,4689U,0U},
  {0U,3675U,1855U,0U},
  {0U,3676U,2933U,0U},
  {0U,3677U,3980U,0U},
  {0U,3678U,2337U,0U},
  {0U,3679U,2227U,0U},
  {0U,3680U,2088U,0U},
  {0U,3681U,589U,0U},
  {0U,3682U,929U,0U},
  {0U,3683U,71U,0U},
  {0U,3684U,2696U,0U},
  {0U,3685U,2511U,0U},
  {0U,3686U,587U,0U},
  {0U,3687U,3143U,0U},
  {0U,3688U,4961U,0U},
  {0U,3689U,4077U,0U},
  {0U,3690U,71U,0U},
  {0U,3691U,1987U,0U},
  {0U,3692U,5073U,0U},
  {0U,3693U,4041U,0U},
  {0U,3694U,5031U,0U},
  {0U,3695U,1791U,0U},
  {0U,3696U,5075U,0U},
  {0U,3697U,4047U,0U},
  {0U,3698U,619U,0U},
  {0U,3699U,1590U,0U},
  {0U,3700U,974U,0U},
  {0U,3701U,4942U,0U},
  {0U,3702U,4474U,0U},
  {0U,3703U,1460U,0U},
  {0U,3704U,3014U,0U},
  {0U,3705U,1936U,0U},
  {0U,3706U,1872U,0U},
  {0U,3707U,729U,0U},
  {0U,3708U,1697U,0U},
  {0U,3709U,877U,0U},
  {0U,3709U,3433U,0U},
  {0U,3710U,3093U,0U},
  {0U,3711U,629U,0U},
  {0U,3712U,5078U,0U},
  {0U,3713U,1850U,0U},
  {0U,3714U,1976U,0U},
  {0U,3715U,2454U,0U},
  {0U,3716U,3846U,0U},
  {0U,3717U,3284U,0U},
  {0U,3718U,4309U,0U},
  {0U,3719U,1413U,0U},
  {0U,3720U,2041U,0U},
  {0U,3721U,4565U,0U},
  {0U,3721U,4905U,0U},
  {0U,3722U,800U,0U},
  {0U,3723U,3122U,0U},
  {0U,3724U,3973U,0U},
  {0U,3725U,816U,0U},
  {0U,3726U,3903U,0U},
  {0U,3727U,3728U,0U},
  {0U,3728U,3727U,0U},
  {0U,3729U,1125U,0U},
  {0U,3730U,1711U,0U},
  {0U,3731U,2330U,0U},
  {0U,3732U,662U,0U},
  {0U,3733U,5083U,0U},
  {0U,3734U,5084U,0U},
  {0U,3735U,2360U,0U},
  {0U,3736U,672U,0U},
  {0U,3737U,1029U,0U},
  {0U,3738U,1806U,0U},
  {0U,3739U,676U,0U},
  {0U,3740U,677U,0U},
  {0U,3741U,2487U,0U},
  {0U,3742U,5085U,0U},
  {0U,3743U,688U,0U},
  {0U,3744U,697U,0U},
  {0U,3745U,1657U,0U},
  {0U,3746U,2386U,0U},
  {0U,3747U,1000U,0U},
  {0U,3748U,707U,0U},
  {0U,3749U,3091U,0U},
  {0U,3750U,4037U,0U},
  {0U,3750U,4054U,0U},
  {0U,3751U,2105U,0U},
  {0U,3752U,718U,0U},
  {0U,3753U,3924U,0U},
  {0U,3754U,3776U,0U},
  {0U,3755U,5094U,0U},
  {0U,3756U,722U,0U},
  {0U,3757U,5098U,0U},
  {0U,3758U,2613U,0U},
  {0U,3759U,2031U,0U},
  {0U,3760U,1820U,0U},
  {0U,3761U,738U,0U},
  {0U,3762U,3486U,0U},
  {0U,3763U,922U,0U},
  {0U,3764U,5099U,0U},
  {0U,3765U,3352U,0U},
  {0U,3766U,745U,0U},
  {0U,3767U,751U,0U},
  {0U,3768U,1390U,0U},
  {0U,3769U,5100U,0U},
  {0U,3770U,764U,0U},
  {0U,3771U,2705U,0U},
  {0U,3772U,4148U,0U},
  {0U,3773U,2155U,0U},
  {0U,3774U,3163U,0U},
  {0U,3775U,3642U,0U},
  {0U,3776U,3754U,0U},
  {0U,3777U,3927U,0U},
  {0U,3778U,1160U,0U},
  {0U,3779U,2536U,0U},
  {0U,3780U,3912U,0U},
  {0U,3781U,799U,0U},
  {0U,3782U,5102U,0U},
  {0U,3783U,2160U,0U},
  {0U,3784U,1055U,0U},
  {0U,3785U,812U,0U},
  {0U,3786U,4691U,0U},
  {0U,3787U,2110U,0U},
  {0U,3788U,2283U,0U},
  {0U,3789U,4643U,0U},
  {0U,3790U,1615U,0U},
  {0U,3791U,1036U,0U},
  {0U,3792U,1953U,0U},
  {0U,3793U,834U,0U},
  {0U,3794U,2300U,0U},
  {0U,3795U,2749U,0U},
  {0U,3796U,1847U,0U},
  {0U,3797U,2443U,0U},
  {0U,3798U,1315U,0U},
  {0U,3798U,3516U,0U},
  {0U,3799U,841U,0U},
  {0U,3800U,3198U,0U},
  {0U,3801U,4648U,0U},
  {0U,3802U,3254U,0U},
  {0U,3803U,1939U,0U},
  {0U,3804U,852U,0U},
  {0U,3805U,4113U,0U},
  {0U,3806U,3370U,0U},
  {0U,3807U,1023U,0U},
  {0U,3808U,2725U,0U},
  {0U,3809U,867U,0U},
  {0U,3810U,4350U,0U},
  {0U,3811U,1762U,0U},
  {0U,3812U,5104U,0U},
  {0U,3813U,4206U,0U},
  {0U,3814U,71U,0U},
  {0U,3815U,3435U,0U},
  {0U,3816U,2837U,0U},
  {0U,3817U,3224U,0U},
  {0U,3818U,2036U,0U},
  {0U,3819U,5105U,0U},
  {0U,3820U,1288U,0U},
  {0U,3821U,947U,0U},
  {0U,3822U,5076U,0U},
  {0U,3823U,4609U,0U},
  {0U,3824U,893U,0U},
  {0U,3825U,1861U,0U},
  {0U,3826U,2429U,0U},
  {0U,3827U,3829U,0U},
  {0U,3828U,975U,0U},
  {0U,3829U,3827U,0U},
  {0U,3830U,4805U,0U},
  {0U,3831U,5107U,0U},
  {0U,3832U,916U,0U},
  {0U,3833U,1781U,0U},
  {0U,3834U,4649U,0U},
  {0U,3835U,922U,0U},
  {0U,3836U,922U,0U},
  {0U,3837U,925U,0U},
  {0U,3838U,1437U,0U},
  {0U,3839U,4714U,0U},
  {0U,3840U,5110U,0U},
  {0U,3841U,2190U,0U},
  {0U,3842U,3156U,0U},
  {0U,3843U,71U,0U},
  {0U,3844U,1154U,0U},
  {0U,3845U,3228U,0U},
  {0U,3846U,3716U,0U},
  {0U,3847U,1140U,0U},
  {0U,3848U,1317U,0U},
  {0U,3849U,2648U,0U},
  {0U,3850U,2075U,0U},
  {0U,3851U,1594U,0U},
  {0U,3852U,4515U,0U},
  {0U,3853U,970U,0U},
  {0U,3854U,2558U,0U},
  {0U,3855U,71U,0U},
  {0U,3856U,4705U,0U},
  {0U,3857U,4526U,0U},
  {0U,3858U,4101U,0U},
  {0U,3859U,3424U,0U},
  {0U,3860U,1991U,0U},
  {0U,3861U,3107U,0U},
  {0U,3862U,4270U,0U},
  {0U,3863U,3439U,0U},
  {0U,3864U,2180U,0U},
  {0U,3865U,4676U,0U},
  {0U,3866U,5111U,0U},
  {0U,3867U,4030U,0U},
  {0U,3868U,4815U,0U},
  {0U,3869U,4014U,0U},
  {0U,3870U,990U,0U},
  {0U,3871U,71U,0U},
  {0U,3872U,994U,0U},
  {0U,3873U,3373U,0U},
  {0U,3873U,4763U,0U},
  {0U,3874U,3032U,0U},
  {0U,3875U,2439U,0U},
  {0U,3876U,998U,0U},
  {0U,3877U,2854U,0U},
  {0U,3878U,2852U,0U},
  {0U,3879U,5115U,0U},
  {0U,3880U,1745U,0U},
  {0U,3881U,1004U,0U},
  {0U,3882U,2719U,0U},
  {0U,3883U,5116U,0U},
  {0U,3884U,1013U,0U},
  {0U,3885U,4156U,0U},
  {0U,3886U,3447U,0U},
  {0U,3887U,4726U,0U},
  {0U,3888U,3500U,0U},
  {0U,3889U,2156U,0U},
  {0U,3890U,1024U,0U},
  {0U,3891U,2111U,0U},
  {0U,3892U,1027U,0U},
  {0U,3893U,2866U,0U},
  {0U,3894U,2455U,0U},
  {0U,3895U,1031U,0U},
  {0U,3896U,4035U,0U},
  {0U,3897U,1933U,0U},
  {0U,3898U,1137U,0U},
  {0U,3899U,1061U,0U},
  {0U,3900U,1035U,0U},
  {0U,3901U,3529U,0U},
  {0U,3902U,5081U,0U},
  {0U,3903U,3726U,0U},
  {0U,3904U,1042U,0U},
  {0U,3905U,5120U,0U},
  {0U,3906U,2388U,0U},
  {0U,3907U,3921U,0U},
  {0U,3908U,2784U,0U},
  {0U,3909U,2831U,0U},
  {0U,3910U,4275U,0U},
  {0U,3911U,1059U,0U},
  {0U,3912U,3780U,0U},
  {0U,3913U,1067U,0U},
  {0U,3914U,3620U,0U},
  {0U,3915U,4704U,0U},
  {0U,3916U,1150U,0U},
  {0U,3917U,3581U,0U},
  {0U,3918U,2469U,0U},
  {0U,3919U,3178U,0U},
  {0U,3920U,3962U,0U},
  {0U,3921U,3907U,0U},
  {0U,3922U,1083U,0U},
  {0U,3923U,2811U,0U},
  {0U,3924U,3753U,0U},
  {0U,3925U,1095U,0U},
  {0U,3926U,5124U,0U},
  {0U,3927U,3777U,0U},
  {0U,3928U,1356U,0U},
  {0U,3929U,3005U,0U},
  {0U,3930U,4773U,0U},
  {0U,3931U,1100U,0U},
  {0U,3932U,3009U,0U},
  {0U,3933U,2340U,0U},
  {0U,3934U,1102U,0U},
  {0U,3935U,4733U,0U},
  {0U,3936U,5125U,0U},
  {0U,3937U,1110U,0U},
  {0U,3938U,4157U,0U},
  {0U,3939U,1111U,0U},
  {0U,3940U,1159U,0U},
  {0U,3941U,5127U,0U},
  {0U,3942U,2968U,0U},
  {0U,3943U,5128U,0U},
  {0U,3944U,4023U,0U},
  {0U,3945U,5112U,0U},
  {0U,3946U,2808U,0U},
  {0U,3947U,4944U,0U},
  {0U,3948U,5131U,0U},
  {0U,3949U,2988U,0U},
  {0U,3950U,1383U,0U},
  {0U,3951U,2464U,0U},
  {0U,3952U,1161U,0U},
  {0U,3953U,1163U,0U},
  {0U,3954U,1169U,0U},
  {0U,3955U,1171U,0U},
  {0U,3956U,1172U,0U},
  {0U,3957U,2492U,0U},
  {0U,3958U,3959U,0U},
  {0U,3959U,3958U,0U},
  {0U,3960U,1178U,0U},
  {0U,3961U,1181U,0U},
  {0U,3962U,3920U,0U},
  {0U,3963U,1188U,0U},
  {0U,3964U,4808U,0U},
  {0U,3965U,4358U,0U},
  {0U,3966U,1194U,0U},
  {0U,3967U,3612U,0U},
  {0U,3968U,1322U,0U},
  {0U,3969U,3970U,0U},
  {0U,3970U,3969U,0U},
  {0U,3971U,2310U,0U},
  {0U,3972U,3617U,0U},
  {0U,3973U,3724U,0U},
  {0U,3974U,5132U,0U},
  {0U,3975U,1825U,0U},
  {0U,3976U,3641U,0U},
  {0U,3977U,3554U,0U},
  {0U,3978U,1245U,0U},
  {0U,3979U,3509U,0U},
  {0U,3980U,3677U,0U},
  {0U,3981U,1436U,0U},
  {0U,3982U,1922U,0U},
  {0U,3983U,1986U,0U},
  {0U,3984U,4430U,0U},
  {0U,3985U,3400U,0U},
  {0U,3986U,4412U,0U},
  {0U,3987U,5093U,0U},
  {0U,3988U,3440U,0U},
  {0U,3989U,3990U,0U},
  {0U,3990U,3989U,0U},
  {0U,3991U,4999U,0U},
  {0U,3992U,4967U,0U},
  {0U,3993U,4211U,0U},
  {0U,3994U,5135U,0U},
  {0U,3995U,3996U,0U},
  {0U,3995U,4480U,0U},
  {0U,3996U,3995U,0U},
  {0U,3997U,4989U,0U},
  {0U,3998U,4036U,0U},
  {0U,3999U,1335U,0U},
  {0U,4000U,1254U,0U},
  {0U,4001U,3519U,0U},
  {0U,4002U,4357U,0U},
  {0U,4003U,4844U,0U},
  {0U,4004U,3236U,0U},
  {0U,4005U,1259U,0U},
  {0U,4006U,4052U,0U},
  {0U,4007U,3592U,0U},
  {0U,4008U,4818U,0U},
  {0U,4009U,4983U,0U},
  {0U,4010U,1455U,0U},
  {0U,4011U,5148U,0U},
  {0U,4012U,4345U,0U},
  {0U,4013U,3212U,0U},
  {0U,4014U,3869U,0U},
  {0U,4015U,4221U,0U},
  {0U,4016U,3442U,0U},
  {0U,4017U,4071U,0U},
  {0U,4018U,3342U,0U},
  {0U,4019U,4133U,0U},
  {0U,4020U,1274U,0U},
  {0U,4021U,1755U,0U},
  {0U,4022U,4193U,0U},
  {0U,4023U,1662U,0U},
  {0U,4023U,3944U,0U},
  {0U,4024U,4799U,0U},
  {0U,4025U,4028U,0U},
  {0U,4026U,4072U,0U},
  {0U,4027U,2886U,0U},
  {0U,4028U,4025U,0U},
  {0U,4029U,1278U,0U},
  {0U,4030U,3867U,0U},
  {0U,4030U,4453U,0U},
  {0U,4031U,1298U,0U},
  {0U,4032U,1303U,0U},
  {0U,4033U,3504U,0U},
  {0U,4034U,3659U,0U},
  {0U,4035U,3896U,0U},
  {0U,4036U,3998U,0U},
  {0U,4037U,3750U,0U},
  {0U,4038U,1314U,0U},
  {0U,4039U,3033U,0U},
  {0U,4039U,3362U,0U},
  {0U,4040U,71U,0U},
  {0U,4041U,3693U,0U},
  {0U,4042U,1325U,0U},
  {0U,4043U,3397U,0U},
  {0U,4043U,5158U,0U},
  {0U,4044U,5149U,0U},
  {0U,4045U,4046U,0U},
  {0U,4046U,4045U,0U},
  {0U,4047U,3697U,0U},
  {0U,4047U,4904U,0U},
  {0U,4048U,4063U,0U},
  {0U,4049U,4665U,0U},
  {0U,4050U,3545U,0U},
  {0U,4051U,4218U,0U},
  {0U,4052U,4006U,0U},
  {0U,4053U,1682U,0U},
  {0U,4054U,3750U,0U},
  {0U,4055U,5097U,0U},
  {0U,4056U,2592U,0U},
  {0U,4057U,4797U,0U},
  {0U,4058U,1330U,0U},
  {0U,4059U,5151U,0U},
  {0U,4060U,4629U,0U},
  {0U,4061U,4253U,0U},
  {0U,4062U,3342U,0U},
  {0U,4063U,4048U,0U},
  {0U,4064U,1333U,0U},
  {0U,4065U,1334U,0U},
  {0U,4066U,4386U,0U},
  {0U,4067U,5162U,0U},
  {0U,4068U,4069U,0U},
  {0U,4069U,4068U,0U},
  {0U,4070U,5020U,0U},
  {0U,4071U,4017U,0U},
  {0U,4072U,4026U,0U},
  {0U,4073U,1340U,0U},
  {0U,4074U,5044U,0U},
  {0U,4075U,2163U,0U},
  {0U,4076U,1347U,0U},
  {0U,4077U,3689U,0U},
  {0U,4078U,4106U,0U},
  {0U,4079U,4080U,0U},
  {0U,4080U,4079U,0U},
  {0U,4081U,3243U,0U},
  {0U,4082U,2413U,0U},
  {0U,4083U,1357U,0U},
  {0U,4084U,3559U,0U},
  {0U,4085U,1362U,0U},
  {0U,4086U,1364U,0U},
  {0U,4087U,4405U,0U},
  {0U,4088U,4151U,0U},
  {0U,4089U,1370U,0U},
  {0U,4090U,1375U,0U},
  {0U,4091U,1379U,0U},
  {0U,4092U,2166U,0U},
  {0U,4093U,2359U,0U},
  {0U,4094U,3629U,0U},
  {0U,4095U,5039U,0U},
  {0U,4096U,3462U,0U},
  {0U,4097U,3497U,0U},
  {0U,4098U,2009U,0U},
  {0U,4099U,3111U,0U},
  {0U,4100U,1399U,0U},
  {0U,4101U,3858U,0U},
  {0U,4102U,2229U,0U},
  {0U,4103U,2623U,0U},
  {0U,4104U,4708U,0U},
  {0U,4105U,5168U,0U},
  {0U,4106U,4078U,0U},
  {0U,4107U,2235U,0U},
  {0U,4108U,1710U,0U},
  {0U,4109U,3017U,0U},
  {0U,4110U,4796U,0U},
  {0U,4110U,4940U,0U},
  {0U,4111U,4166U,0U},
  {0U,4112U,1770U,0U},
  {0U,4113U,3805U,0U},
  {0U,4114U,4586U,0U},
  {0U,4115U,3296U,0U},
  {0U,4116U,3205U,0U},
  {0U,4117U,1448U,0U},
  {0U,4118U,4119U,0U},
  {0U,4118U,4184U,0U},
  {0U,4119U,4118U,0U},
  {0U,4120U,2538U,0U},
  {0U,4121U,4952U,0U},
  {0U,4122U,1452U,0U},
  {0U,4123U,5101U,0U},
  {0U,4124U,4531U,0U},
  {0U,4125U,2871U,0U},
  {0U,4126U,4231U,0U},
  {0U,4127U,2757U,0U},
  {0U,4128U,3632U,0U},
  {0U,4129U,1459U,0U},
  {0U,4130U,2890U,0U},
  {0U,4131U,4158U,0U},
  {0U,4132U,4331U,0U},
  {0U,4133U,4019U,0U},
  {0U,4134U,1665U,0U},
  {0U,4135U,4136U,0U},
  {0U,4136U,4135U,0U},
  {0U,4137U,2912U,0U},
  {0U,4138U,2969U,0U},
  {0U,4139U,4798U,0U},
  {0U,4140U,4778U,0U},
  {0U,4141U,1472U,0U},
  {0U,4142U,1473U,0U},
  {0U,4143U,1474U,0U},
  {0U,4144U,3558U,0U},
  {0U,4145U,4709U,0U},
  {0U,4146U,1476U,0U},
  {0U,4147U,1478U,0U},
  {0U,4148U,3772U,0U},
  {0U,4149U,3081U,0U},
  {0U,4150U,4360U,0U},
  {0U,4151U,4088U,0U},
  {0U,4152U,3188U,0U},
  {0U,4153U,2393U,0U},
  {0U,4154U,4809U,0U},
  {0U,4155U,1483U,0U},
  {0U,4156U,3885U,0U},
  {0U,4157U,3938U,0U},
  {0U,4158U,1499U,0U},
  {0U,4158U,4131U,0U},
  {0U,4159U,3443U,0U},
  {0U,4160U,4271U,0U},
  {0U,4161U,1998U,0U},
  {0U,4162U,5174U,0U},
  {0U,4163U,1514U,0U},
  {0U,4164U,1518U,0U},
  {0U,4165U,5175U,0U},
  {0U,4166U,4111U,0U},
  {0U,4167U,3076U,0U},
  {0U,4168U,4935U,0U},
  {0U,4169U,1536U,0U},
  {0U,4170U,4991U,0U},
  {0U,4171U,4175U,0U},
  {0U,4172U,4822U,0U},
  {0U,4173U,5177U,0U},
  {0U,4174U,1537U,0U},
  {0U,4175U,4171U,0U},
  {0U,4176U,1538U,0U},
  {0U,4177U,1623U,0U},
  {0U,4178U,2526U,0U},
  {0U,4179U,1593U,0U},
  {0U,4180U,3135U,0U},
  {0U,4181U,3215U,0U},
  {0U,4182U,2559U,0U},
  {0U,4183U,4199U,0U},
  {0U,4184U,4118U,0U},
  {0U,4185U,2548U,0U},
  {0U,4186U,5178U,0U},
  {0U,4187U,3053U,0U},
  {0U,4188U,3051U,0U},
  {0U,4189U,1554U,0U},
  {0U,4190U,1549U,0U},
  {0U,4191U,1550U,0U},
  {0U,4192U,5159U,0U},
  {0U,4193U,4022U,0U},
  {0U,4194U,3040U,0U},
  {0U,4195U,3049U,0U},
  {0U,4196U,4792U,0U},
  {0U,4197U,4198U,0U},
  {0U,4198U,4197U,0U},
  {0U,4199U,4183U,0U},
  {0U,4200U,1678U,0U},
  {0U,4201U,3365U,0U},
  {0U,4202U,1557U,0U},
  {0U,4203U,2925U,0U},
  {0U,4204U,4897U,0U},
  {0U,4205U,4313U,0U},
  {0U,4206U,3813U,0U},
  {0U,4207U,4208U,0U},
  {0U,4208U,4207U,0U},
  {0U,4209U,4997U,0U},
  {0U,4210U,4861U,0U},
  {0U,4211U,3993U,0U},
  {0U,4212U,4406U,0U},
  {0U,4213U,1559U,0U},
  {0U,4214U,2325U,0U},
  {0U,4215U,1644U,0U},
  {0U,4216U,4255U,0U},
  {0U,4217U,5181U,0U},
  {0U,4218U,4051U,0U},
  {0U,4219U,4267U,0U},
  {0U,4220U,5182U,0U},
  {0U,4221U,4015U,0U},
  {0U,4222U,5184U,0U},
  {0U,4223U,4548U,0U},
  {0U,4224U,4791U,0U},
  {0U,4225U,5185U,0U},
  {0U,4226U,2751U,0U},
  {0U,4227U,1689U,0U},
  {0U,4228U,4785U,0U},
  {0U,4229U,5186U,0U},
  {0U,4230U,2427U,0U},
  {0U,4231U,4126U,0U},
  {0U,4232U,1640U,0U},
  {0U,4232U,5090U,0U},
  {0U,4233U,1566U,0U},
  {0U,4234U,4979U,0U},
  {0U,4235U,4889U,0U},
  {0U,4236U,1588U,0U},
  {0U,4237U,2887U,0U},
  {0U,4238U,5183U,0U},
  {0U,4239U,5088U,0U},
  {0U,4240U,4445U,0U},
  {0U,4241U,4886U,0U},
  {0U,4242U,4831U,0U},
  {0U,4243U,4522U,0U},
  {0U,4244U,4489U,0U},
  {0U,4245U,1572U,0U},
  {0U,4246U,1589U,0U},
  {0U,4247U,5356U,0U},
  {0U,4248U,5193U,0U},
  {0U,4249U,1573U,0U},
  {0U,4250U,5194U,0U},
  {0U,4251U,4252U,0U},
  {0U,4252U,4251U,0U},
  {0U,4253U,4061U,0U},
  {0U,4254U,4359U,0U},
  {0U,4255U,4216U,0U},
  {0U,4256U,1576U,0U},
  {0U,4257U,1577U,0U},
  {0U,4258U,4872U,0U},
  {0U,4259U,1578U,0U},
  {0U,4260U,5199U,0U},
  {0U,4261U,1666U,0U},
  {0U,4262U,1581U,0U},
  {0U,4263U,4436U,0U},
  {0U,4264U,4317U,0U},
  {0U,4265U,4438U,0U},
  {0U,4266U,4311U,0U},
  {0U,4267U,4219U,0U},
  {0U,4268U,5201U,0U},
  {0U,4269U,1582U,0U},
  {0U,4270U,3862U,0U},
  {0U,4271U,4160U,0U},
  {0U,4272U,5203U,0U},
  {0U,4273U,1585U,0U},
  {0U,4274U,3388U,0U},
  {0U,4275U,3910U,0U},
  {0U,4276U,1963U,0U},
  {0U,4276U,2640U,0U},
  {0U,4277U,5204U,0U},
  {0U,4278U,5150U,0U},
  {0U,4279U,1586U,0U},
  {0U,4280U,4902U,0U},
  {0U,4281U,2872U,0U},
  {0U,4282U,1592U,0U},
  {0U,4283U,5205U,0U},
  {0U,4284U,5206U,0U},
  {0U,4285U,4308U,0U},
  {0U,4286U,1595U,0U},
  {0U,4287U,3210U,0U},
  {0U,4288U,4289U,0U},
  {0U,4289U,4288U,0U},
  {0U,4290U,4300U,0U},
  {0U,4291U,1598U,0U},
  {0U,4292U,4616U,0U},
  {0U,4293U,5207U,0U},
  {0U,4294U,5208U,0U},
  {0U,4295U,1622U,0U},
  {0U,4296U,5211U,0U},
  {0U,4297U,2721U,0U},
  {0U,4298U,4435U,0U},
  {0U,4299U,4419U,0U},
  {0U,4300U,4290U,0U},
  {0U,4301U,5215U,0U},
  {0U,4302U,4632U,0U},
  {0U,4303U,3492U,0U},
  {0U,4304U,5218U,0U},
  {0U,4305U,5133U,0U},
  {0U,4306U,4376U,0U},
  {0U,4307U,5221U,0U},
  {0U,4308U,4285U,0U},
  {0U,4309U,3718U,0U},
  {0U,4310U,1611U,0U},
  {0U,4311U,4266U,0U},
  {0U,4312U,2027U,0U},
  {0U,4313U,4205U,0U},
  {0U,4314U,4413U,0U},
  {0U,4315U,4836U,0U},
  {0U,4316U,4857U,0U},
  {0U,4317U,4264U,0U},
  {0U,4317U,4838U,0U},
  {0U,4318U,4841U,0U},
  {0U,4319U,5192U,0U},
  {0U,4320U,4321U,0U},
  {0U,4321U,4320U,0U},
  {0U,4322U,4860U,0U},
  {0U,4323U,5222U,0U},
  {0U,4324U,5223U,0U},
  {0U,4325U,1614U,0U},
  {0U,4326U,1695U,0U},
  {0U,4327U,1616U,0U},
  {0U,4328U,1617U,0U},
  {0U,4329U,5224U,0U},
  {0U,4330U,5156U,0U},
  {0U,4331U,4132U,0U},
  {0U,4332U,4699U,0U},
  {0U,4333U,4378U,0U},
  {0U,4334U,4544U,0U},
  {0U,4335U,2244U,0U},
  {0U,4336U,4337U,0U},
  {0U,4337U,4336U,0U},
  {0U,4338U,3209U,0U},
  {0U,4339U,4340U,0U},
  {0U,4340U,4339U,0U},
  {0U,4341U,4342U,0U},
  {0U,4342U,4341U,0U},
  {0U,4343U,4856U,0U},
  {0U,4344U,4987U,0U},
  {0U,4344U,5165U,0U},
  {0U,4345U,4012U,0U},
  {0U,4346U,4512U,0U},
  {0U,4347U,1634U,0U},
  {0U,4348U,1624U,0U},
  {0U,4349U,1705U,0U},
  {0U,4350U,3810U,0U},
  {0U,4351U,4901U,0U},
  {0U,4352U,1625U,0U},
  {0U,4353U,4392U,0U},
  {0U,4354U,4486U,0U},
  {0U,4355U,4870U,0U},
  {0U,4356U,5147U,0U},
  {0U,4357U,4002U,0U},
  {0U,4358U,3965U,0U},
  {0U,4359U,4254U,0U},
  {0U,4360U,4150U,0U},
  {0U,4361U,4587U,0U},
  {0U,4362U,3588U,0U},
  {0U,4363U,4557U,0U},
  {0U,4364U,3220U,0U},
  {0U,4365U,4507U,0U},
  {0U,4366U,2497U,0U},
  {0U,4367U,5152U,0U},
  {0U,4368U,2874U,0U},
  {0U,4369U,3465U,0U},
  {0U,4370U,1629U,0U},
  {0U,4371U,4372U,0U},
  {0U,4372U,4371U,0U},
  {0U,4373U,2738U,0U},
  {0U,4374U,4564U,0U},
  {0U,4375U,1630U,0U},
  {0U,4376U,4306U,0U},
  {0U,4377U,4640U,0U},
  {0U,4378U,4333U,0U},
  {0U,4379U,4837U,0U},
  {0U,4380U,3135U,0U},
  {0U,4381U,4834U,0U},
  {0U,4382U,5236U,0U},
  {0U,4383U,4418U,0U},
  {0U,4384U,4385U,0U},
  {0U,4385U,4384U,0U},
  {0U,4386U,4066U,0U},
  {0U,4387U,4426U,0U},
  {0U,4388U,4389U,0U},
  {0U,4389U,4388U,0U},
  {0U,4390U,2604U,0U},
  {0U,4391U,4819U,0U},
  {0U,4392U,4353U,0U},
  {0U,4393U,4963U,0U},
  {0U,4394U,4395U,0U},
  {0U,4395U,4394U,0U},
  {0U,4396U,5240U,0U},
  {0U,4397U,4398U,0U},
  {0U,4398U,4397U,0U},
  {0U,4399U,2615U,0U},
  {0U,4400U,3399U,0U},
  {0U,4401U,2073U,0U},
  {0U,4402U,1724U,0U},
  {0U,4403U,5241U,0U},
  {0U,4404U,4875U,0U},
  {0U,4405U,4087U,0U},
  {0U,4406U,4212U,0U},
  {0U,4407U,1641U,0U},
  {0U,4408U,4409U,0U},
  {0U,4409U,4408U,0U},
  {0U,4410U,1643U,0U},
  {0U,4411U,4556U,0U},
  {0U,4412U,3986U,0U},
  {0U,4413U,4314U,0U},
  {0U,4414U,4835U,0U},
  {0U,4415U,5213U,0U},
  {0U,4416U,2881U,0U},
  {0U,4417U,5244U,0U},
  {0U,4418U,4383U,0U},
  {0U,4419U,4299U,0U},
  {0U,4420U,5200U,0U},
  {0U,4421U,1645U,0U},
  {0U,4422U,5195U,0U},
  {0U,4423U,4511U,0U},
  {0U,4424U,3546U,0U},
  {0U,4425U,3262U,0U},
  {0U,4426U,4387U,0U},
  {0U,4427U,4580U,0U},
  {0U,4428U,2913U,0U},
  {0U,4429U,5245U,0U},
  {0U,4430U,3984U,0U},
  {0U,4431U,1651U,0U},
  {0U,4432U,4433U,0U},
  {0U,4432U,4434U,0U},
  {0U,4433U,4432U,0U},
  {0U,4434U,4432U,0U},
  {0U,4435U,4298U,0U},
  {0U,4436U,4263U,0U},
  {0U,4437U,1658U,0U},
  {0U,4438U,4265U,0U},
  {0U,4439U,1653U,0U},
  {0U,4440U,4441U,0U},
  {0U,4441U,4440U,0U},
  {0U,4442U,5247U,0U},
  {0U,4443U,2667U,0U},
  {0U,4444U,5196U,0U},
  {0U,4445U,4240U,0U},
  {0U,4446U,5046U,0U},
  {0U,4447U,1659U,0U},
  {0U,4448U,1660U,0U},
  {0U,4449U,4638U,0U},
  {0U,4450U,1661U,0U},
  {0U,4451U,3469U,0U},
  {0U,4452U,3555U,0U},
  {0U,4453U,4030U,0U},
  {0U,4454U,5164U,0U},
  {0U,4455U,5250U,0U},
  {0U,4456U,5232U,0U},
  {0U,4457U,4711U,0U},
  {0U,4458U,4542U,0U},
  {0U,4459U,5251U,0U},
  {0U,4460U,1671U,0U},
  {0U,4461U,5252U,0U},
  {0U,4462U,5253U,0U},
  {0U,4463U,5254U,0U},
  {0U,4464U,3521U,0U},
  {0U,4465U,4965U,0U},
  {0U,4466U,1712U,0U},
  {0U,4467U,1680U,0U},
  {0U,4468U,4469U,0U},
  {0U,4469U,4468U,0U},
  {0U,4470U,4471U,0U},
  {0U,4471U,4470U,0U},
  {0U,4472U,5071U,0U},
  {0U,4473U,2336U,0U},
  {0U,4474U,3702U,0U},
  {0U,4475U,1684U,0U},
  {0U,4476U,5216U,0U},
  {0U,4477U,4505U,0U},
  {0U,4478U,4832U,0U},
  {0U,4479U,5119U,0U},
  {0U,4480U,3995U,0U},
  {0U,4481U,3203U,0U},
  {0U,4482U,1685U,0U},
  {0U,4483U,4485U,0U},
  {0U,4484U,3295U,0U},
  {0U,4485U,4483U,0U},
  {0U,4486U,4354U,0U},
  {0U,4487U,4646U,0U},
  {0U,4488U,5109U,0U},
  {0U,4489U,4244U,0U},
  {0U,4490U,4491U,0U},
  {0U,4491U,4490U,0U},
  {0U,4492U,1691U,0U},
  {0U,4493U,5233U,0U},
  {0U,4494U,4843U,0U},
  {0U,4495U,5256U,0U},
  {0U,4496U,5155U,0U},
  {0U,4497U,1714U,0U},
  {0U,4498U,3074U,0U},
  {0U,4499U,5008U,0U},
  {0U,4500U,5257U,0U},
  {0U,4501U,2908U,0U},
  {0U,4502U,4503U,0U},
  {0U,4503U,4502U,0U},
  {0U,4504U,4840U,0U},
  {0U,4505U,4477U,0U},
  {0U,4506U,3357U,0U},
  {0U,4507U,4365U,0U},
  {0U,4508U,4509U,0U},
  {0U,4509U,4508U,0U},
  {0U,4510U,1709U,0U},
  {0U,4511U,4423U,0U},
  {0U,4512U,4346U,0U},
  {0U,4513U,3474U,0U},
  {0U,4514U,4779U,0U},
  {0U,4515U,3852U,0U},
  {0U,4516U,4517U,0U},
  {0U,4517U,4516U,0U},
  {0U,4518U,4851U,0U},
  {0U,4519U,2825U,0U},
  {0U,4520U,4617U,0U},
  {0U,4521U,1761U,0U},
  {0U,4522U,4243U,0U},
  {0U,4523U,5259U,0U},
  {0U,4524U,1715U,0U},
  {0U,4525U,5260U,0U},
  {0U,4526U,3857U,0U},
  {0U,4527U,2911U,0U},
  {0U,4528U,1718U,0U},
  {0U,4529U,4788U,0U},
  {0U,4530U,5263U,0U},
  {0U,4531U,4124U,0U},
  {0U,4532U,4891U,0U},
  {0U,4533U,4812U,0U},
  {0U,4534U,3294U,0U},
  {0U,4535U,1726U,0U},
  {0U,4536U,4829U,0U},
  {0U,4537U,1729U,0U},
  {0U,4538U,4539U,0U},
  {0U,4539U,4538U,0U},
  {0U,4540U,1733U,0U},
  {0U,4541U,5268U,0U},
  {0U,4542U,4458U,0U},
  {0U,4543U,1739U,0U},
  {0U,4543U,5269U,0U},
  {0U,4544U,4334U,0U},
  {0U,4545U,3297U,0U},
  {0U,4545U,3545U,0U},
  {0U,4546U,4859U,0U},
  {0U,4547U,5262U,0U},
  {0U,4548U,4223U,0U},
  {0U,4549U,1748U,0U},
  {0U,4550U,1749U,0U},
  {0U,4551U,5082U,0U},
  {0U,4552U,1752U,0U},
  {0U,4553U,1753U,0U},
  {0U,4554U,4851U,0U},
  {0U,4555U,5214U,0U},
  {0U,4556U,4411U,0U},
  {0U,4557U,4363U,0U},
  {0U,4558U,5161U,0U},
  {0U,4559U,4560U,0U},
  {0U,4560U,4559U,0U},
  {0U,4561U,1759U,0U},
  {0U,4562U,4971U,0U},
  {0U,4563U,5272U,0U},
  {0U,4564U,4374U,0U},
  {0U,4565U,3721U,0U},
  {0U,4566U,4849U,0U},
  {0U,4567U,4750U,0U},
  {0U,4568U,2129U,0U},
  {0U,4569U,4570U,0U},
  {0U,4570U,4569U,0U},
  {0U,4571U,2860U,0U},
  {0U,4572U,2323U,0U},
  {0U,4573U,3398U,0U},
  {0U,4574U,4575U,0U},
  {0U,4575U,4574U,0U},
  {0U,4576U,1769U,0U},
  {0U,4577U,1775U,0U},
  {0U,4578U,4970U,0U},
  {0U,4579U,4667U,0U},
  {0U,4580U,4427U,0U},
  {0U,4581U,4582U,0U},
  {0U,4582U,4581U,0U},
  {0U,4583U,1784U,0U},
  {0U,4584U,4975U,0U},
  {0U,4585U,1786U,0U},
  {0U,4586U,4114U,0U},
  {0U,4587U,4361U,0U},
  {0U,4588U,2657U,0U},
  {0U,4589U,2085U,0U},
  {0U,4590U,1802U,0U},
  {0U,4591U,1804U,0U},
  {0U,4592U,71U,0U},
  {0U,4593U,5013U,0U},
  {0U,4594U,4595U,0U},
  {0U,4595U,4594U,0U},
  {0U,4596U,1851U,0U},
  {0U,4597U,1877U,0U},
  {0U,4598U,2112U,0U},
  {0U,4599U,5091U,0U},
  {0U,4600U,1891U,0U},
  {0U,4601U,4879U,0U},
  {0U,4602U,2816U,0U},
  {0U,4603U,2473U,0U},
  {0U,4604U,5118U,0U},
  {0U,4605U,2753U,0U},
  {0U,4606U,1919U,0U},
  {0U,4607U,4806U,0U},
  {0U,4608U,1914U,0U},
  {0U,4609U,3823U,0U},
  {0U,4610U,2990U,0U},
  {0U,4611U,1916U,0U},
  {0U,4612U,5080U,0U},
  {0U,4613U,4723U,0U},
  {0U,4614U,4615U,0U},
  {0U,4615U,4614U,0U},
  {0U,4616U,4292U,0U},
  {0U,4617U,4520U,0U},
  {0U,4618U,1940U,0U},
  {0U,4619U,1943U,0U},
  {0U,4620U,5280U,0U},
  {0U,4621U,5169U,0U},
  {0U,4622U,1993U,0U},
  {0U,4623U,3571U,0U},
  {0U,4624U,2502U,0U},
  {0U,4625U,2590U,0U},
  {0U,4626U,2891U,0U},
  {0U,4627U,5281U,0U},
  {0U,4628U,5282U,0U},
  {0U,4629U,4060U,0U},
  {0U,4630U,1980U,0U},
  {0U,4631U,71U,0U},
  {0U,4632U,4302U,0U},
  {0U,4633U,4955U,0U},
  {0U,4634U,2460U,0U},
  {0U,4635U,1983U,0U},
  {0U,4636U,1984U,0U},
  {0U,4637U,5284U,0U},
  {0U,4638U,4449U,0U},
  {0U,4639U,2637U,0U},
  {0U,4640U,4377U,0U},
  {0U,4641U,2000U,0U},
  {0U,4642U,2842U,0U},
  {0U,4643U,3789U,0U},
  {0U,4644U,2004U,0U},
  {0U,4645U,2373U,0U},
  {0U,4646U,4487U,0U},
  {0U,4647U,2029U,0U},
  {0U,4648U,3801U,0U},
  {0U,4649U,3834U,0U},
  {0U,4650U,5108U,0U},
  {0U,4651U,5286U,0U},
  {0U,4652U,5113U,0U},
  {0U,4653U,2040U,0U},
  {0U,4654U,2041U,0U},
  {0U,4655U,2082U,0U},
  {0U,4656U,5287U,0U},
  {0U,4657U,2044U,0U},
  {0U,4658U,2047U,0U},
  {0U,4659U,2051U,0U},
  {0U,4660U,4661U,0U},
  {0U,4661U,4660U,0U},
  {0U,4662U,2052U,0U},
  {0U,4663U,2054U,0U},
  {0U,4664U,5288U,0U},
  {0U,4665U,4049U,0U},
  {0U,4666U,2057U,0U},
  {0U,4667U,4579U,0U},
  {0U,4668U,2994U,0U},
  {0U,4669U,3103U,0U},
  {0U,4670U,2065U,0U},
  {0U,4671U,3515U,0U},
  {0U,4672U,4673U,0U},
  {0U,4673U,4672U,0U},
  {0U,4674U,2068U,0U},
  {0U,4675U,2074U,0U},
  {0U,4676U,3865U,0U},
  {0U,4677U,2079U,0U},
  {0U,4678U,2437U,0U},
  {0U,4679U,4680U,0U},
  {0U,4680U,4679U,0U},
  {0U,4681U,5074U,0U},
  {0U,4682U,5289U,0U},
  {0U,4683U,2089U,0U},
  {0U,4684U,2096U,0U},
  {0U,4685U,2100U,0U},
  {0U,4686U,2106U,0U},
  {0U,4687U,71U,0U},
  {0U,4688U,2114U,0U},
  {0U,4689U,3674U,0U},
  {0U,4690U,2123U,0U},
  {0U,4691U,3786U,0U},
  {0U,4692U,3138U,0U},
  {0U,4693U,2997U,0U},
  {0U,4694U,5290U,0U},
  {0U,4695U,2149U,0U},
  {0U,4696U,4700U,0U},
  {0U,4697U,2174U,0U},
  {0U,4698U,2181U,0U},
  {0U,4699U,4332U,0U},
  {0U,4700U,4696U,0U},
  {0U,4701U,2205U,0U},
  {0U,4702U,2206U,0U},
  {0U,4703U,5064U,0U},
  {0U,4704U,3915U,0U},
  {0U,4705U,3856U,0U},
  {0U,4706U,2231U,0U},
  {0U,4707U,2232U,0U},
  {0U,4708U,4104U,0U},
  {0U,4709U,4145U,0U},
  {0U,4710U,2240U,0U},
  {0U,4711U,4457U,0U},
  {0U,4712U,5095U,0U},
  {0U,4713U,4785U,0U},
  {0U,4714U,3839U,0U},
  {0U,4715U,4846U,0U},
  {0U,4716U,5293U,0U},
  {0U,4717U,2293U,0U},
  {0U,4718U,2398U,0U},
  {0U,4719U,2928U,0U},
  {0U,4720U,2307U,0U},
  {0U,4721U,4722U,0U},
  {0U,4722U,4721U,0U},
  {0U,4723U,4613U,0U},
  {0U,4724U,5294U,0U},
  {0U,4725U,2315U,0U},
  {0U,4726U,3887U,0U},
  {0U,4727U,4728U,0U},
  {0U,4728U,4727U,0U},
  {0U,4729U,2690U,0U},
  {0U,4730U,5295U,0U},
  {0U,4731U,3116U,0U},
  {0U,4732U,4752U,0U},
  {0U,4733U,3935U,0U},
  {0U,4734U,4735U,0U},
  {0U,4735U,4734U,0U},
  {0U,4736U,5157U,0U},
  {0U,4737U,5296U,0U},
  {0U,4738U,4744U,0U},
  {0U,4739U,4740U,0U},
  {0U,4740U,4739U,0U},
  {0U,4741U,5297U,0U},
  {0U,4742U,3007U,0U},
  {0U,4743U,2347U,0U},
  {0U,4744U,4738U,0U},
  {0U,4745U,2351U,0U},
  {0U,4746U,3431U,0U},
  {0U,4747U,2353U,0U},
  {0U,4748U,5072U,0U},
  {0U,4749U,2357U,0U},
  {0U,4750U,4567U,0U},
  {0U,4751U,5291U,0U},
  {0U,4752U,4732U,0U},
  {0U,4753U,5298U,0U},
  {0U,4754U,2496U,0U},
  {0U,4755U,5299U,0U},
  {0U,4756U,4757U,0U},
  {0U,4757U,4756U,0U},
  {0U,4758U,4922U,0U},
  {0U,4759U,705U,0U},
  {0U,4760U,4932U,0U},
  {0U,4761U,5300U,0U},
  {0U,4762U,5301U,0U},
  {0U,4763U,3873U,0U},
  {0U,4764U,2420U,0U},
  {0U,4765U,2423U,0U},
  {0U,4766U,2425U,0U},
  {0U,4767U,2425U,0U},
  {0U,4768U,2433U,0U},
  {0U,4769U,2449U,0U},
  {0U,4770U,4974U,0U},
  {0U,4771U,2452U,0U},
  {0U,4772U,5302U,0U},
  {0U,4773U,3930U,0U},
  {0U,4774U,2465U,0U},
  {0U,4775U,5050U,0U},
  {0U,4776U,2468U,0U},
  {0U,4777U,2984U,0U},
  {0U,4778U,4140U,0U},
  {0U,4779U,4514U,0U},
  {0U,4780U,2480U,0U},
  {0U,4781U,3482U,0U},
  {0U,4782U,71U,0U},
  {0U,4783U,2501U,0U},
  {0U,4784U,5303U,0U},
  {0U,4785U,4228U,0U},
  {0U,4785U,4713U,0U},
  {0U,4786U,4807U,0U},
  {0U,4787U,2511U,0U},
  {0U,4788U,4529U,0U},
  {0U,4789U,3131U,0U},
  {0U,4790U,2522U,0U},
  {0U,4791U,4224U,0U},
  {0U,4792U,4196U,0U},
  {0U,4793U,2534U,0U},
  {0U,4794U,2551U,0U},
  {0U,4795U,3052U,0U},
  {0U,4796U,4110U,0U},
  {0U,4797U,4057U,0U},
  {0U,4798U,4139U,0U},
  {0U,4799U,4024U,0U},
  {0U,4800U,3493U,0U},
  {0U,4801U,2575U,0U},
  {0U,4802U,2576U,0U},
  {0U,4803U,5307U,0U},
  {0U,4804U,3672U,0U},
  {0U,4805U,3830U,0U},
  {0U,4806U,4607U,0U},
  {0U,4807U,4786U,0U},
  {0U,4808U,3964U,0U},
  {0U,4809U,4154U,0U},
  {0U,4810U,2599U,0U},
  {0U,4811U,5309U,0U},
  {0U,4812U,4533U,0U},
  {0U,4813U,2605U,0U},
  {0U,4814U,5053U,0U},
  {0U,4815U,3868U,0U},
  {0U,4816U,3153U,0U},
  {0U,4817U,3229U,0U},
  {0U,4818U,4008U,0U},
  {0U,4819U,4391U,0U},
  {0U,4820U,3102U,0U},
  {0U,4821U,2892U,0U},
  {0U,4822U,4172U,0U},
  {0U,4823U,4874U,0U},
  {0U,4824U,2923U,0U},
  {0U,4825U,5314U,0U},
  {0U,4826U,2724U,0U},
  {0U,4827U,2683U,0U},
  {0U,4828U,2686U,0U},
  {0U,4829U,4536U,0U},
  {0U,4830U,2707U,0U},
  {0U,4831U,4242U,0U},
  {0U,4831U,4845U,0U},
  {0U,4832U,4478U,0U},
  {0U,4833U,4834U,0U},
  {0U,4833U,4837U,0U},
  {0U,4834U,4381U,0U},
  {0U,4834U,4833U,0U},
  {0U,4835U,4414U,0U},
  {0U,4836U,4315U,0U},
  {0U,4837U,4379U,0U},
  {0U,4837U,4833U,0U},
  {0U,4838U,4317U,0U},
  {0U,4839U,2723U,0U},
  {0U,4840U,4504U,0U},
  {0U,4841U,4318U,0U},
  {0U,4842U,2720U,0U},
  {0U,4843U,4494U,0U},
  {0U,4844U,4003U,0U},
  {0U,4845U,4831U,0U},
  {0U,4846U,4715U,0U},
  {0U,4847U,2761U,0U},
  {0U,4848U,2763U,0U},
  {0U,4849U,4566U,0U},
  {0U,4850U,4945U,0U},
  {0U,4851U,4518U,0U},
  {0U,4851U,4554U,0U},
  {0U,4852U,5319U,0U},
  {0U,4853U,2792U,0U},
  {0U,4854U,2794U,0U},
  {0U,4855U,5114U,0U},
  {0U,4856U,4343U,0U},
  {0U,4857U,4316U,0U},
  {0U,4858U,4968U,0U},
  {0U,4859U,4546U,0U},
  {0U,4860U,3481U,0U},
  {0U,4860U,4322U,0U},
  {0U,4861U,4210U,0U},
  {0U,4862U,5320U,0U},
  {0U,4863U,2814U,0U},
  {0U,4864U,5092U,0U},
  {0U,4865U,2822U,0U},
  {0U,4866U,2834U,0U},
  {0U,4867U,2849U,0U},
  {0U,4868U,4953U,0U},
  {0U,4869U,3627U,0U},
  {0U,4870U,4355U,0U},
  {0U,4871U,5321U,0U},
  {0U,4872U,4258U,0U},
  {0U,4873U,2868U,0U},
  {0U,4874U,4823U,0U},
  {0U,4875U,4404U,0U},
  {0U,4876U,4877U,0U},
  {0U,4877U,4876U,0U},
  {0U,4878U,2877U,0U},
  {0U,4879U,4601U,0U},
  {0U,4880U,5323U,0U},
  {0U,4881U,4887U,0U},
  {0U,4882U,2888U,0U},
  {0U,4883U,5273U,0U},
  {0U,4884U,5172U,0U},
  {0U,4885U,2895U,0U},
  {0U,4886U,4241U,0U},
  {0U,4887U,4881U,0U},
  {0U,4888U,2897U,0U},
  {0U,4889U,4235U,0U},
  {0U,4890U,2898U,0U},
  {0U,4891U,4532U,0U},
  {0U,4892U,2901U,0U},
  {0U,4893U,2906U,0U},
  {0U,4894U,4895U,0U},
  {0U,4895U,4894U,0U},
  {0U,4896U,3260U,0U},
  {0U,4897U,4204U,0U},
  {0U,4898U,4954U,0U},
  {0U,4899U,4956U,0U},
  {0U,4900U,5143U,0U},
  {0U,4901U,4351U,0U},
  {0U,4902U,4280U,0U},
  {0U,4903U,3349U,0U},
  {0U,4904U,4047U,0U},
  {0U,4905U,3721U,0U},
  {0U,4906U,5001U,0U},
  {0U,4907U,2943U,0U},
  {0U,4908U,2945U,0U},
  {0U,4909U,2947U,0U},
  {0U,4910U,2955U,0U},
  {0U,4911U,2961U,0U},
  {0U,4912U,4994U,0U},
  {0U,4913U,2989U,0U},
  {0U,4914U,5079U,0U},
  {0U,4915U,2980U,0U},
  {0U,4916U,2982U,0U},
  {0U,4917U,2983U,0U},
  {0U,4918U,4919U,0U},
  {0U,4919U,4918U,0U},
  {0U,4920U,2985U,0U},
  {0U,4921U,2996U,0U},
  {0U,4922U,4758U,0U},
  {0U,4923U,3000U,0U},
  {0U,4924U,5328U,0U},
  {0U,4925U,5140U,0U},
  {0U,4926U,5329U,0U},
  {0U,4927U,3010U,0U},
  {0U,4928U,3013U,0U},
  {0U,4929U,4978U,0U},
  {0U,4930U,4931U,0U},
  {0U,4931U,4930U,0U},
  {0U,4932U,4760U,0U},
  {0U,4933U,5331U,0U},
  {0U,4934U,5055U,0U},
  {0U,4935U,4168U,0U},
  {0U,4936U,4937U,0U},
  {0U,4937U,4936U,0U},
  {0U,4938U,3070U,0U},
  {0U,4939U,3073U,0U},
  {0U,4940U,4110U,0U},
  {0U,4941U,3088U,0U},
  {0U,4942U,3701U,0U},
  {0U,4943U,3094U,0U},
  {0U,4944U,3947U,0U},
  {0U,4945U,4850U,0U},
  {0U,4946U,5049U,0U},
  {0U,4947U,5317U,0U},
  {0U,4948U,5332U,0U},
  {0U,4949U,4950U,0U},
  {0U,4950U,4949U,0U},
  {0U,4951U,3125U,0U},
  {0U,4952U,4121U,0U},
  {0U,4953U,4868U,0U},
  {0U,4954U,4898U,0U},
  {0U,4955U,4633U,0U},
  {0U,4956U,4899U,0U},
  {0U,4957U,3154U,0U},
  {0U,4958U,5121U,0U},
  {0U,4959U,3180U,0U},
  {0U,4960U,3184U,0U},
  {0U,4961U,3688U,0U},
  {0U,4962U,5096U,0U},
  {0U,4963U,4393U,0U},
  {0U,4964U,5325U,0U},
  {0U,4965U,4465U,0U},
  {0U,4966U,3230U,0U},
  {0U,4967U,3992U,0U},
  {0U,4968U,4858U,0U},
  {0U,4969U,3235U,0U},
  {0U,4970U,4578U,0U},
  {0U,4971U,4562U,0U},
  {0U,4972U,4973U,0U},
  {0U,4973U,4972U,0U},
  {0U,4974U,4770U,0U},
  {0U,4975U,4584U,0U},
  {0U,4976U,5028U,0U},
  {0U,4977U,3242U,0U},
  {0U,4978U,4929U,0U},
  {0U,4979U,4234U,0U},
  {0U,4980U,5285U,0U},
  {0U,4981U,3336U,0U},
  {0U,4982U,5146U,0U},
  {0U,4983U,4009U,0U},
  {0U,4984U,3251U,0U},
  {0U,4985U,3255U,0U},
  {0U,4986U,5179U,0U},
  {0U,4987U,4344U,0U},
  {0U,4988U,3291U,0U},
  {0U,4989U,3997U,0U},
  {0U,4990U,5336U,0U},
  {0U,4991U,4170U,0U},
  {0U,4992U,3331U,0U},
  {0U,4993U,3341U,0U},
  {0U,4994U,4912U,0U},
  {0U,4995U,5337U,0U},
  {0U,4996U,5318U,0U},
  {0U,4997U,4209U,0U},
  {0U,4998U,5137U,0U},
  {0U,4999U,3991U,0U},
  {0U,5000U,3346U,0U},
  {0U,5001U,4906U,0U},
  {0U,5002U,3347U,0U},
  {0U,5003U,3348U,0U},
  {0U,5004U,5338U,0U},
  {0U,5005U,3520U,0U},
  {0U,5006U,5007U,0U},
  {0U,5006U,5189U,0U},
  {0U,5007U,5006U,0U},
  {0U,5008U,4499U,0U},
  {0U,5009U,3361U,0U},
  {0U,5010U,3368U,0U},
  {0U,5011U,5339U,0U},
  {0U,5012U,3374U,0U},
  {0U,5013U,4593U,0U},
  {0U,5014U,3383U,0U},
  {0U,5015U,3390U,0U},
  {0U,5016U,3393U,0U},
  {0U,5017U,5197U,0U},
  {0U,5018U,5341U,0U},
  {0U,5019U,3396U,0U},
  {0U,5020U,4070U,0U},
  {0U,5021U,5126U,0U},
  {0U,5022U,3407U,0U},
  {0U,5023U,3409U,0U},
  {0U,5024U,71U,0U},
  {0U,5025U,3414U,0U},
  {0U,5026U,5032U,0U},
  {0U,5027U,5342U,0U},
  {0U,5028U,4976U,0U},
  {0U,5029U,3443U,0U},
  {0U,5030U,3446U,0U},
  {0U,5031U,3694U,0U},
  {0U,5032U,5026U,0U},
  {0U,5033U,3455U,0U},
  {0U,5034U,3460U,0U},
  {0U,5035U,5227U,0U},
  {0U,5036U,5037U,0U},
  {0U,5037U,5036U,0U},
  {0U,5038U,3484U,0U},
  {0U,5039U,4095U,0U},
  {0U,5040U,5271U,0U},
  {0U,5041U,5180U,0U},
  {0U,5042U,3496U,0U},
  {0U,5043U,3506U,0U},
  {0U,5044U,4074U,0U},
  {0U,5045U,3559U,0U},
  {0U,5046U,4446U,0U},
  {0U,5047U,3527U,0U},
  {0U,5048U,3545U,0U},
  {0U,5049U,4946U,0U},
  {0U,5050U,4775U,0U},
  {0U,5051U,5052U,0U},
  {0U,5052U,5051U,0U},
  {0U,5053U,4814U,0U},
  {0U,5054U,3557U,0U},
  {0U,5055U,4934U,0U},
  {0U,5056U,5057U,0U},
  {0U,5057U,5056U,0U},
  {0U,5058U,3564U,0U},
  {0U,5059U,3572U,0U},
  {0U,5060U,3582U,0U},
  {0U,5061U,3584U,0U},
  {0U,5062U,704U,0U},
  {0U,5063U,3600U,0U},
  {0U,5064U,4703U,0U},
  {0U,5065U,5345U,0U},
  {0U,5066U,5103U,0U},
  {0U,5067U,5348U,0U},
  {0U,5068U,3614U,0U},
  {0U,5069U,3663U,0U},
  {0U,5070U,3666U,0U},
  {0U,5071U,4472U,0U},
  {0U,5072U,4748U,0U},
  {0U,5073U,3692U,0U},
  {0U,5074U,4681U,0U},
  {0U,5075U,3696U,0U},
  {0U,5076U,3822U,0U},
  {0U,5077U,5349U,0U},
  {0U,5078U,3712U,0U},
  {0U,5079U,4914U,0U},
  {0U,5080U,4612U,0U},
  {0U,5081U,3902U,0U},
  {0U,5082U,4551U,0U},
  {0U,5083U,3733U,0U},
  {0U,5084U,3734U,0U},
  {0U,5085U,3742U,0U},
  {0U,5086U,5087U,0U},
  {0U,5087U,5086U,0U},
  {0U,5088U,4239U,0U},
  {0U,5089U,5160U,0U},
  {0U,5090U,4232U,0U},
  {0U,5091U,4599U,0U},
  {0U,5092U,4864U,0U},
  {0U,5093U,3987U,0U},
  {0U,5094U,3755U,0U},
  {0U,5095U,4712U,0U},
  {0U,5096U,4962U,0U},
  {0U,5097U,4055U,0U},
  {0U,5098U,3757U,0U},
  {0U,5099U,3764U,0U},
  {0U,5100U,3769U,0U},
  {0U,5101U,4123U,0U},
  {0U,5102U,3782U,0U},
  {0U,5103U,5066U,0U},
  {0U,5104U,3812U,0U},
  {0U,5105U,3819U,0U},
  {0U,5106U,5351U,0U},
  {0U,5107U,3831U,0U},
  {0U,5108U,4650U,0U},
  {0U,5109U,4488U,0U},
  {0U,5110U,3840U,0U},
  {0U,5111U,3866U,0U},
  {0U,5112U,3945U,0U},
  {0U,5113U,4652U,0U},
  {0U,5114U,4855U,0U},
  {0U,5115U,3879U,0U},
  {0U,5116U,3883U,0U},
  {0U,5117U,71U,0U},
  {0U,5118U,4604U,0U},
  {0U,5119U,4479U,0U},
  {0U,5120U,3905U,0U},
  {0U,5121U,4958U,0U},
  {0U,5122U,5123U,0U},
  {0U,5123U,5122U,0U},
  {0U,5124U,3926U,0U},
  {0U,5125U,3936U,0U},
  {0U,5126U,5021U,0U},
  {0U,5127U,3941U,0U},
  {0U,5128U,3943U,0U},
  {0U,5129U,5352U,0U},
  {0U,5130U,5279U,0U},
  {0U,5131U,3948U,0U},
  {0U,5132U,3974U,0U},
  {0U,5133U,4305U,0U},
  {0U,5134U,5144U,0U},
  {0U,5135U,3994U,0U},
  {0U,5136U,5145U,0U},
  {0U,5137U,4998U,0U},
  {0U,5138U,5324U,0U},
  {0U,5139U,5239U,0U},
  {0U,5140U,4925U,0U},
  {0U,5141U,5217U,0U},
  {0U,5141U,5235U,0U},
  {0U,5142U,5217U,0U},
  {0U,5143U,4900U,0U},
  {0U,5144U,5134U,0U},
  {0U,5145U,5136U,0U},
  {0U,5146U,4982U,0U},
  {0U,5147U,4356U,0U},
  {0U,5148U,4011U,0U},
  {0U,5149U,4044U,0U},
  {0U,5150U,4278U,0U},
  {0U,5151U,4059U,0U},
  {0U,5152U,4367U,0U},
  {0U,5153U,5326U,0U},
  {0U,5154U,5249U,0U},
  {0U,5155U,4496U,0U},
  {0U,5156U,4330U,0U},
  {0U,5157U,4736U,0U},
  {0U,5158U,4043U,0U},
  {0U,5159U,4192U,0U},
  {0U,5160U,5089U,0U},
  {0U,5161U,4558U,0U},
  {0U,5162U,4067U,0U},
  {0U,5163U,5353U,0U},
  {0U,5164U,4454U,0U},
  {0U,5165U,4344U,0U},
  {0U,5166U,5167U,0U},
  {0U,5167U,5166U,0U},
  {0U,5168U,4105U,0U},
  {0U,5169U,4621U,0U},
  {0U,5170U,5226U,0U},
  {0U,5171U,5335U,0U},
  {0U,5172U,4884U,0U},
  {0U,5173U,5322U,0U},
  {0U,5174U,4162U,0U},
  {0U,5175U,4165U,0U},
  {0U,5176U,5202U,0U},
  {0U,5177U,4173U,0U},
  {0U,5178U,4186U,0U},
  {0U,5179U,4986U,0U},
  {0U,5180U,5041U,0U},
  {0U,5181U,4217U,0U},
  {0U,5182U,4220U,0U},
  {0U,5183U,4238U,0U},
  {0U,5184U,4222U,0U},
  {0U,5185U,4225U,0U},
  {0U,5186U,4229U,0U},
  {0U,5187U,5311U,0U},
  {0U,5188U,5355U,0U},
  {0U,5189U,5006U,0U},
  {0U,5190U,5327U,0U},
  {0U,5191U,5246U,0U},
  {0U,5192U,4319U,0U},
  {0U,5193U,4248U,0U},
  {0U,5194U,4250U,0U},
  {0U,5195U,4422U,0U},
  {0U,5196U,4444U,0U},
  {0U,5197U,5017U,0U},
  {0U,5198U,5316U,0U},
  {0U,5199U,4260U,0U},
  {0U,5200U,4420U,0U},
  {0U,5201U,4268U,0U},
  {0U,5202U,5176U,0U},
  {0U,5203U,4272U,0U},
  {0U,5204U,4277U,0U},
  {0U,5205U,4283U,0U},
  {0U,5206U,4284U,0U},
  {0U,5207U,4293U,0U},
  {0U,5208U,4294U,0U},
  {0U,5209U,5210U,0U},
  {0U,5210U,5209U,0U},
  {0U,5211U,4296U,0U},
  {0U,5212U,5248U,0U},
  {0U,5213U,4415U,0U},
  {0U,5214U,4555U,0U},
  {0U,5215U,4301U,0U},
  {0U,5216U,4476U,0U},
  {0U,5217U,5141U,0U},
  {0U,5217U,5142U,0U},
  {0U,5218U,4304U,0U},
  {0U,5219U,5225U,0U},
  {0U,5220U,5306U,0U},
  {0U,5221U,4307U,0U},
  {0U,5222U,4323U,0U},
  {0U,5223U,4324U,0U},
  {0U,5224U,4329U,0U},
  {0U,5225U,5219U,0U},
  {0U,5226U,5170U,0U},
  {0U,5227U,5035U,0U},
  {0U,5228U,5354U,0U},
  {0U,5229U,5243U,0U},
  {0U,5230U,5283U,0U},
  {0U,5231U,5330U,0U},
  {0U,5232U,4456U,0U},
  {0U,5233U,4493U,0U},
  {0U,5234U,3523U,0U},
  {0U,5235U,5141U,0U},
  {0U,5236U,4382U,0U},
  {0U,5237U,5238U,0U},
  {0U,5238U,5237U,0U},
  {0U,5239U,5139U,0U},
  {0U,5240U,4396U,0U},
  {0U,5241U,4403U,0U},
  {0U,5242U,5312U,0U},
  {0U,5243U,5229U,0U},
  {0U,5244U,4417U,0U},
  {0U,5245U,4429U,0U},
  {0U,5246U,5191U,0U},
  {0U,5247U,4442U,0U},
  {0U,5248U,5212U,0U},
  {0U,5249U,5154U,0U},
  {0U,5250U,4455U,0U},
  {0U,5251U,4459U,0U},
  {0U,5252U,4461U,0U},
  {0U,5253U,4462U,0U},
  {0U,5254U,4463U,0U},
  {0U,5255U,5362U,0U},
  {0U,5256U,4495U,0U},
  {0U,5257U,4500U,0U},
  {0U,5258U,5278U,0U},
  {0U,5259U,4523U,0U},
  {0U,5260U,4525U,0U},
  {0U,5261U,5350U,0U},
  {0U,5262U,4547U,0U},
  {0U,5263U,4530U,0U},
  {0U,5264U,5344U,0U},
  {0U,5265U,5266U,0U},
  {0U,5266U,5265U,0U},
  {0U,5267U,5310U,0U},
  {0U,5268U,4541U,0U},
  {0U,5269U,4543U,0U},
  {0U,5270U,5315U,0U},
  {0U,5271U,5040U,0U},
  {0U,5272U,4563U,0U},
  {0U,5273U,4883U,0U},
  {0U,5274U,5275U,0U},
  {0U,5275U,5274U,0U},
  {0U,5276U,5277U,0U},
  {0U,5277U,5276U,0U},
  {0U,5278U,5258U,0U},
  {0U,5279U,5130U,0U},
  {0U,5280U,4620U,0U},
  {0U,5281U,4627U,0U},
  {0U,5282U,4628U,0U},
  {0U,5283U,5230U,0U},
  {0U,5284U,4637U,0U},
  {0U,5285U,4980U,0U},
  {0U,5286U,4651U,0U},
  {0U,5287U,4656U,0U},
  {0U,5288U,4664U,0U},
  {0U,5289U,4682U,0U},
  {0U,5290U,4694U,0U},
  {0U,5291U,4751U,0U},
  {0U,5292U,5333U,0U},
  {0U,5293U,4716U,0U},
  {0U,5294U,4724U,0U},
  {0U,5295U,4730U,0U},
  {0U,5296U,4737U,0U},
  {0U,5297U,4741U,0U},
  {0U,5298U,4753U,0U},
  {0U,5299U,4755U,0U},
  {0U,5300U,4761U,0U},
  {0U,5301U,4762U,0U},
  {0U,5302U,4772U,0U},
  {0U,5303U,4784U,0U},
  {0U,5304U,1608U,0U},
  {0U,5305U,5340U,0U},
  {0U,5306U,5220U,0U},
  {0U,5307U,4803U,0U},
  {0U,5308U,5365U,0U},
  {0U,5309U,4811U,0U},
  {0U,5310U,5267U,0U},
  {0U,5311U,5187U,0U},
  {0U,5312U,5242U,0U},
  {0U,5313U,5357U,0U},
  {0U,5314U,4825U,0U},
  {0U,5315U,5270U,0U},
  {0U,5316U,5198U,0U},
  {0U,5317U,4947U,0U},
  {0U,5318U,4996U,0U},
  {0U,5319U,4852U,0U},
  {0U,5320U,4862U,0U},
  {0U,5321U,4871U,0U},
  {0U,5322U,5173U,0U},
  {0U,5323U,4880U,0U},
  {0U,5324U,5138U,0U},
  {0U,5325U,4964U,0U},
  {0U,5326U,5153U,0U},
  {0U,5327U,5190U,0U},
  {0U,5328U,4924U,0U},
  {0U,5329U,4926U,0U},
  {0U,5330U,5231U,0U},
  {0U,5331U,4933U,0U},
  {0U,5332U,4948U,0U},
  {0U,5333U,5292U,0U},
  {0U,5334U,5368U,0U},
  {0U,5335U,5171U,0U},
  {0U,5336U,4990U,0U},
  {0U,5337U,4995U,0U},
  {0U,5338U,5004U,0U},
  {0U,5339U,5011U,0U},
  {0U,5340U,5305U,0U},
  {0U,5341U,5018U,0U},
  {0U,5342U,5027U,0U},
  {0U,5343U,5369U,0U},
  {0U,5344U,5264U,0U},
  {0U,5345U,5065U,0U},
  {0U,5346U,5347U,0U},
  {0U,5347U,5346U,0U},
  {0U,5348U,5067U,0U},
  {0U,5349U,5077U,0U},
  {0U,5350U,5261U,0U},
  {0U,5351U,5106U,0U},
  {0U,5352U,5129U,0U},
  {0U,5353U,5163U,0U},
  {0U,5354U,5228U,0U},
  {0U,5355U,5188U,0U},
  {0U,5356U,4247U,0U},
  {0U,5357U,5313U,0U},
  {0U,5358U,5359U,0U},
  {0U,5359U,5358U,0U},
  {0U,5360U,5361U,0U},
  {0U,5361U,5360U,0U},
  {0U,5362U,5255U,0U},
  {0U,5363U,5370U,0U},
  {0U,5364U,71U,0U},
  {0U,5365U,5308U,0U},
  {0U,5366U,5367U,0U},
  {0U,5367U,5366U,0U},
  {0U,5368U,5334U,0U},
  {0U,5369U,5343U,0U},
  {0U,5370U,5363U,0U},
  {0U,5371U,71U,0U},
};
#endif

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

struct PaperVertex { uint32_t pointer;unsigned degree,capacity,padding; };
struct PaperBlock { uint32_t previous;unsigned count,neighbors[62]; };
PAPER_STORAGE(struct PaperVertex,paper_vertices,PAPER_VERTICES);
static struct PaperVertex vertex_read(unsigned u) {
  struct PaperVertex v __attribute__((aligned(8)));mram_read(&paper_vertices[u],&v,16);return v;
}
static void vertex_write(unsigned u,struct PaperVertex *v) { mram_write(v,&paper_vertices[u],16); }
static unsigned graph_signature(unsigned u,unsigned v) { return ((u+1)*0x9e3779b1U)^((v+1)*0x85ebca6bU); }
static unsigned array_read(uint32_t pointer,unsigned i) {
  unsigned pair[2] __attribute__((aligned(8)));mram_read((__mram_ptr void *)pointer+((i&~1U)*4),pair,8);return pair[i&1];
}
static void array_write(uint32_t pointer,unsigned i,unsigned value) {
  unsigned pair[2] __attribute__((aligned(8)));__mram_ptr void *p=(__mram_ptr void *)pointer+((i&~1U)*4);
  mram_read(p,pair,8);pair[i&1]=value;mram_write(pair,p,8);
}
static void graph_insert(unsigned u,unsigned destination) {
  struct PaperVertex v __attribute__((aligned(8)))=vertex_read(u);
#if PAPER_KIND==1
  struct PaperBlock b __attribute__((aligned(8)));
  if(!v.pointer || v.degree%62==0) {
    __mram_ptr void *p=bench_alloc(256);memset(&b,0,sizeof(b));b.previous=v.pointer;v.pointer=(uint32_t)p;
  } else mram_read((__mram_ptr void *)v.pointer,&b,256);
  assert(b.count<62);b.neighbors[b.count++]=destination;mram_write(&b,(__mram_ptr void *)v.pointer,256);
#else
  if((v.degree+1)*4>v.capacity) {
    unsigned capacity=v.capacity ? v.capacity*2 : 64;
    __mram_ptr void *p=bench_alloc(capacity);
    unsigned buffer[64] __attribute__((aligned(8)));
    unsigned bytes=(v.degree*4+7)&~7U;
    for(unsigned offset=0;offset<bytes;offset+=256) {
      unsigned n=bytes-offset;if(n>256)n=256;
      mram_read((__mram_ptr void *)v.pointer+offset,buffer,n);mram_write(buffer,p+offset,n);
    }
    if(v.pointer)bench_free((__mram_ptr void *)v.pointer,v.capacity);
    v.pointer=(uint32_t)p;v.capacity=capacity;
  }
  array_write(v.pointer,v.degree,destination);
#endif
  v.degree++;vertex_write(u,&v);
}
static void graph_delete(unsigned u,unsigned destination) {
  struct PaperVertex v __attribute__((aligned(8)))=vertex_read(u);assert(v.degree && v.pointer);
#if PAPER_KIND==1
  struct PaperBlock tail __attribute__((aligned(8))),found __attribute__((aligned(8)));
  mram_read((__mram_ptr void *)v.pointer,&tail,256);
  uint32_t p=v.pointer;unsigned index=0,hit=0;
  while(p) {
    mram_read((__mram_ptr void *)p,&found,256);
    for(index=0;index<found.count;++index)if(found.neighbors[index]==destination){hit=1;break;}
    if(hit)break;p=found.previous;
  }
  assert(hit);unsigned replacement=tail.neighbors[tail.count-1];tail.count--;
  if(p==v.pointer) { if(index<tail.count)tail.neighbors[index]=replacement; }
  else { found.neighbors[index]=replacement;mram_write(&found,(__mram_ptr void *)p,256); }
  if(!tail.count) { uint32_t old=v.pointer;v.pointer=tail.previous;bench_free((__mram_ptr void *)old,256); }
  else mram_write(&tail,(__mram_ptr void *)v.pointer,256);
#else
  unsigned index=0;while(index<v.degree && array_read(v.pointer,index)!=destination)index++;
  assert(index<v.degree);array_write(v.pointer,index,array_read(v.pointer,v.degree-1));
#endif
  v.degree--;vertex_write(u,&v);
}
static void app_init(void) {
  struct PaperVertex empty __attribute__((aligned(8)))={0};
  for(unsigned u=me();u<PAPER_VERTICES;u+=NR_TASKLETS)vertex_write(u,&empty);
}
static __attribute__((noinline)) void apply_op(unsigned i) {
  struct PaperOp op __attribute__((aligned(8)));input_read(&paper_ops[i],&op,16);
  assert(op.index%NR_TASKLETS==me());
  if(op.kind==3)graph_insert(op.index,op.value);else{assert(op.kind==4);graph_delete(op.index,op.value);}
}
static void check_neighbor(unsigned u,unsigned destination,unsigned *checksum) {
  assert(destination<PAPER_VERTICES);*checksum+=graph_signature(u,destination);
#ifdef HOST_TEST
  host_expect_neighbor(u,destination);
#endif
}
static __attribute__((noinline)) void app_check(void) {
  unsigned checksum=0,total=0;
  for(unsigned u=me();u<PAPER_VERTICES;u+=NR_TASKLETS) {
    struct PaperVertex v __attribute__((aligned(8)))=vertex_read(u);
    unsigned expected[2] __attribute__((aligned(8)));input_read(&paper_expected_degrees[u],expected,8);
    assert(v.degree==expected[0]);unsigned count=0;
#if PAPER_KIND==1
    uint32_t p=v.pointer;
    while(p) {
      struct PaperBlock b __attribute__((aligned(8)));mram_read((__mram_ptr void *)p,&b,256);
      assert(b.count && b.count<=62);
      for(unsigned i=0;i<b.count;++i)check_neighbor(u,b.neighbors[i],&checksum);
      count+=b.count;assert(count<=v.degree);p=b.previous;
    }
#else
    for(unsigned i=0;i<v.degree;++i)check_neighbor(u,array_read(v.pointer,i),&checksum);
    count=v.degree;
#endif
    assert(count==v.degree);total+=count;
  }
  assert(total==paper_expected_edges[me()] && checksum==paper_expected_sums[me()]);
}
static __attribute__((noinline)) void app_destroy(void) {
  for(unsigned u=me();u<PAPER_VERTICES;u+=NR_TASKLETS) {
    struct PaperVertex v __attribute__((aligned(8)))=vertex_read(u);
#if PAPER_KIND==1
    uint32_t p=v.pointer;
    while(p) { unsigned links[2] __attribute__((aligned(8)));mram_read((__mram_ptr void *)p,links,8);bench_free((__mram_ptr void *)p,256);p=links[0]; }
#else
    if(v.pointer)bench_free((__mram_ptr void *)v.pointer,v.capacity);
#endif
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

