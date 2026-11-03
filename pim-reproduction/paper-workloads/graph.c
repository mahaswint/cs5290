/* Paper-style paged adjacency / growing power-of-two arrays. The faimGraph
 * variant additionally performs arbitrary-edge deletion with tail compaction.
 * Graph structure lives in MRAM; only aligned DMA buffers live in WRAM.
 */
#include "harness.h"
struct PaperVertex { uint32_t pointer;unsigned degree,capacity,padding; };
struct PaperBlock { uint32_t previous;unsigned count,neighbors[62]; };
PAPER_STORAGE(struct PaperVertex,paper_vertices,PAPER_VERTICES);
static struct PaperVertex vertex_read(unsigned u) {
  struct PaperVertex v __attribute__((aligned(8)));mram_read(&paper_vertices[u],&v,16);return v;
}
static void vertex_write(unsigned u,struct PaperVertex *v) { mram_write(v,&paper_vertices[u],16); }
static unsigned graph_signature(unsigned u,unsigned v) { return ((u+1)*0x9e3779b1U)^((v+1)*0x85ebca6bU); }
static unsigned array_read(uint32_t pointer,unsigned i) {
  unsigned pair[2] __attribute__((aligned(8)));mram_read((__mram_ptr void *)pointer+((i&~1U)*4),pair,8);return pair[i&1];
}
static void array_write(uint32_t pointer,unsigned i,unsigned value) {
  unsigned pair[2] __attribute__((aligned(8)));__mram_ptr void *p=(__mram_ptr void *)pointer+((i&~1U)*4);
  mram_read(p,pair,8);pair[i&1]=value;mram_write(pair,p,8);
}
static void graph_insert(unsigned u,unsigned destination) {
  struct PaperVertex v __attribute__((aligned(8)))=vertex_read(u);
#if PAPER_KIND==1
  struct PaperBlock b __attribute__((aligned(8)));
  if(!v.pointer || v.degree%62==0) {
    __mram_ptr void *p=bench_alloc(256);memset(&b,0,sizeof(b));b.previous=v.pointer;v.pointer=(uint32_t)p;
  } else mram_read((__mram_ptr void *)v.pointer,&b,256);
  assert(b.count<62);b.neighbors[b.count++]=destination;mram_write(&b,(__mram_ptr void *)v.pointer,256);
#else
  if((v.degree+1)*4>v.capacity) {
    unsigned capacity=v.capacity ? v.capacity*2 : 64;
    __mram_ptr void *p=bench_alloc(capacity);
    unsigned buffer[64] __attribute__((aligned(8)));
    unsigned bytes=(v.degree*4+7)&~7U;
    for(unsigned offset=0;offset<bytes;offset+=256) {
      unsigned n=bytes-offset;if(n>256)n=256;
      mram_read((__mram_ptr void *)v.pointer+offset,buffer,n);mram_write(buffer,p+offset,n);
    }
    if(v.pointer)bench_free((__mram_ptr void *)v.pointer,v.capacity);
    v.pointer=(uint32_t)p;v.capacity=capacity;
  }
  array_write(v.pointer,v.degree,destination);
#endif
  v.degree++;vertex_write(u,&v);
}
static void graph_delete(unsigned u,unsigned destination) {
  struct PaperVertex v __attribute__((aligned(8)))=vertex_read(u);assert(v.degree && v.pointer);
#if PAPER_KIND==1
  struct PaperBlock tail __attribute__((aligned(8))),found __attribute__((aligned(8)));
  mram_read((__mram_ptr void *)v.pointer,&tail,256);
  uint32_t p=v.pointer;unsigned index=0,hit=0;
  while(p) {
    mram_read((__mram_ptr void *)p,&found,256);
    for(index=0;index<found.count;++index)if(found.neighbors[index]==destination){hit=1;break;}
    if(hit)break;p=found.previous;
  }
  assert(hit);unsigned replacement=tail.neighbors[tail.count-1];tail.count--;
  if(p==v.pointer) { if(index<tail.count)tail.neighbors[index]=replacement; }
  else { found.neighbors[index]=replacement;mram_write(&found,(__mram_ptr void *)p,256); }
  if(!tail.count) { uint32_t old=v.pointer;v.pointer=tail.previous;bench_free((__mram_ptr void *)old,256); }
  else mram_write(&tail,(__mram_ptr void *)v.pointer,256);
#else
  unsigned index=0;while(index<v.degree && array_read(v.pointer,index)!=destination)index++;
  assert(index<v.degree);array_write(v.pointer,index,array_read(v.pointer,v.degree-1));
#endif
  v.degree--;vertex_write(u,&v);
}
static void app_init(void) {
  struct PaperVertex empty __attribute__((aligned(8)))={0};
  for(unsigned u=me();u<PAPER_VERTICES;u+=NR_TASKLETS)vertex_write(u,&empty);
}
static __attribute__((noinline)) void apply_op(unsigned i) {
  struct PaperOp op __attribute__((aligned(8)));input_read(&paper_ops[i],&op,16);
  assert(op.index%NR_TASKLETS==me());
  if(op.kind==3)graph_insert(op.index,op.value);else{assert(op.kind==4);graph_delete(op.index,op.value);}
}
static void check_neighbor(unsigned u,unsigned destination,unsigned *checksum) {
  assert(destination<PAPER_VERTICES);*checksum+=graph_signature(u,destination);
#ifdef HOST_TEST
  host_expect_neighbor(u,destination);
#endif
}
static __attribute__((noinline)) void app_check(void) {
  unsigned checksum=0,total=0;
  for(unsigned u=me();u<PAPER_VERTICES;u+=NR_TASKLETS) {
    struct PaperVertex v __attribute__((aligned(8)))=vertex_read(u);
    unsigned expected[2] __attribute__((aligned(8)));input_read(&paper_expected_degrees[u],expected,8);
    assert(v.degree==expected[0]);unsigned count=0;
#if PAPER_KIND==1
    uint32_t p=v.pointer;
    while(p) {
      struct PaperBlock b __attribute__((aligned(8)));mram_read((__mram_ptr void *)p,&b,256);
      assert(b.count && b.count<=62);
      for(unsigned i=0;i<b.count;++i)check_neighbor(u,b.neighbors[i],&checksum);
      count+=b.count;assert(count<=v.degree);p=b.previous;
    }
#else
    for(unsigned i=0;i<v.degree;++i)check_neighbor(u,array_read(v.pointer,i),&checksum);
    count=v.degree;
#endif
    assert(count==v.degree);total+=count;
  }
  assert(total==paper_expected_edges[me()] && checksum==paper_expected_sums[me()]);
}
static __attribute__((noinline)) void app_destroy(void) {
  for(unsigned u=me();u<PAPER_VERTICES;u+=NR_TASKLETS) {
    struct PaperVertex v __attribute__((aligned(8)))=vertex_read(u);
#if PAPER_KIND==1
    uint32_t p=v.pointer;
    while(p) { unsigned links[2] __attribute__((aligned(8)));mram_read((__mram_ptr void *)p,links,8);bench_free((__mram_ptr void *)p,256);p=links[0]; }
#else
    if(v.pointer)bench_free((__mram_ptr void *)v.pointer,v.capacity);
#endif
  }
}
#include "driver.c"
