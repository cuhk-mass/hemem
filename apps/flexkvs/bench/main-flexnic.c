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
#include <unistd.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <string.h>
#include <pthread.h>
#include <signal.h>
#include <netdb.h>
#include <errno.h>
#include <assert.h>
#include <locale.h>
#include <inttypes.h>

#include <tas_ll.h>
#include <protocol_binary.h>

#include "benchmark.h"
#include "../../include/utils_circ.h"

#ifdef USE_MTCP
# include <mtcp_api.h>
# include <mtcp_epoll.h>
#else
# include <sys/epoll.h>
#endif

#define MIN(a,b) ((b) < (a) ? (b) : (a))

#define CONN_DEBUG(c, co, x...) do { } while (0)
/*#define CONN_DEBUG(c, co, x...) \
    do { printf("%d.%d: ", (int) c->id, co->fd); \
         printf(x); } while (0)*/

#define PRINT_STATS
#ifdef PRINT_STATS
#   define STATS_ADD(c, f, n) c->f += n
#else
#   define STATS_ADD(c, f, n) do { } while (0)
#endif

#define HIST_START_US 0
#define HIST_BUCKET_US 1
#define HIST_BUCKETS 4096
#define BUFSIZE 2048
#define SEPCONNS 1

enum conn_state {
    CONN_CLOSED = 0,
    CONN_CONNECTING = 1,
    CONN_OPEN = 2,
};

enum benchmark_phase {
    BENCHMARK_INIT,
    BENCHMARK_WARMUP,
    BENCHMARK_RUNNING,
    BENCHMARK_COOLDOWN,
    BENCHMARK_DONE,
};

struct settings settings;
static struct workload workload;
static volatile enum benchmark_phase phase;
static volatile uint16_t init_count = 0;

struct shared_conn {
    struct flextcp_obj_connection c;
    uint32_t id;
    enum conn_state state;
} __attribute__((aligned(64)));

struct connection {
    struct shared_conn *sc;
    uint32_t pending;
    int need_bump;
#ifdef PRINT_STATS
    uint64_t cnt;
#endif
};

struct core {
    struct flextcp_context ctx;
    struct connection *conns;
#ifdef PRINT_STATS
    uint64_t tx_get;
    uint64_t tx_set;
    uint64_t rx_get;
    uint64_t rx_set;
    uint64_t rx_success;
    uint64_t rx_fail;
    uint32_t *hist;
#endif
    struct workload_core wlc;
    int id;
    pthread_t pthread;
} __attribute__((aligned(64)));

static inline uint64_t get_nanos(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return (uint64_t) ts.tv_sec * 1000 * 1000 * 1000 + ts.tv_nsec;
}

static inline void record_latency(struct core *c, uint64_t nanos)
{
    size_t bucket = ((nanos / 1000) - HIST_START_US) / HIST_BUCKET_US;
    if (bucket >= HIST_BUCKETS) {
        bucket = HIST_BUCKETS - 1;
    }
    __sync_fetch_and_add(&c->hist[bucket], 1);
}

#ifdef PRINT_STATS
static inline uint64_t read_cnt(uint64_t *p)
{
  uint64_t v = *p;
  __sync_fetch_and_sub(p, v);
  return v;
}
#endif

static inline int set_request(struct core *c, struct connection *co,
        struct key *k, uint32_t opaque)
{
    struct flextcp_obj_connection *oc = &co->sc->c;
    protocol_binary_request_set set;
    protocol_binary_request_header *req = &set.message.header;
    struct flextcp_obj_handle oh;
    void *buf_1, *buf_2;
    size_t len_1, len_2, req_len;

    /* allocate object size */
    req_len = sizeof(set) + settings.valuesize;
    if (flextcp_obj_connection_tx_alloc(oc, k->keylen, req_len, &buf_1, &len_1,
                &buf_2, &oh) != 0)
    {
        return -1;
    }
    req_len += k->keylen;
    len_2 = req_len - len_1;

    /* prepare header */
    req->request.magic = PROTOCOL_BINARY_REQ;
    req->request.opcode = PROTOCOL_BINARY_CMD_SET;
    req->request.keylen = 0;
    req->request.extlen = 8;
    req->request.datatype = 0;
    req->request.reserved = htons(c->id);
    req->request.bodylen = htonl(8 + settings.valuesize);
    req->request.opaque = opaque;
    req->request.cas = 0;
    set.message.body.flags = 0;
    set.message.body.expiration = 0;

    /* write key and header to object */
    split_write(k->key, k->keylen, buf_1, len_1, buf_2, len_2, 0);
    split_write(&set, sizeof(set), buf_1, len_1, buf_2, len_2, k->keylen);

