#include <stdlib.h>
#include <pthread.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdbool.h>
#include <pthread.h>
#include <assert.h>
#include <sys/time.h>
#include <unistd.h>

#include "hemem.h"
#include "paging.h"
#include "lru.h"
#include "timer.h"

static struct lru_list active_list;
static struct lru_list inactive_list;
static struct lru_list nvm_active_list;
static struct lru_list nvm_inactive_list;
static pthread_mutex_t global_lock = PTHREAD_MUTEX_INITIALIZER;
static bool __thread in_kswapd = false;
static bool dram_bitmap[FASTMEM_PAGES];
static bool nvm_bitmap[SLOWMEM_PAGES];

static void lru_migrate_down(struct lru_node *n, uint64_t i)
{
  pthread_mutex_lock(&(n->page->page_lock));
  LOG("hemem: lru_migrate_down: migrating %lx to NVM frame %lu\n", n->page->va, i);
  n->page->migrating = true;
  hemem_wp_page(n->page, true);
  hemem_migrate_down(n->page, i);
  n->framenum = i;
  n->page->migrating = false; 
  LOG("hemem: lru_migrate_down: done migrating to NVM\n");
  pthread_mutex_unlock(&(n->page->page_lock));
}

static void lru_migrate_up(struct lru_node *n, uint64_t i)
{
  pthread_mutex_lock(&(n->page->page_lock));
  LOG("hemem: lru_migrate_up: migrating %lx to DRAM frame %lu\n", n->page->va, i);
  n->page->migrating = true;
  hemem_wp_page(n->page, true);
  hemem_migrate_up(n->page, i);
  n->framenum = i;
  n->page->migrating = false;
  LOG("hemem: lru_migrate_up: done migrating to DRAM\n");
  pthread_mutex_unlock(&(n->page->page_lock));
}

static void lru_list_add(struct lru_list *list, struct lru_node *node)
{
  assert(node->prev == NULL);
  node->next = list->first;
  if (list->first != NULL) {
    assert(list->first->prev == NULL);
    list->first->prev = node;
  }
  else {
    assert(list->last == NULL);
    assert(list->numentries == 0);
    list->last = node;
  }

  list->first = node;
  list->numentries++;
}


static struct lru_node* lru_list_remove(struct lru_list *list)
{
  struct lru_node *ret = list->last;

  if (ret == NULL) {
    assert(list->numentries == 0);
    return ret;
  }

  list->last = ret->prev;
  if (list->last != NULL) {
    list->last->next = NULL;
  }
  else {
    list->first = NULL;
  }

  ret->prev = NULL;
  ret->next = NULL;
  assert(list->numentries > 0);
  list->numentries--;
  return ret;
}


static void shrink_caches(struct lru_list *active, struct lru_list *inactive)
{
  size_t nr_pages = 32;

  // find cold pages and move to inactive list
  while (nr_pages > 0 && active->numentries > 0) {
    struct lru_node *n = lru_list_remove(active);
    if (hemem_get_accessed_bit(n->page->va) == HEMEM_ACCESSED_FLAG) {
      // give accessed pages another go-around in active list
      hemem_clear_accessed_bit(n->page->va);
      lru_list_add(active, n);
    }
    else {
      // found a cold page, put it on inactive list
      lru_list_add(inactive, n);
      nr_pages--;
    }
  }
}


static void expand_caches(struct lru_list *active, struct lru_list *inactive)
{
  size_t nr_pages = inactive->numentries;
  size_t i;
  struct lru_node *n;

  // examine each page in inactive list and move to active list if accessed
  for (i = 0; i < nr_pages; i++) {
    n = lru_list_remove(inactive);

    if (hemem_get_accessed_bit(n->page->va) == HEMEM_ACCESSED_FLAG) {
      lru_list_add(active, n);
    }
    else {
      lru_list_add(inactive, n);
    }
  }
}


