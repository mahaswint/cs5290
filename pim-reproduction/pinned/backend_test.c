#define _GNU_SOURCE
#include "common.h"
#include <sys/mman.h>
#include "reference_backend.inc"
#include "pinned_backend.inc"
struct Pair { void *reference,*pinned;unsigned size,tag; };
static struct Pair live[128];
static unsigned random_state=0x12345678U;
static unsigned random_word(void) { random_state^=random_state<<13;random_state^=random_state>>17;random_state^=random_state<<5;return random_state; }
static void compare(struct RefBuddyAllocator *r,struct PinBuddyAllocator *p) {
  ref_flushMetadata(r);pin_syncPinnedMetadata(p);
  assert(r->metadataSize==p->metadataSize);
  assert(!memcmp(r->mramMetadataBase,p->mramMetadataBase,r->metadataSize));
  for(int level=0;level<r->numBuddyLevels;++level)
    for(int i=0;i<ref_getNumLeafs(r,level);++i)
      assert(ref_getMetadata(r,level,i)==pin_getMetadata(p,level,i));
}
static void check_payloads(void) {
  for(unsigned i=0;i<128;++i)if(live[i].pinned) {
    struct Pair *o=&live[i];
    assert(*(unsigned *)o->reference==o->tag && *(unsigned *)o->pinned==o->tag);
    assert(*(unsigned *)((char *)o->reference+o->size-4)==~o->tag);
    assert(*(unsigned *)((char *)o->pinned+o->size-4)==~o->tag);
  }
}
int main(void) {
  host_mram_size=2*(32*1024*1024+16384);
  host_mram=mmap(0,host_mram_size,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_32BIT,-1,0);
  assert(host_mram!=MAP_FAILED && (uintptr_t)host_mram+host_mram_size<0x80000000U);
  const unsigned heaps[]={65536,2*1024*1024,32*1024*1024};
  for(unsigned h=0;h<3;++h) {
    unsigned heap=heaps[h];host_wram_used=0;memset(live,0,sizeof(live));
    struct RefBuddyAllocator *r=ref_initBuddyAllocator(heap,4096,TEST_BUFFER,host_mram);
    struct PinBuddyAllocator *p=pin_initBuddyAllocator(heap,4096,TEST_BUFFER,host_mram+heap+16384);
    assert((p->pinnedMetadataWords*4)%8==0);compare(r,p);
    for(unsigned step=0;step<2000;++step) {
      unsigned i=random_word()%128;struct Pair *o=&live[i];
      if(o->pinned) {
        check_payloads();ref_buddyFree(r,o->reference);pin_buddyFree(p,o->pinned);o->pinned=o->reference=0;
      } else {
        unsigned size=(1U<<(12+random_word()%9));if(random_word()&1)size+=8;
        if(size>heap)size=heap;
        void *x=ref_buddyAllocate(r,size),*y=pin_buddyAllocate(p,size);
        assert((x==0)==(y==0));if(!x)continue;
        assert((char *)x-(char *)r->mramMallocableRegionBase==(char *)y-(char *)p->mramMallocableRegionBase);
        for(unsigned j=0;j<128;++j)if(live[j].pinned)
          assert((char *)y+size<=(char *)live[j].pinned || (char *)live[j].pinned+live[j].size<=(char *)y);
        *o=(struct Pair){x,y,size,random_word()};
        *(unsigned *)x=*(unsigned *)y=o->tag;
        *(unsigned *)((char *)x+size-4)=*(unsigned *)((char *)y+size-4)=~o->tag;
      }
      if(step%97==0){check_payloads();compare(r,p);}
    }
    check_payloads();
    for(unsigned i=0;i<128;++i)if(live[i].pinned) {
      ref_buddyFree(r,live[i].reference);pin_buddyFree(p,live[i].pinned);live[i].pinned=0;
    }
    compare(r,p);assert(ref_getMetadata(r,0,0)==3 && pin_getMetadata(p,0,0)==3);
    /* Fill every leaf, fragment the heap, then coalesce all the way to root. */
    static void *xp[8192],*yp[8192];unsigned n=heap/4096;
    for(unsigned i=0;i<n;++i){xp[i]=ref_buddyAllocate(r,4096);yp[i]=pin_buddyAllocate(p,4096);assert(xp[i] && yp[i]);}
    assert(!ref_buddyAllocate(r,4096) && !pin_buddyAllocate(p,4096));
    for(unsigned i=0;i<n;i+=2){ref_buddyFree(r,xp[i]);pin_buddyFree(p,yp[i]);}
    assert(!ref_buddyAllocate(r,8192) && !pin_buddyAllocate(p,8192));compare(r,p);
    for(unsigned i=1;i<n;i+=2){ref_buddyFree(r,xp[i]);pin_buddyFree(p,yp[i]);}
    compare(r,p);
    void *x=ref_buddyAllocate(r,heap),*y=pin_buddyAllocate(p,heap);assert(x && y);
    compare(r,p);ref_buddyFree(r,x);pin_buddyFree(p,y);compare(r,p);
    assert(pin_getMetadata(p,0,0)==3);
  }
  munmap(host_mram,host_mram_size);
  puts("PASS: exact reference offsets/states, disjoint prefix DMA, checkpoints, fragmentation, OOM, root coalescing");
  return 0;
}
