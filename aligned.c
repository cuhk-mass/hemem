/*
 * =====================================================================================
 *
 *       Filename:  simple.c
 *
 *    Description:  
 *
 *        Version:  1.0
 *        Created:  02/04/2020 09:58:58 AM
 *       Revision:  none
 *       Compiler:  gcc
 *
 *         Author:  YOUR NAME (), 
 *   Organization:  
 *
 * =====================================================================================
 */
#include <stdlib.h>
#include <pthread.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdbool.h>
#include <pthread.h>
#include <assert.h>

#include "hemem.h"
#include "paging.h"
#include "coalesce.h"

void aligned_allocate_page(struct hemem_page *page, struct huge_page* this_hp)
{
  uint64_t inpage_offset = page->va & HUGEPAGE_MASK; 
  page->in_dram = true;
  page->devdax_offset = this_hp->offset + inpage_offset;
  page->next = NULL;
  page->prev = NULL;
  page->pt = pagesize_to_pt(PAGE_SIZE);
}

void aligned_pagefault(struct hemem_page *page, void* huge_page)
{
  struct huge_page* this_hp = (struct huge_page*) huge_page;
  aligned_allocate_page(page, this_hp);
}

void check_in_dram(struct hemem_page* page, void* huge_page, uint32_t dramfd, uint32_t nvmfd)
{
  struct huge_page* this_hp = (struct huge_page*) huge_page;

  if(this_hp->fd == dramfd){
    page->in_dram = true;
    return;
  }
  if(this_hp->fd == nvmfd){
    page->in_dram = false;
    return;
  }
  LOG("fd is in neither dram or nvm, something has gone wrong with %p; this fd: %u nvm: %u dram: %u\n", this_hp, this_hp->fd, dramfd, nvmfd);
  exit(0);
}
void aligned_init(void)
{
  LOG("Memory management policy is aligned, this should not happen!\n");
}
