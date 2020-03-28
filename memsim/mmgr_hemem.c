#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>
#include <assert.h>
#include <inttypes.h>
#include <string.h>

#include "shared.h"

#define HEMEM_INTERVAL		1000000000ULL	// In ns

// Keep at least 10% of fastmem free
#define HEMEM_FASTFREE		(FASTMEM_SIZE / 10)
#define HEMEM_COOL_RATE		GB(10)	// Cool bytes per HEMEM_INTERVAL
#define HEMEM_THAW_RATE		(SLOWMEM_SIZE + FASTMEM_SIZE)	// Warm bytes per HEMEM_INTERVAL

#define FASTMEM_GIGA_PAGES     	(FASTMEM_SIZE / GIGA_PAGE_SIZE)
#define FASTMEM_HUGE_PAGES     	(FASTMEM_SIZE / HUGE_PAGE_SIZE)
#define FASTMEM_BASE_PAGES     	(FASTMEM_SIZE / BASE_PAGE_SIZE)

#define SLOWMEM_GIGA_PAGES	(SLOWMEM_SIZE / GIGA_PAGE_SIZE)
#define SLOWMEM_HUGE_PAGES	(SLOWMEM_SIZE / HUGE_PAGE_SIZE)
#define SLOWMEM_BASE_PAGES	(SLOWMEM_SIZE / BASE_PAGE_SIZE)