    flextcp_obj_connection_tx_send(&c->ctx, oc, &oh);
    return 0;
}

static inline int get_request(struct core *c, struct connection *co,
        struct key *k, uint32_t opaque)
{
    struct flextcp_obj_connection *oc = &co->sc->c;
    protocol_binary_request_get get;
    protocol_binary_request_header *req = &get.message.header;
    struct flextcp_obj_handle oh;
    void *buf_1, *buf_2;
    size_t len_1, len_2, req_len;

    /* allocate object size */
    req_len = sizeof(get);
    if (flextcp_obj_connection_tx_alloc(oc, k->keylen, req_len, &buf_1, &len_1,
                &buf_2, &oh) != 0)
    {
        return -1;
    }
    req_len += k->keylen;
    len_2 = req_len - len_1;

    /* prepare header */
    req->request.magic = PROTOCOL_BINARY_REQ;
    req->request.opcode = PROTOCOL_BINARY_CMD_GET;
    req->request.keylen = 0;
    req->request.extlen = 0;
    req->request.datatype = 0;
    req->request.reserved = htons(c->id);
    req->request.bodylen = htonl(0);
    req->request.opaque = opaque;
    req->request.cas = 0;

    /* write key and header to object */
    split_write(k->key, k->keylen, buf_1, len_1, buf_2, len_2, 0);
    split_write(&get, sizeof(get), buf_1, len_1, buf_2, len_2, k->keylen);

    flextcp_obj_connection_tx_send(&c->ctx, oc, &oh);
    return 0;
}

static inline int parse_response(struct core *c, void *buf_1, size_t len_1,
        void *buf_2, size_t len_2, uint8_t dstlen, enum workload_op *op,
        uint16_t *err, uint32_t *opaque)
{
    protocol_binary_response_header res;
    int cn = c->id;
    size_t reqlen;

    /* check that whole response header is in the response */
    if (len_1 + len_2 < dstlen + sizeof(res)) {
        return -1;
    }

    /* read header */
    split_read(&res, sizeof(res), buf_1, len_1, buf_2, len_2, dstlen);

    /* check magic */
    if (res.response.magic != PROTOCOL_BINARY_RES) {
        fprintf(stderr, "[%d] invalid magic on response: %x\n", cn,
                res.response.magic);
        return -1;
    }

    /* check length of whole response */
    reqlen = dstlen + sizeof(res) + ntohl(res.response.bodylen);
    if (len_1 + len_2 != reqlen) {
        fprintf(stderr, "[%d] invalid response len (got %zu, expected %zu)\n",
                cn, len_1 + len_2, reqlen);
        return -1;
    }

    if (res.response.opcode == PROTOCOL_BINARY_CMD_GET) {
        *op = WL_OP_GET;
    } else if (res.response.opcode == PROTOCOL_BINARY_CMD_SET) {
        *op = WL_OP_SET;
    } else {
        fprintf(stderr, "[%d] conn_recv: unknown opcode=%x\n", cn,
                res.response.opcode);
    }
    *err = ntohs(res.response.status);
    *opaque = res.response.opaque;

    return 0;
}

/* opening all connections and wait for them to be established */
static void open_conns(struct flextcp_context *ctx, struct shared_conn *scs,
        int n)
{
    struct shared_conn *sc;
    uint32_t i;
    int ret, j;
    struct flextcp_event evs[8];


    /* alloc conn structs and initiate connections */
    for (i = 0; i < n; i++) {
        sc = &scs[i];
        if (flextcp_obj_connection_open(ctx, &sc->c, settings.dstip,
                    settings.dstport, FLEXTCP_CONNECT_OBJNOHASH) != 0)
        {
            fprintf(stderr, "open_conns: flextcp_obj_connection_open failed\n");
            abort();
        }

        sc->state = CONN_CONNECTING;
    }

    /* wait for connections to be established */
    for (i = 0; i < n; i++) {
        while (scs[i].state == CONN_CONNECTING) {
            /* get events */
            if ((ret = flextcp_context_poll(ctx, 8, evs)) < 0) {
                fprintf(stderr, "open_conns: flextcp_context_poll "
                        "failed\n");
                abort();
            }

            /* check events */
            for (j = 0; j < ret; j++) {
                if (evs[j].event_type != FLEXTCP_EV_OBJ_CONN_OPEN) {
                    fprintf(stderr, "open_conns: unexpected event type "
                            "%u\n", evs[j].event_type);
                    continue;
                }

                if (evs[j].ev.obj_conn_open.status != 0) {
                    fprintf(stderr, "open_conns: copen connection failed %d\n",
                            evs[j].event_type);
                    abort();
                }

                /* we only exepect events for connections that are currently
                 * connecting. */
                sc = (struct shared_conn *) evs[j].ev.obj_conn_open.conn;
                if (sc->state != CONN_CONNECTING) {
                    fprintf(stderr, "open_conns: event on ready connection\n");
                    abort();
                }

                sc->state = CONN_OPEN;
            }
        }
    }
}

