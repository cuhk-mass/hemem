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

uint64_t fastmem = 0;
uint64_t slowmem = 0;
bool slowmem_switch = false;

void simple_allocate_page(struct hemem_page *page)
{
  if (fastmem< DRAMSIZE) {
    LOG("in dram\n");
    page->in_dram = true;
    page->devdax_offset = fastmem;
    page->next = NULL;
    page->prev = NULL;
    page->accessed = true;
    fastmem += PAGE_SIZE;
  }
  else {
    LOG("in nvm\n");
    assert(slowmem < NVMSIZE);
    page->in_dram = false;
    page->devdax_offset = slowmem;
    page->next = NULL;
    page->prev = NULL;
    page->accessed = true;
    slowmem += PAGE_SIZE;
    if (!slowmem_switch) {
      printf("switch to allocating from slowmem\n");
      slowmem_switch = true;
    }
  }
}

void simple_pagefault(struct hemem_page *page)
{
  simple_allocate_page(page);
}

void simple_init(void)
{
}
