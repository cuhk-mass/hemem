#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>
#include <assert.h>
#include <inttypes.h>

#include "shared.h"

#define NR_PAGES		32
#define KSWAPD_INTERVAL		100000

#define FASTMEM_PAGES	(FASTMEM_SIZE / BASE_PAGE_SIZE)
#define SLOWMEM_PAGES	(SLOWMEM_SIZE / BASE_PAGE_SIZE)

struct page {
  struct page	*next, *prev;
  uint64_t	framenum;
  struct pte	*pte;
};

struct fifo_queue {
  struct page	*first, *last;
  size_t	numentries;
};

static struct fifo_queue pages_active[NMEMTYPES], pages_inactive[NMEMTYPES];
static bool fastmem_bitmap[FASTMEM_PAGES], slowmem_bitmap[SLOWMEM_PAGES];
static pthread_mutex_t global_lock = PTHREAD_MUTEX_INITIALIZER;
static bool __thread in_kswapd = false;

int listnum(struct pte *pte)
{
  int listnum = -1;
  
  pthread_mutex_lock(&global_lock);
  
  uint64_t framenum = (pte->addr & SLOWMEM_MASK) / BASE_PAGE_SIZE;

  for(struct page *p = pages_active[SLOWMEM].first; p != NULL; p = p->next) {
    if(p->framenum == framenum) {
      listnum = 0;
      goto out;
    }
  }

  for(struct page *p = pages_inactive[SLOWMEM].first; p != NULL; p = p->next) {
    if(p->framenum == framenum) {
      listnum = 1;
      goto out;
    }
  }

  for(struct page *p = pages_active[FASTMEM].first; p != NULL; p = p->next) {
    if(p->framenum == framenum) {
      listnum = 2;
      goto out;
    }
  }

  for(struct page *p = pages_inactive[FASTMEM].first; p != NULL; p = p->next) {
    if(p->framenum == framenum) {
      listnum = 3;
      goto out;
    }
  }

 out:
  assert(listnum != -1);
  pthread_mutex_unlock(&global_lock);
  return listnum;
}

static void enqueue_fifo(struct fifo_queue *queue, struct page *entry)
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

static struct page *dequeue_fifo(struct fifo_queue *queue)
{
  struct page *ret = queue->last;

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

static void shrink_caches(struct fifo_queue *pages_active,
			  struct fifo_queue *pages_inactive)
{
  size_t nr_pages = 1;
  /* size_t nr_pages = */
  /*   NR_PAGES * pages_active->numentries / ((pages_inactive->numentries + 1) * 2); */

  // Move cold pages down (or rotate)
  while(nr_pages > 0 && pages_active->numentries > 0) {
    struct page *p = dequeue_fifo(pages_active);

    if(p->pte->accessed) {
      // XXX: Dangerous. Introduce soft accessed bit instead, like Linux?
      p->pte->accessed = false;
      enqueue_fifo(pages_active, p);
    } else {
      // XXX: Dangerous. Introduce soft accessed bit instead, like Linux?
      /* p->pte->accessed = true; */
      enqueue_fifo(pages_inactive, p);
      nr_pages--;
    }
  }
}

static void expand_caches(struct fifo_queue *pages_active,
			  struct fifo_queue *pages_inactive)
{
  size_t nr_pages = pages_inactive->numentries;

