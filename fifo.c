#include <pthread.h>
#include <stdlib.h>

#include "hemem.h"
#include "fifo.h"


// assumes entry's page_lock is held, releases page_lock upon completion
void enqueue_fifo(struct fifo_list *queue, struct hemem_page *entry)
{
  pthread_mutex_lock(&(queue->list_lock));
  assert(entry->prev == NULL);
  entry->next = queue->first;
  if(queue->first != NULL) {
    assert(queue->first->prev == NULL);
    queue->first->prev = entry;
  } else {
    assert(queue->last == NULL);
    assert(queue->numentries == 0);
    queue->last = entry;
  }

  queue->first = entry;
  entry->list = queue;
  queue->numentries++;
  pthread_mutex_unlock(&(queue->list_lock));
  pthread_mutex_unlock(&(entry->page_lock));
}

// locks returned page's page_lock upon completion
struct hemem_page *dequeue_fifo(struct fifo_list *queue)
{
  pthread_mutex_lock(&(queue->list_lock));
  struct hemem_page *ret = queue->last;

  if(ret == NULL) {
    //assert(queue->numentries == 0);
    pthread_mutex_unlock(&(queue->list_lock));
    return ret;
  }

  pthread_mutex_lock(&(ret->page_lock));
  //pthread_mutex_lock(&(queue->list_lock));

  queue->last = ret->prev;
  if(queue->last != NULL) {
    queue->last->next = NULL;
  } else {
    queue->first = NULL;
  }

  ret->prev = ret->next = NULL;
  ret->list = NULL;
  assert(queue->numentries > 0);
  queue->numentries--;
  pthread_mutex_unlock(&(queue->list_lock));

  return ret;
}


// assumes page's page lock is held
void page_list_remove_page(struct fifo_list *list, struct hemem_page *page)
{
  pthread_mutex_lock(&(list->list_lock));
  if (list->first == NULL) {
    assert(list->last == NULL);
    assert(list->numentries == 0);
    pthread_mutex_unlock(&(list->list_lock));
    LOG("page_list_remove_page: list was empty!\n");
    return;
  }

  if (list->first == page) {
    list->first = page->next;
  }

  if (list->last == page) {
    list->last = page->prev;
  }

  if (page->next != NULL) {
    page->next->prev = page->prev;
  }

  if (page->prev != NULL) {
    page->prev->next = page->next;
  }

  assert(list->numentries > 0);
  list->numentries--;
  page->next = NULL;
  page->prev = NULL;
  page->list = NULL;
  pthread_mutex_unlock(&(list->list_lock));
}


