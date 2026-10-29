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

struct ThreadCache;
struct ThreadCacheNode;

struct ThreadCache {
  struct BuddyAllocator* buddyAllocator;
  int sizeClass;
  int pageSize;
  __mram_ptr struct ThreadCacheNode* root;
};

struct ThreadCacheNode {
  // NOTE(bongjoon.hyun@gmail.com): neighbors are prev and next
  __mram_ptr struct ThreadCacheNode* prev;
  __mram_ptr struct ThreadCacheNode* next;

  __mram_ptr int* metadata;

  // NOTE(bongjoon.hyun@gmail.com): data offset is here
};

struct ThreadCache* initThreadCache(struct BuddyAllocator* buddyAllocator,
                                    int sizeClass, int pageSize);
void finiThreadCache(struct ThreadCache* threadCache);

__mram_ptr void* allocateThreadCache(struct ThreadCache* threadCache);
int freeThreadCache(struct ThreadCache* threadCache, __mram_ptr void* pointer);

void initThreadCacheNode(struct ThreadCache* threadCache,
                         __mram_ptr struct ThreadCacheNode* node,
                         __mram_ptr struct ThreadCacheNode* prev);
void finiThreadCacheNode(struct ThreadCache* threadCache,
                         __mram_ptr struct ThreadCacheNode* node);

__mram_ptr void* allocateThreadCacheNode(
    struct ThreadCache* threadCache, __mram_ptr struct ThreadCacheNode* node);
int freeThreadCacheNode(struct ThreadCache* threadCache,
                        __mram_ptr struct ThreadCacheNode* node,
                        __mram_ptr void* pointer);

int getThreadCacheNumChunks(struct ThreadCache* threadCache);

int getThreadCacheMetadata(struct ThreadCache* threadCache,
                           __mram_ptr struct ThreadCacheNode* node,
                           int chunkIndex);
int setThreadCacheMetadata(struct ThreadCache* threadCache,
                           __mram_ptr struct ThreadCacheNode* node,
                           int chunkIndex, int value);

__mram_ptr void* getThreadCachePointer(struct ThreadCache* threadCache,
                                       __mram_ptr struct ThreadCacheNode* node,
                                       int chunkIndex);

int getThreadCacheChunkIndex(struct ThreadCache* threadCache,
                             __mram_ptr struct ThreadCacheNode* node,
                             __mram_ptr void* pointer);

struct UMalloc {
  struct BuddyAllocator *buddyAllocator;

  int *sizeClasses;
  int sizeClassesLength;

  int pageSize;

  struct ThreadCache **threadCaches0;
  struct ThreadCache **threadCaches1;
  struct ThreadCache **threadCaches2;
  struct ThreadCache **threadCaches3;
  struct ThreadCache **threadCaches4;
  struct ThreadCache **threadCaches5;
  struct ThreadCache **threadCaches6;
  struct ThreadCache **threadCaches7;
  struct ThreadCache **threadCaches8;
  struct ThreadCache **threadCaches9;
  struct ThreadCache **threadCaches10;
  struct ThreadCache **threadCaches11;
  struct ThreadCache **threadCaches12;
  struct ThreadCache **threadCaches13;
  struct ThreadCache **threadCaches14;
  struct ThreadCache **threadCaches15;
};

struct UMalloc *initUMalloc(int mallocableRegionSize, int pageSize,
                            int bufferSize, __mram_ptr void *mramBase,
                            int *sizeClasses, int sizeClassesLength);
void finiUMalloc(struct UMalloc *uMalloc);

__mram_ptr void *allocateUMalloc(struct UMalloc *uMalloc, int size);
void freeUMalloc(struct UMalloc *uMalloc, __mram_ptr void *pointer);

struct ThreadCache *getThreadCache(struct UMalloc *uMalloc, int size);

__host int mallocableRegionSize;
__host int pageSize;
__host int bufferSize;

struct UMalloc *uMalloc;

MUTEX_INIT(buddyAllocatorMutex);

BARRIER_INIT(deviceBarrier, NR_TASKLETS);


