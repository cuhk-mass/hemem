#ifndef HEMEM_LRU_MODIFIED_H
#define HEMEM_LRU_MODIFIED_H

#include <pthread.h>
#include <stdint.h>
#include <inttypes.h>

#include "hemem.h"
#include "paging.h"


struct lru_node {
  struct hemem_page *page;
  uint64_t framenum;
  struct lru_node *next, *prev;
};

struct lru_list {
  struct lru_node *first;
  struct lru_node *last;
  size_t numentries;
};

void *lru_kswapd();
void lru_pagefault(struct hemem_page *page);
void lru_init(void);

#endif /*  HEMEM_LRU_MODIFIED_H  */
