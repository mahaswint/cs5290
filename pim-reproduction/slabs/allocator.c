/* Isolated size-dependent slab experiment: no adaptive controller or retention.
 * Same-tasklet bitmap frontend over the maintained author-derived buddy backend.
 * Size classes 16..2048 B use 4 KiB pages. Optional 2049..4096 B class uses
 * SLAB_BYTES (16/32/64 KiB). Wholly empty used pages/slabs return immediately.
 */
#include "buddy_backend.inc"
#include <stdint.h>

#ifndef SLAB_BYTES
#define SLAB_BYTES 0
#endif
#ifndef EAGER_INIT
#define EAGER_INIT 1
#endif
#if SLAB_BYTES != 0 && SLAB_BYTES != 16384 && SLAB_BYTES != 32768 && SLAB_BYTES != 65536
#error "SLAB_BYTES must be 0, 16384, 32768, or 65536"
#endif

#define POLICY_CLASSES 9
#define POLICY_PAGE 4096
#define POLICY_HEADER 24

struct PolicyCache {
  __mram_ptr void *root;
  unsigned size, span, pages, empty;
};
struct PolicyStats {
  unsigned alloc_calls, successes, free_calls, cache_hits, refills, returns;
  unsigned failures, direct_allocations, slab_hits, slab_refills, live_slots, peak_slots;
};
struct PolicyBackend {
  unsigned used, peak, allocs, frees, failed, init_failures;
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
  unsigned n = (c->span - POLICY_HEADER) / c->size;
  return n < 128 ? n : 128;
}
static unsigned policy_empty_bytes(unsigned tid) {
  unsigned bytes = 0;
  for (unsigned i = 0; i < POLICY_CLASSES; ++i)
    bytes += policy_caches[tid][i].empty * policy_caches[tid][i].span;
  return bytes;
}
static void policy_add_page(struct PolicyCache *c, __mram_ptr void *p) {
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
  c->pages++; c->empty++;
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
  assert(c->empty && c->pages);
  c->empty--; c->pages--;
  policy_stats[tid].returns++;
  assert(policy_backend_free(page) == c->span);
}
static __attribute__((noinline)) void policy_init(unsigned heap_size) {
  assert(heap_size >= POLICY_PAGE && isPowOf2(heap_size));
  memset(policy_caches, 0, sizeof(policy_caches));
  memset(policy_stats, 0, sizeof(policy_stats));
  memset(&policy_backend, 0, sizeof(policy_backend));
  policy_heap_size = heap_size;
  policy_buddy = initBuddyAllocator(heap_size, POLICY_PAGE, 64, DPU_MRAM_HEAP_POINTER);
  for (unsigned t = 0; t < NR_TASKLETS; ++t) {
    for (unsigned i = 0; i < POLICY_CLASSES; ++i) {
      struct PolicyCache *c = &policy_caches[t][i];
      c->size = 16U << i;
      c->span = i == 8 ? SLAB_BYTES : POLICY_PAGE;
      /* Original eight classes share the same eager/lazy initialization in
         baseline and variants. The added 4 KiB class is always on demand. */
      if (EAGER_INIT && i < 8) {
        __mram_ptr void *p = policy_backend_alloc(POLICY_PAGE);
        if (p) policy_add_page(c, p);
        else policy_backend.init_failures++;
      }
    }
  }
}
static __attribute__((noinline)) __mram_ptr void *policy_malloc(unsigned size) {
  unsigned tid = me();
  struct PolicyStats *s = &policy_stats[tid];
  s->alloc_calls++;
  if (!size || size > policy_heap_size) { s->failures++; return NULL; }
  unsigned index = 0;
  while (index < POLICY_CLASSES && size > (16U << index)) index++;
  if (index == POLICY_CLASSES || (index == 8 && !SLAB_BYTES)) {
    __mram_ptr void *p = policy_backend_alloc(size);
    if (p) {
      s->successes++; s->direct_allocations++;
      s->live_slots += policy_block_size(size);
      if (s->live_slots > s->peak_slots) s->peak_slots = s->live_slots;
    } else s->failures++;
    return p;
  }
  struct PolicyCache *c = &policy_caches[tid][index];
  __mram_ptr void *page = c->root;
  unsigned bitmap[4] __attribute__((aligned(8)));
  int refill = 0;
  for (;;) {
    while (page) {
      mram_read(page + 8, bitmap, 16);
      for (int w = 3; w >= 0; --w) {
        if (!bitmap[w]) continue;
        unsigned bit = __builtin_clz(bitmap[w]);
        unsigned slot = (3 - w) * 32 + bit;
        if (slot >= policy_capacity(c)) continue;
        if (policy_empty(bitmap)) { assert(c->empty); c->empty--; }
        bitmap[w] &= ~(1U << (31 - bit));
        mram_write(bitmap, page + 8, 16);
        if (!refill) { s->cache_hits++; if (index == 8) s->slab_hits++; }
        s->successes++; s->live_slots += c->size;
        if (s->live_slots > s->peak_slots) s->peak_slots = s->live_slots;
        return page + POLICY_HEADER + slot * c->size;
      }
      uint32_t links[2] __attribute__((aligned(8)));
      policy_links(page, links);
      page = (__mram_ptr void *)links[1];
    }
    page = policy_backend_alloc(c->span);
    if (!page) { s->failures++; return NULL; }
    policy_add_page(c, page);
    s->refills++;
    if (index == 8) s->slab_refills++;
    refill = 1;
  }
}
static __attribute__((noinline)) void policy_free(__mram_ptr void *p) {
  if (!p) return;
  unsigned tid = me();
  struct PolicyStats *s = &policy_stats[tid];
  s->free_calls++;
  for (unsigned i = 0; i < POLICY_CLASSES; ++i) {
    struct PolicyCache *c = &policy_caches[tid][i];
    __mram_ptr void *page = c->root;
    while (page) {
      uint32_t links[2] __attribute__((aligned(8)));
      policy_links(page, links);
      if (p >= page + POLICY_HEADER && p < page + c->span) {
        unsigned offset = p - page - POLICY_HEADER;
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
        if (policy_empty(bitmap)) {
          c->empty++;
          policy_release_page(c, page, tid);
        }
        return;
      }
      page = (__mram_ptr void *)links[1];
    }
  }
  /* Same-tasklet free only; direct allocations bypass the frontend. */
  unsigned block = policy_backend_free(p);
  assert(s->live_slots >= block);
  s->live_slots -= block;
}
static __attribute__((noinline)) void policy_drain(unsigned tid) {
  assert(policy_stats[tid].live_slots == 0);
  for (unsigned i = 0; i < POLICY_CLASSES; ++i) {
    struct PolicyCache *c = &policy_caches[tid][i];
    assert(c->empty == c->pages);
    while (c->root) policy_release_page(c, c->root, tid);
  }
}
static void policy_verify(unsigned tid) {
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
  }
}
