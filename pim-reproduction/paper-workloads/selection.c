/* PrIM SEL kernel port: 256 B block caching, odd uint64 predicate (!pred),
 * stable output compaction. Barriers/prefix sums replace the host handshake
 * wrapper. Input/output buffers remain preallocated as in the source kernel.
 */
#include "harness.h"
PAPER_STORAGE(uint64_t,paper_input,INPUT_ITEMS);
PAPER_STORAGE(uint64_t,paper_output,INPUT_ITEMS);
static unsigned select_counts[NR_TASKLETS],select_total;
static unsigned input_value(unsigned i) {
  unsigned x=i^TRACE_SEED;x^=x>>16;x*=0x7feb352dU;x^=x>>15;x*=0x846ca68bU;return x^(x>>16);
}
static __attribute__((noinline)) void selection_kernel(void) {
  unsigned tid=me();
  uint64_t input[32] __attribute__((aligned(8))),output[32] __attribute__((aligned(8)));
  BEGIN(work);
  for(unsigned repeat=0;repeat<REPEATS;++repeat) {
    if(!tid)select_total=0;barrier_wait(&policy_barrier);
    for(unsigned i=tid*32;i<INPUT_ITEMS;i+=NR_TASKLETS*32) {
      mram_read(&paper_input[i],input,256);unsigned n=0;
      for(unsigned j=0;j<32;++j)if(input[j]&1)output[n++]=input[j];
      select_counts[tid]=n;barrier_wait(&policy_barrier);
      unsigned prefix=select_total;for(unsigned t=0;t<tid;++t)prefix+=select_counts[t];
      barrier_wait(&policy_barrier);
      if(n)mram_write(output,&paper_output[prefix],n*8);
      if(!tid)for(unsigned t=0;t<NR_TASKLETS;++t)select_total+=select_counts[t];
      barrier_wait(&policy_barrier);
    }
  }
  END(work);
}
static __attribute__((noinline)) void selection_drain(void) {
  BEGIN(drain);policy_drain(me());barrier_wait(&policy_barrier);END(drain);
}
int main(void) {
  unsigned tid=me();
  if(!tid) { mem_reset();BEGIN(total);BEGIN(init);policy_init(HEAP_BYTES);PAPER_BIND(paper_input);PAPER_BIND(paper_output); }
  barrier_wait(&policy_barrier);
  for(unsigned i=tid;i<INPUT_ITEMS;i+=NR_TASKLETS) {
    unsigned pair[2] __attribute__((aligned(8)))={input_value(i),0};mram_write(pair,&paper_input[i],8);
  }
  barrier_wait(&policy_barrier);if(!tid)init_end();barrier_wait(&policy_barrier);
  selection_kernel();barrier_wait(&policy_barrier);if(!tid)work_end();barrier_wait(&policy_barrier);
  if(!tid) {
    unsigned count=0;
    for(unsigned i=0;i<INPUT_ITEMS;++i)if(input_value(i)&1) {
      unsigned pair[2] __attribute__((aligned(8)));mram_read(&paper_output[count++],pair,8);
      assert(pair[0]==input_value(i) && !pair[1]);
    }
    assert(count==select_total);
  }
  barrier_wait(&policy_barrier);selection_drain();
  if(!tid)final_report();return 0;
}
