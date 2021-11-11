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
#include <sys/mman.h>
#include <pthread.h>
#include <memory.h>

#include "iokvs.h"


#define SF_INACTIVE 1
#define SF_CLEANED 4

struct segment_header {
    void *data;
    struct segment_header *next;
    struct segment_header *prev;
    uint32_t offset;
    uint32_t freed;
    uint32_t size;
    uint32_t flags;
};

static struct segment_header *free_segments;
static pthread_spinlock_t segalloc_lock;
static void *seg_base;
static struct segment_header **seg_headers;
static size_t seg_alloced;

#ifdef BARRELFISH
void *mem_base;
uint64_t mem_base_phys;
#endif

void ialloc_init(void)
{
    pthread_spin_init(&segalloc_lock, 0);
    free_segments = NULL;
    size_t total;

    seg_alloced = 0;
    total = settings.segsize * settings.segmaxnum;
    printf("Allocating %lu bytes\n", (long unsigned int) total);

#ifdef BARRELFISH
    {
        errval_t r;
        struct capref cap;
        struct frame_identity id;

        r = myt_alloc_map(VREGION_FLAGS_READ_WRITE, total, &seg_base, &cap);
        if (err_is_fail(r)) {
            USER_PANIC_ERR(r, "Preallocating failed");
        }

        r = invoke_frame_identify(cap, &id);
        if (err_is_fail(r)) {
            USER_PANIC_ERR(r, "identify failed");
        }

        mem_base = seg_base;
        mem_base_phys = id.base;
    }
#else
    //if ((seg_base = mmap(NULL, total, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB | MAP_POPULATE,
    //if ((seg_base = mmap(NULL, total, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB,
    //                -1, 0)) == MAP_FAILED)
    if ((seg_base = mmap(NULL, total, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS,
                    -1, 0)) == MAP_FAILED)
    {
        perror("mmap() of segments base failed");
        abort();
    }
    printf("seg base %p through %p\n", seg_base, seg_base + total);
#endif
    if ((seg_headers = calloc(settings.segmaxnum, sizeof(*seg_headers))) ==
            NULL)
    {
        perror("Allocating segment header array failed");
        abort();
    }
}

static struct segment_header *segment_alloc(void)
{
    struct segment_header *h = NULL;
    void *data;
    size_t i, segsz;

    /* Try to get a segment from the freelist */
    if (free_segments != NULL) {
        pthread_spin_lock(&segalloc_lock);
        if (free_segments != NULL) {
            h = free_segments;
            free_segments = h->next;
        }
        pthread_spin_unlock(&segalloc_lock);

        if (h != NULL) {
            goto init_h;
        }
    }

    /* Check if there are still unallocated segments (note: unlocked) */
    i = seg_alloced;
    if (i >= settings.segmaxnum) {
        pthread_spin_unlock(&segalloc_lock);
        return NULL;
    }

    /* If there is a possiblity that there are still unallocated segments, let's
     * go for it. */
    pthread_spin_lock(&segalloc_lock);
    i = seg_alloced;
    if (i >= settings.segmaxnum) {
        pthread_spin_unlock(&segalloc_lock);
        return NULL;
    }

    seg_alloced++;
    pthread_spin_unlock(&segalloc_lock);

    segsz = settings.segsize;
    data = (void *) ((uintptr_t) seg_base + segsz * i);
#ifndef BARRELFISH
    if (mprotect(data, settings.segsize, PROT_READ | PROT_WRITE) != 0) {
        perror("mprotect failed");
        /* TODO: check what to do here */
        return NULL;
    }
#endif

    h = malloc(sizeof(*h));
    if (h == NULL) {
        /* TODO: check what to do here */
        return NULL;
    }
    seg_headers[i] = h;

    h->size = segsz;
    h->data = data;
    //printf("allocating segment with size %zu and data at %p\n", segsz, data);
init_h:
    h->offset = 0;
    h->flags = 0;
    h->freed = 0;
    return h;
}

static inline struct segment_header *segment_from_part(void *data)
{
    size_t i = ((uintptr_t) data - (uintptr_t) seg_base) / settings.segsize;
    assert(i < settings.segmaxnum);
    return seg_headers[i];
}

