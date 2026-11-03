/* Same-tasklet MRAM objects; identical application work for all span sizes. */
#include "harness.h"
struct Object { uint32_t pointer;unsigned size,tag,padding; };
PAPER_STORAGE(struct Object,object_table,NR_TASKLETS*OBJECTS);
static unsigned rng[NR_TASKLETS],sequence[NR_TASKLETS];
BARRIER_INIT(app_barrier, NR_TASKLETS);
static unsigned random_word(void) {
  unsigned *x=&rng[me()];*x^=*x<<13;*x^=*x>>17;*x^=*x<<5;return *x;
}
static unsigned request_size(unsigned i) {
#if BENCH_WORKLOAD==3
  static const unsigned sizes[6]={16,2048,2049,4096,4097,8192};return sizes[i%6];
#else
  (void)i;return OBJECT_SIZE;
#endif
}
static struct Object load(unsigned i) {
  struct Object o __attribute__((aligned(8)));mram_read(&object_table[me()*OBJECTS+i],&o,16);return o;
}
static void save(unsigned i,struct Object *o) { mram_write(o,&object_table[me()*OBJECTS+i],16); }
static void check(struct Object *o) {
  unsigned data[2] __attribute__((aligned(8)));
  mram_read((__mram_ptr void *)o->pointer,data,8);assert(data[0]==o->tag && data[1]==~o->tag);
  mram_read((__mram_ptr void *)o->pointer+((o->size-8)&~7U),data,8);assert(data[0]==o->tag && data[1]==~o->tag);
}
static __attribute__((noinline)) void allocate(unsigned i) {
  struct Object o __attribute__((aligned(8)))=load(i);assert(!o.pointer);
  unsigned size=request_size(i);__mram_ptr void *p=bench_alloc(size);
  o.pointer=(uint32_t)p;o.size=size;o.tag=++sequence[me()];save(i,&o);
  unsigned data[2] __attribute__((aligned(8)))={o.tag,~o.tag};mram_write(data,p,8);
  mram_write(data,p+((size-8)&~7U),8);
}
static __attribute__((noinline)) void object_release(unsigned i) {
  struct Object o __attribute__((aligned(8)))=load(i);assert(o.pointer);check(&o);
  bench_free((__mram_ptr void *)o.pointer,o.size);o.pointer=0;save(i,&o);
}
static __attribute__((noinline)) void perform_work(void) {
  BEGIN(work);
#if BENCH_WORKLOAD==1
  for(unsigned n=0;n<OPERATIONS;++n) { unsigned i=random_word()&(OBJECTS-1);object_release(i);allocate(i); }
#elif BENCH_WORKLOAD==2
  for(unsigned i=0;i<OBJECTS;++i)allocate(i);
  for(unsigned j=0;j<OBJECTS;++j) { unsigned i=(j*37+TRACE_SEED)&(OBJECTS-1);if(i%16)object_release(i); }
  policy_drain(me()); /* Remove empty spans to expose bytes pinned by survivors. */
#else
  for(unsigned round=0;round<REPEATS;++round) {
    for(unsigned i=0;i<OBJECTS;++i)allocate(i);
    for(unsigned j=0;j<OBJECTS;++j)object_release((j*37+TRACE_SEED)&(OBJECTS-1));
  }
#endif
  barrier_wait(&app_barrier);END(work);
}
static __attribute__((noinline)) void app_check(void) {
  unsigned bytes=0,count=0,expected=0,expected_count=0;
  for(unsigned i=0;i<OBJECTS;++i) {
    struct Object o __attribute__((aligned(8)))=load(i);
    unsigned live=BENCH_WORKLOAD==1 || (BENCH_WORKLOAD==2 && i%16==0);
    assert(!!o.pointer==live);
    if(live) { check(&o);assert(o.size==request_size(i));bytes+=o.size;count++;expected+=request_size(i);expected_count++; }
  }
  assert(bytes==expected && count==expected_count && bytes==benchmark_live[me()]);
}
static __attribute__((noinline)) void perform_drain(void) {
  BEGIN(drain);
  for(unsigned i=0;i<OBJECTS;++i) { struct Object o __attribute__((aligned(8)))=load(i);if(o.pointer)object_release(i); }
  policy_drain(me());barrier_wait(&app_barrier);END(drain);
}
int main(void) {
  unsigned tid=me();
  if(!tid) { mem_reset();BEGIN(total);BEGIN(init);policy_init(HEAP_BYTES);PAPER_BIND(object_table); }
  barrier_wait(&app_barrier);
  struct Object zero __attribute__((aligned(8)))={0};
  for(unsigned i=0;i<OBJECTS;++i)save(i,&zero);
  rng[tid]=(TRACE_SEED^((tid+1)*0x9e3779b9U))|1U;
#if BENCH_WORKLOAD==1
  for(unsigned i=0;i<OBJECTS;++i)allocate(i);
#endif
  barrier_wait(&app_barrier);if(!tid)init_end();barrier_wait(&app_barrier);
  perform_work();if(!tid)work_end();barrier_wait(&app_barrier);app_check();
  if(VALIDATE)policy_verify(tid);
  barrier_wait(&app_barrier);perform_drain();if(!tid)final_report();return 0;
}
