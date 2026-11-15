// Offline network: nn::socket and the nnsocket* C API for a title that brings its network stack up at boot (Switch 2
// titles start it on a worker thread and abort if nn::socket::Initialize fails). This build has no network:
// Initialize succeeds, no socket can be made (ENETDOWN), name lookups fail (EAI_NONAME), and every other call on a
// descriptor that is not an eventfd fails with EBADF. eventfd is real: the stack's worker threads wake each other
// through it and through nnsocketPoll, so a counter with a kernel event each, waits made with WaitSynchronization
// on the calling guest thread (SVC 0x18 through kern_svc, as the SDK's own wait would).
// errno values are the BSD ones the SDK uses (nn::socket::Errno).
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "host.h"
#include "platform/platform.h"

#define E_BADF 9
#define E_INVAL 22
#define E_AGAIN 35
#define E_NETDOWN 50
#define EAI_NONAME_ 8
#define NIFM_R_COMMUNICATION_DISABLED_NET 0x8ae6eu   // nifm module 110, description 1111
#define POLL_IN 0x0001
#define POLL_NVAL 0x0020
#define EFD_SEMAPHORE_ 0x1
#define EFD_BASE 0x4000   // eventfd descriptors: above anything a guest socket table would hand out
#define NEFD 64

typedef struct { int used, flags; uint64_t count; uint32_t w, r; } EventFd;
static EventFd efd[NEFD];
// Kernel events of closed eventfds, reused by the next EventFd. They are never closed: a thread can still be
// blocked on one (EventFdRead, nnsocketPoll) when another closes the descriptor, and a freed handle number is
// soon reused by another kernel object, which corrupted that thread's wait (round 15 crashes).
static struct { uint32_t w, r; } spare[NEFD];
static int nspare;
static PlatMutex lock = PLAT_MUTEX_INIT;
static volatile int32_t last_errno;   // one value for all threads: nothing here succeeds partially
static volatile int32_t initialized;

static int fail(int e) {
    __atomic_store_n(&last_errno, e, __ATOMIC_RELAXED);
    return -1;
}

static EventFd *efd_of(int fd) {
    int i = fd - EFD_BASE;
    return i >= 0 && i < NEFD && efd[i].used ? &efd[i] : NULL;
}

// WaitSynchronization(handles, n, ns) on the calling guest thread. Returns the result; *index the signalled one.
static uint32_t guest_wait(Cpu *c, const uint32_t *hs, int n, int64_t ns, int *index) {
    uint64_t save[4];
    memcpy(save, c->x, sizeof save);
    c->x[1] = (uint64_t)(uintptr_t)hs;
    c->x[2] = (uint64_t)n;
    c->x[3] = (uint64_t)ns;
    if (!kern_svc(c, 0x18)) rt_fatal("net: WaitSynchronization not handled by the kernel");
    uint32_t r = (uint32_t)c->x[0];
    if (index) *index = (int)c->x[1];
    memcpy(c->x, save, sizeof save);
    return r;
}

// ---- nn::socket
// Result Initialize(const Config &) / Result Finalize() / bool IsInitialized()
static void ov_initialize(Cpu *c) {
    if (!plat_xchg32(&initialized, 1)) rt_log("net: socket layer up, offline (no socket can connect)");
    ret_x(c, 0);
}
static void ov_finalize(Cpu *c) { plat_xchg32(&initialized, 0); ret_x(c, 0); }
static void ov_is_initialized(Cpu *c) { ret_x(c, (uint64_t)__atomic_load_n(&initialized, __ATOMIC_RELAXED)); }

