#ifndef HEMEM_PAGING_H
#define HEMEM_PAGING_H

#include <pthread.h>
#include <stdint.h>
#include <inttypes.h>

#include "hemem.h"

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

#endif /* HEMEM_PAGING_H */
