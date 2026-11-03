#define BUDDY_PIN_LEVELS 11
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
/* Pinned-prefix overlay of the maintained buddy backend. Upper packed words
 * are authoritative in shared WRAM; lower DMA windows never overlap them.
 * All callers retain the existing buddy mutex. */
#ifndef BUDDY_PIN_LEVELS
#define BUDDY_PIN_LEVELS 11
#endif
#if BUDDY_PIN_LEVELS < 0 || BUDDY_PIN_LEVELS > 16
#error "BUDDY_PIN_LEVELS must be in 0..16"
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

#if BUDDY_PIN_LEVELS
  int pinnedMetadataWords;
  int *pinnedMetadata;
#endif
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

#if BUDDY_PIN_LEVELS
  int levels = BUDDY_PIN_LEVELS;
  if (levels > buddyAllocator->numBuddyLevels) levels = buddyAllocator->numBuddyLevels;
  /* Round to a complete 8 B DMA boundary. If a level ends on a half-word
   * pair, the first packed word of the following level is pinned too. */
  int bytes = (buddyAllocator->sumMetadataSizePerLevels[levels] + 7) & ~7;
  assert(bytes <= buddyAllocator->metadataSize);
  buddyAllocator->pinnedMetadataWords = bytes >> 2;
  buddyAllocator->pinnedMetadata = mem_alloc(bytes);
  memset(buddyAllocator->pinnedMetadata, 0xFF, bytes);
#endif
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

#if BUDDY_PIN_LEVELS
  if (metadataIndex < buddyAllocator->pinnedMetadataWords)
    return ((unsigned)buddyAllocator->pinnedMetadata[metadataIndex] >> metadataShift) & 3U;
#endif

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
#if BUDDY_PIN_LEVELS
    assert((uint32_t)mramAddress >= (uint32_t)buddyAllocator->mramMetadataBase +
           (buddyAllocator->pinnedMetadataWords << 2));
#endif
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

#if BUDDY_PIN_LEVELS
  if (metadataIndex < buddyAllocator->pinnedMetadataWords) {
    unsigned mask = 3U << metadataShift;
    buddyAllocator->pinnedMetadata[metadataIndex] =
      ((unsigned)buddyAllocator->pinnedMetadata[metadataIndex] & ~mask) | ((unsigned)value << metadataShift);
    return;
  }
#endif

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
#if BUDDY_PIN_LEVELS
    assert((uint32_t)mramAddress >= (uint32_t)buddyAllocator->mramMetadataBase +
           (buddyAllocator->pinnedMetadataWords << 2));
#endif
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
#if BUDDY_PIN_LEVELS
    assert(buddyAllocator->bufferedMetadataIndex >= buddyAllocator->pinnedMetadataWords);
#endif
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


/* Explicit diagnostic/persistence checkpoint; ordinary allocation uses the
 * disjoint WRAM/MRAM authorities and does not write the pinned prefix back.
 * Caller must hold the existing buddy lock, or have quiesced all users. */
void syncPinnedMetadata(struct BuddyAllocator *b) {
  flushMetadata(b);
#if BUDDY_PIN_LEVELS
  int bytes = b->pinnedMetadataWords << 2;
  for (int offset = 0; offset < bytes; offset += 2048) {
    int n = bytes - offset; if (n > 2048) n = 2048;
    mram_write((char *)b->pinnedMetadata + offset, (__mram_ptr char *)b->mramMetadataBase + offset, n);
  }
#endif
}

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
/* Pinned-prefix overlay of the maintained buddy backend. Upper packed words
 * are authoritative in shared WRAM; lower DMA windows never overlap them.
 * All callers retain the existing buddy mutex. */
#ifndef BUDDY_PIN_LEVELS
#define BUDDY_PIN_LEVELS 11
#endif
#if BUDDY_PIN_LEVELS < 0 || BUDDY_PIN_LEVELS > 16
#error "BUDDY_PIN_LEVELS must be in 0..16"
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

#if BUDDY_PIN_LEVELS
  int pinnedMetadataWords;
  int *pinnedMetadata;
#endif
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

#if BUDDY_PIN_LEVELS
  int levels = BUDDY_PIN_LEVELS;
  if (levels > buddyAllocator->numBuddyLevels) levels = buddyAllocator->numBuddyLevels;
  /* Round to a complete 8 B DMA boundary. If a level ends on a half-word
   * pair, the first packed word of the following level is pinned too. */
  int bytes = (buddyAllocator->sumMetadataSizePerLevels[levels] + 7) & ~7;
  assert(bytes <= buddyAllocator->metadataSize);
  buddyAllocator->pinnedMetadataWords = bytes >> 2;
  buddyAllocator->pinnedMetadata = mem_alloc(bytes);
  memset(buddyAllocator->pinnedMetadata, 0xFF, bytes);
#endif
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

#if BUDDY_PIN_LEVELS
  if (metadataIndex < buddyAllocator->pinnedMetadataWords)
    return ((unsigned)buddyAllocator->pinnedMetadata[metadataIndex] >> metadataShift) & 3U;
#endif

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
#if BUDDY_PIN_LEVELS
    assert((uint32_t)mramAddress >= (uint32_t)buddyAllocator->mramMetadataBase +
           (buddyAllocator->pinnedMetadataWords << 2));
#endif
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

#if BUDDY_PIN_LEVELS
  if (metadataIndex < buddyAllocator->pinnedMetadataWords) {
    unsigned mask = 3U << metadataShift;
    buddyAllocator->pinnedMetadata[metadataIndex] =
      ((unsigned)buddyAllocator->pinnedMetadata[metadataIndex] & ~mask) | ((unsigned)value << metadataShift);
    return;
  }
#endif

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
#if BUDDY_PIN_LEVELS
    assert((uint32_t)mramAddress >= (uint32_t)buddyAllocator->mramMetadataBase +
           (buddyAllocator->pinnedMetadataWords << 2));
#endif
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
#if BUDDY_PIN_LEVELS
    assert(buddyAllocator->bufferedMetadataIndex >= buddyAllocator->pinnedMetadataWords);
#endif
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


/* Explicit diagnostic/persistence checkpoint; ordinary allocation uses the
 * disjoint WRAM/MRAM authorities and does not write the pinned prefix back.
 * Caller must hold the existing buddy lock, or have quiesced all users. */
void syncPinnedMetadata(struct BuddyAllocator *b) {
  flushMetadata(b);
#if BUDDY_PIN_LEVELS
  int bytes = b->pinnedMetadataWords << 2;
  for (int offset = 0; offset < bytes; offset += 2048) {
    int n = bytes - offset; if (n > 2048) n = 2048;
    mram_write((char *)b->pinnedMetadata + offset, (__mram_ptr char *)b->mramMetadataBase + offset, n);
  }
#endif
}

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
/* Pinned-prefix overlay of the maintained buddy backend. Upper packed words
 * are authoritative in shared WRAM; lower DMA windows never overlap them.
 * All callers retain the existing buddy mutex. */
#ifndef BUDDY_PIN_LEVELS
#define BUDDY_PIN_LEVELS 11
#endif
#if BUDDY_PIN_LEVELS < 0 || BUDDY_PIN_LEVELS > 16
#error "BUDDY_PIN_LEVELS must be in 0..16"
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

#if BUDDY_PIN_LEVELS
  int pinnedMetadataWords;
  int *pinnedMetadata;
#endif
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

#if BUDDY_PIN_LEVELS
  int levels = BUDDY_PIN_LEVELS;
  if (levels > buddyAllocator->numBuddyLevels) levels = buddyAllocator->numBuddyLevels;
  /* Round to a complete 8 B DMA boundary. If a level ends on a half-word
   * pair, the first packed word of the following level is pinned too. */
  int bytes = (buddyAllocator->sumMetadataSizePerLevels[levels] + 7) & ~7;
  assert(bytes <= buddyAllocator->metadataSize);
  buddyAllocator->pinnedMetadataWords = bytes >> 2;
  buddyAllocator->pinnedMetadata = mem_alloc(bytes);
  memset(buddyAllocator->pinnedMetadata, 0xFF, bytes);
#endif
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

#if BUDDY_PIN_LEVELS
  if (metadataIndex < buddyAllocator->pinnedMetadataWords)
    return ((unsigned)buddyAllocator->pinnedMetadata[metadataIndex] >> metadataShift) & 3U;
#endif

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
#if BUDDY_PIN_LEVELS
    assert((uint32_t)mramAddress >= (uint32_t)buddyAllocator->mramMetadataBase +
           (buddyAllocator->pinnedMetadataWords << 2));
#endif
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

#if BUDDY_PIN_LEVELS
  if (metadataIndex < buddyAllocator->pinnedMetadataWords) {
    unsigned mask = 3U << metadataShift;
    buddyAllocator->pinnedMetadata[metadataIndex] =
      ((unsigned)buddyAllocator->pinnedMetadata[metadataIndex] & ~mask) | ((unsigned)value << metadataShift);
    return;
  }
#endif

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
#if BUDDY_PIN_LEVELS
    assert((uint32_t)mramAddress >= (uint32_t)buddyAllocator->mramMetadataBase +
           (buddyAllocator->pinnedMetadataWords << 2));
#endif
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
#if BUDDY_PIN_LEVELS
    assert(buddyAllocator->bufferedMetadataIndex >= buddyAllocator->pinnedMetadataWords);
#endif
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


/* Explicit diagnostic/persistence checkpoint; ordinary allocation uses the
 * disjoint WRAM/MRAM authorities and does not write the pinned prefix back.
 * Caller must hold the existing buddy lock, or have quiesced all users. */
