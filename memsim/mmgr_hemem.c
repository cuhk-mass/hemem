#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>
#include <assert.h>
#include <inttypes.h>

#include "shared.h"

#define HEMEM_INTERVAL		100000	// In ns

// Keep at least 10% of fastmem free
#define HEMEM_FASTFREE		(FASTMEM_SIZE / 10)
#define HEMEM_MIGRATE_RATE	GB(1)

#define FASTMEM_GIGA_PAGES     	(FASTMEM_SIZE / GIGA_PAGE_SIZE)
#define FASTMEM_HUGE_PAGES     	(FASTMEM_SIZE / HUGE_PAGE_SIZE)
#define FASTMEM_BASE_PAGES     	(FASTMEM_SIZE / BASE_PAGE_SIZE)

#define SLOWMEM_GIGA_PAGES	(SLOWMEM_SIZE / GIGA_PAGE_SIZE)
#define SLOWMEM_HUGE_PAGES	(SLOWMEM_SIZE / HUGE_PAGE_SIZE)
#define SLOWMEM_BASE_PAGES	(SLOWMEM_SIZE / BASE_PAGE_SIZE)

enum pagetypes {
  GIGA = 0, HUGE, BASE, NPAGETYPES
};

struct page {
  struct page	*next, *prev;
  uint64_t	paddr, vaddr;
  struct pte	*pte;
};

struct fifo_queue {
  struct page	*first, *last;
  size_t	numentries;
};

static struct pte pml4[512]; // Top-level page table (we only emulate one process)
static struct fifo_queue fastmem_free[NPAGETYPES], slowmem_free[NPAGETYPES],
  fastmem_active[NPAGETYPES], slowmem_active[NPAGETYPES],
  /*fastmem_inactive[NPAGETYPES],*/ slowmem_inactive[NPAGETYPES],
  transition[NPAGETYPES];
static pthread_mutex_t global_lock = PTHREAD_MUTEX_INITIALIZER;
static bool __thread in_background = false;
static _Atomic uint64_t fastmem_freebytes = FASTMEM_SIZE;
static _Atomic uint64_t slowmem_freebytes = SLOWMEM_SIZE;

