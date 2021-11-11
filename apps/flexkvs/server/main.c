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

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <pthread.h>
#include <stdint.h>
#include <inttypes.h>
#include <sys/un.h>

#include <protocol_binary.h>

#include "iokvs.h"
#include "../../common/socket_shim.h"
#include <utils.h>

#ifndef BATCH_MAX
#define BATCH_MAX 1
#endif

#define MAX_MSGSIZE 1000000
#define MAX_CONNS (128 * 1024)
#define LISTEN_BACKLOG 128
#define EPOLL_EVENTS 16
#define LISTEN_PORT 11211

enum conn_status {
  CONN_RECV,
  CONN_SEND,
};

struct connection {
  struct item *it;
  int fd;
  uint16_t rx_len;
  uint16_t rx_off;
  uint16_t tx_len;
  uint16_t tx_off;
  uint8_t epoll_wr;
  uint8_t req_done;
  uint8_t rx_buf[MAX_MSGSIZE];
  uint8_t tx_buf[MAX_MSGSIZE];
};

extern void hemem_print_stats();

static int open_listening(ssctx_t sc, int cn)
{
    int fd, ret;
    //struct sockaddr_un sa;
    struct sockaddr_in si;
    char buf[32];

    sprintf(buf, "kvs_sock%d", cn);
    fprintf(stderr, "open listening %d\n", cn);
    if ((fd = ss_socket(sc, AF_INET, SOCK_STREAM, IPPROTO_TCP)) < 0) {
	//if ((fd = ss_socket(sc, AF_UNIX, SOCK_STREAM, 0)) < 0) {
        fprintf(stderr, "[%d] socket failed\n", cn);
        abort();
    }

    /* set reuse port (for linux to open multiple listening sockets) */
    if (ss_set_reuseport(sc, fd) != 0) {
        fprintf(stderr, "[%d] set reuseport failed\n", cn);
        abort();
    }

    /* set non blocking */
    if ((ret = ss_set_nonblock(sc, fd)) < 0) {
        fprintf(stderr, "[%d] setsock_nonblock failed: %d\n", cn, ret);
        abort();
    }

    memset(&si, 0, sizeof(si));
    
    //strncpy(sa.sun_path, buf, sizeof(sa.sun_path) - 1);
    //sa.sun_family = AF_UNIX;
    si.sin_family = AF_INET;
    si.sin_port = htons(LISTEN_PORT);

    /* bind socket */
    if ((ret = ss_bind(sc, fd, (struct sockaddr *) &si, sizeof(si))) < 0) {
        fprintf(stderr, "[%d] bind failed: %d\n", cn, ret);
        perror("bind");
	abort();
    }

    /* listen on socket */
    if ((ret = ss_listen(sc, fd, LISTEN_BACKLOG)) < 0) {
        fprintf(stderr, "[%d] listen failed: %d\n", cn, ret);
        abort();
    }

    return fd;
}

static void network_init(int *p_epfd, int *p_lfd, ssctx_t *p_sc, int cn)
{
    int lfd, epfd;
    ssctx_t sc;
    ss_epev_t ev;
#ifdef USE_MTCP
    int ret;

    /* get mtcp context ready */
    if ((ret = mtcp_core_affinitize(cn)) != 0) {
        fprintf(stderr, "[%d] mtcp_core_affinitize failed: %d\n", cn, ret);
        abort();
    }

    if ((sc = mtcp_create_context(cn)) == NULL) {
        fprintf(stderr, "[%d] mtcp_create_context failed\n", cn);
        abort();
    }
#else
    memset(&sc, 0, sizeof(sc));
#endif

    /* prepare listening socket */
    lfd = open_listening(sc, cn);

    /* prepare epoll */
    if ((epfd = ss_epoll_create(sc, MAX_CONNS + 1)) < 0) {
        fprintf(stderr, "[%d] epoll_create failed\n", cn);
        abort();
    }

    /* add listening socket to epoll */
    ev.events = SS_EPOLLIN | SS_EPOLLERR | SS_EPOLLHUP;
    ev.data.ptr = NULL;
    if (ss_epoll_ctl(sc, epfd, SS_EPOLL_CTL_ADD, lfd, &ev) < 0) {
        fprintf(stderr, "[%d] mtcp_epoll_ctl\n", cn);
        abort();
    }

    *p_epfd = epfd;
    *p_lfd = lfd;
    *p_sc = sc;
}

