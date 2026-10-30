#define BEGIN(kind) __asm__ volatile("policy_begin_" #kind "_%=:" ::: "memory")
#define END(kind) __asm__ volatile("policy_end_" #kind "_%=:" ::: "memory")
#include "kernel.c"
#include "trace.inc"

__host int mallocableRegionSize, pageSize, bufferSize;
__mram_noinit struct GraphVertex graph_vertex_storage[GRAPH_VERTICES];
__mram_noinit uint64_t graph_base_applied[GRAPH_BASE_ARCS];
__mram_noinit uint64_t graph_update_applied[GRAPH_UPDATE_ARCS];
struct Snapshot { unsigned phase, round, requested, slots, reserved, empty_bytes; };
static struct Snapshot snapshots[8 * ROUNDS + 2];
static unsigned snapshot_count;
static volatile unsigned policy_report_meta[5];
BARRIER_INIT(policy_barrier, NR_TASKLETS);

static void applied_write(__mram_ptr uint64_t *address, unsigned value) {
  uint32_t cell[2] __attribute__((aligned(8))) = {value, 0};
  mram_write(cell, address, 8);
}
static unsigned applied_read(__mram_ptr uint64_t *address) {
  uint32_t cell[2] __attribute__((aligned(8)));
  mram_read(address, cell, 8);
  return cell[0];
}
static void snapshot(unsigned phase, unsigned round) {
  struct Snapshot *s = &snapshots[snapshot_count++];
  assert(snapshot_count <= 8 * ROUNDS + 2);
  s->phase = phase; s->round = round;
  s->reserved = policy_backend.used;
  s->requested = s->slots = s->empty_bytes = 0;
  for (unsigned t = 0; t < NR_TASKLETS; ++t) {
    s->requested += benchmark_live[t];
    s->slots += policy_stats[t].live_slots;
    s->empty_bytes += policy_stats[t].retained * POLICY_PAGE;
  }
}

/* Keep a zero-instruction end label inside a function only tasklet 0 calls.
 * At a conditional branch join the same PC could also be reached by others.
 */
static __attribute__((noinline)) void graph_init_end(void) {
  snapshot(0, 0);
  END(init);
}

static __attribute__((noinline)) void graph_final_report(void) {
  END(total);
  assert(policy_backend.used == 0 && getMetadata(policy_buddy, 0, 0) == 3);
  for (unsigned t = 0; t < NR_TASKLETS; ++t) policy_verify(t);
  snapshot(99, 0);
  policy_report_meta[0] = (unsigned)__HEAP_POINTER;
  policy_report_meta[1] = sizeof(policy_caches);
  policy_report_meta[2] = sizeof(policy_stats);
  policy_report_meta[3] = snapshot_count;
  policy_report_meta[4] = NR_TASKLETS;
  __asm__ volatile("policy_report_ready_%=:" :: "r"(policy_stats), "r"(&policy_backend),
        "r"(snapshots), "r"(policy_report_meta) : "memory");
}

int main(void) {
  unsigned tid = me();
  if (tid == 0) {
    mem_reset(); BEGIN(total); BEGIN(init);
    policy_init(HEAP_BYTES);
    graph_vertices = graph_vertex_storage;
  }
  barrier_wait(&policy_barrier);
  for (unsigned u = tid; u < GRAPH_VERTICES; u += NR_TASKLETS) {
    struct GraphVertex vertex __attribute__((aligned(8))) = {0, 0};
    mram_write(&vertex, &graph_vertices[u], 8);
  }
  for (unsigned i = graph_base_offsets[tid]; i < graph_base_offsets[tid + 1]; ++i)
    applied_write(&graph_base_applied[i], graph_insert(graph_base[i].source, graph_base[i].destination));
  graph_check(GRAPH_VERTICES);
  if (!policy_stats[tid].failures) {
    assert(graph_live_sum[tid] == graph_expected_sums[0][tid]);
    assert(graph_live_edges[tid] == graph_expected_counts[0][tid]);
  }
  barrier_wait(&policy_barrier);
  if (tid == 0) graph_init_end();
  barrier_wait(&policy_barrier);

  for (unsigned round = 0; round < ROUNDS; ++round) {
    for (unsigned phase = 0; phase < 4; ++phase) {
      BEGIN(work);
      for (unsigned i = graph_updates_offsets[tid]; i < graph_updates_offsets[tid + 1]; ++i) {
        unsigned packed = graph_updates[i].destination;
        if (packed >> 20 >= graph_limits[phase]) continue;
        applied_write(&graph_update_applied[i], graph_insert(graph_updates[i].source, packed & 0xfffff));
      }
      barrier_wait(&policy_barrier);
      if (tid == 0) snapshot(2 * phase + 1, round);
      barrier_wait(&policy_barrier);
      unsigned read_passes = phase % 2 ? 1 : 4;
      for (unsigned pass = 0; pass < read_passes; ++pass) graph_check(GRAPH_VERTICES);
      if (!policy_stats[tid].failures) {
        assert(graph_live_sum[tid] == graph_expected_sums[phase + 1][tid]);
        assert(graph_live_edges[tid] == graph_expected_counts[phase + 1][tid]);
      }
      for (unsigned i = graph_updates_offsets[tid + 1]; i > graph_updates_offsets[tid]; ) {
        --i;
        unsigned packed = graph_updates[i].destination;
        if (packed >> 20 < graph_limits[phase] && applied_read(&graph_update_applied[i]))
          graph_remove_last(graph_updates[i].source, packed & 0xfffff);
      }
      graph_check(GRAPH_VERTICES);
      barrier_wait(&policy_barrier);
      if (VALIDATE) policy_verify(tid);
      barrier_wait(&policy_barrier);
      if (tid == 0) snapshot(2 * phase + 2, round);
      barrier_wait(&policy_barrier);
      END(work);
    }
  }
  BEGIN(drain);
  for (unsigned i = graph_base_offsets[tid + 1]; i > graph_base_offsets[tid]; ) {
    --i;
    if (applied_read(&graph_base_applied[i]))
      graph_remove_last(graph_base[i].source, graph_base[i].destination);
  }
  assert(graph_live_edges[tid] == 0 && graph_live_sum[tid] == 0 && benchmark_live[tid] == 0);
  policy_drain(tid);
  barrier_wait(&policy_barrier);
  END(drain);
  if (tid == 0) graph_final_report();
  return 0;
}