static void segment_free(struct segment_header *h)
{
    pthread_spin_lock(&segalloc_lock);
    h->offset = 0;
    h->next = free_segments;
    free_segments = h;
    pthread_spin_unlock(&segalloc_lock);
}

static void segment_item_free(struct segment_header *h, size_t total)
{
    if (h->size != __sync_add_and_fetch(&h->freed, total)) {
        return;
    }
}

static struct item *segment_item_alloc(struct segment_header *h, size_t total)
{
    struct item *it = (struct item *) ((uintptr_t) h->data + h->offset);
    size_t avail;

    /* Not enough room in this segment */
    avail = h->size - h->offset;
    if (avail == 0) {
        return NULL;
    } else if (avail < total) {
        if (avail >= sizeof(struct item)) {
            it->refcount = 0;
            /* needed for log scan */
            it->keylen = avail - sizeof(struct item);
            it->vallen = 0;
        }
        segment_item_free(h, avail);
        h->offset += avail;
        return NULL;
    }

    /* Ordering here is important */
    it->refcount = 1;

    h->offset += total;

    return it;
}


void ialloc_init_allocator(struct item_allocator *ia)
{
    struct segment_header *h;

    memset(ia, 0, sizeof(*ia));

    if ((h = segment_alloc()) == NULL) {
        fprintf(stderr, "Allocating segment failed\n");
        abort();
    }
    h->next = NULL;
    ia->cur = h;
    ia->oldest = h;

    if ((h = segment_alloc()) == NULL) {
        fprintf(stderr, "Allocating reserved segment failed\n");
        abort();
    }
    h->next = NULL;
    ia->reserved = h;

    printf("Initializing allocator: %lu\n", (unsigned long) (settings.segcqsize *
            sizeof(*ia->cleanup_queue)));
    ia->cleanup_queue = calloc(settings.segcqsize, sizeof(*ia->cleanup_queue));
    ia->cq_head = ia->cq_tail = 0;
    ia->cleaning = NULL;
}

struct item *ialloc_alloc(struct item_allocator *ia, size_t total, bool cleanup)
{
    struct segment_header *h, *old;
    struct item *it;
    assert(total < settings.segsize);

    /* If the reserved segment is currently active, only allocations for cleanup
     * are allowed */
    if (ia->reserved == NULL && !cleanup) {
        printf("Only cleanup!\n");
        return NULL;
    }

    old = ia->cur;
    if ((it = segment_item_alloc(old, total)) != NULL) {
        return it;
    }

    if ((h = segment_alloc()) == NULL) {
        /* We're currently doing cleanup, and still have the reserved segment
         * then that can be used now */
        if (cleanup && ia->reserved != NULL) {
            h = ia->reserved;
            ia->reserved = NULL;
        } else {
            printf("Fail 2!\n");
            return NULL;
        }
    }
    old->next = h;
    h->next = NULL;
    /* Mark old segment as GC-able */
    old->flags |= SF_INACTIVE;
    ia->cur = h;

    it = segment_item_alloc(h, total);
    if (it == NULL) {
        printf("Fail 3!\n");
        return NULL;
    }
    return it;
}

void ialloc_free(struct item *it, size_t total)
{
    struct segment_header *h = segment_from_part(it);
    segment_item_free(h, total);
}

void ialloc_free_dont_need(struct item *it, size_t total)
{
    struct segment_header *h = segment_from_part(it);
    segment_item_free(h, total);
    if (madvise(it, item_totalsz(it), MADV_DONTNEED) != 0){
	  perror("madvise");
    }
}

struct item *ialloc_cleanup_item(struct item_allocator *ia, bool idle)
{
    size_t i;
    struct item *it;

    if (!idle) {
        if (ia->cleanup_count >= 32) {
            return NULL;
        }
        ia->cleanup_count++;
    }

    i = ia->cq_head;
    it = ia->cleanup_queue[i];
    if (it != NULL) {
        ia->cleanup_queue[i] = NULL;
        ia->cq_head = (i + 1) % settings.segcqsize;
    }
    if (ia->reserved == NULL) {
        ia->reserved = segment_alloc();
    }
    return it;
}

void ialloc_cleanup_nextrequest(struct item_allocator *ia)
{
    ia->cleanup_count = 0;
}

