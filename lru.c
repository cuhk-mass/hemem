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
static struct lru_list dram_free_list;
static struct lru_list nvm_free_list;
static pthread_mutex_t global_lock = PTHREAD_MUTEX_INITIALIZER;
static bool __thread in_kswapd = false;

static void lru_migrate_down(struct lru_node *n, uint64_t i)
{
  pthread_mutex_lock(&(n->page->page_lock));
  LOG("hemem: lru_migrate_down: migrating %lx to NVM frame %lu\n", n->page->va, i);
  n->page->migrating = true;
  hemem_wp_page(n->page, true);
  hemem_migrate_down(n->page, i);
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
static uint64_t lru_allocate_page(uint64_t addr, struct hemem_page *page)
{
  struct timeval start, end;
  struct lru_node *node;
#ifdef LRU_SWAP
  struct lru_node *cn;
  int tries;
#endif

  pthread_mutex_lock(&global_lock);
  
  gettimeofday(&start, NULL);
#ifdef LRU_SWAP
  for (tries = 0; tries < 2; tries++) {
#endif
    node = lru_list_remove(&dram_free_list);
    if (node != NULL) {
      node->page = page;
      node->page->in_dram = true;
      lru_list_add(&active_list, node);

      pthread_mutex_unlock(&global_lock);

      gettimeofday(&end, NULL);
      LOG_TIME("mem_policy_allocate_page: %f s\n", elapsed(&start, &end));

      return node->framenum * PAGE_SIZE;
    }
    
#ifndef LRU_SWAP
    // DRAM is full, fall back to NVM
    node = lru_list_remove(&nvm_free_list);
    if (node != NULL) {
      node->page = page;
      node->page->in_dram = false;
      lru_list_add(&nvm_active_list, node);

      pthread_mutex_unlock(&global_lock);
      
      gettimeofday(&end, NULL);
      LOG_TIME("mem_policy_allocate_page: %f s\n", elapsed(&start, &end));

      return node->framenum * PAGE_SIZE;
    }
    
#else
    // DRAM was full, try to free some space by moving a cold page down
    if (inactive_list.numentries == 0){
      // force some pages down to slow memory/inactive list
      shrink_caches(&active_list, &inactive_list);
    }

    // move a cold page from dram to nvm
    cn = lru_list_remove(&inactive_list);
    node = lru_list_remove(&nvm_free_list);
    if (node != NULL) {
      LOG("Out of hot memory -> move hot frame %lu to cold frame %lu\n", cn->framenum, node->framenum);
      LOG("\tmoving va: 0x%lx\n", cn->page->va);

      node->page = cn->page;
      lru_migrate_down(cn, node->framenum);

      lru_list_add(&nvm_inactive_list, node);

      lru_list_add(&dram_free_list, cn);
    }
    
    
#endif
#ifdef LRU_SWAP
  }
#endif

  pthread_mutex_unlock(&global_lock);
  assert(!"Out of memory");
}


void *lru_kswapd()
{
  int tries;
  struct lru_node *n;
  struct lru_node *cn;
  struct lru_node *nn;

  in_kswapd = true;

  for (;;) {
    usleep(KSWAPD_INTERVAL);
    pthread_mutex_lock(&global_lock);

    // identify cold pages
    shrink_caches(&active_list, &inactive_list);
    shrink_caches(&nvm_active_list, &nvm_inactive_list);

    // identify hot pages
    expand_caches(&active_list, &inactive_list);
    expand_caches(&nvm_active_list, &nvm_inactive_list);

    // move each active NVM page to DRAM
    for (n = lru_list_remove(&nvm_active_list); n != NULL; n = lru_list_remove(&nvm_active_list)) {
      for (tries = 0; tries < 2; tries++) {
        // find a free DRAM page
        nn = lru_list_remove(&dram_free_list);

        if (nn != NULL) {
          LOG("%lx: cold %lu -> hot %lu\t slowmem.active: %lu, slowmem.inactive: %lu\t hotmem.active: %lu, hotmem.inactive: %lu\n",
                n->page->va, n->framenum, nn->framenum, nvm_active_list.numentries, nvm_inactive_list.numentries, active_list.numentries, inactive_list.numentries);

          lru_migrate_up(n, nn->framenum);
          nn->page = n->page;

          lru_list_add(&active_list, nn);

          lru_list_add(&nvm_free_list, n);

          break;
        }

        // no free dram page, try to find a cold dram page to move down
        cn = lru_list_remove(&inactive_list);
        if (cn == NULL) {
          // all dram pages are hot
          lru_list_add(&nvm_active_list, n);
          goto out;
        }

        // find a free nvm page to move the cold dram page to
        nn = lru_list_remove(&nvm_free_list);
        if (nn != NULL) {
          LOG("%lx: hot %lu -> cold %lu\t slowmem.active: %lu, slowmem.inactive: %lu\t hotmem.active: %lu, hotmem.inactive: %lu\n",
                cn->page->va, cn->framenum, nn->framenum, nvm_active_list.numentries, nvm_inactive_list.numentries, active_list.numentries, inactive_list.numentries);

          lru_migrate_down(cn, nn->framenum);
          nn->page = cn->page;

          lru_list_add(&nvm_inactive_list, nn);

          lru_list_add(&dram_free_list, cn);
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

  assert(page != NULL);
  pthread_mutex_lock(&(page->page_lock));
  
  // do the heavy lifting of finding the devdax file offset to place the page
  offset = lru_allocate_page(page->va, page);
  page->devdax_offset = offset;
  page->next = NULL;
  page->prev = NULL;

  pthread_mutex_unlock(&(page->page_lock));
}


void lru_init(void)
{
  pthread_t kswapd_thread;
  int r;
  uint64_t i;
  struct lru_node *n;

  LOG("lru_init: started\n");

  n = calloc(FASTMEM_PAGES, sizeof(struct lru_node));
  for (i = 0; i < FASTMEM_PAGES; i++) {
    n[i].framenum = i;
    lru_list_add(&dram_free_list, &n[i]);
  }

  n = calloc(SLOWMEM_PAGES, sizeof(struct lru_node));
  for (i = 0; i < SLOWMEM_PAGES; i++) {
    n[i].framenum = i;
    lru_list_add(&nvm_free_list, &n[i]);
  }

  r = pthread_create(&kswapd_thread, NULL, lru_kswapd, NULL);
  assert(r == 0);
  
#ifndef LRU_SWAP
  LOG("Memory management policy is LRU\n");
#else
  LOG("Memory management policy is LRU-swap\n");
#endif

  LOG("lru_init: finished\n");

}
