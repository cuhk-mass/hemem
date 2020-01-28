#include <stdint.h>
#include <stdbool.h>
#include <assert.h>

#include "shared.h"

// Top-level page table (we only emulate one process)
static struct pte pml4[512];
static uint64_t fastmem = 0, slowmem = 0;	// Clock hands

static uint64_t getmem(uint64_t addr, struct pte *pte)
{
  uint64_t ret;

  if(fastmem < FASTMEM_SIZE) {
    ret = fastmem;
    fastmem += BASE_PAGE_SIZE;
  } else {
    assert(slowmem < SLOWMEM_SIZE);
    ret = slowmem | SLOWMEM_BIT;
    slowmem += BASE_PAGE_SIZE;
  }

  return ret;
}

static struct pte *alloc_ptables(uint64_t addr)
{
  struct pte *ptable = pml4, *pte;

  // Allocate page tables down to the leaf
  for(int i = 1; i < 4; i++) {
    pte = &ptable[(addr >> (48 - (i * 9))) & 511];

    if(!pte->present) {
      pte->present = true;
      pte->next = calloc(512, sizeof(struct pte));
    }

    ptable = pte->next;
  }

  // Return last-level PTE corresponding to addr
  return &ptable[(addr >> (48 - (4 * 9))) & 511];
}

void pagefault(uint64_t addr)
{
  // Allocate page tables
  struct pte *pte = alloc_ptables(addr);
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
