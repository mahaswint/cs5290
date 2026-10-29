#ifndef POLICY_HOST_SHIM
#define POLICY_HOST_SHIM
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#define __mram_ptr
static unsigned host_tid;
static unsigned char *host_mram;
static size_t host_mram_size;
static unsigned char host_wram[65536] __attribute__((aligned(8)));
static size_t host_wram_used;
#define DPU_MRAM_HEAP_POINTER ((void *)host_mram)
#define MUTEX_INIT(name) static int name
static unsigned me(void) { return host_tid; }
static void mutex_lock(int name) { (void)name; }
static void mutex_unlock(int name) { (void)name; }
static void *mem_alloc(size_t n) {
  host_wram_used = (host_wram_used + 7) & ~(size_t)7;
  assert(host_wram_used + n <= sizeof(host_wram));
  void *p = host_wram + host_wram_used;
  host_wram_used += n;
  return p;
}
static void mram_read(const void *src, void *dst, size_t n) {
  assert(n >= 8 && n <= 2048 && n % 8 == 0);
  assert((uintptr_t)src % 8 == 0 && (uintptr_t)dst % 8 == 0);
  assert((uintptr_t)src >= (uintptr_t)host_mram);
  assert((uintptr_t)src + n <= (uintptr_t)host_mram + host_mram_size);
  memcpy(dst, src, n);
}
static void mram_write(const void *src, void *dst, size_t n) {
  assert(n >= 8 && n <= 2048 && n % 8 == 0);
  assert((uintptr_t)src % 8 == 0 && (uintptr_t)dst % 8 == 0);
  assert((uintptr_t)dst >= (uintptr_t)host_mram);
  assert((uintptr_t)dst + n <= (uintptr_t)host_mram + host_mram_size);
  memcpy(dst, src, n);
}
#endif