// int EventFd(uint64_t initval, EventFdFlags flags)
static void ov_eventfd(Cpu *c) {
    uint64_t init = arg_x(c, 0);
    int flags = (int)arg_x(c, 1);
    plat_mutex_lock(&lock);
    uint32_t w = 0, r = 0;
    if (nspare) {
        nspare--;
        w = spare[nspare].w, r = spare[nspare].r;
    }
    plat_mutex_unlock(&lock);
    if (!w) {
        w = kern_event_new();
        r = kern_event_readable(w);
    } else {
        kern_event_clear(w);
    }
    plat_mutex_lock(&lock);
    int i = 0;
    while (i < NEFD && efd[i].used) i++;
    if (i == NEFD) rt_fatal("net: eventfd table full (%d)", NEFD);
    efd[i] = (EventFd){1, flags, init, w, r};
    plat_mutex_unlock(&lock);
    if (init) kern_event_signal(w);
    static volatile int32_t logged;
    if (!plat_xchg32(&logged, 1)) rt_log("net: eventfd flags %#x (bit 0 taken as semaphore)", flags);
    ret_x(c, (uint64_t)(int64_t)(EFD_BASE + i));
}

// int EventFdWrite(int fd, uint64_t value)
static void ov_eventfd_write(Cpu *c) {
    plat_mutex_lock(&lock);
    EventFd *e = efd_of((int)arg_x(c, 0));
    if (!e) {
        plat_mutex_unlock(&lock);
        ret_x(c, (uint64_t)(int64_t)fail(E_BADF));
        return;
    }
    e->count += arg_x(c, 1);
    uint32_t w = e->w;
    int on = e->count != 0;
    plat_mutex_unlock(&lock);
    if (on) kern_event_signal(w);
    ret_x(c, 0);
}

// int EventFdRead(int fd, uint64_t *value): blocks while the counter is 0 (no nonblocking flag is honoured yet)
static void ov_eventfd_read(Cpu *c) {
    int fd = (int)arg_x(c, 0);
    uint64_t *out = arg_p(c, 1);
    for (;;) {
        plat_mutex_lock(&lock);
        EventFd *e = efd_of(fd);
        if (!e || !out) {
            plat_mutex_unlock(&lock);
            ret_x(c, (uint64_t)(int64_t)fail(e ? E_INVAL : E_BADF));
            return;
        }
        if (e->count) {
            uint64_t v = e->flags & EFD_SEMAPHORE_ ? 1 : e->count;
            e->count -= v;
            if (!e->count) kern_event_clear(e->w);
            plat_mutex_unlock(&lock);
            *out = v;
            ret_x(c, 0);
            return;
        }
        uint32_t r = e->r;
        plat_mutex_unlock(&lock);
        guest_wait(c, &r, 1, -1, NULL);
    }
}

// int SetSockOpt(int, Level, Option, const void *, uint32_t)
static void ov_badf(Cpu *c) { ret_x(c, (uint64_t)(int64_t)fail(E_BADF)); }

