#define _GNU_SOURCE
#include "shim/common.h"
#include <sys/mman.h>
#include "../allocator.c"

struct Object { void *p; unsigned size, tag; };
static struct Object objects[NR_TASKLETS][64];
static unsigned rng = 0x12345678;
static unsigned random_word(void) {
  rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng;
}
static void check_objects(void) {
  for (unsigned t = 0; t < NR_TASKLETS; ++t) {
    for (unsigned i = 0; i < 64; ++i) {
      struct Object *o = &objects[t][i];
      if (!o->p) continue;
      assert((uintptr_t)o->p % 8 == 0);
      for (unsigned b = 0; b < o->size; ++b)
        assert(((unsigned char *)o->p)[b] == (unsigned char)o->tag);
    }
  }
}
static void allocate_object(unsigned t, unsigned i, unsigned size) {
  host_tid = t;
  struct Object *o = &objects[t][i];
  assert(!o->p);
  o->p = policy_malloc(size);
  assert(o->p);
  o->size = size;
  o->tag = random_word();
  for (unsigned u = 0; u < NR_TASKLETS; ++u) {
    for (unsigned j = 0; j < 64; ++j) {
      struct Object *other = &objects[u][j];
      if (other == o || !other->p) continue;
      assert((char *)o->p + o->size <= (char *)other->p ||
             (char *)other->p + other->size <= (char *)o->p);
    }
  }
  memset(o->p, o->tag, size);
}
static void free_object(unsigned t, unsigned i) {
  host_tid = t;
  policy_free(objects[t][i].p);
  objects[t][i].p = NULL;
}

int main(void) {
  unsigned heap = 1024 * 1024;
  host_mram_size = heap + 65536;
  host_mram = mmap(NULL, host_mram_size, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
  assert(host_mram != MAP_FAILED && (uintptr_t)host_mram + host_mram_size < 0x80000000U);
  policy_init(heap);
  /* Multiple pages, arbitrary free order, reuse, and controller activity. */
  for (unsigned round = 0; round < 6; ++round) {
    for (unsigned t = 0; t < NR_TASKLETS; ++t)
      for (unsigned i = 0; i < 48; ++i) allocate_object(t, i, 256);
    check_objects();
    for (unsigned t = 0; t < NR_TASKLETS; ++t) {
      for (unsigned i = 0; i < 48; ++i) free_object(t, (i * 5) % 48);
      policy_verify(t);
    }
  }
  if (POLICY == 3) {
    unsigned grows = 0;
    for (unsigned t = 0; t < NR_TASKLETS; ++t) grows += policy_stats[t].grows;
    assert(grows > 0);
    host_tid = 0;
    for (unsigned k = 0; k < 3 * DECAY_OPS; ++k) {
      void *p = policy_malloc(64);
      assert(p);
      policy_free(p);
    }
    assert(policy_caches[0][4].target == 0 && policy_caches[0][4].empty == 0);
    assert(policy_stats[0].idle_trims && policy_stats[0].decays);
  }
  /* Reference is live object intervals/payloads, not allocator internals. */
  const unsigned sizes[] = {16, 32, 64, 129, 256, 512, 1024, 2048, 2049, 4096, 5000, 8192};
  for (unsigned step = 0; step < 4000; ++step) {
    unsigned t = random_word() % NR_TASKLETS, i = random_word() % 64;
    if (objects[t][i].p) free_object(t, i);
    else allocate_object(t, i, sizes[random_word() % 12]);
    if (step % 127 == 0) {
      check_objects();
      for (unsigned u = 0; u < NR_TASKLETS; ++u) policy_verify(u);
    }
  }
  check_objects();
  for (unsigned t = 0; t < NR_TASKLETS; ++t) {
    for (unsigned i = 0; i < 64; ++i) if (objects[t][i].p) free_object(t, i);
    host_tid = t;
    policy_drain(t);
    policy_verify(t);
    assert(!policy_stats[t].live_slots);
  }
  assert(policy_backend.used == 0);
  if (POLICY == 3 && EMPTY_BUDGET && MAX_KEEP) {
    host_tid = 0;
    for (unsigned i = 0; i < 48; ++i) allocate_object(0, i, 256);
    for (unsigned i = 0; i < 48; ++i) free_object(0, i);
    assert(policy_stats[0].retained);
    policy_backend.pressure_limit = 1;
    void *held = policy_malloc(4096);
    assert(held && policy_backend.pressure);
    for (unsigned k = 0; k < DECAY_OPS; ++k) policy_tick(0);
    assert(policy_stats[0].retained == 0 && policy_stats[0].pressure_trims);
    policy_free(held);
    policy_backend.pressure_limit = (heap / 100) * PRESSURE_PERCENT;
    assert(policy_backend.used == 0);
  }
  /* Exercise large-node free and complete coalescing, including root size. */
  host_tid = 0;
  for (unsigned block = 4096; block <= heap; block <<= 1) {
    void *p = policy_malloc(block);
    assert(p);
    memset(p, 0xA5, block);
    policy_free(p);
    assert(policy_backend.used == 0);
  }
  /* Exhaustion is nonfatal, does not damage existing objects, and recovers. */
  void *p = policy_malloc(heap);
  assert(p);
  assert(policy_malloc(4096) == NULL);
  memset(p, 0x5A, heap);
  policy_free(p);
  assert(policy_malloc(0) == NULL && policy_malloc(heap + 1) == NULL);
  assert(policy_backend.used == 0);
  /* Reach the last metadata words, not only the low-address allocation path. */
  void *pages[256];
  for (unsigned i = 0; i < 256; ++i) {
    pages[i] = policy_malloc(4096);
    assert(pages[i]);
    memset(pages[i], i % 251, 4096);
  }
  assert(policy_malloc(4096) == NULL);
  for (unsigned i = 0; i < 256; ++i)
    for (unsigned j = 0; j < 4096; ++j)
      assert(((unsigned char *)pages[i])[j] == i % 251);
  for (unsigned i = 0; i < 256; ++i) policy_free(pages[(i * 73) % 256]);
  assert(policy_backend.used == 0 && getMetadata(policy_buddy, 0, 0) == 3);
  puts("PASS: payload integrity, non-overlap, reuse, large frees, coalescing, OOM recovery");
  munmap(host_mram, host_mram_size);
  return 0;
}
