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

enum pagetypes {
  GIGA = 0, HUGE, BASE, NPAGETYPES
};

#define FASTMEM_GIGA_PAGES     	(FASTMEM_SIZE / GIGA_PAGE_SIZE)
#define FASTMEM_HUGE_PAGES     	(FASTMEM_SIZE / HUGE_PAGE_SIZE)
#define FASTMEM_BASE_PAGES     	(FASTMEM_SIZE / BASE_PAGE_SIZE)

#define SLOWMEM_GIGA_PAGES	(SLOWMEM_SIZE / GIGA_PAGE_SIZE)
#define SLOWMEM_HUGE_PAGES	(SLOWMEM_SIZE / HUGE_PAGE_SIZE)
#define SLOWMEM_BASE_PAGES	(SLOWMEM_SIZE / BASE_PAGE_SIZE)

struct page {
  struct page	*next, *prev;
  uint64_t	paddr;
  struct pte	*pte;
};

struct fifo_queue {
  struct page	*first, *last;
  size_t	numentries;
};

static struct pte pml4[512]; // Top-level page table (we only emulate one process)
static struct fifo_queue fastmem_free[NPAGETYPES], slowmem_free[NPAGETYPES];
  /* fastmem_active[NPAGETYPES], slowmem_active[NPAGETYPES]; */
static pthread_mutex_t global_lock = PTHREAD_MUTEX_INITIALIZER;
static bool __thread in_background = false;

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

static void *background_thread(void *arg)
{
  size_t oldruntime = 0;

  in_background = true;

  for(;;) {
    while(runtime - oldruntime < KSWAPD_INTERVAL);

    oldruntime = runtime;
  }

  return NULL;
}

static struct pte *alloc_ptables(uint64_t addr, enum pagetypes ptype)
{
  struct pte *ptable = pml4, *pte;
  int level = ptype + 2;

  assert(level >= 2 && level <= 4);
  
  // Allocate page tables down to the leaf
  for(int i = 1; i < level; i++) {
    pte = &ptable[(addr >> (48 - (i * 9))) & 511];

    if(!pte->present) {
      pte->present = true;
      pte->next = calloc(512, sizeof(struct pte));
    }

    ptable = pte->next;
  }

  // Return last-level PTE corresponding to addr
  pte = &ptable[(addr >> (48 - (level * 9))) & 511];
  pte->present = true;
  pte->pagemap = true;

  return pte;
}

static struct page *getmem(uint64_t addr)
{
  pthread_mutex_lock(&global_lock);

  // Allocate from fastmem
  struct page *p = dequeue_fifo(&fastmem_free[GIGA]);
  assert(p != NULL);

  p->pte = alloc_ptables(addr, GIGA);
  assert(p->pte != NULL);
  p->pte->addr = p->paddr;

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

  // All giga pages in the beginning
  for(int i = 0; i < FASTMEM_GIGA_PAGES; i++) {
    struct page *p = calloc(1, sizeof(struct page));
    p->paddr = i * GIGA_PAGE_SIZE;
    enqueue_fifo(&fastmem_free[GIGA], p);
  }
  for(int i = 0; i < SLOWMEM_GIGA_PAGES; i++) {
    struct page *p = calloc(1, sizeof(struct page));
    p->paddr = (i * GIGA_PAGE_SIZE) | SLOWMEM_BIT;
    enqueue_fifo(&slowmem_free[GIGA], p);
  }
  
  pthread_t thread;
  int r = pthread_create(&thread, NULL, background_thread, NULL);
  assert(r == 0);
}
