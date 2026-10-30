#include "allocator.c"

#ifndef BURST
#define BURST 32
#endif
#ifndef ROUNDS
#define ROUNDS 4
#endif
#ifndef SIZE_BYTES
#define SIZE_BYTES 4096
#endif
#ifndef HEAP_BYTES
#define HEAP_BYTES (32 * 1024 * 1024)
#endif
#ifndef WORKLOAD
#define WORKLOAD 0
#endif
#ifndef VALIDATE
#define VALIDATE 0
#endif

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
static void snapshot(unsigned phase, unsigned round) {
  assert(snapshot_count < 8 * ROUNDS + 2);
  struct Snapshot *s = &snapshots[snapshot_count++];
  s->phase = phase; s->round = round;
  s->reserved = policy_backend.used;
  s->requested = s->slots = s->empty_bytes = 0;
  for (unsigned t = 0; t < NR_TASKLETS; ++t) {
    s->requested += benchmark_live[t];
    s->slots += policy_stats[t].live_slots;
    s->empty_bytes += policy_empty_bytes(t);
  }
}
static void check_and_free(unsigned tid, unsigned i, unsigned size, unsigned phase, unsigned round) {
  __mram_ptr void *p = load_pointer(tid, i);
  unsigned tag[2] __attribute__((aligned(8)));
  mram_read(p, tag, 8);
  assert(tag[0] == (0x13570000U ^ (tid << 12) ^ (phase << 8) ^ i));
  assert(tag[1] == (0x24680000U ^ round));
  mram_read(p + size - 8, tag, 8);
  assert(tag[0] == (0x13570000U ^ (tid << 12) ^ (phase << 8) ^ i));
  assert(tag[1] == (0x24680000U ^ round));
  BEGIN(free);
  policy_free(p);
  END(free);
  benchmark_live[tid] -= size;
}
int main(void) {
  unsigned tid = me();
  if (tid == 0) {
    mem_reset();
    BEGIN(total);
    BEGIN(init);
    policy_init(HEAP_BYTES);
    END(init);
    snapshot(0, 0);
  }
  barrier_wait(&policy_barrier);
  BEGIN(work);
  unsigned phases = WORKLOAD == 1 || WORKLOAD == 3 ? 4 : 1;
  for (unsigned phase = 0; phase < phases; ++phase) {
    unsigned size = SIZE_BYTES, limit = BURST;
    if (WORKLOAD == 1) {
      const unsigned divisors[4] = {8, 1, 4, 2};
      limit = BURST / divisors[phase];
      if (!limit) limit = 1;
    } else if (WORKLOAD == 3) {
      const unsigned sizes[4] = {16, 129, 4096, 8192};
      size = sizes[phase];
    }
    for (unsigned round = 0; round < ROUNDS; ++round) {
      unsigned count = 0;
      for (unsigned i = 0; i < limit; ++i) {
        BEGIN(alloc);
        __mram_ptr void *p = policy_malloc(size);
        END(alloc);
        if (!p) break;
        save_pointer(tid, i, p);
        benchmark_live[tid] += size;
        unsigned tag[2] __attribute__((aligned(8))) = {
          0x13570000U ^ (tid << 12) ^ (phase << 8) ^ i,
          0x24680000U ^ round
        };
        mram_write(tag, p, 8);
        mram_write(tag, p + size - 8, 8);
        count++;
      }
      barrier_wait(&policy_barrier);
      if (VALIDATE) policy_verify(tid);
      barrier_wait(&policy_barrier);
      if (tid == 0) snapshot(phase * 3 + 1, round);
      barrier_wait(&policy_barrier);
      /* Identical sparse trace for every slab size: keep indices 0,16,32,...
         until a second snapshot, so partially occupied spans stay observable. */
      for (unsigned j = 0; j < count; ++j) {
        unsigned i = count - 1 - j;
        if (WORKLOAD == 2 && i % 16 == 0) continue;
        check_and_free(tid, i, size, phase, round);
      }
      barrier_wait(&policy_barrier);
      if (VALIDATE) policy_verify(tid);
      barrier_wait(&policy_barrier);
      if (tid == 0) snapshot(phase * 3 + 2, round);
      barrier_wait(&policy_barrier);
      if (WORKLOAD == 2) {
        for (unsigned i = 0; i < count; i += 16)
          check_and_free(tid, i, size, phase, round);
        barrier_wait(&policy_barrier);
        if (VALIDATE) policy_verify(tid);
        barrier_wait(&policy_barrier);
        if (tid == 0) snapshot(3, round);
        barrier_wait(&policy_barrier);
      }
    }
  }
  END(work);
  BEGIN(drain);
  policy_drain(tid);
  barrier_wait(&policy_barrier);
  END(drain);
  if (tid == 0) {
    END(total);
    assert(policy_backend.used == 0 && getMetadata(policy_buddy, 0, 0) == 3);
    for (unsigned t = 0; t < NR_TASKLETS; ++t) {
      assert(policy_stats[t].live_slots == 0 && benchmark_live[t] == 0);
      policy_verify(t);
    }
    snapshot(99, 0);
    policy_report_meta[0] = (unsigned)__HEAP_POINTER;
    policy_report_meta[1] = sizeof(policy_caches);
    policy_report_meta[2] = sizeof(policy_stats);
    policy_report_meta[3] = snapshot_count;
    policy_report_meta[4] = NR_TASKLETS;
    __asm__ volatile("policy_report_ready_%=:" :: "r"(policy_stats),
          "r"(&policy_backend), "r"(snapshots), "r"(policy_report_meta) : "memory");
  }
  return 0;
}
