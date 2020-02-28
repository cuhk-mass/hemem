#include <stdlib.h>
#include <pthread.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdbool.h>
#include <pthread.h>
#include <assert.h>
#include <sys/time.h>

#include "hemem.h"
#include "paging.h"
#include "lru_modified.h"
#include "timer.h"

struct modified_lru_list active_list;
struct modified_lru_list inactive_list;
struct modified_lru_list nvm_active_list;
struct modified_lru_list nvm_inactive_list;
pthread_mutex_t global_lock = PTHREAD_MUTEX_INITIALIZER;
bool __thread in_kswapd = false;
uint64_t last_dram_framenum = 0;
uint64_t last_nvm_framenum = 0;
bool dram_bitmap[FASTMEM_PAGES];
bool nvm_bitmap[SLOWMEM_PAGES];
struct timeval kswapdruntime;


void modified_lru_migrate_down(struct modified_lru_node *n, uint64_t i)
{
  pthread_mutex_lock(&(n->page->page_lock));
  n->page->migrating = true;
  hemem_wp_page(n->page, true);
  hemem_migrate_down(n->page, i * PAGE_SIZE);
  n->framenum = i;
  n->page->devdax_offset = (n->framenum * PAGE_SIZE);
  n->page->migrating = false; 
  pthread_mutex_unlock(&(n->page->page_lock));
}

void modified_lru_migrate_up(struct modified_lru_node *n, uint64_t i)
{
  pthread_mutex_lock(&(n->page->page_lock));
  n->page->migrating = true;
  hemem_wp_page(n->page, true);
  hemem_migrate_up(n->page, i * PAGE_SIZE);
  n->framenum = i;
  n->page->devdax_offset = (n->framenum * PAGE_SIZE);
  n->page->migrating = false;
  pthread_mutex_unlock(&(n->page->page_lock));
}

void modified_lru_list_add(struct modified_lru_list *list, struct modified_lru_node *node)
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