  // Move hot pages up
  for(size_t i = 0; i < nr_pages; i++) {
    struct page *p = dequeue_fifo(pages_inactive);

    if(p->pte->accessed) {
      enqueue_fifo(pages_active, p);
    } else {
      enqueue_fifo(pages_inactive, p);
    }
  }
}

static void *kswapd(void *arg)
{
  size_t oldruntime = 0;

  in_kswapd = true;

  for(;;) {
    while(runtime - oldruntime < KSWAPD_INTERVAL);

    pthread_mutex_lock(&global_lock);

    shrink_caches(&pages_active[FASTMEM], &pages_inactive[FASTMEM]);
    shrink_caches(&pages_active[SLOWMEM], &pages_inactive[SLOWMEM]);

    // Move_hot
    expand_caches(&pages_active[FASTMEM], &pages_inactive[FASTMEM]);
    expand_caches(&pages_active[SLOWMEM], &pages_inactive[SLOWMEM]);

    // Move hot pages from slowmem to fastmem
    for(struct page *p = dequeue_fifo(&pages_active[SLOWMEM]); p != NULL;
	p = dequeue_fifo(&pages_active[SLOWMEM])) {
      bool moved = false;

      for(int tries = 0; tries < 2 && !moved; tries++) {
	for(uint64_t i = 0; i < FASTMEM_PAGES; i++) {
	  if(fastmem_bitmap[i] == false) {
	    fastmem_bitmap[i] = true;

	    // Free slowmem
	    slowmem_bitmap[p->framenum] = false;

	    LOG("%zu cold (%" PRIu64 ") -> hot (%" PRIu64 "), "
		"slowmem.active = %zu, slowmem.inactive = %zu\n",
		runtime,
		p->framenum, i,
		pages_active[SLOWMEM].numentries,
		pages_inactive[SLOWMEM].numentries);

	    // Remap page
	    tlb_shootdown(p->framenum * BASE_PAGE_SIZE);
	    p->framenum = i;
	    p->pte->addr = p->framenum * BASE_PAGE_SIZE;

	    // Put on fastmem active list
	    enqueue_fifo(&pages_active[FASTMEM], p);

	    moved = true;
	    break;
	  }
	}

	if(moved) {
	  // Page moved -- we're done for this page
	  break;
	}

	// Not moved - Out of fastmem - move cold page down
	struct page *cp = dequeue_fifo(&pages_inactive[FASTMEM]);
	if(cp == NULL) {
	  // All fastmem pages are hot -- bail out
	  enqueue_fifo(&pages_active[SLOWMEM], p);
	  goto out;
	}

	for(uint64_t i = 0; i < SLOWMEM_PAGES; i++) {
	  if(slowmem_bitmap[i] == false) {
	    slowmem_bitmap[i] = true;

	    // Free fastmem
	    fastmem_bitmap[cp->framenum] = false;

	    // Remap page
	    tlb_shootdown(cp->framenum * BASE_PAGE_SIZE);
	    cp->framenum = i;
	    cp->pte->addr = (cp->framenum * BASE_PAGE_SIZE) | SLOWMEM_BIT;

	    // Put on slowmem inactive list
	    enqueue_fifo(&pages_inactive[SLOWMEM], cp);

	    /* fprintf(stderr, "%zu hot -> cold\n", runtime); */

	    break;
	  }
	}
      }
    }

  out:
    pthread_mutex_unlock(&global_lock);
    
    oldruntime = runtime;
  }

  return NULL;
}

uint64_t getmem(uint64_t addr, struct pte *pte)
{
  pthread_mutex_lock(&global_lock);

  for(int tries = 0; tries < 2; tries++) {
    // Allocate from fastmem, put on active FIFO queue
    for(uint64_t i = 0; i < FASTMEM_PAGES; i++) {
      if(fastmem_bitmap[i] == false) {
	fastmem_bitmap[i] = true;

	struct page *newpage = calloc(1, sizeof(struct page));
	newpage->framenum = i;
	newpage->pte = pte;
	enqueue_fifo(&pages_active[FASTMEM], newpage);

	pthread_mutex_unlock(&global_lock);
	return newpage->framenum * BASE_PAGE_SIZE;
      }
    }

    // Move a page to cold memory
    if(pages_inactive[FASTMEM].numentries == 0) {
      // Force some pages down if there aren't any
      shrink_caches(&pages_active[FASTMEM], &pages_inactive[FASTMEM]);
      /* shrink_caches(&pages_active[SLOWMEM], &pages_inactive[SLOWMEM]); */
    }

    // Move a cold page from fastmem to slowmem
    struct page *p = dequeue_fifo(&pages_inactive[FASTMEM]);
    for(uint64_t i = 0; i < SLOWMEM_PAGES; i++) {
      if(slowmem_bitmap[i] == false) {
	slowmem_bitmap[i] = true;

	// Emulate memory copy from fast to slow mem
	if(!in_kswapd) {
	  runtime += TIME_SLOWMOVE;
	}

	// Free fastmem
	fastmem_bitmap[p->framenum] = false;

	LOG("%zu OOM hot (%" PRIu64 ") -> cold (%" PRIu64 ")\n",
	    runtime, p->framenum, i);

	// Remap page
	tlb_shootdown(p->framenum * BASE_PAGE_SIZE);
	p->framenum = i;
	p->pte->addr = (p->framenum * BASE_PAGE_SIZE) | SLOWMEM_BIT;

	// Put on slowmem inactive list
	enqueue_fifo(&pages_inactive[SLOWMEM], p);

	break;
      }
    }

  }

  pthread_mutex_unlock(&global_lock);
  assert(!"Out of memory");
}

void getmem_init(void)
{
  pthread_t thread;
  
  int r = pthread_create(&thread, NULL, kswapd, NULL);
  assert(r == 0);
}