// ---- nnsocket* C API
static void ov_socket(Cpu *c) { ret_x(c, (uint64_t)(int64_t)fail(E_NETDOWN)); }
static void ov_close(Cpu *c) {
    int fd = (int)arg_x(c, 0);
    plat_mutex_lock(&lock);
    EventFd *e = efd_of(fd);
    EventFd gone = e ? *e : (EventFd){0};
    if (e) {
        *e = (EventFd){0};
        if (nspare < NEFD) spare[nspare].w = gone.w, spare[nspare].r = gone.r, nspare++;
    }
    plat_mutex_unlock(&lock);
    if (!e) {
        ret_x(c, (uint64_t)(int64_t)fail(E_BADF));
        return;
    }
    kern_event_signal(gone.w);   // wake its waiters: they find the descriptor gone (EBADF) or look again
    ret_x(c, 0);
}
// int nnsocketPoll(pollfd *fds, nfds_t n, int timeout_ms): eventfds report POLLIN while their counter is nonzero;
// any other descriptor reports POLLNVAL.
typedef struct { int32_t fd; int16_t events, revents; } PollFd;
static void ov_poll(Cpu *c) {
    PollFd *p = arg_p(c, 0);
    uint64_t n = arg_x(c, 1);
    int timeout = (int)arg_x(c, 2);
    if (n > 64 || (n && !p)) {
        ret_x(c, (uint64_t)(int64_t)fail(E_INVAL));
        return;
    }
    for (int round = 0;; round++) {
        uint32_t hs[64];
        int nh = 0, ready = 0;
        plat_mutex_lock(&lock);
        for (uint64_t i = 0; i < n; i++) {
            EventFd *e = efd_of(p[i].fd);
            p[i].revents = 0;
            if (p[i].fd < 0) continue;
            if (!e) p[i].revents = POLL_NVAL;
            else if (e->count && (p[i].events & POLL_IN)) p[i].revents = POLL_IN;
            else if (p[i].events & POLL_IN) hs[nh++] = e->r;
            ready += p[i].revents != 0;
        }
        plat_mutex_unlock(&lock);
        if (ready || timeout == 0 || round) {
            ret_x(c, (uint64_t)(int64_t)ready);
            return;
        }
        int64_t ns = timeout < 0 ? -1 : (int64_t)timeout * 1000000;
        if (nh) guest_wait(c, hs, nh, ns, NULL);
        else if (ns > 0) {
            uint64_t save = c->x[0];
            c->x[0] = (uint64_t)ns;
            kern_svc(c, 0x0B);   // SleepThread
            c->x[0] = save;
        } else {
            ret_x(c, 0);   // nothing to wait on, forever: report none instead of hanging
            return;
        }
    }
}
// int nnsocketGetAddrInfo(...) / ...WithOptions(...): no name resolves offline
static void ov_getaddrinfo(Cpu *c) { ret_x(c, EAI_NONAME_); }
static void ov_freeaddrinfo(Cpu *c) { (void)c; }
static const char gai_text[] = "name resolution is unavailable (offline build)";
static void ov_gai_strerror(Cpu *c) { ret_x(c, (uint64_t)(uintptr_t)gai_text); }
static void ov_get_errno(Cpu *c) { ret_x(c, (uint64_t)(int64_t)__atomic_load_n(&last_errno, __ATOMIC_RELAXED)); }
static void ov_set_errno(Cpu *c) { __atomic_store_n(&last_errno, (int32_t)arg_x(c, 0), __ATOMIC_RELAXED); }

HOST_OVERRIDE("_ZN2nn6socket10InitializeERKNS0_6ConfigE", ov_initialize);
HOST_OVERRIDE("_ZN2nn6socket8FinalizeEv", ov_finalize);
HOST_OVERRIDE("_ZN2nn6socket13IsInitializedEv", ov_is_initialized);
HOST_OVERRIDE("_ZN2nn6socket7EventFdEmNS0_12EventFdFlagsE", ov_eventfd);
HOST_OVERRIDE("_ZN2nn6socket11EventFdReadEiPm", ov_eventfd_read);
HOST_OVERRIDE("_ZN2nn6socket12EventFdWriteEim", ov_eventfd_write);
HOST_OVERRIDE("_ZN2nn6socket10SetSockOptEiNS0_5LevelENS0_6OptionEPKvj", ov_badf);
HOST_OVERRIDE("nnsocketSocket", ov_socket);
HOST_OVERRIDE("nnsocketClose", ov_close);
HOST_OVERRIDE("nnsocketPoll", ov_poll);
HOST_OVERRIDE("nnsocketGetAddrInfo", ov_getaddrinfo);
HOST_OVERRIDE("nnsocketGetAddrInfoWithOptions", ov_getaddrinfo);
HOST_OVERRIDE("nnsocketFreeAddrInfo", ov_freeaddrinfo);
HOST_OVERRIDE("nnsocketGAIStrError", ov_gai_strerror);
HOST_OVERRIDE("nnsocketGetLastErrno", ov_get_errno);
HOST_OVERRIDE("nnsocketSetLastErrno", ov_set_errno);
HOST_OVERRIDE("nnsocketAccept", ov_badf);
HOST_OVERRIDE("nnsocketBind", ov_badf);
HOST_OVERRIDE("nnsocketConnect", ov_badf);
HOST_OVERRIDE("nnsocketFcntl", ov_badf);
HOST_OVERRIDE("nnsocketGetPeerName", ov_badf);
HOST_OVERRIDE("nnsocketGetSockName", ov_badf);
HOST_OVERRIDE("nnsocketGetSockOpt", ov_badf);
HOST_OVERRIDE("nnsocketSetSockOpt", ov_badf);
HOST_OVERRIDE("nnsocketListen", ov_badf);
HOST_OVERRIDE("nnsocketRecv", ov_badf);
HOST_OVERRIDE("nnsocketRecvMsg", ov_badf);
HOST_OVERRIDE("nnsocketSend", ov_badf);
HOST_OVERRIDE("nnsocketSendMsg", ov_badf);
HOST_OVERRIDE("nnsocketShutdown", ov_badf);
HOST_OVERRIDE("nnsocketResolverGetOption", ov_badf);
HOST_OVERRIDE("nnsocketResolverSetOption", ov_badf);

