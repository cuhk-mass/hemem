#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>
#include <assert.h>
#include <inttypes.h>
#include <string.h>
#include <time.h>

#include "shared.h"

#define MAX_SHIFT	30

static struct pte pml4[512]; // Top-level page table (we only emulate one process)
static int level;
static size_t pages_swept[NPAGETYPES];
static size_t allocsize = 0;

static void sweep(struct pte *ptable)
{
  level++;
  
  for(int i = 0; i < 512; i++) {
    if(!ptable[i].present) {
      continue;
    }
    
    if(ptable[i].pagemap) {
      assert(level >= 2 && level <= 4);
      pages_swept[level - 2]++;

      if(ptable[i].accessed) {
	ptable[i].accessed = false;
      }
    } else {
      assert(ptable[i].next != NULL);

      // Reset accessed bit at this level
      ptable[i].accessed = false;
      
      sweep(ptable[i].next);
    }
  }

  level--;
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
	allocsize += 512 * sizeof(struct pte);
	ptable = newtree;
	continue;
      } else {
	assert(!pte->pagemap);
	pte->present = true;
	pte->next = calloc(512, sizeof(struct pte));
	allocsize += 512 * sizeof(struct pte);
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

int main(int argc, char *argv[])
{
  if(argc < 3) {
    printf("Usage: %s MEMSIZE PAGETYPE\n", argv[0]);
    exit(0);
  }
  size_t memsize = atoll(argv[1]), amemsize = memsize;
  enum pagetypes pt = atoi(argv[2]);

  size_t iters = 1;
  if(pt == BASE) {
    iters = memsize >> MAX_SHIFT;
    amemsize = (memsize > (1ULL << MAX_SHIFT)) ? 1ULL << MAX_SHIFT : memsize;
    if(iters == 0) { iters = 1; }
  }

  fprintf(stderr, "Allocating... iters = %zu, amemsize = %zu\n", iters, amemsize);
  for(size_t i = 0; i < amemsize; i += page_size(pt)) {
    alloc_ptables(i, pt, 0);
  }

  fprintf(stderr, "Measuring...\n");
  clock_t begin = clock();
  for(size_t i = 0; i < iters; i++) {
    level = 0;
    sweep(pml4);
  }
  clock_t end = clock();

  printf("%zu %u %.2f ", memsize, pt, ((double)end - begin) / (CLOCKS_PER_SEC / 1000000.0));

  for(int i = 0; i < NPAGETYPES; i++) {
    printf("%zu ", pages_swept[i]);
  }
  printf("%zu %zu\n", allocsize, iters);
  
  return 0;
}
