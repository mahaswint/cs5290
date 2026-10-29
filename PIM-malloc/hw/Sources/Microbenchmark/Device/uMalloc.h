#ifndef UMALLOC_DEVICE_UMALLOC_H_
#define UMALLOC_DEVICE_UMALLOC_H_

#include <mram.h>

#include "Microbenchmark/Device/BuddyAllocator.h"
#include "Microbenchmark/Device/ThreadCache.h"

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

#endif
