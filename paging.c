#include <stdlib.h>
#include <stdio.h>
#include <assert.h>
#include <unistd.h>
#include <sys/time.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <math.h>
#include <string.h>
#include <pthread.h>
#include <sys/mman.h>
#include <linux/userfaultfd.h>
#include <poll.h>
#include <sys/syscall.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <stdint.h>
#include <inttypes.h>

#include "hemem.h"
#include "timer.h"
#include "paging.h"
#include "interpose.h"

uint64_t va_to_pa(uint64_t va)
{
  uint64_t pt_base = ((uint64_t)(cr3 & ADDRESS_MASK));
  uint64_t *pgd;
  uint64_t *pud;
  uint64_t *pmd;
  uint64_t *pte;
  uint64_t pgd_entry;
  uint64_t pud_entry;
  uint64_t pmd_entry;
  uint64_t pte_entry;
  uint64_t offset;

  pgd = (uint64_t*)libc_mmap(NULL, BASEPAGE_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, devmemfd, pt_base);
  if (pgd == MAP_FAILED) {
    perror("hemem_va_to_pa pgd mmap:");
    ignore_this_mmap = true;
    assert(0);
    ignore_this_mmap = false;
  }
  offset = (((va) >> HEMEM_PGDIR_SHIFT) & (HEMEM_PTRS_PER_PGD - 1));
  ignore_this_mmap = true;
  assert(offset < BASEPAGE_SIZE);
  ignore_this_mmap = false;
  pgd_entry = *(pgd + offset) ;
  if (!((pgd_entry & HEMEM_PRESENT_FLAG) == HEMEM_PRESENT_FLAG)) {
    LOG("hemem_va_to_pa: pgd not present: %016lx\n", pgd_entry);
    ignore_this_mmap = true;
    assert(0);
    ignore_this_mmap = false;
  }

  pud = (uint64_t*)libc_mmap(NULL, BASEPAGE_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, devmemfd, pgd_entry & ADDRESS_MASK);
  if (pud == MAP_FAILED) {
    perror("hemem_va_to_pa pud mmap:");
    ignore_this_mmap = true;
    assert(0);
    ignore_this_mmap = false;
  }
  offset =  (((va) >> HEMEM_PUD_SHIFT) & (HEMEM_PTRS_PER_PUD - 1));
  ignore_this_mmap = true;
  assert(offset < BASEPAGE_SIZE);
  ignore_this_mmap = false;
  pud_entry = *(pud + offset);
  if (!((pud_entry & HEMEM_PRESENT_FLAG) == HEMEM_PRESENT_FLAG)) {
    LOG("hemem_va_to_pa: pud not present: %016lx\n", pud_entry);
    ignore_this_mmap = true;
    assert(0);
    ignore_this_mmap = false;
  }

  pmd = (uint64_t*)libc_mmap(NULL, BASEPAGE_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, devmemfd, pud_entry & ADDRESS_MASK);
  if (pmd == MAP_FAILED) {
    perror("hemem_va_to_pa pmd mmap:");
    ignore_this_mmap = true;
    assert(0);
    ignore_this_mmap = false;
  }
  offset = (((va) >> HEMEM_PMD_SHIFT) & (HEMEM_PTRS_PER_PMD - 1));
  ignore_this_mmap = true;
  assert(offset < BASEPAGE_SIZE);
  ignore_this_mmap = false;
  pmd_entry = *(pmd + offset);
  if (!((pmd_entry & HEMEM_PRESENT_FLAG) == HEMEM_PRESENT_FLAG)) {
    LOG("hemem_va_to_pa: pmd not present: %016lx\n", pmd_entry);
    ignore_this_mmap = true;
    assert(0);
    ignore_this_mmap = false;
  }

  if ((pmd_entry & HEMEM_HUGEPAGE_FLAG) == HEMEM_HUGEPAGE_FLAG) {
    munmap(pmd, BASEPAGE_SIZE);
    munmap(pud, BASEPAGE_SIZE);
    munmap(pgd, BASEPAGE_SIZE);
    return pmd_entry;
  }

  pte = (uint64_t*)libc_mmap(NULL, BASEPAGE_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, devmemfd, pmd_entry & ADDRESS_MASK);
  if (pte == MAP_FAILED) {
    perror("hemem_va_to_pa pte mmap:");
    ignore_this_mmap = true;
    assert(0);
    ignore_this_mmap = false;
  }
  offset = (((va) >> HEMEM_PAGE_SHIFT) & (HEMEM_PTRS_PER_PTE - 1));
  ignore_this_mmap = true;
  assert(offset < BASEPAGE_SIZE);
  ignore_this_mmap = false;
  pte_entry = *(pte + offset);
  if (!((pte_entry & HEMEM_PRESENT_FLAG) == HEMEM_PRESENT_FLAG)) {
    LOG("hemem_va_to_pa: pte not present: %016lx\n", pte_entry);
    ignore_this_mmap = true;
    assert(0);
    ignore_this_mmap = false;
  }

  munmap(pte, BASEPAGE_SIZE);
  munmap(pmd, BASEPAGE_SIZE);
  munmap(pud, BASEPAGE_SIZE);
  munmap(pgd, BASEPAGE_SIZE);
  return pte_entry;
}

