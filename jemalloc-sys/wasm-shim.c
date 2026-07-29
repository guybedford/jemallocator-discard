/*
 * Minimal libc/OS shims for building jemalloc on wasm32-unknown-unknown.
 *
 * Deliberately NOT defined here:
 * - memcpy/memmove/memset/memcmp: provided by Rust compiler-builtins.
 *
 * The environment is single-threaded: pthread stubs are no-ops.
 */
#include <stddef.h>
#include <stdint.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include <sched.h>
#include <sys/mman.h>

#define WASM_PAGE 65536UL

/*
 * mmap over memory.grow. Grown regions are 64KiB-aligned which satisfies
 * jemalloc's page alignment (LG_PAGE=16); larger alignment requests take
 * jemalloc's over-allocate-and-trim slow path where the munmap of the
 * excess is a no-op (jemalloc runs with opt.retain so unmap is rare).
 */
void *mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off) {
    (void)addr; (void)prot; (void)flags; (void)fd; (void)off;
    size_t pages = (len + WASM_PAGE - 1) / WASM_PAGE;
    size_t prev = __builtin_wasm_memory_grow(0, pages);
    if (prev == (size_t)-1) return MAP_FAILED;
    return (void *)(prev * WASM_PAGE);
}
int munmap(void *addr, size_t len) { (void)addr; (void)len; return 0; }

/*
 * Page discard via the wasm memory-control proposal. The `__wbindgen_memory_discard`
 * import is replaced by wasm-bindgen with a local function containing the
 * `memory.discard` instruction, so no import survives to instantiation.
 * jemalloc is configured with JEMALLOC_PURGE_MADVISE_DONTNEED (zeroing
 * semantics) and always purges page-aligned ranges (LG_PAGE=16).
 */
__attribute__((import_module("env"), import_name("__wbindgen_memory_discard")))
extern void __wbindgen_memory_discard(void *addr, size_t len);

int madvise(void *addr, size_t len, int advice) {
    if (advice == MADV_DONTNEED) __wbindgen_memory_discard(addr, len);
    return 0;
}

/*
 * jemalloc only consults the clock for decay-based purging; the baked
 * MALLOC_CONF uses dirty_decay_ms:0 (purge on free), so a synthetic
 * monotonic clock is sufficient.
 */
static uint64_t fake_ns = 0;
int clock_gettime(clockid_t c, struct timespec *ts) {
    (void)c;
    fake_ns += 1000000;
    ts->tv_sec = fake_ns / 1000000000;
    ts->tv_nsec = fake_ns % 1000000000;
    return 0;
}

long sysconf(int name) {
    if (name == _SC_PAGESIZE) return 65536;
    if (name == _SC_NPROCESSORS_ONLN) return 1;
    return -1;
}

static int errno_val = 0;
int *__errno_location(void) { return &errno_val; }

char *getenv(const char *n) { (void)n; return 0; }

/*
 * jemalloc reads /proc/sys/vm/overcommit_memory at boot; answer "1" so that
 * os_overcommits=true: all mappings are committed at mmap time and decommit
 * degrades to purging via madvise. Without this jemalloc maps extents
 * uncommitted and commits them with mmap(addr, MAP_FIXED), which the bump
 * mmap above cannot honor, leaking every extent into the retained cache.
 * The only open() caller in this build is that boot-time read.
 */
#define FAKE_OVERCOMMIT_FD 1000
int open(const char *path, int flags, ...) { (void)path; (void)flags; return FAKE_OVERCOMMIT_FD; }
int close(int fd) { (void)fd; return 0; }
ssize_t read(int f, void *b, size_t c) {
    if (f == FAKE_OVERCOMMIT_FD && c >= 1) { *(char *)b = '1'; return 1; }
    (void)b; return -1;
}
ssize_t readlink(const char *p, char *b, size_t s) { (void)p; (void)b; (void)s; return -1; }
ssize_t write(int f, const void *b, size_t c) { (void)f; (void)b; return (ssize_t)c; }
int creat(const char *p, mode_t m) { (void)p; (void)m; return -1; }
pid_t getpid(void) { return 1; }
int atexit(void (*f)(void)) { (void)f; return 0; }
void abort(void) { __builtin_trap(); }
int strerror_r(int e, char *buf, size_t n) { (void)e; if (n) buf[0] = 0; return 0; }

int pthread_mutex_init(pthread_mutex_t *m, const pthread_mutexattr_t *a) { (void)m; (void)a; return 0; }
int pthread_mutex_lock(pthread_mutex_t *m) { (void)m; return 0; }
int pthread_mutex_trylock(pthread_mutex_t *m) { (void)m; return 0; }
int pthread_mutex_unlock(pthread_mutex_t *m) { (void)m; return 0; }
int pthread_mutexattr_init(pthread_mutexattr_t *a) { (void)a; return 0; }
int pthread_mutexattr_settype(pthread_mutexattr_t *a, int t) { (void)a; (void)t; return 0; }
int pthread_mutexattr_destroy(pthread_mutexattr_t *a) { (void)a; return 0; }
int pthread_key_create(pthread_key_t *k, void (*d)(void *)) { (void)k; (void)d; return 0; }
int pthread_setspecific(pthread_key_t k, const void *v) { (void)k; (void)v; return 0; }
pthread_t pthread_self(void) { return (pthread_t)1; }
int pthread_getaffinity_np(pthread_t t, size_t s, cpu_set_t *c) { (void)t; (void)s; (void)c; return -1; }
int __sched_cpucount(size_t s, const cpu_set_t *c) { (void)s; (void)c; return 1; }

void *memchr(const void *s, int c, size_t n) {
    const unsigned char *p = s;
    for (; n--; p++) if (*p == (unsigned char)c) return (void *)p;
    return 0;
}
size_t strlen(const char *s) { const char *p = s; while (*p) p++; return (size_t)(p - s); }
char *strchr(const char *s, int c) { for (;; s++) { if (*s == (char)c) return (char *)s; if (!*s) return 0; } }
int strcmp(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return (unsigned char)*a - (unsigned char)*b; }
int strncmp(const char *a, const char *b, size_t n) {
    while (n && *a && *a == *b) { a++; b++; n--; }
    return n ? (unsigned char)*a - (unsigned char)*b : 0;
}
char *strncpy(char *d, const char *s, size_t n) {
    char *r = d;
    while (n && *s) { *d++ = *s++; n--; }
    while (n--) *d++ = 0;
    return r;
}
char *strtok(char *s, const char *delim) { (void)s; (void)delim; return 0; }
long atol(const char *s) {
    long v = 0; int neg = 0;
    while (*s == ' ') s++;
    if (*s == '-') { neg = 1; s++; } else if (*s == '+') s++;
    while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0');
    return neg ? -v : v;
}
