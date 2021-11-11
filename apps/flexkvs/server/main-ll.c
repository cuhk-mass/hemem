/*
 * Copyright 2019 University of Washington, Max Planck Institute for
 * Software Systems, and The University of Texas at Austin
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <pthread.h>
#include <stdint.h>
#include <arpa/inet.h>

#include <tas_ll.h>
#include <protocol_binary.h>

#include "iokvs.h"
#include <utils.h>
#include <utils_circ.h>

#ifndef BATCH_MAX
#define BATCH_MAX 1
#endif

#define MAX_MSGSIZE 128
#define MAX_CONNS (128 * 1024)
#define MAX_EVENTS 32
#define MAX_KEY 255
#define LISTEN_PORT 11211

struct connection {
    struct flextcp_connection conn;
    void *buf_1;
    void *buf_2;
    int len_1;
    int len_2;
    int closed;

    struct connection *next;
    char buf[];
};

struct core {
    struct flextcp_context ctx;
    struct item_allocator ia;
    struct flextcp_listener listener;

    pthread_t pt;
    int id;
    uint8_t reqbuf[MAX_MSGSIZE];

    uint64_t reqs;
    uint64_t its;
    uint64_t clean;
};

static uint32_t listen_backlog = 512;
static struct item_allocator **iallocs;
static struct connection *conns;
static struct core **cores;
static volatile size_t n_ready = 0;

/** Opening listener and wait for success */
static int open_listening(struct flextcp_context *ctx,
        struct flextcp_listener *l)
{
    struct flextcp_event ev;
    int ret;

    //if (flextcp_listen_open(ctx, ol, LISTEN_PORT, listen_backlog, 0) != 0)
    if (flextcp_listen_open(ctx, l, LISTEN_PORT, listen_backlog, FLEXTCP_LISTEN_REUSEPORT) != 0)
    {
        fprintf(stderr, "flextcp_obj_listen_open failed\n");
        return -1;
    }

    /* wait until listen request is done */
    while (1) {
        if ((ret = flextcp_context_poll(ctx, 1, &ev)) < 0) {
            fprintf(stderr, "init_listen: flextcp_context_poll failed\n");
            return -1;
        }

        /* skip if no event */
        if (ret == 0)  {
            continue;
        }

        if (ev.event_type != FLEXTCP_EV_LISTEN_OPEN) {
            fprintf(stderr, "init_listen: unexpected event type (%u)\n",
                    ev.event_type);
            continue;
        }

        if (ev.ev.listen_open.status != 0) {
            fprintf(stderr, "init_listen: listen open request failed\n");
            return -1;
        }

        break;
    }

    return 0;
}

/** Connection event: new connection arrived */
static inline void connection_new(struct core *co,
        struct flextcp_listener *l)
{
    static volatile size_t allocd = 0;
    struct connection *c;
    size_t i;

    i = __sync_fetch_and_add(&allocd, 1);
    if (i >= MAX_CONNS) {
        fprintf(stderr, "connection_new: too many connections, dropping\n");
        return;
    }

    c = &conns[i];
    c->len_1 = c->len_2 = 0;
    if (flextcp_listen_accept(&co->ctx, l, &(c->conn)) != 0) {
        fprintf(stderr, "connection_new: flextcp_obj_listen_accept failed\n");
        abort();
    }
}

/** Connection event: accept succeeded */
static inline void connection_accepted(struct core *co, int16_t status,
        struct flextcp_connection *c)
{
    //printf("[%d] connection accepted\n", co->id);
}

