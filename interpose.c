#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <libsyscall_intercept_hook_point.h>
#include <syscall.h>
#include <errno.h>
#define __USE_GNU
#include <dlfcn.h>
#include <pthread.h>
#include <sys/mman.h>

#include "hemem.h"
#include "interpose.h"

void* (*libc_mmap)(void *addr, size_t length, int prot, int flags, int fd, off_t offset) = NULL;
int (*libc_munmap)(void *addr, size_t length) = NULL;
void* (*libc_malloc)(size_t size) = NULL;
void (*libc_free)(void* ptr) = NULL;

void* tmp_mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset)
{
  void *ret;
  //ensure_init();

  //TODO: figure out which mmap calls should go to libc vs hemem
  // non-anonymous mappings should probably go to libc (e.g., file mappings)
  if ((flags & MAP_ANONYMOUS) != MAP_ANONYMOUS) {
    LOG("hemem interpose: calling libc mmap\n");
    return libc_mmap(addr, length, prot, flags, fd, offset);
  }

  if ((flags & MAP_STACK) == MAP_STACK) {
    // pthread mmaps are called with MAP_STACK
    return libc_mmap(addr, length, prot, flags, fd, offset);
  }

  if ((ret = hemem_mmap(addr, length, prot, flags, fd, offset)) == MAP_FAILED) {
    // hemem failed for some reason, try libc
    LOG("hemem mmap failed\n\tmmap(0x%lx, %ld, %x, %x, %d, %ld)\n", (uint64_t)addr, length, prot, flags, fd, offset);
    //return libc_mmap(addr, length, prot, flags, fd, offset);
  }
  return ret;
}


int tmp_munmap(void *addr, size_t length)
{
  //ensure_init();
  
  //TODO: figure out which munmap calls should go to libc vs hemem
  // for now, just call libc munmap because that's all hemem will do anyway
  return libc_munmap(addr, length);
}

static void* bind_symbol(const char *sym)
{
  void *ptr;
  if ((ptr = dlsym(RTLD_NEXT, sym)) == NULL) {
    fprintf(stderr, "hemem memory manager interpose: dlsym failed (%s)\n", sym);
    abort();
  }
  return ptr;
}

static int hook(long syscall_number, long arg0, long arg1, long arg2, long arg3,	long arg4, long arg5,	long *result)
{
	if (syscall_number == SYS_mmap) {
    if (!intercept_this_call) {
      // mmap was not called from a malloc or free call so we probably don't care
      // about it, just let libc handle it normally
      return 1;
    }
		*result = (long)tmp_mmap((void*)arg0, (size_t)arg1, (int)arg2, (int)arg3, (int)arg4, (off_t)arg5);
    return 0;
	} else {
    // ignore non-mmap system calls
		return 1;
	}
}

static __attribute__((constructor)) void init(void)
{
  libc_mmap = bind_symbol("mmap");
  libc_munmap = bind_symbol("munmap");
  libc_malloc = bind_symbol("malloc");
  libc_free = bind_symbol("free");
  intercept_hook_point = hook;

  hemem_init();
}
/*
void* malloc(size_t size)
{
  void* ret;
  old_intercept_this_call = intercept_this_call;
  intercept_this_call = true;
  ret = libc_malloc(size);
  intercept_this_call = old_intercept_this_call;
  return ret;
}

void free(void* ptr)
{
  old_intercept_this_call = intercept_this_call;
  intercept_this_call = true;
  libc_free(ptr);
  intercept_this_call = old_intercept_this_call;

}
*/
