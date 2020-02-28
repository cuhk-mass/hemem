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
#include "lru.h"
#include "timer.h"

struct lru_list active_list;
struct lru_list inactive_list;
struct lru_list nvm_active_list;
struct lru_list nvm_inactive_list;
pthread_mutex_t global_lock = PTHREAD_MUTEX_INITIALIZER;
bool __thread in_kswapd = false;
uint64_t last_dram_framenum = 0;
uint64_t last_nvm_framenum = 0;
bool dram_bitmap[FASTMEM_PAGES];
bool nvm_bitmap[SLOWMEM_PAGES];
struct timeval kswapdruntime;

void lru_migrate_up(struct lru_node *n, uint64_t i)
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

void lru_migrate_down(struct lru_node *n, uint64_t i)
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

void lru_list_add(struct lru_list *list, struct lru_node *node)
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


struct lru_node* lru_list_remove(struct lru_list *list)
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


void shrink_caches(struct lru_list *active, struct lru_list *inactive)
{
  size_t nr_pages = 32;

  // find cold pages and move to inactive list
  while (nr_pages > 0 && active->numentries > 0) {
    struct lru_node *n = lru_list_remove(active);
    if (hemem_get_accessed_bit(n->page->va)) {
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


void expand_caches(struct lru_list *active, struct lru_list *inactive)
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


uint64_t lru_allocate_page(struct lru_node *n)
{
  int tries;
  uint64_t i;
  struct lru_node *cn;
  bool found = false;
  struct timeval start, end;

  pthread_mutex_lock(&global_lock);

  gettimeofday(&start, NULL);

  for (tries = 0; tries < 2; tries++) {
    found = false;
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
    // last_dram_framenum -> end of dram pages bitmap
    for (i = last_dram_framenum; i < FASTMEM_PAGES; i++) {
      if (dram_bitmap[i] == false) {
        dram_bitmap[i] = true;
        n->framenum = i;
        lru_list_add(&active_list, n);
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
        lru_list_add(&active_list, n);
        pthread_mutex_unlock(&global_lock);
        last_dram_framenum = i;

        gettimeofday(&end, NULL);
        LOG_TIME("mem_policy_allocate_page: %f s\n", elapsed(&start, &end));

        // return offset in devdax file -- done!
        return i * PAGE_SIZE;
      }
      
    }

    // dram was full so move a page to cold memory
    if (inactive_list.numentries == 0){
      // force some pages down to slow memory/inactive list
      shrink_caches(&active_list, &inactive_list);
    }

    // move a cold page from dram to nvm
    cn = lru_list_remove(&inactive_list);
    assert(cn != NULL);

    /* last_nvm_framenum behaves the same way as last_dram_framenum
     * (see above)
     */
    if (last_nvm_framenum >= SLOWMEM_PAGES) {
      last_nvm_framenum = 0;
    }

    for (i = last_nvm_framenum; i < SLOWMEM_PAGES; i++) {
      if (nvm_bitmap[i] == false) {
        LOG("Out of hot memory -> move hot frame %lu to cold frame %lu\n", cn->framenum, i);
        LOG("\tmoving va: 0x%lx\n", cn->page->va);

        // found a free slowmem page, grab it
        nvm_bitmap[i] = true;
        dram_bitmap[cn->framenum] = false;
        lru_migrate_down(cn, i);
        last_nvm_framenum = i;

        lru_list_add(&nvm_inactive_list, cn);
        found = true;
        break;
      }
    }
    if (!found) {
      for (i = 0; i < last_nvm_framenum; i++) {
        if (nvm_bitmap[i] == false) {
          LOG("Out of hot memory -> move hot frame %lu to cold frame %lu\n", cn->framenum, i);
          LOG("\tmoving va: 0x%lx\n", cn->page->va);
          nvm_bitmap[i] = true;
          dram_bitmap[cn->framenum] = false;
          lru_migrate_down(cn, i);
          last_nvm_framenum = i;

          lru_list_add(&nvm_inactive_list, cn);
          break;
        }
      }
    }
  }

  pthread_mutex_unlock(&global_lock);
  assert(!"Out of memory");
}


void *lru_kswapd()
{
  bool moved;
  int tries;
  uint64_t i;
  struct lru_node *n;
  struct lru_node *cn;
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

    shrink_caches(&active_list, &inactive_list);
    shrink_caches(&nvm_active_list, &nvm_inactive_list);

    expand_caches(&active_list, &inactive_list);
    expand_caches(&nvm_active_list, &nvm_inactive_list);

    for (n = lru_list_remove(&nvm_active_list); n != NULL; n = lru_list_remove(&nvm_active_list)) {
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

            lru_migrate_up(n, i);

            lru_list_add(&active_list, n);

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

        if (last_nvm_framenum >= SLOWMEM_PAGES) {
          last_nvm_framenum = 0;
        }

        for (i = last_nvm_framenum; i < SLOWMEM_PAGES; i++) {
          if (nvm_bitmap[i] == false) {
            nvm_bitmap[i] = true;
            dram_bitmap[cn->framenum] = false;

            lru_migrate_down(cn, i);

            lru_list_add(&nvm_inactive_list, cn);
            found = true;
            
            break;
          }
        }
        if (!found) {
          for (i = last_nvm_framenum; i < SLOWMEM_PAGES; i++) {
            if (nvm_bitmap[i] == false) {
              nvm_bitmap[i] = true;
              dram_bitmap[cn->framenum] = false;

              lru_migrate_down(cn, i);
              
              lru_list_add(&nvm_inactive_list, cn);

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


void lru_pagefault(struct hemem_page *page)
{
  uint64_t offset;
  struct lru_node *node;

  assert(page != NULL);

  // set up the lru node for the lru lists
  node = (struct lru_node*)calloc(1, sizeof(struct lru_node));
  if(node == NULL) {
    perror("node calloc");
    assert(0);
  }
  node->next = NULL;
  node->prev = NULL;
  node->page = page;
  
  // do the heavy lifting of finding the devdax file offset to place the page
  offset = lru_allocate_page(node);
  page->in_dram = true;         // LRU will always place a new page in dram
  page->devdax_offset = offset;
  page->next = NULL;
  page->prev = NULL;
}


void lru_init(void)
{
  pthread_t kswapd_thread;
  int r;

  r = pthread_create(&kswapd_thread, NULL, lru_kswapd, NULL);
  assert(r == 0);
  
  gettimeofday(&kswapdruntime, NULL);
  LOG("Memory management policy is LRU\n");
}