static void accept_connections(ssctx_t sc, int epfd, int lfd, int cn)
{
    int cfd;
    struct connection *c;
    ss_epev_t ev;

    while (1) {
        if ((cfd = ss_accept(sc, lfd, NULL, NULL)) < 0) {
            if (errno == EAGAIN) {
                break;
            }

            fprintf(stderr, "[%d] accept failed: %d\n", cn, cfd);
            abort();
        }

        if (ss_set_nonblock(sc, cfd) < 0) {
            fprintf(stderr, "[%d] set nonblock failed\n", cn);
            abort();
        }

        //if (ss_set_nonagle(sc, cfd) < 0) {
        //    fprintf(stderr, "[%d] set nonagle failed\n", cn);
        //    abort();
        //}

	printf("allocating connections\n");
        if ((c = calloc(1, sizeof(*c))) == NULL) {
            fprintf(stderr, "[%d] connection calloc failed\n", cn);
            close(cfd);
            break;
        }

        c->fd = cfd;

        /* add socket to epoll */
        ev.data.ptr = c;
        ev.events = SS_EPOLLIN | SS_EPOLLERR | SS_EPOLLHUP;
        if (ss_epoll_ctl(sc, epfd, SS_EPOLL_CTL_ADD, cfd, &ev) < 0) {
            fprintf(stderr, "[%d] epoll_ctl CA\n", cn);
            abort();
        }
    }
}

static void conn_close(ssctx_t sc, struct connection *c)
{
    ss_close(sc, c->fd);
    if (c->it != NULL) {
        item_unref(c->it);
    }
    free(c);
}

static inline int conn_upepoll(ssctx_t sc, int epfd, struct connection *c,
        uint8_t wr)
{
    ss_epev_t ev;

    if (c->epoll_wr == wr) {
        return 0;
    }

    printf("conn_epupdate(%p, %d)\n", c, wr);

    ev.data.ptr = c;
    ev.events = (wr ? SS_EPOLLOUT : SS_EPOLLIN) | SS_EPOLLERR | SS_EPOLLHUP;
    if (ss_epoll_ctl(sc, epfd, SS_EPOLL_CTL_MOD, c->fd, &ev) < 0) {
        fprintf(stderr, "epoll_ctl in conn_upepoll failed\n");
        abort();
    }
    c->epoll_wr = wr;

    return 0;
}

static inline int conn_req_iscomp(struct connection *c)
{
    protocol_binary_request_header *req =
            (protocol_binary_request_header *) (c->rx_buf + c->rx_off);
    uint16_t avail = c->rx_len - c->rx_off;
    //fprintf(stderr, "rx_len %d rx_off %d req size %d bodylen %d\n", c->rx_len, c->rx_off, sizeof(*req), ntohl(req->request.bodylen));
    if (avail < sizeof(*req)) {
        return 0;
    }
    if (avail < sizeof(*req) + ntohl(req->request.bodylen)) {
        return 0;
    }
    return 1;
}

static inline int conn_recv(ssctx_t sc, int epfd, struct connection *c)
{
    ssize_t res;

    /* read until request is complete (or no more data) */
    do {
        res = ss_read(sc, c->fd, c->rx_buf + c->rx_len, MAX_MSGSIZE -
                c->rx_len);
        if (res == 0) {
	    fprintf(stderr, "Closing connection on EOF\n");
            conn_close(sc, c);
            return -1;
        } else if (res < 0 && errno == EAGAIN) {
            /* request is not complete yet */
            conn_upepoll(sc, epfd, c, 0);
            return 1;
        } else if (res < 0) {
            perror("Closing connection on read error");
            conn_close(sc, c);
            return -1;
        }
        c->rx_len += res;
    } while (!conn_req_iscomp(c));

    return 0;
}

