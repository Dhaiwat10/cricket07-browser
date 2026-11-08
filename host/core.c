/* Host services shared by the Windows API layer: guest heap, handles, path mapping, time. */
#include "win32.h"

#include <ctype.h>
#include <dirent.h>
#include <pthread.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

int g_log;
char g_game_root[1024];
char g_user_root[1024];

/* ------------------------------------------------------------------ guest heap
 * Lives entirely in guest memory so the game can hold pointers into it. 16-byte header before each
 * block: [size][magic][0][0]. Size classes in 16-byte steps up to 4 KiB; larger blocks go on a list.
 */

#define HEAP_LO GUEST_HEAP_BASE
#define HEAP_HI GUEST_HEAP_HI
#define HEAP_MAGIC 0x4EA911C8u
#define NCLASS 256
static pthread_mutex_t g_heap_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t g_heap_top = HEAP_LO;
static uint32_t g_free[NCLASS + 1];  /* singly linked through the first payload word */

static uint32_t round16(uint32_t n) { return (n + 15) & ~15u; }

uint32_t heap_alloc(uint32_t size, int zero) {
    uint32_t sz = round16(size ? size : 1), a = 0;
    pthread_mutex_lock(&g_heap_lock);
    uint32_t cls = sz / 16;
    if (cls <= NCLASS && g_free[cls]) {
        a = g_free[cls];
        g_free[cls] = R32(a);
    } else if (cls > NCLASS) {  /* first fit among large free blocks */
        uint32_t prev = 0, p = g_free[0];
        while (p && R32(p - 16) < sz) { prev = p; p = R32(p); }
        if (p) {
            a = p;
            if (prev) W32(prev, R32(p)); else g_free[0] = R32(p);
            sz = R32(p - 16);
        }
    }
    if (!a) {
        if (g_heap_top + 16 + sz > HEAP_HI) {
            pthread_mutex_unlock(&g_heap_lock);
            fprintf(stderr, "guest heap exhausted (%u bytes requested)\n", size);
            abort();
        }
        a = g_heap_top + 16;
        g_heap_top += 16 + sz;
        W32(a - 16, sz);
    }
    W32(a - 12, HEAP_MAGIC);
    pthread_mutex_unlock(&g_heap_lock);
    if (zero) memset(HP(a), 0, R32(a - 16));
    return a;
}

void heap_free(uint32_t a) {
    if (!a) return;
    if (a < HEAP_LO || a >= HEAP_HI || R32(a - 12) != HEAP_MAGIC) {
        LOG("[heap] free of non-heap pointer %08x ignored\n", a);
        return;
    }
    pthread_mutex_lock(&g_heap_lock);
    W32(a - 12, 0);
    uint32_t cls = R32(a - 16) / 16;
    if (cls > NCLASS) cls = 0;
    W32(a, g_free[cls]);
    g_free[cls] = a;
    pthread_mutex_unlock(&g_heap_lock);
}

uint32_t heap_size(uint32_t a) {
    return (a >= HEAP_LO && a < HEAP_HI && R32(a - 12) == HEAP_MAGIC) ? R32(a - 16) : 0;
}

uint32_t heap_realloc(uint32_t a, uint32_t size, int zero) {
    if (!a) return heap_alloc(size, zero);
    uint32_t old = heap_size(a);
    if (round16(size) <= old) return a;
    uint32_t b = heap_alloc(size, 0);
    memcpy(HP(b), HP(a), old);
    if (zero) memset(HP(b + old), 0, R32(b - 16) - old);
    heap_free(a);
    return b;
}

#define VALLOC_LO GUEST_VALLOC_LO
#define VALLOC_HI GUEST_VALLOC_HI
static uint32_t g_valloc_top = VALLOC_LO;
static uint32_t g_valloc_hwm = VALLOC_LO;   /* above this, the range was never handed out, so it is still zero */

/* Regions handed out (so a release knows the size) and released regions, kept sorted and merged so a
   released file view or VirtualAlloc region can be handed out again: the game maps its 64 MB motion
   archive at every match start and unmaps it at the end, and the browser build only has 768 MB. */
typedef struct { uint32_t addr, size; } VRegion;
static VRegion *g_vused, *g_vfree;
static uint32_t g_nvused, g_capvused, g_nvfree, g_capvfree;