/* wait for all connections to be established */
static void load_keys(struct core *c)
{
    int cn, ret, l, pe, next_bump = 0;
    size_t i, pending;
    struct key *k;
    struct connection *co;
    struct shared_conn *sco;
    struct flextcp_event evs[8];
    uint32_t opaque;
    uint16_t err;
    enum workload_op op;

    cn = c->id;
    i = cn;
    pending = 0;
    while (i < workload.keys_num || pending > 0) {
        /* send out requests on connections that can transmit more */
        for (l = 0; l < settings.conns && i < workload.keys_num; l++) {
            co = &c->conns[l];
            if (co->pending >= settings.pending) {
                continue;
            }

            k = &workload.keys[i];
            if (set_request(c, co, k, cn) != 0) {
                continue;
            }

            co->need_bump = 1;
            co->pending++;
            pending++;
            i += settings.threads;
        }

        /* bump connections that need it */
        for (l = 0; l < settings.conns; l++) {
            co = &c->conns[next_bump];
            if (!co->need_bump) {
                continue;
            }

            if (flextcp_obj_connection_bump(&c->ctx, &co->sc->c) != 0) {
                break;
            }
            co->need_bump = 0;

            next_bump++;
            if (next_bump >= settings.conns) {
                next_bump -= settings.conns;
            }
        }

        /* poll for events */
        if ((ret = flextcp_context_poll(&c->ctx, 8, evs)) < 0) {
            fprintf(stderr, "[%d] load_keys flextcp_context_poll failed\n", cn);
            abort();
        }

        /* process events */
        for (l = 0; l < ret; l++) {
            if (evs[l].event_type != FLEXTCP_EV_OBJ_CONN_RECEIVED) {
                fprintf(stderr, "[%d] load keys unexpected event: %u\n", cn,
                        evs[l].event_type);
                continue;
            }

            sco = (struct shared_conn *) evs[l].ev.obj_conn_received.conn;
            co = &c->conns[sco->id];

            pe = parse_response(c, evs[l].ev.obj_conn_received.buf_1,
                    evs[l].ev.obj_conn_received.len_1,
                    evs[l].ev.obj_conn_received.buf_2,
                    evs[l].ev.obj_conn_received.len_2,
                    evs[l].ev.obj_conn_received.dstlen,
                    &op, &err, &opaque);
            flextcp_obj_connection_rx_done(&c->ctx, &sco->c,
                    &evs[l].ev.obj_conn_received.handle);
            co->need_bump = 1;
            if (pe != 0) {
                fprintf(stderr, "[%d] invalid response on conn %p\n", cn, co);
                continue;
            }

            if (err != 0) {
                fprintf(stderr, "[%d] request failed on conn %p\n", cn, co);
            }
            assert(opaque == cn);
            pending--;
            co->pending--;
        }
    }
}