static inline int conn_send(ssctx_t sc, int epfd, struct connection *c)
{
    ssize_t res;

    /* write until tx buffer is empty (or can't send) */
    do {
        res = ss_write(sc, c->fd, c->tx_buf + c->tx_off,
                c->tx_len - c->tx_off);
        if (res == 0) {
            fprintf(stderr, "Closing connection on TX EOF\n");
            conn_close(sc, c);
            return -1;
        } else if (res < 0 && errno == EAGAIN) {
            /* request is not complete yet */
            conn_upepoll(sc, epfd, c, 1);
            return 1;
        } else if (res < 0) {
            perror("Closing connection on write error");
            conn_close(sc, c);
            return -1;
        }

        c->tx_off += res;
    } while (c->tx_off < c->tx_len);

    c->tx_off = 0;
    c->tx_len = 0;
    return 0;
}

static inline int conn_req_process(ssctx_t sc, struct item_allocator *ia,
        struct connection *c)
{
    assert(ia != NULL);
    protocol_binary_request_header *req =
            (protocol_binary_request_header *) (c->rx_buf + c->rx_off);
    protocol_binary_request_set *seth =
            (protocol_binary_request_set *) req;
    protocol_binary_response_header *res =
        (protocol_binary_response_header *) (c->tx_buf + c->tx_len);
    protocol_binary_response_get *gres =
        (protocol_binary_response_get *) (c->tx_buf + c->tx_len);

    uint8_t op = req->request.opcode;
    uint16_t kl = ntohs(req->request.keylen);
    uint32_t h, bl = ntohl(req->request.bodylen), vl, rql, rsl;
    void *k, *v;

    if (req->request.magic != PROTOCOL_BINARY_REQ) {
        fprintf(stderr, "Closing connection on invalid magic: %x\n",
                req->request.magic);
        conn_close(sc, c);
        return -1;
    }

    rql = sizeof(*req) + bl;

