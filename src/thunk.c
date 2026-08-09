#define _GNU_SOURCE
#include "box64dl_thunk.h"

#include <pthread.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

/* From thunk_la64.S — the code half of one thunk, copied verbatim. */
extern const char box64dl_thunk_code[];
extern const char box64dl_thunk_code_end[];

#define THUNK_CODE_SZ 24u
#define THUNK_SZ 48u /* code + data0 + data1 + target */
#define CHUNK_SZ (64u * 1024u)
#define PER_CHUNK (CHUNK_SZ / THUNK_SZ)

/*
 * Pages are mapped twice over the same memfd: a writable view we patch through
 * and an executable view callers jump to. Toggling PROT_EXEC on a single
 * mapping would be simpler but races with other threads already executing a
 * thunk in that page.
 */
struct chunk {
  struct chunk* next;
  char* rw;
  char* rx;
  unsigned used; /* bump allocator; freed slots come back via the free list */
};

struct slot_data {
  uint64_t data0;
  uint64_t data1;
  uint64_t target;
};

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static struct chunk* g_chunks;
static void** g_free;   /* free list, threaded through rw data0 */
static size_t g_nfree;
static size_t g_live;

/* Open-addressed (target, data0) -> rx thunk, so lookup does not scan. */
static void** g_tab;
static size_t g_cap;  /* power of two */
static size_t g_fill;

static struct chunk* chunk_of(void* rx) {
  struct chunk* c;
  for (c = g_chunks; c; c = c->next)
    if ((char*)rx >= c->rx && (char*)rx < c->rx + CHUNK_SZ) return c;
  return NULL;
}

static struct slot_data* slot_rw(void* rx) {
  struct chunk* c = chunk_of(rx);
  if (!c) return NULL;
  return (struct slot_data*)(c->rw + ((char*)rx - c->rx) + THUNK_CODE_SZ);
}

static uint64_t slot_key_hash(uint64_t target, uint64_t data0) {
  uint64_t h = data0 * 0x9e3779b97f4a7c15ull ^ (target * 0xff51afd7ed558ccdull);
  h ^= h >> 29;
  h *= 0xbf58476d1ce4e5b9ull;
  h ^= h >> 32;
  return h;
}

static void tab_insert(void** tab, size_t cap, void* rx) {
  struct slot_data* d = slot_rw(rx);
  size_t i = (size_t)(slot_key_hash(d->target, d->data0) & (cap - 1));
  while (tab[i]) i = (i + 1) & (cap - 1);
  tab[i] = rx;
}