void clear_bit(uint64_t va, uint64_t bit)
{
  uint64_t pt_base = ((uint64_t)(cr3 & ADDRESS_MASK));
  uint64_t *pgd;
  uint64_t *pud;
  uint64_t *pmd;
  uint64_t *pte;
  uint64_t *pgd_entry;
  uint64_t *pud_entry;
  uint64_t *pmd_entry;
  uint64_t *pte_entry;
  uint64_t offset;

  pgd = (uint64_t*)libc_mmap(NULL, BASEPAGE_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, devmemfd, pt_base);
  if (pgd == MAP_FAILED) {
    perror("clear_accessed_bit: pgd mmap:");
    ignore_this_mmap = true;
    assert(0);
    ignore_this_mmap = false;
  }
  offset = (((va) >> HEMEM_PGDIR_SHIFT) & (HEMEM_PTRS_PER_PGD - 1));
  ignore_this_mmap = true;
  assert(offset < BASEPAGE_SIZE);
  ignore_this_mmap = false;
  pgd_entry = (pgd + offset);
  if (!((*pgd_entry & HEMEM_PRESENT_FLAG) == HEMEM_PRESENT_FLAG)) {
    LOG("clear_accessed_bit: pgd not present: %016lx\n", *pgd_entry);
    //assert(0);
    return;
  }
  
  pud = (uint64_t*)libc_mmap(NULL, BASEPAGE_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, devmemfd, *pgd_entry & ADDRESS_MASK);
  if (pud == MAP_FAILED) {
    perror("clear_accessed_bit: pud mmap:");
    ignore_this_mmap = true;
    assert(0);
    ignore_this_mmap = false;
  }
  offset = (((va) >> HEMEM_PUD_SHIFT) & (HEMEM_PTRS_PER_PUD - 1));
  ignore_this_mmap = true;
  assert(offset < BASEPAGE_SIZE);
  ignore_this_mmap = false;
  pud_entry = (pud + offset);
  if (!((*pud_entry & HEMEM_PRESENT_FLAG) == HEMEM_PRESENT_FLAG)) {
    LOG("clear_accessed_bit: pud not present: %016lx\n", *pud_entry);
    //assert(0);
    return;
  }

  pmd = (uint64_t*)libc_mmap(NULL, BASEPAGE_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, devmemfd, *pud_entry & ADDRESS_MASK);
  if (pmd == MAP_FAILED) {
    perror("clear_accessed_bit: pmd mmap:");
    ignore_this_mmap = true;
    assert(0);
    ignore_this_mmap = false;
  }
  offset = (((va) >> HEMEM_PMD_SHIFT) & (HEMEM_PTRS_PER_PMD - 1));
  ignore_this_mmap = true;
  assert(offset < BASEPAGE_SIZE);
  ignore_this_mmap = false;
  pmd_entry = (pmd + offset);
  if (!((*pmd_entry & HEMEM_PRESENT_FLAG) == HEMEM_PRESENT_FLAG)) {
    LOG("clear_accessed_bit: pmd not present: %016lx\n", *pmd_entry);
    //assert(0);
    return;
  }

  if ((*pmd_entry & HEMEM_HUGEPAGE_FLAG) == HEMEM_HUGEPAGE_FLAG) {
    *pmd_entry = *pmd_entry & ~bit;
    munmap(pmd, BASEPAGE_SIZE);
    munmap(pud, BASEPAGE_SIZE);
    munmap(pgd, BASEPAGE_SIZE);
    return;
  }

  pte = (uint64_t*)libc_mmap(NULL, BASEPAGE_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, devmemfd, *pmd_entry & ADDRESS_MASK);
  if (pte == MAP_FAILED) {
    perror("clear_accessed_bit pte mmap:");
    ignore_this_mmap = true;
    assert(0);
    ignore_this_mmap = false;
  }
  offset = (((va) >> HEMEM_PAGE_SHIFT) & (HEMEM_PTRS_PER_PTE - 1));
  ignore_this_mmap = true;
  assert(offset < BASEPAGE_SIZE);
  ignore_this_mmap = false;
  pte_entry = (pte + offset);
  if (!((*pte_entry & HEMEM_PRESENT_FLAG) == HEMEM_PRESENT_FLAG)) {
    LOG("clear_accessed_bit: pte not present: %016lx\n", *pte_entry);
    //assert(0);
    return;
  }
  
  *pte_entry = *pte_entry & ~bit;

  munmap(pte, BASEPAGE_SIZE);
  munmap(pmd, BASEPAGE_SIZE);
  munmap(pud, BASEPAGE_SIZE);
  munmap(pgd, BASEPAGE_SIZE);
}

