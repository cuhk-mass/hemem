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


static void enqueue_fifo(struct hemem_list *queue, struct hemem_node *entry)
{
  ignore_this_mmap = true;
  assert(entry->prev == NULL);
  ignore_this_mmap = false;
  entry->next = queue->first;
  if(queue->first != NULL) {
    ignore_this_mmap = true;
    assert(queue->first->prev == NULL);
    ignore_this_mmap = false;
    queue->first->prev = entry;
  } else {
    ignore_this_mmap = true;
    assert(queue->last == NULL);
    assert(queue->numentries == 0);
    ignore_this_mmap = false;
    queue->last = entry;
  }

  queue->first = entry;
  queue->numentries++;
}

static struct hemem_node *dequeue_fifo(struct hemem_list *queue)
{
  struct hemem_node *ret = queue->last;

  if(ret == NULL) {
    ignore_this_mmap = true;
    assert(queue->numentries == 0);
    ignore_this_mmap = false;
    return ret;
  }

  queue->last = ret->prev;
  if(queue->last != NULL) {
    queue->last->next = NULL;
  } else {
    queue->first = NULL;
  }

  ret->prev = ret->next = NULL;
  ignore_this_mmap = true;
  assert(queue->numentries > 0);
  ignore_this_mmap = false;
  queue->numentries--;
  return ret;
}

static struct hemem_node *peek_fifo(struct hemem_list *queue)
{
  return queue->last;
}

static void move_hot(void)
{
  struct hemem_list transition[NPAGETYPES];
  size_t transition_bytes = 0;
  struct hemem_node *n;

  memset(transition, 0, NPAGETYPES * sizeof(struct hemem_list));

  // identify pages for movement and mark read-only until out of fastmem
  while (transition_bytes + pt_to_pagesize(BASEP) < fastmem_freebytes) {
    n = dequeue_fifo(&mem_active[SLOWMEM][BASEP]);

    if (n == NULL) {
      // no more active pages
      break;
    }

    enqueue_fifo(&transition[BASEP], n);
    n->page->migrating = true;
    hemem_wp_page(n->page, true);

    transition_bytes += pt_to_pagesize(BASEP);
  }

  if (transition_bytes == 0) {
    // everything is cold or we are out of fastmem -- bail out
    return;
  }

  hemem_tlb_shootdown(0);

  while ((n = dequeue_fifo(&transition[BASEP])) != NULL) {
    struct hemem_node *nn;

again:
    nn = dequeue_fifo(&mem_free[FASTMEM][BASEP]);

    if (nn == NULL) {
      // break up a huge page
      struct hemem_node *hn = dequeue_fifo(&mem_free[FASTMEM][HUGEP]);
      ignore_this_mmap = true;
      assert(hn != NULL);
      ignore_this_mmap = false;

      //hemem_demote_pages(hn->page->va);

      nn = calloc(512, sizeof(struct hemem_node));
      for (size_t i = 0; i < 512; i++) {
        // TODO: break up huge page
        nn[i].offset = hn->offset + (i * BASEPAGE_SIZE);
        enqueue_fifo(&mem_free[FASTMEM][BASEP], &nn[i]);
      }
      free(hn);

      goto again;
    }

    // TODO: move memory
    fastmem_freebytes -= pt_to_pagesize(BASEP);
    slowmem_freebytes += pt_to_pagesize(BASEP);
    // update va
    // update pages
    enqueue_fifo(&mem_active[FASTMEM][BASEP], nn);

    hemem_wp_page(n->page, false);
    n->page->migrating = false;
    enqueue_fifo(&mem_free[SLOWMEM][BASEP], n);
  }
}