static void vregion_add(VRegion **v, uint32_t *n, uint32_t *cap, uint32_t at, VRegion r) {
    if (*n == *cap) { *cap = *cap ? *cap * 2 : 64; *v = realloc(*v, *cap * sizeof **v); }
    memmove(*v + at + 1, *v + at, (*n - at) * sizeof **v);
    (*v)[at] = r;
    (*n)++;
}
static void vregion_del(VRegion *v, uint32_t *n, uint32_t at) {
    memmove(v + at, v + at + 1, (*n - at - 1) * sizeof *v);
    (*n)--;
}

static uint32_t region_alloc(uint32_t size, int zero) {
    size = (size + 0xffff) & ~0xffffu;  /* Windows allocation granularity */
    if (!size) size = 0x10000;
    pthread_mutex_lock(&g_heap_lock);
    uint32_t a = 0;
    for (uint32_t i = 0; i < g_nvfree; i++)   /* first fit among released regions (sorted by address) */
        if (g_vfree[i].size >= size) {
            a = g_vfree[i].addr;
            if (g_vfree[i].size == size) vregion_del(g_vfree, &g_nvfree, i);
            else { g_vfree[i].addr += size; g_vfree[i].size -= size; }
            break;
        }
    if (!a) {
        if (g_valloc_top + size > VALLOC_HI) { pthread_mutex_unlock(&g_heap_lock); return 0; }
        a = g_valloc_top;
        g_valloc_top += size;
        /* a region given back at the top holds what it held; beyond the high-water mark it's fresh, still zero
           (and not yet resident in the browser, which a memset would change) */
        if (zero && a < g_valloc_hwm) memset(HP(a), 0, g_valloc_hwm - a < size ? g_valloc_hwm - a : size);
        if (g_valloc_top > g_valloc_hwm) g_valloc_hwm = g_valloc_top;
    } else if (zero) memset(HP(a), 0, size);   /* Windows hands out zeroed pages */
    uint32_t i = 0;
    while (i < g_nvused && g_vused[i].addr < a) i++;
    vregion_add(&g_vused, &g_nvused, &g_capvused, i, (VRegion){ a, size });
    pthread_mutex_unlock(&g_heap_lock);
    return a;
}

uint32_t guest_valloc(uint32_t size) { return region_alloc(size, 1); }

/* Releases a region guest_valloc returned; 0 (and no change) when `a` isn't the start of one. */
uint32_t guest_vfree(uint32_t a) {
    pthread_mutex_lock(&g_heap_lock);
    uint32_t lo = 0, hi = g_nvused;
    while (lo < hi) { uint32_t m = (lo + hi) / 2; if (g_vused[m].addr < a) lo = m + 1; else hi = m; }
    if (lo >= g_nvused || g_vused[lo].addr != a) { pthread_mutex_unlock(&g_heap_lock); return 0; }
    uint32_t size = g_vused[lo].size;
    vregion_del(g_vused, &g_nvused, lo);
    uint32_t i = 0;
    while (i < g_nvfree && g_vfree[i].addr < a) i++;
    vregion_add(&g_vfree, &g_nvfree, &g_capvfree, i, (VRegion){ a, size });
    if (i + 1 < g_nvfree && g_vfree[i].addr + g_vfree[i].size == g_vfree[i + 1].addr) {   /* merge with the next */
        g_vfree[i].size += g_vfree[i + 1].size;
        vregion_del(g_vfree, &g_nvfree, i + 1);
    }
    if (i > 0 && g_vfree[i - 1].addr + g_vfree[i - 1].size == a) {   /* and with the previous */
        g_vfree[i - 1].size += g_vfree[i].size;
        vregion_del(g_vfree, &g_nvfree, i);
        i--;
    }
    if (g_vfree[i].addr + g_vfree[i].size == g_valloc_top) {   /* top of the range: give it back */
        g_valloc_top = g_vfree[i].addr;
        vregion_del(g_vfree, &g_nvfree, i);
    }
    pthread_mutex_unlock(&g_heap_lock);
    return size;
}