/*  called with global lock held via lru_pagefault function */
static uint64_t lru_allocate_page(struct lru_node *n)
{
  uint64_t i;
  struct timeval start, end;
#ifdef LRU_SWAP
  struct lru_node *cn;
  int tries;
#endif
  
  gettimeofday(&start, NULL);
#ifdef LRU_SWAP
  for (tries = 0; tries < 2; tries++) {
#endif
    // last_dram_framenum -> end of dram pages bitmap
    for (i = 0; i < FASTMEM_PAGES; i++) {
      if (dram_bitmap[i] == false) {
        dram_bitmap[i] = true;
        n->framenum = i;
        lru_list_add(&active_list, n);
        n->page->in_dram = true;

        gettimeofday(&end, NULL);
        LOG_TIME("mem_policy_allocate_page: %f s\n", elapsed(&start, &end));
        
        // return offset in devdax file -- done!
        return i * PAGE_SIZE;
      }
    }
#ifndef LRU_SWAP
    // DRAM is full, fall back to NVM
    for (i = 0; i < SLOWMEM_PAGES; i++) {
      if (nvm_bitmap[i] == false) {
        // found a free slowmem page, grab it
        nvm_bitmap[i] = true;
        n->framenum = i;
        lru_list_add(&nvm_active_list, n);
        n->page->in_dram = false;

        gettimeofday(&end, NULL);
        LOG_TIME("mem_policy_allocate_page: %f s\n", elapsed(&start, &end));
        
        return i * PAGE_SIZE;
      }
    }
#else
    // DRAM was full, try to free some space by moving a cold page down
    if (inactive_list.numentries == 0){
      // force some pages down to slow memory/inactive list
      shrink_caches(&active_list, &inactive_list);
    }

    // move a cold page from dram to nvm
    cn = lru_list_remove(&inactive_list);
    assert(cn != NULL);
    
    for (i = 0; i < SLOWMEM_PAGES; i++) {
      if (nvm_bitmap[i] == false) {
        LOG("Out of hot memory -> move hot frame %lu to cold frame %lu\n", cn->framenum, i);
        LOG("\tmoving va: 0x%lx\n", cn->page->va);

        // found a free slowmem page, grab it
        nvm_bitmap[i] = true;
        dram_bitmap[cn->framenum] = false;
        lru_migrate_down(cn, i);

        lru_list_add(&nvm_inactive_list, cn);
        break;
      }
    }
#endif
#ifdef LRU_SWAP
  }
#endif

  assert(!"Out of memory");
}


void *lru_kswapd()
{
  bool moved;
  int tries;
  uint64_t i;
  struct lru_node *n;
  struct lru_node *cn;

  in_kswapd = true;

  for (;;) {
    usleep(KSWAPD_INTERVAL);
    pthread_mutex_lock(&global_lock);

    shrink_caches(&active_list, &inactive_list);
    shrink_caches(&nvm_active_list, &nvm_inactive_list);

    expand_caches(&active_list, &inactive_list);
    expand_caches(&nvm_active_list, &nvm_inactive_list);

    for (n = lru_list_remove(&nvm_active_list); n != NULL; n = lru_list_remove(&nvm_active_list)) {
      moved = false;

      for (tries = 0; tries < 2 && !moved; tries++) {
        for (i = 0; i < FASTMEM_PAGES; i++) {
          if (dram_bitmap[i] == false) {
            dram_bitmap[i] = true;
            nvm_bitmap[n->framenum] = false;
            
            LOG("%lx: cold %lu -> hot %lu\t slowmem.active: %lu, slowmem.inactive: %lu\t hotmem.active: %lu, hotmem.inactive: %lu\n",
                n->page->va, n->framenum, i, nvm_active_list.numentries, nvm_inactive_list.numentries, active_list.numentries, inactive_list.numentries);

            lru_migrate_up(n, i);

            lru_list_add(&active_list, n);

            moved = true;
            break;
          }
        }

        if (moved) {
          break;
        }

        cn = lru_list_remove(&inactive_list);
        if (cn == NULL) {
          // all dram pages are hot
          lru_list_add(&nvm_active_list, n);
          goto out;
        }

        for (i = 0; i < SLOWMEM_PAGES; i++) {
          if (nvm_bitmap[i] == false) {
            nvm_bitmap[i] = true;
            dram_bitmap[cn->framenum] = false;

            LOG("%lx: hot %lu -> cold %lu\t slowmem.active: %lu, slowmem.inactive: %lu\t hotmem.active: %lu, hotmem.inactive: %lu\n",
                  cn->page->va, cn->framenum, i, nvm_active_list.numentries, nvm_inactive_list.numentries, active_list.numentries, inactive_list.numentries);

            lru_migrate_down(cn, i);

            lru_list_add(&nvm_inactive_list, cn);
            break;
          }
        }
      }
    }

out:
    pthread_mutex_unlock(&global_lock);
  }

  return NULL;
}


void lru_pagefault(struct hemem_page *page)
{
  uint64_t offset;
  struct lru_node *node;

  assert(page != NULL);
  pthread_mutex_lock(&global_lock);
  pthread_mutex_lock(&(page->page_lock));

  // set up the lru node for the lru lists
  node = (struct lru_node*)calloc(1, sizeof(struct lru_node));
  if (node == NULL) {
    perror("node calloc");
    assert(0);
  }
  node->next = NULL;
  node->prev = NULL;
  node->page = page;
  
  // do the heavy lifting of finding the devdax file offset to place the page
  offset = lru_allocate_page(node);
  page->devdax_offset = offset;
  page->next = NULL;
  page->prev = NULL;
  pthread_mutex_unlock(&(page->page_lock));
  pthread_mutex_unlock(&global_lock);
}


void lru_init(void)
{
  pthread_t kswapd_thread;
  int r;

  r = pthread_create(&kswapd_thread, NULL, lru_kswapd, NULL);
  assert(r == 0);
  
#ifndef LRU_SWAP
  LOG("Memory management policy is LRU\n");
#else
  LOG("Memory management policy is LRU-swap\n");
#endif

}
