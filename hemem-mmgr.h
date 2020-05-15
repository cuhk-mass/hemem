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

struct mmgr_node {
  struct hemem_page *page;
  uint64_t accesses, tot_accesses;
  uint64_t offset;
  struct hemem_node *next, *prev;
};

struct mmgr_list {
  struct mmgr_node *first;
  struct mmgr_node *last;
  size_t numentries;
  pthread_mutex_t list_lock;
};

void *mmgr_kswapd(void);
struct hemem_page* mmgr_pagefault();
void mmgr_init(void);
void mmgr_remove_page(struct hemem_page *page);

#endif