/* ------------------------------------------------------------------ lazy file views
 * The game maps its .big archives whole with MapViewOfFile (data.gob alone is 325 MB, about 575 MB in all by the
 * first match) but reads only a few percent of them: about 30 MB into a match. Copied in whole, they were most
 * of the browser build's memory, and iPhones killed the tab. A lazy view reads the archive's header and
 * directory (which the game parses in place) at once, and any other 64 KB chunk when it is first copied out:
 * the game reads every file from an archive with its memcpy, whose entry hook (profiles/cricket07.patches.json) lands here.
 * Natively, RC_VIEW_CHECK=1 keeps unread chunks inaccessible, so a read that skipped the hook crashes at once.
 */
#define VIEW_CHUNK 0x10000u
#define VIEW_MAX_CHUNKS 0x10000u   /* a 4 GB view */
#define MAX_VIEWS 16
/* A slot's lock and bitmap last as long as the program, so a copy that races an unmap never touches freed memory,
   and a read checks under the lock that its view is still mapped before it writes into the region. */
typedef struct { pthread_mutex_t lock; uint32_t a, len; int fd; uint8_t filled[VIEW_MAX_CHUNKS / 8]; } LazyView;
static LazyView g_views[MAX_VIEWS];
static int g_nviews;   /* slots ever used; a released slot has len 0 */
static pthread_mutex_t g_view_lock = PTHREAD_MUTEX_INITIALIZER;   /* picking, mapping and releasing slots */
static int g_view_check;

#ifndef __EMSCRIPTEN__
#include <dlfcn.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/ucontext.h>
static void view_protect(uint32_t a, uint32_t len, int prot) { mprotect(HP(a), (len + 0x3fff) & ~0x3fffu, prot); }
static void view_check_fault(int sig, siginfo_t *si, void *uc) {
    uintptr_t p = (uintptr_t)si->si_addr;
    Dl_info di = { 0 };
    dladdr((void *)((ucontext_t *)uc)->uc_mcontext->__ss.__pc, &di);
    for (int i = 0; i < g_nviews; i++)
        if (g_views[i].len && p >= (uintptr_t)HP(g_views[i].a) && p < (uintptr_t)HP(g_views[i].a) + g_views[i].len) {
            fprintf(stderr, "[view] read of an unfilled chunk of the view at %08x (+%lx) outside the memcpy hook, in %s\n",
                    g_views[i].a, (unsigned long)(p - (uintptr_t)HP(g_views[i].a)), di.dli_sname ? di.dli_sname : "?");
            break;
        }
    signal(sig, SIG_DFL);
}
#else
static void view_protect(uint32_t a, uint32_t len, int prot) { (void)a; (void)len; (void)prot; }
#endif

static int chunk_filled(LazyView *v, uint32_t k) {
    return __atomic_load_n(&v->filled[k >> 3], __ATOMIC_ACQUIRE) & (1u << (k & 7));
}

/* Reads chunks k0..k1 (none of them filled yet) in one go, with v->lock held. A chunk counts as filled only once
   all of it has been read; after a failed or short read the rest stay unfilled, and the next copy tries again. */
static void view_read_run(LazyView *v, uint32_t k0, uint32_t k1) {
    uint64_t run_end = (uint64_t)(k1 + 1) * VIEW_CHUNK;
    uint32_t at = k0 * VIEW_CHUNK, end = run_end < v->len ? (uint32_t)run_end : v->len, got = 0;
    if (g_view_check) view_protect(v->a + at, end - at, 3 /* PROT_READ | PROT_WRITE */);
    while (at + got < end) {
        ssize_t r = pread(v->fd, HP(v->a + at + got), end - at - got, (off_t)at + got);
        if (r <= 0) break;
        got += (uint32_t)r;
    }
    if (at + got < end)
        fprintf(stderr, "[view] read %u of %u bytes at +%x of the view at %08x\n", got, end - at, at, v->a);
    LOG("[view] %08x: read %u KB at +%x\n", v->a, got >> 10, at);
    for (uint32_t k = k0; k <= k1; k++) {
        uint64_t k_end = (uint64_t)(k + 1) * VIEW_CHUNK < v->len ? (uint64_t)(k + 1) * VIEW_CHUNK : v->len;
        if (k_end > (uint64_t)at + got) break;
        __atomic_or_fetch(&v->filled[k >> 3], (uint8_t)(1u << (k & 7)), __ATOMIC_RELEASE);
    }
}

