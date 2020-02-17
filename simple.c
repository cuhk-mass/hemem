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
    page->in_dram = true;
    page->devdax_offset = fastmem;
    page->next = NULL;
    page->prev = NULL;
    fastmem += PAGE_SIZE;
  }
  else {
    assert(slowmem < NVMSIZE);
    page->in_dram = false;
    page->devdax_offset = slowmem;
    page->next = NULL;
    page->prev = NULL;
    slowmem += PAGE_SIZE;
    if (!slowmem_switch) {
      LOG("Switched to allocating from slowmem\n");
      slowmem_switch = true;
    }
  }
}

void simple_pagefault(struct hemem_page *page)
{
  assert(page != NULL);
  simple_allocate_page(page);
}

void simple_init(void)
{
  LOG("Memory management policy is simple\n");
}