/** Connection event: object received */
static inline void connection_recv(struct core *co, struct flextcp_event *ev)
{
    protocol_binary_request_header *req = (protocol_binary_request_header*) co->reqbuf;
    protocol_binary_response_get gres;
    protocol_binary_response_header *res = (protocol_binary_response_header*) &gres;
    struct item *it;
    void *keybuf;
    uint8_t op;
    uint32_t h, bl, vl, vo, rsl, ko;
    uint16_t kl;
    void *txb_1, *txb_2;
    size_t txl_1, txl_2, reql;
    struct connection *c = (struct connection *) ev->ev.conn_received.conn;

    /* add new bytes to receive buffer */
    if (c->len_1 == 0) {
      assert(c->len_2 == 0);
      c->buf_1 = ev->ev.conn_received.buf;
      c->len_1 = ev->ev.conn_received.len;
    } else if (c->buf_1 + c->len_1 == ev->ev.conn_received.buf) {
      assert(c->len_2 == 0);
      c->len_1 += ev->ev.conn_received.len;
    } else if (c->len_2 == 0) {
      c->buf_2 = ev->ev.conn_received.buf;
      c->len_2 = ev->ev.conn_received.len;
    } else if (c->buf_2 + c->len_2 == ev->ev.conn_received.buf) {
      c->len_2 += ev->ev.conn_received.len;
    } else {
      fprintf(stderr, "weird situation l1=%u l2=%u b1=%p b2=%p\n", c->len_1, c->len_2, c->buf_1, c->buf_2);
      abort();
    }


    /* consume requests */
    while (c->len_1 + c->len_2 >= sizeof(*req)) {
      /* read minimal header */
      split_read(req, sizeof(protocol_binary_request_header), c->buf_1, c->len_1, c->buf_2, c->len_2, 0);

      /* calculate total header length */
      bl = ntohl(req->request.bodylen);
      reql = sizeof(protocol_binary_request_header) + bl;

      if (c->len_1 + c->len_2 < reql) {
        /*printf("Request is short: %u+%u < %u\n", c->len_1, c->len_2, reql);*/
        break;
      }

      /* read rest of header if needed */
      if (req->request.extlen > 0) {
        split_read(req + 1, req->request.extlen, c->buf_1, c->len_1, c->buf_2,
            c->len_2, sizeof(protocol_binary_request_header));
      }


      /* caculate key & value length, and offset */
      kl = ntohs(req->request.keylen);
      ko = sizeof(protocol_binary_request_header) + req->request.extlen;
      vl = bl - req->request.extlen - kl;
      vo = sizeof(protocol_binary_request_header) + req->request.extlen + kl;

      /* read key */
      keybuf = co->reqbuf + ko;
      split_read(keybuf, kl, c->buf_1, c->len_1, c->buf_2, c->len_2, ko);


      op = req->request.opcode;
	/* validate request magic */
	if (req->request.magic != PROTOCOL_BINARY_REQ) {
		
		fprintf(stderr, "Closing connection on invalid magic: %x\n"
				"Other: op = %x, keylen= %u, extlen %u, bl = %u\n", 
				req->request.magic, op, kl, req->request.extlen, bl); 
		abort();
	}

	    if (op == PROTOCOL_BINARY_CMD_GET) {
		/* lookup item */
		h = jenkins_hash(keybuf, kl);
		it = hasht_get(keybuf, kl, h);

		/* calculate response length */
		rsl = sizeof(protocol_binary_response_get);
		if (it != NULL) {
		    rsl += it->vallen;
		}

		if (flextcp_connection_tx_alloc2(&c->conn, rsl, &txb_1, &txl_1,
			    &txb_2) != rsl)
		{
		    fprintf(stderr, "connection_recv: tx alloc failed (TODO)\n");
		    abort();
		}
		txl_2 = rsl - txl_1;

		/* build response header */
		res->response.magic = PROTOCOL_BINARY_RES;
		res->response.opcode = PROTOCOL_BINARY_CMD_GET;
		res->response.keylen = htons(0);
		res->response.extlen = 4;
		res->response.datatype = 0;
		if (it != NULL) {
		    res->response.status = htons(PROTOCOL_BINARY_RESPONSE_SUCCESS);
		    res->response.bodylen = htonl(4 + it->vallen);
		} else {
		    res->response.status = htons(PROTOCOL_BINARY_RESPONSE_KEY_ENOENT);
		    res->response.bodylen = htonl(4);
		}
		res->response.opaque = req->request.opaque;
		gres.message.body.flags = htonl(0);

		/* write to object */
		split_write(&gres, sizeof(gres), txb_1, txl_1, txb_2, txl_2, 0);
		if (it != NULL) {
		    split_write(item_value(it), it->vallen, txb_1, txl_1, txb_2,
			    txl_2, sizeof(gres));
		    item_unref(it);
		}
	    } else if (op == PROTOCOL_BINARY_CMD_SET) {
		/* allocate item */
		it = ialloc_alloc(&co->ia, sizeof(struct item) + kl + vl, false);

		/* if successfull, initialize then add to hash table */
		if (it != NULL) {
		    h = jenkins_hash(keybuf, kl);

		    it->hv = h;
		    it->vallen = vl;
		    it->keylen = kl;
		    memcpy(item_key(it), keybuf, kl);
                    split_read(item_value(it), vl, c->buf_1, c->len_1, c->buf_2, c->len_2, vo);

		    hasht_put(it, NULL);
		    item_unref(it);
		}

		/* allocate tx object */
		rsl = sizeof(protocol_binary_response_set);
		if (flextcp_connection_tx_alloc2(&c->conn, rsl, &txb_1, &txl_1,
			    &txb_2) != rsl)
		{
		    fprintf(stderr, "connection_recv: tx alloc failed (TODO)\n");
		    abort();
		}
		txl_2 = rsl - txl_1;

		/* build response header */
		res->response.magic = PROTOCOL_BINARY_RES;
		res->response.opcode = PROTOCOL_BINARY_CMD_SET;
		res->response.keylen = htons(0);
		res->response.extlen = 0;
		res->response.datatype = 0;
		if (it != NULL) {
		    res->response.status = htons(PROTOCOL_BINARY_RESPONSE_SUCCESS);
		} else {
		    res->response.status = htons(PROTOCOL_BINARY_RESPONSE_ENOMEM);
		}
		res->response.bodylen = htonl(0);
		res->response.opaque = req->request.opaque;

		/* write to object */
		split_write(res, sizeof(*res), txb_1, txl_1, txb_2, txl_2, 0);
	    } else {
		fprintf(stderr, "Closing connection on invalid request opcode: %x\n",
			op);
		abort();
	    }

	    if (c->len_1 >= reql) {
	      c->buf_1 += reql;
	      c->len_1 -= reql;
	      if (c->len_1 == 0) {
		c->buf_1 = c->buf_2;
		c->len_1 = c->len_2;
		c->len_2 = 0;
	      }
	    } else {
	      c->buf_2 += reql - c->len_1;
	      c->len_2 -= reql - c->len_1;
	      c->buf_1 = c->buf_2;
	      c->len_1 = c->len_2;
	      c->len_2 = 0;
	    }

	    
	    if (flextcp_connection_tx_send(&co->ctx, &c->conn, rsl) != 0) {
	      fprintf(stderr, "thread_event_rx: tx_send failed\n");
	      abort();
	    }

	    if (flextcp_connection_rx_done(&co->ctx, &c->conn, reql) != 0) {
	      fprintf(stderr, "thread_event_rx: rx_done failed\n");
	      abort();
	    }
    }
}
/*
static inline void thread_event_rx(struct core *co, struct flextcp_event *ev)
{
  struct connection *c = (struct connection *) ev->ev.conn_received.conn;
  size_t len;
  void *buf, *buf_2;
  ssize_t ret;

  protocol_binary_request_header req;
    protocol_binary_response_get gres;
    protocol_binary_response_header *res = &gres.message.header;
    struct item *it;
    void *keybuf = co->keybuf;

  if (c->len_1 == 0) {
    assert(c->len_2 == 0);
    c->buf_1 = ev->ev.conn_received.buf;
    c->len_1 = ev->ev.conn_received.len;
  } else if (c->buf_1 + c->len_1 == ev->ev.conn_received.buf) {
    assert(c->len_2 == 0);
    c->len_1 += ev->ev.conn_received.len;
  } else if (c->len_2 == 0) {
    c->buf_2 = ev->ev.conn_received.buf;
    c->len_2 = ev->ev.conn_received.len;
  } else if (c->buf_2 + c->len_2 == ev->ev.conn_received.buf) {
    c->len_2 += ev->ev.conn_received.len;
  } else {
    fprintf(stderr, "weird situation l1=%u l2=%u b1=%p b2=%p\n", c->len_1, c->len_2, c->buf_1, c->buf_2);
    abort();
  }

  while (c->len_1 + c->len_2 >= max_bytes) {
    ret = flextcp_connection_tx_alloc2(&c->conn, max_bytes, &buf, &len, &buf_2);
    if (ret != max_bytes) {
      fprintf(stderr, "thread_event_rx: tx alloc failed (%zd)\n", ret);
      abort();
    }

    
    assert(len == max_bytes);

    if (c->len_1 >= max_bytes) {
      memcpy(buf, c->buf_1, max_bytes);
      c->buf_1 += max_bytes;
      c->len_1 -= max_bytes;
      if (c->len_1 == 0) {
        c->buf_1 = c->buf_2;
        c->len_1 = c->len_2;
        c->len_2 = 0;
      }
    } else {
      memcpy(buf, c->buf_1, c->len_1);
      memcpy(buf + c->len_1, c->buf_2, max_bytes - c->len_1);

      c->buf_2 += max_bytes - c->len_1;
      c->len_2 -= max_bytes - c->len_1;
      c->buf_1 = c->buf_2;
      c->len_1 = c->len_2;
      c->len_2 = 0;
    }

    if (flextcp_connection_tx_send(&co->ctx, &c->conn, max_bytes) != 0) {
      fprintf(stderr, "thread_event_rx: tx_send failed\n");
      abort();
    }

    if (flextcp_connection_rx_done(&co->ctx, &c->conn, max_bytes) != 0) {
      fprintf(stderr, "thread_event_rx: rx_done failed\n");
      abort();
    }
  }

  assert(c->len_1 == 0);
}
*/
/** Cleaning up item allocator */
static size_t clean_log(struct item_allocator *ia, bool idle)
{
    struct item *it, *nit;
    size_t n;

    if (!idle) {
        /* We're starting processing for a new request */
        ialloc_cleanup_nextrequest(ia);
    }

    n = 0;
    while ((it = ialloc_cleanup_item(ia, idle)) != NULL) {
        n++;
        if (it->refcount != 1) {
            if ((nit = ialloc_alloc(ia, sizeof(*nit) + it->keylen + it->vallen,
                    true)) == NULL)
            {
                fprintf(stderr, "Warning: ialloc_alloc failed during cleanup :-/\n");
                abort();
            }

            nit->hv = it->hv;
            nit->vallen = it->vallen;
            nit->keylen = it->keylen;
            memcpy(item_key(nit), item_key(it), it->keylen + it->vallen);
            hasht_put(nit, it);
            item_unref(nit);
        }
        item_unref(it);
    }
    return n;
}