// ---- nn::ssl: initialised, never used (no socket connects). Contexts are created and accept imports; the
// built-in CA list is empty: GetBuiltInCertificateBufSize asks for room for one zeroed (terminating) record and
// GetBuiltInCertificates hands back that zeroed record.
#define SSL_EMPTY_LIST 0x40u
static void ov_ssl_ok(Cpu *c) { ret_x(c, 0); }
// Result BuiltInManager::GetBuiltInCertificateBufSize(uint32_t *out, CaCertificateId *ids, uint32_t count)
static void ov_ssl_ca_size(Cpu *c) {
    uint32_t *out = arg_p(c, 0);
    if (out) *out = SSL_EMPTY_LIST;
    ret_x(c, 0);
}
// Result BuiltInManager::GetBuiltInCertificates(BuiltInCertificateInfoBase **out, uint8_t *buf, uint32_t size,
//                                               CaCertificateId *ids, uint32_t count)
static void ov_ssl_ca_list(Cpu *c) {
    void **out = arg_p(c, 0);
    uint8_t *buf = arg_p(c, 1);
    uint32_t size = (uint32_t)arg_x(c, 2);
    if (buf && size) memset(buf, 0, size < SSL_EMPTY_LIST ? size : SSL_EMPTY_LIST);
    if (out) *out = buf;
    ret_x(c, 0);
}
// Result Context::Import{ClientPki,ServerPki,Crl}(uint64_t *id, ...): an id for the caller to keep
static void ov_ssl_import(Cpu *c) {
    uint64_t *id = arg_p(c, 1);
    if (id) *id = 1;
    ret_x(c, 0);
}
HOST_OVERRIDE("_ZN2nn3ssl10InitializeEv", ov_ssl_ok);
HOST_OVERRIDE("_ZN2nn3ssl8FinalizeEv", ov_ssl_ok);
HOST_OVERRIDE("_ZN2nn3ssl14BuiltInManager28GetBuiltInCertificateBufSizeEPjPNS0_15CaCertificateIdEj", ov_ssl_ca_size);
HOST_OVERRIDE("_ZN2nn3ssl14BuiltInManager22GetBuiltInCertificatesEPPNS0_26BuiltInCertificateInfoBaseEPhjPNS0_15CaCertificateIdEj",
              ov_ssl_ca_list);
HOST_OVERRIDE("_ZN2nn3ssl7Context15ImportClientPkiEPmPKcS4_jj", ov_ssl_import);
HOST_OVERRIDE("_ZN2nn3ssl7Context15ImportServerPkiEPmPKcjNS0_17CertificateFormatE", ov_ssl_import);
HOST_OVERRIDE("_ZN2nn3ssl7Context9ImportCrlEPmPKcj", ov_ssl_import);
HOST_OVERRIDE("_ZN2nn3ssl7Context6CreateENS1_10SslVersionE", ov_ssl_ok);
HOST_OVERRIDE("_ZN2nn3ssl7Context7DestroyEv", ov_ssl_ok);

