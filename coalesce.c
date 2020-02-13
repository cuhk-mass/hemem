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

#include "hemem.h"
#include "paging.h"
#include "hash.h"

#define NUM_SMPAGES 512
#define HASHTABLE_SIZE 8192

struct huge_page {
  uint64_t base_addr;
  uint16_t num_faulted;
  uint16_t fd;
};

struct hash_table* hp_ht;
extern long uffd;

void coalesce_pages(uint64_t addr, uint32_t fd){
  int i, ret2;
  void* ret;
  struct uffdio_range range;

  //lock pages??

  for(i = 0; i < NUM_SMPAGES; i++){
    munmap((void*) (addr + (i * 4096)), 4096);
  }
  ret = mmap((void*) addr, HUGEPAGE_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE | MAP_FIXED, fd, 0);

  if(ret == NULL) perror("coalesce mmap");

  range.start = addr;
  range.len = HUGEPAGE_SIZE;
  ret2 = ioctl(uffd, UFFDIO_TLBFLUSH, &range);

  if(ret2 < 0) perror("coalesce ioctl");
}

void coalesce_thread() {
  int i;
  struct bucket* ht_bucket;

  hp_ht = ht_alloc(HASHTABLE_SIZE);
  uffd = syscall(__NR_userfaultfd, O_CLOEXEC | O_NONBLOCK);

  while(1){
    for(i = 0; i<HASHTABLE_SIZE; i++){
      if(hp_ht->buckets[i].value2 == NUM_SMPAGES) coalesce_pages(hp_ht->buckets[i].value, hp_ht->buckets[i].value3);
      if(hp_ht->buckets[i].next != NULL){
        ht_bucket = hp_ht->buckets[i].next;
        do {
          if(ht_bucket->value2 == NUM_SMPAGES) coalesce_pages(ht_bucket->value, ht_bucket->value3);
          ht_bucket = ht_bucket->next;
        } while(ht_bucket);
      }
    }
    sleep(1);
  }
}

void check_huge_page (uint64_t addr, uint32_t fd){
  addr = addr % HUGEPAGE_SIZE;

  struct huge_page* this_hp = (struct huge_page*) ht_search(hp_ht, addr); 
  
  if(this_hp){
    this_hp->num_faulted++;
  } else {
    ht_insert(hp_ht, addr, 1, fd);
  }
}