uint64_t get_bit(uint64_t va, uint64_t bit)
{
  uint64_t pt_base = ((uint64_t)(cr3 & ADDRESS_MASK));
  uint64_t *pgd;
  uint64_t *pud;
  uint64_t *pmd;
  uint64_t *pte;
  uint64_t *pgd_entry;
  uint64_t *pud_entry;
  uint64_t *pmd_entry;
  uint64_t *pte_entry;
  uint64_t offset;

  pgd = (uint64_t*)libc_mmap(NULL, BASEPAGE_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, devmemfd, pt_base);
  if (pgd == MAP_FAILED) {
    perror("get_accessed_bit: pgd mmap:");
    ignore_this_mmap = true;
    assert(0);
    ignore_this_mmap = false;
  }
  offset = (((va) >> HEMEM_PGDIR_SHIFT) & (HEMEM_PTRS_PER_PGD - 1));
  ignore_this_mmap = true;
  assert(offset < BASEPAGE_SIZE);
  ignore_this_mmap = false;
  pgd_entry = (pgd + offset);
  if (!((*pgd_entry & HEMEM_PRESENT_FLAG) == HEMEM_PRESENT_FLAG)) {
    LOG("get_accessed_bit: pgd not present: %016lx\n", *pgd_entry);
    //assert(0);
    return 0;
  }

  pud = (uint64_t*)libc_mmap(NULL, BASEPAGE_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, devmemfd, *pgd_entry & ADDRESS_MASK);
  if (pud == MAP_FAILED) {
    perror("get_accessed_bit: pud mmap:");
    ignore_this_mmap = true;
    assert(0);
    ignore_this_mmap = false;
  }
  offset = (((va) >> HEMEM_PUD_SHIFT) & (HEMEM_PTRS_PER_PUD - 1));
  ignore_this_mmap = true;
  assert(offset < BASEPAGE_SIZE);
  ignore_this_mmap = false;
  pud_entry = (pud + offset);
  if (!((*pud_entry & HEMEM_PRESENT_FLAG) == HEMEM_PRESENT_FLAG)) {
    LOG("get_accessed_bit: pud not present: %016lx\n", *pud_entry);
    //assert(0);
    return 0;
  }

  pmd = (uint64_t*)libc_mmap(NULL, BASEPAGE_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, devmemfd, *pud_entry & ADDRESS_MASK);
  if (pmd == MAP_FAILED) {
    perror("get_accessed_bit: pmd mmap:");
    ignore_this_mmap = true;
    assert(0);
    ignore_this_mmap = false;
  }
  offset = (((va) >> HEMEM_PMD_SHIFT) & (HEMEM_PTRS_PER_PMD - 1));
  ignore_this_mmap = true;
  assert(offset < BASEPAGE_SIZE);
  ignore_this_mmap = false;
  pmd_entry = (pmd + offset);
  if (!((*pmd_entry & HEMEM_PRESENT_FLAG) == HEMEM_PRESENT_FLAG)) {
    LOG("get_accessed_bit: pmd not present: %016lx\n", *pmd_entry);
    //assert(0);
    return 0;
  }

  if ((*pmd_entry & HEMEM_HUGEPAGE_FLAG) == HEMEM_HUGEPAGE_FLAG) {
    int ret = *pmd_entry & bit;
    munmap(pmd, BASEPAGE_SIZE);
    munmap(pud, BASEPAGE_SIZE);
    munmap(pgd, BASEPAGE_SIZE);
    return ret;
  }

  pte = (uint64_t*)libc_mmap(NULL, BASEPAGE_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, devmemfd, *pmd_entry & ADDRESS_MASK);
  if (pte == MAP_FAILED) {
    perror("get_accessed_bit pte mmap:");
    ignore_this_mmap = true;
    assert(0);
    ignore_this_mmap = false;
  }
  offset = (((va) >> HEMEM_PAGE_SHIFT) & (HEMEM_PTRS_PER_PTE - 1));
  ignore_this_mmap = true;
  assert(offset < BASEPAGE_SIZE);
  ignore_this_mmap = false;
  pte_entry = (pte + offset);
  if (!((*pte_entry & HEMEM_PRESENT_FLAG) == HEMEM_PRESENT_FLAG)) {
    LOG("get_accessed_bit: pte not present: %016lx\n", *pte_entry);
    //assert(0);
    return 0;
  }
  
  int ret = *pte_entry & bit;

  munmap(pte, BASEPAGE_SIZE);
  munmap(pmd, BASEPAGE_SIZE);
  munmap(pud, BASEPAGE_SIZE);
  munmap(pgd, BASEPAGE_SIZE);

  return ret;

}


