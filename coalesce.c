/*
 * =====================================================================================
 *
 *       Filename:  coaslecing.c
 *
 *    Description:  
 *
 *        Version:  1.0
 *        Created:  02/13/2020 11:03:17 AM
 *       Revision:  none
 *       Compiler:  gcc
 *
 *         Author:  YOUR NAME (), 
 *   Organization:  
 *
 * =====================================================================================
 */
#include <stdlib.h>
#include <unistd.h>
#include <sys/mman.h>
#include <linux/userfaultfd.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include "coalesce.h"
#include "hash.h"
#include "bitmap.h"

#define HASHTABLE_SIZE 81920UL

struct hash_table* dram_hp_ht;
struct hash_table* nvm_hp_ht;

const int BREAK_N = NUM_SMPAGES*BREAK_RATIO;
const int COALESCE_N = NUM_SMPAGES*COALESCE_RATIO;

void coalesce_pages(uint64_t addr, uint32_t fd, uint64_t offset){
  void* ret;
  //int ret2;
  //struct uffdio_range range;

  //get offset


  LOG("coalescing pages at %p\n", addr);
  hemem_combine_base_pages(addr);

  //LOG2("mapping hugepage at %p with offset %p and length %x\n", addr, offset & ~(HUGEPAGE_MASK), HUGEPAGE_SIZE);
  //ret = libc_mmap((void*) addr, HUGEPAGE_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE | MAP_FIXED, fd, offset & ~(HUGEPAGE_MASK));
  //if(ret == MAP_FAILED) perror("coalesce mmap");

  //LOG2("shooting TLB\n");
  //hemem_huge_tlb_shootdown(addr);
  //range.start = addr;
  //range.len = HUGEPAGE_SIZE;
  //ret2 = ioctl(uffd, UFFDIO_TLBFLUSH, &range);

  //if(ret2 < 0) perror("coalesce ioctl");
}

void coalesce_init() {
  dram_hp_ht = ht_alloc(HASHTABLE_SIZE);
  nvm_hp_ht = ht_alloc(HASHTABLE_SIZE);
  LOG2("N_SMPAGES = %d, COALESCE_N = %d, BREAK_N = %d, BASEPAGE=%u, HUGEPAGE=%u, PAGE=%u\n", NUM_SMPAGES, COALESCE_N, BREAK_N, BASEPAGE_SIZE, HUGEPAGE_SIZE, PAGE_SIZE);
  //uffd = syscall(__NR_userfaultfd, O_CLOEXEC | O_NONBLOCK);
}

void* check_aligned(uint64_t addr){
  
  LOG("checking for addr %p in huge page %p \n", addr, addr - (addr & HUGEPAGE_MASK));
  void* ret = ht_search(dram_hp_ht, addr - (addr & HUGEPAGE_MASK));
  //LOG("got %p \n", ret);

  if(ret) return ret;
  else return (void*) ht_search(nvm_hp_ht, addr - (addr & HUGEPAGE_MASK));
}

void incr_dram_huge_page (uint64_t addr, uint32_t fd, uint64_t offset){
  uint64_t bp_addr = addr;
  uint64_t page_offset = addr & HUGEPAGE_MASK;
  addr = bp_addr - page_offset;

  LOG("incrementing dram page at %p\n", addr);

  struct huge_page* this_hp = (struct huge_page*) ht_search(dram_hp_ht, addr); 
 
  LOG("hp found? %p\n", this_hp);

  if(this_hp){
    this_hp->num_faulted++;
    //LOG2("incrementing base page %p inside huge page %p with %d faulted at %d\n", bp_addr, addr, this_hp->num_faulted, page_offset/4096);
    //bitmap_print(&(this_hp->map));
    bitmap_set(&(this_hp->map), page_offset/4096);
    //bitmap_print(&(this_hp->map));
    if(this_hp->num_faulted == COALESCE_N) coalesce_pages(addr, fd, offset); 
  } else {
    LOG("inserting to hash table %p, %p, %u\n", addr, offset & ~(HUGEPAGE_MASK), fd);
    ht_insert(dram_hp_ht, addr, offset & ~(HUGEPAGE_MASK), fd, 1);
  }
}


void incr_nvm_huge_page (uint64_t addr, uint32_t fd, uint64_t offset){
  uint64_t bp_addr = addr;
  uint64_t page_offset = addr & HUGEPAGE_MASK;
  addr = bp_addr - page_offset;

  LOG("incrementing nvm page at %p\n", addr);
  
  struct huge_page* this_hp = (struct huge_page*) ht_search(nvm_hp_ht, addr); 
  
  if(this_hp){
    this_hp->num_faulted++;
    bitmap_set(&(this_hp->map), page_offset/4096);
    if(this_hp->num_faulted == NUM_SMPAGES) coalesce_pages(addr, fd, offset); 
  } else {
    ht_insert(nvm_hp_ht, addr, offset & (~HUGEPAGE_SIZE), fd, 1);
  }
}

