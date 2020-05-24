#ifndef HEMEM_LRU_H
#define HEMEM_LRU_H

#include <pthread.h>
#include <stdint.h>
#include <inttypes.h>

#include "hemem.h"
#include "paging.h"


#define KSCAND_INTERVAL   (10000) // in us (10ms)
#define KSWAPD_INTERVAL   (1000000) // in us (1s)
#define KSWAPD_MIGRATE_RATE  (10UL * 1024UL * 1024UL * 1024UL) // 10GB

struct lru_node {
  struct hemem_page *page;
  uint64_t framenum;
  uint64_t accesses, tot_accesses;
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
struct hemem_page* lru_pagefault_unlocked(void);
void lru_init(void);
void lru_remove_page(struct hemem_page *page);
void lru_stats();
void lru_lock();
void lru_unlock();


#endif /*  HEMEM_LRU_MODIFIED_H  */