static void *thread_run(void *arg)
{
    struct core *c = arg;
    struct connection *co;
    int cn, ret, l, pe, next_bump = 0;
    struct key *k;
    struct shared_conn *sco;
    struct flextcp_event evs[32];
    uint32_t opaque;
    uint16_t err;
    enum workload_op op;

    cn = c->id;

    /* pre-load keys */
    printf("[%d] Preloading keys...\n", cn);
    load_keys(c);
    printf("[%d] Preloaded keys...\n", cn);
    __sync_fetch_and_add(&init_count, 1);

    /* wait until we start running */
    while (phase != BENCHMARK_RUNNING) {
        pthread_yield();
    }
    printf("[%d] Start running...\n", cn);

    while (1) {
        /* send out requests on connections that can transmit more */
        for (l = 0; l < settings.conns; l++) {
            co = &c->conns[l];
            if (co->pending >= settings.pending) {
                continue;
            }

            workload_op(&workload, &c->wlc, &k, &op);

            /* assign a time stamp */
            opaque = get_nanos();

            /* assemble request */
            if (op == WL_OP_GET) {
                get_request(c, co, k, opaque);
                STATS_ADD(c, tx_get, 1);
            } else {
                set_request(c, co, k, opaque);
                STATS_ADD(c, tx_set, 1);
            }

            co->need_bump = 1;
            co->pending++;
        }

        /* bump connections that need it */
        for (l = 0; l < settings.conns; l++) {
            co = &c->conns[next_bump];
            if (!co->need_bump) {
                continue;
            }

            if (flextcp_obj_connection_bump(&c->ctx, &co->sc->c) != 0) {
                break;
            }
            co->need_bump = 0;

            next_bump++;
            if (next_bump >= settings.conns) {
                next_bump -= settings.conns;
            }
        }

        /* poll for events */
        if ((ret = flextcp_context_poll(&c->ctx, 32, evs)) < 0) {
            fprintf(stderr, "[%d] thread_run flextcp_context_poll failed\n", cn);
            abort();
        }

        /* process events */
        for (l = 0; l < ret; l++) {
            if (evs[l].event_type != FLEXTCP_EV_OBJ_CONN_RECEIVED) {
                fprintf(stderr, "[%d] load keys unexpected event: %u\n", cn,
                        evs[l].event_type);
                continue;
            }

            sco = (struct shared_conn *) evs[l].ev.obj_conn_received.conn;
            co = &c->conns[sco->id];

            pe = parse_response(c, evs[l].ev.obj_conn_received.buf_1,
                    evs[l].ev.obj_conn_received.len_1,
                    evs[l].ev.obj_conn_received.buf_2,
                    evs[l].ev.obj_conn_received.len_2,
                    evs[l].ev.obj_conn_received.dstlen,
                    &op, &err, &opaque);
            flextcp_obj_connection_rx_done(&c->ctx, &sco->c,
                    &evs[l].ev.obj_conn_received.handle);
            co->need_bump = 1;
            if (pe != 0) {
                fprintf(stderr, "[%d] invalid response on conn %p\n", cn, co);
                continue;
            }

            record_latency(c, (uint32_t) get_nanos() - opaque);

            STATS_ADD(co, cnt, 1);
            if (op == WL_OP_GET) {
                STATS_ADD(c, rx_get, 1);
            } else {
                STATS_ADD(c, rx_set, 1);
            }
            if (err == 0) {
                STATS_ADD(c, rx_success, 1);
            } else {
                STATS_ADD(c, rx_fail, 1);
            }
            co->pending--;
        }
    }

    return NULL;
}

static inline void hist_fract_buckets(uint32_t *hist, uint64_t total,
        double *fracs, size_t *idxs, size_t num)
{
    size_t i, j;
    uint64_t sum = 0, goals[num];
    for (j = 0; j < num; j++) {
        goals[j] = total * fracs[j];
    }
    for (i = 0, j = 0; i < HIST_BUCKETS && j < num; i++) {
        sum += hist[i];
        for (; j < num && sum >= goals[j]; j++) {
            idxs[j] = i;
        }
    }
}

static inline int hist_value(size_t i)
{
    if (i == HIST_BUCKETS - 1) {
        return -1;
    }

    return i * HIST_BUCKET_US + HIST_START_US;
}