    if (op == PROTOCOL_BINARY_CMD_GET) {
        /* lookup item */
        if (!c->req_done) {
            k = req + 1;
            h = jenkins_hash(k, kl);
            c->it = hasht_get(k, kl, h);
            c->req_done = 1;
        }

        /* calculate response length */
        rsl = sizeof(protocol_binary_response_get);
        if (c->it != NULL) {
            rsl += c->it->vallen;
        }

        /* check whether we have sufficient tx buffer space available */
        if (MAX_MSGSIZE - c->tx_len < rsl) {
            /* not enough space, start sending out */
            return 1;
        }

        /* build response header */
        res->response.magic = PROTOCOL_BINARY_RES;
        res->response.opcode = PROTOCOL_BINARY_CMD_GET;
        res->response.keylen = htons(0);
        res->response.extlen = 4;
        res->response.datatype = 0;
        if (c->it != NULL) {
            res->response.status = htons(PROTOCOL_BINARY_RESPONSE_SUCCESS);
            res->response.bodylen = htonl(4 + c->it->vallen);
        } else {
            res->response.status = htons(PROTOCOL_BINARY_RESPONSE_KEY_ENOENT);
            res->response.bodylen = htonl(4);
        }
        res->response.opaque = req->request.opaque;
        gres->message.body.flags = htonl(0);

        /* add item value */
        if (c->it != NULL) {
            memcpy(gres + 1, item_value(c->it), c->it->vallen);
            item_unref(c->it);
        }
    } else if (op == PROTOCOL_BINARY_CMD_SET) {
        k = seth + 1;
        v = (uint8_t *) k + kl;
        vl = bl - kl - req->request.extlen;
	    
        if (!c->req_done) {
            /* allocate item */
            assert(ia != NULL);
	    //c->it = ialloc_alloc(ia, sizeof(struct item) + kl + vl, false);

	    //printf("got set\n");
            /* if successfull, initialize then add to hash table */
            //if (c->it != NULL) {
            	h = jenkins_hash(k, kl);

		c->it = hasht_get(k, kl, h);

            	if(c->it == NULL) {
	    		//printf("item not found\n");
			c->it = ialloc_alloc(ia, sizeof(struct item) + kl + vl, false);
                
			c->it->hv = h;
                	c->it->vallen = vl;
                	c->it->keylen = kl;
                	memcpy(item_key(c->it), k, kl);
                	memcpy(item_value(c->it), v, vl);

                	hasht_put(c->it, NULL);
		} else {
	    		//printf("item found\n");
			c->it->hv = h;
                	c->it->vallen = vl;
                	c->it->keylen = kl;
                	memcpy(item_key(c->it), k, kl);
                	memcpy(item_value(c->it), v, vl);
		}
                item_unref(c->it);
            //}

            c->req_done = 1;
        }

        /* check whether we have sufficient tx buffer space available */
        rsl = sizeof(protocol_binary_response_set);
        if (MAX_MSGSIZE - c->tx_len < rsl) {
            /* not enough space, start sending out */
            return 1;
        }

        /* build response header */
        res->response.magic = PROTOCOL_BINARY_RES;
        res->response.opcode = PROTOCOL_BINARY_CMD_SET;
        res->response.keylen = htons(0);
        res->response.extlen = 0;
        res->response.datatype = 0;
        if (c->it != NULL) {
            res->response.status = htons(PROTOCOL_BINARY_RESPONSE_SUCCESS);
        } else {
            res->response.status = htons(PROTOCOL_BINARY_RESPONSE_ENOMEM);
        }
        res->response.bodylen = htonl(0);
        res->response.opaque = req->request.opaque;
    } else if (op == PROTOCOL_BINARY_CMD_DELETE) {
        int success = 0;
        if (!c->req_done) {
            k = req + 1;
            h = jenkins_hash(k, kl);
            c->it = hasht_get(k, kl, h);
	    if(c->it != NULL){
		ialloc_free_dont_need(c->it, item_totalsz(c->it));
	    	success = 1;
	    }
	    c->req_done = 1;
        }

        /* calculate response length */
        rsl = sizeof(protocol_binary_response_delete);

        /* check whether we have sufficient tx buffer space available */
        if (MAX_MSGSIZE - c->tx_len < rsl) {
            /* not enough space, start sending out */
            return 1;
        }

        /* build response header */
        res->response.magic = PROTOCOL_BINARY_RES;
        res->response.opcode = PROTOCOL_BINARY_CMD_DELETE;
        res->response.keylen = htons(0);
        res->response.extlen = 4;
        res->response.datatype = 0;
        res->response.bodylen = htonl(0);
        if (success) {
            res->response.status = htons(PROTOCOL_BINARY_RESPONSE_SUCCESS);
        } else {
            res->response.status = htons(PROTOCOL_BINARY_RESPONSE_KEY_ENOENT);
        }
        res->response.opaque = req->request.opaque;
    } else {
        fprintf(stderr, "Closing connection on invalid request opcode: %x\n",
                op);
        conn_close(sc, c);
        return -1;
    }

    /* done with request */
    c->rx_off += rql;
    c->tx_len += rsl;
    c->req_done = 0;
    return 0;
}

static void conn_events(struct item_allocator *ia, ssctx_t sc, int epfd,
        struct connection *c, uint32_t events)
{
    int ret;

    assert(ia != NULL);

    /* connection */
    if ((events & ~(SS_EPOLLIN | SS_EPOLLOUT))) {
        fprintf(stderr, "Closing connection on EP error\n");
        perror("epoll");
	conn_close(sc, c);
        return;
    }

    /* more data available to be received */
    if ((events & SS_EPOLLIN) == SS_EPOLLIN) {
        /* receive requests */
        if (conn_recv(sc, epfd, c) != 0) {
            return;
        }
    } else if ((events & SS_EPOLLOUT) == SS_EPOLLOUT) {
        assert(c->tx_off < c->tx_len);

        /* send out tx buffer */
        if (conn_send(sc, epfd, c) != 0) {
            return;
        }
    }

    /* process request(s) */
    while (conn_req_iscomp(c)) {
        assert(ia != NULL);
        ret = conn_req_process(sc, ia, c);
        if (ret == -1) {
            printf("request failed!\n");
	    /* request failed and connection was closed */
            return;
        } else if (c->tx_len != 0) {
            /* not enough tx buffer space, send out */
            if (conn_send(sc, epfd, c) != 0) {
                return;
            }
        }
    }

