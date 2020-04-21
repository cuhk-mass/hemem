#ifndef HEMEM_MMGR_H
#define HEMEM_MMGR_H

#include <stdint.h>
#include <inttypes.h>
#include <pthread.h>

#include "hemem.h"
#include "paging.h"

#define HEMEM_INTERVAL 1000000ULL // in us

#define HEMEM_FASTFREE    (DRAMSIZE / 10)
#define HEMEM_COOL_RATE   (10ULL * 1024ULL * 1024ULL * 1024ULL)
#define HEMEM_THAW_RATE   (NVMSIZE + DRAMSIZE)

#define FASTMEM_HUGE_PAGES  ((DRAMSIZE) / (HUGEPAGE_SIZE))
#define FASTMEM_BASE_PAGES  ((DRAMSIZE) / (BASEPAGE_SIZE))

#define SLOWMEM_HUGE_PAGES  ((NVMSIZE) / (HUGEPAGE_SIZE))
#define SLOWMEM_BASE_PAGES  ((NVMSIZE) / (BASEPAGE_SIZE))

struct hemem_node {
  struct hemem_page *page;
  bool accessed2;
  uint64_t offset;
  struct hemem_node *next, *prev;
};

struct hemem_list {
  struct hemem_node *first;
  struct hemem_node *last;
  size_t numentries;
};

void *hemem_kswapd(void);
void hemem_pagefault(struct hemem_page *page);
void hemem_mmgr_init(void);

#endif
