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

#include <stdbool.h>

#include "rng.h"

#define DEL_RATIO 0.7

enum key_dist {
    DIST_UNIFORM,
    DIST_ZIPF,
};

struct settings {
    uint32_t dstip;
    uint16_t dstport;
    uint16_t threads;
    uint16_t conns;
    uint16_t pending;

    uint32_t keynum;
    union {
        struct {
            double s;
        } zipf;
    } keydistparams;
    double get_prob;
    enum key_dist keydist;
    uint64_t key_seed;
    uint64_t op_seed;
    uint32_t request_gap;
    uint32_t warmup_time;
    uint32_t cooldown_time;
    uint32_t run_time;
    uint16_t keysize;
    uint16_t valuesize;

    uint8_t batchsize;

    bool keybased;
};

struct key {
    void *key;
    size_t keylen;
    double cdf;
};

struct workload {
    struct rng op_rng;
    struct key *keys;
    size_t keys_num;
};

struct workload_core {
    struct rng rng;
};

enum workload_op {
    WL_OP_GET,
    WL_OP_SET,
    WL_OP_DELETE
};

extern struct settings settings;

enum error_ids {
    ERR_SUCCESS,
    ERR_KEY_ENOENT,
    ERR_KEY_EEXIST,
    ERR_E2BIG,
    ERR_EINVAL,
    ERR_NOT_STORED,
    ERR_DELTA_BADVAL,
    ERR_UNKNOWN_CMD,
    ERR_ENOMEM,
    ERR_OTHER,
    ERR_MAX,
};

void print_usage(void);
void init_settings(struct settings *s);
int parse_settings(int argc, char *argv[], struct settings *s);

bool trace_open(const char *path);
bool trace_init(void);
void trace_request_get(uint8_t thread, struct key *key, uint16_t id);
void trace_request_set(uint8_t thread, struct key *key, uint32_t valsz,
        uint16_t id);
void trace_response(uint8_t thread, uint16_t id, uint8_t err);
void trace_flush(uint8_t thread);

void workload_init(struct workload *wl);
void workload_adjust(struct workload *wl, struct workload *wl2);
void workload_core_init(struct workload *wl, struct workload_core *wc);
void workload_op(struct workload *wl, struct workload_core *wc, struct key **k,
        enum workload_op *op);
