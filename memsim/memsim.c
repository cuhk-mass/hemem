/*
 * memsim - memory system emulator
 *
 * Caveats:
 * L123 caches are not emulated.
 */

#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include <inttypes.h>

#include "shared.h"

// Top-level page table (we only emulate one process)
static struct pte pml4[512];
_Atomic size_t runtime = 0;		// Elapsed simulation time

// Hardware 2-level TLB emulating Cascade Lake
struct tlbe {
  uint64_t	vaddr, paddr;
  bool		present;
};

static struct tlbe l1tlb_1g[4], l1tlb_2m[32], l1tlb_4k[64];
static struct tlbe l2tlb_1g[16], l2tlb_2m4k[1536];

static size_t accesses[NMEMTYPES], tlbmisses = 0, pagefaults = 0;

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

static void pagefault(uint64_t addr)
{
  // Allocate page tables
  struct pte *pte = alloc_ptables(addr);
  pte->present = true;
  runtime += TIME_PAGEFAULT;
  pagefaults++;

  pte->addr = getmem(addr, pte);
}

#if 0
static unsigned int tlb_hash(uint64_t addr)
{
  return addr >> 12;
}
#endif

void tlb_shootdown(uint64_t addr)
{
  memset(l1tlb_1g, 0, sizeof(l1tlb_1g));
  memset(l1tlb_2m, 0, sizeof(l1tlb_2m));
  memset(l1tlb_4k, 0, sizeof(l1tlb_4k));
  memset(l2tlb_1g, 0, sizeof(l2tlb_1g));
  memset(l2tlb_2m4k, 0, sizeof(l2tlb_2m4k));

  runtime += TIME_TLBSHOOTDOWN;
}

static void memaccess(uint64_t addr, enum access_type type)
{
  // Must be canonical addr
  assert((addr >> 48) == 0);

  // TODO: In TLB?
  tlbmisses++;

  // 4-level page walk
  struct pte *ptable = pml4, *pte = NULL;

  for(int i = 1; i <= 4 && ptable != NULL; i++) {
    pte = &ptable[(addr >> (48 - (i * 9))) & 511];

    if(!pte->present) {
      pagefault(addr);
      assert(pte->present);
    }

    pte->accessed = true;
    if(type == TYPE_WRITE) {
      pte->modified = true;
    }
    ptable = pte->next;
    runtime += TIME_PAGEWALK;
  }

  assert(pte != NULL);
  if(type == TYPE_READ) {
    runtime += (pte->addr & SLOWMEM_BIT) ? TIME_SLOWMEM_READ : TIME_FASTMEM_READ;
  } else {
    runtime += (pte->addr & SLOWMEM_BIT) ? TIME_SLOWMEM_WRITE : TIME_FASTMEM_WRITE;
  }

  accesses[(pte->addr & SLOWMEM_BIT) ? SLOWMEM : FASTMEM]++;

  if((pte->addr & SLOWMEM_BIT) && type == TYPE_READ) {
    LOG("%zu memaccess %s %" PRIu64 " %s %" PRIu64 " %d\n",
	runtime,
	type == TYPE_READ ? "read" : "write",
	addr,
	(pte->addr & SLOWMEM_BIT) ? "slow" : "fast",
	(pte->addr & SLOWMEM_MASK) / BASE_PAGE_SIZE,
	listnum(pte));
  }
}

#define WORKSET_SIZE	MB(10)

#define RAND_WITHIN(x)	(((double)rand() / RAND_MAX) * (x))

static void gups(size_t iters, uint64_t hotset_start, uint64_t hotset_size,
		 double hotset_prob)
{
  // GUPS with hotset
  for(size_t i = 0; i < iters; i++) {
    uint64_t a;

    if(RAND_WITHIN(1) < hotset_prob) {
      // Hot set
      a = hotset_start + (uint64_t)RAND_WITHIN(hotset_size);
    } else {
      // Entire working set
      a = (uint64_t)RAND_WITHIN(WORKSET_SIZE);
    }

    // Read&update
    memaccess(a, TYPE_READ);
    memaccess(a, TYPE_WRITE);

    runtime += 100;	// 100ns program time per update
  }
}

static void reset_stats(void)
{
  LOG("%zu --- reset_stats ---\n", runtime);

  runtime = 0;
  accesses[FASTMEM] = accesses[SLOWMEM] = 0;
  tlbmisses = 0;
  pagefaults = 0;
}

int main(int argc, char *argv[])
{
  getmem_init();

  // Get memory traces from Onur's group at ETH? membench? Replay them here?

  // GUPS!
  gups(10000000, 0, KB(512), 0.9);

  printf("%s\t%.2f\t%zu\t%zu\t%zu\t%zu\n", argv[0],
	 (double)runtime / 1000000.0, accesses[FASTMEM], accesses[SLOWMEM],
	 tlbmisses, pagefaults);

  reset_stats();

  // Move hotset up
  gups(10000000, MB(9), KB(512), 1);

  printf("%s\t%.2f\t%zu\t%zu\t%zu\t%zu\n", argv[0],
	 (double)runtime / 1000000.0, accesses[FASTMEM], accesses[SLOWMEM],
	 tlbmisses, pagefaults);

  return 0;
}
