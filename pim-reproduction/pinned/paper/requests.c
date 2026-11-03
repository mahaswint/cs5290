/* Paper fixed-size allocation microbenchmarks and a separately labelled
 * allocator-only KV-growth replay. Application records live in MRAM. */
#include "harness.h"
struct RequestRecord { uint32_t pointer; unsigned tag; };
PAPER_STORAGE(struct RequestRecord,request_records,NR_TASKLETS*REQUEST_COUNT);
static unsigned payload_word(unsigned tag,unsigned word) {
  return (tag+1)*0x9e3779b1U ^ (word+1)*0x85ebca6bU;
}
static struct RequestRecord record_read(unsigned index) {
  struct RequestRecord r __attribute__((aligned(8)));
  mram_read(&request_records[me()*REQUEST_COUNT+index],&r,8);return r;
}
static void payload_write(struct RequestRecord r) {
  unsigned words[64] __attribute__((aligned(8)));
  for(unsigned offset=0;offset<REQUEST_BYTES;offset+=sizeof(words)) {
    unsigned n=REQUEST_BYTES-offset;if(n>sizeof(words))n=sizeof(words);
    for(unsigned w=0;w<n/4;++w)words[w]=payload_word(r.tag,offset/4+w);
    mram_write(words,(__mram_ptr void *)r.pointer+offset,n);
  }
}
static void request_allocate(unsigned index) {
  struct RequestRecord r __attribute__((aligned(8)));
  r.pointer=(uint32_t)bench_alloc(REQUEST_BYTES);r.tag=me()*REQUEST_COUNT+index;
  mram_write(&r,&request_records[r.tag],8);
  if(KV_GROWTH)payload_write(r);
}
static __attribute__((noinline)) void perform_work(void) {
  BEGIN(work);
  for(unsigned i=WARM_REQUESTS;i<REQUEST_COUNT;++i)request_allocate(i);
  barrier_wait(&policy_barrier);END(work);
}
static void app_check(void) {
  /* Microbenchmark payload writes occur after timing. Write every live object
   * before reading any, checking preservation while all allocations coexist. */
  if(!KV_GROWTH)for(unsigned i=0;i<REQUEST_COUNT;++i)payload_write(record_read(i));
  for(unsigned i=0;i<REQUEST_COUNT;++i) {
    struct RequestRecord r=record_read(i);assert(r.tag==me()*REQUEST_COUNT+i);
    unsigned words[64] __attribute__((aligned(8)));
    for(unsigned offset=0;offset<REQUEST_BYTES;offset+=sizeof(words)) {
      unsigned n=REQUEST_BYTES-offset;if(n>sizeof(words))n=sizeof(words);
      mram_read((__mram_ptr void *)r.pointer+offset,words,n);
      for(unsigned w=0;w<n/4;++w)assert(words[w]==payload_word(r.tag,offset/4+w));
    }
  }
}
static __attribute__((noinline)) void perform_drain(void) {
  BEGIN(drain);
  for(unsigned i=0;i<REQUEST_COUNT;++i)bench_free((__mram_ptr void *)record_read(i).pointer,REQUEST_BYTES);
  policy_drain(me());barrier_wait(&policy_barrier);END(drain);
}
int main(void) {
  unsigned tid=me();
  if(!tid) { mem_reset();BEGIN(total);BEGIN(init);policy_init(HEAP_BYTES);PAPER_BIND(request_records); }
  barrier_wait(&policy_barrier);
  for(unsigned i=0;i<WARM_REQUESTS;++i)request_allocate(i);
  barrier_wait(&policy_barrier);if(!tid)init_end();barrier_wait(&policy_barrier);
  perform_work();if(!tid)work_end();barrier_wait(&policy_barrier);
  app_check();if(VALIDATE)policy_verify(tid);barrier_wait(&policy_barrier);
  perform_drain();if(!tid)final_report();return 0;
}