void clear_accessed_bit(uint64_t va)
{
  clear_bit(va, HEMEM_ACCESSED_FLAG);
}


uint64_t get_accessed_bit(uint64_t va)
{
  return get_bit(va, HEMEM_ACCESSED_FLAG);
}


void clear_dirty_bit(uint64_t va)
{
  clear_bit(va, HEMEM_DIRTY_FLAG);
}


uint64_t get_dirty_bit(uint64_t va)
{
  return get_bit(va, HEMEM_DIRTY_FLAG);
}


FILE *ptes, *pdes, *pdtpes, *pml4es, *valid;


void scan_fourth_level(uint64_t pde, bool clear_flag, uint64_t flag)
{
  uint64_t *ptable4_ptr;
  uint64_t *pte_ptr;
  uint64_t pte;

  ptable4_ptr = libc_mmap(NULL, BASEPAGE_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, devmemfd, pde & ADDRESS_MASK);
  if (ptable4_ptr == MAP_FAILED) {
    perror("third level page table mmap");
    ignore_this_mmap = true;
    assert(0);
    ignore_this_mmap = false;
  }

  pte_ptr = (uint64_t*)ptable4_ptr;
  for (int i = 0; i < 512; i++) {
    pte = *pte_ptr;
    fprintf(ptes, "%016lx\n", pte);

    if (((pte & FLAGS_MASK) & HEMEM_PAGE_WALK_FLAGS) == HEMEM_PAGE_WALK_FLAGS) {
      if (((pte & FLAGS_MASK) & HEMEM_PWTPCD_FLAGS) == 0) {
        fprintf(valid, "pte[%x]:   %016lx\n", i, pte);

        if (clear_flag) {
          pte = pte & ~flag;
        }
      }
    }

    pte_ptr++;
  }

  munmap(ptable4_ptr, BASEPAGE_SIZE);
}


void scan_third_level(uint64_t pdtpe, bool clear_flag, uint64_t flag)
{
  uint64_t *ptable3_ptr;
  uint64_t *pde_ptr;
  uint64_t pde;

  ptable3_ptr = libc_mmap(NULL, BASEPAGE_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, devmemfd, pdtpe & ADDRESS_MASK);
  if (ptable3_ptr == MAP_FAILED) {
    perror("third level page table mmap");
    ignore_this_mmap = true;
    assert(0);
    ignore_this_mmap = false;
  }

  pde_ptr = (uint64_t*)ptable3_ptr;
  for (int i = 0; i < 512; i++) {
    pde = *pde_ptr;
    fprintf(pdes, "%016lx\n", pde);

    if (((pde & FLAGS_MASK) & HEMEM_PAGE_WALK_FLAGS) == HEMEM_PAGE_WALK_FLAGS) {
      if (((pde & FLAGS_MASK) & HEMEM_PWTPCD_FLAGS) == 0) {
        fprintf(valid, "pde[%x]:   %016lx\n", i, pde);
        scan_fourth_level(pde, clear_flag, flag);
      }
    }

    pde_ptr++;
  }

  munmap(ptable3_ptr, BASEPAGE_SIZE);
}


