#ifndef UMALLOC_DEVICE_THREAD_CACHE_H_
#define UMALLOC_DEVICE_THREAD_CACHE_H_

#include <dpuruntime.h>
#include <mram.h>

#include "BuddyAllocator.h"
#include "Microbenchmark/Device/BuddyAllocator.h"

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

#endif
