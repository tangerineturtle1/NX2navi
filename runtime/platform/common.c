// The parts of platform.h built on its locks rather than on the OS: recursive mutexes, one-time initialisation and
// events, and the network address helpers. Every OS shares them.
#include "platform.h"
#include <string.h>

// ---- recursive mutex: owner is the holder's thread id (0 when free); only the holder writes depth.

void plat_rmutex_lock_at(PlatRMutex *m, void *site) {
    uint32_t me = plat_thread_id();
    if (__atomic_load_n(&m->owner, __ATOMIC_RELAXED) == me) {
        m->depth++;
        return;
    }
    plat_mutex_lock_at(&m->m, site);
    __atomic_store_n(&m->owner, me, __ATOMIC_RELAXED);
    m->depth = 1;
}

void plat_rmutex_lock(PlatRMutex *m) { plat_rmutex_lock_at(m, __builtin_return_address(0)); }

int plat_rmutex_trylock(PlatRMutex *m) {
    uint32_t me = plat_thread_id();
    if (__atomic_load_n(&m->owner, __ATOMIC_RELAXED) == me) {
        m->depth++;
        return 1;
    }
    if (!plat_mutex_trylock(&m->m)) return 0;
    __atomic_store_n(&m->owner, me, __ATOMIC_RELAXED);
    m->depth = 1;
    return 1;
}

void plat_rmutex_unlock(PlatRMutex *m) {
    if (--m->depth) return;
    __atomic_store_n(&m->owner, 0, __ATOMIC_RELAXED);
    plat_mutex_unlock(&m->m);
}

// ---- once: state 0 not run, 1 running, 2 done. Waiters for a running initialiser sleep on one process-wide
// condition variable; initialisers are rare and short.

static PlatMutex once_lock = PLAT_MUTEX_INIT;
static PlatCond once_done = PLAT_COND_INIT;

void plat_once(PlatOnce *o, void (*fn)(void *arg), void *arg) {
    if (__atomic_load_n(&o->state, __ATOMIC_ACQUIRE) == 2) return;
    int32_t expected = 0;
    if (__atomic_compare_exchange_n(&o->state, &expected, 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        fn(arg);
        plat_mutex_lock(&once_lock);
        __atomic_store_n(&o->state, 2, __ATOMIC_RELEASE);
        plat_cond_broadcast(&once_done);
        plat_mutex_unlock(&once_lock);
        return;
    }
    plat_mutex_lock(&once_lock);
    while (__atomic_load_n(&o->state, __ATOMIC_ACQUIRE) != 2) plat_cond_wait(&once_done, &once_lock);
    plat_mutex_unlock(&once_lock);
}

// ---- events

void plat_event_init(PlatEvent *e, int manual, int initially_set) {
    plat_mutex_init(&e->m);
    plat_cond_init(&e->c);
    e->manual = manual;
    e->set = initially_set;
}

void plat_event_set(PlatEvent *e) {
    plat_mutex_lock(&e->m);
    e->set = 1;
    if (e->manual) plat_cond_broadcast(&e->c);
    else plat_cond_signal(&e->c);
    plat_mutex_unlock(&e->m);
}

void plat_event_reset(PlatEvent *e) {
    plat_mutex_lock(&e->m);
    e->set = 0;
    plat_mutex_unlock(&e->m);
}

void plat_event_wait(PlatEvent *e) {
    plat_mutex_lock(&e->m);
    while (!e->set) plat_cond_wait(&e->c, &e->m);
    if (!e->manual) e->set = 0;
    plat_mutex_unlock(&e->m);
}

int plat_event_wait_ns(PlatEvent *e, uint64_t ns) {
    uint64_t end = plat_now_ns() + ns;
    plat_mutex_lock(&e->m);
    while (!e->set) {
        uint64_t now = plat_now_ns();
        if (now >= end) break;
        plat_cond_wait_ns(&e->c, &e->m, end - now);
    }
    int got = e->set;
    if (got && !e->manual) e->set = 0;
    plat_mutex_unlock(&e->m);
    return got;
}

// ---- network addresses: the OS-free part of platform.h's UDP sockets. Layout of PlatAddr.b: the IPv6 address at 0 (an
// IPv4 one as ::ffff:a.b.c.d), the port big-endian at 16, the scope id little-endian at 18, zeros after.

void plat_addr_set(PlatAddr *a, const uint8_t ip[16], uint16_t port, uint32_t scope) {
    memset(a, 0, sizeof *a);
    memcpy(a->b, ip, 16);
    a->b[16] = (uint8_t)(port >> 8), a->b[17] = (uint8_t)port;
    for (int i = 0; i < 4; i++) a->b[18 + i] = (uint8_t)(scope >> (8 * i));
}

void plat_addr_get(const PlatAddr *a, uint8_t ip[16], uint32_t *scope) {
    memcpy(ip, a->b, 16);
    if (scope) *scope = (uint32_t)a->b[18] | (uint32_t)a->b[19] << 8 | (uint32_t)a->b[20] << 16 | (uint32_t)a->b[21] << 24;
}

uint16_t plat_addr_port(const PlatAddr *a) { return (uint16_t)(a->b[16] << 8 | a->b[17]); }

int plat_addr_eq(const PlatAddr *a, const PlatAddr *b) { return !memcmp(a, b, sizeof *a); }

static int addr_is_v4(const uint8_t *ip) {
    static const uint8_t mapped[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};
    return !memcmp(ip, mapped, sizeof mapped);
}

int plat_addr_str(const PlatAddr *a, char *out, size_t cap) {
    char s[96];
    int n;
    uint32_t scope;
    uint8_t ip[16];
    plat_addr_get(a, ip, &scope);
    if (addr_is_v4(ip)) {
        n = snprintf(s, sizeof s, "%u.%u.%u.%u:%u", ip[12], ip[13], ip[14], ip[15], plat_addr_port(a));
    } else {
        unsigned g[8];
        for (int i = 0; i < 8; i++) g[i] = (unsigned)ip[2 * i] << 8 | ip[2 * i + 1];
        int best = -1, best_len = 1;   // the longest run of two or more zero groups becomes "::"
        for (int i = 0, run; i < 8; i += run ? run : 1) {
            run = 0;
            while (i + run < 8 && !g[i + run]) run++;
            if (run > best_len) best = i, best_len = run;
        }
        n = snprintf(s, sizeof s, "[");
        for (int i = 0; i < 8; i++) {
            if (i == best) {
                n += snprintf(s + n, sizeof s - (size_t)n, "::");
                i += best_len - 1;
                continue;
            }
            n += snprintf(s + n, sizeof s - (size_t)n, "%s%x", i && i != best + best_len ? ":" : "", g[i]);
        }
        if (scope) n += snprintf(s + n, sizeof s - (size_t)n, "%%%u", scope);
        n += snprintf(s + n, sizeof s - (size_t)n, "]:%u", plat_addr_port(a));
    }
    if (n <= 0 || (size_t)n >= sizeof s || (size_t)n >= cap) return 0;
    memcpy(out, s, (size_t)n + 1);
    return n;
}