void syncPinnedMetadata(struct BuddyAllocator *b) {
  flushMetadata(b);
#if BUDDY_PIN_LEVELS
  int bytes = b->pinnedMetadataWords << 2;
  for (int offset = 0; offset < bytes; offset += 2048) {
    int n = bytes - offset; if (n > 2048) n = 2048;
    mram_write((char *)b->pinnedMetadata + offset, (__mram_ptr char *)b->mramMetadataBase + offset, n);
  }
#endif
}

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
  {0U,0U,6480U,0U},
  {0U,1U,6904U,0U},
  {0U,2U,6148U,0U},
  {0U,3U,442U,0U},
  {0U,4U,2939U,0U},
  {0U,5U,3380U,0U},
  {0U,6U,4626U,0U},
  {0U,7U,6750U,0U},
  {0U,8U,2697U,0U},
  {0U,9U,6284U,0U},
  {0U,10U,3635U,0U},
  {0U,11U,509U,0U},
  {0U,12U,5690U,0U},
  {0U,13U,3062U,0U},
  {0U,14U,3512U,0U},
  {0U,15U,6306U,0U},
  {0U,16U,5817U,0U},
  {0U,17U,134U,0U},
  {0U,18U,375U,0U},
  {0U,19U,2068U,0U},
  {0U,20U,7382U,0U},
  {0U,21U,6196U,0U},
  {0U,22U,7753U,0U},
  {0U,23U,5986U,0U},
  {0U,24U,2142U,0U},
  {0U,25U,3541U,0U},
  {0U,26U,10U,0U},
  {0U,27U,97U,0U},
  {0U,28U,6371U,0U},
  {0U,29U,7801U,0U},
  {0U,30U,6846U,0U},
  {0U,31U,1798U,0U},
  {0U,32U,723U,0U},
  {0U,33U,5252U,0U},
  {0U,34U,6454U,0U},
  {0U,35U,5893U,0U},
  {0U,36U,4148U,0U},
  {0U,37U,157U,0U},
  {0U,38U,6173U,0U},
  {0U,39U,4145U,0U},
  {0U,40U,4832U,0U},
  {0U,41U,684U,0U},
  {0U,42U,1159U,0U},
  {0U,43U,8106U,0U},
  {0U,44U,5076U,0U},
  {0U,45U,3146U,0U},
  {0U,46U,6622U,0U},
  {0U,47U,1852U,0U},
  {0U,48U,8176U,0U},
  {0U,49U,2291U,0U},
  {0U,50U,8095U,0U},
  {0U,51U,4995U,0U},
  {0U,52U,4644U,0U},
  {0U,53U,7556U,0U},
  {0U,54U,7721U,0U},
  {0U,55U,1942U,0U},
  {0U,56U,3843U,0U},
  {0U,57U,5823U,0U},
  {0U,58U,3207U,0U},
  {0U,59U,1421U,0U},
  {0U,60U,6520U,0U},
  {0U,61U,1265U,0U},
  {0U,62U,824U,0U},
  {0U,63U,4703U,0U},
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
  {2U,19U,3850U,129U},
  {2U,22U,4657U,130U},
  {2U,53U,3638U,131U},
  {2U,13U,2090U,132U},
  {2U,0U,5284U,133U},
  {2U,60U,4414U,134U},
  {2U,31U,3486U,135U},
  {2U,15U,749U,136U},
  {2U,5U,6846U,137U},
  {2U,30U,339U,138U},
  {2U,60U,1306U,139U},
  {2U,52U,6439U,140U},
  {2U,10U,5201U,141U},
  {2U,56U,2103U,142U},
  {2U,51U,6107U,143U},
  {2U,52U,2618U,144U},
  {2U,14U,1371U,145U},
  {2U,11U,8129U,146U},
  {2U,61U,6061U,147U},
  {2U,19U,1211U,148U},
  {2U,37U,278U,149U},
  {2U,16U,5888U,150U},
  {2U,55U,6737U,151U},
  {2U,27U,1353U,152U},
  {2U,7U,6250U,153U},
  {2U,63U,6481U,154U},
  {2U,28U,6991U,155U},
  {2U,49U,2755U,156U},
  {2U,43U,6518U,157U},
  {2U,7U,272U,158U},
  {2U,58U,2961U,159U},
  {2U,21U,5672U,160U},
  {2U,11U,1023U,161U},
  {2U,62U,2547U,162U},
  {2U,60U,1271U,163U},
  {2U,15U,2463U,164U},
  {2U,53U,2500U,165U},
  {2U,60U,7766U,166U},
  {2U,53U,6680U,167U},
  {2U,35U,5606U,168U},
  {2U,53U,4796U,169U},
  {2U,63U,3188U,170U},
  {2U,5U,6288U,171U},
  {2U,30U,6112U,172U},
  {2U,41U,1387U,173U},
  {2U,31U,5009U,174U},
  {2U,58U,1217U,175U},
  {2U,33U,5804U,176U},
  {2U,59U,1374U,177U},
  {2U,21U,4540U,178U},
  {2U,11U,2058U,179U},
  {2U,51U,52U,180U},
  {2U,17U,3960U,181U},
  {2U,15U,2403U,182U},
  {2U,7U,401U,183U},
  {2U,29U,5538U,184U},
  {2U,31U,6609U,185U},
  {2U,7U,5506U,186U},
  {2U,20U,2049U,187U},
  {2U,38U,1214U,188U},
  {2U,41U,6177U,189U},
  {2U,24U,3733U,190U},
  {2U,44U,6271U,191U},
  {2U,59U,4299U,192U},
  {2U,3U,7730U,193U},
  {2U,11U,2088U,194U},
  {2U,10U,5417U,195U},
  {2U,35U,485U,196U},
  {2U,45U,4661U,197U},
  {2U,54U,6143U,198U},
  {2U,37U,1997U,199U},
  {2U,59U,610U,200U},
  {2U,39U,8001U,201U},
  {2U,18U,174U,202U},
  {2U,39U,2515U,203U},
  {2U,60U,5094U,204U},
  {2U,32U,6165U,205U},
  {2U,20U,3195U,206U},
  {2U,2U,1592U,207U},
  {2U,15U,8023U,208U},
  {2U,58U,5043U,209U},
  {2U,19U,4037U,210U},
  {2U,4U,1203U,211U},
  {2U,19U,3327U,212U},
  {2U,61U,2367U,213U},
  {2U,29U,270U,214U},
  {2U,52U,3382U,215U},
  {2U,39U,2721U,216U},
  {2U,8U,4574U,217U},
  {2U,47U,4968U,218U},
  {2U,12U,7377U,219U},
  {2U,35U,7784U,220U},
  {2U,8U,1851U,221U},
  {2U,18U,3001U,222U},
  {2U,20U,4834U,223U},
  {2U,60U,7965U,224U},
  {2U,31U,3948U,225U},
  {2U,17U,7577U,226U},
  {2U,41U,4556U,227U},
  {2U,31U,2520U,228U},
  {2U,0U,2863U,229U},
  {2U,29U,5525U,230U},
  {2U,42U,212U,231U},
  {2U,28U,4834U,232U},
  {2U,5U,3208U,233U},
  {2U,46U,7193U,234U},
  {2U,5U,5221U,235U},
  {2U,17U,3442U,236U},
  {2U,0U,2086U,237U},
  {2U,8U,235U,238U},
  {2U,57U,5316U,239U},
  {2U,3U,1230U,240U},
  {2U,4U,6756U,241U},
  {2U,13U,5883U,242U},
  {2U,26U,7131U,243U},
  {2U,4U,1106U,244U},
  {2U,52U,513U,245U},
  {2U,30U,3614U,246U},
  {2U,17U,5551U,247U},
  {2U,40U,3950U,248U},
  {2U,21U,4662U,249U},
  {2U,62U,1945U,250U},
  {2U,20U,581U,251U},
  {2U,29U,130U,252U},
  {2U,6U,165U,253U},
  {2U,53U,5488U,254U},
  {2U,11U,233U,255U},
  {2U,13U,5556U,256U},
  {2U,61U,4456U,257U},
  {2U,60U,7895U,258U},
  {2U,62U,4845U,259U},
  {2U,63U,4500U,260U},
  {2U,30U,3629U,261U},
  {2U,11U,6794U,262U},
  {2U,47U,6826U,263U},
  {2U,14U,3492U,264U},
  {2U,42U,3252U,265U},
  {2U,47U,2678U,266U},
  {2U,50U,1941U,267U},
  {2U,39U,5528U,268U},
  {2U,61U,307U,269U},
  {2U,63U,2402U,270U},
  {2U,14U,4471U,271U},
  {2U,36U,4678U,272U},
  {2U,0U,2833U,273U},
  {2U,39U,2873U,274U},
  {2U,58U,7800U,275U},
  {2U,3U,2850U,276U},
  {2U,19U,5435U,277U},
  {2U,31U,1308U,278U},
  {2U,25U,2380U,279U},
  {2U,8U,868U,280U},
  {2U,54U,4559U,281U},
  {2U,42U,3338U,282U},
  {2U,14U,701U,283U},
  {2U,34U,238U,284U},
  {2U,2U,2873U,285U},
  {2U,7U,4069U,286U},
  {2U,27U,1444U,287U},
  {2U,41U,961U,288U},
  {2U,19U,6570U,289U},
  {2U,37U,6010U,290U},
  {2U,56U,3994U,291U},
  {2U,33U,1036U,292U},
  {2U,47U,3565U,293U},
  {2U,28U,7484U,294U},
  {2U,7U,7456U,295U},
  {2U,57U,3439U,296U},
  {2U,31U,5340U,297U},
  {2U,19U,8011U,298U},
  {2U,12U,4712U,299U},
  {2U,45U,7811U,300U},
  {2U,3U,566U,301U},
  {2U,53U,4181U,302U},
  {2U,40U,5469U,303U},
  {2U,32U,245U,304U},
  {2U,21U,2399U,305U},
  {2U,55U,8139U,306U},
  {2U,36U,6751U,307U},
  {2U,23U,5363U,308U},
  {2U,39U,4373U,309U},
  {2U,10U,4841U,310U},
  {2U,57U,6674U,311U},
  {2U,26U,1528U,312U},
  {2U,63U,450U,313U},
  {2U,13U,6577U,314U},
  {2U,15U,349U,315U},
  {2U,22U,2888U,316U},
  {2U,27U,3796U,317U},
  {2U,63U,4801U,318U},
  {2U,46U,2922U,319U},
  {2U,3U,773U,320U},
  {2U,46U,4528U,321U},
  {2U,50U,5494U,322U},
  {2U,17U,3960U,323U},
  {2U,39U,1426U,324U},
  {2U,27U,8125U,325U},
  {2U,23U,1261U,326U},
  {2U,12U,1052U,327U},
  {2U,7U,2835U,328U},
  {2U,11U,7620U,329U},
  {2U,38U,4672U,330U},
  {2U,39U,271U,331U},
  {2U,54U,1952U,332U},
  {2U,32U,1515U,333U},
  {2U,57U,95U,334U},
  {2U,12U,7151U,335U},
  {2U,43U,1234U,336U},
  {2U,9U,4984U,337U},
  {2U,12U,2409U,338U},
  {2U,8U,5843U,339U},
  {2U,54U,2501U,340U},
  {2U,20U,3230U,341U},
  {2U,36U,6433U,342U},
  {2U,50U,1911U,343U},
  {2U,6U,5306U,344U},
  {2U,35U,3315U,345U},
  {2U,55U,3544U,346U},
  {2U,8U,4662U,347U},
  {2U,30U,2498U,348U},
  {2U,47U,5727U,349U},
  {2U,53U,1575U,350U},
  {2U,36U,5216U,351U},
  {2U,20U,1725U,352U},
  {2U,57U,4541U,353U},
  {2U,55U,2683U,354U},
  {2U,23U,3425U,355U},
  {2U,3U,719U,356U},
  {2U,0U,4227U,357U},
  {2U,59U,5778U,358U},
  {2U,37U,3144U,359U},
  {2U,48U,205U,360U},
  {2U,56U,4725U,361U},
  {2U,0U,4157U,362U},
  {2U,18U,1795U,363U},
  {2U,31U,2437U,364U},
  {2U,30U,5859U,365U},
  {2U,30U,4877U,366U},
  {2U,17U,454U,367U},
  {2U,2U,929U,368U},
  {2U,8U,4927U,369U},
  {2U,9U,189U,370U},
  {2U,9U,1832U,371U},
  {2U,26U,7388U,372U},
  {2U,57U,1366U,373U},
  {2U,40U,4764U,374U},
  {2U,37U,5274U,375U},
  {2U,1U,7451U,376U},
  {2U,62U,5417U,377U},
  {2U,1U,7778U,378U},
  {2U,14U,4311U,379U},
  {2U,33U,2723U,380U},
  {2U,32U,5579U,381U},
  {2U,46U,193U,382U},
  {2U,13U,7465U,383U},
  {2U,24U,2799U,384U},
  {2U,45U,2174U,385U},
  {2U,45U,126U,386U},
  {2U,51U,7117U,387U},
  {2U,44U,5676U,388U},
  {2U,63U,3021U,389U},
  {2U,16U,4596U,390U},
  {2U,40U,2959U,391U},
  {2U,29U,4421U,392U},
  {2U,13U,5895U,393U},
  {2U,27U,4872U,394U},
  {2U,26U,6518U,395U},
  {2U,22U,6096U,396U},
  {2U,17U,7784U,397U},
  {2U,21U,2788U,398U},
  {2U,12U,4006U,399U},
  {2U,17U,7069U,400U},
  {2U,16U,5512U,401U},
  {2U,52U,311U,402U},
  {2U,38U,7481U,403U},
  {2U,55U,193U,404U},
  {2U,17U,7426U,405U},
  {2U,42U,1674U,406U},
  {2U,59U,766U,407U},
  {2U,61U,912U,408U},
  {2U,44U,3780U,409U},
  {2U,32U,3282U,410U},
  {2U,50U,5127U,411U},
  {2U,29U,1055U,412U},
  {2U,16U,1293U,413U},
  {2U,13U,214U,414U},
  {2U,51U,6111U,415U},
  {2U,15U,1227U,416U},
  {2U,51U,4489U,417U},
  {2U,34U,3014U,418U},
  {2U,33U,6129U,419U},
  {2U,26U,1095U,420U},
  {2U,62U,6922U,421U},
  {2U,8U,5830U,422U},
  {2U,62U,4925U,423U},
  {2U,57U,6713U,424U},
  {2U,36U,7930U,425U},
  {2U,59U,5672U,426U},
  {2U,56U,4295U,427U},
  {2U,2U,6020U,428U},
  {2U,56U,7141U,429U},
  {2U,5U,5494U,430U},
  {2U,6U,4811U,431U},
  {2U,41U,7861U,432U},
  {2U,60U,3817U,433U},
  {2U,30U,1756U,434U},
  {2U,42U,2736U,435U},
  {2U,48U,3903U,436U},
  {2U,3U,6862U,437U},
  {2U,34U,5548U,438U},
  {2U,26U,5075U,439U},
  {2U,19U,5731U,440U},
  {2U,49U,2588U,441U},
  {2U,12U,4266U,442U},
  {2U,14U,4355U,443U},
  {2U,6U,6108U,444U},
  {2U,20U,907U,445U},
  {2U,24U,2995U,446U},
  {2U,21U,4756U,447U},
  {2U,29U,5641U,448U},
  {2U,0U,1327U,449U},
  {2U,39U,4077U,450U},
  {2U,25U,6206U,451U},
  {2U,18U,7417U,452U},
  {2U,3U,7524U,453U},
  {2U,54U,2834U,454U},
  {2U,13U,422U,455U},
  {2U,48U,6226U,456U},
  {2U,2U,6726U,457U},
  {2U,42U,4256U,458U},
  {2U,37U,256U,459U},
  {2U,21U,3536U,460U},
  {2U,50U,4665U,461U},
  {2U,30U,6073U,462U},
  {2U,34U,7741U,463U},
  {2U,10U,69U,464U},
  {2U,59U,7292U,465U},
  {2U,51U,3491U,466U},
  {2U,10U,8116U,467U},
  {2U,26U,4546U,468U},
  {2U,59U,4466U,469U},
  {2U,6U,883U,470U},
  {2U,6U,5628U,471U},
  {2U,23U,2515U,472U},
  {2U,55U,7244U,473U},
  {2U,42U,847U,474U},
  {2U,48U,3847U,475U},
  {2U,34U,2668U,476U},
  {2U,56U,7761U,477U},
  {2U,62U,6612U,478U},
  {2U,23U,3557U,479U},
  {2U,16U,7836U,480U},
  {2U,49U,2622U,481U},
  {2U,30U,4286U,482U},
  {2U,46U,3290U,483U},
  {2U,43U,7761U,484U},
  {2U,49U,4778U,485U},
  {2U,15U,3841U,486U},
  {2U,63U,1092U,487U},
  {2U,18U,3259U,488U},
  {2U,29U,1195U,489U},
  {2U,35U,1336U,490U},
  {2U,58U,998U,491U},
  {2U,57U,1578U,492U},
  {2U,7U,2419U,493U},
  {2U,9U,6063U,494U},
  {2U,10U,6863U,495U},
  {2U,27U,763U,496U},
  {2U,17U,6216U,497U},
  {2U,35U,4427U,498U},
  {2U,46U,1442U,499U},
  {2U,31U,1033U,500U},
  {2U,51U,5755U,501U},
  {2U,56U,1433U,502U},
  {2U,47U,6968U,503U},
  {2U,57U,2897U,504U},
  {2U,39U,7663U,505U},
  {2U,42U,5956U,506U},
  {2U,46U,5751U,507U},
  {2U,15U,484U,508U},
  {2U,57U,5036U,509U},
  {2U,45U,5435U,510U},
  {2U,31U,2795U,511U},
  {2U,9U,617U,512U},
  {2U,23U,5091U,513U},
  {2U,38U,2404U,514U},
  {2U,27U,1965U,515U},
  {2U,14U,3990U,516U},
  {2U,36U,1694U,517U},
  {2U,25U,7754U,518U},
  {2U,11U,2965U,519U},
  {2U,12U,511U,520U},
  {2U,30U,4664U,521U},
  {2U,36U,6558U,522U},
  {2U,33U,4291U,523U},
  {2U,54U,6611U,524U},
  {2U,37U,7604U,525U},
  {2U,7U,7053U,526U},
  {2U,4U,2420U,527U},
  {2U,54U,4525U,528U},
  {2U,46U,6713U,529U},
  {2U,63U,7523U,530U},
  {2U,25U,3946U,531U},
  {2U,34U,3712U,532U},
  {2U,59U,6748U,533U},
  {2U,8U,1155U,534U},
  {2U,30U,4833U,535U},
  {2U,4U,1285U,536U},
  {2U,38U,3861U,537U},
  {2U,53U,6947U,538U},
  {2U,12U,7373U,539U},
  {2U,29U,1111U,540U},
  {2U,52U,8146U,541U},
  {2U,41U,2441U,542U},
  {2U,26U,2447U,543U},
  {2U,49U,4305U,544U},
  {2U,27U,2566U,545U},
  {2U,19U,5277U,546U},
  {2U,24U,1871U,547U},
  {2U,57U,6673U,548U},
  {2U,30U,3419U,549U},
  {2U,48U,613U,550U},
  {2U,1U,5558U,551U},
  {2U,63U,3660U,552U},
  {2U,42U,3091U,553U},
  {2U,6U,2661U,554U},
  {2U,10U,6860U,555U},
  {2U,3U,3965U,556U},
  {2U,61U,7594U,557U},
  {2U,32U,8087U,558U},
  {2U,16U,494U,559U},
  {2U,34U,853U,560U},
  {2U,6U,8063U,561U},
  {2U,22U,2153U,562U},
  {2U,63U,5679U,563U},
  {2U,31U,3442U,564U},
  {2U,50U,1927U,565U},
  {2U,8U,2716U,566U},
  {2U,45U,4800U,567U},
  {2U,40U,8181U,568U},
  {2U,12U,5161U,569U},
  {2U,59U,4970U,570U},
  {2U,44U,8042U,571U},
  {2U,33U,6681U,572U},
  {2U,7U,6067U,573U},
  {2U,59U,2904U,574U},
  {2U,18U,3225U,575U},
  {2U,46U,2404U,576U},
  {2U,34U,1494U,577U},
  {2U,25U,4782U,578U},
  {2U,57U,2813U,579U},
  {2U,30U,5290U,580U},
  {2U,63U,3128U,581U},
  {2U,38U,678U,582U},
  {2U,25U,1183U,583U},
  {2U,9U,5569U,584U},
  {2U,31U,6008U,585U},
  {2U,8U,7187U,586U},
  {2U,43U,4236U,587U},
  {2U,12U,7900U,588U},
  {2U,53U,7212U,589U},
  {2U,8U,1496U,590U},
  {2U,32U,3505U,591U},
  {2U,55U,2546U,592U},
  {2U,32U,7510U,593U},
  {2U,32U,2163U,594U},
  {2U,33U,124U,595U},
  {2U,43U,5422U,596U},
  {2U,22U,1667U,597U},
  {2U,23U,7031U,598U},
  {2U,17U,1135U,599U},
  {2U,11U,3360U,600U},
  {2U,29U,6975U,601U},
  {2U,58U,1994U,602U},
  {2U,10U,2929U,603U},
  {2U,60U,293U,604U},
  {2U,13U,7157U,605U},
  {2U,45U,4920U,606U},
  {2U,38U,5096U,607U},
  {2U,32U,2857U,608U},
  {2U,55U,1059U,609U},
  {2U,59U,2150U,610U},
  {2U,62U,1882U,611U},
  {2U,15U,2768U,612U},
  {2U,21U,7979U,613U},
  {2U,63U,7726U,614U},
  {2U,60U,1785U,615U},
  {2U,24U,4233U,616U},
  {2U,7U,1039U,617U},
  {2U,42U,4242U,618U},
  {2U,43U,3853U,619U},
  {2U,37U,220U,620U},
  {2U,30U,870U,621U},
  {2U,19U,2541U,622U},
  {2U,29U,1870U,623U},
  {2U,33U,2433U,624U},
  {2U,8U,2046U,625U},
  {2U,56U,534U,626U},
  {2U,10U,1281U,627U},
  {2U,8U,5947U,628U},
  {2U,20U,6971U,629U},
  {2U,41U,6781U,630U},
  {2U,51U,5099U,631U},
  {2U,0U,5846U,632U},
  {2U,13U,1233U,633U},
  {2U,58U,6956U,634U},
  {2U,32U,4269U,635U},
  {2U,39U,2139U,636U},
  {2U,7U,5904U,637U},
  {2U,62U,7887U,638U},
  {2U,46U,5974U,639U},
  {2U,35U,8139U,640U},
  {0U,0U,2133U,0U},
  {0U,1U,6972U,0U},
  {0U,2U,8016U,0U},
  {0U,3U,516U,0U},
  {0U,4U,4343U,0U},
  {0U,5U,4562U,0U},
  {0U,6U,1261U,0U},
  {0U,7U,7265U,0U},
  {0U,8U,4155U,0U},
  {0U,9U,6944U,0U},
  {0U,10U,5267U,0U},
  {0U,11U,5816U,0U},
  {0U,12U,2097U,0U},
  {0U,13U,3463U,0U},
  {0U,14U,5669U,0U},
  {0U,15U,1115U,0U},
  {0U,16U,1389U,0U},
  {0U,17U,5899U,0U},
  {0U,18U,3843U,0U},
  {0U,19U,2274U,0U},
  {0U,20U,1555U,0U},
  {0U,21U,1834U,0U},
  {0U,22U,910U,0U},
  {0U,23U,2319U,0U},
  {0U,24U,5765U,0U},
  {0U,25U,6279U,0U},
  {0U,26U,7191U,0U},
  {0U,27U,6331U,0U},
  {0U,28U,1034U,0U},
  {0U,29U,1110U,0U},
  {0U,30U,6583U,0U},
  {0U,31U,811U,0U},
  {0U,32U,4056U,0U},
  {0U,33U,7361U,0U},
  {0U,34U,1842U,0U},
  {0U,35U,5904U,0U},
  {0U,36U,1295U,0U},
  {0U,37U,3620U,0U},
  {0U,38U,4759U,0U},
  {0U,39U,5729U,0U},
  {0U,40U,2839U,0U},
  {0U,41U,6048U,0U},
  {0U,42U,6804U,0U},
  {0U,43U,8009U,0U},
  {0U,44U,412U,0U},
  {0U,45U,7287U,0U},
  {0U,46U,1107U,0U},
  {0U,47U,2256U,0U},
  {0U,48U,7278U,0U},
  {0U,49U,4210U,0U},
  {0U,50U,3724U,0U},
  {0U,51U,3456U,0U},
  {0U,52U,5650U,0U},
  {0U,53U,8140U,0U},
  {0U,54U,4134U,0U},
  {0U,55U,2798U,0U},
  {0U,56U,1893U,0U},
  {0U,57U,6338U,0U},
  {0U,58U,5163U,0U},
  {0U,59U,2664U,0U},
  {0U,60U,6286U,0U},
  {0U,61U,5800U,0U},
  {0U,62U,7118U,0U},
  {0U,63U,1972U,0U},
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
  {2U,34U,5090U,129U},
  {2U,23U,1793U,130U},
  {2U,40U,4946U,131U},
  {2U,45U,1608U,132U},
  {2U,45U,5806U,133U},
  {2U,19U,3064U,134U},
  {2U,54U,2369U,135U},
  {2U,43U,7407U,136U},
  {2U,5U,7375U,137U},
  {2U,61U,3481U,138U},
  {2U,33U,5760U,139U},
  {2U,31U,7155U,140U},
  {2U,50U,2662U,141U},
  {2U,18U,5341U,142U},
  {2U,60U,4660U,143U},
  {2U,27U,6898U,144U},
  {2U,35U,4411U,145U},
  {2U,21U,5919U,146U},
  {2U,23U,2534U,147U},
  {2U,53U,7206U,148U},
  {2U,38U,2395U,149U},
  {2U,3U,5842U,150U},
  {2U,45U,688U,151U},
  {2U,63U,2744U,152U},
  {2U,62U,2017U,153U},
  {2U,27U,2967U,154U},
  {2U,40U,6877U,155U},
  {2U,62U,5630U,156U},
  {2U,25U,2449U,157U},
  {2U,61U,7891U,158U},
  {2U,23U,2810U,159U},
  {2U,33U,2099U,160U},
  {2U,56U,6134U,161U},
  {2U,4U,1065U,162U},
  {2U,8U,8117U,163U},
  {2U,49U,3891U,164U},
  {2U,52U,2428U,165U},
  {2U,39U,6328U,166U},
  {2U,54U,5228U,167U},
  {2U,54U,5330U,168U},
  {2U,22U,2314U,169U},
  {2U,60U,430U,170U},
  {2U,54U,6102U,171U},
  {2U,28U,7049U,172U},
  {2U,43U,4921U,173U},
  {2U,58U,7644U,174U},
  {2U,22U,7453U,175U},
  {2U,24U,1763U,176U},
  {2U,6U,7302U,177U},
  {2U,46U,700U,178U},
  {2U,20U,421U,179U},
  {2U,25U,3650U,180U},
  {2U,31U,7394U,181U},
  {2U,16U,1593U,182U},
  {2U,30U,6616U,183U},
  {2U,55U,2171U,184U},
  {2U,37U,4289U,185U},
  {2U,20U,7862U,186U},
  {2U,26U,1551U,187U},
  {2U,53U,7061U,188U},
  {2U,22U,6181U,189U},
  {2U,47U,3953U,190U},
  {2U,42U,5683U,191U},
  {2U,8U,177U,192U},
  {2U,33U,2670U,193U},
  {2U,47U,6454U,194U},
  {2U,20U,540U,195U},
  {2U,40U,3422U,196U},
  {2U,3U,4669U,197U},
  {2U,55U,3300U,198U},
  {2U,22U,5323U,199U},
  {2U,45U,6079U,200U},
  {2U,7U,5218U,201U},
  {2U,41U,6104U,202U},
  {2U,36U,6669U,203U},
  {2U,7U,5070U,204U},
  {2U,61U,1392U,205U},
  {2U,38U,2218U,206U},
  {2U,22U,2900U,207U},
  {2U,10U,5408U,208U},
  {2U,40U,7958U,209U},
  {2U,32U,8082U,210U},
  {2U,59U,5066U,211U},
  {2U,20U,1286U,212U},
  {2U,17U,1371U,213U},
  {2U,41U,2947U,214U},
  {2U,41U,3320U,215U},
  {2U,45U,7730U,216U},
  {2U,57U,5676U,217U},
  {2U,24U,6116U,218U},
  {2U,29U,2115U,219U},
  {2U,5U,6440U,220U},
  {2U,31U,2503U,221U},
  {2U,32U,322U,222U},
  {2U,22U,6473U,223U},
  {2U,2U,7607U,224U},
  {2U,52U,7730U,225U},
  {2U,43U,5577U,226U},
  {2U,55U,324U,227U},
  {2U,37U,181U,228U},
  {2U,47U,4309U,229U},
  {2U,43U,3838U,230U},
  {2U,48U,1186U,231U},
  {2U,27U,4919U,232U},
  {2U,24U,4373U,233U},
  {2U,11U,4707U,234U},
  {2U,3U,6625U,235U},
  {2U,9U,6524U,236U},
  {2U,63U,3732U,237U},
  {2U,0U,6371U,238U},
  {2U,22U,5050U,239U},
  {2U,41U,128U,240U},
  {2U,6U,3761U,241U},
  {2U,56U,7299U,242U},
  {2U,13U,427U,243U},
  {2U,27U,4402U,244U},
  {2U,2U,4357U,245U},
  {2U,30U,326U,246U},
  {2U,6U,2850U,247U},
  {2U,12U,6160U,248U},
  {2U,26U,7142U,249U},
  {2U,22U,7002U,250U},
  {2U,54U,3064U,251U},
  {2U,20U,3343U,252U},
  {2U,45U,5676U,253U},
  {2U,20U,1902U,254U},
  {2U,29U,1979U,255U},
  {2U,36U,7805U,256U},
  {2U,37U,6732U,257U},
  {2U,39U,4774U,258U},
  {2U,54U,1288U,259U},
  {2U,15U,3679U,260U},
  {2U,19U,6754U,261U},
  {2U,17U,6299U,262U},
  {2U,53U,1392U,263U},
  {2U,7U,7310U,264U},
  {2U,43U,2567U,265U},
  {2U,60U,6112U,266U},
  {2U,58U,8030U,267U},
  {2U,55U,7250U,268U},
  {2U,33U,1348U,269U},
  {2U,32U,6040U,270U},
  {2U,40U,4502U,271U},
  {2U,29U,4374U,272U},
  {2U,61U,1194U,273U},
  {2U,28U,5630U,274U},
  {2U,37U,339U,275U},
  {2U,55U,4357U,276U},
  {2U,53U,1760U,277U},
  {2U,32U,5006U,278U},
  {2U,7U,6871U,279U},
  {2U,33U,2593U,280U},
  {2U,46U,2855U,281U},
  {2U,26U,2276U,282U},
  {2U,47U,5473U,283U},
  {2U,32U,6532U,284U},
  {2U,16U,3439U,285U},
  {2U,50U,1507U,286U},
  {2U,28U,6125U,287U},
  {2U,10U,661U,288U},
  {2U,28U,5130U,289U},
  {2U,35U,7132U,290U},
  {2U,61U,7346U,291U},
  {2U,53U,800U,292U},
  {2U,31U,5826U,293U},
  {2U,21U,7878U,294U},
  {2U,38U,830U,295U},
  {2U,25U,6621U,296U},
  {2U,5U,5159U,297U},
  {2U,61U,6269U,298U},
  {2U,39U,296U,299U},
  {2U,14U,4942U,300U},
  {2U,53U,5976U,301U},
  {2U,15U,5209U,302U},
  {2U,21U,7541U,303U},
  {2U,14U,1041U,304U},
  {2U,12U,3684U,305U},
  {2U,55U,3866U,306U},
  {2U,1U,4595U,307U},
  {2U,14U,129U,308U},
  {2U,19U,2516U,309U},
  {2U,59U,6082U,310U},
  {2U,49U,6961U,311U},
  {2U,57U,6702U,312U},
  {2U,42U,1550U,313U},
  {2U,37U,3152U,314U},
  {2U,29U,8070U,315U},
  {2U,25U,6110U,316U},
  {2U,52U,6839U,317U},
  {2U,12U,6262U,318U},
  {2U,21U,1969U,319U},
  {2U,41U,6045U,320U},
  {2U,45U,1880U,321U},
  {2U,24U,6569U,322U},
  {2U,0U,7676U,323U},
  {2U,35U,5746U,324U},
  {2U,30U,4944U,325U},
  {2U,53U,5717U,326U},
  {2U,29U,5699U,327U},
  {2U,20U,2856U,328U},
  {2U,23U,3676U,329U},
  {2U,34U,6661U,330U},
  {2U,47U,6199U,331U},
  {2U,11U,4940U,332U},
  {2U,32U,5995U,333U},
  {2U,10U,5658U,334U},
  {2U,2U,465U,335U},
  {2U,15U,6987U,336U},
  {2U,61U,4767U,337U},
  {2U,44U,1409U,338U},
  {2U,23U,198U,339U},
  {2U,4U,6159U,340U},
  {2U,30U,845U,341U},
  {2U,31U,3159U,342U},
  {2U,13U,5865U,343U},
  {2U,32U,6936U,344U},
  {2U,18U,3391U,345U},
  {2U,18U,7440U,346U},
  {2U,10U,2611U,347U},
  {2U,15U,7921U,348U},
  {2U,26U,6533U,349U},
  {2U,42U,730U,350U},
  {2U,24U,1168U,351U},
  {2U,37U,6935U,352U},
  {2U,34U,2372U,353U},
  {2U,47U,5494U,354U},
  {2U,27U,3578U,355U},
  {2U,54U,5853U,356U},
  {2U,62U,6989U,357U},
  {2U,32U,4133U,358U},
  {2U,45U,5151U,359U},
  {2U,26U,5465U,360U},
  {2U,7U,2147U,361U},
  {2U,54U,3266U,362U},
  {2U,4U,3427U,363U},
  {2U,8U,3807U,364U},
  {2U,62U,570U,365U},
  {2U,22U,6181U,366U},
  {2U,53U,8007U,367U},
  {2U,16U,1474U,368U},
  {2U,33U,5141U,369U},
  {2U,15U,274U,370U},
  {2U,40U,3522U,371U},
  {2U,47U,7775U,372U},
  {2U,45U,3566U,373U},
  {2U,43U,5898U,374U},
  {2U,33U,7995U,375U},
  {2U,46U,6362U,376U},
  {2U,16U,7896U,377U},
  {2U,6U,61U,378U},
  {2U,49U,7734U,379U},
  {2U,58U,5254U,380U},
  {2U,20U,7631U,381U},
  {2U,13U,818U,382U},
  {2U,6U,5194U,383U},
  {2U,10U,3228U,384U},
  {2U,52U,3271U,385U},
  {2U,4U,5740U,386U},
  {2U,60U,7032U,387U},
  {2U,22U,6707U,388U},
  {2U,41U,6057U,389U},
  {2U,43U,4318U,390U},
  {2U,37U,2232U,391U},
  {2U,26U,847U,392U},
  {2U,42U,4392U,393U},
  {2U,43U,6882U,394U},
  {2U,63U,3310U,395U},
  {2U,18U,5055U,396U},
  {2U,34U,6809U,397U},
  {2U,56U,5098U,398U},
  {2U,3U,211U,399U},
  {2U,59U,6100U,400U},
  {2U,50U,738U,401U},
  {2U,4U,2305U,402U},
  {2U,29U,7062U,403U},
  {2U,15U,8168U,404U},
  {2U,42U,7302U,405U},
  {2U,23U,1799U,406U},
  {2U,5U,3703U,407U},
  {2U,41U,2311U,408U},
  {2U,46U,7935U,409U},
  {2U,50U,531U,410U},
  {2U,63U,5507U,411U},
  {2U,25U,3982U,412U},
  {2U,20U,1417U,413U},
  {2U,9U,6006U,414U},
  {2U,54U,3582U,415U},
  {2U,43U,7404U,416U},
  {2U,40U,172U,417U},
  {2U,18U,6308U,418U},
  {2U,56U,1667U,419U},
  {2U,25U,5418U,420U},
  {2U,30U,6264U,421U},
  {2U,34U,963U,422U},
  {2U,22U,4682U,423U},
  {2U,12U,3684U,424U},
  {2U,8U,320U,425U},
  {2U,13U,7848U,426U},
  {2U,60U,6343U,427U},
  {2U,58U,8052U,428U},
  {2U,15U,6822U,429U},
  {2U,54U,3812U,430U},
  {2U,16U,1785U,431U},
  {2U,45U,3217U,432U},
  {2U,34U,4033U,433U},
  {2U,63U,4259U,434U},
  {2U,21U,1577U,435U},
  {2U,0U,3295U,436U},
  {2U,30U,1174U,437U},
  {2U,50U,1921U,438U},
  {2U,30U,7037U,439U},
  {2U,29U,1849U,440U},
  {2U,46U,5389U,441U},
  {2U,14U,4021U,442U},
  {2U,50U,5482U,443U},
  {2U,33U,6933U,444U},
  {2U,41U,2222U,445U},
  {2U,24U,539U,446U},
  {2U,34U,6084U,447U},
  {2U,45U,1976U,448U},
  {2U,50U,1722U,449U},
  {2U,1U,5799U,450U},
  {2U,32U,2599U,451U},
  {2U,15U,901U,452U},
  {2U,34U,548U,453U},
  {2U,58U,4002U,454U},
  {2U,17U,1544U,455U},
  {2U,32U,4639U,456U},
  {2U,24U,2445U,457U},
  {2U,28U,7435U,458U},
  {2U,43U,4809U,459U},
  {2U,45U,2551U,460U},
  {2U,53U,3819U,461U},
  {2U,26U,662U,462U},
  {2U,34U,6969U,463U},
  {2U,14U,952U,464U},
  {2U,10U,2612U,465U},
  {2U,9U,3926U,466U},
  {2U,27U,8048U,467U},
  {2U,13U,7522U,468U},
  {2U,61U,2696U,469U},
  {2U,5U,5900U,470U},
  {2U,23U,7971U,471U},
  {2U,3U,610U,472U},
  {2U,40U,4618U,473U},
  {2U,44U,5433U,474U},
  {2U,44U,5739U,475U},
  {2U,24U,3250U,476U},
  {2U,4U,531U,477U},
  {2U,60U,3753U,478U},
  {2U,36U,4246U,479U},
  {2U,8U,5081U,480U},
  {2U,57U,6456U,481U},
  {2U,24U,3665U,482U},
  {2U,16U,6503U,483U},
  {2U,26U,6366U,484U},
  {2U,24U,5457U,485U},
  {2U,3U,1875U,486U},
  {2U,49U,4958U,487U},
  {2U,8U,5684U,488U},
  {2U,32U,5837U,489U},
  {2U,41U,2088U,490U},
  {2U,22U,6764U,491U},
  {2U,32U,2993U,492U},
  {2U,47U,1976U,493U},
  {2U,41U,5202U,494U},
  {2U,40U,97U,495U},
  {2U,0U,1179U,496U},
  {2U,36U,8140U,497U},
  {2U,9U,4543U,498U},
  {2U,25U,5031U,499U},
  {2U,21U,3797U,500U},
  {2U,21U,6028U,501U},
  {2U,34U,1658U,502U},
  {2U,48U,6947U,503U},
  {2U,14U,3567U,504U},
  {2U,29U,1382U,505U},
  {2U,25U,4334U,506U},
  {2U,63U,6719U,507U},
  {2U,6U,2506U,508U},
  {2U,37U,6427U,509U},
  {2U,32U,6726U,510U},
  {2U,35U,7374U,511U},
  {2U,22U,3537U,512U},
  {2U,14U,5785U,513U},
  {2U,61U,1704U,514U},
  {2U,19U,2038U,515U},
  {2U,56U,1855U,516U},
  {2U,53U,7750U,517U},
  {2U,54U,1092U,518U},
  {2U,52U,3328U,519U},
  {2U,60U,6359U,520U},
  {2U,49U,1122U,521U},
  {2U,30U,3936U,522U},
  {2U,20U,2063U,523U},
  {2U,21U,4534U,524U},
  {2U,17U,4358U,525U},
  {2U,6U,3091U,526U},
  {2U,38U,148U,527U},
  {2U,12U,5570U,528U},
  {2U,31U,2535U,529U},
  {2U,53U,7512U,530U},
  {2U,45U,7120U,531U},
  {2U,37U,7184U,532U},
  {2U,23U,4655U,533U},
  {2U,55U,7973U,534U},
  {2U,25U,2227U,535U},
  {2U,37U,5951U,536U},
  {2U,61U,3686U,537U},
  {2U,12U,7465U,538U},
  {2U,17U,3507U,539U},
  {2U,44U,5586U,540U},
  {2U,42U,2913U,541U},
  {2U,4U,7591U,542U},
  {2U,63U,4560U,543U},
  {2U,61U,3678U,544U},
  {2U,62U,551U,545U},
  {2U,29U,2804U,546U},
  {2U,18U,992U,547U},
  {2U,57U,7116U,548U},
  {2U,27U,1426U,549U},
  {2U,4U,6501U,550U},
  {2U,37U,2413U,551U},
  {2U,19U,6067U,552U},
  {2U,36U,8073U,553U},
  {2U,26U,2305U,554U},
  {2U,3U,2894U,555U},
  {2U,51U,4080U,556U},
  {2U,50U,1595U,557U},
  {2U,52U,6132U,558U},
  {2U,49U,73U,559U},
  {2U,59U,1839U,560U},
  {2U,35U,2049U,561U},
  {2U,51U,7219U,562U},
  {2U,28U,2089U,563U},
  {2U,57U,7601U,564U},
  {2U,34U,5542U,565U},
  {2U,1U,4692U,566U},
  {2U,23U,5383U,567U},
  {2U,57U,1299U,568U},
  {2U,48U,6423U,569U},
  {2U,24U,4383U,570U},
  {2U,51U,5838U,571U},
  {2U,3U,3643U,572U},
  {2U,48U,2631U,573U},
  {2U,0U,3683U,574U},
  {2U,13U,462U,575U},
  {2U,13U,313U,576U},
  {2U,54U,7508U,577U},
  {2U,43U,5245U,578U},
  {2U,21U,4878U,579U},
  {2U,21U,7357U,580U},
  {2U,10U,6251U,581U},
  {2U,37U,1504U,582U},
  {2U,24U,5176U,583U},
  {2U,0U,5884U,584U},
  {2U,63U,6098U,585U},
  {2U,48U,6039U,586U},
  {2U,12U,1627U,587U},
  {2U,28U,1767U,588U},
  {2U,20U,3392U,589U},
  {2U,49U,7084U,590U},
  {2U,3U,533U,591U},
  {2U,23U,1868U,592U},
  {2U,23U,6005U,593U},
  {2U,53U,5685U,594U},
  {2U,36U,1450U,595U},
  {2U,18U,7586U,596U},
  {2U,6U,7594U,597U},
  {2U,50U,7526U,598U},
  {2U,55U,5336U,599U},
  {2U,8U,6441U,600U},
  {2U,42U,4521U,601U},
  {2U,37U,7326U,602U},
  {2U,14U,947U,603U},
  {2U,57U,1902U,604U},
  {2U,44U,5165U,605U},
  {2U,9U,2263U,606U},
  {2U,50U,3370U,607U},
  {2U,8U,6217U,608U},
  {2U,23U,2527U,609U},
  {2U,59U,666U,610U},
  {2U,4U,3421U,611U},
  {2U,13U,533U,612U},
  {2U,49U,6549U,613U},
  {2U,44U,1058U,614U},
  {2U,24U,2639U,615U},
  {2U,24U,4158U,616U},
  {2U,9U,6327U,617U},
  {2U,39U,360U,618U},
  {2U,56U,1927U,619U},
  {2U,61U,5990U,620U},
  {2U,31U,2812U,621U},
  {2U,31U,2864U,622U},
  {2U,35U,7751U,623U},
  {2U,55U,766U,624U},
  {2U,7U,2856U,625U},
  {2U,48U,1948U,626U},
  {2U,58U,4169U,627U},
  {2U,27U,1933U,628U},
  {2U,10U,7989U,629U},
  {2U,44U,5726U,630U},
  {2U,16U,5373U,631U},
  {2U,11U,1944U,632U},
  {2U,20U,2507U,633U},
  {2U,1U,910U,634U},
  {2U,63U,7028U,635U},
  {2U,59U,3913U,636U},
  {2U,46U,7312U,637U},
  {2U,25U,2306U,638U},
  {2U,23U,2066U,639U},
  {2U,28U,1321U,640U},
  {0U,0U,7665U,0U},
  {0U,1U,397U,0U},
  {0U,2U,3239U,0U},
  {0U,3U,5659U,0U},
  {0U,4U,922U,0U},
  {0U,5U,4170U,0U},
  {0U,6U,5647U,0U},
  {0U,7U,415U,0U},
  {0U,8U,5848U,0U},
  {0U,9U,8061U,0U},
  {0U,10U,3580U,0U},
  {0U,11U,5557U,0U},
  {0U,12U,1639U,0U},
  {0U,13U,4684U,0U},
  {0U,14U,5195U,0U},
  {0U,15U,6101U,0U},
  {0U,16U,3677U,0U},
  {0U,17U,1324U,0U},
  {0U,18U,156U,0U},
  {0U,19U,4452U,0U},
  {0U,20U,6550U,0U},
  {0U,21U,7294U,0U},
  {0U,22U,4803U,0U},
  {0U,23U,7845U,0U},
  {0U,24U,2288U,0U},
  {0U,25U,2197U,0U},
  {0U,26U,6052U,0U},
  {0U,27U,3052U,0U},
  {0U,28U,4376U,0U},
  {0U,29U,58U,0U},
  {0U,30U,7524U,0U},
  {0U,31U,1061U,0U},
  {0U,32U,392U,0U},
  {0U,33U,4253U,0U},
  {0U,34U,6365U,0U},
  {0U,35U,3355U,0U},
  {0U,36U,1941U,0U},
  {0U,37U,6610U,0U},
  {0U,38U,3565U,0U},
  {0U,39U,2812U,0U},
  {0U,40U,1356U,0U},
  {0U,41U,4612U,0U},
  {0U,42U,1655U,0U},
  {0U,43U,4872U,0U},
  {0U,44U,5247U,0U},
  {0U,45U,1794U,0U},
  {0U,46U,6675U,0U},
  {0U,47U,5686U,0U},
  {0U,48U,5277U,0U},
  {0U,49U,2128U,0U},
  {0U,50U,2079U,0U},
  {0U,51U,7675U,0U},
  {0U,52U,3480U,0U},
  {0U,53U,5078U,0U},
  {0U,54U,6653U,0U},
  {0U,55U,5729U,0U},
  {0U,56U,2074U,0U},
  {0U,57U,36U,0U},
  {0U,58U,5205U,0U},
  {0U,59U,7915U,0U},
  {0U,60U,685U,0U},
  {0U,61U,5849U,0U},
  {0U,62U,4288U,0U},
  {0U,63U,966U,0U},
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
  {2U,35U,654U,129U},
  {2U,47U,3336U,130U},
  {2U,20U,6656U,131U},
  {2U,45U,6136U,132U},
  {2U,40U,4828U,133U},
  {2U,23U,3614U,134U},
  {2U,6U,3328U,135U},
  {2U,47U,344U,136U},
  {2U,34U,4742U,137U},
  {2U,37U,2453U,138U},
  {2U,23U,7422U,139U},
  {2U,40U,5423U,140U},
  {2U,1U,2546U,141U},
  {2U,14U,7374U,142U},
  {2U,21U,5233U,143U},
  {2U,41U,5028U,144U},
  {2U,6U,4705U,145U},
  {2U,10U,4265U,146U},
  {2U,54U,352U,147U},
  {2U,18U,4102U,148U},
  {2U,28U,2112U,149U},
  {2U,45U,2580U,150U},
  {2U,5U,4812U,151U},
  {2U,33U,7573U,152U},
  {2U,1U,462U,153U},
  {2U,9U,6390U,154U},
  {2U,53U,6811U,155U},
  {2U,53U,5240U,156U},
  {2U,26U,6618U,157U},
  {2U,8U,7666U,158U},
  {2U,29U,8126U,159U},
  {2U,6U,4553U,160U},
  {2U,32U,3757U,161U},
  {2U,39U,6078U,162U},
  {2U,49U,1498U,163U},
  {2U,31U,382U,164U},
  {2U,54U,3705U,165U},
  {2U,39U,6261U,166U},
  {2U,61U,7118U,167U},
  {2U,49U,6893U,168U},
  {2U,5U,5046U,169U},
  {2U,12U,4891U,170U},
  {2U,26U,3815U,171U},
  {2U,57U,3309U,172U},
  {2U,21U,7323U,173U},
  {2U,52U,3239U,174U},
  {2U,54U,1866U,175U},
  {2U,61U,1736U,176U},
  {2U,35U,5322U,177U},
  {2U,32U,6634U,178U},
  {2U,36U,3594U,179U},
  {2U,1U,6102U,180U},
  {2U,38U,2726U,181U},
  {2U,19U,5310U,182U},
  {2U,48U,6099U,183U},
  {2U,1U,3383U,184U},
  {2U,7U,5128U,185U},
  {2U,13U,1682U,186U},
  {2U,38U,2080U,187U},
  {2U,43U,5506U,188U},
  {2U,40U,3388U,189U},
  {2U,4U,6298U,190U},
  {2U,1U,6772U,191U},
  {2U,1U,6120U,192U},
  {2U,46U,4423U,193U},
  {2U,7U,4053U,194U},
  {2U,19U,2470U,195U},
  {2U,13U,1104U,196U},
  {2U,62U,4898U,197U},
  {2U,26U,2271U,198U},
  {2U,27U,5951U,199U},
  {2U,41U,4237U,200U},
  {2U,20U,4709U,201U},
  {2U,23U,3118U,202U},
  {2U,23U,4170U,203U},
  {2U,42U,1252U,204U},
  {2U,58U,3559U,205U},
  {2U,44U,2463U,206U},
  {2U,46U,3031U,207U},
  {2U,63U,6012U,208U},
  {2U,62U,3428U,209U},
  {2U,54U,2861U,210U},
  {2U,55U,3396U,211U},
  {2U,12U,906U,212U},
  {2U,3U,1667U,213U},
  {2U,58U,7158U,214U},
  {2U,52U,5544U,215U},
  {2U,40U,5993U,216U},
  {2U,54U,7363U,217U},
  {2U,18U,7335U,218U},
  {2U,8U,6533U,219U},
  {2U,44U,3498U,220U},
  {2U,26U,2258U,221U},
  {2U,35U,8104U,222U},
  {2U,23U,4961U,223U},
  {2U,29U,6896U,224U},
  {2U,18U,2569U,225U},
  {2U,56U,6503U,226U},
  {2U,58U,5500U,227U},
  {2U,48U,1786U,228U},
  {2U,48U,5773U,229U},
  {2U,57U,4606U,230U},
  {2U,3U,3178U,231U},
  {2U,44U,1128U,232U},
  {2U,34U,4608U,233U},
  {2U,24U,4247U,234U},
  {2U,15U,7660U,235U},
  {2U,60U,6429U,236U},
  {2U,37U,4025U,237U},
  {2U,25U,7747U,238U},
  {2U,34U,363U,239U},
  {2U,57U,527U,240U},
  {2U,34U,4525U,241U},
  {2U,5U,6747U,242U},
  {2U,34U,2207U,243U},
  {2U,16U,889U,244U},
  {2U,5U,2190U,245U},
  {2U,10U,645U,246U},
  {2U,14U,469U,247U},
  {2U,54U,7260U,248U},
  {2U,48U,8090U,249U},
  {2U,46U,958U,250U},
  {2U,31U,3755U,251U},
  {2U,38U,1449U,252U},
  {2U,16U,2513U,253U},
  {2U,38U,7277U,254U},
  {2U,62U,3562U,255U},
  {2U,11U,65U,256U},
  {2U,58U,561U,257U},
  {2U,24U,5591U,258U},
  {2U,40U,6821U,259U},
  {2U,17U,5700U,260U},
  {2U,19U,1278U,261U},
  {2U,33U,7848U,262U},
  {2U,22U,3254U,263U},
  {2U,21U,1862U,264U},
  {2U,60U,1554U,265U},
  {2U,59U,5062U,266U},
  {2U,62U,8116U,267U},
  {2U,62U,2721U,268U},
  {2U,36U,6555U,269U},
  {2U,0U,6535U,270U},
  {2U,42U,4684U,271U},
  {2U,56U,6815U,272U},
  {2U,14U,1696U,273U},
  {2U,45U,640U,274U},
  {2U,41U,3785U,275U},
  {2U,37U,2218U,276U},
  {2U,39U,7967U,277U},
  {2U,29U,5390U,278U},
  {2U,63U,6855U,279U},
  {2U,55U,4814U,280U},
  {2U,17U,1480U,281U},
  {2U,23U,5671U,282U},
  {2U,33U,3155U,283U},
  {2U,52U,1903U,284U},
  {2U,47U,1727U,285U},
  {2U,61U,3833U,286U},
  {2U,11U,699U,287U},
  {2U,16U,325U,288U},
  {2U,59U,6452U,289U},
  {2U,26U,1065U,290U},
  {2U,21U,2489U,291U},
  {2U,27U,6200U,292U},
  {2U,63U,4109U,293U},
  {2U,3U,6894U,294U},
  {2U,16U,6263U,295U},
  {2U,5U,5918U,296U},
  {2U,52U,7092U,297U},
  {2U,33U,1275U,298U},
  {2U,20U,5813U,299U},
  {2U,22U,4157U,300U},
  {2U,24U,2345U,301U},
  {2U,35U,255U,302U},
  {2U,6U,832U,303U},
  {2U,49U,844U,304U},
  {2U,4U,4117U,305U},
  {2U,61U,7735U,306U},
  {2U,0U,2469U,307U},
  {2U,19U,3350U,308U},
  {2U,55U,6544U,309U},
  {2U,62U,6490U,310U},
  {2U,3U,7438U,311U},
  {2U,1U,721U,312U},
  {2U,61U,7032U,313U},
  {2U,28U,1149U,314U},
  {2U,33U,1178U,315U},
  {2U,10U,7673U,316U},
  {2U,2U,4346U,317U},
  {2U,13U,2059U,318U},
  {2U,12U,2103U,319U},
  {2U,13U,6196U,320U},
  {2U,15U,3530U,321U},
  {2U,43U,2091U,322U},
  {2U,43U,3060U,323U},
  {2U,41U,3623U,324U},
  {2U,4U,4677U,325U},
  {2U,63U,7045U,326U},
  {2U,28U,2992U,327U},
  {2U,59U,2125U,328U},
  {2U,11U,4132U,329U},
  {2U,57U,237U,330U},
  {2U,54U,1728U,331U},
  {2U,23U,2632U,332U},
  {2U,13U,696U,333U},
  {2U,58U,1272U,334U},
  {2U,59U,5955U,335U},
  {2U,54U,7651U,336U},
  {2U,39U,7763U,337U},
  {2U,4U,3460U,338U},
  {2U,40U,3193U,339U},
  {2U,9U,1664U,340U},
  {2U,9U,8134U,341U},
  {2U,43U,622U,342U},
  {2U,11U,5764U,343U},
  {2U,23U,7425U,344U},
  {2U,61U,3048U,345U},
  {2U,47U,3749U,346U},
  {2U,34U,3188U,347U},
  {2U,36U,7174U,348U},
  {2U,2U,1612U,349U},
  {2U,36U,6570U,350U},
  {2U,54U,5602U,351U},
  {2U,60U,2116U,352U},
  {2U,12U,5978U,353U},
  {2U,27U,1366U,354U},
  {2U,19U,3943U,355U},
  {2U,30U,5515U,356U},
  {2U,41U,3500U,357U},
  {2U,23U,2294U,358U},
  {2U,36U,3946U,359U},
  {2U,5U,6705U,360U},
  {2U,46U,6762U,361U},
  {2U,51U,766U,362U},
  {2U,16U,238U,363U},
  {2U,30U,4931U,364U},
  {2U,8U,4387U,365U},
  {2U,20U,1487U,366U},
  {2U,42U,783U,367U},
  {2U,56U,4780U,368U},
  {2U,55U,5879U,369U},
  {2U,53U,4321U,370U},
  {2U,54U,3814U,371U},
  {2U,59U,1213U,372U},
  {2U,18U,6256U,373U},
  {2U,45U,1645U,374U},
  {2U,24U,7943U,375U},
  {2U,50U,3859U,376U},
  {2U,19U,2710U,377U},
  {2U,39U,6505U,378U},
  {2U,49U,5759U,379U},
  {2U,0U,6914U,380U},
  {2U,15U,2553U,381U},
  {2U,52U,7909U,382U},
  {2U,51U,5265U,383U},
  {2U,49U,2362U,384U},
  {2U,3U,7819U,385U},
  {2U,34U,1997U,386U},
  {2U,31U,1989U,387U},
  {2U,16U,4398U,388U},
  {2U,9U,2466U,389U},
  {2U,29U,2007U,390U},
  {2U,36U,1481U,391U},
  {2U,60U,5399U,392U},
  {2U,9U,6331U,393U},
  {2U,1U,5462U,394U},
  {2U,51U,3920U,395U},
  {2U,31U,4825U,396U},
  {2U,30U,5347U,397U},
  {2U,48U,678U,398U},
  {2U,49U,346U,399U},
  {2U,50U,4995U,400U},
  {2U,56U,3719U,401U},
  {2U,63U,1860U,402U},
  {2U,41U,1650U,403U},
  {2U,6U,6682U,404U},
  {2U,34U,3195U,405U},
  {2U,57U,7646U,406U},
  {2U,26U,6026U,407U},
  {2U,27U,7635U,408U},
  {2U,49U,6936U,409U},
  {2U,9U,4765U,410U},
  {2U,44U,4988U,411U},
  {2U,34U,5813U,412U},
  {2U,63U,3488U,413U},
  {2U,25U,4618U,414U},
  {2U,33U,7371U,415U},
  {2U,36U,1506U,416U},
  {2U,2U,2210U,417U},
  {2U,14U,870U,418U},
  {2U,11U,4254U,419U},
  {2U,31U,3271U,420U},
  {2U,59U,6566U,421U},
  {2U,60U,3069U,422U},
  {2U,54U,5918U,423U},
  {2U,31U,1883U,424U},
  {2U,59U,1220U,425U},
  {2U,6U,6678U,426U},
  {2U,56U,2475U,427U},
  {2U,15U,5146U,428U},
  {2U,19U,3248U,429U},
  {2U,14U,7326U,430U},
  {2U,40U,1106U,431U},
  {2U,3U,7328U,432U},
  {2U,22U,6288U,433U},
  {2U,46U,860U,434U},
  {2U,25U,113U,435U},
  {2U,61U,1783U,436U},
  {2U,7U,6278U,437U},
  {2U,5U,7761U,438U},
  {2U,52U,1329U,439U},
  {2U,30U,1687U,440U},
  {2U,31U,3668U,441U},
  {2U,26U,4122U,442U},
  {2U,16U,2555U,443U},
  {2U,8U,5938U,444U},
  {2U,31U,2449U,445U},
  {2U,7U,7548U,446U},
  {2U,33U,1066U,447U},
  {2U,24U,3771U,448U},
  {2U,28U,7816U,449U},
  {2U,37U,1072U,450U},
  {2U,23U,6281U,451U},
  {2U,25U,535U,452U},
  {2U,33U,3577U,453U},
  {2U,16U,853U,454U},
  {2U,19U,3205U,455U},
  {2U,45U,7048U,456U},
  {2U,36U,3566U,457U},
  {2U,46U,7539U,458U},
  {2U,16U,3441U,459U},
  {2U,60U,4526U,460U},
  {2U,37U,24U,461U},
  {2U,63U,6921U,462U},
  {2U,32U,6744U,463U},
  {2U,56U,6350U,464U},
  {2U,30U,3495U,465U},
  {2U,4U,5104U,466U},
  {2U,23U,1825U,467U},
  {2U,11U,2049U,468U},
  {2U,43U,3675U,469U},
  {2U,27U,8040U,470U},
  {2U,38U,7407U,471U},
  {2U,53U,4807U,472U},
  {2U,37U,6235U,473U},
  {2U,9U,2890U,474U},
  {2U,9U,6919U,475U},
  {2U,1U,5222U,476U},
  {2U,61U,1490U,477U},
  {2U,33U,7070U,478U},
  {2U,1U,8059U,479U},
  {2U,34U,81U,480U},
  {2U,18U,975U,481U},
  {2U,33U,7088U,482U},
  {2U,56U,1600U,483U},
  {2U,43U,3910U,484U},
  {2U,47U,7411U,485U},
  {2U,29U,8106U,486U},
  {2U,45U,2385U,487U},
  {2U,7U,3642U,488U},
  {2U,25U,52U,489U},
  {2U,58U,5805U,490U},
  {2U,29U,805U,491U},
  {2U,4U,2113U,492U},
  {2U,41U,6468U,493U},
  {2U,3U,7163U,494U},
  {2U,18U,2426U,495U},
  {2U,34U,1510U,496U},
  {2U,42U,5779U,497U},
  {2U,58U,6726U,498U},
  {2U,23U,554U,499U},
  {2U,22U,6888U,500U},
  {2U,17U,6781U,501U},
  {2U,45U,7764U,502U},
  {2U,27U,5303U,503U},
  {2U,44U,8056U,504U},
  {2U,30U,1430U,505U},
  {2U,46U,5250U,506U},
  {2U,61U,5889U,507U},
  {2U,10U,684U,508U},
  {2U,17U,760U,509U},
  {2U,19U,620U,510U},
  {2U,1U,4391U,511U},
  {2U,40U,2089U,512U},
  {2U,8U,5601U,513U},
  {2U,19U,4738U,514U},
  {2U,43U,5288U,515U},
  {2U,34U,6653U,516U},
  {2U,41U,4936U,517U},
  {2U,16U,6645U,518U},
  {2U,26U,5449U,519U},
  {2U,49U,95U,520U},
  {2U,51U,2873U,521U},
  {2U,14U,2341U,522U},
  {2U,4U,4105U,523U},
  {2U,2U,4571U,524U},
  {2U,21U,7241U,525U},
  {2U,19U,3720U,526U},
  {2U,57U,1786U,527U},
  {2U,46U,1160U,528U},
  {2U,49U,7471U,529U},
  {2U,14U,6746U,530U},
  {2U,36U,676U,531U},
  {2U,57U,6050U,532U},
  {2U,30U,430U,533U},
  {2U,54U,2366U,534U},
  {2U,9U,6151U,535U},
  {2U,35U,5923U,536U},
  {2U,3U,179U,537U},
  {2U,37U,4485U,538U},
  {2U,48U,936U,539U},
  {2U,13U,1904U,540U},
  {2U,43U,707U,541U},
  {2U,61U,4151U,542U},
  {2U,10U,7053U,543U},
  {2U,20U,658U,544U},
  {2U,1U,5069U,545U},
  {2U,4U,7074U,546U},
  {2U,52U,520U,547U},
  {2U,24U,4712U,548U},
  {2U,40U,6969U,549U},
  {2U,41U,5118U,550U},
  {2U,10U,2073U,551U},
  {2U,38U,2125U,552U},
  {2U,16U,1648U,553U},
  {2U,22U,509U,554U},
  {2U,25U,5226U,555U},
  {2U,60U,5552U,556U},
  {2U,50U,171U,557U},
  {2U,47U,2523U,558U},
  {2U,9U,4418U,559U},
  {2U,19U,1256U,560U},
  {2U,39U,4252U,561U},
  {2U,10U,7156U,562U},
  {2U,31U,4284U,563U},
  {2U,23U,4568U,564U},
  {2U,54U,1495U,565U},
  {2U,8U,7678U,566U},
  {2U,14U,4083U,567U},
  {2U,50U,5032U,568U},
  {2U,5U,3731U,569U},
  {2U,50U,575U,570U},
  {2U,22U,1320U,571U},
  {2U,63U,5887U,572U},
  {2U,53U,3649U,573U},
  {2U,61U,7551U,574U},
  {2U,13U,1384U,575U},
  {2U,0U,3889U,576U},
  {2U,18U,7263U,577U},
  {2U,47U,879U,578U},
  {2U,44U,2029U,579U},
  {2U,51U,7698U,580U},
  {2U,23U,4743U,581U},
  {2U,55U,3093U,582U},
  {2U,13U,2099U,583U},
  {2U,2U,2575U,584U},
  {2U,57U,36U,585U},
  {2U,13U,6455U,586U},
  {2U,24U,1374U,587U},
  {2U,28U,2221U,588U},
  {2U,19U,3956U,589U},
  {2U,9U,4300U,590U},
  {2U,1U,816U,591U},
  {2U,0U,2816U,592U},
  {2U,46U,554U,593U},
  {2U,29U,344U,594U},
  {2U,9U,5334U,595U},
  {2U,20U,3418U,596U},
  {2U,52U,5869U,597U},
  {2U,4U,6886U,598U},
  {2U,20U,8083U,599U},
  {2U,0U,2910U,600U},
  {2U,39U,2393U,601U},
  {2U,2U,7208U,602U},
  {2U,28U,1273U,603U},
  {2U,37U,5002U,604U},
  {2U,7U,7343U,605U},
  {2U,36U,842U,606U},
  {2U,33U,3059U,607U},
  {2U,52U,3450U,608U},
  {2U,12U,3982U,609U},
  {2U,13U,5209U,610U},
  {2U,58U,1744U,611U},
  {2U,35U,2912U,612U},
  {2U,62U,776U,613U},
  {2U,46U,4284U,614U},
  {2U,1U,808U,615U},
  {2U,27U,5195U,616U},
  {2U,56U,5993U,617U},
  {2U,47U,5733U,618U},
  {2U,41U,5011U,619U},
  {2U,29U,6303U,620U},
  {2U,37U,3186U,621U},
  {2U,53U,288U,622U},
  {2U,32U,3319U,623U},
  {2U,54U,111U,624U},
  {2U,2U,7897U,625U},
  {2U,30U,7331U,626U},
  {2U,7U,386U,627U},
  {2U,47U,1672U,628U},
  {2U,0U,4275U,629U},
  {2U,22U,3866U,630U},
  {2U,59U,7217U,631U},
  {2U,20U,7690U,632U},
  {2U,19U,4594U,633U},
  {2U,28U,5370U,634U},
  {2U,20U,6019U,635U},
  {2U,57U,5923U,636U},
  {2U,54U,7455U,637U},
  {2U,42U,146U,638U},
  {2U,46U,6790U,639U},
  {2U,3U,2448U,640U},
  {0U,0U,18U,0U},
  {0U,1U,7872U,0U},
  {0U,2U,1860U,0U},
  {0U,3U,7664U,0U},
  {0U,4U,6940U,0U},
  {0U,5U,793U,0U},
  {0U,6U,1238U,0U},
  {0U,7U,1018U,0U},
  {0U,8U,1641U,0U},
  {0U,9U,8099U,0U},
  {0U,10U,7189U,0U},
  {0U,11U,7752U,0U},
  {0U,12U,5831U,0U},
  {0U,13U,31U,0U},
  {0U,14U,5506U,0U},
  {0U,15U,3860U,0U},
  {0U,16U,5976U,0U},
  {0U,17U,485U,0U},
  {0U,18U,6318U,0U},
  {0U,19U,8143U,0U},
  {0U,20U,7733U,0U},
  {0U,21U,2158U,0U},
  {0U,22U,273U,0U},
  {0U,23U,1496U,0U},
  {0U,24U,3328U,0U},
  {0U,25U,4645U,0U},
  {0U,26U,4427U,0U},
  {0U,27U,4268U,0U},
  {0U,28U,5892U,0U},
  {0U,29U,915U,0U},
  {0U,30U,7950U,0U},
  {0U,31U,6558U,0U},
  {0U,32U,6871U,0U},
  {0U,33U,5937U,0U},
  {0U,34U,7584U,0U},
  {0U,35U,790U,0U},
  {0U,36U,3180U,0U},
  {0U,37U,4755U,0U},
  {0U,38U,2186U,0U},
  {0U,39U,5646U,0U},
  {0U,40U,5574U,0U},
  {0U,41U,2467U,0U},
  {0U,42U,3801U,0U},
  {0U,43U,7596U,0U},
  {0U,44U,4862U,0U},
  {0U,45U,6479U,0U},
  {0U,46U,5872U,0U},
  {0U,47U,5950U,0U},
  {0U,48U,1563U,0U},
  {0U,49U,3450U,0U},
  {0U,50U,1256U,0U},
  {0U,51U,626U,0U},
  {0U,52U,8181U,0U},
  {0U,53U,3955U,0U},
  {0U,54U,2500U,0U},
  {0U,55U,2293U,0U},
  {0U,56U,19U,0U},
  {0U,57U,4999U,0U},
  {0U,58U,8047U,0U},
  {0U,59U,5529U,0U},
  {0U,60U,835U,0U},
  {0U,61U,5953U,0U},
  {0U,62U,4473U,0U},
  {0U,63U,7703U,0U},
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
  {2U,38U,6674U,129U},
  {2U,48U,2909U,130U},
  {2U,2U,5965U,131U},
  {2U,6U,2384U,132U},
  {2U,58U,2309U,133U},
  {2U,11U,7443U,134U},
  {2U,22U,3078U,135U},
  {2U,29U,995U,136U},
  {2U,3U,2485U,137U},
  {2U,46U,1849U,138U},
  {2U,25U,1171U,139U},
  {2U,40U,83U,140U},
  {2U,22U,3237U,141U},
  {2U,55U,3409U,142U},
  {2U,29U,6746U,143U},
  {2U,4U,6902U,144U},
  {2U,0U,6243U,145U},
  {2U,11U,6402U,146U},
  {2U,12U,1801U,147U},
  {2U,47U,4846U,148U},
  {2U,3U,4316U,149U},
  {2U,41U,1562U,150U},
  {2U,43U,381U,151U},
  {2U,44U,4974U,152U},
  {2U,55U,6659U,153U},
  {2U,62U,4413U,154U},
  {2U,18U,4353U,155U},
  {2U,23U,4134U,156U},
  {2U,23U,2898U,157U},
  {2U,1U,1405U,158U},
  {2U,30U,7790U,159U},
  {2U,26U,2700U,160U},
  {2U,12U,5336U,161U},
  {2U,48U,8037U,162U},
  {2U,18U,920U,163U},
  {2U,55U,2107U,164U},
  {2U,15U,2639U,165U},
  {2U,39U,831U,166U},
  {2U,10U,6792U,167U},
  {2U,59U,5901U,168U},
  {2U,50U,1210U,169U},
  {2U,4U,79U,170U},
  {2U,47U,2801U,171U},
  {2U,35U,284U,172U},
  {2U,33U,4300U,173U},
  {2U,15U,6254U,174U},
  {2U,43U,3031U,175U},
  {2U,56U,3129U,176U},
  {2U,42U,7977U,177U},
  {2U,39U,6917U,178U},
  {2U,14U,4641U,179U},
  {2U,16U,2747U,180U},
  {2U,35U,5161U,181U},
  {2U,10U,394U,182U},
  {2U,14U,5655U,183U},
  {2U,20U,2127U,184U},
  {2U,39U,275U,185U},
  {2U,11U,5531U,186U},
  {2U,12U,1196U,187U},
  {2U,34U,7467U,188U},
  {2U,52U,1717U,189U},
  {2U,7U,7225U,190U},
  {2U,61U,5427U,191U},
  {2U,29U,1080U,192U},
  {2U,47U,2564U,193U},
  {2U,50U,257U,194U},
  {2U,30U,5498U,195U},
  {2U,15U,8034U,196U},
  {2U,41U,7036U,197U},
  {2U,59U,1634U,198U},
  {2U,21U,3379U,199U},
  {2U,29U,6293U,200U},
  {2U,20U,531U,201U},
  {2U,61U,6837U,202U},
  {2U,2U,5291U,203U},
  {2U,20U,7693U,204U},
  {2U,59U,7850U,205U},
  {2U,23U,6096U,206U},
  {2U,29U,5238U,207U},
  {2U,38U,2612U,208U},
  {2U,56U,1570U,209U},
  {2U,57U,3180U,210U},
  {2U,58U,1583U,211U},
  {2U,49U,2094U,212U},
  {2U,62U,8149U,213U},
  {2U,36U,523U,214U},
  {2U,48U,3404U,215U},
  {2U,12U,5434U,216U},
  {2U,9U,4819U,217U},
  {2U,40U,1272U,218U},
  {2U,25U,1692U,219U},
  {2U,57U,1386U,220U},
  {2U,7U,7680U,221U},
  {2U,11U,1812U,222U},
  {2U,15U,5564U,223U},
  {2U,25U,314U,224U},
  {2U,29U,5296U,225U},
  {2U,33U,2499U,226U},
  {2U,63U,327U,227U},
  {2U,5U,3635U,228U},
  {2U,19U,2376U,229U},
  {2U,25U,2776U,230U},
  {2U,50U,4757U,231U},
  {2U,35U,1111U,232U},
  {2U,20U,4146U,233U},
  {2U,43U,7222U,234U},
  {2U,0U,6711U,235U},
  {2U,37U,7749U,236U},
  {2U,28U,7924U,237U},
  {2U,2U,4125U,238U},
  {2U,50U,4398U,239U},
  {2U,52U,3223U,240U},
  {2U,39U,3385U,241U},
  {2U,10U,1812U,242U},
  {2U,25U,6101U,243U},
  {2U,49U,1257U,244U},
  {2U,44U,2825U,245U},
  {2U,43U,3237U,246U},
  {2U,25U,5794U,247U},
  {2U,7U,3910U,248U},
  {2U,15U,642U,249U},
  {2U,7U,6122U,250U},
  {2U,54U,2703U,251U},
  {2U,44U,597U,252U},
  {2U,39U,2311U,253U},
  {2U,34U,3391U,254U},
  {2U,20U,6979U,255U},
  {2U,27U,4370U,256U},
  {2U,3U,2165U,257U},
  {2U,61U,3912U,258U},
  {2U,61U,4863U,259U},
  {2U,63U,7060U,260U},
  {2U,22U,5490U,261U},
  {2U,4U,2287U,262U},
  {2U,52U,2502U,263U},
  {2U,42U,3104U,264U},
  {2U,31U,3057U,265U},
  {2U,24U,2263U,266U},
  {2U,10U,1939U,267U},
  {2U,62U,436U,268U},
  {2U,48U,4915U,269U},
  {2U,55U,4260U,270U},
  {2U,47U,5864U,271U},
  {2U,14U,6319U,272U},
  {2U,32U,2928U,273U},
  {2U,53U,5553U,274U},
  {2U,13U,2842U,275U},
  {2U,17U,241U,276U},
  {2U,63U,6442U,277U},
  {2U,18U,5174U,278U},
  {2U,31U,7986U,279U},
  {2U,34U,6566U,280U},
  {2U,28U,2867U,281U},
  {2U,20U,3143U,282U},
  {2U,9U,3347U,283U},
  {2U,26U,899U,284U},
  {2U,17U,6445U,285U},
  {2U,45U,1971U,286U},
  {2U,18U,367U,287U},
  {2U,25U,5948U,288U},
  {2U,31U,2185U,289U},
  {2U,47U,4080U,290U},
  {2U,3U,1456U,291U},
  {2U,53U,4818U,292U},
  {2U,8U,6047U,293U},
  {2U,15U,6698U,294U},
  {2U,63U,58U,295U},
  {2U,14U,3143U,296U},
  {2U,40U,1274U,297U},
  {2U,42U,5956U,298U},
  {2U,48U,262U,299U},
  {2U,45U,5080U,300U},
  {2U,34U,2204U,301U},
  {2U,58U,6267U,302U},
  {2U,33U,884U,303U},
  {2U,19U,1670U,304U},
  {2U,45U,2379U,305U},
  {2U,44U,7851U,306U},
  {2U,11U,6263U,307U},
  {2U,49U,1010U,308U},
  {2U,24U,1795U,309U},
  {2U,56U,7061U,310U},
  {2U,0U,419U,311U},
  {2U,2U,708U,312U},
  {2U,32U,1699U,313U},
  {2U,54U,7424U,314U},
  {2U,49U,6613U,315U},
  {2U,32U,965U,316U},
  {2U,58U,1160U,317U},
  {2U,39U,6244U,318U},
  {2U,55U,7894U,319U},
  {2U,30U,7682U,320U},
  {2U,11U,3579U,321U},
  {2U,16U,6289U,322U},
  {2U,1U,1587U,323U},
  {2U,1U,555U,324U},
  {2U,50U,6801U,325U},
  {2U,44U,459U,326U},
  {2U,29U,5760U,327U},
  {2U,26U,1544U,328U},
  {2U,48U,969U,329U},
  {2U,3U,2185U,330U},
  {2U,13U,7110U,331U},
  {2U,58U,3848U,332U},
  {2U,2U,6924U,333U},
  {2U,61U,1883U,334U},
  {2U,53U,6810U,335U},
  {2U,9U,3842U,336U},
  {2U,51U,2806U,337U},
  {2U,32U,4078U,338U},
  {2U,56U,6185U,339U},
  {2U,19U,1668U,340U},
  {2U,51U,2794U,341U},
  {2U,60U,2915U,342U},
  {2U,9U,7108U,343U},
  {2U,10U,5798U,344U},
  {2U,60U,6610U,345U},
  {2U,39U,1617U,346U},
  {2U,6U,7442U,347U},
  {2U,13U,1995U,348U},
  {2U,11U,6730U,349U},
  {2U,17U,2804U,350U},
  {2U,1U,2237U,351U},
  {2U,55U,5229U,352U},
  {2U,20U,6775U,353U},
  {2U,13U,6828U,354U},
  {2U,34U,4100U,355U},
  {2U,18U,2725U,356U},
  {2U,23U,3277U,357U},
  {2U,22U,4505U,358U},
  {2U,21U,2799U,359U},
  {2U,39U,3288U,360U},
  {2U,29U,4152U,361U},
  {2U,9U,1790U,362U},
  {2U,30U,4625U,363U},
  {2U,34U,7825U,364U},
  {2U,3U,594U,365U},
  {2U,14U,4667U,366U},
  {2U,23U,8039U,367U},
  {2U,59U,5741U,368U},
  {2U,9U,3624U,369U},
  {2U,10U,6931U,370U},
  {2U,23U,4257U,371U},
  {2U,37U,4064U,372U},
  {2U,24U,6852U,373U},
  {2U,18U,6512U,374U},
  {2U,11U,3030U,375U},
  {2U,14U,6151U,376U},
  {2U,42U,783U,377U},
  {2U,31U,6297U,378U},
  {2U,41U,5920U,379U},
  {2U,21U,7986U,380U},
  {2U,25U,6841U,381U},
  {2U,52U,1483U,382U},
  {2U,30U,6596U,383U},
  {2U,16U,7702U,384U},
  {2U,8U,2684U,385U},
  {2U,44U,538U,386U},
  {2U,3U,2222U,387U},
  {2U,10U,6112U,388U},
  {2U,48U,5230U,389U},
  {2U,17U,6532U,390U},
  {2U,62U,3071U,391U},
  {2U,40U,4436U,392U},
  {2U,27U,2755U,393U},
  {2U,62U,7784U,394U},
  {2U,41U,885U,395U},
  {2U,48U,848U,396U},
  {2U,19U,3336U,397U},
  {2U,11U,1499U,398U},
  {2U,32U,6042U,399U},
  {2U,22U,7243U,400U},
  {2U,56U,4618U,401U},
  {2U,6U,3730U,402U},
  {2U,62U,3577U,403U},
  {2U,38U,439U,404U},
  {2U,39U,4182U,405U},
  {2U,14U,1867U,406U},
  {2U,34U,7705U,407U},
  {2U,5U,6874U,408U},
  {2U,8U,2273U,409U},
  {2U,8U,43U,410U},
  {2U,49U,2115U,411U},
  {2U,18U,6060U,412U},
  {2U,37U,4598U,413U},
  {2U,22U,2422U,414U},
  {2U,6U,6606U,415U},
  {2U,56U,3668U,416U},
  {2U,34U,3896U,417U},
  {2U,62U,7578U,418U},
  {2U,28U,3305U,419U},
  {2U,44U,419U,420U},
  {2U,54U,4505U,421U},
  {2U,32U,1455U,422U},
  {2U,58U,6395U,423U},
  {2U,10U,3433U,424U},
  {2U,25U,7814U,425U},
  {2U,47U,6550U,426U},
  {2U,30U,1863U,427U},
  {2U,56U,2963U,428U},
  {2U,38U,3005U,429U},
  {2U,0U,1702U,430U},
  {2U,9U,2956U,431U},
  {2U,24U,743U,432U},
  {2U,19U,4742U,433U},
  {2U,23U,6906U,434U},
  {2U,55U,8097U,435U},
  {2U,32U,2345U,436U},
  {2U,58U,5790U,437U},
  {2U,22U,7955U,438U},
  {2U,32U,964U,439U},
  {2U,56U,7864U,440U},
  {2U,49U,3330U,441U},
  {2U,59U,884U,442U},
  {2U,34U,812U,443U},
  {2U,42U,1309U,444U},
  {2U,29U,7623U,445U},
  {2U,19U,2589U,446U},
  {2U,1U,2127U,447U},
  {2U,12U,1530U,448U},
  {2U,55U,4438U,449U},
  {2U,6U,6102U,450U},
  {2U,21U,533U,451U},
  {2U,38U,69U,452U},
  {2U,11U,7741U,453U},
  {2U,48U,2631U,454U},
  {2U,14U,7162U,455U},
  {2U,26U,5398U,456U},
  {2U,15U,178U,457U},
  {2U,41U,7439U,458U},
  {2U,13U,384U,459U},
  {2U,26U,7205U,460U},
  {2U,61U,1648U,461U},
  {2U,2U,792U,462U},
  {2U,60U,3704U,463U},
  {2U,54U,235U,464U},
  {2U,46U,4399U,465U},
  {2U,52U,634U,466U},
  {2U,49U,5018U,467U},
  {2U,18U,175U,468U},
  {2U,63U,1520U,469U},
  {2U,15U,4000U,470U},
  {2U,54U,911U,471U},
  {2U,19U,6027U,472U},
  {2U,58U,7695U,473U},
  {2U,24U,6964U,474U},
  {2U,4U,630U,475U},
  {2U,46U,6040U,476U},
  {2U,5U,4823U,477U},
  {2U,17U,5551U,478U},
  {2U,29U,5484U,479U},
  {2U,21U,8U,480U},
  {2U,49U,1663U,481U},
  {2U,7U,2938U,482U},
  {2U,39U,7677U,483U},
  {2U,37U,567U,484U},
  {2U,27U,2246U,485U},
  {2U,19U,7968U,486U},
  {2U,19U,497U,487U},
  {2U,57U,8025U,488U},
  {2U,39U,6195U,489U},
  {2U,3U,3320U,490U},
  {2U,45U,1838U,491U},
  {2U,1U,7485U,492U},
  {2U,62U,1408U,493U},
  {2U,59U,2203U,494U},
  {2U,11U,1663U,495U},
  {2U,57U,7571U,496U},
  {2U,27U,7501U,497U},
  {2U,40U,7619U,498U},
  {2U,34U,4950U,499U},
  {2U,28U,727U,500U},
  {2U,36U,5927U,501U},
  {2U,31U,2543U,502U},
  {2U,5U,5366U,503U},
  {2U,16U,2193U,504U},
  {2U,47U,1695U,505U},
  {2U,55U,1686U,506U},
  {2U,42U,4103U,507U},
  {2U,22U,1524U,508U},
  {2U,54U,714U,509U},
  {2U,35U,707U,510U},
  {2U,57U,3857U,511U},
  {2U,31U,3207U,512U},
  {2U,63U,6446U,513U},
  {2U,6U,2027U,514U},
  {2U,22U,5869U,515U},
  {2U,4U,3567U,516U},
  {2U,49U,1662U,517U},
  {2U,11U,2148U,518U},
  {2U,27U,6332U,519U},
  {2U,21U,1834U,520U},
  {2U,6U,2138U,521U},
  {2U,43U,5728U,522U},
  {2U,52U,3189U,523U},
  {2U,5U,3472U,524U},
  {2U,7U,258U,525U},
  {2U,22U,5224U,526U},
  {2U,36U,342U,527U},
  {2U,54U,4348U,528U},
  {2U,13U,4674U,529U},
  {2U,48U,2165U,530U},
  {2U,56U,4623U,531U},
  {2U,11U,6118U,532U},
  {2U,63U,2125U,533U},
  {2U,48U,4812U,534U},
  {2U,23U,1595U,535U},
  {2U,30U,1019U,536U},
  {2U,53U,1866U,537U},
  {2U,8U,4525U,538U},
  {2U,21U,3149U,539U},
  {2U,40U,4527U,540U},
  {2U,36U,4102U,541U},
  {2U,15U,4920U,542U},
  {2U,23U,7770U,543U},
  {2U,33U,2777U,544U},
  {2U,5U,1316U,545U},
  {2U,35U,7447U,546U},
  {2U,27U,1577U,547U},
  {2U,36U,2699U,548U},
  {2U,36U,199U,549U},
  {2U,44U,7908U,550U},
  {2U,37U,2711U,551U},
  {2U,41U,1795U,552U},
  {2U,8U,4620U,553U},
  {2U,51U,2222U,554U},
  {2U,59U,1585U,555U},
  {2U,46U,641U,556U},
  {2U,57U,4828U,557U},
  {2U,7U,2033U,558U},
  {2U,61U,5839U,559U},
  {2U,14U,2447U,560U},
  {2U,52U,2220U,561U},
  {2U,6U,2093U,562U},
  {2U,20U,7992U,563U},
  {2U,20U,7311U,564U},
  {2U,41U,6775U,565U},
  {2U,14U,3274U,566U},
  {2U,16U,5595U,567U},
  {2U,48U,3597U,568U},
  {2U,57U,648U,569U},
  {2U,36U,2553U,570U},
  {2U,33U,5938U,571U},
  {2U,27U,5141U,572U},
  {2U,27U,7920U,573U},
  {2U,55U,391U,574U},
  {2U,22U,5596U,575U},
  {2U,26U,3518U,576U},
  {2U,57U,964U,577U},
  {2U,27U,4515U,578U},
  {2U,62U,8122U,579U},
  {2U,14U,3003U,580U},
  {2U,23U,3029U,581U},
  {2U,0U,6183U,582U},
  {2U,12U,3220U,583U},
  {2U,26U,4118U,584U},
  {2U,32U,3185U,585U},
  {2U,5U,1924U,586U},
  {2U,60U,456U,587U},
  {2U,59U,1824U,588U},
  {2U,11U,7725U,589U},
  {2U,22U,694U,590U},
  {2U,21U,3356U,591U},
  {2U,39U,7147U,592U},
  {2U,26U,5178U,593U},
  {2U,35U,5638U,594U},
  {2U,4U,4702U,595U},
  {2U,58U,4277U,596U},
  {2U,51U,4751U,597U},
  {2U,34U,794U,598U},
  {2U,17U,7044U,599U},
  {2U,9U,1034U,600U},
  {2U,29U,2057U,601U},
  {2U,9U,3167U,602U},
  {2U,12U,1615U,603U},
  {2U,4U,5950U,604U},
  {2U,8U,337U,605U},
  {2U,22U,6114U,606U},
  {2U,55U,5957U,607U},
  {2U,4U,5584U,608U},
  {2U,37U,5512U,609U},
  {2U,37U,841U,610U},
  {2U,7U,4483U,611U},
  {2U,13U,1533U,612U},
  {2U,3U,3554U,613U},
  {2U,41U,6357U,614U},
  {2U,48U,4963U,615U},
  {2U,1U,1583U,616U},
  {2U,19U,5136U,617U},
  {2U,3U,4922U,618U},
  {2U,29U,3669U,619U},
  {2U,51U,219U,620U},
  {2U,56U,6381U,621U},
  {2U,16U,6467U,622U},
  {2U,1U,4468U,623U},
  {2U,44U,7434U,624U},
  {2U,7U,3966U,625U},
  {2U,41U,3422U,626U},
  {2U,42U,3989U,627U},
  {2U,4U,1203U,628U},
  {2U,60U,4790U,629U},
  {2U,63U,7705U,630U},
  {2U,20U,4428U,631U},
  {2U,40U,1430U,632U},
  {2U,19U,6751U,633U},
  {2U,48U,2253U,634U},
  {2U,39U,3109U,635U},
  {2U,12U,1315U,636U},
  {2U,50U,1720U,637U},
  {2U,44U,7022U,638U},
  {2U,12U,6502U,639U},
  {2U,29U,6281U,640U},
};
static const unsigned paper_offsets[5] = {0U,640U,1280U,1920U,2560U};
static const unsigned paper_work_starts[4] = {384U,1024U,1664U,2304U};
static const unsigned paper_expected_sums[4] = {37395U,37154U,37407U,36900U};
static const unsigned paper_expected_live[4] = {282233U,256745U,277598U,237279U};

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

