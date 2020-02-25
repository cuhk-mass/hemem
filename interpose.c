#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <errno.h>
#define __USE_GNU
#include <dlfcn.h>
#include <pthread.h>
#include <sys/mman.h>

#include "hemem.h"
#include "interpose.h"

void* (*libc_mmap)(void *addr, size_t length, int prot, int flags, int fd, off_t offset) = NULL;
int (*libc_munmap)(void *addr, size_t length) = NULL;

static void* bind_symbol(const char *sym)
{
  void *ptr;
  if ((ptr = dlsym(RTLD_NEXT, sym)) == NULL) {
    fprintf(stderr, "hemem memory manager interpose: dlsym failed (%s)\n", sym);
    abort();
  }
  return ptr;
}

static void init(void)
{
  libc_mmap = bind_symbol("mmap");
  libc_munmap = bind_symbol("munmap");

  hemem_init();
}

static inline void ensure_init(void)
{
  static volatile uint32_t init_cnt = 0;
  static volatile uint8_t init_done = 0;
  static __thread uint8_t in_init = 0;

  if (init_done == 0) {
    if (in_init) {
      return;
    }

    if (__sync_fetch_and_add(&init_cnt, 1) == 0) {
      in_init = 1;
      init();
      in_init = 0;
      MEM_BARRIER();
      init_done = 1;
    } else {
      while (init_done == 0) {
        pthread_yield();
      }
      MEM_BARRIER();
    }
  }
}

void* mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset)
{
  void *ret;
  ensure_init();

  //TODO: figure out which mmap calls should go to libc vs hemem
  // non-anonymous mappings should probably go to libc (e.g., file mappings)
  if ((flags & MAP_ANONYMOUS) != MAP_ANONYMOUS) {
    LOG("hemem interpose: calling libc mmap\n");
    return libc_mmap(addr, length, prot, flags, fd, offset);
  }

  if ((ret = hemem_mmap(addr, length, prot, flags, fd, offset)) == MAP_FAILED) {
    // hemem failed for some reason, try libc
    LOG("hemem mmap failed\n\tmmap(0x%lx, %ld, %x, %x, %d, %ld)\n", (uint64_t)addr, length, prot, flags, fd, offset);
    //return libc_mmap(addr, length, prot, flags, fd, offset);
  }
  return ret;
}


int munmap(void *addr, size_t length)
{
  ensure_init();
  
  //TODO: figure out which munmap calls should go to libc vs hemem
  // for now, just call libc munmap because that's all hemem will do anyway
  return libc_munmap(addr, length);
}
