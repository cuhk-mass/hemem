/*
 * =====================================================================================
 *
 *       Filename:  simple.h
 *
 *    Description:  
 *
 *        Version:  1.0
 *        Created:  02/04/2020 09:56:26 AM
 *       Revision:  none
 *       Compiler:  gcc
 *
 *         Author:  YOUR NAME (), 
 *   Organization:  
 *
 * =====================================================================================
 */
#ifndef HEMEM_ALIGNED_H
#define HEMEM_ALIGNED_H

#include <stdint.h>
#include <stdbool.h>

#include "hemem.h"
#include "paging.h"

extern uint64_t fastmem;
extern uint64_t slowmem;

void aligned_pagefault(struct hemem_page *page, void* huge_page);
void aligned_init(void);
void check_in_dram(struct hemem_page* page, void* huge_page, uint32_t dramfd, uint32_t nvmfd);

#endif // HEMEM_ALIGNED_H