static void view_fill(uint32_t src, uint32_t n) {
    if (!n) return;
    int nv = __atomic_load_n(&g_nviews, __ATOMIC_ACQUIRE);
    for (int i = 0; i < nv; i++) {
        LazyView *v = &g_views[i];
        uint32_t len = __atomic_load_n(&v->len, __ATOMIC_ACQUIRE), a = v->a;
        if (!len || src >= a + len || src + n <= a) continue;
        uint32_t lo = src > a ? src - a : 0, hi = src + n - a < len ? src + n - a : len;
        uint32_t k1 = (hi - 1) / VIEW_CHUNK;
        for (uint32_t k = lo / VIEW_CHUNK; k <= k1; k++) {
            if (chunk_filled(v, k)) continue;
            pthread_mutex_lock(&v->lock);
            if (v->len == len && v->a == a)   /* still this view: read each run of chunks still unfilled */
                for (uint32_t s = k; s <= k1; s++) {
                    if (chunk_filled(v, s)) continue;
                    uint32_t e = s;
                    while (e < k1 && !chunk_filled(v, e + 1)) e++;
                    view_read_run(v, s, e);
                    s = e;
                }
            pthread_mutex_unlock(&v->lock);
            break;
        }
    }
}

/* A lazy view of `len` bytes of the open file `fd` from its start, whose first `eager` bytes are read now; 0 when
   there is no free slot or address space. */
uint32_t guest_view_map(int fd, uint32_t len, uint32_t eager) {
    static int init;
    pthread_mutex_lock(&g_view_lock);
    if (!init) {
        init = 1;
#ifndef __EMSCRIPTEN__
        if ((g_view_check = getenv("RC_VIEW_CHECK") != NULL)) {
            struct sigaction sa;
            memset(&sa, 0, sizeof sa);
            sa.sa_sigaction = view_check_fault;
            sa.sa_flags = SA_SIGINFO;
            sigaction(SIGSEGV, &sa, NULL);
            sigaction(SIGBUS, &sa, NULL);
        }
#endif
        g_copy_source_hook = view_fill;
    }
    int i = 0;
    while (i < g_nviews && g_views[i].len) i++;
    uint32_t a = i < MAX_VIEWS ? region_alloc(len, 0) : 0;   /* not zeroed: no byte is read before its chunk is filled */
    if (!a) { pthread_mutex_unlock(&g_view_lock); return 0; }
    LazyView *v = &g_views[i];
    if (i == g_nviews) pthread_mutex_init(&v->lock, NULL);
    pthread_mutex_lock(&v->lock);
    memset(v->filled, 0, (len - 1) / VIEW_CHUNK / 8 + 1);
    v->a = a;
    v->fd = dup(fd);
    if (g_view_check) view_protect(a, len, 0 /* PROT_NONE */);
    __atomic_store_n(&v->len, len, __ATOMIC_RELEASE);
    if (eager) view_read_run(v, 0, ((eager < len ? eager : len) - 1) / VIEW_CHUNK);
    pthread_mutex_unlock(&v->lock);
    if (i == g_nviews) __atomic_store_n(&g_nviews, i + 1, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&g_view_lock);
    return a;
}

/* The end of the lazy view holding `a`, or 0: memory sweeps (RC_HASH_ALL) skip views, read-only file data that
   is only partly read in. */
uint32_t guest_view_end(uint32_t a) {
    for (int i = 0; i < g_nviews; i++)
        if (g_views[i].len && a >= g_views[i].a && a - g_views[i].a < g_views[i].len)
            return (g_views[i].a + g_views[i].len + 0xffff) & ~0xffffu;   /* its whole region */
    return 0;
}

/* Forgets the lazy view at `a` (before its range is released); 0 when there is none. */
int guest_view_unmap(uint32_t a) {
    pthread_mutex_lock(&g_view_lock);
    int found = 0;
    for (int i = 0; i < g_nviews; i++)
        if (g_views[i].len && g_views[i].a == a) {
            pthread_mutex_lock(&g_views[i].lock);   /* waits out a read into the region */
            if (g_view_check) view_protect(a, g_views[i].len, 3);
            __atomic_store_n(&g_views[i].len, 0, __ATOMIC_RELEASE);
            close(g_views[i].fd);
            pthread_mutex_unlock(&g_views[i].lock);
            found = 1;
        }
    pthread_mutex_unlock(&g_view_lock);
    return found;
}

