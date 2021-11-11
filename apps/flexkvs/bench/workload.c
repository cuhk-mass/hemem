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

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

#include "benchmark.h"
#include "rng.h"

static struct key *generate_keys(struct rng *rng, size_t n, size_t ks);
static void distribute_uniform(struct key *keys, size_t n);
static void distribute_zipf(struct key *keys, size_t n, double s);
static struct key *draw_key(struct key *keys, size_t n, struct rng *rng);


void workload_init(struct workload *wl)
{
    struct rng key_rng;

    /* prepare rngs and distributions for keys */
    rng_init(&key_rng, settings.key_seed);
    rng_init(&wl->op_rng, settings.op_seed);
    wl->keys = generate_keys(&key_rng, settings.keynum, settings.keysize);
    wl->keys_num = settings.keynum;
    if (settings.keydist == DIST_UNIFORM) {
        distribute_uniform(wl->keys, wl->keys_num);
    } else {
        distribute_zipf(wl->keys, wl->keys_num, settings.keydistparams.zipf.s);
    }
}

void workload_adjust(struct workload *wl, struct workload *wl2)
{
    struct rng key_rng;

    /* prepare rngs and distributions for keys */
    wl->keys = generate_keys(&key_rng, settings.keynum * (1-DEL_RATIO), settings.keysize);
    wl2->keys = generate_keys(&key_rng, settings.keynum * (DEL_RATIO), settings.keysize);
    wl->keys_num = settings.keynum * (1-DEL_RATIO);
    wl2->keys_num = settings.keynum * DEL_RATIO;
    if (settings.keydist == DIST_UNIFORM) {
        distribute_uniform(wl->keys, wl->keys_num);
        distribute_uniform(wl2->keys, wl2->keys_num);
    } else {
        distribute_zipf(wl->keys, wl->keys_num, settings.keydistparams.zipf.s);
        distribute_zipf(wl2->keys, wl2->keys_num, settings.keydistparams.zipf.s);
    }
}


void workload_core_init(struct workload *wl, struct workload_core *wc)
{
    rng_init(&wc->rng,
            ((uint64_t) rng_gen32(&wl->op_rng) << 16) ^ rng_gen32(&wl->op_rng));
}

void workload_op(struct workload *wl, struct workload_core *wc, struct key **k,
        enum workload_op *op)
{
    if (rng_gend(&wc->rng) <= settings.get_prob) {
        *op = WL_OP_GET;
    } else {
        *op = WL_OP_SET;
    }
    *k = draw_key(wl->keys, wl->keys_num, &wc->rng);
}

/** Generate n keys (no distribution set) */
static struct key *generate_keys(struct rng *rng, size_t n, size_t keysz)
{
    size_t i;
    struct key *k = malloc(n * sizeof(*k) + n * keysz);
    uint8_t *keys = (uint8_t *) (k + n);
    if (k == NULL) {
        abort();
    }
    for (i = 0; i < n; i++) {
        rng_gen(rng, keys, keysz);
        k[i].key = keys;
        k[i].keylen = keysz;
        keys += keysz;
    }

    // TODO: Fix duplicates
    return k;
}

/** Distribute keys uniformly */
static void distribute_uniform(struct key *keys, size_t n)
{
    size_t i;
    double p = (double) 1 / (double) n;
    double sum = 0;
    for (i = 0; i < n; i++) {
        sum += p;
        keys[i].cdf = sum;
    }
}

/** Distribute keys according to zipf distribution with parameter s. */
static void distribute_zipf(struct key *keys, size_t n, double s)
{
    size_t i;
    double c = 0;
    double sum = 0;

    for (i = 0; i < n; i++) {
        c += 1 / pow(i + 1, s);
    }

    for (i = 0; i < n; i++) {
        sum += 1 / pow(i + 1, s) / c;
        keys[i].cdf = sum;
    }
}

/** Binary search helper (returns -1 to go left, 0 if found, 1 to go right). */
static inline int key_in_range(struct key *keys, size_t n, size_t i, double x)
{
    double cdf = keys[i].cdf;
    //printf("key in range n=%lu i=%lu x=%lf cdf=%lf\n", n, i, x, cdf);
    if (x < cdf) {
        if (i == 0) {
            return 0;
        } else {
            return (x <= keys[i - 1].cdf ? -1 : 0);
        }
    } else if (x > cdf) {
        if (i == n - 1) {
            // Already at right-most value (could happen due to rounding errors
            // when generating the distribution)
            return 0;
        } else {
            return 1;
        }
    } else {
        return 0;
    }
}

/** Draw a key at random, according to the configured distribution. */
static struct key *draw_key(struct key *keys, size_t n, struct rng *rng)
{
    double x = rng_gend(rng);
    size_t l, r, mid = 0;
    int res;
    //printf("draw_key(n=%lu)\n", n);

    l = 0;
    r = n - 1;
    while (l < r) {
        mid = (l + r) / 2;
        res = key_in_range(keys, n, mid, x);
        if (res < 0) {
            r = mid - 1;
        } else if (res > 0) {
            l = mid + 1;
        } else {
            break;
        }
    }

    return keys + mid;
}