#define REPRO_SIZE 32
#define REPRO_REQUESTS 2
struct BuddyAllocator *reproBuddy;
volatile unsigned int reproChecksum[NR_TASKLETS];
int main() {
  int threadId = me();
  if (threadId == 0) {
    mem_reset();
    
    int sizeClasses[8] = {16, 32, 64, 128, 256, 512, 1024, 2048};
    uMalloc = initUMalloc(mallocableRegionSize, pageSize, bufferSize,
                         DPU_MRAM_HEAP_POINTER, sizeClasses, 8);

  }
  barrier_wait(&deviceBarrier);
  #pragma clang loop unroll(disable)
  for (unsigned int i = 0; i < REPRO_REQUESTS; i++) {
    __asm__ volatile("repro_begin_%=:" ::: "memory");
    __mram_ptr void *pointer = allocateUMalloc(uMalloc, REPRO_SIZE);
    __asm__ volatile("repro_end_%=:" ::: "memory");
    assert(pointer != NULL);
    reproChecksum[threadId] ^= (unsigned int)pointer;
  }
  return 0;
}

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

__mram_ptr void *safeBuddyAllocate(struct BuddyAllocator *buddyAllocator,
                                   int size) {
  __mram_ptr void *pointer = buddyAllocateForward(buddyAllocator, 0, 0, size);

  assert(pointer != NULL);

  return pointer;
}