void guest_mem_ranges(uint32_t r[4]) { r[0] = HEAP_LO; r[1] = g_heap_top; r[2] = VALLOC_LO; r[3] = g_valloc_top; }
void guest_mem_stats(uint32_t *heap_used, uint32_t *valloc_used) {
    *heap_used = g_heap_top - HEAP_LO;
    *valloc_used = g_valloc_top - VALLOC_LO;
}

uint32_t guest_strdup(const char *s) {
    uint32_t a = heap_alloc((uint32_t)strlen(s) + 1, 0);
    strcpy((char *)HP(a), s);
    return a;
}

/* ------------------------------------------------------------------ handles */

#define MAX_HANDLES 4096
static WinHandle g_handles[MAX_HANDLES];
static pthread_mutex_t g_handle_lock = PTHREAD_MUTEX_INITIALIZER;

uint32_t handle_new(int type) {
    pthread_mutex_lock(&g_handle_lock);
    for (int i = 16; i < MAX_HANDLES; i++) {
        if (g_handles[i].type == H_FREE) {
            memset(&g_handles[i], 0, sizeof g_handles[i]);
            g_handles[i].type = type;
            g_handles[i].fd = -1;
            pthread_mutex_unlock(&g_handle_lock);
            return 0x1000u + 4u * (uint32_t)i;
        }
    }
    pthread_mutex_unlock(&g_handle_lock);
    fprintf(stderr, "out of handles\n");
    abort();
}

WinHandle *handle_get(uint32_t h, int type) {
    if (h < 0x1000u || (h & 3)) return NULL;
    uint32_t i = (h - 0x1000u) / 4;
    if (i >= MAX_HANDLES || g_handles[i].type == H_FREE) return NULL;
    if (type != -1 && g_handles[i].type != type) return NULL;
    return &g_handles[i];
}

void handle_close(uint32_t h) {
    WinHandle *x = handle_get(h, -1);
    if (!x) return;
    /* A mapping holds its own dup of the file (CreateFileMappingA), released with it as on Windows. Left open, the
       browser's file system keeps the file locked (OPFS refuses removeEntry/move on an open file), so a later
       DeleteFileA/MoveFileA of it fails there, though not natively, and a save that replaces it is lost. Lazy views
       keep their own dup (guest_view_map); copy-in views no longer need the file. */
    if ((x->type == H_FILE || x->type == H_MAPPING) && x->fd >= 0) close(x->fd);
    if (x->type == H_FIND && x->ptr) free(x->ptr);
    x->type = H_FREE;
}

/* ------------------------------------------------------------------ paths */

/* Finds `name` in directory `dir` ignoring case; writes the real name into `out`. */
static int find_ci(const char *dir, const char *name, char *out, size_t n) {
    DIR *d = opendir(*dir ? dir : ".");
    if (!d) return 0;
    struct dirent *e;
    int found = 0;
    while ((e = readdir(d))) {
        if (!strcasecmp(e->d_name, name)) {
            snprintf(out, n, "%s", e->d_name);
            found = 1;
            break;
        }
    }
    closedir(d);
    return found;
}

static char g_cwd[1024] = GUEST_ROOT;  /* the game's current directory, in Windows form */

const char *guest_cwd(void) { return g_cwd; }
void guest_set_cwd(const char *p) { snprintf(g_cwd, sizeof g_cwd, "%s", p); }