struct page {
  struct page	*next, *prev;
  // XXX: 1 vaddr per paddr, sharing not supported yet!
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
static _Atomic bool background_wait = false;
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

static struct pte *alloc_ptables(uint64_t addr, enum pagetypes ptype,
				 uint64_t paddr)
{
  struct pte *ptable = pml4, *pte, *pivot = NULL, *newtree = NULL;
  int level = ptype + 2;

  assert(level >= 2 && level <= 4);

  // Allocate page tables down to the leaf
  for(int i = 1; i < level; i++) {
    pte = &ptable[(addr >> (48 - (i * 9))) & 511];

    if(!pte->present || pte->pagemap) {
      if(pivot == NULL) {
	// This is the junction in the tree where we're allocating a
	// new subtree -- will atomically hook it in later
	pivot = pte;
	newtree = calloc(512, sizeof(struct pte));
	ptable = newtree;
	continue;
      } else {
	assert(!pte->pagemap);
	pte->present = true;
	pte->next = calloc(512, sizeof(struct pte));
      }
    }

    ptable = pte->next;
  }

  // Return last-level PTE corresponding to addr
  pte = &ptable[(addr >> (48 - (level * 9))) & 511];
  pte->addr = paddr;
  pte->pagemap = true;
  pte->present = true;

  // Update pivot PTE to guarantee atomic page table updates without locks
  if(pivot != NULL) {
    assert(newtree != NULL);
    pivot->next = newtree;
    if(pivot->pagemap) {
      pivot->pagemap = false;
      pivot->addr = 0;
    }
    pivot->present = true;
  }
  
  return pte;
}

static void move_memory(enum memtypes dst, enum memtypes src, size_t size)
{
  size_t movetime = 0;
  
  if(dst == FASTMEM) {
    assert(src == SLOWMEM);
    movetime = TIME_FASTMOVE;
  }

  if(dst == SLOWMEM) {
    assert(src == FASTMEM);
    movetime = TIME_SLOWMOVE;
  }

  if(!in_background || background_wait) {
    add_runtime(movetime);
  }
}

static void move_hot(void)
{
  struct fifo_queue transition[NPAGETYPES];
  size_t transition_bytes = 0;
  struct page *p;

  memset(transition, 0, NPAGETYPES * sizeof(struct fifo_queue));

  // Identify pages for movement and mark read-only until out of fastmem
  while(transition_bytes + page_size(BASE) < fastmem_freebytes) {
    p = dequeue_fifo(&mem_active[SLOWMEM][BASE]);

    if(p == NULL) {
      // No more active pages
      break;
    }
    
    enqueue_fifo(&transition[BASE], p);
    p->pte->readonly = true;
    p->pte->migration = true;
    transition_bytes += page_size(BASE);
  }

  if(transition_bytes == 0) {
    // Everything is cold or out of fastmem -- bail out
    return;
  }
  tlb_shootdown(0);	// Sync

  LOG("[HOT identified %zu bytes as hot]\n", transition_bytes);
  
  // Move hot pages up (TODO: and defragment)
  while((p = dequeue_fifo(&transition[BASE])) != NULL) {
    struct page *np;
    
  again:
    np = dequeue_fifo(&mem_free[FASTMEM][BASE]);

    if(np == NULL) {
      // Break up a GIGA page
      struct page *gp = dequeue_fifo(&mem_free[FASTMEM][GIGA]);
      assert(gp != NULL);

      np = calloc(262144, sizeof(struct page));
      for(size_t i = 0; i < 262144; i++) {
	np[i].paddr = gp->paddr + (i * BASE_PAGE_SIZE);
	enqueue_fifo(&mem_free[FASTMEM][BASE], &np[i]);
      }
      free(gp);
      
      goto again;
    }

    LOG("[HOT vaddr 0x%" PRIx64 ", pt = %u] paddr 0x%" PRIx64 " -> 0x%" PRIx64
	", fastmem_free = %" PRIu64 ", slowmem_free = %" PRIu64 "\n",
	p->vaddr, BASE, p->paddr, np->paddr, fastmem_freebytes, slowmem_freebytes);

    move_memory(FASTMEM, SLOWMEM, page_size(BASE));
    fastmem_freebytes -= page_size(BASE);
    slowmem_freebytes += page_size(BASE);
    np->vaddr = p->vaddr;
    np->pte = alloc_ptables(np->vaddr, BASE, np->paddr);
    assert(np->pte != NULL && np->pte == p->pte);
    enqueue_fifo(&mem_active[FASTMEM][BASE], np);

    // Release read-only lock, reset migration hint
    p->pte->readonly = false;
    p->pte->migration = false;
    // Slowmem page is now free
    enqueue_fifo(&mem_free[SLOWMEM][BASE], p);
  }
}

static void move_cold(void)
{
  struct fifo_queue transition[NPAGETYPES];
  size_t transition_bytes = 0;

  memset(transition, 0, NPAGETYPES * sizeof(struct fifo_queue));

  // Identify pages for movement, mark read-only, set migration hint
  for(enum pagetypes pt = GIGA; pt < NPAGETYPES; pt++) {
    struct page *p;
    while((p = dequeue_fifo(&mem_inactive[FASTMEM][pt])) != NULL) {
      enqueue_fifo(&transition[pt], p);

      p->pte->readonly = true;
      p->pte->migration = true;
      // Until enough free fastmem
      transition_bytes += page_size(pt);
      if(fastmem_freebytes + transition_bytes >= HEMEM_FASTFREE) {
	goto move;
      }
    }
  }

 move:
  if(transition_bytes == 0) {
    if(fastmem_freebytes < HEMEM_FASTFREE) {
      LOG("[COLD emergency cooling -- picking random pages]\n");
      // If low on memory and all is hot, we pick random pages to move down
      for(enum pagetypes pt = GIGA; pt < NPAGETYPES; pt++) {
	struct page *p;
	while((p = dequeue_fifo(&mem_active[FASTMEM][pt])) != NULL) {
	  enqueue_fifo(&transition[pt], p);

	  p->pte->readonly = true;
	  p->pte->migration = true;
	  // Until enough free fastmem
	  transition_bytes += page_size(pt);
	  if(fastmem_freebytes + transition_bytes >= HEMEM_FASTFREE) {
	    goto move;
	  }
	}
      }
    } else {
      // Everything is hot and we're not low on fastmem -- nothing to move
      return;
    }
  }
  tlb_shootdown(0);	// Sync

  LOG("[COLD identified %zu bytes as cold]\n", transition_bytes);
  
  // Move cold pages down (and split them to base pages)
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
	struct page *np = dequeue_fifo(&mem_free[SLOWMEM][BASE]);
	assert(np != NULL);

	if(i == 0) {
	  LOG("[COLD vaddr 0x%" PRIx64 ", pt = %u] paddr 0x%" PRIx64 " -> 0x%" PRIx64
	      ", fastmem_free = %" PRIu64 ", slowmem_free = %" PRIu64 "\n",
	      p->vaddr, pt, p->paddr, np->paddr, fastmem_freebytes, slowmem_freebytes);
	}
	
	move_memory(SLOWMEM, FASTMEM, page_size(BASE));
	slowmem_freebytes -= page_size(BASE);
	fastmem_freebytes += page_size(BASE);
	np->vaddr = p->vaddr + (i * BASE_PAGE_SIZE);
	np->pte = alloc_ptables(np->vaddr, BASE, np->paddr + (i * BASE_PAGE_SIZE));
	assert(np->pte != NULL);
	enqueue_fifo(&mem_inactive[SLOWMEM][BASE], np);
      }