// ---- nn::nsd: the server environment is production ("lp1", as the console answers);
// ResolveEx(Fqdn *out, const Fqdn &in) copies the 0x100-byte name (nothing resolves offline anyway).
static void ov_nsd_environment(Cpu *c) {
    char *out = arg_p(c, 0);
    if (out) {
        memset(out, 0, 8);
        memcpy(out, "lp1", 3);
    }
    ret_x(c, 0);
}
static void ov_nsd_resolve(Cpu *c) {
    char *out = arg_p(c, 0);
    const char *in = arg_p(c, 1);
    if (out && in) memmove(out, in, 0x100);
    ret_x(c, 0);
}
HOST_OVERRIDE("_ZN2nn3nsd24GetEnvironmentIdentifierEPNS0_21EnvironmentIdentifierE", ov_nsd_environment);
HOST_OVERRIDE("_ZN2nn3nsd9ResolveExEPNS0_4FqdnERKS1_", ov_nsd_resolve);

// ---- nn::time::AsyncEnsureNetworkClockAvailabilityTask and nn::account asynchronous NSA calls. The SDK versions
// run as nn::async tasks over the time / account service objects, which the host never creates (it overrides
// nn::time::Initialize and nn::account::Initialize); on the executor thread that corrupted memory at random
// (round 15 crashes). Here a task completes at once and reports the network as unavailable. The object's first
// 8 bytes hold the host record (the task's real size is unknown; every SDK object is larger than a pointer).
#define ACCOUNT_R_NSA_UNAVAILABLE 0x1907cu   // account module 124, description 200: a failure the caller handles
typedef struct { uint8_t event[0x40]; int bound; } HostTask;   // event: an nn::os::SystemEventType (0x30 bytes)

static HostTask *task_of(Cpu *c, void *obj) {
    if (!obj) return NULL;
    HostTask **slot = obj;
    if (!*slot) {
        *slot = calloc(1, sizeof **slot);
        if (!*slot) rt_fatal("net: task allocation");
    }
    if (!(*slot)->bound) {
        kern_bind_system_event(c, (uint64_t)(uintptr_t)(*slot)->event, 0);
        (*slot)->bound = 1;
    }
    return *slot;
}
static void task_done(HostTask *t) {
    if (t) kern_signal_system_event((uint64_t)(uintptr_t)t->event);
}

// AsyncEnsureNetworkClockAvailabilityTask(): C1 / ~: the record
static void ov_clock_task_ctor(Cpu *c) { *(void **)arg_p(c, 0) = NULL; }
static void ov_task_dtor(Cpu *c) {
    void **slot = arg_p(c, 0);
    if (slot && *slot) {
        HostTask *t = *slot;
        // The event lives in this record: its entry in the system-event table goes back before the record is freed,
        // or the table would count it live for the rest of the run (kernel.c kern_unbind_system_event)
        if (t->bound) kern_unbind_system_event(c, (uint64_t)(uintptr_t)t->event);
        free(t);
        *slot = NULL;
    }
}
// Result Initialize(EventClearMode) / Result StartTask(): done at once
static void ov_clock_task_init(Cpu *c) { task_of(c, arg_p(c, 0)); ret_x(c, 0); }
static void ov_clock_task_start(Cpu *c) {
    task_done(task_of(c, arg_p(c, 0)));
    static volatile int32_t logged;
    if (!plat_xchg32(&logged, 1)) rt_log("net: network clock task finished at once (offline)");
    ret_x(c, 0);
}
// SystemEvent *GetFinishEvent(): nn::os::SystemEvent holds its SystemEventType at offset 0
static void ov_clock_task_event(Cpu *c) {
    HostTask *t = task_of(c, arg_p(c, 0));
    ret_x(c, t ? (uint64_t)(uintptr_t)t->event : 0);
}
static void ov_task_result(Cpu *c) { ret_x(c, NIFM_R_COMMUNICATION_DISABLED_NET); }

