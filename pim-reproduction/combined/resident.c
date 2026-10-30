/* Stable pool, scattered object replacement, no periodic full-pool teardown.
 * Initialization and destruction are included in total, outside work time.
 * Payload tags deliberately stay constant across rounds so surviving objects
 * can be checked without per-object WRAM bookkeeping.
 */
#include "benchmark_helpers.c"
int main(void) {
  unsigned tid = me();
  if (!tid) { mem_reset(); BEGIN(total); BEGIN(init); policy_init(HEAP_BYTES); }
  barrier_wait(&policy_barrier);
  for (unsigned i = 0; i < BURST; ++i) allocate_slot(i, 256, 0, 0);
  barrier_wait(&policy_barrier);
  if (!tid) init_end();
  barrier_wait(&policy_barrier);
  BEGIN(work);
  for (unsigned phase = 0; phase < 4; ++phase) {
    for (unsigned round = 0; round < ROUNDS; ++round) {
      for (unsigned j = 0; j < BURST / 2; ++j) free_slot(slot(j, phase, round), 256, 0, 0);
      barrier_wait(&policy_barrier);
      if (!tid) snapshot(2 * phase + 1, round);
      barrier_wait(&policy_barrier);
      for (unsigned i = 0; i < BURST; ++i) {
        __mram_ptr void *p = load_pointer(tid, i);
        if (p) check_payload(p, 256, i, 0, 0);
      }
      for (unsigned j = 0; j < BURST / 2; ++j) allocate_slot(slot(j, phase, round), 256, 0, 0);
      barrier_wait(&policy_barrier);
      if (VALIDATE) policy_verify(tid);
      barrier_wait(&policy_barrier);
      if (!tid) snapshot(2 * phase + 2, round);
      barrier_wait(&policy_barrier);
    }
  }
  END(work);
  BEGIN(drain);
  for (unsigned j = 0; j < BURST; ++j) free_slot(slot(j, 0, 0), 256, 0, 0);
  policy_drain(tid); barrier_wait(&policy_barrier); END(drain);
  if (!tid) final_report();
  return 0;
}
