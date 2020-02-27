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

#define NUM_SMPAGES 512
#define HASHTABLE_SIZE 8192

struct hash_table* hp_ht;

void coalesce_pages(uint64_t addr, uint32_t fd, uint64_t offset){
  void* ret;
  int ret2;
  struct uffdio_range range;

  //get offset

  ret = mmap((void*) addr, HUGEPAGE_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE | MAP_FIXED, fd, offset % HUGEPAGE_SIZE);

  if(ret < 0) perror("coalesce mmap");

  range.start = addr;
  range.len = HUGEPAGE_SIZE;
  //ret2 = ioctl(uffd, UFFDIO_TLBFLUSH, &range);

  //if(ret2 < 0) perror("coalesce ioctl");
}

void coalesce_init() {
  hp_ht = ht_alloc(HASHTABLE_SIZE);
  //uffd = syscall(__NR_userfaultfd, O_CLOEXEC | O_NONBLOCK);
}

void* check_aligned(uint64_t addr){
  return (void*) ht_search(hp_ht, addr % HUGEPAGE_SIZE);
}

void check_huge_page (uint64_t addr, uint32_t fd, uint64_t offset){
  addr = addr % HUGEPAGE_SIZE;

  struct huge_page* this_hp = (struct huge_page*) ht_search(hp_ht, addr); 
  
  if(this_hp){
    this_hp->num_faulted++;
    if(this_hp->num_faulted == NUM_SMPAGES) coalesce_pages(addr, fd, offset); 
  } else {
    ht_insert(hp_ht, addr, offset % HUGEPAGE_SIZE, 1, fd);
  }
}

void coalesce_thread() {
  int i;
  struct bucket* ht_bucket;

  coalesce_init();

  while(1){
    for(i = 0; i<HASHTABLE_SIZE; i++){
      if(hp_ht->buckets[i].value4 == NUM_SMPAGES) coalesce_pages(hp_ht->buckets[i].value, hp_ht->buckets[i].value3, hp_ht->buckets[i].value2);
      if(hp_ht->buckets[i].next != NULL){
        ht_bucket = hp_ht->buckets[i].next;
        do {
          if(ht_bucket->value4 == NUM_SMPAGES) coalesce_pages(ht_bucket->value, ht_bucket->value3, hp_ht->buckets[i].value2);
          ht_bucket = ht_bucket->next;
        } while(ht_bucket);
      }
    }
    sleep(1);
  }
}

void break_huge_page(uint64_t addr){

}

void decrement_huge_page(uint64_t addr) {
  addr = addr % HUGEPAGE_SIZE;

  struct huge_page* this_hp = (struct huge_page*) ht_search(hp_ht, addr);

  if(this_hp){
    if(this_hp->num_faulted == NUM_SMPAGES) break_huge_page(addr);
    this_hp->num_faulted--;
    if(this_hp->num_faulted == 0) ht_delete(hp_ht, (struct bucket*) this_hp);
  } else {
    LOG("decrementing nonexistent huge page\n");
  }
}