void ialloc_maintenance(struct item_allocator *ia)
{
#if 0
    struct segment_header *h, *prev, *next, *cand;
    struct item *it,  **cq = ia->cleanup_queue;
    size_t off, size, idx;
    double cand_ratio, ratio;
    void *data;

    /* Check if we can now free some segments? While we're at it, we can also
     * look for a candidate to be cleaned */
    h = ia->oldest;
    prev = NULL;
    cand = NULL;
    cand_ratio = 0;
    while (h != NULL && (h->flags & SF_INACTIVE) == SF_INACTIVE) {
        next = h->next;
        /* Done with this segment? */
        if (h->freed == h->size) {
            if (prev == NULL) {
                ia->oldest = h->next;
            } else {
                prev->next = h->next;
            }
            segment_free(h);
            h = prev;
        } else {
            /* Otherwise we also look for the next cleanup candidate if
             * necessary */
            ratio = (double) h->freed / h->size;
            if (ratio >= 0.5 && ratio > cand_ratio) {
                cand_ratio = ratio;
                cand = h;
            }
        }
        prev = h;
        h = next;
    }

    /* Check if we're currently working on cleaning a segment */
    h = ia->cleaning;
    off = ia->clean_offset;
    size = (h == NULL ? 0 : h->size);
    if (h == NULL || off == size) {
        h = cand;
        ia->cleaning = h;
        off = ia->clean_offset = 0;
    }

    /* No segments to clean, that's great! */
    if (h == NULL) {
        return;
    }

    /* Enqueue clean requests to worker untill we run out or the queue is filled
     * up */
    idx = ia->cq_tail;
    data = h->data;
    while (off < size && cq[idx] == NULL) {
        it = (struct item *) ((uintptr_t) data + off);
        if (size - off < sizeof(struct item)) {
            off = size;
            break;
        }
        if (item_tryref(it)) {
            cq[idx] = it;
            idx = (idx + 1) % settings.segcqsize;
        }
        off += item_totalsz(it);
    }
    ia->cq_tail = idx;
    ia->clean_offset = off;
#endif
    struct segment_header *h, *prev, *next, *cand;
    struct item *it,  **cq = ia->cleanup_queue;
    size_t off, size, idx;
    double cand_ratio, ratio;
    void *data;

    /* Check if we can now free some segments? While we're at it, we can also
     * look for a candidate to be cleaned */
    cand = NULL;
    cand_ratio = 0;
    h = ia->oldest;
    prev = NULL;
    /* We stop before the last segment in the list, and if we hit any
     * non-inactive segments. This prevents us from having to touch the cur
     * pointers. */
    while (h != NULL && h->next != NULL &&
            (h->flags & SF_INACTIVE) == SF_INACTIVE)
    {
        next = h->next;
        ratio = (double) h->freed / h->size;
        /* Done with this segment? */
        if (h->freed == h->size) {
            if (prev == NULL) {
                ia->oldest = h->next;
            } else {
                prev->next = h->next;
            }
            segment_free(h);
            h = prev;
        } else if ((h->flags & SF_CLEANED) != SF_CLEANED) {
            /* Otherwise we also look for the next cleanup candidate if
             * necessary */
            ratio = (double) h->freed / h->size;
            if (ratio >= settings.clean_ratio && ratio > cand_ratio) {
                cand_ratio = ratio;
                cand = h;
            }
        }
        prev = h;
        h = next;
    }

    /* Check if we're currently working on cleaning a segment */
    h = ia->cleaning;
    off = ia->clean_offset;
    size = (h == NULL ? 0 : h->size);
    if (h == NULL || off == size) {
        h = cand;
        ia->cleaning = h;
        off = ia->clean_offset = 0;
        if (h != NULL) {
            h->flags |= SF_CLEANED;
        }
    }

    /* No segments to clean, that's great! */
    if (h == NULL) {
        return;
    }

    /* Enqueue clean requests to worker untill we run out or the queue is filled
     * up */
    idx = ia->cq_tail;
    data = h->data;
    while (off < size && cq[idx] == NULL) {
        it = (struct item *) ((uintptr_t) data + off);
        if (size - off < sizeof(struct item)) {
            off = size;
            break;
        }
        if (item_tryref(it)) {
            cq[idx] = it;
            idx = (idx + 1) % settings.segcqsize;
        }
        off += item_totalsz(it);
    }
    ia->cq_tail = idx;
    ia->clean_offset = off;

}