int map_path(const char *guest, char *host, size_t n) {
    char full[1024];
    /* make absolute in Windows form */
    if (isalpha((unsigned char)guest[0]) && guest[1] == ':')
        snprintf(full, sizeof full, "%s", guest);
    else if (guest[0] == '\\' || guest[0] == '/')
        snprintf(full, sizeof full, "C:%s", guest);
    else
        snprintf(full, sizeof full, "%s\\%s", g_cwd, guest);
    for (char *p = full; *p; p++) if (*p == '/') *p = '\\';

    const char *rest;
    const char *base;
    size_t rl = strlen(GUEST_ROOT);
    if (!strncasecmp(full, GUEST_ROOT, rl) && (full[rl] == '\\' || full[rl] == 0)) {
        base = g_game_root;
        rest = full + rl;
    } else {
        base = g_user_root;  /* anything outside the game folder: profiles, temp files, ... */
        rest = full + 2;
    }

    char out[1024];
    snprintf(out, sizeof out, "%s", base);
    int exists = 1;
    char comp[512];
    while (*rest) {
        while (*rest == '\\') rest++;
        if (!*rest) break;
        size_t k = 0;
        while (rest[k] && rest[k] != '\\') k++;
        snprintf(comp, sizeof comp, "%.*s", (int)k, rest);
        rest += k;
        if (!strcmp(comp, ".")) continue;
        if (!strcmp(comp, "..")) {
            char *sl = strrchr(out, '/');
            if (sl && sl > out + strlen(base) - 1) *sl = 0;
            continue;
        }
        char real[512];
        if (exists && find_ci(out, comp, real, sizeof real)) {
            size_t L = strlen(out);
            snprintf(out + L, sizeof out - L, "/%s", real);
        } else {
            exists = 0;
            size_t L = strlen(out);
            snprintf(out + L, sizeof out - L, "/%s", comp);
        }
    }
    snprintf(host, n, "%s", out);
    struct stat st;
    return stat(host, &st) == 0;
}

/* ------------------------------------------------------------------ time */

/* In WebAssembly the clocks come from JavaScript, and every call of libc's clock_gettime crosses into JS with
   64-bit arguments and results, which the engine boxes as BigInt objects: two heap allocations per read. The
   game reads the clock millions of times while it boots (its movie player spins on the time until the next
   frame is due), and in Safari that garbage stayed resident for the life of the page: ~500 MB more than the
   same game under Chrome, enough to push an iPhone's web content process over its 2 GB limit (jetsam kill a
   few seconds after Play). emscripten_get_now / emscripten_date_now return a double and allocate nothing. */
#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#include <emscripten/html5.h>   /* emscripten_date_now */
static double wall_ms(void) { return emscripten_date_now(); }
static double mono_s(void) { return emscripten_get_now() * 1e-3; }
#else
static double wall_ms(void) { struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); return ts.tv_sec * 1e3 + ts.tv_nsec * 1e-6; }
static double mono_s(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (double)ts.tv_sec + ts.tv_nsec * 1e-9; }
#endif

uint64_t host_time_100ns(void) {
    return (uint64_t)(wall_ms() * 10000.0) + 11644473600ull * 10000000ull;
}

/* The wall clock as a timespec, for pthread_cond_timedwait deadlines (CLOCK_REALTIME). */
void host_realtime(struct timespec *ts) {
    double ms = wall_ms();
    ts->tv_sec = (time_t)(ms / 1000.0);
    ts->tv_nsec = (long)((ms - (double)ts->tv_sec * 1000.0) * 1e6);
    if (ts->tv_nsec < 0) ts->tv_nsec = 0;
    if (ts->tv_nsec >= 1000000000L) ts->tv_nsec = 999999999L;
}

/* Seconds since the process started, plus a minute: like uptime on a freshly booted PC. Games turn
   GetTickCount/timeGetTime into 32-bit floats, so the values must stay small; the raw monotonic clock
   is epoch-based in WebAssembly (~1.8e12 ms) and broke games' timer scheduling there. */
double host_seconds(void) {
    static double start = -1;
    double now = mono_s();
    if (start < 0) start = now;   /* first call happens during startup, before other threads exist */
    return now - start + 60.0;
}

/* ------------------------------------------------------------------ the game's clock */

/* With RC_DETERMINISTIC=1 the game sees a virtual clock driven by the deterministic scheduler (sched.c):
   it jumps to the next deadline when every thread is blocked, and every read adds 1 us so a spin-wait on
   the clock still makes progress. Present blocks until the next 1/60 s tick (vsync). */
static int g_det = -1;
uint64_t g_vtime_us;             /* virtual microseconds (sched.c advances it) */
uint32_t g_det_sleeps, g_det_waits, g_det_reads;   /* game-thread counters for the hash log */
static GuestThread g_main_thread;