/* Worker thread main loop */
static void *processing_thread(void *data)
{
    struct core *co = data;
    struct item_allocator *ia = &co->ia;
    struct flextcp_context *ctx = &co->ctx;

    size_t total_reqs = 0, total_clean = 0;
    int cn = co->id, i, n, n_cu;
    struct flextcp_event *evs;

    ialloc_init_allocator(ia);

    if ((evs = calloc(MAX_EVENTS, sizeof(*evs))) == NULL) {
        fprintf(stderr, "Allocating event buffer failed\n");
        abort();
    }
    /* create listener connection */
    if (open_listening(ctx, &co->listener) != 0) {
        fprintf(stderr, "open_listening failed\n");
        return NULL;
    }

    __sync_fetch_and_add(&n_ready, 1);

    printf("[%d] Worker starting (ctx=%p)\n", cn, ctx);

    while (1) {
        n = flextcp_context_poll(ctx, MAX_EVENTS, evs);
        if (n < 0) {
            fprintf(stderr, "flextcp_context_poll failed\n");
            abort();
        }

        for (i = 0; i < n; i++) {
            if (evs[i].event_type == FLEXTCP_EV_CONN_RECEIVED) {
                util_prefetch0(evs[i].ev.conn_received.conn);
            }
        }

        for (i = 0; i < n; i++) {
            switch (evs[i].event_type) {
                case FLEXTCP_EV_LISTEN_NEWCONN:
                    connection_new(co, &co->listener); //evs[i].ev.listen_newconn.listener);
                    break;

                case FLEXTCP_EV_LISTEN_ACCEPT:
                    connection_accepted(co, evs[i].ev.listen_accept.status,
                            evs[i].ev.listen_accept.conn);
                    break;

                case FLEXTCP_EV_CONN_RECEIVED:
                    //thread_event_rx(co, evs[i]);
                    connection_recv(co, evs + i);//, evs[i].ev.conn_received.len,
                            //((struct connection*) evs[i].ev.conn_received.conn)->buf_1,
			    //((struct connection*) evs[i].ev.conn_received.conn)->len_1,
			    //((struct connection*) evs[i].ev.conn_received.conn)->buf_2,
			    //((struct connection*) evs[i].ev.conn_received.conn)->len_2);
                    break;

                default:
                    fprintf(stderr, "[%d] unknown event: %u\n", cn,
                            evs[i].event_type);
                    break;
            }
        }

        n_cu = clean_log(ia, n == 0);
        total_clean += n_cu;
        co->reqs += n;
        co->clean += n_cu;
        co->its++;
#if 0
        if (total_reqs / 100000 != (total_reqs + had_pkts) / 100000) {
            printf("%d: total=%10zu  clean=%10zu\n", cn, total_reqs, total_clean);
        }
#endif
        total_reqs += n;
    }

    return 0;
}

