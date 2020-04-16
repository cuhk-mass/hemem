#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>
#include <assert.h>
#include <inttypes.h>
#include <string.h>
#include <unistd.h>

#include "hemem.h"
#include "paging.h"
#include "timer.h"
#include "hemem-mmgr.h"

static struct hemem_list mem_free[NMEMTYPES][NPAGETYPES];
static struct hemem_list mem_active[NMEMTYPES][NPAGETYPES];
static struct hemem_list mem_inactive[NMEMTYPES][NPAGETYPES];
static pthread_mutex_t global_lock = PTHREAD_MUTEX_INITIALIZER;
static _Atomic uint64_t fastmem_freebytes = DRAMSIZE;
static _Atomic uint64_t slowmem_freebytes = NVMSIZE;

static inline uint64_t pagesize(enum pagetypes pt)
{
  switch(pt) {
  case HUGEP: return HUGEPAGE_SIZE;
  case BASEP: return BASEPAGE_SIZE;
  default: assert(!"Unknown page type");
  }
}

static void enqueue_fifo(struct hemem_list *queue, struct hemem_node *entry)
{
  assert(entry->prev == NULL);
  entry->next = queue->first;
  if(queue->first != NULL) {
    assert(queue->first->prev == NULL);
    queue->first->prev = entry;
  } else {
    assert(queue->last == NULL);
    assert(queue->numentries == 0);
    queue->last = entry;
  }

  queue->first = entry;
  queue->numentries++;
}

static struct hemem_node *dequeue_fifo(struct hemem_list *queue)
{
  struct hemem_node *ret = queue->last;

  if(ret == NULL) {
    assert(queue->numentries == 0);
    return ret;
  }

  queue->last = ret->prev;
  if(queue->last != NULL) {
    queue->last->next = NULL;
  } else {
    queue->first = NULL;
  }

  ret->prev = ret->next = NULL;
  assert(queue->numentries > 0);
  queue->numentries--;
  return ret;
}

static struct hemem_node *peek_fifo(struct hemem_list *queue)
{
  return queue->last;
}

static void move_memory(enum memtypes dst, enum memtypes src, size_t size)
{

}

static void move_hot(void)
{

}

static void move_cold(void)
{

}

static void cool(void)
{
  struct hemem_node *bookmark[NMEMTYPES][NPAGETYPES];
  uint64_t sweeped = 0;
  uint64_t oldsweeped;

  memset(bookmark, 0, sizeof(bookmark));

  for (sweeped = 0; sweeped < HEMEM_COOL_RATE;) {
    oldsweeped = sweeped;

    for (enum memtypes mt = FASTMEM; mt < NMEMTYPES; mt++) {
      // spread evenly over all page size types;
      for (enum pagetypes pt = HUGEP; pt < NPAGETYPES; pt++) {
        struct hemem_node *n = peek_fifo(&mem_active[mt][pt]);

        if (n == NULL || bookmark[mt][pt] == n) {
          // bail out if no more pages or if we have seen this page
          // before -- we've made it all the way around the fifo queue
          continue;
        }

        n = dequeue_fifo(&mem_active[mt][pt]);

        if (hemem_get_accessed_bit(n->page->va) == HEMEM_ACCESSED_FLAG) {
          hemem_clear_accessed_bit(n->page->va);
          enqueue_fifo(&mem_active[mt][pt], n);

          // remember first recirculated page;
          if (bookmark[mt][pt] == NULL) {
            bookmark[mt][pt] = n;
          }
        }
        else {
          enqueue_fifo(&mem_inactive[mt][pt], n);
        }

        sweeped += pagesize(pt);
      }
    }

    // no progress was made, bail out
    if (sweeped == oldsweeped) {
      return;
    }
  } 
}

static void thaw(void)
{
  struct hemem_node *bookmark[NMEMTYPES][NPAGETYPES];
  uint64_t sweeped = 0;
  uint64_t oldsweeped;

  memset(bookmark, 0, sizeof(bookmark));

  for (sweeped = 0; sweeped < HEMEM_THAW_RATE;) {
    oldsweeped = sweeped;

    for (enum memtypes mt = FASTMEM; mt < NMEMTYPES; mt++) {
      // spread evenly over all page size types
      for (enum pagetypes pt = HUGEP; pt < NPAGETYPES; pt++) {
        bool recirculated = false;
        struct hemem_node *n = peek_fifo(&mem_inactive[mt][pt]);

        if (n == NULL || bookmark[mt][pt] == n) {
          // bail out if no more pages or if we have seen this page
          // before -- we've made it all the way around the fifo queue
          continue;
        }
        n = dequeue_fifo(&mem_inactive[mt][pt]);

        if (hemem_get_accessed_bit(n->page->va) == HEMEM_ACCESSED_FLAG) {
          if (n->accessed2) {
            n->accessed2 = false;
            enqueue_fifo(&mem_active[mt][pt], n);
          }
          else {
            n->accessed2 = true;
            hemem_clear_accessed_bit(n->page->va);
            enqueue_fifo(&mem_inactive[mt][pt], n);
            recirculated = true;
          }
        }
        else {
          enqueue_fifo(&mem_inactive[mt][pt], n);
          recirculated = true;
        }

        if (recirculated && bookmark[mt][pt] == NULL) {
          bookmark[mt][pt] = n;
        }

        sweeped += pagesize(pt);
      }
    }
    
    // no progress made -> bail out
    if (sweeped == oldsweeped) {
      return;
    }
  }
}

static void *hemem_thread(void *arg)
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

static struct hemem_node* hemem_allocate_page(uint64_t addr, struct hemem_page *page)
{
  struct hemem_node *n = NULL;

  return n;
}

void hemem_pagefault(struct hemem_page *page)
{
}

void hemem_mmgr_init(void)
{
  uint64_t i;
  struct hemem_node *n;
  pthread_t thread;
  int r;

  for (i = 0; i < FASTMEM_HUGE_PAGES; i++) {
    n = calloc(1, sizeof(struct hemem_node));
    n->offset = i * HUGEPAGE_SIZE;
    enqueue_fifo(&mem_free[FASTMEM][HUGEP], n);
  }

  for (i = 0; i < SLOWMEM_BASE_PAGES; i++) {
    n = calloc(1, sizeof(struct hemem_node));
    n->offset = i * BASEPAGE_SIZE;
    enqueue_fifo(&mem_free[SLOWMEM][BASEP], n);
  }

  r = pthread_create(&thread, NULL, hemem_thread, NULL);
  assert(r == 0);

  LOG("Memory management policy is Hemem\n");
}