int main(int argc, char *argv[])
{
    int i, j, num_threads;
    struct core *cs;
    struct shared_conn *scs;
    uint64_t t_prev, t_cur;
    long double *ttp, tp, tp_total;
    uint32_t *hist, hx;
    uint64_t msg_total;
    double fracs[6] = { 0.5, 0.9, 0.95, 0.99, 0.999, 0.9999 };
    size_t fracs_pos[sizeof(fracs) / sizeof(fracs[0])];

    setlocale(LC_NUMERIC, "");

    /* parse settings from command line */
    init_settings(&settings);
    if (parse_settings(argc, argv, &settings) != 0) {
        print_usage();
        return EXIT_FAILURE;
    }
    num_threads = settings.threads;

    /* initialize stack */
    if (flextcp_init() != 0) {
        fprintf(stderr, "flextcp_init failed\n");
        return EXIT_FAILURE;
    }

    /* initialize workload */
    workload_init(&workload);

    /* allocate core structs */
    assert(sizeof(*cs) % 64 == 0);
    assert(sizeof(*scs) % 64 == 0);
    cs = calloc(num_threads, sizeof(*cs));
#ifdef SEPCONNS
    scs = calloc(settings.conns * num_threads, sizeof(*scs));
#else
    scs = calloc(settings.conns, sizeof(*scs));
#endif
    ttp = calloc(num_threads, sizeof(*ttp));
    hist = calloc(HIST_BUCKETS, sizeof(*hist));
    if (cs == NULL || scs == NULL || ttp == NULL || hist == NULL) {
        fprintf(stderr, "allocation failed failed\n");
        return EXIT_FAILURE;
    }

    /* initialize cores */
    for (i = 0; i < num_threads; i++) {
        if (flextcp_context_create(&cs[i].ctx) != 0) {
            fprintf(stderr, "creating context %d failed\n", i);
            return EXIT_FAILURE;
        }

        cs[i].id = i;
        workload_core_init(&workload, &cs[i].wlc);

        cs[i].conns = calloc(settings.conns, sizeof(*cs[i].conns));
        if (cs[i].conns == NULL) {
            fprintf(stderr, "allocating context %d connections failed\n", i);
            return EXIT_FAILURE;
        }

        if ((cs[i].hist = calloc(HIST_BUCKETS, sizeof(*cs[i].hist))) == NULL) {
            fprintf(stderr, "allocating histogram %d failed\n", i);
            return EXIT_FAILURE;
        }

        for (j = 0; j < settings.conns; j++) {
#ifdef SEPCONNS
            scs[i * settings.conns + j].id = j;
            cs[i].conns[j].sc = &scs[i * settings.conns + j];
#else
            scs[j].id = j;
            cs[i].conns[j].sc = &scs[j];
#endif
        }
    }
#ifdef SEPCONNS
    open_conns(&cs[0].ctx, scs, settings.conns * num_threads);
#else
    open_conns(&cs[0].ctx, scs, settings.conns);
#endif
    printf("connections opened\n");

    phase = BENCHMARK_INIT;

    for (i = 0; i < num_threads; i++) {
        cs[i].id = i;
        workload_core_init(&workload, &cs[i].wlc);
        if (pthread_create(&cs[i].pthread, NULL, thread_run, cs + i)) {
            fprintf(stderr, "pthread_create failed\n");
            return EXIT_FAILURE;
        }
    }

    while (init_count < num_threads) {
        pthread_yield();
    }
    printf("Preloading completed\n");
    phase = BENCHMARK_RUNNING;



    t_prev = get_nanos();
    while (1) {
        sleep(1);
        t_cur = get_nanos();
        tp_total = 0;
        msg_total = 0;
        for (i = 0; i < num_threads; i++) {
            tp = cs[i].rx_success;
            tp /= (double) (t_cur - t_prev) / 1000000000.;
            ttp[i] = tp;
            tp_total += tp;

            for (j = 0; j < HIST_BUCKETS; j++) {
                hx = cs[i].hist[j];
                msg_total += hx;
                hist[j] += hx;
            }
        }

        hist_fract_buckets(hist, msg_total, fracs, fracs_pos,
                sizeof(fracs) / sizeof(fracs[0]));


        printf("TP: total=%'.2Lf mops  50p=%d us  90p=%d us  95p=%d us  "
                "99p=%d us  99.9p=%d us  99.99p=%d us  ",
                tp_total / 1000000.,
                hist_value(fracs_pos[0]), hist_value(fracs_pos[1]),
                hist_value(fracs_pos[2]), hist_value(fracs_pos[3]),
                hist_value(fracs_pos[4]), hist_value(fracs_pos[5]));

#ifdef PRINT_PERCORE
        for (i = 0; i < num_threads; i++) {
            printf("core[%d]=%'.2Lf mbps  ", i,
                    ttp[i] * message_size * 8 / 1000000.);
        }
#endif
        printf("\n");
#ifdef PRINT_STATS
        printf("stats:\n");
        for (i = 0; i < num_threads; i++) {
            printf("    core %2d: (tg=%"PRIu64", ts=%"PRIu64", rg=%"PRIu64
                ", rs=%"PRIu64", rS=%"PRIu64", rF=%"PRIu64")\n", i,
                read_cnt(&cs[i].tx_get), read_cnt(&cs[i].tx_set),
                read_cnt(&cs[i].rx_get), read_cnt(&cs[i].rx_set),
                read_cnt(&cs[i].rx_success), read_cnt(&cs[i].rx_fail));
        }
        for (i = 0; i < num_threads; i++) {
            for (j = 0; j < settings.conns; j++) {
                printf("      t[%d].conns[%d]:  pend=%u  "
                        "  cnt=%"PRIu64"\n",
                        i, j, cs[i].conns[j].pending, cs[i].conns[j].cnt);
            }
        }
#endif

        fflush(stdout);
        memset(hist, 0, sizeof(*hist) * HIST_BUCKETS);

        t_prev = t_cur;
    }

#ifdef USE_MTCP
    mtcp_destroy();
#endif
}
