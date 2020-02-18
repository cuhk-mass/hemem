#ifndef HEMEM_H

#define HEMEM_H

#include <pthread.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#include "paging.h"
#include "lru.h"
#include "simple.h"
#include "lru_modified.h"
#include "timer.h"

#define NVMSIZE   (256L * (1024L * 1024L * 1024L))
#define DRAMSIZE  (8L * (1024L * 1024L * 1024L))

#define DRAMPATH "/dev/dax0.0"
#define NVMPATH "/dev/dax1.0"

//#define PAGE_SIZE (1024 * 1024 * 1024)
//#define PAGE_SIZE (2 * (1024 * 1024))
#define BASEPAGE_SIZE (4 * 1024)
#define HUGEPAGE_SIZE (2 * 1024 * 1024)
#define PAGE_SIZE HUGEPAGE_SIZE

#define FASTMEM_PAGES ((DRAMSIZE) / (PAGE_SIZE))
#define SLOWMEM_PAGES   ((NVMSIZE) / (PAGE_SIZE))

//#define LOG(...)	printf(__VA_ARGS__)
#define LOG(str, ...) while(0) {}


FILE *timef;
#define LOG_TIME(str, ...) fprintf(timef, str, __VA_ARGS__)

#if defined (ALLOC_LRU)
  #define pagefault(...) lru_pagefault(__VA_ARGS__)
  #define paging_init(...) lru_init(__VA_ARGS__)
#elif defined (ALLOC_SIMPLE)
  #define pagefault(...) simple_pagefault(__VA_ARGS__)
  #define paging_init(...) simple_init(__VA_ARGS__)
#elif defined (ALLOC_LRU_MODIFIED)
  #define pagefault(...) lru_modified_pagefault(__VA_ARGS__)
  #define paging_init(...) lru_modified_init(__VA_ARGS__)
#endif


extern uint64_t base;
extern int devmemfd;

struct hemem_page {
  uint64_t va;
  uint64_t devdax_offset;
  bool in_dram;
  bool migrating;
  pthread_mutex_t page_lock;

  struct hemem_page *next, *prev;
};

struct page_list {
  struct hemem_page *first, *last;
  size_t numentries;
};

void hemem_init();
void* hemem_mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset);
int hemem_munmap(void* addr, size_t length);
void *handle_fault();
void hemem_migrate_up(struct hemem_page *page, uint64_t dram_offset);
void hemem_migrate_down(struct hemem_page *page, uint64_t nvm_offset);
void hemem_wp_page(struct hemem_page *page, bool protect);

uint64_t hemem_va_to_pa(uint64_t va);
void hemem_clear_accessed_bit(uint64_t va);
int hemem_get_accessed_bit(uint64_t va);

#endif /* HEMEM_H */