static int tab_grow(void) {
  size_t ncap = g_cap ? g_cap * 2 : 64;
  void** ntab = mmap(NULL, ncap * sizeof(void*), PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  size_t i;
  if (ntab == MAP_FAILED) return -1;
  for (i = 0; i < g_cap; ++i)
    if (g_tab[i]) tab_insert(ntab, ncap, g_tab[i]);
  if (g_tab) munmap(g_tab, g_cap * sizeof(void*));
  g_tab = ntab;
  g_cap = ncap;
  return 0;
}

/* Tombstone-free removal: re-insert the run that follows the hole. */
static void tab_remove(void* rx) {
  size_t i, j;
  if (!g_cap) return;
  for (i = 0; i < g_cap; ++i)
    if (g_tab[i] == rx) break;
  if (i == g_cap) return;
  g_tab[i] = NULL;
  g_fill--;
  j = (i + 1) & (g_cap - 1);
  while (g_tab[j]) {
    void* moved = g_tab[j];
    g_tab[j] = NULL;
    tab_insert(g_tab, g_cap, moved);
    j = (j + 1) & (g_cap - 1);
  }
}

static void* tab_find(uint64_t target, uint64_t data0) {
  size_t i;
  if (!g_cap) return NULL;
  i = (size_t)(slot_key_hash(target, data0) & (g_cap - 1));
  while (g_tab[i]) {
    struct slot_data* d = slot_rw(g_tab[i]);
    if (d && d->target == target && d->data0 == data0) return g_tab[i];
    i = (i + 1) & (g_cap - 1);
  }
  return NULL;
}

static int chunk_new(void) {
  struct chunk* c;
  char* rw;
  char* rx;
  int fd;
  unsigned k;

  fd = memfd_create("box64dl-thunks", MFD_CLOEXEC);
  if (fd < 0) return -1;
  if (ftruncate(fd, (off_t)CHUNK_SZ) != 0) {
    close(fd);
    return -1;
  }
  rw = mmap(NULL, CHUNK_SZ, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  rx = mmap(NULL, CHUNK_SZ, PROT_READ | PROT_EXEC, MAP_SHARED, fd, 0);
  close(fd);
  if (rw == MAP_FAILED || rx == MAP_FAILED) {
    if (rw != MAP_FAILED) munmap(rw, CHUNK_SZ);
    if (rx != MAP_FAILED) munmap(rx, CHUNK_SZ);
    return -1;
  }

  /* Stamp every slot's code once; only the data words change afterwards. */
  for (k = 0; k < PER_CHUNK; ++k)
    memcpy(rw + k * THUNK_SZ, box64dl_thunk_code, THUNK_CODE_SZ);
  __builtin___clear_cache(rx, rx + CHUNK_SZ);

  c = mmap(NULL, sizeof(*c), PROT_READ | PROT_WRITE,
           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (c == MAP_FAILED) {
    munmap(rw, CHUNK_SZ);
    munmap(rx, CHUNK_SZ);
    return -1;
  }
  c->rw = rw;
  c->rx = rx;
  c->used = 0;
  c->next = g_chunks;
  g_chunks = c;
  return 0;
}

static void* slot_alloc(void) {
  struct chunk* c;
  if (g_nfree) return g_free[--g_nfree];
  for (c = g_chunks; c; c = c->next)
    if (c->used < PER_CHUNK) return c->rx + (c->used++) * THUNK_SZ;
  if (chunk_new() != 0) return NULL;
  return g_chunks->rx + (g_chunks->used++) * THUNK_SZ;
}

static int free_push(void* rx) {
  static size_t cap;
  if (g_nfree == cap) {
    size_t ncap = cap ? cap * 2 : 64;
    void** n = mmap(NULL, ncap * sizeof(void*), PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (n == MAP_FAILED) return -1;
    if (g_free) {
      memcpy(n, g_free, g_nfree * sizeof(void*));
      munmap(g_free, cap * sizeof(void*));
    }
    g_free = n;
    cap = ncap;
  }
  g_free[g_nfree++] = rx;
  return 0;
}

void* box64dl_thunk_get(void* target, uint64_t data0, uint64_t data1) {
  void* rx;
  struct slot_data* d;
  if (!target) return NULL;
  pthread_mutex_lock(&g_mu);

  rx = tab_find((uint64_t)(uintptr_t)target, data0);
  if (rx) {
    pthread_mutex_unlock(&g_mu);
    return rx;
  }
  if ((g_fill + 1) * 4 >= g_cap * 3 && tab_grow() != 0) {
    pthread_mutex_unlock(&g_mu);
    return NULL;
  }
  rx = slot_alloc();
  if (!rx) {
    pthread_mutex_unlock(&g_mu);
    return NULL;
  }
  d = slot_rw(rx);
  d->data0 = data0;
  d->data1 = data1;
  d->target = (uint64_t)(uintptr_t)target;
  /* Code was stamped at chunk creation; only data changed, but the first use
   * of a recycled slot may still be in the i-cache from its previous life. */
  __builtin___clear_cache((char*)rx, (char*)rx + THUNK_CODE_SZ);
  tab_insert(g_tab, g_cap, rx);
  g_fill++;
  g_live++;

  pthread_mutex_unlock(&g_mu);
  return rx;
}

void* box64dl_thunk_peek(void* target, uint64_t data0) {
  void* rx;
  pthread_mutex_lock(&g_mu);
  rx = tab_find((uint64_t)(uintptr_t)target, data0);
  pthread_mutex_unlock(&g_mu);
  return rx;
}

uint64_t box64dl_thunk_data0(void* thunk) {
  struct slot_data* d;
  uint64_t v = 0;
  pthread_mutex_lock(&g_mu);
  d = slot_rw(thunk);
  if (d) v = d->data0;
  pthread_mutex_unlock(&g_mu);
  return v;
}

static void release_locked(void* rx) {
  struct slot_data* d = slot_rw(rx);
  if (!d) return;
  tab_remove(rx);
  d->data0 = 0;
  d->data1 = 0;
  d->target = 0;
  if (free_push(rx) == 0 && g_live) g_live--;
}

void box64dl_thunk_release(void* thunk) {
  pthread_mutex_lock(&g_mu);
  release_locked(thunk);
  pthread_mutex_unlock(&g_mu);
}

size_t box64dl_thunk_release_range(uint64_t lo, uint64_t hi) {
  size_t n = 0, i;
  pthread_mutex_lock(&g_mu);
  /* Restart after each removal: tab_remove reshuffles the following run. */
  for (i = 0; i < g_cap;) {
    struct slot_data* d = g_tab[i] ? slot_rw(g_tab[i]) : NULL;
    if (d && d->data0 >= lo && d->data0 < hi) {
      release_locked(g_tab[i]);
      n++;
      i = 0;
      continue;
    }
    i++;
  }
  pthread_mutex_unlock(&g_mu);
  return n;
}

size_t box64dl_thunk_count(void) {
  size_t n;
  pthread_mutex_lock(&g_mu);
  n = g_live;
  pthread_mutex_unlock(&g_mu);
  return n;
}
