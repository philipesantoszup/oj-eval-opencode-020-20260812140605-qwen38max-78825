#include <stdlib.h>

#include "buddy.h"

#define PAGE_SIZE 4096L
#define MAX_RANK 16

enum { ST_INVALID = 0, ST_FREE = 1, ST_ALLOC = 2, ST_SPLIT = 3 };

static void *g_base = NULL;
static long g_total_pages = 0;
static int g_max_rank = 0;

static unsigned char *g_state[MAX_RANK + 1];
static int g_nblocks[MAX_RANK + 1];
static int g_free_cnt[MAX_RANK + 1];

typedef struct {
    int *a;
    int n;
    int cap;
} MinHeap;

static MinHeap g_free[MAX_RANK + 1];

static void heap_push(MinHeap *h, int v) {
    if (h->n == h->cap) {
        int ncap = h->cap ? h->cap * 2 : 256;
        int *na = (int *)realloc(h->a, (size_t)ncap * sizeof(int));
        if (!na) return;
        h->a = na;
        h->cap = ncap;
    }
    int i = h->n++;
    h->a[i] = v;
    while (i > 0) {
        int p = (i - 1) / 2;
        if (h->a[p] <= h->a[i]) break;
        int t = h->a[p];
        h->a[p] = h->a[i];
        h->a[i] = t;
        i = p;
    }
}

static int heap_pop(MinHeap *h) {
    int top = h->a[0];
    int last = h->a[--h->n];
    if (h->n > 0) {
        h->a[0] = last;
        int i = 0;
        for (;;) {
            int l = 2 * i + 1, r = 2 * i + 2, s = i;
            if (l < h->n && h->a[l] < h->a[s]) s = l;
            if (r < h->n && h->a[r] < h->a[s]) s = r;
            if (s == i) break;
            int t = h->a[i];
            h->a[i] = h->a[s];
            h->a[s] = t;
            i = s;
        }
    }
    return top;
}

static void mark_free(int r, int idx) {
    g_state[r][idx] = ST_FREE;
    g_free_cnt[r]++;
    heap_push(&g_free[r], idx);
}

int init_page(void *p, int pgcount) {
    if (!p || pgcount < 1) return -EINVAL;

    if (g_base) {
        for (int r = 1; r <= MAX_RANK; r++) {
            free(g_state[r]);
            g_state[r] = NULL;
            free(g_free[r].a);
            g_free[r].a = NULL;
            g_free[r].n = 0;
            g_free[r].cap = 0;
            g_free_cnt[r] = 0;
            g_nblocks[r] = 0;
        }
    }

    g_base = p;
    g_total_pages = pgcount;
    g_max_rank = 0;
    for (int r = MAX_RANK; r >= 1; r--) {
        if (((long)pgcount) >> (r - 1)) {
            g_max_rank = r;
            break;
        }
    }

    for (int r = 1; r <= MAX_RANK; r++) {
        long n = ((long)pgcount) >> (r - 1);
        if (n < 1) n = 1;
        g_nblocks[r] = (int)n;
        g_state[r] = (unsigned char *)calloc((size_t)n + 2, 1);
        if (!g_state[r]) return -EINVAL;
        g_free_cnt[r] = 0;
    }

    long off = 0, rem = pgcount;
    for (int r = MAX_RANK; r >= 1 && rem > 0; r--) {
        long sz = 1L << (r - 1);
        while (rem >= sz) {
            mark_free(r, (int)(off >> (r - 1)));
            off += sz;
            rem -= sz;
        }
    }

    return OK;
}

void *alloc_pages(int rank) {
    if (rank < 1 || rank > MAX_RANK) return ERR_PTR(-EINVAL);
    if (!g_base) return ERR_PTR(-ENOSPC);

    int r;
    for (r = rank; r <= g_max_rank; r++) {
        if (g_free_cnt[r] > 0) break;
    }
    if (r > g_max_rank) return ERR_PTR(-ENOSPC);

    MinHeap *h = &g_free[r];
    int idx = -1;
    while (h->n > 0) {
        int cand = heap_pop(h);
        if (g_state[r][cand] == ST_FREE) {
            idx = cand;
            break;
        }
    }
    if (idx < 0) return ERR_PTR(-ENOSPC);

    while (r > rank) {
        g_state[r][idx] = ST_SPLIT;
        g_free_cnt[r]--;
        r--;
        idx <<= 1;
        mark_free(r, idx);
        mark_free(r, idx + 1);
    }

    g_state[r][idx] = ST_ALLOC;
    g_free_cnt[r]--;

    long page_idx = (long)idx << (rank - 1);
    return (void *)((char *)g_base + page_idx * PAGE_SIZE);
}

static int find_block(long page_idx, int *out_rank, int *out_idx) {
    for (int r = 1; r <= g_max_rank; r++) {
        int i = (int)(page_idx >> (r - 1));
        if (i >= g_nblocks[r]) continue;
        if (g_state[r][i] != ST_INVALID) {
            *out_rank = r;
            *out_idx = i;
            return 1;
        }
    }
    return 0;
}

int return_pages(void *p) {
    if (!p || !g_base) return -EINVAL;
    long off = (long)((char *)p - (char *)g_base);
    if (off < 0 || off % PAGE_SIZE != 0) return -EINVAL;
    long page_idx = off / PAGE_SIZE;
    if (page_idx >= g_total_pages) return -EINVAL;

    int r, idx;
    if (!find_block(page_idx, &r, &idx)) return -EINVAL;
    if (g_state[r][idx] != ST_ALLOC) return -EINVAL;
    if (((long)idx << (r - 1)) != page_idx) return -EINVAL;

    g_state[r][idx] = ST_FREE;
    g_free_cnt[r]++;
    heap_push(&g_free[r], idx);

    while (r < g_max_rank) {
        int b = idx ^ 1;
        if (b >= g_nblocks[r]) break;
        if (g_state[r][b] != ST_FREE) break;
        g_state[r][b] = ST_INVALID;
        g_state[r][idx] = ST_INVALID;
        g_free_cnt[r] -= 2;
        r++;
        idx >>= 1;
        g_state[r][idx] = ST_FREE;
        g_free_cnt[r]++;
        heap_push(&g_free[r], idx);
    }

    return OK;
}

int query_ranks(void *p) {
    if (!p || !g_base) return -EINVAL;
    long off = (long)((char *)p - (char *)g_base);
    if (off < 0 || off % PAGE_SIZE != 0) return -EINVAL;
    long page_idx = off / PAGE_SIZE;
    if (page_idx >= g_total_pages) return -EINVAL;

    int r, idx;
    if (!find_block(page_idx, &r, &idx)) return -EINVAL;
    return r;
}

int query_page_counts(int rank) {
    if (rank < 1 || rank > MAX_RANK) return -EINVAL;
    if (!g_base || rank > g_max_rank) return 0;
    return g_free_cnt[rank];
}
