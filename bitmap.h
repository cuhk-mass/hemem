/*
 * =====================================================================================
 *
 *       Filename:  bitmap.h
 *
 *    Description:  
 *
 *        Version:  1.0
 *        Created:  04/17/2020 10:15:54 AM
 *       Revision:  none
 *       Compiler:  gcc
 *
 *         Author:  YOUR NAME (), 
 *   Organization:  
 *
 * =====================================================================================
 */

#ifndef BITMAP

#include <stdio.h>

struct bitmap {
  uint8_t bytes[64];
};

static inline void print_bin(uint8_t n){
  int i=0;
  for(i = 0; i<8; i++){
    if(n&1) printf("1");
    else printf("0");
    n >>= 1;
  }  
}
static inline void bitmap_set(struct bitmap* map, uint32_t pos){
  uint8_t* byte = &(map->bytes[pos/8]);
  uint8_t sub_pos = pos % 8;

  *byte = (*byte) | (1 << sub_pos);
}

static inline void bitmap_unset(struct bitmap* map, uint32_t pos){
  uint8_t* byte = &(map->bytes[pos/8]);
  uint8_t sub_pos = pos % 8;

  *byte = (*byte) & (0xff ^ (1 << sub_pos));
}

static inline int bitmap_get(struct bitmap* map, uint32_t pos){
  uint8_t* byte = &(map->bytes[pos/8]);
  uint8_t sub_pos = pos % 8;

  return ((*byte) & (1 << sub_pos)) >> sub_pos; 
}

static inline void bitmap_print(struct bitmap* map){
  int i = 0;
  for(i=0; i<64; i++){
    print_bin(map->bytes[i]);
  }
  printf("\n");
}
#define BITMAP
#endif
