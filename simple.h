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
#ifndef HEMEM_SIMPLE_H
#define HEMEM_SIMPLE_H

#include <stdint.h>
#include <stdbool.h>

#include "hemem.h"
#include "paging.h"

extern uint64_t fastmem;
extern uint64_t slowmem;

struct hemem_page* simple_pagefault();
void simple_init(void);

#endif // HEMEM_SIMPLE_H
