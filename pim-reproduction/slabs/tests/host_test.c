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
static void reset_heap(unsigned heap) {
  host_wram_used = 0;
  memset(host_wram, 0, sizeof(host_wram));
  memset(objects, 0, sizeof(objects));
  memset(host_mram, 0, host_mram_size);
  host_tid = 0;
  policy_init(heap);
  assert(policy_backend.init_failures == 0);
}
static void check_objects(void) {
  for (unsigned t = 0; t < NR_TASKLETS; ++t)
    for (unsigned i = 0; i < 64; ++i) {
      struct Object *o = &objects[t][i];
      if (!o->p) continue;
      assert((uintptr_t)o->p % 8 == 0);
      for (unsigned b = 0; b < o->size; ++b)
        assert(((unsigned char *)o->p)[b] == (unsigned char)o->tag);
    }
}
static void allocate_object(unsigned t, unsigned i, unsigned size) {
  host_tid = t;
  struct Object *o = &objects[t][i];
  assert(!o->p);
  o->p = policy_malloc(size);
  assert(o->p);
  o->size = size; o->tag = random_word();
  for (unsigned u = 0; u < NR_TASKLETS; ++u)
    for (unsigned j = 0; j < 64; ++j) {
      struct Object *other = &objects[u][j];
      if (other == o || !other->p) continue;
      assert((char *)o->p + o->size <= (char *)other->p ||
             (char *)other->p + other->size <= (char *)o->p);
    }
  memset(o->p, o->tag, size);
}
static void free_object(unsigned t, unsigned i) {
  host_tid = t;
  policy_free(objects[t][i].p);
  objects[t][i].p = NULL;
}
static void drain_all(void) {
  for (unsigned t = 0; t < NR_TASKLETS; ++t) {
    host_tid = t; policy_drain(t); policy_verify(t);
  }
  assert(policy_backend.used == 0 && getMetadata(policy_buddy, 0, 0) == 3);
}
int main(void) {
  const unsigned heap = 4 * 1024 * 1024;
  host_mram_size = heap + 65536;
  host_mram = mmap(NULL, host_mram_size, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
  assert(host_mram != MAP_FAILED && (uintptr_t)host_mram + host_mram_size < 0x80000000U);
  reset_heap(heap);
  /* Fill several spans, then free in a permutation with root/middle/tail edits. */
  for (unsigned t = 0; t < NR_TASKLETS; ++t)
    for (unsigned i = 0; i < 48; ++i) allocate_object(t, i, 4096);
  check_objects();
  for (unsigned t = 0; t < NR_TASKLETS; ++t) {
    if (SLAB_BYTES) {
      unsigned capacity = (SLAB_BYTES - POLICY_HEADER) / 4096;
      assert(policy_stats[t].slab_refills == (48 + capacity - 1) / capacity);
      assert(policy_stats[t].slab_hits == 48 - policy_stats[t].slab_refills);
    } else assert(policy_stats[t].direct_allocations == 48 && !policy_stats[t].slab_hits);
    for (unsigned i = 0; i < 48; ++i) {
      free_object(t, (i * 5) % 48);
      policy_verify(t);
    }
  }
  drain_all();
  reset_heap(heap);
  /* Mixed size-class boundaries plus fully checked payloads and live intervals. */
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
  for (unsigned t = 0; t < NR_TASKLETS; ++t)
    for (unsigned i = 0; i < 64; ++i)
      if (objects[t][i].p) free_object(t, i);
  drain_all();
  /* Exact capacity accounts for in-band slab headers and common eager pages. */
  reset_heap(256 * 1024);
  unsigned available = 256 * 1024 - policy_backend.used;
  unsigned span = SLAB_BYTES ? SLAB_BYTES : POLICY_PAGE;
  unsigned capacity = SLAB_BYTES ? (SLAB_BYTES - POLICY_HEADER) / 4096 : 1;
  unsigned expected = available / span * capacity;
  void *pages[256]; unsigned count = 0;
  host_tid = 0;
  while (count < 256 && (pages[count] = policy_malloc(4096))) {
    memset(pages[count], count % 251, 4096); count++;
  }
  assert(count == expected && count < 256 && policy_stats[0].failures == 1);
  for (unsigned i = 0; i < count; ++i)
    for (unsigned j = 0; j < 4096; ++j)
      assert(((unsigned char *)pages[i])[j] == i % 251);
  for (unsigned i = 0; i < count; ++i) policy_free(pages[count - i - 1]);
  drain_all();
  /* Large direct nodes still free/coalesce, including allocation of the root. */
  reset_heap(heap); drain_all(); host_tid = 0;
  for (unsigned block = 8192; block <= heap; block <<= 1) {
    void *p = policy_malloc(block); assert(p);
    memset(p, 0xA5, block); policy_free(p);
    assert(policy_backend.used == 0);
  }
  void *p = policy_malloc(heap); assert(p);
  assert(policy_malloc(4096) == NULL);
  memset(p, 0x5A, heap); policy_free(p);
  assert(policy_malloc(0) == NULL && policy_malloc(heap + 1) == NULL);
  assert(policy_backend.used == 0 && getMetadata(policy_buddy, 0, 0) == 3);
  puts("PASS: slab geometry/hits, payloads, non-overlap, mixed sizes, partial frees, exact capacity, coalescing, OOM recovery");
  munmap(host_mram, host_mram_size);
  return 0;
}
