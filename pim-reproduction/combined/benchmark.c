/* Deterministic bounded-live-set workloads. Every policy receives the same
 * allocation sizes, slot schedule, payload work, and synchronization.
 * Phases are synthetic mechanisms tests, not captured application arrivals.
 */
#include "allocator.c"
__host int mallocableRegionSize, pageSize, bufferSize;
__mram_noinit uint64_t policy_pointer_table[NR_TASKLETS][BURST];
static unsigned benchmark_live[NR_TASKLETS];
struct Snapshot { unsigned phase, round, requested, slots, reserved, empty_bytes; };
static struct Snapshot snapshots[8 * ROUNDS + 2];
static unsigned snapshot_count;
static volatile unsigned policy_report_meta[5];
BARRIER_INIT(policy_barrier, NR_TASKLETS);
#define BEGIN(kind) __asm__ volatile("policy_begin_" #kind "_%=:" ::: "memory")
#define END(kind) __asm__ volatile("policy_end_" #kind "_%=:" ::: "memory")

static void save_pointer(unsigned tid, unsigned i, __mram_ptr void *p) {
  uint32_t cell[2] __attribute__((aligned(8))) = {(uint32_t)p, 0};
  mram_write(cell, &policy_pointer_table[tid][i], 8);
}
static __mram_ptr void *load_pointer(unsigned tid, unsigned i) {
  uint32_t cell[2] __attribute__((aligned(8)));
  mram_read(&policy_pointer_table[tid][i], cell, 8);
  return (__mram_ptr void *)cell[0];
}
static unsigned slot(unsigned j, unsigned phase, unsigned round) {
  return (j * ((TRACE_SEED << 1) | 1U) + phase * 13U + round * 7U) & (BURST - 1);
}
static unsigned signature(unsigned tid, unsigned i, unsigned phase, unsigned round) {
  return 0x13570000U ^ TRACE_SEED ^ (tid << 20) ^ (phase << 16) ^ (round << 10) ^ i;
}
static void check_payload(__mram_ptr void *p, unsigned size, unsigned i, unsigned phase, unsigned round) {
  unsigned tag[2] __attribute__((aligned(8)));
  mram_read(p, tag, 8);
  assert(tag[0] == signature(me(), i, phase, round) && tag[1] == ~tag[0]);
  mram_read(p + size - 8, tag, 8);
  assert(tag[0] == signature(me(), i, phase, round) && tag[1] == ~tag[0]);
}
static __attribute__((noinline)) void allocate_slot(unsigned i, unsigned size, unsigned phase, unsigned round) {
  unsigned tid = me();
  BEGIN(alloc);
  __mram_ptr void *p = policy_malloc(size);
  END(alloc);
  save_pointer(tid, i, p);
  if (p) {
    unsigned word = signature(tid, i, phase, round);
    unsigned tag[2] __attribute__((aligned(8))) = {word, ~word};
    mram_write(tag, p, 8); mram_write(tag, p + size - 8, 8);
    benchmark_live[tid] += size;
  }
}
static __attribute__((noinline)) void free_slot(unsigned i, unsigned size, unsigned phase, unsigned round) {
  unsigned tid = me(); __mram_ptr void *p = load_pointer(tid, i);
  if (!p) return;
  check_payload(p, size, i, phase, round);
  BEGIN(free);
  policy_free(p);
  END(free);
  benchmark_live[tid] -= size; save_pointer(tid, i, NULL);
}
static void snapshot(unsigned phase, unsigned round) {
  struct Snapshot *s = &snapshots[snapshot_count++];
  assert(snapshot_count <= 8 * ROUNDS + 2);
  s->phase = phase; s->round = round; s->reserved = policy_backend.used;
  s->requested = s->slots = s->empty_bytes = 0;
  for (unsigned t = 0; t < NR_TASKLETS; ++t) {
    s->requested += benchmark_live[t]; s->slots += policy_stats[t].live_slots;
    s->empty_bytes += policy_stats[t].retained * POLICY_PAGE;
  }
}
static __attribute__((noinline)) void init_end(void) { snapshot(0, 0); END(init); }
static __attribute__((noinline)) void final_report(void) {
  END(total);
  assert(policy_backend.used == 0 && getMetadata(policy_buddy, 0, 0) == 3);
  for (unsigned t = 0; t < NR_TASKLETS; ++t) {
    assert(!benchmark_live[t] && !policy_stats[t].live_slots); policy_verify(t);
  }
  snapshot(99, 0);
  policy_report_meta[0] = (unsigned)__HEAP_POINTER;
  policy_report_meta[1] = sizeof(policy_caches); policy_report_meta[2] = sizeof(policy_stats);
  policy_report_meta[3] = snapshot_count; policy_report_meta[4] = NR_TASKLETS;
  __asm__ volatile("policy_report_ready_%=:" :: "r"(policy_stats), "r"(&policy_backend),
        "r"(snapshots), "r"(policy_report_meta) : "memory");
}
int main(void) {
  unsigned tid = me();
  if (!tid) { mem_reset(); BEGIN(total); BEGIN(init); policy_init(HEAP_BYTES); init_end(); }
  barrier_wait(&policy_barrier);
  BEGIN(work);
  for (unsigned phase = 0; phase < 4; ++phase) {
    unsigned size = 256;
    if (WORKLOAD == 0) size = 64;
    if (WORKLOAD == 2) { const unsigned sizes[4] = {256, 1024, 64, 512}; size = sizes[phase]; }
    if (WORKLOAD == 3) { const unsigned sizes[4] = {256, 1024, 2048, 64}; size = sizes[phase]; }
    if (WORKLOAD == 5) size = 4096;
    for (unsigned round = 0; round < ROUNDS; ++round) {
      for (unsigned i = 0; i < BURST; ++i) allocate_slot(i, size, phase, round);
      if (WORKLOAD == 1) {
        /* Leave half the live set in place, freeing holes across many pages. */
        for (unsigned j = 0; j < BURST / 2; ++j) free_slot(slot(j, phase, round), size, phase, round);
        for (unsigned i = 0; i < BURST; ++i) {
          __mram_ptr void *p = load_pointer(tid, i);
          if (p) check_payload(p, size, i, phase, round);
        }
        for (unsigned j = 0; j < BURST / 2; ++j) allocate_slot(slot(j, phase, round), size, phase, round);
      }
      barrier_wait(&policy_barrier);
      if (!tid) snapshot(2 * phase + 1, round);
      barrier_wait(&policy_barrier);
      for (unsigned j = 0; j < BURST; ++j) free_slot(slot(j, phase, round), size, phase, round);
      barrier_wait(&policy_barrier);
      if (VALIDATE) policy_verify(tid);
      barrier_wait(&policy_barrier);
      if (!tid) snapshot(2 * phase + 2, round);
      barrier_wait(&policy_barrier);
    }
  }
  END(work);
  BEGIN(drain); policy_drain(tid); barrier_wait(&policy_barrier); END(drain);
  if (!tid) final_report();
  return 0;
}
