#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>
#include <assert.h>
#include <inttypes.h>
#include <string.h>
#include <unistd.h>
#include <sys/time.h>

#include "hemem.h"
#include "paging.h"
#include "timer.h"
#include "hemem-mmgr.h"

static struct mmgr_list mem_free[NMEMTYPES][NPAGETYPES];
static struct mmgr_list mem_active[NMEMTYPES][NPAGETYPES];
static struct mmgr_list mem_inactive[NMEMTYPES][NPAGETYPES];
static pthread_mutex_t global_lock = PTHREAD_MUTEX_INITIALIZER;
static _Atomic uint64_t fastmem_freebytes = DRAMSIZE;
static _Atomic uint64_t slowmem_freebytes = NVMSIZE;


static void mmgr_list_add(struct mmgr_list *list, struct mmgr_node *node)
{
  pthread_mutex_lock(&(list->list_lock));
  ignore_this_mmap = true;
  assert(node->prev == NULL);
  ignore_this_mmap = false;
  node->next = list->first;
  if(list->first != NULL) {
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

static struct mmgr_node* mmgr_list_remove(struct mmgr_list *list)
{
  pthread_mutex_lock(&(list->list_lock));
  struct mmgr_node *ret = list->last;

  if(ret == NULL) {
    ignore_this_mmap = true;
    assert(list->numentries == 0);
    ignore_this_mmap = false;
    pthread_mutex_unlock(&(list->list_lock));
    return ret;
  }

  list->last = ret->prev;
  if(list->last != NULL) {
    list->last->next = NULL;
  } else {
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

static struct mmgr_node* mmgr_list_peek(struct mmgr_list *list)
{
  return list->last;
}

static void mmgr_list_remove_node(struct mmgr_list *list, struct mmgr_node *node)
{
  pthread_mutex_lock(&(list->list_lock));
  if (list->first == NULL) {
    ignore_this_mmap = true;
    assert(list->last == NULL);
    assert(list->numentries == 0);
    ignore_this_mmap = false;
    pthread_mutex_unlock(&(list->list_lock));
    LOG("mmgr_list_remove_node: list was empty\n");
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

static void move_hot(void)
{
  struct mmgr_list transition[NPAGETYPES];
  size_t transition_bytes = 0;
  struct mmgr_node *n;

  memset(transition, 0, NPAGETYPES * sizeof(struct mmgr_list));

  // identify pages for movement and mark read-only until out of fastmem
  while (transition_bytes + pt_to_pagesize(HUGEP) < fastmem_freebytes) {
    n = mmgr_list_remove(&mem_active[SLOWMEM][HUGEP]);

    if (n == NULL) {
      // no more active pages
      break;
    }

    mmgr_list_add(&transition[HUGEP], n);
    pthread_mutex_lock(&(n->page->page_lock));
    n->page->migrating = true;
    hemem_wp_page(n->page, true);

    transition_bytes += pt_to_pagesize(HUGEP);
  }

  if (transition_bytes == 0) {
    // everything is cold or we are out of fastmem -- bail out
    return;
  }

  hemem_tlb_shootdown(0);

  while ((n = mmgr_list_remove(&transition[HUGEP])) != NULL) {
    struct mmgr_node *nn;

//again:
    nn = mmgr_list_remove(&mem_free[FASTMEM][HUGEP]);

  /*if (nn == NULL) {
      // break up a huge page
      struct mmgr_node *hn = mmgr_list_remove(&mem_free[FASTMEM][HUGEP]);
      ignore_this_mmap = true;
      assert(hn != NULL);
      ignore_this_mmap = false;

      //hemem_demote_pages(hn->page->va);

      nn = calloc(512, sizeof(struct mmgr_node));
      for (size_t i = 0; i < 512; i++) {
        // TODO: break up huge page
        nn[i].offset = hn->offset + (i * BASEPAGE_SIZE);
        enqueue_fifo(&mem_free[FASTMEM][BASEP], &nn[i]);
      }
      free(hn);

      goto again;
    }
  */

    // TODO: move memory
    fastmem_freebytes -= pt_to_pagesize(HUGEP);
    slowmem_freebytes += pt_to_pagesize(HUGEP);

    hemem_migrate_down(n->page, nn->offset);

    // swap page structures on nodes
    struct hemem_page *tmp;
    tmp = nn->page;
    nn->page = n->page;
    nn->page->management = nn;

    n->page = tmp;
    n->page->management = n;

    // update va
    // update pages
    mmgr_list_add(&mem_active[FASTMEM][HUGEP], nn);

    hemem_wp_page(n->page, false);
    n->page->migrating = false;
    mmgr_list_add(&mem_free[SLOWMEM][HUGEP], n);
    pthread_mutex_unlock(&(nn->page->page_lock));
  }
}

static void move_cold(void)
{
  struct mmgr_list transition[NPAGETYPES];
  size_t transition_bytes = 0;

  memset(transition, 0, NPAGETYPES * sizeof(struct mmgr_list));

  //for (enum pagetypes pt = HUGEP; pt < NPAGETYPES; pt++) {
    enum pagetypes pt = HUGEP;
    struct mmgr_node *n;

    while ((n = mmgr_list_remove(&mem_inactive[FASTMEM][pt])) != NULL) {
      mmgr_list_add(&transition[pt], n);

      pthread_mutex_lock(&(n->page->page_lock));
      n->page->migrating = true;
      hemem_wp_page(n->page, true);
      transition_bytes += pt_to_pagesize(pt);

      // until enough free fastmem
      if (fastmem_freebytes + transition_bytes >= HEMEM_FASTFREE) {
        goto move;
      }
    }
  //}

move:
  if (transition_bytes == 0) {
    if (fastmem_freebytes <= HEMEM_FASTFREE) {
      LOG("COLD emergency cooling -- picking a random page\n");
      // if low on memory and all is hot, pick a random page to move down
      //for (enum pagetypes pt = HUGEP; pt < NPAGETYPES; pt++) {
        enum pagetypes pt = HUGEP;
        struct mmgr_node *n;
        while ((n = mmgr_list_remove(&mem_active[FASTMEM][pt])) != NULL) {
          mmgr_list_add(&transition[pt], n);

          pthread_mutex_lock(&(n->page->page_lock));
          n->page->migrating = true;
          hemem_wp_page(n->page, true);
          transition_bytes += pt_to_pagesize(pt);

          if (fastmem_freebytes + transition_bytes >= HEMEM_FASTFREE) {
            goto move;
          }
        }
      //}
    }
    else {
      // everything is hot and we are not low on fastmem -- nothing to move
      return;
    }
  }

  hemem_tlb_shootdown(0);

  LOG("COLD identified %zu bytes as cold\n", transition_bytes);

  // move cold pages down (and split them into base pages
  //for (enum pagetypes pt = HUGEP; pt < NPAGETYPES; pt++) {
    pt = HUGEP;

    while((n = mmgr_list_remove(&transition[pt])) != NULL) {
      //size_t times = 1;
      /*
      switch (pt) {
        case BASEP: times = 1; break;
        case HUGEP: times = 512; break;
        default: ignore_this_mmap = true; assert("Unknown page type"); ignore_this_mmap = false;
      }
      */
      //for (size_t i = 0; i < times; i++) {
        struct mmgr_node *nn = mmgr_list_remove(&mem_free[SLOWMEM][HUGEP]);
        ignore_this_mmap = true;
        assert(nn != NULL);
        ignore_this_mmap = false;

        // TODO: move memory
        slowmem_freebytes -= pt_to_pagesize(HUGEP);
        fastmem_freebytes += pt_to_pagesize(HUGEP);

        hemem_migrate_up(n->page, nn->offset);

        // swap page structures on nodes
        struct hemem_page *tmp;
        tmp = nn->page;
        nn->page = n->page;
        nn->page->management = nn;

        n->page = tmp;
        n->page->management = n;
        
        mmgr_list_add(&mem_inactive[SLOWMEM][HUGEP], nn);
      //}

      hemem_wp_page(n->page, false);
      n->page->migrating = false;
      // fastmem page is now free
      mmgr_list_add(&mem_free[FASTMEM][pt], n);
      pthread_mutex_unlock(&(nn->page->page_lock));
    }  
  //}
}

static void cool(void)
{
  struct mmgr_node *bookmark[NMEMTYPES][NPAGETYPES];
  uint64_t sweeped = 0;
  uint64_t oldsweeped;

  memset(bookmark, 0, sizeof(bookmark));

  for (sweeped = 0; sweeped < HEMEM_COOL_RATE;) {
    oldsweeped = sweeped;

    for (enum memtypes mt = FASTMEM; mt < NMEMTYPES; mt++) {
      // spread evenly over all page size types;
      //for (enum pagetypes pt = HUGEP; pt < NPAGETYPES; pt++) {
        enum pagetypes pt = HUGEP;
        struct mmgr_node *n = mmgr_list_peek(&mem_active[mt][pt]);

        if (n == NULL || bookmark[mt][pt] == n) {
          // bail out if no more pages or if we have seen this page
          // before -- we've made it all the way around the fifo queue
          continue;
        }

        n = mmgr_list_remove(&mem_active[mt][pt]);

        if (hemem_get_accessed_bit(n->page->va) == HEMEM_ACCESSED_FLAG) {
          hemem_clear_accessed_bit(n->page->va);
          mmgr_list_add(&mem_active[mt][pt], n);

          // remember first recirculated page;
          if (bookmark[mt][pt] == NULL) {
            bookmark[mt][pt] = n;
          }
        }
        else {
          mmgr_list_add(&mem_inactive[mt][pt], n);
        }

        sweeped += pt_to_pagesize(pt);
      }
    //}

    // no progress was made, bail out
    if (sweeped == oldsweeped) {
      return;
    }
  } 
}

static void thaw(void)
{
  struct mmgr_node *bookmark[NMEMTYPES][NPAGETYPES];
  uint64_t sweeped = 0;
  uint64_t oldsweeped;

  memset(bookmark, 0, sizeof(bookmark));

  for (sweeped = 0; sweeped < HEMEM_THAW_RATE;) {
    oldsweeped = sweeped;

    for (enum memtypes mt = FASTMEM; mt < NMEMTYPES; mt++) {
      // spread evenly over all page size types
      //for (enum pagetypes pt = HUGEP; pt < NPAGETYPES; pt++) {
        enum pagetypes pt = HUGEP;
        bool recirculated = false;
        struct mmgr_node *n = mmgr_list_peek(&mem_inactive[mt][pt]);

        if (n == NULL || bookmark[mt][pt] == n) {
          // bail out if no more pages or if we have seen this page
          // before -- we've made it all the way around the fifo queue
          continue;
        }
        n = mmgr_list_remove(&mem_inactive[mt][pt]);

        if (hemem_get_accessed_bit(n->page->va) == HEMEM_ACCESSED_FLAG) {
          n->tot_accesses++;
          if (n->accesses >= 2) {
            n->accesses = 0;
            hemem_clear_accessed_bit(n->page->va);
            mmgr_list_add(&mem_active[mt][pt], n);
          }
          else {
            n->accesses++;
            hemem_clear_accessed_bit(n->page->va);
            mmgr_list_add(&mem_inactive[mt][pt], n);
            recirculated = true;
          }
        }
        else {
          mmgr_list_add(&mem_inactive[mt][pt], n);
          recirculated = true;
        }

        if (recirculated && bookmark[mt][pt] == NULL) {
          bookmark[mt][pt] = n;
        }

        sweeped += pt_to_pagesize(pt);
      //}
    }
    
    // no progress made -> bail out
    if (sweeped == oldsweeped) {
      return;
    }
  }
}

static void *mmgr_thread(void *arg)
{
  for (;;) {
    usleep(HEMEM_INTERVAL);

    pthread_mutex_lock(&global_lock);

    // track hot/cold mem
    cool();
    thaw();

    hemem_tlb_shootdown(0);

    // under memory pressure in fastmem?
    if (fastmem_freebytes < HEMEM_FASTFREE) {
      move_cold();
    }

    // room in fastmem?
    if (fastmem_freebytes > 0) {
      move_hot();
    }

    hemem_tlb_shootdown(0);

    pthread_mutex_unlock(&global_lock);
  }

  return NULL;
}

static struct hemem_page* mmgr_allocate_page()
{
  struct timeval start, end;
  struct mmgr_node *node;

  pthread_mutex_lock(&global_lock);

  gettimeofday(&start, NULL);

  //for (pagetypes pt = HUGEP; pt < NPAGETYPES; pt++) {
    enum pagetypes pt = HUGEP;
    node = mmgr_list_remove(&mem_free[FASTMEM][pt]);
    if (node != NULL) {
      ignore_this_mmap = true;
      assert(node->page->in_dram);
      assert(!node->page->present);
      assert(node->page->pt == HUGEP);
      ignore_this_mmap = false;
      
      node->page->present = true;
      mmgr_list_add(&mem_active[FASTMEM][pt], node);
      fastmem_freebytes -= pt_to_pagesize(pt);

      node->page->management = node;

      pthread_mutex_unlock(&global_lock);

      gettimeofday(&end, NULL);
      LOG_TIME("mem_policy_allocate_page: %f s\n", elapsed(&start, &end));
      return node->page;
    }
  //}

  if (node == NULL) {
    pt = HUGEP;
    node = mmgr_list_remove(&mem_free[SLOWMEM][pt]);

    ignore_this_mmap = true;
    assert(node != NULL);
    assert(!node->page->in_dram);
    assert(!node->page->present);
    assert(node->page->pt == HUGEP);
    ignore_this_mmap = false;

    node->page->present = true;
    mmgr_list_add(&mem_active[SLOWMEM][pt], node);
    slowmem_freebytes -= pt_to_pagesize(pt);

    node->page->management = node;

    pthread_mutex_unlock(&global_lock);

    gettimeofday(&end, NULL);
    LOG_TIME("mem_policy_allocate_page: %f s\n", elapsed(&start, &end));

    return node->page;
  }

  return NULL;
}

struct hemem_page* hemem_mmgr_pagefault()
{
  struct hemem_page *page;

  page = mmgr_allocate_page();
  ignore_this_mmap = true;
  assert(page != NULL);
  ignore_this_mmap = false;

  return page;
}

void hemem_mmgr_remove_page(struct hemem_page *page)
{
  struct mmgr_node *node;
  struct mmgr_list *list;

  ignore_this_mmap = true;
  assert(page != NULL);
  ignore_this_mmap = false;

  node = page->management;
  ignore_this_mmap = true;
  assert(node != NULL);
  ignore_this_mmap = false;

  list = node->list;
  ignore_this_mmap = true;
  assert(list != NULL);
  ignore_this_mmap = false;

  mmgr_list_remove_node(list, node);
  page->present = false;

  if (page->in_dram) {
    mmgr_list_add(&mem_free[FASTMEM][page->pt], node);
  }
  else {
    mmgr_list_add(&mem_free[SLOWMEM][page->pt], node);
  }
}

void hemem_mmgr_init(void)
{
  pthread_t thread;
 
  for (uint64_t i = 0; i < NPAGETYPES; i++) {
    pthread_mutex_init(&(mem_free[FASTMEM][i].list_lock), NULL);
    pthread_mutex_init(&(mem_free[SLOWMEM][i].list_lock), NULL);
  }
  for (uint64_t i = 0; i < FASTMEM_HUGE_PAGES; i++) {
    struct mmgr_node *n = calloc(1, sizeof(struct mmgr_node));
    n->offset = i * HUGEPAGE_SIZE;

    struct hemem_page *p = calloc(1, sizeof(struct hemem_page));
    p->devdax_offset = i * HUGEPAGE_SIZE;
    p->present = false;
    p->in_dram = true;
    p->pt = pagesize_to_pt(HUGEPAGE_SIZE);
    pthread_mutex_init(&(p->page_lock), NULL);

    n->page = p;
    p->management = n;
    mmgr_list_add(&mem_free[FASTMEM][HUGEP], n);
  }

  for (uint64_t i = 0; i < SLOWMEM_HUGE_PAGES; i++) {
    struct mmgr_node *n = calloc(1, sizeof(struct mmgr_node));
    n->offset = i * HUGEPAGE_SIZE;

    struct hemem_page *p = calloc(1, sizeof(struct hemem_page));
    p->devdax_offset = i * HUGEPAGE_SIZE;
    p->present = false;
    p->in_dram = false;
    p->pt = pagesize_to_pt(HUGEPAGE_SIZE);
    pthread_mutex_init(&(p->page_lock), NULL);

    n->page = p;
    p->management = n;
    mmgr_list_add(&mem_free[SLOWMEM][HUGEP], n);
  }

  int r = pthread_create(&thread, NULL, mmgr_thread, NULL);
  ignore_this_mmap = true;
  assert(r == 0);
  ignore_this_mmap = false;

  LOG("Memory management policy is Hemem\n");
}

void hemem_mmgr_stats()
{

}