int listnum(struct pte *pte)
{
  // Unused debug function
  return -1;
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

static uint64_t page_size(enum pagetypes pt)
{
  switch(pt) {
  case GIGA: return GIGA_PAGE_SIZE;
  case HUGE: return HUGE_PAGE_SIZE;
  case BASE: return BASE_PAGE_SIZE;
  default: assert(!"Unknown page type");
  }
}

static uint64_t pfn_mask(enum pagetypes pt)
{
  switch(pt) {
  case GIGA: return GIGA_PFN_MASK;
  case HUGE: return HUGE_PFN_MASK;
  case BASE: return BASE_PFN_MASK;
  default: assert(!"Unknown page type");
  }
}

static struct pte *alloc_ptables(uint64_t addr, enum pagetypes ptype)
{
  struct pte *ptable = pml4, *pte;
  int level = ptype + 2;

  assert(level >= 2 && level <= 4);
  
  // Allocate page tables down to the leaf
  for(int i = 1; i < level; i++) {
    pte = &ptable[(addr >> (48 - (i * 9))) & 511];

    if(!pte->present || pte->pagemap) {
      pte->present = true;
      pte->next = calloc(512, sizeof(struct pte));
      if(pte->pagemap) {
	pte->pagemap = false;
	pte->addr = 0;
      }
    }

    ptable = pte->next;
  }

  // Return last-level PTE corresponding to addr
  pte = &ptable[(addr >> (48 - (level * 9))) & 511];
  pte->present = true;
  pte->pagemap = true;

  return pte;
}

static void *hemem_thread(void *arg)
{
  size_t oldruntime = 0;

  in_background = true;

  for(;;) {
    while(runtime - oldruntime < HEMEM_INTERVAL);

    pthread_mutex_lock(&global_lock);

    // Under memory pressure?
    if(fastmem_freebytes >= HEMEM_FASTFREE) {
      goto done;
    }

    // Analyze HEMEM_MIGRATE_RATE data for hot/cold
    int64_t tosweep = HEMEM_MIGRATE_RATE;
    while(tosweep > 0) {
      // Analyze fastmem data for cold
      // Spread evenly over all page size types
      // XXX: Probably better to sweep in physical memory to defragment
      for(enum pagetypes pt = GIGA; pt < NPAGETYPES; pt++) {
	struct page *p = dequeue_fifo(&fastmem_active[pt]);
	
	if(p == NULL) {
	  continue;
	}

	if(p->pte->accessed) {
	  p->pte->accessed = false;
	  enqueue_fifo(&fastmem_active[pt], p);
	} else {
	  enqueue_fifo(&transition[pt], p);
	  /* enqueue_fifo(&fastmem_inactive[pt], p); */
	  tosweep -= page_size(pt);
	  if(tosweep == 0) {
	    break;
	  }
	}
      }

#if 0
      // Analyze inactive pages for hot
      for(enum pagetypes pt = GIGA; pt < NPAGETYPES; pt++) {
	struct page *p = dequeue_fifo(&fastmem_inactive[pt]);

	if(p == NULL) {
	  continue;
	}

	if(p->pte->accessed) {
	  enqueue_fifo(&fastmem_active[pt], p);
	} else {
	  enqueue_fifo(&fastmem_inactive[pt], p);
	}

	tosweep -= page_size(pt);
      }

      // TODO: Check slowmem, too
#endif
    }

#if 0
    // Identify cold pages in fastmem
    while(fastmem_freebytes < HEMEM_FASTFREE) {
      // Start with smallest, then larger page sizes
      for(enum pagetypes rpt = GIGA; rpt < NPAGETYPES; rpt++) {
	enum pagetypes pt = BASE - rpt;	// Go in reverse size order to realize small to large
	struct page *p;
	while((p = dequeue_fifo(&fastmem_active[pt])) != NULL) {
	  if(p->pte->accessed) {
	    p->pte->accessed = false;
	    enqueue_fifo(&fastmem_active[pt], p);
	  } else {
	    p->pte->readonly = true;
	    enqueue_fifo(&transition[pt], p);
	  }
	}
      }
    }
#endif
    tlb_shootdown(0);	// Sync changes to page tables

    // Move pages down (and split them to base pages)
    for(enum pagetypes pt = GIGA; pt < NPAGETYPES; pt++) {
      struct page *p;
      while((p = dequeue_fifo(&transition[pt])) != NULL) {
	size_t times = 1;
	
	switch(pt) {
	case BASE: times = 1; break;
	case HUGE: times = 512; break;
	case GIGA: times = 262144; break;
	default: assert(!"Unknown page type"); break;
	}

	for(size_t i = 0; i < times; i++) {
	  struct page *np = dequeue_fifo(&slowmem_free[BASE]);
	  assert(np != NULL);

	  // XXX: Move data in background
	  slowmem_freebytes -= page_size(BASE);
	  fastmem_freebytes += page_size(BASE);
	  np->pte = alloc_ptables(p->vaddr + (i * BASE_PAGE_SIZE), BASE);
	  assert(np->pte != NULL);
	  np->pte->addr = np->paddr + (i * BASE_PAGE_SIZE);
	  enqueue_fifo(&slowmem_inactive[pt], np);
	}

	// Fastmem page is now free
	enqueue_fifo(&fastmem_free[pt], p);
      }
    }
    tlb_shootdown(0);	// sync

  done:
    pthread_mutex_unlock(&global_lock);
    oldruntime = runtime;
  }

  return NULL;
}

static struct page *getmem(uint64_t addr)
{
  struct page *p = NULL;
  enum pagetypes pt;

  pthread_mutex_lock(&global_lock);

  // Allocate from fastmem first, iterate over page types
  for(pt = GIGA; pt < NPAGETYPES; pt++) {
    p = dequeue_fifo(&fastmem_free[pt]);
    if(p != NULL) {
      enqueue_fifo(&fastmem_active[pt], p);
      fastmem_freebytes -= page_size(pt);
      break;
    }
  }
  if(p == NULL) {
    // If out of fastmem, look for slowmem
    pt = BASE;
    p = dequeue_fifo(&slowmem_free[pt]);
    // If NULL, we're totally out of mem
    assert(p != NULL);
    enqueue_fifo(&slowmem_active[pt], p);
    slowmem_freebytes -= page_size(pt);
  }

  p->pte = alloc_ptables(addr, pt);
  assert(p->pte != NULL);
  p->pte->addr = p->paddr;
  p->vaddr = addr & pfn_mask(pt);

  pthread_mutex_unlock(&global_lock);
  return p;
}

void pagefault(uint64_t addr)
{
  getmem(addr);
}

void mmgr_init(void)
{
  cr3 = pml4;

  // Fastmem: all giga pages in the beginning
  for(int i = 0; i < FASTMEM_GIGA_PAGES; i++) {
    struct page *p = calloc(1, sizeof(struct page));
    p->paddr = i * GIGA_PAGE_SIZE;
    enqueue_fifo(&fastmem_free[GIGA], p);
  }
  // Slowmem: Try with base pages (lots of memory use and likely slow,
  // but hey, it's slowmem!)
  for(int i = 0; i < SLOWMEM_BASE_PAGES; i++) {
    struct page *p = calloc(1, sizeof(struct page));
    p->paddr = (i * BASE_PAGE_SIZE) | SLOWMEM_BIT;
    enqueue_fifo(&slowmem_free[BASE], p);
  }
  
  pthread_t thread;
  int r = pthread_create(&thread, NULL, hemem_thread, NULL);
  assert(r == 0);
}
