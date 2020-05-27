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
uint64_t lru_runs = 0;

static void lru_migrate_down(struct lru_node *n, uint64_t i)
{
  pthread_mutex_lock(&(n->page->page_lock));
  LOG("hemem: lru_migrate_down: migrating %lx to NVM frame %lu\n", n->page->va, i);
  n->page->migrating = true;
  hemem_wp_page(n->page, true);
  hemem_migrate_down(n->page, i * PAGE_SIZE);
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
  hemem_migrate_up(n->page, i * PAGE_SIZE);
  n->page->migrating = false;
  LOG("hemem: lru_migrate_up: done migrating to DRAM\n");
  pthread_mutex_unlock(&(n->page->page_lock));
}

static void lru_list_add(struct lru_list *list, struct lru_node *node)
{
  pthread_mutex_lock(&(list->list_lock));
  ignore_this_mmap = true;
  assert(node->prev == NULL);
  ignore_this_mmap = false;
  node->next = list->first;
  if (list->first != NULL) {
    ignore_this_mmap = true;
    assert(list->first->prev == NULL);
    ignore_this_mmap = false;
    list->first->prev = node;
  }
  else {
    ignore_this_mmap = true;
    assert(list->last == NULL);
    assert(list->numentries == 0);
    ignore_this_mmap = false;
    list->last = node;
  }

  list->first = node;
  node->list = list;
  list->numentries++;
  pthread_mutex_unlock(&(list->list_lock));
}


static struct lru_node* lru_list_remove(struct lru_list *list)
{
  pthread_mutex_lock(&(list->list_lock));
  struct lru_node *ret = list->last;