    /* move up rx buffer contents, if any */
    if (c->rx_off != 0) {
        if (c->rx_off == c->rx_len) {
            c->rx_len = 0;
        } else {
            memmove(c->rx_buf, c->rx_buf + c->rx_off, c->rx_len - c->rx_off);
            c->rx_len = c->rx_len - c->rx_off;
        }
        c->rx_off = 0;
    }

    conn_upepoll(sc, epfd, c, 0);
}

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


static struct item_allocator **iallocs;
static volatile size_t n_ready = 0;

static void *processing_thread(void *data)
{
    int i, epfd, lfd, n;
    struct item_allocator ia;
    size_t total_reqs = 0, total_clean = 0;
    static uint16_t qcounter;
    uint16_t q;
    ssctx_t sc;
    ss_epev_t *evs;
    struct connection *c;

    ialloc_init_allocator(&ia);
    q = __sync_fetch_and_add(&qcounter, 1);

    network_init(&epfd, &lfd, &sc, q);

    evs = calloc(EPOLL_EVENTS, sizeof(*evs));
    if (evs == NULL) {
        fprintf(stderr, "Allocating event buffer failed\n");
        abort();
    }


    iallocs[q] = &ia;
    __sync_fetch_and_add(&n_ready, 1);

    printf("Worker starting\n");

    while (1) {
        if ((n = ss_epoll_wait(sc, epfd, evs, EPOLL_EVENTS, 1)) < 0) {
            perror("ss_epoll_wait failed");
            abort();
        }
        for (i = 0; i < n; i++) {
            c = evs[i].data.ptr;
            if (c != NULL)
              util_prefetch0(c);
        }

        for (i = 0; i < n; i++) {
            c = evs[i].data.ptr;
            if (c == NULL) {
                /* the listening socket */
                if ((evs[i].events != SS_EPOLLIN)) {
                    fprintf(stderr, "Error on listening socket\n");
                    abort();
                }

                accept_connections(sc, epfd, lfd, q);
            } else {
                conn_events(&ia, sc, epfd, c, evs[i].events);
            }
        }

#ifndef DEL_TEST
        total_clean += clean_log(&ia, 1);
#endif
#if 0
        if (total_reqs / 100000 != (total_reqs + had_pkts) / 100000) {
            printf("%u: total=%10zu  clean=%10zu\n", q, total_reqs, total_clean);
        }
#endif
        total_reqs += n;
    }

    return 0;
}

static void maintenance(void)
{
    size_t i, n;

    n = settings.numcores;
    while (1) {
        for (i = 0; i < n; i++) {
            ialloc_maintenance(iallocs[i]);
        }
        usleep(10);
    }
}


int main(int argc, char *argv[])
{
#ifdef USE_MTCP
    int ret;
#endif
    char name[32];
    unsigned num_threads, i;
    pthread_t *pts;

    if (settings_init(argc, argv) != 0) {
      return EXIT_FAILURE;
    }
    num_threads = settings.numcores;

#ifdef USE_MTCP
    if ((ret = mtcp_init(settings.config_file)) != 0) {
        fprintf(stderr, "mtcp_init failed: %d\n", ret);
        return EXIT_FAILURE;
    }
#endif

    printf("initiating hash table\n");
    hasht_init();
    printf("initiating ialloc\n");
    ialloc_init();

    iallocs = calloc(num_threads, sizeof(*iallocs));
    pts = calloc(num_threads, sizeof(*pts));
    for (i = 0; i < num_threads; i++) {
        if (pthread_create(pts + i, NULL, processing_thread, NULL)) {
            fprintf(stderr, "pthread_create failed\n");
            return EXIT_FAILURE;
        }

        snprintf(name, sizeof(name), "flexkvs-w%u", i);
        pthread_setname_np(pts[i], name);
    }

    while (n_ready < num_threads);
    printf("Starting maintenance\n");
    fflush(stdout);
    while (1) {
        maintenance();
    }
    return 0;
}
