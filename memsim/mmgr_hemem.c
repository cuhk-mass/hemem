#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>
#include <assert.h>
#include <inttypes.h>
#include <string.h>

#include "shared.h"
#include "uthash.h"

/* #define PERF_METHOD */
#define PERF_LIMIT		10000

#define HEMEM_INTERVAL		MS(10)

// Keep at least 10% of fastmem free
#define HEMEM_FASTFREE		(FASTMEM_SIZE / 10)

#define FASTMEM_GIGA_PAGES     	(FASTMEM_SIZE / GIGA_PAGE_SIZE)
#define FASTMEM_HUGE_PAGES     	(FASTMEM_SIZE / HUGE_PAGE_SIZE)
#define FASTMEM_BASE_PAGES     	(FASTMEM_SIZE / BASE_PAGE_SIZE)

#define SLOWMEM_GIGA_PAGES	(SLOWMEM_SIZE / GIGA_PAGE_SIZE)
#define SLOWMEM_HUGE_PAGES	(SLOWMEM_SIZE / HUGE_PAGE_SIZE)
#define SLOWMEM_BASE_PAGES	(SLOWMEM_SIZE / BASE_PAGE_SIZE)

struct page {
  struct page		*next, *prev;
  // XXX: 1 vaddr per paddr - sharing not supported yet!
  uint64_t		paddr, vaddr;
  struct pte		*pte;
  enum pagetypes	type;
  size_t		accesses, tot_accesses;
  UT_hash_handle	hh;

  // Statistics
};

struct fifo_queue {
  struct page	*first, *last;
  size_t	numentries;
};

static struct pte pml4[512]; // Top-level page table (we only emulate one process)
static struct fifo_queue mem_free[NMEMTYPES][NPAGETYPES];
static struct page *mem_active[NMEMTYPES], *mem_inactive[NMEMTYPES];
static pthread_mutex_t global_lock = PTHREAD_MUTEX_INITIALIZER;
static bool __thread in_background = false;
static _Atomic bool background_wait = false;
static _Atomic uint64_t fastmem_freebytes = FASTMEM_SIZE;
static _Atomic uint64_t slowmem_freebytes = SLOWMEM_SIZE;
static _Atomic size_t hotset_size = 0;
static int recstats_level;
static pthread_t hemem_threadid;
static bool hemem_thread_running = true;
static size_t tot_sweeps = 0;

#ifdef PERF_METHOD
static size_t perf_buckets[SLOWMEM_SIZE / GIGA_PAGE_SIZE];
#endif

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

static void add_hash(struct page **hashtable, struct page *entry)
{
  struct page *p;
  
  HASH_FIND(hh, *hashtable, &entry->paddr, sizeof(uint64_t), p);
  assert(p == NULL);
  HASH_ADD(hh, *hashtable, paddr, sizeof(uint64_t), entry);
}

static struct page *find_hash(struct page **hashtable, uint64_t paddr)
{
  struct page *p;
  
  HASH_FIND(hh, *hashtable, &paddr, sizeof(uint64_t), p);
  return p;
}

