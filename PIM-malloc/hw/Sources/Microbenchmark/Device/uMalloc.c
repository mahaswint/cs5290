#include "Microbenchmark/Device/uMalloc.h"

#include <alloc.h>
#include <assert.h>
#include <defs.h>
#include <mram.h>
#include <mutex.h>
#include <stdio.h>

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