int deterministic(void) {
    if (g_det < 0) g_det = getenv("RC_DETERMINISTIC") != NULL;
    return g_det;
}
/* The refresh rate Present is paced to: the virtual vsync in deterministic mode, headless pacing and
   RC_PACE60. RC_REFRESH_HZ=144 runs the game at 144 frames per second. Netplay stays on 60 Hz frames. */
int refresh_hz(void) {
    static int hz;
    if (!hz) {
        const char *e = getenv("RC_REFRESH_HZ");
        hz = e ? atoi(e) : 60;
        if (hz < 30) hz = 30;
        if (hz > 1000) hz = 1000;
    }
    return hz;
}
int on_game_thread(void) { return t_self == &g_main_thread; }

double guest_seconds(void) {
    if (!deterministic()) return host_seconds();
    if (on_game_thread()) g_det_reads++;
    return 60.0 + (double)__atomic_add_fetch(&g_vtime_us, 1, __ATOMIC_RELAXED) * 1e-6;
}
double guest_peek_seconds(void) {   /* the game's clock without advancing it */
    if (!deterministic()) return host_seconds();
    return 60.0 + (double)__atomic_load_n(&g_vtime_us, __ATOMIC_RELAXED) * 1e-6;
}
uint64_t guest_time_100ns(void) {
    if (!deterministic()) return host_time_100ns();
    /* 2006-01-01 00:00 UTC in FILETIME units, plus the virtual clock */
    return 127804032000000000ull + (uint64_t)((guest_seconds() - 60.0) * 1e7);
}
/* Present: wait for the next vsync tick (exact multiples of 1/60 s); online, the frame lasts the time the
   host measured for it (netplay.c), counted from the previous frame's boundary. */
void vclock_frame(void) {
    if (!deterministic() || !on_game_thread()) return;
    extern int sched_wait(int (*)(void *), void *, uint64_t, uint64_t (*)(void));
    extern uint32_t netplay_frame_dt_us(void);
    uint64_t now = __atomic_load_n(&g_vtime_us, __ATOMIC_RELAXED);
    uint32_t dt = netplay_frame_dt_us();
    if (dt) {
        static uint64_t boundary;
        boundary += dt;
        if (boundary < now) boundary = now;   /* the frame already took longer in game time (loading) */
        sched_wait(NULL, NULL, boundary, NULL);
        return;
    }
    uint64_t hz = (uint64_t)refresh_hz();
    uint64_t k = now * hz / 1000000ull + 1;
    sched_wait(NULL, NULL, (k * 1000000ull + hz - 1) / hz, NULL);
}
static uint64_t vclock_rdtsc(void) { return (uint64_t)(guest_seconds() * 2e9); }   /* 2 GHz, as the registry says */
/* A thread preempted after a full quantum has used CPU time, so the clock moves on by RC_DET_PREEMPT_US
   (default 1000) before the next participant is picked. Without it a thread that spins on memory without
   reading the clock (Cricket's loading screen polling its async loader) never lets a timer come due, and
   the timer callback it waits for never runs. */
static void vclock_preempt(void) {
    static int64_t us = -1;
    if (us < 0) { const char *e = getenv("RC_DET_PREEMPT_US"); us = e ? atoll(e) : 1000; }
    __atomic_add_fetch(&g_vtime_us, (uint64_t)us, __ATOMIC_RELAXED);
    sched_yield_det();
}
void vclock_init(void) {
    extern uint64_t (*g_rdtsc_hook)(void);
    extern int g_preempt_on;
    extern void (*g_preempt_fn)(void);
    if (deterministic()) {
        g_rdtsc_hook = vclock_rdtsc;
        g_preempt_fn = vclock_preempt;
        g_preempt_on = 1;
        fprintf(stderr, "deterministic mode: virtual clock + scheduler\n");
    }
}

/* ------------------------------------------------------------------ threads */

__thread GuestThread *t_self;

GuestThread *thread_main_init(CPU *c) {
    (void)c;
    g_main_thread.id = 0x100;
    g_main_thread.tib = GUEST_TIB;
    t_self = &g_main_thread;
    return t_self;
}