// account: AsyncContext() / ~AsyncContext(); Result EnsureNetworkServiceAccountIdTokenCacheAsync(AsyncContext *,
// const UserHandle &); Result AsyncContext::GetSystemEvent(SystemEvent *out): out becomes a signalled event;
// Result AsyncContext::GetResult()
static void ov_account_async(Cpu *c) {
    task_done(task_of(c, arg_p(c, 0)));
    ret_x(c, 0);
}
static void ov_account_event(Cpu *c) {
    void *out = arg_p(c, 1);
    if (!out) {
        ret_x(c, ACCOUNT_R_NSA_UNAVAILABLE);
        return;
    }
    kern_bind_system_event(c, (uint64_t)(uintptr_t)out, 0);
    kern_signal_system_event((uint64_t)(uintptr_t)out);
    ret_x(c, 0);
}
static void ov_account_result(Cpu *c) { ret_x(c, ACCOUNT_R_NSA_UNAVAILABLE); }
// Result EnsureNetworkServiceAccountAvailable(const UserHandle &), GetNetworkServiceAccountId(NsaId *, handle),
// LoadNetworkServiceAccountIdTokenCache(size_t *, char *, size_t, handle): no network service account offline
static void ov_account_nsa(Cpu *c) { ret_x(c, ACCOUNT_R_NSA_UNAVAILABLE); }

HOST_OVERRIDE("_ZN2nn4time39AsyncEnsureNetworkClockAvailabilityTaskC1Ev", ov_clock_task_ctor);
HOST_OVERRIDE("_ZN2nn4time39AsyncEnsureNetworkClockAvailabilityTaskD1Ev", ov_task_dtor);
HOST_OVERRIDE("_ZN2nn4time39AsyncEnsureNetworkClockAvailabilityTask10InitializeENS_2os14EventClearModeE",
              ov_clock_task_init);
HOST_OVERRIDE("_ZN2nn4time39AsyncEnsureNetworkClockAvailabilityTask9StartTaskEv", ov_clock_task_start);
HOST_OVERRIDE("_ZN2nn4time39AsyncEnsureNetworkClockAvailabilityTask14GetFinishEventEv", ov_clock_task_event);
HOST_OVERRIDE("_ZN2nn4time39AsyncEnsureNetworkClockAvailabilityTask9GetResultEv", ov_task_result);
HOST_OVERRIDE("_ZN2nn7account12AsyncContextC1Ev", ov_clock_task_ctor);
HOST_OVERRIDE("_ZN2nn7account12AsyncContextD1Ev", ov_task_dtor);
HOST_OVERRIDE("_ZN2nn7account44EnsureNetworkServiceAccountIdTokenCacheAsyncEPNS0_12AsyncContextERKNS0_10UserHandleE",
              ov_account_async);
HOST_OVERRIDE("_ZN2nn7account12AsyncContext14GetSystemEventEPNS_2os11SystemEventE", ov_account_event);
HOST_OVERRIDE("_ZN2nn7account12AsyncContext9GetResultEv", ov_account_result);
HOST_OVERRIDE("_ZN2nn7account36EnsureNetworkServiceAccountAvailableERKNS0_10UserHandleE", ov_account_nsa);
HOST_OVERRIDE("_ZN2nn7account26GetNetworkServiceAccountIdEPNS0_23NetworkServiceAccountIdERKNS0_10UserHandleE",
              ov_account_nsa);
HOST_OVERRIDE("_ZN2nn7account37LoadNetworkServiceAccountIdTokenCacheEPmPcmRKNS0_10UserHandleE", ov_account_nsa);
