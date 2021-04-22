#ifndef HEMEM_FIFO_H
#define HEMEM_FIFO_H

#include <pthread.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdlib.h>

#include "hemem.h"

struct fifo_list {
  struct hemem_page *first, *last;
  pthread_mutex_t list_lock;
  size_t numentries;
};


void enqueue_fifo(struct fifo_list *list, struct hemem_page *page);
struct hemem_page* dequeue_fifo(struct fifo_list *list);
void page_list_remove_page(struct fifo_list *list, struct hemem_page *page);

#endif

