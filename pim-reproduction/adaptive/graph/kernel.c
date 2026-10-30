/* A tasklet owns each source vertex and its adjacency blocks for their lifetime.
 * This is a small UPMEM port of a paged adjacency list, not the CUDA faimGraph.
 * Each graph block is an allocator object, distinct from its 4096-byte slab.
 */
#include "allocator.c"
#ifndef GRAPH_BLOCK_BYTES
#define GRAPH_BLOCK_BYTES 256
#endif
#define GRAPH_CAPACITY ((GRAPH_BLOCK_BYTES - 8) / 4)
struct GraphVertex { uint32_t tail; unsigned degree; };
struct GraphBlock { uint32_t previous; unsigned count, neighbors[GRAPH_CAPACITY]; };
static __mram_ptr struct GraphVertex *graph_vertices;
static unsigned graph_live_edges[NR_TASKLETS], graph_live_sum[NR_TASKLETS];
static unsigned benchmark_live[NR_TASKLETS];

static unsigned graph_signature(unsigned source, unsigned destination) {
  return ((source + 1) * 0x9e3779b1U) ^ ((destination + 1) * 0x85ebca6bU);
}

static __attribute__((noinline)) int graph_insert(unsigned source, unsigned destination) {
  unsigned tid = me();
  assert(source % NR_TASKLETS == tid);
  struct GraphVertex vertex __attribute__((aligned(8)));
  struct GraphBlock block __attribute__((aligned(8)));
  mram_read(&graph_vertices[source], &vertex, 8);
  if (!vertex.tail || vertex.degree % GRAPH_CAPACITY == 0) {
    BEGIN(alloc);
    __mram_ptr void *p = policy_malloc(GRAPH_BLOCK_BYTES);
    END(alloc);
    if (!p) return 0;
    memset(&block, 0, sizeof(block));
    block.previous = vertex.tail;
    vertex.tail = (uint32_t)p;
    benchmark_live[tid] += GRAPH_BLOCK_BYTES;
  } else mram_read((__mram_ptr void *)vertex.tail, &block, sizeof(block));
  assert(block.count < GRAPH_CAPACITY);
  block.neighbors[block.count++] = destination;
  vertex.degree++;
  mram_write(&block, (__mram_ptr void *)vertex.tail, sizeof(block));
  mram_write(&vertex, &graph_vertices[source], 8);
  graph_live_edges[tid]++;
  graph_live_sum[tid] += graph_signature(source, destination);
  return 1;
}

/* Reverse-batch deletion is deliberate: no hidden scan to find an edge.
 * Other deletion orders/compaction policies are separate workload variants.
 */
static __attribute__((noinline)) void graph_remove_last(unsigned source, unsigned destination) {
  unsigned tid = me();
  assert(source % NR_TASKLETS == tid);
  struct GraphVertex vertex __attribute__((aligned(8)));
  struct GraphBlock block __attribute__((aligned(8)));
  mram_read(&graph_vertices[source], &vertex, 8);
  assert(vertex.degree && vertex.tail);
  mram_read((__mram_ptr void *)vertex.tail, &block, sizeof(block));
  assert(block.count && block.neighbors[block.count - 1] == destination);
  vertex.degree--; block.count--;
  if (!block.count) {
    __mram_ptr void *p = (__mram_ptr void *)vertex.tail;
    vertex.tail = block.previous;
    BEGIN(free);
    policy_free(p);
    END(free);
    benchmark_live[tid] -= GRAPH_BLOCK_BYTES;
  } else mram_write(&block, (__mram_ptr void *)vertex.tail, sizeof(block));
  mram_write(&vertex, &graph_vertices[source], 8);
  graph_live_edges[tid]--;
  graph_live_sum[tid] -= graph_signature(source, destination);
}

static void graph_check(unsigned vertices) {
  unsigned tid = me(), edges = 0, checksum = 0;
  for (unsigned u = tid; u < vertices; u += NR_TASKLETS) {
    struct GraphVertex vertex __attribute__((aligned(8)));
    mram_read(&graph_vertices[u], &vertex, 8);
    unsigned count = 0;
    uint32_t p = vertex.tail;
    while (p) {
      struct GraphBlock block __attribute__((aligned(8)));
      mram_read((__mram_ptr void *)p, &block, sizeof(block));
      assert(block.count && block.count <= GRAPH_CAPACITY);
      if (count) assert(block.count == GRAPH_CAPACITY);
      for (unsigned i = 0; i < block.count; ++i)
        checksum += graph_signature(u, block.neighbors[i]);
      count += block.count;
      assert(count <= vertex.degree);
      p = block.previous;
    }
    assert(count == vertex.degree);
    edges += count;
  }
  assert(edges == graph_live_edges[tid] && checksum == graph_live_sum[tid]);
}