void scan_second_level(uint64_t pml4e, bool clear_flag, uint64_t flag)
{
  uint64_t *ptable2_ptr;
  uint64_t *pdtpe_ptr;
  uint64_t pdtpe;

  ptable2_ptr = libc_mmap(NULL, BASEPAGE_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, devmemfd, pml4e & ADDRESS_MASK);
  if (ptable2_ptr == MAP_FAILED) {
    perror("second level page table mmap");
    ignore_this_mmap = true;
    assert(0);
    ignore_this_mmap = false;
  }

  pdtpe_ptr = (uint64_t*)ptable2_ptr;
  for (int i = 0; i < 512; i++) {
    pdtpe = *pdtpe_ptr;
    fprintf(pdtpes, "%016lx\n", pdtpe);

    if (((pdtpe & FLAGS_MASK) & HEMEM_PAGE_WALK_FLAGS) == HEMEM_PAGE_WALK_FLAGS) {
      if (((pdtpe & FLAGS_MASK) & HEMEM_PWTPCD_FLAGS) == 0) {
        fprintf(valid, "pdtpe[%x]: %016lx\n", i, pdtpe);
        scan_third_level(pdtpe, clear_flag, flag);
      }
    }

    pdtpe_ptr++;
  }

  munmap(ptable2_ptr, BASEPAGE_SIZE);
}


void _scan_pagetable(bool clear_flag, uint64_t flag)
{
  int *rootptr;
  uint64_t *pml4e_ptr;
  uint64_t pml4e;

  pml4es = fopen("logs/pml4es.txt", "w+");
  if (pml4es == NULL) {
    perror("pml4e file open");
    ignore_this_mmap = true;
    assert(0);
    ignore_this_mmap = false;
  }
  
  pdtpes = fopen("logs/pdtpes.txt", "w+");
  if (pdtpes == NULL) {
    perror("pdtpes open");
    ignore_this_mmap = true;
    assert(0);
    ignore_this_mmap = false;
  }

  pdes = fopen("logs/pdes.txt", "w+");
  if (pdes == NULL) {
    perror("pdes open");
    ignore_this_mmap = true;
    assert(0);
    ignore_this_mmap = false;
  }
  
  ptes = fopen("logs/ptes.txt", "w+");
  if (ptes == NULL) {
    perror("ptes open");
    ignore_this_mmap = true;
    assert(0);
    ignore_this_mmap = false;
  }

  valid = fopen("logs/valid.txt", "w+");
  if (valid == NULL) {
    perror("valid open");
    ignore_this_mmap = true;
    assert(0);
    ignore_this_mmap = false;
  }

  rootptr = libc_mmap(NULL, BASEPAGE_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, devmemfd, cr3 & ADDRESS_MASK);
  if (rootptr == MAP_FAILED) {
    perror("/dev/mem mmap");
    ignore_this_mmap = true;
    assert(0);
    ignore_this_mmap = false;
  }

  pml4e_ptr = (uint64_t*)rootptr;
  for (int i = 0; i < 512; i++) {
    pml4e = *pml4e_ptr;
    fprintf(pml4es, "%016lx\n", pml4e);

    if (((pml4e & FLAGS_MASK) & HEMEM_PAGE_WALK_FLAGS) == HEMEM_PAGE_WALK_FLAGS) {
      if (((pml4e & FLAGS_MASK) & HEMEM_PWTPCD_FLAGS) == 0) {
        fprintf(valid, "pml4e[%x]: %016lx\n", i, pml4e);
        scan_second_level(pml4e, clear_flag, flag); 
      }
    }
    pml4e_ptr++;
  }

  munmap(rootptr, BASEPAGE_SIZE);
}

void scan_pagetable()
{
  _scan_pagetable(false, 0);
}

