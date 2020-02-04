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

#define NVMSIZE   (128L * (1024L * 1024L * 1024L))
#define DRAMSIZE  (8L * (1024L * 1024L * 1024L))

#define DRAMPATH "/dev/dax0.0"
#define NVMPATH "/dev/dax1.0"

//#define PAGE_SIZE (1024 * 1024 * 1024)
//#define PAGE_SIZE (2 * (1024 * 1024))
#define PAGE_SIZE (4 * 1024)
#define HUGEPAGE_SIZE (2 * 1024 * 1024)

#define FASTMEM_PAGES ((DRAMSIZE) / (PAGE_SIZE))
#define SLOWMEM_PAGES   ((NVMSIZE) / (PAGE_SIZE))

//#define LOG(...)	printf(__VA_ARGS__)
#define LOG(str, ...) while(0) {}

#ifdef ALLOC_LRU
#define pagefault(...) lru_pagefault(__VA_ARGS__)
#define paging_init(...) lru_init(__VA_ARGS__)
#endif

#ifdef ALLOC_SIMPLE
#define pagefault(...) simple_pagefault(__VA_ARGS__)
#define paging_init(...) simple_init(__VA_ARGS__)
#endif


extern uint64_t base;
int devmemfd;

struct hemem_page {
  uint64_t va;
  uint64_t devdax_offset;
  bool in_dram;
  bool accessed;

  struct hemem_page *next, *prev;
};

struct page_list {
  struct hemem_page *first, *last;
  size_t numentries;
};

extern struct page_list list;

void hemem_init();
void* hemem_mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset);
int hemem_munmap(void* addr, size_t length);
void *handle_fault();
void hemem_migrate_up(struct hemem_page *page, uint64_t dram_offset);
void hemem_migrate_down(struct hemem_page *page, uint64_t nvm_offset);

uint64_t hemem_va_to_pa(uint64_t va);
void hemem_clear_accessed_bit(uint64_t va);
int hemem_get_accessed_bit(uint64_t va);

#endif /* HEMEM_H */
