/*
 * =====================================================================================
 *
 *       Filename:  gups.h
 *
 *    Description:  i
 *
 *        Version:  1.0
 *        Created:  02/17/2020 09:13:33 AM
 *       Revision:  none
 *       Compiler:  gcc
 *
 *         Author:  YOUR NAME (), 
 *   Organization:  
 *
 * =====================================================================================
 */
#ifndef GUPS_H
#define GUPS_H

#define INDEX_FILE "logs/indices.txt"

//#define ZIPFIAN
#define HOTSPOT
//#define UNIFORM_RANDOM

void calc_indices(unsigned long* indices, unsigned long updates, unsigned long nelems);

#ifdef HOTSPOT
extern uint64_t hotset_start;
extern double hotset_fraction;
#endif

#endif
