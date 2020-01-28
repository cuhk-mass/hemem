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

struct pte	*cr3 = NULL;
_Atomic size_t	runtime = 0;		// Elapsed simulation time

// Hardware 2-level TLB emulating Cascade Lake
struct tlbe {
  uint64_t	vaddr, paddr;
  bool		present;
};

static struct tlbe l1tlb_1g[4], l1tlb_2m[32], l1tlb_4k[64];
static struct tlbe l2tlb_1g[16], l2tlb_2m4k[1536];

static size_t accesses[NMEMTYPES], tlbmisses = 0, tlbhits = 0, pagefaults = 0;

static unsigned int tlb_hash(uint64_t addr)
{
  return addr >> 12;
}

void tlb_shootdown(uint64_t addr)
{
  memset(l1tlb_1g, 0, sizeof(l1tlb_1g));
  memset(l1tlb_2m, 0, sizeof(l1tlb_2m));
  memset(l1tlb_4k, 0, sizeof(l1tlb_4k));
  memset(l2tlb_1g, 0, sizeof(l2tlb_1g));
  memset(l2tlb_2m4k, 0, sizeof(l2tlb_2m4k));

  runtime += TIME_TLBSHOOTDOWN;
}

static struct tlbe *tlb_lookup(struct tlbe *tlb, unsigned int size, uint64_t vaddr)
{
  struct tlbe *te = &tlb[tlb_hash(vaddr) % size];
  if(te->present && te->vaddr == vaddr) {
    return te;
  } else {
    return NULL;
  }
}

static void tlb_insert(uint64_t vaddr, uint64_t paddr, unsigned int level)
{
  struct tlbe *te;

  assert(level >= 2 && level <= 4);
  
  switch(level) {
  case 2:	// 1GB page
    te = &l1tlb_1g[tlb_hash(vaddr) % 4];
    if(te->present) {
      // Move previous entry down
      assert(te->vaddr != vaddr);
      memcpy(&l2tlb_1g[tlb_hash(vaddr) % 16], te, sizeof(struct tlbe));
    }
    break;

  case 3:	// 2MB page
    te = &l1tlb_2m[tlb_hash(vaddr) % 32];

    // Fall through...
  case 4:	// 4KB page
    if(level == 4) {
      te = &l1tlb_4k[tlb_hash(vaddr) % 64];
    }
    if(te->present) {
      // Move previous entry down
      assert(te->vaddr != vaddr);
      memcpy(&l2tlb_2m4k[tlb_hash(vaddr) % 1536], te, sizeof(struct tlbe));
    }
    break;
  }

  te->present = true;
  te->vaddr = vaddr;
  te->paddr = paddr;
}

static void memaccess(uint64_t addr, enum access_type type)
{
  // Must be canonical addr
  assert((addr >> 48) == 0);

  // In TLB?
  struct tlbe *te = NULL;
  uint64_t paddr;
  if((te = tlb_lookup(l1tlb_1g, 4, addr)) == NULL &&
     (te = tlb_lookup(l1tlb_2m, 32, addr)) == NULL &&
     (te = tlb_lookup(l1tlb_4k, 64, addr)) == NULL &&
     (te = tlb_lookup(l2tlb_1g, 16, addr)) == NULL &&
     (te = tlb_lookup(l2tlb_2m4k, 1536, addr)) == NULL) {
    tlbmisses++;

    // 4-level page walk
    assert(cr3 != NULL);
    struct pte *ptable = cr3, *pte = NULL;
    int level;

    for(level = 1; level <= 4 && ptable != NULL; level++) {
      pte = &ptable[(addr >> (48 - (level * 9))) & 511];

      runtime += TIME_PAGEWALK;

      if(!pte->present) {
	pagefault(addr);
	runtime += TIME_PAGEFAULT;
	pagefaults++;
	assert(pte->present);
      }

      pte->accessed = true;
      if(type == TYPE_WRITE) {
	pte->modified = true;
      }

      if(pte->pagemap) {
	// Page here -- terminate walk
	break;
      }
      
      ptable = pte->next;
    }

    assert(pte != NULL);
    paddr = pte->addr;

    // Insert in TLB
    tlb_insert(addr, paddr, level);
  } else {
    tlbhits++;
    paddr = te->paddr;
  }

  if(type == TYPE_READ) {
    runtime += (paddr & SLOWMEM_BIT) ? TIME_SLOWMEM_READ : TIME_FASTMEM_READ;
  } else {
    runtime += (paddr & SLOWMEM_BIT) ? TIME_SLOWMEM_WRITE : TIME_FASTMEM_WRITE;
  }

  accesses[(paddr & SLOWMEM_BIT) ? SLOWMEM : FASTMEM]++;

  /* if((pte->addr & SLOWMEM_BIT) && type == TYPE_READ) { */
  /*   LOG("%zu memaccess %s %" PRIu64 " %s %" PRIu64 " %d\n", */
  /* 	runtime, */
  /* 	type == TYPE_READ ? "read" : "write", */
  /* 	addr, */
  /* 	(pte->addr & SLOWMEM_BIT) ? "slow" : "fast", */
  /* 	(pte->addr & SLOWMEM_MASK) / BASE_PAGE_SIZE, */
  /* 	listnum(pte)); */
  /* } */
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
  tlbmisses = tlbhits = 0;
  pagefaults = 0;
}

int main(int argc, char *argv[])
{
  mmgr_init();

  // Get memory traces from Onur's group at ETH? membench? Replay them here?

  // GUPS!
  gups(10000000, 0, KB(512), 0.9);

  printf("%s\t%.2f\t%zu\t%zu\t%zu\t%zu\t%zu\n", argv[0],
	 (double)runtime / 1000000.0, accesses[FASTMEM], accesses[SLOWMEM],
	 tlbmisses, tlbhits, pagefaults);

  reset_stats();

  // Move hotset up
  gups(10000000, MB(9), KB(512), 0.9);

  printf("%s\t%.2f\t%zu\t%zu\t%zu\t%zu\t%zu\n", argv[0],
	 (double)runtime / 1000000.0, accesses[FASTMEM], accesses[SLOWMEM],
	 tlbmisses, tlbhits, pagefaults);

  return 0;
}
