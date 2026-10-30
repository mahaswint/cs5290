#define _GNU_SOURCE
#include "../tests/shim/common.h"
#include <sys/mman.h>
#define BEGIN(kind) ((void)0)
#define END(kind) ((void)0)
#include "kernel.c"

static unsigned reference[16][320], lengths[16];
static void reference_check(void) {
  for (unsigned t = 0; t < NR_TASKLETS; ++t) {
    host_tid = t;
    graph_check(16);
    policy_verify(t);
  }
  for (unsigned u = 0; u < 16; ++u) {
    struct GraphVertex vertex __attribute__((aligned(8)));
    mram_read(&graph_vertices[u], &vertex, 8);
    assert(vertex.degree == lengths[u]);
    unsigned index = lengths[u];
    uint32_t p = vertex.tail;
    while (p) {
      struct GraphBlock block __attribute__((aligned(8)));
      mram_read((void *)p, &block, sizeof(block));
      assert(index >= block.count);
      index -= block.count;
      for (unsigned i = 0; i < block.count; ++i)
        assert(reference[u][index + i] == block.neighbors[i]);
      p = block.previous;
    }
    assert(index == 0);
  }
}
static void insert(unsigned u, unsigned value) {
  host_tid = u % NR_TASKLETS;
  assert(graph_insert(u, value));
  reference[u][lengths[u]++] = value;
}
static void remove_last(unsigned u) {
  host_tid = u % NR_TASKLETS;
  assert(lengths[u]);
  graph_remove_last(u, reference[u][--lengths[u]]);
}

int main(void) {
  unsigned heap = 256 * 1024;
  host_mram_size = heap + 131072;
  host_mram = mmap(NULL, host_mram_size, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
  assert(host_mram != MAP_FAILED && (uintptr_t)host_mram + host_mram_size < 0x80000000U);
  graph_vertices = (void *)(host_mram + heap + 65536);
  memset(graph_vertices, 0, 16 * sizeof(struct GraphVertex));
  policy_init(heap);
  for (unsigned u = 0; u < 16; ++u)
    for (unsigned j = 0; j < 96; ++j) insert(u, 100000 + u * 1000 + j);
  reference_check();
  const unsigned batches[] = {4, 128, 8, 64};
  for (unsigned round = 0; round < 2; ++round) {
    for (unsigned phase = 0; phase < 4; ++phase) {
      for (unsigned u = 0; u < 16; ++u)
        for (unsigned j = 0; j < batches[phase]; ++j) insert(u, round * 10000 + phase * 1000 + j);
      reference_check();
      for (unsigned u = 0; u < 16; ++u)
        for (unsigned j = 0; j < batches[phase]; ++j) remove_last(u);
      reference_check();
    }
  }
  for (unsigned u = 0; u < 16; ++u) while (lengths[u]) remove_last(u);
  reference_check();
  for (unsigned t = 0; t < NR_TASKLETS; ++t) policy_drain(t);
  assert(policy_backend.used == 0 && getMetadata(policy_buddy, 0, 0) == 3);
  munmap(host_mram, host_mram_size);
  puts("PASS: exact adjacency order, long chains, changing batches, rollback, final coalescing");
  return 0;
}