#ifdef EXAMINE_PGTABLES
void *examine_pagetables()
{
  FILE *maps;
  int pagemaps;
  FILE *kpageflags;
  char *line = NULL;
  ssize_t nread;
  size_t len;
  uint64_t vm_start, vm_end;
  int n, num_pages;
  long index;
  off_t o;
  ssize_t t;
  struct pagemapEntry entry;
  int maps_copy;
  ssize_t nwritten;
  FILE *pfn_file;
  uint64_t num_pfn = 0;

  maps = fopen("/proc/self/maps", "r");
  if (maps == NULL) {
    perror("/proc/self/maps fopen");
    ignore_this_mmap = true;
    assert(0);
    ignore_this_mmap = false;
  }

  pagemaps = open("/proc/self/pagemap", O_RDONLY);
  if (pagemaps == -1) {
    perror("/proc/self/pagemap fopen");
    ignore_this_mamp = true;
    assert(0);
    ignore_this_mmap = false;
  }

  maps_copy = open("logs/maps.txt", O_CREAT | O_RDWR);
  if (maps_copy == -1) {
    perror("map.txt open");
    ignore_this_mmap = true;
    assert(0);
    ignore_this_mmap = false;
  }

  kpageflags = fopen("/proc/kpageflags", "r");
  if (kpageflags == NULL) {
    perror("/proc/kpageflags fopen");
    ignore_this_mmap = true;
    assert(0);
    ignore_this_mmap = false;
  }

  pfn_file = fopen("logs/pfn.txt", "w+");
  if (pfn_file == NULL) {
    perror("pfn.txt open");
    ignore_this_mmap = true;
    assert(0);
    ignore_this_mmap = false;
  }

  nread = getline(&line, &len, maps);
  while (nread != -1) {
    nwritten = write(maps_copy, line, nread);
    if (nwritten < 0) {
      perror("maps_copy write");
      ignore_this_mmap = true;
      assert(0);
      ignore_this_mmap = false;
    }
    if (strstr(line, DRAMPATH) != NULL) {
      n = sscanf(line, "%lX-%lX", &vm_start, &vm_end);
      if (n != 2) {
        fprintf(stderr, "error, invalid line: %s\n", line);
        ignore_this_mmap = true;
        assert(0);
        ignore_this_mmap = false;
      }

      num_pages = (vm_end - vm_start) / PAGE_SIZE;
      if (num_pages > 0) {
        index = (vm_start / PAGE_SIZE) * sizeof(uint64_t);

        o = lseek(pagemaps, index, SEEK_SET);
        if (o != index) {
          perror("pagemaps lseek");
          ignore_this_mmap = true;
          assert(0);
          ignore_this_mmap = false;
        }

        while (num_pages > 0) {
          uint64_t pfn;
          t = read(pagemaps, &pfn, sizeof(uint64_t));
          if (t < 0) {
            perror("pagemaps read");
            ignore_this_mmap = true;
            assert(0);
            ignore_this_mmap = false;
          }

          entry.pfn = pfn & 0x7ffffffffffff;
          entry.soft_dirty = (pfn >> 55) & 1;
          entry.exclusive = (pfn >> 56) & 1;
          entry.file_page = (pfn >> 61) & 1;
          entry.swapped = (pfn >> 62) & 1;
          entry.present = (pfn >> 63) & 1;

          fprintf(pfn_file, "DRAM: %016lX\n", (entry.pfn * sysconf(_SC_PAGESIZE)));
          num_pages--;
    num_pfn++;
        }
      }
    }
    else if (strstr(line, NVMPATH) != NULL) {
      n = sscanf(line, "%lX-%lX", &vm_start, &vm_end);
      if (n != 2) {
        fprintf(stderr, "error, invalid line: %s\n", line);
        ignore_this_mmap = true;
        assert(0);
        ignore_this_mmap = false;
      }

      num_pages = (vm_end - vm_start) / PAGE_SIZE;
      if (num_pages > 0) {
        index = (vm_start / PAGE_SIZE) * sizeof(uint64_t);

        o = lseek(pagemaps, index, SEEK_SET);
        if (o != index) {
          perror("pagemaps lseek");
          ignore_this_mmap = true;
          assert(0);
          ignore_this_mmap = false;
        }

        while (num_pages > 0) {
          uint64_t pfn;
          t = read(pagemaps, &pfn, sizeof(uint64_t));
          if (t < 0) {
            perror("pagemaps read");
            ignore_this_mmap = true;
            assert(0);
            ignore_this_mmap = false;
          }

          entry.pfn = pfn & 0x7ffffffffffff;
          entry.soft_dirty = (pfn >> 55) & 1;
          entry.exclusive = (pfn >> 56) & 1;
          entry.file_page = (pfn >> 61) & 1;
          entry.swapped = (pfn >> 62) & 1;
          entry.present = (pfn >> 63) & 1;

          fprintf(pfn_file, "NVM:  %016lX\n", (entry.pfn * sysconf(_SC_PAGE_SIZE)));
          num_pages--;
    num_pfn++;
        }
      }
    }
    nread = getline(&line, &len, maps);
  }

  fclose(maps);
  close(pagemaps);
  fclose(kpageflags);
  close(maps_copy);
  fclose(pfn_file);

  return 0;
}
#endif