      // Release read-only lock, reset migration hint
      p->pte->readonly = false;
      p->pte->migration = false;
      // Fastmem page is now free
      enqueue_fifo(&mem_free[FASTMEM][pt], p);
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

	if(p->vaddr < 1048576) {
	  LOG("[THAW vaddr 0x%" PRIu64 "] accessed = %u\n", p->vaddr, p->pte->accessed);
	}
	
	if(p->pte->accessed) {
	  enqueue_fifo(&mem_active[mt][pt], p);
	} else {
	  enqueue_fifo(&mem_inactive[mt][pt], p);
	}
	
	sweeped += page_size(pt);
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
  in_background = true;

  for(;;) {
    memsim_nanosleep(HEMEM_INTERVAL);

    pthread_mutex_lock(&global_lock);

    LOG("[HEMEM TICK]\n");
    
    // Track hot/cold memory
    cool();
    thaw();

    // XXX: Can comment out for less overhead & accuracy
    tlb_shootdown(0);	// Sync active bit changes in TLB

    // Move cold memory down if under memory pressure?
    if(fastmem_freebytes < HEMEM_FASTFREE) {
      move_cold();
    }
    
    // Always try to move hot memory up
    if(fastmem_freebytes > 0) {
      move_hot();
    }

    tlb_shootdown(0);	// sync

    pthread_mutex_unlock(&global_lock);
  }

  return NULL;
}

static struct page *getmem(uint64_t addr)
{
  struct page *p = NULL;
  enum pagetypes pt;

  pthread_mutex_lock(&global_lock);
  
  struct pte *pte = &pml4[(addr >> (48 - 9)) & 511], *ptable = NULL;
  if(pte->present) {
    assert(!pte->pagemap && pte->next != NULL);
    ptable = pte->next;
  }
  
  // Allocate from fastmem first, iterate over page types
  for(pt = GIGA; pt < NPAGETYPES; pt++) {
    // Check that we're not fragmented at this page size
    if(ptable != NULL) {
      int level = pt + 2;
      assert(level >= 2 && level <= 4);
      pte = &ptable[(addr >> (48 - (level * 9))) & 511];

      if(pte->present) {
	assert(!pte->pagemap && pte->next != NULL);
	// Fragmented at this level. Continue one level down.
	ptable = pte->next;
	continue;
      } else {
	// Page not present -> unfragmented at this level. Stop checking.
	ptable = NULL;
      }
    }
    
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
  
  p->pte = alloc_ptables(addr, pt, p->paddr);
  assert(p->pte != NULL);
  p->vaddr = addr & pfn_mask(pt);

  /* LOG("[ALLOC vaddr 0x%" PRIx64 ", pt = %u] paddr 0x%" PRIx64 "\n", */
  /*     p->vaddr, pt, p->paddr); */

  pthread_mutex_unlock(&global_lock);
  return p;
}

static bool under_migration(uint64_t addr)
{
  struct pte *ptable = pml4;

  for(int level = 1; level <= 4 && ptable != NULL; level++) {
    struct pte *pte = &ptable[(addr >> (48 - (level * 9))) & 511];

    if(pte->migration) {
      return true;
    }

    if(pte->pagemap) {
      // Page here -- terminate walk
      break;
    }

    ptable = pte->next;
  }

  return false;
}

void pagefault(uint64_t addr, bool readonly)
{
  if(under_migration(addr)) {
    background_wait = true;
    // Wait for current background iteration to finish
    pthread_mutex_lock(&global_lock);
    pthread_mutex_unlock(&global_lock);
    return;
  }
  
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