/** Maintenance thread */
static void maintenance(void)
{
    size_t i, n/*, j = 0*/;

    n = settings.numcores;
    while (1) {
        for (i = 0; i < n; i++) {
            ialloc_maintenance(iallocs[i]);
        }
        usleep(10);
        /*if (j++ % 10000ULL == 0) {
            printf("stats: ");
            for (i = 0; i < n; i++) {
                printf("%zu=%zu %zu %zu  ", i, cores[i]->reqs, cores[i]->clean, cores[i]->its);
                cores[i]->reqs = 0;
                cores[i]->its = 0;
                cores[i]->clean = 0;
            }
            printf("\n");
        }*/
    }
}


int main(int argc, char *argv[])
{
    unsigned num_threads, i;
    struct core *co;
    struct flextcp_obj_listener *ol;

    if (settings_init(argc, argv) != 0) {
        return EXIT_FAILURE;
    }
    num_threads = settings.numcores;

    /* initialize flextcp stack */
    if (flextcp_init() != 0) {
        fprintf(stderr, "flextcp_init failed\n");
        return EXIT_FAILURE;
    }

    /* initialize and allocate structs */
    hasht_init();
    ialloc_init();
    iallocs = calloc(num_threads, sizeof(*iallocs));
    conns = calloc(MAX_CONNS, sizeof(*conns));
    cores = calloc(num_threads, sizeof(*cores));
    ol = calloc(1, sizeof(*ol));
    if (iallocs == NULL || cores == NULL || conns == NULL || ol == NULL) {
        fprintf(stderr, "alloc failed in main\n");
        return EXIT_FAILURE;
    }

    /* allocate core structs and create flextcp contexts */
    for (i = 0; i < num_threads; i++) {
        if ((co = calloc(1, sizeof(*co))) == NULL) {
            fprintf(stderr, "flextcp_init failed\n");
            return EXIT_FAILURE;
        }

        if (flextcp_context_create(&co->ctx) != 0) {
            fprintf(stderr, "flextcp_context_create failed\n");
            return EXIT_FAILURE;
        }

        co->id = i;
        cores[i] = co;
        iallocs[i] = &co->ia;
    }

//    /* create listener connection */
 //   if (open_listening(&cores[0]->ctx, ol) != 0) {
 //       fprintf(stderr, "open_listening failed\n");
  //      return EXIT_FAILURE;
   // }

    /* start worker threads */
    for (i = 0; i < num_threads; i++) {
        co = cores[i];
        if (pthread_create(&co->pt, NULL, processing_thread, co)) {
            fprintf(stderr, "pthread_create failed\n");
            return EXIT_FAILURE;
        }
    }

    while (n_ready < num_threads);
    printf("Starting maintenance\n");
    fflush(stdout);
    while (1) {
        maintenance();
    }
    return 0;
}