  if (ret == NULL) {
    ignore_this_mmap = true;
    assert(list->numentries == 0);
    ignore_this_mmap = false;
    pthread_mutex_unlock(&(list->list_lock));
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
  ret->list = NULL;
  ignore_this_mmap = true;
  assert(list->numentries > 0);
  ignore_this_mmap = false;
  list->numentries--;
  pthread_mutex_unlock(&(list->list_lock));
  return ret;
}

static void lru_list_remove_node(struct lru_list *list, struct lru_node *node)
{
  pthread_mutex_lock(&(list->list_lock));
  if (list->first == NULL) {
    ignore_this_mmap = true;
    assert(list->last == NULL);
    assert(list->numentries == 0);
    ignore_this_mmap = false;
    pthread_mutex_unlock(&(list->list_lock));
    LOG("lru_list_remove_node: list was empty!\n");
    return;
  }

  if (list->first == node) {
    list->first = node->next;
  }

  if (list->last == node) {
    list->last = node->prev;
  }

  if (node->next != NULL) {
    node->next->prev = node->prev;
  }

  if (node->prev != NULL) {
    node->prev->next = node->next;
  }

  list->numentries--;
  node->next = NULL;
  node->prev = NULL;
  node->list = NULL;
  pthread_mutex_unlock(&(list->list_lock));
}


static void shrink_caches(struct lru_list *active, struct lru_list *inactive)
{
  size_t nr_pages = active->numentries;

  // find cold pages and move to inactive list
  while (nr_pages > 0 && active->numentries > 0) {
    struct lru_node *n = lru_list_remove(active);
    if (hemem_get_accessed_bit(n->page) == HEMEM_ACCESSED_FLAG) {
      // give accessed pages another go-around in active list
      hemem_clear_accessed_bit(n->page);
      lru_list_add(active, n);
    }
    else {
      // found a cold page, put it on inactive list
      lru_list_add(inactive, n);
    }
    nr_pages--;
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

    if (n == NULL) {
      break;
    }

    if (hemem_get_accessed_bit(n->page) == HEMEM_ACCESSED_FLAG) {
      lru_list_add(active, n);
    }
    else {
      lru_list_add(inactive, n);
    }
  }
}

void *lru_kscand()
{
  struct timeval start, end;

  for (;;) {
    usleep(KSCAND_INTERVAL);
    //pthread_mutex_lock(&global_lock);

    gettimeofday(&start, NULL);

    shrink_caches(&active_list, &inactive_list);
    shrink_caches(&nvm_active_list, &nvm_inactive_list);

    expand_caches(&active_list, &inactive_list);
    expand_caches(&nvm_active_list, &nvm_inactive_list);

    hemem_tlb_shootdown(0);

    gettimeofday(&end, NULL);

    LOG_TIME("scan: %f s\n", elapsed(&start, &end));
    //pthread_mutex_unlock(&global_lock);
  }
}

void *lru_kswapd()
{
  int tries;
  struct lru_node *n;
  struct lru_node *cn;
  struct lru_node *nn;
  struct timeval start, end;
  uint64_t migrated_bytes;

  //free(malloc(65536));
  
  in_kswapd = true;

  for (;;) {
    usleep(KSWAPD_INTERVAL);

    pthread_mutex_lock(&global_lock);

    gettimeofday(&start, NULL);

    // move each active NVM page to DRAM
    for (migrated_bytes = 0; migrated_bytes < KSWAPD_MIGRATE_RATE;) {
      n = lru_list_remove(&nvm_active_list);
      if (n == NULL) {
        break;
      }

      for (tries = 0; tries < 2; tries++) {
        // find a free DRAM page
        nn = lru_list_remove(&dram_free_list);

        if (nn != NULL) {
          LOG("%lx: cold %lu -> hot %lu\t slowmem.active: %lu, slowmem.inactive: %lu\t hotmem.active: %lu, hotmem.inactive: %lu\n",
                n->page->va, n->framenum, nn->framenum, nvm_active_list.numentries, nvm_inactive_list.numentries, active_list.numentries, inactive_list.numentries);

          lru_migrate_up(n, nn->framenum);
          struct hemem_page *tmp;
          tmp = nn->page;
          nn->page = n->page;
          nn->page->management = nn;

          n->page = tmp;
          n->page->management = n;

          n->page->devdax_offset = n->framenum * PAGE_SIZE;
          n->page->in_dram = false;

          lru_list_add(&active_list, nn);

          lru_list_add(&nvm_free_list, n);

          migrated_bytes += pt_to_pagesize(nn->page->pt);

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
          struct hemem_page *tmp;
          tmp = nn->page;
          nn->page = cn->page;
          nn->page->management = nn;

          cn->page = tmp;
          cn->page->management = cn;
          cn->page->devdax_offset = cn->framenum * PAGE_SIZE;
          cn->page->in_dram = true;

          lru_list_add(&nvm_inactive_list, nn);

          lru_list_add(&dram_free_list, cn);
        }
      }
    }

out:
    lru_runs++;
    pthread_mutex_unlock(&global_lock);
    gettimeofday(&end, NULL);
    LOG_TIME("migrate: %f s\n", elapsed(&start, &end));
  }

  return NULL;
}


/*  called with global lock held via lru_pagefault function */
static struct hemem_page* lru_allocate_page()
{
  struct timeval start, end;
  struct lru_node *node;
#ifdef LRU_SWAP
  struct lru_node *cn;
  int tries;
#endif

  gettimeofday(&start, NULL);
#ifdef LRU_SWAP
  for (tries = 0; tries < 2; tries++) {
#endif
    node = lru_list_remove(&dram_free_list);
    if (node != NULL) {
      ignore_this_mmap = true;
      assert(node->page->in_dram);
      assert(!node->page->present);
      assert(node->page->devdax_offset == node->framenum * PAGE_SIZE);
      ignore_this_mmap = false;

      node->page->present = true;
      lru_list_add(&active_list, node);

      node->page->management = node;

      gettimeofday(&end, NULL);
      LOG_TIME("mem_policy_allocate_page: %f s\n", elapsed(&start, &end));

      return node->page;
    }
    
#ifndef LRU_SWAP
    // DRAM is full, fall back to NVM
    node = lru_list_remove(&nvm_free_list);
    if (node != NULL) {
      ignore_this_mmap = true;
      assert(!node->page->in_dram);
      assert(!node->page->present);
      assert(node->page->devdax_offset == node->framenum * PAGE_SIZE);
      ignore_this_mmap = false;

      node->page->present = true;
      lru_list_add(&nvm_active_list, node);

      node->page->management = node;

      gettimeofday(&end, NULL);
      LOG_TIME("mem_policy_allocate_page: %f s\n", elapsed(&start, &end));

      return node->page;
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

      lru_migrate_down(cn, node->framenum);

      node->page = cn->page;
      node->page->management = node;

      lru_list_add(&nvm_inactive_list, node);

      lru_list_add(&dram_free_list, cn);
    }
    
    
#endif
#ifdef LRU_SWAP
  }
#endif

