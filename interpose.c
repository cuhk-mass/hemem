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
#include <assert.h>

#include "hemem.h"
#include "interpose.h"

void* (*libc_mmap)(void *addr, size_t length, int prot, int flags, int fd, off_t offset) = NULL;
int (*libc_munmap)(void *addr, size_t length) = NULL;
void* (*libc_malloc)(size_t size) = NULL;
void (*libc_free)(void* ptr) = NULL;
int (*libc_madvise)(void* addr, size_t length, int advice);

static int mmap_filter(void *addr, size_t length, int prot, int flags, int fd, off_t offset, uint64_t *result)
{
  //ensure_init();

  //TODO: figure out which mmap calls should go to libc vs hemem
  // non-anonymous mappings should probably go to libc (e.g., file mappings)
  if (((flags & MAP_ANONYMOUS) != MAP_ANONYMOUS) && !((fd == dramfd) || (fd == nvmfd))) {
    //LOG("hemem interpose: calling libc mmap due to non-anonymous, non-devdax mapping\n");
    return 1;
  }

  if ((flags & MAP_STACK) == MAP_STACK) {
    // pthread mmaps are called with MAP_STACK
    //LOG("hemem interpose: calling libc mmap due to stack mapping\n");
    return 1;
  }

  if (((flags & MAP_NORESERVE) == MAP_NORESERVE)) {
    // thread stack is called without swap space reserved, so we can probably ignore these
    LOG("hemem interpose: calling libc mmap due to non-swap space reserved mapping\n");
    return 1;
  }

  if (internal_malloc) {
    return 1;
  }

  if (length < 4096) {
    //LOG("hemem interpose calling libc mmap due to small allocation size\n");
    return 1;
  }

  if (!is_init) {
    //LOG("hemem interpose: calling libc mmap due to hemem init in progress\n");
    return 1;
  }

  if ((fd == dramfd) || (fd == nvmfd)) {
    //LOG("hemem interpose: calling libc mmap due to hemem devdax mapping\n");
    return 1;
  }

  LOG("hemem interpose: calling hemem mmap(0x%lx, %ld, %x, %x, %d, %ld)\n", (uint64_t)addr, length, prot, flags, fd, offset);
  if ((*result = (uint64_t)hemem_mmap(addr, length, prot, flags, fd, offset)) == (uint64_t)MAP_FAILED) {
    // hemem failed for some reason, try libc
    LOG("hemem mmap failed\n\tmmap(0x%lx, %ld, %x, %x, %d, %ld)\n", (uint64_t)addr, length, prot, flags, fd, offset);
  }
  return 0;
}

static int madvise_filter(void* addr, size_t length, int advice, uint64_t *result) {

  if(((uint64_t) addr) & 0xfff || length < 0){
    LOG("madvise interpose: addr not aligned or negative length\n");
    return 1;
  }

  if(advice != MADV_DONTNEED){
    LOG("madvise interpose: advice other than MADV_DONTNEED\n");
    return 1;
  }

  LOG("madvice interpose: calling hemem_madvise\n");
  *result = hemem_madvise(addr, length, advice);
  return 0;
}

/* 
static int munmap_filter(void *addr, size_t length)
{
  //ensure_init();
  
  //TODO: figure out which munmap calls should go to libc vs hemem
  // for now, just call libc munmap because that's all hemem will do anyway
  return libc_munmap(addr, length);
}
*/
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
	  return mmap_filter((void*)arg0, (size_t)arg1, (int)arg2, (int)arg3, (int)arg4, (off_t)arg5, (uint64_t*)result);
	} 
#ifdef COALESCE
  else if (syscall_number == SYS_madvise){
    return madvise_filter((void*) arg0, (size_t) arg1, (int) arg2, (uint64_t*) result);
  } 
#endif
  else {
    // ignore non-mmap/madvise system calls
		return 1;
	}
}

static __attribute__((constructor)) void init(void)
{
  libc_mmap = bind_symbol("mmap");
  libc_munmap = bind_symbol("munmap");
  libc_malloc = bind_symbol("malloc");
  libc_free = bind_symbol("free");
  libc_madvise = bind_symbol("madvise");
  intercept_hook_point = hook;

  hemem_init();
}
/* 
void* malloc(size_t size)
{
  void* ret;
  if(libc_malloc == NULL) {
    libc_malloc = bind_symbol("malloc");
  }
  assert(libc_malloc != NULL);
  ret = libc_malloc(size);
  return ret;
}

void free(void* ptr)
{
  if(libc_free == NULL) {
    libc_free = bind_symbol("free");
  }
  assert(libc_free != NULL);
  libc_free(ptr);
}
*/