void safeBuddyFree(struct BuddyAllocator *buddyAllocator,
                   __mram_ptr void *pointer) {
  buddyFreeBackward(buddyAllocator, buddyAllocator->numBuddyLevels - 1,
                    pointer);
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
  return metadataSize;
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
                                   (0b11 << metadataShift))) >>
                   metadataShift;

    return metadata;
  } else {
    flushMetadata(buddyAllocator);

    // NOTE(bongjoon.hyun@gmail.com): MRAM address is assumed to be aligned with
    // 8
    int mramAddress = getMramAddress(buddyAllocator, level, leafIndex);
    int i = 1;
    while(mramAddress % 8 != 0) {
        mramAddress = getMramAddress(buddyAllocator, level, leafIndex - i);
        i++;
    }

    int dmaTransferSize;
    if (mramAddress + buddyAllocator->bufferSize < buddyAllocator->mramMetadataBase + buddyAllocator->sumMetadataSizePerLevels[buddyAllocator->numBuddyLevels]) {
      dmaTransferSize = buddyAllocator->bufferSize;
    } else {
      dmaTransferSize = (__mram_ptr void*) buddyAllocator->mramMetadataBase - (__mram_ptr void*)  mramAddress;
      dmaTransferSize +=
          buddyAllocator
              ->sumMetadataSizePerLevels[buddyAllocator->numBuddyLevels];
    }

    mram_read(mramAddress, buddyAllocator->wramBuffer, dmaTransferSize);

    buddyAllocator->bufferedMetadataIndex =
       ((int) (((__mram_ptr void* ) mramAddress) - ((__mram_ptr void* ) buddyAllocator->mramMetadataBase))) >> 2;

    buddyAllocator->bufferedMetadataLength = dmaTransferSize >> 2;

    int bufferIndex = metadataIndex - buddyAllocator->bufferedMetadataIndex;

    int metadata = ((unsigned int)(buddyAllocator->wramBuffer[bufferIndex] &
                                   (0b11 << metadataShift))) >>
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
      int mask = ~(0b11 << metadataShift);

      buddyAllocator->wramBuffer[bufferIndex] &= mask;
    } else if (value == 1) {
      int mask0 = 0b1 << metadataShift;
      int mask1 = ~(0b1 << (metadataShift + 1));

      buddyAllocator->wramBuffer[bufferIndex] |= mask0;
      buddyAllocator->wramBuffer[bufferIndex] &= mask1;
    } else if (value == 2) {
      int mask0 = ~(0b1 << metadataShift);
      int mask1 = 0b1 << (metadataShift + 1);

      buddyAllocator->wramBuffer[bufferIndex] &= mask0;
      buddyAllocator->wramBuffer[bufferIndex] |= mask1;
    } else if (value == 3) {
      int mask = 0b11 << metadataShift;

      buddyAllocator->wramBuffer[bufferIndex] |= mask;
    } else {
      assert(0 && "value is corrupted");
    }
  } else {
    flushMetadata(buddyAllocator);

    // NOTE(bongjoon.hyun@gmail.com): MRAM address is assumed to be aligned with
    // 8
    int mramAddress = getMramAddress(buddyAllocator, level, leafIndex);
    int i = 1;
    while(mramAddress % 8 != 0) {
        mramAddress = getMramAddress(buddyAllocator, level, leafIndex - i);
        i++;
    }

    int dmaTransferSize;
    if (mramAddress + buddyAllocator->bufferSize < buddyAllocator->mramMetadataBase + buddyAllocator->sumMetadataSizePerLevels[buddyAllocator->numBuddyLevels]) {
      dmaTransferSize = buddyAllocator->bufferSize;
    } else {
      dmaTransferSize = (__mram_ptr void*) buddyAllocator->mramMetadataBase - (__mram_ptr void*)  mramAddress;
      dmaTransferSize +=
          buddyAllocator
              ->sumMetadataSizePerLevels[buddyAllocator->numBuddyLevels];
    }

    mram_read(mramAddress, buddyAllocator->wramBuffer, dmaTransferSize);

    buddyAllocator->bufferedMetadataIndex =
       ((int) (((__mram_ptr void* ) mramAddress) - ((__mram_ptr void* ) buddyAllocator->mramMetadataBase))) >> 2;

    buddyAllocator->bufferedMetadataLength = dmaTransferSize >> 2;

    int bufferIndex = metadataIndex - buddyAllocator->bufferedMetadataIndex;

    assert(0 <= bufferIndex &&
           bufferIndex < buddyAllocator->bufferedMetadataLength);

    if (value == 0) {
      int mask = ~(0b11 << metadataShift);

      buddyAllocator->wramBuffer[bufferIndex] &= mask;
    } else if (value == 1) {
      int mask0 = 0b1 << metadataShift;
      int mask1 = ~(0b1 << (metadataShift + 1));

      buddyAllocator->wramBuffer[bufferIndex] |= mask0;
      buddyAllocator->wramBuffer[bufferIndex] &= mask1;
    } else if (value == 2) {
      int mask0 = ~(0b1 << metadataShift);
      int mask1 = 0b1 << (metadataShift + 1);

      buddyAllocator->wramBuffer[bufferIndex] &= mask0;
      buddyAllocator->wramBuffer[bufferIndex] |= mask1;
    } else if (value == 3) {
      int mask = 0b11 << metadataShift;

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
  for (int i = buddyAllocator->numBuddyLevels - 1; i > 0; i--) {
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

struct ThreadCache *initThreadCache(struct BuddyAllocator *buddyAllocator,
                                    int sizeClass, int pageSize) {
  struct ThreadCache *threadCache = mem_alloc(sizeof(struct ThreadCache));

  assert(sizeClass > 0);
  assert(isPowOf2(pageSize));

  threadCache->buddyAllocator = buddyAllocator;
  threadCache->sizeClass = sizeClass;
  threadCache->pageSize = pageSize;
  threadCache->root = buddyAllocate(buddyAllocator, pageSize);

  initThreadCacheNode(threadCache, threadCache->root, NULL);

  return threadCache;
}

void finiThreadCache(struct ThreadCache *threadCache) {
  __mram_ptr struct ThreadCacheNode *node = threadCache->root;

  while (node != NULL) {
    struct ThreadCacheNode *neighbors[2];

    mram_read((__mram_ptr void *)node, (void *)neighbors,
              2 * sizeof(__mram_ptr struct ThreadCacheNode *));

    __mram_ptr struct ThreadCacheNode *next =
        (__mram_ptr struct ThreadCacheNode *)neighbors[1];

    buddyFree(threadCache->buddyAllocator, node);

    node = next;
  }
}

__mram_ptr void *allocateThreadCache(struct ThreadCache *threadCache) {
  __mram_ptr struct ThreadCacheNode *prev = NULL;
  __mram_ptr struct ThreadCacheNode *node = threadCache->root;

  while (node != NULL) {
    __mram_ptr void *pointer = allocateThreadCacheNode(threadCache, node);

    if (pointer != NULL) {
      return pointer;
    }

    struct ThreadCacheNode *neighbors[2];

    mram_read((__mram_ptr void *)node, (void *)neighbors,
              2 * sizeof(__mram_ptr struct ThreadCacheNode *));

    __mram_ptr struct ThreadCacheNode *next =
        (__mram_ptr struct ThreadCacheNode *)neighbors[1];

    prev = node;
    node = next;
  }

  __mram_ptr struct ThreadCacheNode *newNode =
      buddyAllocate(threadCache->buddyAllocator, threadCache->pageSize);
  initThreadCacheNode(threadCache, newNode, prev);

  __mram_ptr void *pointer = allocateThreadCacheNode(threadCache, newNode);

  assert(pointer != NULL);

  return pointer;
}

int freeThreadCache(struct ThreadCache *threadCache, __mram_ptr void *pointer) {
  int isFreedInThreadCache = 0;

  __mram_ptr struct ThreadCacheNode *node = threadCache->root;
  while (node != NULL) {
    int isFreedInThreadCacheNode =
        freeThreadCacheNode(threadCache, node, pointer);

    if (isFreedInThreadCacheNode) {
      return 1;
    }

    struct ThreadCacheNode *neighbors[2];

    mram_read((__mram_ptr void *)node, (void *)neighbors,
              2 * sizeof(__mram_ptr struct ThreadCacheNode *));

    __mram_ptr struct ThreadCacheNode *next =
        (__mram_ptr struct ThreadCacheNode *)neighbors[1];

    node = next;
  }

  return isFreedInThreadCache;
}

void initThreadCacheNode(struct ThreadCache *threadCache,
                         __mram_ptr struct ThreadCacheNode *node,
                         __mram_ptr struct ThreadCacheNode *prev) {
  if (prev != NULL) {
      struct ThreadCacheNode *prevNeighbors[2];

      mram_read((__mram_ptr void *)prev, (void *)prevNeighbors,
                2 * sizeof(__mram_ptr struct ThreadCacheNode *));

      prevNeighbors[1] = (struct ThreadCache *)node;

      mram_write(prevNeighbors, (__mram_ptr void *)prev,
                 2 * sizeof(__mram_ptr struct ThreadCacheNode *));

      struct ThreadCacheNode *neighbors[2] = {(struct ThreadCacheNode *)prev, NULL};

      int metadata[4] = {0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF};

      mram_write(neighbors, (__mram_ptr void *)node,
                 2 * sizeof(__mram_ptr struct ThreadCacheNode *));

      mram_write(metadata,
                 ((__mram_ptr void *)node) +
                     2 * sizeof(__mram_ptr struct ThreadCacheNode *),
                 4 * sizeof(int));
  } else {
    struct ThreadCacheNode *neighbors[2] = {(struct ThreadCacheNode *)prev, NULL};

    int metadata[4] = {0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF};

    mram_write(neighbors, (__mram_ptr void *)node,
               2 * sizeof(__mram_ptr struct ThreadCacheNode *));

    mram_write(metadata,
               ((__mram_ptr void *)node) +
                   2 * sizeof(__mram_ptr struct ThreadCacheNode *),
               4 * sizeof(int));
  }
}

void finiThreadCacheNode(struct ThreadCache *threadCache,
                         __mram_ptr struct ThreadCacheNode *node) {
  struct ThreadCacheNode *neighbors[2];
  int metadata[4];

  mram_read((__mram_ptr void *)node, (void *)neighbors,
            2 * sizeof(__mram_ptr struct ThreadCacheNode *));
  mram_read(((__mram_ptr void *)node) +
                2 * sizeof(__mram_ptr struct ThreadCacheNode *),
            (void *)metadata, 4 * sizeof(int));

  assert(metadata[0] = 0xFFFFFFFF);
  assert(metadata[1] = 0xFFFFFFFF);
  assert(metadata[2] = 0xFFFFFFFF);
  assert(metadata[3] = 0xFFFFFFFF);

  __mram_ptr struct ThreadCacheNode *prev =
      (__mram_ptr struct ThreadCacheNode *)neighbors[0];
  __mram_ptr struct ThreadCacheNode *next =
      (__mram_ptr struct ThreadCacheNode *)neighbors[1];

  struct ThreadCacheNode *prevNeighbors[2];
  struct ThreadCacheNode *nextNeighbors[2];

  mram_read((__mram_ptr void *)prev, (void *)prevNeighbors,
            2 * sizeof(__mram_ptr struct ThreadCacheNode *));
  mram_read((__mram_ptr void *)next, (void *)nextNeighbors,
            2 * sizeof(__mram_ptr struct ThreadCacheNode *));

  prevNeighbors[1] = (struct ThreadCacheNode *)next;
  nextNeighbors[0] = (struct ThreadCacheNode *)prev;

  mram_write(prevNeighbors, (__mram_ptr void *)prev,
             2 * sizeof(__mram_ptr struct ThreadCacheNode *));
  mram_write(nextNeighbors, (__mram_ptr void *)next,
             2 * sizeof(__mram_ptr struct ThreadCacheNode *));

  buddyFree(threadCache->buddyAllocator, node);
}

__mram_ptr void *allocateThreadCacheNode(
    struct ThreadCache *threadCache, __mram_ptr struct ThreadCacheNode *node) {
  int metadata[4];

  mram_read(((__mram_ptr void *)node) +
                2 * sizeof(__mram_ptr struct ThreadCacheNode *),
            (void *)metadata, 4 * sizeof(int));

  int numChunks = getThreadCacheNumChunks(threadCache);

  int metadataIndex;
  int countLeadingZeros3 = count_leading_zeros(metadata[3]);
  metadataIndex = countLeadingZeros3;
  if (metadata[3] != 0 && metadataIndex < numChunks) {
    int metadataIndex = countLeadingZeros3;
    int metadataShift = 31 - countLeadingZeros3;

    int mask = ~(0b1 << metadataShift);

    metadata[3] &= mask;

    mram_write(metadata,
               ((__mram_ptr void *)node) +
                   2 * sizeof(__mram_ptr struct ThreadCacheNode *),
               4 * sizeof(int));

    return getThreadCachePointer(threadCache, node, metadataIndex);
  }

  int countLeadingZeros2 = count_leading_zeros(metadata[2]);
  metadataIndex = 32 + countLeadingZeros2;
  if (metadata[2] != 0 && metadataIndex < numChunks) {
    int metadataShift = 31 - countLeadingZeros2;

    int mask = ~(0b1 << metadataShift);

    metadata[2] &= mask;

    mram_write(metadata,
               ((__mram_ptr void *)node) +
                   2 * sizeof(__mram_ptr struct ThreadCacheNode *),
               4 * sizeof(int));

    return getThreadCachePointer(threadCache, node, metadataIndex);
  }

  int countLeadingZeros1 = count_leading_zeros(metadata[1]);
  metadataIndex = 64 + countLeadingZeros1;
  if (metadata[1] != 0 && metadataIndex < numChunks) {
    int metadataShift = 31 - countLeadingZeros1;

    int mask = ~(0b1 << metadataShift);

    metadata[1] &= mask;

    mram_write(metadata,
               ((__mram_ptr void *)node) +
                   2 * sizeof(__mram_ptr struct ThreadCacheNode *),
               4 * sizeof(int));

    return getThreadCachePointer(threadCache, node, metadataIndex);
  }

  int countLeadingZeros0 = count_leading_zeros(metadata[0]);
  metadataIndex = 96 + countLeadingZeros0;
  if (metadata[0] != 0 && metadataIndex < numChunks) {
    int metadataShift = 31 - countLeadingZeros0;

    int mask = ~(0b1 << metadataShift);

    metadata[0] &= mask;

    mram_write(metadata,
               ((__mram_ptr void *)node) +
                   2 * sizeof(__mram_ptr struct ThreadCacheNode *),
               4 * sizeof(int));

    return getThreadCachePointer(threadCache, node, metadataIndex);
  }

  return NULL;
}

int freeThreadCacheNode(struct ThreadCache *threadCache,
                        __mram_ptr struct ThreadCacheNode *node,
                        __mram_ptr void *pointer) {
  if (((__mram_ptr void *)node) + 24 <= pointer &&
      pointer < ((__mram_ptr void *)node) + threadCache->pageSize) {
    int metadata[4];
    mram_read(((__mram_ptr void *)node) +
                  2 * sizeof(__mram_ptr struct ThreadCacheNode *),
              (void *)metadata, 4 * sizeof(int));

    int chunkIndex = getThreadCacheChunkIndex(threadCache, node, pointer);

    int metadataIndex = chunkIndex / 32;
    int metadataShift = chunkIndex % 32;

    int mask = 0b1 << metadataShift;

    metadata[metadataIndex] |= mask;

    mram_write(metadata,
               ((__mram_ptr void *)node) +
                   2 * sizeof(__mram_ptr struct ThreadCacheNode *),
               4 * sizeof(int));

    return 1;
  } else {
    return 0;
  }
}

int getThreadCacheNumChunks(struct ThreadCache *threadCache) {
  return (threadCache->pageSize - 24) / threadCache->sizeClass;
}

__mram_ptr void *getThreadCachePointer(struct ThreadCache *threadCache,
                                       __mram_ptr struct ThreadCacheNode *node,
                                       int chunkIndex) {
  assert(0 <= chunkIndex && chunkIndex < getThreadCacheNumChunks(threadCache));

  return ((__mram_ptr void *)node) + 24 + chunkIndex * threadCache->sizeClass;
}

int getThreadCacheChunkIndex(struct ThreadCache *threadCache,
                             __mram_ptr struct ThreadCacheNode *node,
                             __mram_ptr void *pointer) {
  assert(((__mram_ptr void *)node) + 24 <= pointer &&
         pointer < ((__mram_ptr void *)node) + threadCache->pageSize);

  return (((__mram_ptr void *)pointer) - ((__mram_ptr void *)node) - 24) /
         threadCache->sizeClass;
}

struct UMalloc *initUMalloc(int mallocableRegionSize, int pageSize,
                            int bufferSize, __mram_ptr void *mramBase,
                            int *sizeClasses, int sizeClassesLength) {
  assert(isPowOf2(pageSize));
  assert(sizeClassesLength > 0);

  struct UMalloc *uMalloc = mem_alloc(sizeof(struct UMalloc));

  uMalloc->buddyAllocator =
      initBuddyAllocator(mallocableRegionSize, pageSize, bufferSize, mramBase);

  uMalloc->sizeClasses = mem_alloc(sizeClassesLength * sizeof(int));
  uMalloc->sizeClassesLength = sizeClassesLength;

  for (int i = 0; i < sizeClassesLength; i++) {
    uMalloc->sizeClasses[i] = sizeClasses[i];
  }

  uMalloc->pageSize = pageSize;

  uMalloc->threadCaches0 =
      mem_alloc(sizeClassesLength * sizeof(struct ThreadCache *));
  uMalloc->threadCaches1 =
      mem_alloc(sizeClassesLength * sizeof(struct ThreadCache *));
  uMalloc->threadCaches2 =
      mem_alloc(sizeClassesLength * sizeof(struct ThreadCache *));
  uMalloc->threadCaches3 =
      mem_alloc(sizeClassesLength * sizeof(struct ThreadCache *));
  uMalloc->threadCaches4 =
      mem_alloc(sizeClassesLength * sizeof(struct ThreadCache *));
  uMalloc->threadCaches5 =
      mem_alloc(sizeClassesLength * sizeof(struct ThreadCache *));
  uMalloc->threadCaches6 =
      mem_alloc(sizeClassesLength * sizeof(struct ThreadCache *));
  uMalloc->threadCaches7 =
      mem_alloc(sizeClassesLength * sizeof(struct ThreadCache *));
  uMalloc->threadCaches8 =
      mem_alloc(sizeClassesLength * sizeof(struct ThreadCache *));
  uMalloc->threadCaches9 =
      mem_alloc(sizeClassesLength * sizeof(struct ThreadCache *));
  uMalloc->threadCaches10 =
      mem_alloc(sizeClassesLength * sizeof(struct ThreadCache *));
  uMalloc->threadCaches11 =
      mem_alloc(sizeClassesLength * sizeof(struct ThreadCache *));
  uMalloc->threadCaches12 =
      mem_alloc(sizeClassesLength * sizeof(struct ThreadCache *));
  uMalloc->threadCaches13 =
      mem_alloc(sizeClassesLength * sizeof(struct ThreadCache *));
  uMalloc->threadCaches14 =
      mem_alloc(sizeClassesLength * sizeof(struct ThreadCache *));
  uMalloc->threadCaches15 =
      mem_alloc(sizeClassesLength * sizeof(struct ThreadCache *));

  for (int i = 0; i < sizeClassesLength; i++) {
    uMalloc->threadCaches0[i] =
        initThreadCache(uMalloc->buddyAllocator, sizeClasses[i], pageSize);
    uMalloc->threadCaches1[i] =
        initThreadCache(uMalloc->buddyAllocator, sizeClasses[i], pageSize);
    uMalloc->threadCaches2[i] =
        initThreadCache(uMalloc->buddyAllocator, sizeClasses[i], pageSize);
    uMalloc->threadCaches3[i] =
        initThreadCache(uMalloc->buddyAllocator, sizeClasses[i], pageSize);
    uMalloc->threadCaches4[i] =
        initThreadCache(uMalloc->buddyAllocator, sizeClasses[i], pageSize);
    uMalloc->threadCaches5[i] =
        initThreadCache(uMalloc->buddyAllocator, sizeClasses[i], pageSize);
    uMalloc->threadCaches6[i] =
        initThreadCache(uMalloc->buddyAllocator, sizeClasses[i], pageSize);
    uMalloc->threadCaches7[i] =
        initThreadCache(uMalloc->buddyAllocator, sizeClasses[i], pageSize);
    uMalloc->threadCaches8[i] =
        initThreadCache(uMalloc->buddyAllocator, sizeClasses[i], pageSize);
    uMalloc->threadCaches9[i] =
        initThreadCache(uMalloc->buddyAllocator, sizeClasses[i], pageSize);
    uMalloc->threadCaches10[i] =
        initThreadCache(uMalloc->buddyAllocator, sizeClasses[i], pageSize);
    uMalloc->threadCaches11[i] =
        initThreadCache(uMalloc->buddyAllocator, sizeClasses[i], pageSize);
    uMalloc->threadCaches12[i] =
        initThreadCache(uMalloc->buddyAllocator, sizeClasses[i], pageSize);
    uMalloc->threadCaches13[i] =
        initThreadCache(uMalloc->buddyAllocator, sizeClasses[i], pageSize);
    uMalloc->threadCaches14[i] =
        initThreadCache(uMalloc->buddyAllocator, sizeClasses[i], pageSize);
    uMalloc->threadCaches15[i] =
        initThreadCache(uMalloc->buddyAllocator, sizeClasses[i], pageSize);
  }

  return uMalloc;
}

void finiUMalloc(struct UMalloc *uMalloc) {
  finiBuddyAllocator(uMalloc->buddyAllocator);

  for (int i = 0; i < uMalloc->sizeClassesLength; i++) {
    finiThreadCache(uMalloc->threadCaches0[i]);
    finiThreadCache(uMalloc->threadCaches1[i]);
    finiThreadCache(uMalloc->threadCaches2[i]);
    finiThreadCache(uMalloc->threadCaches3[i]);
    finiThreadCache(uMalloc->threadCaches4[i]);
    finiThreadCache(uMalloc->threadCaches5[i]);
    finiThreadCache(uMalloc->threadCaches6[i]);
    finiThreadCache(uMalloc->threadCaches7[i]);
    finiThreadCache(uMalloc->threadCaches8[i]);
    finiThreadCache(uMalloc->threadCaches9[i]);
    finiThreadCache(uMalloc->threadCaches10[i]);
    finiThreadCache(uMalloc->threadCaches11[i]);
    finiThreadCache(uMalloc->threadCaches12[i]);
    finiThreadCache(uMalloc->threadCaches13[i]);
    finiThreadCache(uMalloc->threadCaches14[i]);
    finiThreadCache(uMalloc->threadCaches15[i]);
  }
}

__mram_ptr void *allocateUMalloc(struct UMalloc *uMalloc, int size) {
  struct ThreadCache *threadCache = getThreadCache(uMalloc, size);

  if (threadCache != NULL) {
    return allocateThreadCache(threadCache);
  } else {
    return buddyAllocate(uMalloc->buddyAllocator, size);
  }
}

void freeUMalloc(struct UMalloc *uMalloc, __mram_ptr void *pointer) {
  int threadId = me();

  int isFreedInUMalloc = 0;

  if (threadId == 0) {
    for (int i = 0; i < uMalloc->sizeClassesLength; i++) {
      int isFreedInThreadCache =
          freeThreadCache(uMalloc->threadCaches0[i], pointer);

      if (isFreedInThreadCache) {
        return;
      }
    }
  } else if (threadId == 1) {
    for (int i = 0; i < uMalloc->sizeClassesLength; i++) {
      int isFreedInThreadCache =
          freeThreadCache(uMalloc->threadCaches1[i], pointer);

      if (isFreedInThreadCache) {
        return;
      }
    }
  } else if (threadId == 2) {
    for (int i = 0; i < uMalloc->sizeClassesLength; i++) {
      int isFreedInThreadCache =
          freeThreadCache(uMalloc->threadCaches2[i], pointer);

      if (isFreedInThreadCache) {
        return;
      }
    }
  } else if (threadId == 3) {
    for (int i = 0; i < uMalloc->sizeClassesLength; i++) {
      int isFreedInThreadCache =
          freeThreadCache(uMalloc->threadCaches3[i], pointer);

      if (isFreedInThreadCache) {
        return;
      }
    }
  } else if (threadId == 4) {
    for (int i = 0; i < uMalloc->sizeClassesLength; i++) {
      int isFreedInThreadCache =
          freeThreadCache(uMalloc->threadCaches4[i], pointer);

      if (isFreedInThreadCache) {
        return;
      }
    }
  } else if (threadId == 5) {
    for (int i = 0; i < uMalloc->sizeClassesLength; i++) {
      int isFreedInThreadCache =
          freeThreadCache(uMalloc->threadCaches5[i], pointer);

      if (isFreedInThreadCache) {
        return;
      }
    }
  } else if (threadId == 6) {
    for (int i = 0; i < uMalloc->sizeClassesLength; i++) {
      int isFreedInThreadCache =
          freeThreadCache(uMalloc->threadCaches6[i], pointer);

      if (isFreedInThreadCache) {
        return;
      }
    }
  } else if (threadId == 7) {
    for (int i = 0; i < uMalloc->sizeClassesLength; i++) {
      int isFreedInThreadCache =
          freeThreadCache(uMalloc->threadCaches7[i], pointer);

      if (isFreedInThreadCache) {
        return;
      }
    }
  } else if (threadId == 8) {
    for (int i = 0; i < uMalloc->sizeClassesLength; i++) {
      int isFreedInThreadCache =
          freeThreadCache(uMalloc->threadCaches8[i], pointer);

      if (isFreedInThreadCache) {
        return;
      }
    }
  } else if (threadId == 9) {
    for (int i = 0; i < uMalloc->sizeClassesLength; i++) {
      int isFreedInThreadCache =
          freeThreadCache(uMalloc->threadCaches9[i], pointer);

      if (isFreedInThreadCache) {
        return;
      }
    }
  } else if (threadId == 10) {
    for (int i = 0; i < uMalloc->sizeClassesLength; i++) {
      int isFreedInThreadCache =
          freeThreadCache(uMalloc->threadCaches10[i], pointer);

      if (isFreedInThreadCache) {
        return;
      }
    }
  } else if (threadId == 11) {
    for (int i = 0; i < uMalloc->sizeClassesLength; i++) {
      int isFreedInThreadCache =
          freeThreadCache(uMalloc->threadCaches11[i], pointer);

      if (isFreedInThreadCache) {
        return;
      }
    }
  } else if (threadId == 12) {
    for (int i = 0; i < uMalloc->sizeClassesLength; i++) {
      int isFreedInThreadCache =
          freeThreadCache(uMalloc->threadCaches12[i], pointer);

      if (isFreedInThreadCache) {
        return;
      }
    }
  } else if (threadId == 13) {
    for (int i = 0; i < uMalloc->sizeClassesLength; i++) {
      int isFreedInThreadCache =
          freeThreadCache(uMalloc->threadCaches13[i], pointer);

      if (isFreedInThreadCache) {
        return;
      }
    }
  } else if (threadId == 14) {
    for (int i = 0; i < uMalloc->sizeClassesLength; i++) {
      int isFreedInThreadCache =
          freeThreadCache(uMalloc->threadCaches14[i], pointer);

      if (isFreedInThreadCache) {
        return;
      }
    }
  } else if (threadId == 15) {
    for (int i = 0; i < uMalloc->sizeClassesLength; i++) {
      int isFreedInThreadCache =
          freeThreadCache(uMalloc->threadCaches15[i], pointer);

      if (isFreedInThreadCache) {
        return;
      }
    }
  } else {
    assert(0 && "threadId > 15");
  }

  if (isFreedInUMalloc) {
    buddyFree(uMalloc->buddyAllocator, pointer);
  }
}

struct ThreadCache *getThreadCache(struct UMalloc *uMalloc, int size) {
  int threadId = me();

  int sizeClassIndex = -1;
  for (int i = 0; i < uMalloc->sizeClassesLength; i++) {
    if (size <= uMalloc->sizeClasses[i]) {
      sizeClassIndex = i;
      break;
    }
  }

  if (sizeClassIndex == -1) {
    return NULL;
  }

  if (threadId == 0) {
    return uMalloc->threadCaches0[sizeClassIndex];
  } else if (threadId == 1) {
    return uMalloc->threadCaches1[sizeClassIndex];
  } else if (threadId == 2) {
    return uMalloc->threadCaches2[sizeClassIndex];
  } else if (threadId == 3) {
    return uMalloc->threadCaches3[sizeClassIndex];
  } else if (threadId == 4) {
    return uMalloc->threadCaches4[sizeClassIndex];
  } else if (threadId == 5) {
    return uMalloc->threadCaches5[sizeClassIndex];
  } else if (threadId == 6) {
    return uMalloc->threadCaches6[sizeClassIndex];
  } else if (threadId == 7) {
    return uMalloc->threadCaches7[sizeClassIndex];
  } else if (threadId == 8) {
    return uMalloc->threadCaches8[sizeClassIndex];
  } else if (threadId == 9) {
    return uMalloc->threadCaches9[sizeClassIndex];
  } else if (threadId == 10) {
    return uMalloc->threadCaches10[sizeClassIndex];
  } else if (threadId == 11) {
    return uMalloc->threadCaches11[sizeClassIndex];
  } else if (threadId == 12) {
    return uMalloc->threadCaches12[sizeClassIndex];
  } else if (threadId == 13) {
    return uMalloc->threadCaches13[sizeClassIndex];
  } else if (threadId == 14) {
    return uMalloc->threadCaches14[sizeClassIndex];
  } else if (threadId == 15) {
    return uMalloc->threadCaches15[sizeClassIndex];
  } else {
    assert(0 && "threadId > 15");
  }
}
