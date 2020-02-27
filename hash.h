#ifndef HASH_TABLE
#define HASH_TABLE
/*
 * =====================================================================================
 *
 *       Filename:  hash.h
 *
 *    Description:  
 *
 *        Version:  1.0
 *        Created:  02/13/2020 11:20:58 AM
 *       Revision:  none
 *       Compiler:  gcc
 *
 *         Author:  YOUR NAME (), 
 *   Organization:  
 *
 * =====================================================================================
 */

struct bucket {
  uint64_t value;
  uint64_t value2;
  uint32_t value3;
  uint16_t value4;
  struct bucket* next;
};

struct hash_table {
  struct bucket* buckets;
  uint16_t n_buckets;
};

uint16_t hash(uint64_t input, uint16_t size){
  input ^= input >> 7;
  input ^= input << 9;
  input ^= input >> 13;
  return input % size;
}

static inline struct hash_table* ht_alloc(uint16_t size) {
  struct hash_table* new_ht = malloc(sizeof(struct hash_table));
  int i = 0;

  new_ht->buckets = malloc(sizeof(struct bucket) * size);
  new_ht->n_buckets = size;

  for(i = 0; i<size; i++){
    new_ht->buckets[i].next = NULL;
    new_ht->buckets[i].value = -1;
  }
  return new_ht;
}

static inline void ht_insert(struct hash_table* ht, uint64_t value, uint64_t value2, uint32_t value3, uint16_t value4){
  uint16_t index = hash(value, ht->n_buckets);
  struct bucket* bucket = &(ht->buckets[index]);

  if(bucket->value == -1){
    bucket->value = value;
    bucket->value2 = value2;
    bucket->value3 = value3;
    bucket->value4 = value4;
  } 
  else {
    struct bucket* new_bucket = malloc(sizeof(struct bucket));

    new_bucket->next = NULL;
    new_bucket->value = value;
    new_bucket->value2 = value2;
    new_bucket->value3 = value3;
    new_bucket->value4 = value4;

    while(bucket->next != NULL) bucket = bucket->next;
    bucket->next = new_bucket;
  } 
}

static inline struct bucket* ht_search(struct hash_table* ht, uint64_t value){
  uint16_t index = hash(value, ht->n_buckets);
  struct bucket* bucket = &(ht->buckets[index]);

  do {
    if (bucket->value == value) return bucket;
  } while (bucket->next != NULL);

  return 0;
}

static inline void ht_delete(struct hash_table* ht, struct bucket* bucket){
  if(bucket->next == NULL) bucket->value = -1;
  else {
    bucket->value = bucket->next->value;
    bucket->value2 = bucket->next->value2;
    bucket->value3 = bucket->next->value3;
    bucket->value4 = bucket->next->value4;
    bucket->next = bucket->next->next;
  }

}
#endif
