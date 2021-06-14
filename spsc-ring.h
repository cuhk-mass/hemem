#ifndef SPSC_RING_H
#define SPSC_RING_H

#include <stdbool.h>

typedef struct ring_buf_t ring_buf_t;

typedef ring_buf_t* ring_handle_t;

ring_handle_t ring_buf_init(uint64_t** buffer, size_t size);
void ring_buf_free(ring_handle_t rbuf);
void ring_buf_reset(ring_handle_t rbuf);
void ring_buf_put(ring_handle_t rbuf, uint64_t* data);
int ring_buf_put2(ring_handle_t rbuf, uint64_t* data);
uint64_t* ring_buf_get(ring_handle_t rbuf);
bool ring_buf_empty(ring_handle_t rbuf);
bool ring_buf_full(ring_handle_t rbuf);
size_t ring_buf_capacity(ring_handle_t rbuf);
size_t ring_buf_size(ring_handle_t rbuf);

#endif //SPSC_RING_H
