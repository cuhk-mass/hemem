#ifndef HEMEM_LRU_MODIFIED_H
#define HEMEM_LRU_MODIFIED_H

#include <pthread.h>
#include <stdint.h>
#include <inttypes.h>

#include "hemem.h"
#include "paging.h"


struct modified_lru_node {
  struct hemem_page *page;
  uint64_t framenum;
  struct modified_lru_node *next, *prev;
};

struct modified_lru_list {
  struct modified_lru_node *first;
  struct modified_lru_node *last;
  size_t numentries;
};

void modified_lru_list_add(struct modified_lru_list *list, struct modified_lru_node *node);
struct modified_lru_node* modified_lru_list_remove(struct modified_lru_list *list);

uint64_t lru_modified_allocate_page();
void *lru_modified_kswapd();
void lru_modified_pagefault(struct hemem_page *page);
void lru_modified_init(void);

#endif /*  HEMEM_LRU_MODIFIED_H  */