static void move_cold(void)
{
  struct hemem_list transition[NPAGETYPES];
  size_t transition_bytes = 0;

  memset(transition, 0, NPAGETYPES * sizeof(struct hemem_list));

  for (enum pagetypes pt = HUGEP; pt < NPAGETYPES; pt++) {
    struct hemem_node *n;

    while ((n = dequeue_fifo(&mem_inactive[FASTMEM][pt])) != NULL) {
      enqueue_fifo(&transition[pt], n);

      n->page->migrating = true;
      hemem_wp_page(n->page, true);
      transition_bytes += pt_to_pagesize(pt);

      // until enough free fastmem
      if (fastmem_freebytes + transition_bytes >= HEMEM_FASTFREE) {
        goto move;
      }
    }
  }

move:
  if (transition_bytes == 0) {
    if (fastmem_freebytes <= HEMEM_FASTFREE) {
      LOG("COLD emergency cooling -- picking a random page\n");
      // if low on memory and all is hot, pick a random page to move down
      for (enum pagetypes pt = HUGEP; pt < NPAGETYPES; pt++) {
        struct hemem_node *n;
        while ((n = dequeue_fifo(&mem_active[FASTMEM][pt])) != NULL) {
          enqueue_fifo(&transition[pt], n);

          n->page->migrating = true;
          hemem_wp_page(n->page, true);
          transition_bytes += pt_to_pagesize(pt);

          if (fastmem_freebytes + transition_bytes >= HEMEM_FASTFREE) {
            goto move;
          }
        }
      }
    }
    else {
      // everything is hot and we are not low on fastmem -- nothing to move
      return;
    }
  }

  hemem_tlb_shootdown(0);

  LOG("COLD identified %zu bytes as cold\n", transition_bytes);

  // move cold pages down (and split them into base pages
  for (enum pagetypes pt = HUGEP; pt < NPAGETYPES; pt++) {
    struct hemem_node *n;

    while((n = dequeue_fifo(&transition[pt])) != NULL) {
      size_t times = 1;

      switch (pt) {
        case BASEP: times = 1; break;
        case HUGEP: times = 512; break;
        default: ignore_this_mmap = true; assert("Unknown page type"); ignore_this_mmap = false;
      }

      for (size_t i = 0; i < times; i++) {
        struct hemem_node *nn = dequeue_fifo(&mem_free[SLOWMEM][BASEP]);
        ignore_this_mmap = true;
        assert(nn != NULL);
        ignore_this_mmap = false;

        // TODO: move memory
        slowmem_freebytes -= pt_to_pagesize(BASEP);
        fastmem_freebytes += pt_to_pagesize(BASEP);
        // update address
        // update page
        enqueue_fifo(&mem_inactive[SLOWMEM][BASEP], nn);
      }

      hemem_wp_page(n->page, false);
      n->page->migrating = false;
      // fastmem page is now free
      enqueue_fifo(&mem_free[FASTMEM][pt], n);
    }  
  }
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

        sweeped += pt_to_pagesize(pt);
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

        sweeped += pt_to_pagesize(pt);
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

struct hemem_page* hemem_pagefault()
{
  struct hemem_page *page;
  struct hemem_node *n;
  enum pagetypes pt;
  
  pthread_mutex_lock(&global_lock);

  page = hemem_get_free_page();
  ignore_this_mmap = true;
  assert(page != NULL);
  ignore_this_mmap = false;

  for (pt = HUGEP; pt < NPAGETYPES; pt++) {
    // check that we're not fragmented at this page size
    //TODO?

    n = dequeue_fifo(&mem_free[FASTMEM][pt]);
    if (n != NULL) {
      n->page = page;
      n->page->pt = pt;
      n->page->in_dram = true;
      enqueue_fifo(&mem_active[FASTMEM][pt], n);
      fastmem_freebytes -= pt_to_pagesize(pt);
      break;
    }
  }

  if (n == NULL) {
    // out of fastmem, look for slowmem
    pt = BASEP;
    n = dequeue_fifo(&mem_free[SLOWMEM][pt]);

    // if NULL, totally out of memory
    ignore_this_mmap = true;
    assert(n != NULL);
    ignore_this_mmap = false;

    n->page = page;
    n->page->pt = pt;
    n->page->in_dram = false;
    enqueue_fifo(&mem_inactive[SLOWMEM][pt], n);
    slowmem_freebytes -= pt_to_pagesize(pt);
  }

  pthread_mutex_unlock(&global_lock);

  page->devdax_offset = n->offset;
  return page;
}

void hemem_mmgr_remove_page(struct hemem_page *page)
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
  ignore_this_mmap = true;
  assert(r == 0);
  ignore_this_mmap = false;

  LOG("Memory management policy is Hemem\n");
}