static void del_hash(struct page **hashtable, struct page *entry)
{
  HASH_DEL(*hashtable, entry);
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

    // Reset all_slow hint if part of virtual range is not in slowmem
    if(!(paddr & SLOWMEM_BIT)) {
      pte->all_slow = false;
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

static void copy_page(struct page *new, struct page *old)
{
  new->vaddr = old->vaddr;
  new->accesses = old->accesses;
  new->tot_accesses = old->tot_accesses;
}

static void move_hot(void)
{
  struct fifo_queue transition;
  size_t transition_bytes = 0;
  struct page *p, *tmp;

  memset(&transition, 0, sizeof(struct fifo_queue));

  // Identify pages for movement and mark read-only until out of fastmem
  HASH_ITER(hh, mem_active[SLOWMEM], p, tmp) {
    if(transition_bytes + page_size(p->type) >= fastmem_freebytes) {
      break;
    }
    
    del_hash(&mem_active[SLOWMEM], p);
    enqueue_fifo(&transition, p);
    
    p->pte->readonly = true;
    p->pte->migration = true;
    transition_bytes += page_size(p->type);
  }

  if(transition_bytes == 0) {
    // Everything is cold or out of fastmem -- bail out
    return;
  }
  tlb_shootdown(0);	// Sync

  LOG("[HOT identified %zu bytes as hot]\n", transition_bytes);
  
  // Move hot pages up (TODO: and defragment)
  while((p = dequeue_fifo(&transition)) != NULL) {
    struct page *np;
    
  again:
    np = dequeue_fifo(&mem_free[FASTMEM][p->type]);

    assert(np != NULL);
    if(np == NULL) {
      // Break up a GIGA page
      struct page *gp = dequeue_fifo(&mem_free[FASTMEM][GIGA]);
      assert(gp != NULL);

      np = calloc(262144, sizeof(struct page));
      for(size_t i = 0; i < 262144; i++) {
	np[i].type = BASE;
	np[i].paddr = gp->paddr + (i * BASE_PAGE_SIZE);
	enqueue_fifo(&mem_free[FASTMEM][BASE], &np[i]);
      }
      free(gp);
      
      goto again;
    }

    if(p->vaddr < 1048576) {
      LOG("[HOT vaddr 0x%" PRIx64 ", pt = %u] paddr 0x%" PRIx64 " -> 0x%" PRIx64
	  ", fastmem_free = %" PRIu64 ", slowmem_free = %" PRIu64 "\n",
	  p->vaddr, BASE, p->paddr, np->paddr, fastmem_freebytes, slowmem_freebytes);
    }

    move_memory(FASTMEM, SLOWMEM, page_size(p->type));
    fastmem_freebytes -= page_size(p->type);
    slowmem_freebytes += page_size(p->type);
    copy_page(np, p);
    np->pte = alloc_ptables(np->vaddr, p->type, np->paddr);
    assert(np->pte != NULL && np->pte == p->pte);
    np->pte->ups++;
    add_hash(&mem_active[FASTMEM], np);

    // Release read-only lock, reset migration hint
    p->pte->readonly = false;
    p->pte->migration = false;
    // Slowmem page is now free
    enqueue_fifo(&mem_free[SLOWMEM][p->type], p);
  }
}

static void move_cold(void)
{
  struct fifo_queue transition;
  size_t transition_bytes = 0;
  struct page *p, *tmp;

  memset(&transition, 0, sizeof(struct fifo_queue));

  // Identify pages for movement, mark read-only, set migration hint
  HASH_ITER(hh, mem_inactive[FASTMEM], p, tmp) {
    del_hash(&mem_inactive[FASTMEM], p);
    enqueue_fifo(&transition, p);

    p->pte->readonly = true;
    p->pte->migration = true;
    // Until enough free fastmem
    transition_bytes += page_size(p->type);
    if(fastmem_freebytes + transition_bytes >= HEMEM_FASTFREE) {
      break;
    }
  }

  if(transition_bytes == 0) {
    if(fastmem_freebytes < HEMEM_FASTFREE) {
      LOG("[COLD emergency cooling -- picking random pages]\n");
      // If low on memory and all is hot, we pick random pages to move down
      HASH_ITER(hh, mem_active[FASTMEM], p, tmp) {
	del_hash(&mem_active[FASTMEM], p);
	hotset_size -= page_size(p->type);
	enqueue_fifo(&transition, p);

	p->pte->readonly = true;
	p->pte->migration = true;
	// Until enough free fastmem
	transition_bytes += page_size(p->type);
	if(fastmem_freebytes + transition_bytes >= HEMEM_FASTFREE) {
	  break;
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
  while((p = dequeue_fifo(&transition)) != NULL) {
    /* size_t times = 1; */

    /* /\* p->pte->downs++; *\/ */
    
    /* switch(p->type) { */
    /* case BASE: times = 1; break; */
    /* case HUGE: times = 512; break; */
    /* case GIGA: times = 262144; break; */
    /* default: assert(!"Unknown page type"); break; */
    /* } */

    /* for(size_t i = 0; i < times; i++) { */
      struct page *np = dequeue_fifo(&mem_free[SLOWMEM][GIGA]);
      assert(np != NULL);

      if(/* i == 0 && */ p->vaddr < 1048576) {
	LOG("[COLD vaddr 0x%" PRIx64 ", pt = %u] paddr 0x%" PRIx64 " -> 0x%" PRIx64
	    ", fastmem_free = %" PRIu64 ", slowmem_free = %" PRIu64 "\n",
	    p->vaddr, p->type, p->paddr, np->paddr, fastmem_freebytes, slowmem_freebytes);
      }
	
      move_memory(SLOWMEM, FASTMEM, page_size(GIGA));
      slowmem_freebytes -= page_size(GIGA);
      fastmem_freebytes += page_size(GIGA);
      /* np->vaddr = p->vaddr + (i * BASE_PAGE_SIZE); */
      copy_page(np, p);
      np->pte = alloc_ptables(np->vaddr, GIGA, np->paddr);
      assert(np->pte != NULL);
      np->pte->downs++;
      add_hash(&mem_inactive[SLOWMEM], np);
    /* } */

    // Release read-only lock, reset migration hint, set all_slow hint
    p->pte->all_slow = true;
    p->pte->readonly = false;
    p->pte->migration = false;
    // Fastmem page is now free
    enqueue_fifo(&mem_free[FASTMEM][p->type], p);
  }
}

static size_t pages_swept[NPAGETYPES], pages_skipped[4];
static int level;
static size_t sweep_hotset;

// Returns whether all pages are in slowmem at the level sweeped
static bool sweep(struct pte *ptable)
{
  bool all_slow = true;
  
  level++;
  
  for(int i = 0; i < 512; i++) {
    if(!ptable[i].present) {
      continue;
    }
    
    if(ptable[i].pagemap) {
      enum memtypes mt = ptable[i].addr & SLOWMEM_BIT ? SLOWMEM : FASTMEM;

      assert(level >= 2 && level <= 4);
      pages_swept[level - 2]++;

      if(mt == FASTMEM) {
	all_slow = false;
      }
      
      if(!ptable[i].accessed) {
	// Page not accessed - mark inactive if active
	struct page *p = find_hash(&mem_active[mt], ptable[i].addr);
	if(p != NULL) {
	  p->accesses = 0;
	  del_hash(&mem_active[mt], p);
	  add_hash(&mem_inactive[mt], p);
	  hotset_size -= page_size(p->type);

	  if(p->vaddr < 1048576) {
	    LOG("[NOW COLD vaddr 0x%" PRIx64 "] accesses = %zu\n",
		p->vaddr, p->accesses);
	  }
	} else {
	  struct page *p = find_hash(&mem_inactive[mt], ptable[i].addr);
	  if(p != NULL) {
	    p->accesses = 0;
	    
	    if(p->vaddr < 1048576) {
	      LOG("[STILL COLD vaddr 0x%" PRIx64 "] accesses = %zu\n",
		  p->vaddr, p->accesses);
	    }
	  }
	}
      } else {
	struct page *p = find_hash(&mem_inactive[mt], ptable[i].addr);

	ptable[i].accessed = false;

	sweep_hotset += page_size(level - 2);
	
	if(p == NULL) {
	  p = find_hash(&mem_active[mt], ptable[i].addr);
	  if(p != NULL) {
	    p->accesses++;
	    p->tot_accesses++;
	  }
	  
	  continue;
	}
	
	p->tot_accesses++;
	
	if(p->accesses >= 5) {
	  if(p->vaddr < 1048576) {
	    LOG("[NOW HOT vaddr 0x%" PRIx64 "] accesses = %zu\n",
		p->vaddr, p->accesses);
	  }
	  /* p->accesses = 250; */
	  p->accesses++;
	  del_hash(&mem_inactive[mt], p);
	  add_hash(&mem_active[mt], p);
	  hotset_size += page_size(p->type);
	} else {
	  p->accesses++;
	}

	/* if(p->vaddr < 1048576) { */
	/*   LOG("[SWEEP vaddr 0x%" PRIx64 "] accesses = %zu\n", */
	/*       p->vaddr, p->accesses); */
	/* } */
      }
    } else {
      assert(ptable[i].next != NULL);

      // Skip if all pages are in slowmem and none were touched
      if(ptable[i].all_slow && !ptable[i].accessed) {
	pages_skipped[level - 1]++;
	continue;
      }
      
      // Reset accessed bit at this level
      ptable[i].accessed = false;
      
      bool is_slow = sweep(ptable[i].next);

      if(!is_slow) {
	assert(!ptable[i].all_slow);
	all_slow = false;
      } else {
	ptable[i].all_slow = true;
      }
    }
  }

  level--;
  return all_slow;
}

static void *hemem_thread(void *arg)
{
  size_t last_time = 0;
  
  in_background = true;
  memsim_timebound_thread = true;

  while(hemem_thread_running) {
    ssize_t sleep_time = HEMEM_INTERVAL - (runtime - last_time);
    assert(sleep_time <= (ssize_t)HEMEM_INTERVAL);
    memsim_nanosleep(sleep_time < 0 ? 0 : sleep_time);
    last_time = runtime;
    memsim_timebound = runtime + HEMEM_INTERVAL / 2;

    pthread_mutex_lock(&global_lock);

    LOG("[HEMEM TICK]\n");
    
    // Track hot/cold memory
    for(enum pagetypes i = 0; i < NPAGETYPES; i++) {
      pages_swept[i] = 0;
    }
    for(int i = 0; i < 4; i++) {
      pages_skipped[i] = 0;
    }
#ifdef LOG_DEBUG
    size_t oldruntime = runtime;
#endif
    level = 0;
    sweep_hotset = 0;
    tot_sweeps++;
    sweep(pml4);
    LOG("SWEEP took %.3fs. swept %zu BASE, %zu HUGE, %zu GIGA. "
	"skipped %zu HUGE, %zu GIGA, %zu PML4. "
	"hotset_size = %.2f GB, sweep_hotset = %.2f GB.\n",
	(float)(runtime - oldruntime) / 1000000000.0,
	pages_swept[BASE], pages_swept[HUGE], pages_swept[GIGA],
	pages_skipped[2], pages_skipped[1], pages_skipped[0],
	hotset_size / (float)GB(1), sweep_hotset / (float)GB(1));

    // Can comment out for less overhead & accuracy
    tlb_shootdown(0);	// Sync active bit changes in TLB

    // Move cold memory down if under memory pressure
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
      add_hash(&mem_active[FASTMEM], p);
      hotset_size += page_size(p->type);
      fastmem_freebytes -= page_size(p->type);
      break;
    }
  }
  if(p == NULL) {
    // If out of fastmem, look for slowmem
    pt = GIGA;
    p = dequeue_fifo(&mem_free[SLOWMEM][pt]);
    // If NULL, we're totally out of mem
    assert(p != NULL);
    add_hash(&mem_inactive[SLOWMEM], p);
    slowmem_freebytes -= page_size(p->type);
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

#ifdef PERF_METHOD
static void perf_callback(uint64_t addr)
{
  perf_buckets[addr / GIGA_PAGE_SIZE]++;
}
#endif

void mmgr_init(void)
{
  cr3 = pml4;

  // Fastmem: all giga pages in the beginning
  for(int i = 0; i < FASTMEM_GIGA_PAGES; i++) {
    struct page *p = calloc(1, sizeof(struct page));
    p->type = GIGA;
    p->paddr = i * GIGA_PAGE_SIZE;
    enqueue_fifo(&mem_free[FASTMEM][GIGA], p);
  }
  // Slowmem: all giga pages
  for(int i = 0; i < SLOWMEM_GIGA_PAGES; i++) {
    struct page *p = calloc(1, sizeof(struct page));
    p->type = GIGA;
    p->paddr = (i * GIGA_PAGE_SIZE) | SLOWMEM_BIT;
    enqueue_fifo(&mem_free[SLOWMEM][GIGA], p);
  }
  
  int r = pthread_create(&hemem_threadid, NULL, hemem_thread, NULL);
  assert(r == 0);

#ifdef PERF_METHOD
  perf_register(&perf_callback, PERF_LIMIT);
#endif
}

static size_t last_sweeps = 0;

static void rec_stats(struct pte *ptable, uint64_t vaddr)
{
  recstats_level++;
  
  for(int i = 0; i < 512; i++) {
    if(!ptable[i].present) {
      assert(ptable[i].ups == 0 && ptable[i].downs == 0);
      continue;
    }

    assert(recstats_level >= 1 && recstats_level <= 4);
    /* if(ptable[i].ups > 1 || ptable[i].downs > 1) { */
    /*   LOG("vaddr = 0x%" PRIx64 ", level = %u, ups = %zu, downs = %zu\n", */
    /* 	  vaddr + i * ((uint64_t)1 << ((5 - recstats_level) * 9 + 3)), */
    /* 	  recstats_level, ptable[i].ups, ptable[i].downs); */
    /* } */
    
    if(!ptable[i].pagemap) {
      assert(ptable[i].next != NULL);
      rec_stats(ptable[i].next, vaddr + i * ((uint64_t)1 << ((5 - recstats_level) * 9 + 3)));
    } else {
      enum memtypes mt = ptable[i].addr & SLOWMEM_BIT ? SLOWMEM : FASTMEM;
      struct page *p = find_hash(&mem_active[mt], ptable[i].addr);
      if(p == NULL) {
	p = find_hash(&mem_inactive[mt], ptable[i].addr);
      }

      assert(p != NULL);
      LOG("vaddr = 0x%" PRIx64 ", level = %u, accesses = %zu, tot_accesses = %zu / %zu\n",
	  vaddr + i * ((uint64_t)1 << ((5 - recstats_level) * 9 + 3)),
	  recstats_level, p->accesses, p->tot_accesses, tot_sweeps - last_sweeps);
    }
  }

  recstats_level--;
}

int listnum(struct pte *pte)
{
  if(pte == (void *)1) {
    last_sweeps = tot_sweeps;
    return -1;
  }
  
  LOG("--- rec_stats ---\n");

  // Wait for hemem_thread to exit
  hemem_thread_running = false;
  add_runtime(HEMEM_INTERVAL);
  int r = pthread_join(hemem_threadid, NULL);
  assert(r == 0);

  recstats_level = 0;
  rec_stats(pml4, 0);

#ifdef PERF_METHOD
  for(size_t i = 0; i < SLOWMEM_SIZE / GIGA_PAGE_SIZE; i++) {
    LOG("vaddr 0x%zx: %zu\n", i * GIGA_PAGE_SIZE, perf_buckets[i]);
  }
#endif
  
  // Unused debug function
  return -1;
}
