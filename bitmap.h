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

struct bitmap {
  uint8_t bytes[64];
};

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

#define BITMAP
#endif
