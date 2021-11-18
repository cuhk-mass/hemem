#ifndef HEMEM_PAGING_H
#define HEMEM_PAGING_H

#include <pthread.h>
#include <stdint.h>
#include <inttypes.h>

#include "../hemem.h"


#define ADDRESS_MASK  ((uint64_t)0x00000ffffffff000UL)
#define FLAGS_MASK  ((uint64_t)0x0000000000000fffUL)

#define HEMEM_PRESENT_FLAG  ((uint64_t)0x0000000000000001UL)
#define HEMEM_WRITE_FLAG  ((uint64_t)0x0000000000000002UL)
#define HEMEM_USER_FLAG   ((uint64_t)0x0000000000000004UL)
#define HEMEM_PWT_FLAG    ((uint64_t)0x0000000000000008UL)
#define HEMEM_PCD_FLAG    ((uint64_t)0x0000000000000010UL)
#define HEMEM_ACCESSED_FLAG ((uint64_t)0x0000000000000020UL)
#define HEMEM_DIRTY_FLAG  ((uint64_t)0x0000000000000040UL)
#define HEMEM_HUGEPAGE_FLAG ((uint64_t)0x0000000000000080UL)


#define HEMEM_PAGE_WALK_FLAGS (HEMEM_PRESENT_FLAG |   \
               HEMEM_WRITE_FLAG | \
         HEMEM_USER_FLAG |  \
         HEMEM_ACCESSED_FLAG |  \
         HEMEM_DIRTY_FLAG)

#define HEMEM_PWTPCD_FLAGS  (HEMEM_PWT_FLAG | HEMEM_PCD_FLAG)

#define HEMEM_PGDIR_SHIFT 39
#define HEMEM_PTRS_PER_PGD  512
#define HEMEM_PUD_SHIFT   30
#define HEMEM_PTRS_PER_PUD  512
#define HEMEM_PMD_SHIFT   21
#define HEMEM_PTRS_PER_PMD  512
#define HEMEM_PAGE_SHIFT  12
#define HEMEM_PTRS_PER_PTE  512

//#define EXAMINE_PGTABLES


void scan_pagetable();
void _scan_pagetable(bool clear_flag, uint64_t flag);

//void clear_accessed_bit(uint64_t pa);
//uint64_t get_accessed_bit(uint64_t pa);
//void clear_dirty_bit(uint64_t pa);
//uint64_t get_dirty_bit(uint64_t pa);
//
//uint64_t* va_to_pa(uint64_t va);

#ifdef EXAMINE_PGTABLES

struct pagemapEntry {
  uint64_t pfn : 54;
  unsigned int soft_dirty : 1;
  unsigned int exclusive : 1;
  unsigned int file_page : 1;
  unsigned int swapped : 1;
  unsigned int present : 1;
};

void *examine_pagetables();

#endif /*EXAMINE_PGTABLES*/

#endif /* HEMEM_PAGING_H */

