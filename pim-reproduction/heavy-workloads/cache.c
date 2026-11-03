/* Variable-size LRU write-cache replay. Metadata stays outside buffer payloads.
 * Exact source request lengths are retained, including direct-bypass requests.
 */
#include "harness.h"
struct CacheEntry { uint32_t pointer;unsigned key,size,tag,touch; };
static struct CacheEntry cache_entries[NR_TASKLETS][POOL_SIZE];
static unsigned cache_clock[NR_TASKLETS],cache_counts[NR_TASKLETS][6];
static unsigned payload_word(unsigned key,unsigned tag,unsigned index) {
  return key^tag^(index*0x9e3779b1U);
}
static void payload_write(struct CacheEntry *e) {
  unsigned buffer[64] __attribute__((aligned(8)));
  for(unsigned offset=0;offset<e->size;offset+=256) {
    unsigned n=e->size-offset;if(n>256)n=256;
    for(unsigned j=0;j<n/4;++j)buffer[j]=payload_word(e->key,e->tag,offset/4+j);
    mram_write(buffer,(__mram_ptr void *)e->pointer+offset,n);
  }
}
static void payload_check(struct CacheEntry *e) {
  unsigned buffer[64] __attribute__((aligned(8)));
  for(unsigned offset=0;offset<e->size;offset+=256) {
    unsigned n=e->size-offset;if(n>256)n=256;
    mram_read((__mram_ptr void *)e->pointer+offset,buffer,n);
    for(unsigned j=0;j<n/4;++j)assert(buffer[j]==payload_word(e->key,e->tag,offset/4+j));
  }
}
static void cache_release(struct CacheEntry *e,unsigned timed) {
  payload_check(e);bench_free((__mram_ptr void *)e->pointer,e->size);e->pointer=0;
  cache_counts[me()][1]++;if(timed)cache_counts[me()][4]++;
}
static __attribute__((noinline)) void apply_op(unsigned i) {
  unsigned tid=me(),timed=i>=paper_work_starts[tid];
  struct PaperOp op __attribute__((aligned(8)));input_read(&paper_ops[i],&op,16);
  assert(((op.index^(op.index>>16))%NR_TASKLETS)==tid);
  struct CacheEntry *entry=0,*empty=0,*oldest=0;
  for(unsigned j=0;j<POOL_SIZE;++j) {
    struct CacheEntry *e=&cache_entries[tid][j];
    if(!e->pointer){empty=e;continue;}
    if(e->key==op.index)entry=e;
    if(!oldest || e->touch<oldest->touch)oldest=e;
  }
  if(entry && entry->size!=op.value){cache_release(entry,timed);empty=entry;entry=0;}
  if(entry) {
    payload_check(entry);cache_counts[tid][2]++;if(timed)cache_counts[tid][5]++;
    if(op.kind){entry->tag=op.tag;payload_write(entry);}else assert(entry->tag==op.tag);
  } else {
    entry=empty ? empty : oldest;assert(entry);
    if(entry->pointer)cache_release(entry,timed);
    entry->pointer=(uint32_t)bench_alloc(op.value);entry->key=op.index;
    entry->size=op.value;entry->tag=op.tag;payload_write(entry);
    cache_counts[tid][0]++;if(timed)cache_counts[tid][3]++;
  }
  entry->touch=++cache_clock[tid];
}
static __attribute__((noinline)) void app_check(void) {
  unsigned tid=me(),live=0;
  assert(cache_counts[tid][0]==cache_expected_allocations[tid]);
  assert(cache_counts[tid][1]==cache_expected_frees[tid]);
  assert(cache_counts[tid][2]==cache_expected_hits[tid]);
  assert(cache_counts[tid][3]==cache_expected_work_allocations[tid]);
  assert(cache_counts[tid][4]==cache_expected_work_frees[tid]);
  assert(cache_counts[tid][5]==cache_expected_work_hits[tid]);
  for(unsigned j=0;j<POOL_SIZE;++j) {
    struct CacheEntry *e=&cache_entries[tid][j];if(!e->pointer)continue;
    payload_check(e);unsigned found=0;
    for(unsigned k=cache_expected_offsets[tid];k<cache_expected_offsets[tid+1];++k) {
      struct PaperOp expected=cache_expected_final[k];
      if(expected.index==e->key){assert(expected.value==e->size && expected.tag==e->tag);found++;}
    }
    assert(found==1);live++;
    for(unsigned k=0;k<j;++k)assert(!cache_entries[tid][k].pointer || cache_entries[tid][k].key!=e->key);
  }
  assert(live==cache_expected_live[tid]);
}
static void app_destroy(void) {
  for(unsigned j=0;j<POOL_SIZE;++j) {
    struct CacheEntry *e=&cache_entries[me()][j];
    if(e->pointer){payload_check(e);bench_free((__mram_ptr void *)e->pointer,e->size);e->pointer=0;}
  }
}
static __attribute__((noinline)) void perform_work(unsigned start,unsigned end) {
  BEGIN(work);for(unsigned i=start;i<end;++i)apply_op(i);
  barrier_wait(&policy_barrier);END(work);
}
static __attribute__((noinline)) void perform_drain(void) {
  BEGIN(drain);app_destroy();policy_drain(me());barrier_wait(&policy_barrier);END(drain);
}
int main(void) {
  unsigned tid=me();
  if(!tid){mem_reset();BEGIN(total);BEGIN(init);policy_init(HEAP_BYTES);}
  barrier_wait(&policy_barrier);
  for(unsigned i=paper_offsets[tid];i<paper_work_starts[tid];++i)apply_op(i);
  barrier_wait(&policy_barrier);if(!tid)init_end();barrier_wait(&policy_barrier);
  perform_work(paper_work_starts[tid],paper_offsets[tid+1]);
  if(!tid)work_end();barrier_wait(&policy_barrier);app_check();
  if(VALIDATE)policy_verify(tid);barrier_wait(&policy_barrier);
  perform_drain();if(!tid)final_report();return 0;
}