void coalesce_thread() {
  int i;
  struct bucket* ht_bucket;

  coalesce_init();

  while(1){
    for(i = 0; i<HASHTABLE_SIZE; i++){
      if(dram_hp_ht->buckets[i].value4 == NUM_SMPAGES) coalesce_pages(dram_hp_ht->buckets[i].value, dram_hp_ht->buckets[i].value3, dram_hp_ht->buckets[i].value2);
      if(dram_hp_ht->buckets[i].next != NULL){
        ht_bucket = dram_hp_ht->buckets[i].next;
        do {
          if(ht_bucket->value4 == NUM_SMPAGES) coalesce_pages(ht_bucket->value, ht_bucket->value3, dram_hp_ht->buckets[i].value2);
          ht_bucket = ht_bucket->next;
        } while(ht_bucket);
      }
      
      if(nvm_hp_ht->buckets[i].value4 == NUM_SMPAGES) coalesce_pages(nvm_hp_ht->buckets[i].value, nvm_hp_ht->buckets[i].value3, nvm_hp_ht->buckets[i].value2);
      
      if(nvm_hp_ht->buckets[i].next != NULL){
        ht_bucket = nvm_hp_ht->buckets[i].next;
        do {
          if(ht_bucket->value4 == NUM_SMPAGES) coalesce_pages(ht_bucket->value, ht_bucket->value3, dram_hp_ht->buckets[i].value2);
          ht_bucket = ht_bucket->next;
        } while(ht_bucket);
      }
    }
    sleep(1);
  }
}

void break_huge_page(uint64_t addr, uint32_t fd, uint64_t offset){
  int i;

  for(i = 0; i < NUM_SMPAGES; i++){
    libc_mmap((void*) (addr + BASEPAGE_SIZE*i), BASEPAGE_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE | MAP_FIXED, fd, offset + BASEPAGE_SIZE*i);
  }
}

void decr_dram_huge_page(uint64_t addr) {
  uint64_t bp_addr = addr;
  uint64_t page_offset = addr & HUGEPAGE_MASK;
  addr = bp_addr - page_offset;

  struct huge_page* this_hp = (struct huge_page*) ht_search(dram_hp_ht, addr);
  LOG2("decrementing dram page at %p with %d faulted out of %d, %d total\n", addr, this_hp->num_faulted, BREAK_N, NUM_SMPAGES);

  if(this_hp){
    this_hp->num_faulted--;
    //bitmap_print(&(this_hp->map));
    bitmap_unset(&(this_hp->map), page_offset/4096);
    //bitmap_print(&(this_hp->map));
    if(this_hp->num_faulted == BREAK_N){
      LOG2("breaking huge page with %d faulted out of %d, n is %d\n", this_hp->num_faulted, NUM_SMPAGES, BREAK_N);
      hemem_break_huge_page(addr, this_hp->fd, this_hp->offset, &(this_hp->map));
    } 
    //if(this_hp->num_faulted == 0) ht_delete(dram_hp_ht, (struct bucket*) this_hp);
  } else {
    LOG("decrementing nonexistent huge page\n");
  }
}

void decr_nvm_huge_page(uint64_t addr) {
  uint64_t bp_addr = addr;
  uint64_t page_offset = addr & HUGEPAGE_MASK;
  addr = bp_addr - page_offset;

  LOG("decrementing nvm page at %p\n", addr);

  struct huge_page* this_hp = (struct huge_page*) ht_search(nvm_hp_ht, addr);

  if(this_hp){
    this_hp->num_faulted--;
    bitmap_unset(&(this_hp->map), page_offset/4096);
    if((this_hp->num_faulted)/NUM_SMPAGES < BREAK_RATIO) hemem_break_huge_page(addr, this_hp->fd, this_hp->offset, &(this_hp->map));
    //if(this_hp->num_faulted == 0) ht_delete(nvm_hp_ht, (struct bucket*) this_hp);
  } else {
    LOG("decrementing nonexistent huge page\n");
  }
}

void migrate_to_dram_hp(uint64_t addr, uint32_t fd, uint64_t offset) {
  decr_nvm_huge_page(addr);
  incr_dram_huge_page(addr, fd, offset);
}

void migrate_to_nvm_hp(uint64_t addr, uint32_t fd, uint64_t offset) {
  decr_dram_huge_page(addr);
  incr_nvm_huge_page(addr, fd, offset);
}