struct modified_lru_node* modified_lru_list_remove(struct modified_lru_list *list)
{
  struct modified_lru_node *ret = list->last;

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


void modified_shrink_caches(struct modified_lru_list *active, struct modified_lru_list *inactive)
{
  size_t nr_pages = 32;

  // find cold pages and move to inactive list
  while (nr_pages > 0 && active->numentries > 0) {
    struct modified_lru_node *n = modified_lru_list_remove(active);
    if (hemem_get_accessed_bit(n->page->va)) {
      // give accessed pages another go-around in active list
      hemem_clear_accessed_bit(n->page->va);
      modified_lru_list_add(active, n);
    }
    else {
      // found a cold page, put it on inactive list
      modified_lru_list_add(inactive, n);
      nr_pages--;
    }
  }
}


void modified_expand_caches(struct modified_lru_list *active, struct modified_lru_list *inactive)
{
  size_t nr_pages = inactive->numentries;
  size_t i;
  struct modified_lru_node *n;

  // examine each page in inactive list and move to active list if accessed
  for (i = 0; i < nr_pages; i++) {
    n = modified_lru_list_remove(inactive);

    if (hemem_get_accessed_bit(n->page->va) == HEMEM_ACCESSED_FLAG) {
      modified_lru_list_add(active, n);
    }
    else {
      modified_lru_list_add(inactive, n);
    }
  }
}


uint64_t lru_modified_allocate_page(struct modified_lru_node *n)
{
  uint64_t i;
  struct timeval start, end;
  
  pthread_mutex_lock(&global_lock);
  /* last_dram_framenum remembers the last dram frame number
   * we allocated. This should help speed up the search
   * for a free frame because we don't iterate over frames
   * that were already allocated and are probably still in
   * use. If last_dram_framenum is equal to the number of
   * dram pages, we wrap back around to 0. We iterate through
   * the fastmem bitmap in two passes. The first pass starts
   * at last_dram_framenum and goes to the end of the array
   * to skip over probably already allocated and still in
   * use pages. The second pass goes back to the start of the
   * bitmap and scans up until last_dram_framenum to use any
   * pages that have been freed in that range before resorting
   * to slow memory
   */
  if (last_dram_framenum >= FASTMEM_PAGES) {
    last_dram_framenum = 0;
  }
  gettimeofday(&start, NULL);
  // last_dram_framenum -> end of dram pages bitmap
  for (i = last_dram_framenum; i < FASTMEM_PAGES; i++) {
    if (dram_bitmap[i] == false) {
      dram_bitmap[i] = true;
      n->framenum = i;
      modified_lru_list_add(&active_list, n);
      n->page->in_dram = true;
      pthread_mutex_unlock(&global_lock);
      last_dram_framenum = i;

      gettimeofday(&end, NULL);
      LOG_TIME("mem_policy_allocate_page: %f s\n", elapsed(&start, &end));
      
      // return offset in devdax file -- done!
      return i * PAGE_SIZE;
    }
  }

  // start of dram pages bitmap -> last_dram_framenum
  for (i = 0; i < last_dram_framenum; i++) {
    if (dram_bitmap[i] == false) {
      dram_bitmap[i] = true;
      n->framenum = i;
      n->page->in_dram = true;
      modified_lru_list_add(&active_list, n);
      pthread_mutex_unlock(&global_lock);
      last_dram_framenum = i;
      
      gettimeofday(&end, NULL);
      LOG_TIME("mem_policy_allocate_page: %f s\n", elapsed(&start, &end));

      // return offset in devdax file -- done!
      return i * PAGE_SIZE;
    }
  }

  /* last_nvm_framenum behaves the same way as last_dram_framenum
   * (see above)
   */
  if (last_nvm_framenum >= SLOWMEM_PAGES) {
    last_nvm_framenum = 0;
  }

  for (i = last_nvm_framenum; i < SLOWMEM_PAGES; i++) {
    if (nvm_bitmap[i] == false) {
      // found a free slowmem page, grab it
      nvm_bitmap[i] = true;
      n->framenum = i;
      n->page->in_dram = false;
      modified_lru_list_add(&nvm_active_list, n);
      pthread_mutex_unlock(&global_lock);
      last_nvm_framenum = i;

      gettimeofday(&end, NULL);
      LOG_TIME("mem_policy_allocate_page: %f s\n", elapsed(&start, &end));
      
      return i * PAGE_SIZE;
    }
  }
  for (i = 0; i < last_nvm_framenum; i++) {
    if (nvm_bitmap[i] == false) {
      nvm_bitmap[i] = true;
      n->framenum = i;
      n->page->in_dram = false;
      modified_lru_list_add(&nvm_active_list, n);
      pthread_mutex_unlock(&global_lock);
      last_nvm_framenum = i;
      
      gettimeofday(&end, NULL);
      LOG_TIME("mem_policy_allocate_page: %f s\n", elapsed(&start, &end));

      return i * PAGE_SIZE;
    }
  }

  pthread_mutex_unlock(&global_lock);
  assert(!"Out of memory");
}


void *lru_modified_kswapd()
{
  bool moved;
  int tries;
  uint64_t i;
  struct modified_lru_node *n;
  struct modified_lru_node *cn;
  struct timeval curtime;
  bool found;

  in_kswapd = true;

  for (;;) {
    gettimeofday(&curtime, NULL);
    while(elapsed(&kswapdruntime, &curtime) < KSWAPD_INTERVAL) {
      gettimeofday(&curtime, NULL);
    }

    pthread_mutex_lock(&global_lock);
    
    found = false;

    modified_shrink_caches(&active_list, &inactive_list);
    modified_shrink_caches(&nvm_active_list, &nvm_inactive_list);

    modified_expand_caches(&active_list, &inactive_list);
    modified_expand_caches(&nvm_active_list, &nvm_inactive_list);

    for (n = modified_lru_list_remove(&nvm_active_list); n != NULL; n = modified_lru_list_remove(&nvm_active_list)) {
      moved = false;

      for (tries = 0; tries < 2 && !moved; tries++) {
        if (last_dram_framenum >= FASTMEM_PAGES) {
          last_dram_framenum = 0;
        }

        for (i = last_dram_framenum; i < FASTMEM_PAGES; i++) {
          if (dram_bitmap[i] == false) {
            dram_bitmap[i] = true;
            nvm_bitmap[n->framenum] = false;
            
            LOG("cold %lu -> hot %lu\t slowmem.active: %lu, slowmem.inactive %lu\t hotmem.active: %lu, hotmem.inactive: %lu\n",
                n->framenum, i, nvm_active_list.numentries, nvm_inactive_list.numentries, active_list.numentries, inactive_list.numentries);

            modified_lru_migrate_up(n, i);

            modified_lru_list_add(&active_list, n);

            moved = true;
            break;
          }
        }
        for (i = 0; i < last_dram_framenum; i++) {
          if (dram_bitmap[i] == false) {
            dram_bitmap[i] = true;
            nvm_bitmap[n->framenum] = false;
            
            LOG("cold %lu -> hot %lu\t slowmem.active: %lu, slowmem.inactive %lu\t hotmem.active: %lu, hotmem.inactive: %lu\n", 
                n->framenum, i, nvm_active_list.numentries, nvm_inactive_list.numentries, active_list.numentries, inactive_list.numentries);

            modified_lru_migrate_up(n, i);

            modified_lru_list_add(&active_list, n);

            moved = true;
            break;
          }
        }

        if (moved) {
          break;
        }

        cn = modified_lru_list_remove(&inactive_list);
        if (cn == NULL) {
          // all dram pages are hot
          modified_lru_list_add(&nvm_active_list, n);
          goto out;
        }

        if (last_nvm_framenum >= SLOWMEM_PAGES) {
          last_nvm_framenum = 0;
        }

        for (i = last_nvm_framenum; i < SLOWMEM_PAGES; i++) {
          if (nvm_bitmap[i] == false) {
            nvm_bitmap[i] = true;
            dram_bitmap[cn->framenum] = false;

            modified_lru_migrate_down(cn, i);

            modified_lru_list_add(&nvm_inactive_list, cn);
            found = true;
            
            break;
          }
        }
        if (!found) {
          for (i = last_nvm_framenum; i < SLOWMEM_PAGES; i++) {
            if (nvm_bitmap[i] == false) {
              nvm_bitmap[i] = true;
              dram_bitmap[cn->framenum] = false;
              
              modified_lru_migrate_down(cn, i);

              modified_lru_list_add(&nvm_inactive_list, cn);

              break;
            }
          }
        }
      }
    }

out:
    pthread_mutex_unlock(&global_lock);
  
    gettimeofday(&kswapdruntime, NULL);
  }

  return NULL;
}


void lru_modified_pagefault(struct hemem_page *page)
{
  uint64_t offset;
  struct modified_lru_node *node;

  assert(page != NULL);

  // set up the lru node for the lru lists
  node = (struct modified_lru_node*)calloc(1, sizeof(struct modified_lru_node));
  if (node == NULL) {
    perror("node calloc");
    assert(0);
  }
  node->next = NULL;
  node->prev = NULL;
  node->page = page;
  
  // do the heavy lifting of finding the devdax file offset to place the page
  offset = lru_modified_allocate_page(node);
  page->devdax_offset = offset;
  page->next = NULL;
  page->prev = NULL;
}


void lru_modified_init(void)
{
  pthread_t kswapd_thread;
  int r;

  r = pthread_create(&kswapd_thread, NULL, lru_modified_kswapd, NULL);
  assert(r == 0);
  
  gettimeofday(&kswapdruntime, NULL);
  LOG("Memory management policy is Modified LRU\n");
}
