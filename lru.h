#ifndef HEMEM_LRU_H
#define HEMEM_LRU_H

#include <pthread.h>
#include <stdint.h>
#include <inttypes.h>

#include "hemem.h"
#include "paging.h"

struct lru_node {
  struct hemem_page *page;
  uint64_t framenum;
  struct lru_node *next, *prev;
  struct lru_list *list;
};

struct lru_list {
  struct lru_node *first;
  struct lru_node *last;
  size_t numentries;
  pthread_mutex_t list_lock;
};

void *lru_kswapd();
struct hemem_page* lru_pagefault(void);
void lru_init(void);
void lru_remove_page(struct hemem_page *page);
void lru_stats();

#endif /*  HEMEM_LRU_MODIFIED_H  */
