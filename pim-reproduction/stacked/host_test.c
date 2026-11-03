#define _GNU_SOURCE
#include "common.h"
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#include <signal.h>
#include "allocator.c"
struct Object { void *p;unsigned size,tag; };
static struct Object objects[NR_TASKLETS][64];
static unsigned random_state=0x12345678U;
static unsigned random_word(void) { random_state^=random_state<<13;random_state^=random_state>>17;random_state^=random_state<<5;return random_state; }
static void check_all(void) {
  for(unsigned t=0;t<NR_TASKLETS;++t)for(unsigned i=0;i<64;++i) {
    struct Object *o=&objects[t][i];if(!o->p)continue;
    for(unsigned b=0;b<o->size;++b)assert(((unsigned char *)o->p)[b]==(unsigned char)o->tag);
  }
}
static void allocate(unsigned t,unsigned i,unsigned size) {
  host_tid=t;struct Object *o=&objects[t][i];assert(!o->p);
  o->p=policy_malloc(size);assert(o->p && (uintptr_t)o->p%8==0);o->size=size;o->tag=random_word();
  for(unsigned u=0;u<NR_TASKLETS;++u)for(unsigned j=0;j<64;++j) {
    struct Object *other=&objects[u][j];if(other==o || !other->p)continue;
    assert((char *)o->p+size<=(char *)other->p || (char *)other->p+other->size<=(char *)o->p);
  }
  memset(o->p,o->tag,size);
}
static void release(unsigned t,unsigned i) { host_tid=t;assert(objects[t][i].p);policy_free(objects[t][i].p);objects[t][i].p=NULL; }
static void empty_heap(unsigned heap) {
  for(unsigned t=0;t<NR_TASKLETS;++t) { host_tid=t;policy_drain(t);policy_verify(t);assert(!policy_stats[t].live_slots && !policy_stats[t].retained); }
  assert(!policy_backend.used && getMetadata(policy_buddy,0,0)==3);
  for(unsigned i=0;i<heap/4096;++i) {
    struct PolicyOwner entry __attribute__((aligned(8)));mram_read(&policy_owners[i],&entry,8);assert(!entry.tag && !entry.bytes);
  }
}
static void reset(unsigned heap) {
  memset(objects,0,sizeof(objects));host_wram_used=0;host_tid=0;policy_init(heap);
}
static void invalid_free(void *p,unsigned tid,unsigned offset) {
  pid_t child=fork();assert(child>=0);
  if(!child) { freopen("/dev/null","w",stderr);host_tid=tid;policy_free((char *)p+offset);_exit(0); }
  int status;assert(waitpid(child,&status,0)==child);assert(WIFSIGNALED(status) && WTERMSIG(status)==SIGABRT);
}
int main(void) {
  host_mram_size=8*1024*1024;
  host_mram=mmap(NULL,host_mram_size,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_32BIT,-1,0);
  assert(host_mram!=MAP_FAILED && (uintptr_t)host_mram+host_mram_size<0x80000000U);
  unsigned heap=4*1024*1024;reset(heap);
  /* Many span heads/tail leaves and actual 4 KiB payloads crossing leaves. */
  for(unsigned t=0;t<NR_TASKLETS;++t)for(unsigned i=0;i<32;++i)allocate(t,i,4096);
  check_all();for(unsigned t=0;t<NR_TASKLETS;++t)policy_verify(t);
  for(unsigned t=0;t<NR_TASKLETS;++t)for(unsigned i=0;i<32;++i)release(t,(i*13)%32);
  empty_heap(heap);
  /* Independent full-payload and non-overlap oracle across boundary sizes. */
  const unsigned sizes[]={16,31,64,128,256,1024,2048,2049,3072,4096,4097,8192};
  for(unsigned step=0;step<3000;++step) {
    unsigned t=random_word()%NR_TASKLETS,i=random_word()%64;
    if(objects[t][i].p)release(t,i);else allocate(t,i,sizes[random_word()%12]);
    if(step%97==0) { check_all();for(unsigned u=0;u<NR_TASKLETS;++u)policy_verify(u); }
  }
  check_all();for(unsigned t=0;t<NR_TASKLETS;++t)for(unsigned i=0;i<64;++i)if(objects[t][i].p)release(t,i);
  empty_heap(heap);
  /* Byte-charged budgets, cold-class decay, and sparse pinning. */
  heap=1024*1024;reset(heap);
  for(unsigned i=0;i<32;++i)allocate(0,i,4096);
  for(unsigned i=0;i<32;++i) { release(0,i);assert(policy_stats[0].retained<=EMPTY_BUDGET); }
#if SLAB_BYTES
  unsigned allowed=EMPTY_BUDGET/(SLAB_BYTES/4096);if(allowed>2)allowed=2;
  assert(policy_caches[0][8].empty==allowed);
  assert(policy_stats[0].retained==allowed*(SLAB_BYTES/4096));
#if POLICY==3
  unsigned retained=policy_stats[0].retained;
  for(unsigned i=0;i<4*DECAY_OPS;++i) { void *p=policy_malloc(64);assert(p);policy_free(p); }
  assert(policy_caches[0][8].target==0 && !policy_caches[0][8].empty);
  if(retained)assert(policy_stats[0].idle_trims);
#endif
#endif
  empty_heap(heap);reset(heap);
  for(unsigned i=0;i<32;++i)allocate(0,i,4096);
  for(unsigned i=0;i<32;++i)if(i%16)release(0,i);
  policy_drain(0);check_all();
  assert(policy_backend.used==2*(SLAB_BYTES ? SLAB_BYTES : 4096));
  release(0,0);release(0,16);empty_heap(heap);
  /* Wrong owner, interior address and double free are rejected. */
  for(unsigned size=4096;size<=8192;size*=2) {
    host_tid=0;void *p=policy_malloc(size);assert(p);
    invalid_free(p,1,0);invalid_free(p,0,8);policy_free(p);invalid_free(p,0,0);policy_drain(0);
  }
  empty_heap(heap);
  /* OOM is nonfatal; whole slab capacity is charged including its header. */
  heap=65536;reset(heap);void *pointers[32];unsigned count=0;
  while(count<32) { void *p=policy_malloc(4096);if(!p)break;pointers[count++]=p;memset(p,count,4096); }
  unsigned expected=SLAB_BYTES ? (heap/SLAB_BYTES)*((SLAB_BYTES-24)/4096) : heap/4096;
  assert(count==expected && policy_stats[0].failures==1);
  for(unsigned i=0;i<count;++i)for(unsigned b=0;b<4096;++b)assert(((unsigned char *)pointers[i])[b]==(unsigned char)(i+1));
  for(unsigned i=count;i>0;--i)policy_free(pointers[i-1]);
  empty_heap(heap);
  heap=1024*1024;reset(heap);
  void *root=policy_malloc(heap);assert(root && !policy_malloc(4096));memset(root,0xA5,heap);policy_free(root);empty_heap(heap);
  assert(!policy_malloc(0) && !policy_malloc(heap+1));
#if SLAB_BYTES
  /* Enough free bytes can still lack a contiguous slab. No silent fallback. */
  heap=65536;reset(heap);
  for(unsigned i=0;i<8;++i) { pointers[i]=policy_malloc(8192);assert(pointers[i]); }
  for(unsigned i=0;i<8;i+=2)policy_free(pointers[i]);
  assert(!policy_malloc(4096));
  for(unsigned i=1;i<8;i+=2)policy_free(pointers[i]);empty_heap(heap);
#endif
  munmap(host_mram,host_mram_size);
  puts("PASS: full payloads, intervals, all span owners, byte budget, decay, sparse pinning, invalid frees, capacity, OOM, coalescing");
  return 0;
}
