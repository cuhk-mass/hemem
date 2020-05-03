/*
 * Simple memory allocator that only allocates fixed-size pages, allocates
 * physical memory linearly (first fast, then slow), and cannot free memory.
 */

#include <stdint.h>
#include <stdbool.h>
#include <assert.h>

#include "shared.h"

#define PAGE_TYPE	GIGA

// Top-level page table (we only emulate one process)
static struct pte pml4[512];
static uint64_t fastmem = 0, slowmem = 0;	// Clock hands

static uint64_t getmem(uint64_t addr, struct pte *pte)
{
  uint64_t ret;

  if(fastmem < FASTMEM_SIZE) {
    ret = fastmem;
    fastmem += page_size(PAGE_TYPE);
  } else {
    assert(slowmem < SLOWMEM_SIZE);
    ret = slowmem | SLOWMEM_BIT;
    slowmem += page_size(PAGE_TYPE);
  }

  assert((ret & page_mask(PAGE_TYPE)) == 0);	// Must be aligned
  return ret;
}

static struct pte *alloc_ptables(uint64_t addr, enum pagetypes pt)
{
  struct pte *ptable = pml4, *pte;

  // Allocate page tables down to the leaf
  for(int i = 1; i < pt + 2; i++) {
    pte = &ptable[(addr >> (48 - (i * 9))) & 511];

    if(!pte->present) {
      pte->present = true;
      pte->next = calloc(512, sizeof(struct pte));
    }

    ptable = pte->next;
  }

  // Return last-level PTE corresponding to addr
  return &ptable[(addr >> (48 - ((pt + 2) * 9))) & 511];
}

void pagefault(uint64_t addr, bool readonly)
{
  assert(!readonly);
  // Allocate page tables
  struct pte *pte = alloc_ptables(addr, PAGE_TYPE);
  pte->present = true;
  pte->pagemap = true;

  pte->addr = getmem(addr, pte);
}

void mmgr_init(void)
{
  cr3 = pml4;
}

int listnum(struct pte *pte)
{
  // Nothing to do
  return -1;
}
