#ifndef SHARED_H
#define SHARED_H

#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>

// Handy size macros
#define KB(x)		(((uint64_t)x) * 1024)
#define MB(x)		(KB(x) * 1024)
#define GB(x)		(MB(x) * 1024)

// Page sizes
#define BASE_PAGE_SIZE	KB(4)
#define HUGE_PAGE_SIZE	MB(2)
#define GIGA_PAGE_SIZE	GB(1)

// Page offset masks
#define BASE_PAGE_MASK	(BASE_PAGE_SIZE - 1)
#define HUGE_PAGE_MASK	(HUGE_PAGE_SIZE - 1)
#define GIGA_PAGE_MASK	(GIGA_PAGE_SIZE - 1)

#define BASE_PFN_MASK	(BASE_PAGE_MASK ^ UINT64_MAX)
#define HUGE_PFN_MASK	(HUGE_PAGE_MASK ^ UINT64_MAX)
#define GIGA_PFN_MASK	(GIGA_PAGE_MASK ^ UINT64_MAX)

// Physical memory sizes in bytes
#define FASTMEM_SIZE	MB(1)
#define SLOWMEM_SIZE	MB(10)
#define CACHELINE_SIZE	64

// Simulated execution times in ns
#define TIME_PAGEFAULT		2000		// pagefault interrupt
#define TIME_PAGEWALK		200		// 1 level of pagewalk
#define TIME_SLOWMOVE		2000		// Move to slow memory
#define TIME_FASTMOVE		1000		// Move to fast memory
#define TIME_TLBSHOOTDOWN	4000		// TLB shootdown

#define TIME_FASTMEM_READ	82
#define TIME_FASTMEM_WRITE	82
#define TIME_SLOWMEM_READ	1000
#define TIME_SLOWMEM_WRITE	1000

// Fake offset for slowmem in physical memory
#define SLOWMEM_BIT	((uint64_t)1 << 63)
#define SLOWMEM_MASK	(((uint64_t)1 << 63) - 1)

// Memory access type
enum access_type {
  TYPE_READ,
  TYPE_WRITE,
};

enum memtypes {
  FASTMEM = 0,
  SLOWMEM = 1,
  NMEMTYPES,
};

// Page table entry
struct pte {
  uint64_t addr;		// Physical address
  struct pte *next;		// Next page table pointer

  // Hardware bits
  bool present;
  bool accessed;
  bool modified;
  bool pagemap;
};

void pagefault(uint64_t addr);
void tlb_shootdown(uint64_t addr);
void mmgr_init(void);

// XXX: Debug
int listnum(struct pte *pte);

extern _Atomic size_t runtime;
extern struct pte *cr3;

#ifdef LOG_DEBUG
#	define LOG(str, ...)	fprintf(stderr, str, __VA_ARGS__)
#else
#	define LOG(std, ...)	while(0) {}
#endif

#endif