  ignore_this_mmap = true;
  assert(!"Out of memory");
  ignore_this_mmap = false;
}


struct hemem_page* lru_pagefault(void)
{
  struct hemem_page *page;

  pthread_mutex_lock(&global_lock);
  // do the heavy lifting of finding the devdax file offset to place the page
  page = lru_allocate_page();
  pthread_mutex_unlock(&global_lock);
  ignore_this_mmap = true;
  assert(page != NULL);
  ignore_this_mmap = false;
  
  return page;
}

struct hemem_page* lru_pagefault_unlocked(void)
{
  struct hemem_page *page;

  page = lru_allocate_page();
  ignore_this_mmap = true;
  assert(page != NULL);
  ignore_this_mmap = false;

  return page;
}

void lru_remove_page(struct hemem_page *page)
{
  struct lru_node *node;
  struct lru_list *list;

  ignore_this_mmap = true;
  assert(page != NULL);
  ignore_this_mmap = false;

  node = page->management;
  ignore_this_mmap = true;
  assert(node != NULL);
  ignore_this_mmap = false;

  LOG("LRU: remove page: va: 0x%lx\n", page->va);

  list = node->list;
  ignore_this_mmap = true;
  assert(list != NULL);
  ignore_this_mmap = false;

  lru_list_remove_node(list, node);
  page->present = false;

  if (page->in_dram) {
    lru_list_add(&dram_free_list, node);
  }
  else {
    lru_list_add(&nvm_free_list, node);
  }
}


void lru_init(void)
{
  pthread_t kswapd_thread;
  pthread_t scan_thread;

  LOG("lru_init: started\n");

  pthread_mutex_init(&(dram_free_list.list_lock), NULL);
  for (int i = 0; i < DRAMSIZE / PAGE_SIZE; i++) {
    struct lru_node *n = calloc(1, sizeof(struct lru_node));
    n->framenum = i;

    struct hemem_page *p = calloc(1, sizeof(struct hemem_page));
    p->devdax_offset = i * PAGE_SIZE;
    p->present = false;
    p->in_dram = true;
    p->pt = pagesize_to_pt(PAGE_SIZE);
    pthread_mutex_init(&(p->page_lock), NULL);

    n->page = p;
    p->management = n;
    lru_list_add(&dram_free_list, n);
  }

  pthread_mutex_init(&(nvm_free_list.list_lock), NULL);
  for (int i = 0; i < NVMSIZE / PAGE_SIZE; i++) {
    struct lru_node *n = calloc(1, sizeof(struct lru_node));
    n->framenum = i;

    struct hemem_page *p = calloc(1, sizeof(struct hemem_page));
    p->devdax_offset = i * PAGE_SIZE;
    p->present = false;
    p->in_dram = false;
    p->pt = pagesize_to_pt(PAGE_SIZE);
    pthread_mutex_init(&(p->page_lock), NULL);

    n->page = p;
    p->management = n;
    lru_list_add(&nvm_free_list, n);
  }

  int r = pthread_create(&scan_thread, NULL, lru_kscand, NULL);
  ignore_this_mmap = true;
  assert(r == 0);
  ignore_this_mmap = false;
  
  r = pthread_create(&kswapd_thread, NULL, lru_kswapd, NULL);
  ignore_this_mmap = true;
  assert(r == 0);
  ignore_this_mmap = false;
  
#ifndef LRU_SWAP
  LOG("Memory management policy is LRU\n");
#else
  LOG("Memory management policy is LRU-swap\n");
#endif

  LOG("lru_init: finished\n");

}

void lru_stats()
{
  LOG_STATS("\tactive_list.numentries: [%ld]\tinactive_list.numentries: [%ld]\tnvm_active_list.numentries: [%ld]\tnvm_inactive_list.numentries: [%ld]\n",
          active_list.numentries,
          inactive_list.numentries,
          nvm_active_list.numentries,
          nvm_inactive_list.numentries);
}


void lru_lock()
{
  pthread_mutex_lock(&global_lock);
}

void lru_unlock()
{
  pthread_mutex_unlock(&global_lock);
}

