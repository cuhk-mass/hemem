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
#define HEMEM_COOL_RATE		GB(1)	// Cool bytes per HEMEM_INTERVAL
#define HEMEM_THAW_RATE		GB(1)	// Warm bytes per HEMEM_INTERVAL

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
static struct fifo_queue mem_free[NMEMTYPES][NPAGETYPES],
  mem_active[NMEMTYPES][NPAGETYPES], mem_inactive[NMEMTYPES][NPAGETYPES];
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

static void move_hot(void)
{
  struct page *p;

  // Move hot pages up (and defragment)
  while((p = dequeue_fifo(&mem_active[SLOWMEM][BASE])) != NULL) {
    struct page *np = dequeue_fifo(&mem_free[FASTMEM][BASE]);
    assert(np != NULL);

    // XXX: Move data in background
    fastmem_freebytes -= page_size(BASE);
    slowmem_freebytes += page_size(BASE);
    np->pte = alloc_ptables(p->vaddr, BASE);
    assert(np->pte != NULL);
    np->pte->addr = np->paddr;
    enqueue_fifo(&mem_active[FASTMEM][BASE], np);
    enqueue_fifo(&mem_free[SLOWMEM][BASE], p);

    // Stop if under memory pressure
    if(fastmem_freebytes < HEMEM_FASTFREE) {
      return;
    }
  }
}

static void move_cold(void)
{
  // Move cold pages down (and split them to base pages)
  for(enum pagetypes pt = GIGA; pt < NPAGETYPES; pt++) {
    struct page *p;
    while((p = dequeue_fifo(&mem_inactive[FASTMEM][pt])) != NULL) {
      size_t times = 1;

      switch(pt) {
      case BASE: times = 1; break;
      case HUGE: times = 512; break;
      case GIGA: times = 262144; break;
      default: assert(!"Unknown page type"); break;
      }

      for(size_t i = 0; i < times; i++) {
	struct page *np = dequeue_fifo(&mem_free[SLOWMEM][BASE]);
	assert(np != NULL);

	// XXX: Move data in background
	slowmem_freebytes -= page_size(BASE);
	fastmem_freebytes += page_size(BASE);
	np->vaddr = p->vaddr + (i * BASE_PAGE_SIZE);
	np->pte = alloc_ptables(np->vaddr, BASE);
	assert(np->pte != NULL);
	np->pte->addr = np->paddr + (i * BASE_PAGE_SIZE);
	enqueue_fifo(&mem_inactive[SLOWMEM][BASE], np);
      }

      // Fastmem page is now free
      enqueue_fifo(&mem_free[FASTMEM][pt], p);

      // Until enough free fastmem
      if(fastmem_freebytes >= HEMEM_FASTFREE) {
	return;
      }
    }
  }
}

static void cool(void)
{
  // Data cools at HEMEM_COOL_RATE per HEMEM_INTERVAL
  for(uint64_t sweeped = 0; sweeped < HEMEM_COOL_RATE;) {
    uint64_t oldsweeped = sweeped;

    for(enum memtypes mt = FASTMEM; mt < NMEMTYPES; mt++) {
      // Spread evenly over all page size types
      // XXX: Probably better to sweep in physical memory to defragment
      for(enum pagetypes pt = GIGA; pt < NPAGETYPES; pt++) {
	struct page *p = dequeue_fifo(&mem_active[mt][pt]);

	if(p == NULL) {
	  continue;
	}

	if(p->pte->accessed) {
	  p->pte->accessed = false;
	  enqueue_fifo(&mem_active[mt][pt], p);
	} else {
	  enqueue_fifo(&mem_inactive[mt][pt], p);
	}

	sweeped += page_size(pt);
	/* if(sweeped >= HEMEM_COOL_RATE) { */
	/*   return; */
	/* } */
      }
    }

    // If no progress then bail out
    if(sweeped == oldsweeped) {
      return;
    }
  }
}

static void thaw(void)
{
  // Data thaws at HEMEM_THAW_RATE per HEMEM_INTERVAL
  for(uint64_t sweeped = 0; sweeped < HEMEM_THAW_RATE;) {
    uint64_t oldsweeped = sweeped;
    
    for(enum memtypes mt = FASTMEM; mt < NMEMTYPES; mt++) {
      // Spread evenly over all page size types
      // XXX: Probably better to sweep in physical memory to defragment
      for(enum pagetypes pt = GIGA; pt < NPAGETYPES; pt++) {
	struct page *p = dequeue_fifo(&mem_inactive[mt][pt]);

	if(p == NULL) {
	  continue;
	}

	if(p->pte->accessed) {
	  enqueue_fifo(&mem_active[mt][pt], p);
	} else {
	  enqueue_fifo(&mem_inactive[mt][pt], p);
	}

	sweeped += page_size(pt);
	/* if(sweeped >= HEMEM_THAW_RATE) { */
	/*   return; */
	/* } */
      }
    }

    // If no progress then bail out
    if(sweeped == oldsweeped) {
      return;
    }
  }
}

static void *hemem_thread(void *arg)
{
  size_t oldruntime = 0;

  in_background = true;

  for(;;) {
    while(runtime - oldruntime < HEMEM_INTERVAL);

    pthread_mutex_lock(&global_lock);

    cool();
    thaw();

    // XXX: Can comment out for less overhead & accuracy
    tlb_shootdown(0);	// Sync active bit changes in TLB

    // Under memory pressure?
    if(fastmem_freebytes >= HEMEM_FASTFREE) {
      move_hot();
    } else {
      move_cold();
    }

    tlb_shootdown(0);	// sync

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
    p = dequeue_fifo(&mem_free[FASTMEM][pt]);
    if(p != NULL) {
      enqueue_fifo(&mem_active[FASTMEM][pt], p);
      fastmem_freebytes -= page_size(pt);
      break;
    }
  }
  if(p == NULL) {
    // If out of fastmem, look for slowmem
    pt = BASE;
    p = dequeue_fifo(&mem_free[SLOWMEM][pt]);
    // If NULL, we're totally out of mem
    assert(p != NULL);
    enqueue_fifo(&mem_active[SLOWMEM][pt], p);
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
    enqueue_fifo(&mem_free[FASTMEM][GIGA], p);
  }
  // Slowmem: Try with base pages (lots of memory use and likely slow,
  // but hey, it's slowmem!)
  for(int i = 0; i < SLOWMEM_BASE_PAGES; i++) {
    struct page *p = calloc(1, sizeof(struct page));
    p->paddr = (i * BASE_PAGE_SIZE) | SLOWMEM_BIT;
    enqueue_fifo(&mem_free[SLOWMEM][BASE], p);
  }
  
  pthread_t thread;
  int r = pthread_create(&thread, NULL, hemem_thread, NULL);
  assert(r == 0);
}
