// The real runtime/rtss_limit.c against a fake RivaTuner Statistics Server: the Win32 calls it makes go to the fakes
// below (its folder in the registry, its hooks library with the profile calls, the profile files, the shared memory
// RTSSSharedMemoryV2), and its worker thread polls when a scenario says so (plat_sleep_ms on that thread waits for the
// next poll). Each scenario runs in a process of its own, since rtss_limit.c's state lasts the process, with the state
// file (rtss-profile.txt) in a folder of its own beside this program; the kill scenarios run two more processes of it,
// a run that is killed and the next start, which share the fake profile through a file there. Windows only; built and
// run by tests/rtss_limit_check.py. Prints the log and each failed check, and exits 1 on any.
//
// rtss_worker_check <scenario>
#include <windows.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

LSTATUS stub_RegGetValueA(HKEY, LPCSTR, LPCSTR, DWORD, LPDWORD, PVOID, LPDWORD);
HMODULE stub_LoadLibraryA(LPCSTR);
FARPROC stub_GetProcAddress(HMODULE, LPCSTR);
BOOL stub_FreeLibrary(HMODULE);
HANDLE stub_OpenFileMappingA(DWORD, BOOL, LPCSTR);
LPVOID stub_MapViewOfFile(HANDLE, DWORD, DWORD, DWORD, SIZE_T);
BOOL stub_UnmapViewOfFile(LPCVOID);
SIZE_T stub_VirtualQuery(LPCVOID, PMEMORY_BASIC_INFORMATION, SIZE_T);
BOOL stub_CloseHandle(HANDLE);
DWORD stub_GetFileAttributesA(LPCSTR);
BOOL stub_GetFileAttributesExA(LPCSTR, GET_FILEEX_INFO_LEVELS, LPVOID);
DWORD stub_GetPrivateProfileStringA(LPCSTR, LPCSTR, LPCSTR, LPSTR, DWORD, LPCSTR);
DWORD stub_GetModuleFileNameA(HMODULE, LPSTR, DWORD);
DWORD stub_GetCurrentProcessId(void);
#define RegGetValueA stub_RegGetValueA
#define LoadLibraryA stub_LoadLibraryA
#define GetProcAddress stub_GetProcAddress
#define FreeLibrary stub_FreeLibrary
#define OpenFileMappingA stub_OpenFileMappingA
#define MapViewOfFile stub_MapViewOfFile
#define UnmapViewOfFile stub_UnmapViewOfFile
#define VirtualQuery stub_VirtualQuery
#define CloseHandle stub_CloseHandle
#define GetFileAttributesA stub_GetFileAttributesA
#define GetFileAttributesExA stub_GetFileAttributesExA
#define GetPrivateProfileStringA stub_GetPrivateProfileStringA
#define GetModuleFileNameA stub_GetModuleFileNameA
#define GetCurrentProcessId stub_GetCurrentProcessId

#include "rtss_limit.c"
#undef GetModuleFileNameA   // this program's own path (main), not the game's

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

// ---- the fake profiles: Global, and the game's own Profiles\ocarina.exe.cfg. LoadProfile copies one into the loaded
// copy (Global for a name without its own), SaveProfile writes the loaded copy as the game's own. Each [Framerate] key
// rtss_limit.c reads is a field; stamp is the file's write time, which every change moves on.
typedef struct {
    int exists;
    DWORD limit, denom;   // FramerateLimit and FramerateLimitDenominator: Limit and LimitDenominator in the file
    int time, time_den, scan0, scan1, sync_display, reflex, passive;
    DWORD stamp;
} Prof;
static Prof global = {1, 0, 1, 0, 0, 0, 0, 0, 0, 0, 1}, own, loaded;
static int no_denominator;   // an RTSS that does not know FramerateLimitDenominator
static int readonly;         // SaveProfile does not reach the file (no administrator rights)
static DWORD refuse_over;    // RTSS refuses a limit above this many frames a second (0: none refused)
static int no_library;       // LoadLibraryA fails
static volatile LONG saves, deletes, updates, loads, created, removed;
static volatile LONG calls;   // every call into the fake hooks library

static void touch(Prof *p) { p->stamp++; }
static void WINAPI f_load(LPCSTR name) {
    (void)name;
    calls++;
    loaded = own.exists ? own : global;
}
static void (*during_save)(void);   // runs inside the next SaveProfile, once
static void WINAPI f_save(LPCSTR name) {
    (void)name;
    calls++, saves++;
    if (during_save) {
        void (*f)(void) = during_save;
        during_save = NULL;
        f();
    }
    if (readonly) return;
    DWORD stamp = own.stamp;
    own = loaded, own.exists = 1, own.stamp = stamp;
    touch(&own);
}
static void WINAPI f_delete(LPCSTR name) {
    (void)name;
    calls++, deletes++;
    if (!readonly) own.exists = 0, touch(&own);
}
static BOOL WINAPI f_get(LPCSTR prop, LPBYTE data, DWORD size) {
    calls++;
    if (size != 4) return FALSE;
    if (!strcmp(prop, "FramerateLimit")) return memcpy(data, &loaded.limit, 4), TRUE;
    if (!strcmp(prop, "FramerateLimitDenominator") && !no_denominator) return memcpy(data, &loaded.denom, 4), TRUE;
    return FALSE;
}
static BOOL WINAPI f_set(LPCSTR prop, LPBYTE data, DWORD size) {
    calls++;
    if (size != 4) return FALSE;
    DWORD v;
    memcpy(&v, data, 4);
    if (!strcmp(prop, "FramerateLimit") && refuse_over && v / (loaded.denom ? loaded.denom : 1) > refuse_over) return FALSE;
    if (!strcmp(prop, "FramerateLimit")) return memcpy(&loaded.limit, data, 4), TRUE;
    if (!strcmp(prop, "FramerateLimitDenominator") && !no_denominator) return memcpy(&loaded.denom, data, 4), TRUE;
    return FALSE;
}
static void WINAPI f_update(void) { calls++, updates++; }
static DWORD WINAPI f_enum(LPSTR list, DWORD size) {
    (void)size;
    calls++;
    list[0] = 0;
    return 1;
}

static Prof *file_of(const char *path) {
    if (strstr(path, "\\Profiles\\ocarina.exe.cfg")) return own.exists ? &own : NULL;
    if (strstr(path, "\\Profiles\\Global")) return &global;
    return NULL;
}

// ---- the fake shared memory: the header and one application entry, this process's (listed) or another's
static uint8_t shm[65536];
static volatile LONG running = 1;
enum { PID = 4242, ENTRY_AT = 0x100, ENTRY_SIZE = 1024 };
static void app_set(int listed, DWORD flags, DWORD t0, DWORD t1, DWORD frames) {
    DWORD h[5] = {0x52545353u, 0x0002000Eu, ENTRY_SIZE, ENTRY_AT, 1};
    memcpy(shm, h, sizeof h);
    uint8_t *e = shm + ENTRY_AT;
    DWORD pid = listed ? PID : 7;
    DWORD t[3] = {t0, t1, frames};
    memcpy(e, &pid, 4);
    memcpy(e + 4 + MAX_PATH, &flags, 4);
    memcpy(e + 4 + MAX_PATH + 4, t, sizeof t);
}
static void app_flags(DWORD flags) { memcpy(shm + ENTRY_AT + 4 + MAX_PATH, &flags, 4); }
static void app_frames(DWORD frames) { memcpy(shm + ENTRY_AT + 4 + MAX_PATH + 4 + 8, &frames, 4); }

// RTSS's clock and the game's presents as rtss_limit.c reads them: each poll() comes 500 ms after the one before, RTSS
// measures a new period of the game's frames at each (counting), and the game presents a millisecond before it unless
// a scenario stops that (presenting).
static uint64_t fake_now = 10000000000ull, hosted_at;
static int counting = 1, presenting = 1;
uint64_t kern_now_ns(void) { return fake_now; }
uint64_t gfx_window_hosted_ns(void) { return presenting ? fake_now - 1000000 : hosted_at; }
static void time_passes(void) {
    if (presenting) hosted_at = fake_now - 1000000;
    fake_now += 500000000ull;
    if (!counting) return;
    DWORD t[2];
    memcpy(t, shm + ENTRY_AT + 4 + MAX_PATH + 4, sizeof t);
    t[0] += 500, t[1] += 500;
    memcpy(shm + ENTRY_AT + 4 + MAX_PATH + 4, t, sizeof t);
}

// The registry and file calls the thread that asks for a limit makes itself: a present must make none.
static DWORD present_tid;
static volatile LONG present_calls, reg_calls;
static int installed = 1;
static void on_call(void) {
    if (GetCurrentThreadId() == present_tid) present_calls++;
}

LSTATUS stub_RegGetValueA(HKEY k, LPCSTR sub, LPCSTR val, DWORD fl, LPDWORD ty, PVOID data, LPDWORD size) {
    (void)k, (void)sub, (void)val, (void)fl, (void)ty;
    on_call();
    reg_calls++;
    if (!installed) return ERROR_FILE_NOT_FOUND;
    snprintf(data, *size, "C:\\FakeRTSS");
    return ERROR_SUCCESS;
}
HMODULE stub_LoadLibraryA(LPCSTR p) {
    (void)p;
    loads++;
    if (no_library) return NULL;
    return (HMODULE)(uintptr_t)0x1000;
}
FARPROC stub_GetProcAddress(HMODULE m, LPCSTR n) {
    (void)m;
    if (!strcmp(n, "EnumProfiles")) return (FARPROC)(void *)f_enum;
    if (!strcmp(n, "LoadProfile")) return (FARPROC)(void *)f_load;
    if (!strcmp(n, "SaveProfile")) return (FARPROC)(void *)f_save;
    if (!strcmp(n, "DeleteProfile")) return (FARPROC)(void *)f_delete;
    if (!strcmp(n, "GetProfileProperty")) return (FARPROC)(void *)f_get;
    if (!strcmp(n, "SetProfileProperty")) return (FARPROC)(void *)f_set;
    if (!strcmp(n, "UpdateProfiles")) return (FARPROC)(void *)f_update;
    return NULL;
}
BOOL stub_FreeLibrary(HMODULE m) { (void)m; return TRUE; }
HANDLE stub_OpenFileMappingA(DWORD a, BOOL b, LPCSTR n) {
    (void)a, (void)b, (void)n;
    return running ? (HANDLE)(uintptr_t)0x2000 : NULL;
}
LPVOID stub_MapViewOfFile(HANDLE h, DWORD a, DWORD b, DWORD c, SIZE_T d) {
    (void)h, (void)a, (void)b, (void)c, (void)d;
    return shm;
}
BOOL stub_UnmapViewOfFile(LPCVOID p) { (void)p; return TRUE; }
SIZE_T stub_VirtualQuery(LPCVOID p, PMEMORY_BASIC_INFORMATION mi, SIZE_T n) {
    (void)p;
    memset(mi, 0, n);
    mi->RegionSize = sizeof shm;
    return n;
}
BOOL stub_CloseHandle(HANDLE h) { (void)h; return TRUE; }
DWORD stub_GetFileAttributesA(LPCSTR p) {
    on_call();
    if (strstr(p, "RTSSHooks64.dll")) return FILE_ATTRIBUTE_NORMAL;
    return file_of(p) ? FILE_ATTRIBUTE_NORMAL : INVALID_FILE_ATTRIBUTES;
}
BOOL stub_GetFileAttributesExA(LPCSTR p, GET_FILEEX_INFO_LEVELS level, LPVOID out) {
    (void)level;
    on_call();
    const Prof *f = file_of(p);
    if (!f) return FALSE;
    WIN32_FILE_ATTRIBUTE_DATA *a = out;
    memset(a, 0, sizeof *a);
    a->ftLastWriteTime.dwLowDateTime = f->stamp;
    return TRUE;
}
DWORD stub_GetPrivateProfileStringA(LPCSTR sec, LPCSTR key, LPCSTR def, LPSTR out, DWORD n, LPCSTR path) {
    on_call();
    const Prof *f = file_of(path);
    long v = 0;
    int have = f && !strcmp(sec, "Framerate");
    if (have) {
        if (!strcmp(key, "Limit")) v = (long)f->limit;
        else if (!strcmp(key, "LimitDenominator")) v = (long)f->denom;
        else if (!strcmp(key, "LimitTime")) v = f->time;
        else if (!strcmp(key, "LimitTimeDenominator")) v = f->time_den;
        else if (!strcmp(key, "SyncScanline0")) v = f->scan0;
        else if (!strcmp(key, "SyncScanline1")) v = f->scan1;
        else if (!strcmp(key, "SyncDisplay")) v = f->sync_display;
        else if (!strcmp(key, "ReflexSleep")) v = f->reflex;
        else if (!strcmp(key, "PassiveWait")) v = f->passive;
        else have = 0;
    }
    if (have) return (DWORD)snprintf(out, n, "%ld", v);
    return (DWORD)snprintf(out, n, "%s", def);
}
DWORD stub_GetModuleFileNameA(HMODULE m, LPSTR out, DWORD n) {
    (void)m;
    on_call();
    return (DWORD)snprintf(out, n, "C:\\game\\ocarina.exe");
}
DWORD stub_GetCurrentProcessId(void) { return PID; }

// ---- host.h and platform.h, as far as rtss_limit.c uses them
static char logged_lines[1 << 16];
static size_t logged_at;
static int quiet;   // the stress scenario: no lines kept or printed
void rt_log(const char *fmt, ...) {
    if (quiet) return;
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (strstr(line, " created;")) created++;
    if (strstr(line, " removed,")) removed++;
    int k = snprintf(logged_lines + logged_at, sizeof logged_lines - logged_at, "%s\n", line);
    if (k > 0 && logged_at + (size_t)k < sizeof logged_lines) logged_at += (size_t)k;
    printf("    log: %s\n", line);
}
static int logged(const char *text) { return strstr(logged_lines, text) != NULL; }
int rt_knob(const char *name, const char *def) { (void)name, (void)def; return 0; }

// The state file next to the executable: here the scenario's folder.
static char exe_dir[MAX_PATH];
static int state_readonly;   // the state file cannot be written
static int state_unreadable;   // the state file is there but does not read (another program holds it)
static int no_exe_path;      // the executable's path is unknown: no state file at all
int plat_exe_path(char *out, size_t cap) {
    int k = snprintf(out, cap, "%s\\ocarina.exe", exe_dir);
    return !no_exe_path && k > 0 && (size_t)k < cap;
}
static uint8_t *read_all(const char *path, uint64_t *size) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = n > 0 ? malloc((size_t)n + 1) : NULL;
    if (b && fread(b, 1, (size_t)n, f) != (size_t)n) free(b), b = NULL;
    fclose(f);
    if (b) b[n] = 0, *size = (uint64_t)n;
    return b;
}
uint8_t *plat_file_read(const char *path, uint64_t *size) { return state_unreadable ? NULL : read_all(path, size); }
int64_t plat_file_size(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fclose(f);
    return n;
}
int plat_file_write_atomic(const char *path, const void *data, size_t n) {
    if (state_readonly) return 0;
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    int ok = fwrite(data, 1, n, f) == n;
    return !fclose(f) && ok;
}
void plat_file_delete(const char *path) { remove(path); }
int plat_file_exists(const char *path) {
    FILE *f = fopen(path, "rb");
    if (f) fclose(f);
    return f != NULL;
}
uint32_t plat_thread_id(void) { return GetCurrentThreadId(); }
static char state_text[4096];
static const char *state(void) {   // the state file as it is now; "" when there is none
    char path[MAX_PATH + 32];
    snprintf(path, sizeof path, "%s\\rtss-profile.txt", exe_dir);
    uint64_t n = 0;
    uint8_t *b = read_all(path, &n);
    snprintf(state_text, sizeof state_text, "%s", b ? (char *)b : "");
    free(b);
    return state_text;
}
static int state_has(const char *line) { return strstr(state(), line) != NULL; }

static DWORD worker_tid;
static HANDLE tick, done, at_lock, go;
static volatile LONG pause_at_lock;   // the worker's next plat_mutex_lock stops before it takes the lock

void plat_mutex_lock(PlatMutex *m) {
    if (GetCurrentThreadId() == worker_tid && InterlockedExchange(&pause_at_lock, 0)) {
        SetEvent(at_lock);
        WaitForSingleObject(go, INFINITE);
    }
    AcquireSRWLockExclusive((PSRWLOCK)m);
}
void plat_mutex_unlock(PlatMutex *m) { ReleaseSRWLockExclusive((PSRWLOCK)m); }
int plat_mutex_trylock(PlatMutex *m) { return TryAcquireSRWLockExclusive((PSRWLOCK)m) != 0; }

static PlatThreadFn worker_fn;
static HANDLE worker_thread;
static volatile LONG spawned;
static volatile LONG free_run;   // the worker polls without waiting
static volatile LONG64 polls;
static DWORD WINAPI worker_main(void *arg) {
    worker_tid = GetCurrentThreadId();
    worker_fn(arg);
    return 0;
}
int plat_thread_spawn(PlatThreadFn fn, void *arg, const char *name, size_t stack) {
    (void)name, (void)stack;
    worker_fn = fn;
    spawned = 1;
    worker_thread = CreateThread(NULL, 0, worker_main, arg, 0, NULL);
    return worker_thread != NULL;
}
void plat_sleep_ms(uint32_t ms) {
    if (GetCurrentThreadId() != worker_tid) {
        Sleep(ms);
        return;
    }
    InterlockedIncrement64(&polls);
    if (free_run) return;
    SetEvent(done);                      // a poll ended
    WaitForSingleObject(tick, INFINITE);   // the next one starts when the scenario says
}

// The fake profile kept across the processes of a kill scenario.
static void prof_save(void) {
    char path[MAX_PATH + 16];
    snprintf(path, sizeof path, "%s\\own.bin", exe_dir);
    FILE *f = fopen(path, "wb");
    if (f) fwrite(&own, sizeof own, 1, f), fclose(f);
}
static void prof_load(void) {
    char path[MAX_PATH + 16];
    snprintf(path, sizeof path, "%s\\own.bin", exe_dir);
    FILE *f = fopen(path, "rb");
    if (f) {
        if (fread(&own, sizeof own, 1, f) != 1) memset(&own, 0, sizeof own);
        fclose(f);
    }
}

// ---- the scenarios' steps
static int wait_done(void) { return WaitForSingleObject(done, 5000) == WAIT_OBJECT_0; }
static void poll(void) {   // one more poll of the worker, waited for
    time_passes();
    SetEvent(tick);
    CHECK(wait_done(), "the worker did not finish a poll");
}
static void ask(int64_t mhz) {   // a want at a present; the first one that starts the worker waits for its first poll
    int was = spawned;
    rtss_limit_want(mhz);
    if (spawned && !was) {
        HANDLE h[2] = {done, worker_thread};   // or for its end, when it finds no RTSS
        CHECK(WaitForMultipleObjects(2, h, FALSE, 5000) != WAIT_TIMEOUT, "the worker did not finish its first poll");
    }
}
static void settle(int64_t mhz) {   // a new want, then the polls it takes to reach the profile (RTSS_SETTLE_POLLS)
    ask(mhz);
    poll();
    poll();
}
static void show(const char *what) {
    printf("  %s: profile %s %lu/%lu; active %d; state %d; saves %ld deletes %ld updates %ld loads %ld\n", what,
           own.exists ? "exists," : "absent,", own.limit, own.denom, rtss_limit_active(), rtss_limit_state(), saves,
           deletes, updates, loads);
}

// The exit puts the profile back (rtss_limit_restore) and the game presents on until the process ends: a want after it
// never reaches the profile, and the pacer paces again.
static void restore_race(void) {
    app_set(1, 0xA, 1000, 2000, 60);
    ask(60000);
    poll();
    show("playing at 60");
    CHECK(own.exists && own.limit == 60, "the limit went into the profile");
    rtss_limit_restore();
    show("after rtss_limit_restore");
    CHECK(!own.exists, "the profile the game created is gone after the restore");
    CHECK(!rtss_limit_active() && !rtss_limit_state(), "the game's limiter paces once the profile is back");
    ask(60000);   // frame_step_cap_ns at a present after the restore
    poll();
    poll();
    show("two polls later");
    CHECK(!own.exists, "the profile stays as it was put back");
    CHECK(created == 1, "the profile was created %ld times", created);
    CHECK(!rtss_limit_active() && !rtss_limit_state(), "the game's limiter paces until the end");
}

// The same with the player's own profile: it keeps the player's limit.
static void restore_player(void) {
    own = (Prof){1, 30, 1, 0, 0, 0, 0, 0, 0, 0, 1};
    app_set(1, 0xA, 1000, 2000, 60);
    ask(60000);
    poll();
    CHECK(own.limit == 60, "the game's limit went into the profile");
    rtss_limit_restore();
    CHECK(own.exists && own.limit == 30, "the player's limit is back after the restore");
    ask(144000);
    poll();
    show("a want and a poll after the restore");
    CHECK(own.exists && own.limit == 30 && own.denom == 1, "the profile keeps the player's limit");
}

// A want the worker read before the restore and takes the lock with after it: it must not reach the profile.
static void restore_inflight(void) {
    app_set(1, 0xA, 1000, 2000, 60);
    ask(60000);
    poll();
    pause_at_lock = 1;
    rtss_limit_want(144000);
    SetEvent(tick);
    CHECK(WaitForSingleObject(at_lock, 5000) == WAIT_OBJECT_0, "the worker did not reach its lock");
    rtss_limit_restore();   // the worker read 144000 and waits for the lock
    SetEvent(go);
    CHECK(wait_done(), "the worker did not finish the poll");
    show("the poll that read its want before the restore");
    CHECK(!own.exists, "the profile stays as it was put back");
    CHECK(created == 1, "the profile was created %ld times", created);
}

// An exit before the first want, and one while RTSS was kept out: a want after it does not reach the profile.
static void restore_first(void) {
    app_set(1, 0xA, 1000, 2000, 60);
    rtss_limit_restore();
    ask(60000);
    if (spawned) poll();
    show("a want after an exit before the first one");
    CHECK(!own.exists && !created, "no profile written after the exit");
    CHECK(!rtss_limit_active(), "the game's limiter paces");
}
static void restore_kept_out(void) {
    app_set(1, 0xA, 1000, 2000, 60);
    ask(RTSS_OUT_SETTING);
    rtss_limit_restore();
    ask(60000);
    if (spawned) poll();
    show("a want after an exit while RTSS was kept out");
    CHECK(!own.exists && !created, "no profile written after the exit");
    CHECK(!rtss_limit_active(), "the game's limiter paces");
}

// NX2NAVI_RTSS=0 with a limit set in RTSS's Global profile: the worker only reads, never loads the hooks library, and
// tells of the two limiters until the player clears that limit.
static void kept_two(void) {
    global.limit = 50;
    app_set(1, 0xA, 1000, 2000, 50);
    ask(RTSS_OUT_SETTING);
    poll();
    show("kept out by the setting, Global limit 50");
    CHECK(spawned, "a kept-out first want starts the worker");
    CHECK(!loads && !saves && !deletes, "kept out, the hooks library is never loaded and no profile written");
    CHECK(rtss_limit_state() == 4 && !rtss_limit_active(), "the menu says two limiters; the game's limiter paces");
    CHECK(logged("kept out: NX2NAVI_RTSS=0"), "the kept-out line names the setting");
    CHECK(logged("two limiters: RivaTuner Statistics Server's own setting for the game (Profiles\\Global: limit 50/1, "
                 "frame time limit 0/0, scanline sync 0/0) limits it beside the game's limiter"),
          "the two-limiter line names the file and the values");
    global.limit = 0, touch(&global);
    poll();
    show("the player cleared RTSS's limit");
    CHECK(rtss_limit_state() == 0, "one limiter again");
    CHECK(logged("RivaTuner Statistics Server no longer limits the game beside the game's limiter"), "logged once it ended");
}

// Low latency keeps RTSS out from the first present, with scanline sync set in the game's own profile (a negative line
// counts from the bottom); once low latency is off, a limit starts RTSS's part as usual.
static void kept_lowlat_then_limit(void) {
    own = (Prof){1, 0, 1, 0, 0, -40, 0, 0, 0, 0, 1};
    app_set(0, 0xA, 1000, 2000, 60);
    ask(RTSS_OUT_LOWLAT);
    poll();
    CHECK(rtss_limit_state() == 0, "not listed yet: RTSS holds nothing");
    app_set(1, 0xA, 1000, 2000, 60);
    poll();
    show("listed, kept out by low latency");
    CHECK(rtss_limit_state() == 4, "scanline sync -40 holds the game: two limiters");
    CHECK(logged("kept out: the driver's low latency"), "the kept-out line names low latency");
    CHECK(logged("(Profiles\\ocarina.exe.cfg: limit 0/1, frame time limit 0/0, scanline sync -40/0) limits it beside "
                 "the driver's low latency"),
          "the two-limiter line names the game's profile and low latency");
    CHECK(!loads, "kept out, the hooks library is never loaded");
    settle(60000);
    show("low latency off, limit 60");
    CHECK(loads == 1 && own.exists && own.limit == 60, "the limit goes into the profile once RTSS is let in");
    poll();
    CHECK(rtss_limit_state() == 1 && rtss_limit_active(), "RTSS limits the game from the next poll");
}

// Frame generation keeps RTSS out, with no limit of RTSS's own: one limiter, and the line names frame generation.
static void kept_framegen(void) {
    app_set(1, 0xA, 1000, 2000, 60);
    ask(RTSS_OUT_FRAMEGEN);
    poll();
    show("kept out by frame generation");
    CHECK(logged("kept out: frame generation"), "the kept-out line names frame generation");
    CHECK(!logged("two limiters") && rtss_limit_state() == 0, "no second limiter");
    CHECK(!loads && !saves, "kept out, the hooks library is never loaded");
}

// Frame generation switched on while the worker polls without pause: from the switch until the worker sees it, the
// game's limiter paces, never RTSS's hold of the limit before (the worker's store for a want that changed after it
// read it must not count for the new one).
static void active_race(void) {
    no_exe_path = 1;   // 400000 writes and removals of the state file would take minutes
    app_set(1, 0xA, 1000, 2000, 60);
    ask(60000);
    poll();
    poll();
    CHECK(rtss_limit_active(), "RTSS limits at 60");
    quiet = 1, free_run = 1;
    SetEvent(tick);
    long hits = 0, n = 400000, stuck = 0;
    srand(7);
    for (long i = 0; i < n && !stuck; i++) {
        for (volatile int k = rand() % 200; k > 0; k--) {}
        rtss_limit_want(RTSS_OUT_FRAMEGEN);
        LONG64 p0 = polls;
        int seen = 0;
        while (polls < p0 + 2) seen |= rtss_limit_active();
        hits += seen;
        rtss_limit_want(60000);
        ULONGLONG t0 = GetTickCount64();
        while (!rtss_limit_active() && !stuck) stuck = GetTickCount64() - t0 > 5000;
    }
    quiet = 0;
    printf("  %ld of %ld switches read RTSS's hold afterwards (worker polls %lld)\n", hits, n, (long long)polls);
    CHECK(!stuck, "RTSS did not take the limit back");
    CHECK(!hits, "the game's limiter stood aside for a kept-out want %ld times", hits);
}

// Frame generation switched on while the worker writes the first limit: RTSS holds that limit, but the game's limiter
// paces the kept-out want, so the log must not say RTSS does the limiting; the next poll logs the kept-out line. A
// later limit is held and logged as usual.
static void framegen_on(void) { rtss_limit_want(RTSS_OUT_FRAMEGEN); }
static void active_logged(void) {
    app_set(1, 0xA, 1000, 2000, 60);
    during_save = framegen_on;
    ask(60000);
    poll();
    show("frame generation switched on during the first write");
    CHECK(own.exists && own.limit == 60, "the limit went into the profile");
    CHECK(!rtss_limit_active(), "the game's limiter paces the kept-out want");
    CHECK(!logged("does the limiting"), "no line says RTSS does the limiting");
    poll();
    show("the next poll");
    CHECK(logged("the game's own limiter (RivaTuner Statistics Server kept out: frame generation)"),
          "the kept-out line");
    CHECK(!logged("does the limiting"), "still no line says RTSS does the limiting");
    ask(60000);
    poll();
    poll();
    show("frame generation off");
    CHECK(rtss_limit_active() && logged("does the limiting; the game's own limiter stands aside"), "held and logged");
}

// A limit of a fractional monitor (display at 59.94 and 143.856 Hz): an RTSS that knows the denominator holds it as it
// is, one that does not holds it rounded down to whole frames, never above the refresh, and the log says what the
// profile holds, not what was asked.
static void round_logged(void) {
    app_set(1, 0xA, 1000, 2000, 60);
    ask(59940);
    poll();
    show("display at 59.94 Hz");
    CHECK(own.limit == 59940 && own.denom == 1000, "the denominator holds the fraction");
    CHECK(logged("profile ocarina.exe limits to 59.940 fps\n"), "the exact limit as it is");
}
static void round_logged_whole(void) {
    no_denominator = 1;
    app_set(1, 0xA, 1000, 2000, 60);
    ask(59940);
    poll();
    show("display at 59.94 Hz, RTSS without a denominator");
    CHECK(own.limit == 59, "rounded down to whole frames");
    CHECK(logged("profile ocarina.exe limits to 59/1 (59.000 fps; the game asked 59.940)"), "the log names 59/1");
    settle(143856);
    CHECK(own.limit == 143, "143.856 rounded down");
    CHECK(logged("profile ocarina.exe limits to 143/1 (143.000 fps; the game asked 143.856)"), "the log names 143/1");
    settle(60000);
    CHECK(logged("profile ocarina.exe limits to 60.000 fps\n"), "a whole limit as it is");
}

// The first want comes at a present: the worker, not the present, reads RTSS's registry key and looks for its files.
static void start_off_present(void) {
    present_tid = GetCurrentThreadId();
    app_set(1, 0xA, 1000, 2000, 60);
    ask(60000);
    poll();
    show("playing at 60");
    CHECK(own.exists && own.limit == 60, "the limit went into the profile");
    CHECK(reg_calls == 1, "RTSS's folder was looked up once (%ld)", reg_calls);
    CHECK(!present_calls, "the presenting thread made %ld registry or file calls", present_calls);
}

// No RTSS: the worker says so and ends, the game's limiter paces, and the exit has nothing to put back.
static void not_installed(void) {
    installed = 0;
    ask(60000);
    CHECK(spawned && WaitForSingleObject(worker_thread, 5000) == WAIT_OBJECT_0, "the worker ended");
    CHECK(logged("RivaTuner Statistics Server is not installed; the game's own limiter"), "logged");
    CHECK(!rtss_limit_active() && !rtss_limit_state(), "the game's limiter paces");
    ask(144000);
    rtss_limit_restore();
    CHECK(!loads && !saves && !own.exists, "nothing loaded or written");
}

// While RTSS lists the game, its settings for the game are logged once per change: Global's before the game has a
// profile of its own, then the game's own with the limit it wrote, frame time limit, scanline sync and the modes.
static int count(const char *text) {
    int n = 0;
    for (const char *p = logged_lines; (p = strstr(p, text)); p++) n++;
    return n;
}
static void settings_logged(void) {
    global.time = 16667, global.time_den = 1, global.sync_display = 1, global.reflex = 1, global.passive = 1;
    app_set(0, 0xA, 1000, 2000, 60);
    ask(60000);
    poll();
    CHECK(!count("settings for the game"), "not listed: nothing read");
    app_set(1, 0xA, 1000, 2000, 60);
    poll();
    poll();
    CHECK(logged("frame rate: RivaTuner Statistics Server's settings for the game (Profiles\\ocarina.exe.cfg): limit "
                 "60/1, frame time limit 16667/1, scanline sync 0/0, display sync 1, Reflex sleep 1, passive wait 1\n"),
          "the game's profile, with what it took from Global");
    CHECK(count("settings for the game") == 1, "logged once while nothing changes (%d)", count("settings for the game"));
    ask(144000);
    poll();
    poll();
    CHECK(logged("(Profiles\\ocarina.exe.cfg): limit 144/1, frame time limit 16667/1"), "logged again on a change");
    CHECK(count("settings for the game") == 2, "once per change (%d)", count("settings for the game"));
}

// The decisions as they stand, pinned against the real worker (rtss_limit_check.c pins the same in rtss_decide.h).

// Profiles under Program Files without administrator rights: the save does not stick, and RTSS is out for the run.
static void readonly_run(void) {
    readonly = 1;
    app_set(1, 0xA, 1000, 2000, 60);
    ask(60000);
    poll();
    show("read-only profiles");
    CHECK(rtss_limit_state() == 2 && !rtss_limit_active(), "the menu says RTSS needs administrator rights");
    ask(144000);
    poll();
    poll();
    CHECK(saves == 1, "no write after the failure (%ld saves)", saves);
    CHECK(logged("RivaTuner Statistics Server's profile cannot be changed"), "logged");
}

// The same with a limit set in RTSS's Global profile: RTSS, out of the game's limit for the run, still holds the game
// by its own setting, and the log tells of the two limiters; the menu keeps its administrator note.
static void readonly_two(void) {
    readonly = 1;
    global.limit = 50;
    app_set(1, 0xA, 1000, 2000, 50);
    ask(60000);
    poll();
    show("read-only profiles, Global limit 50");
    CHECK(rtss_limit_state() == 2 && !rtss_limit_active(), "the menu says RTSS needs administrator rights");
    CHECK(logged("two limiters: RivaTuner Statistics Server's own setting for the game (Profiles\\Global: limit 50/1, "
                 "frame time limit 0/0, scanline sync 0/0) limits it beside the game's limiter"),
          "the two-limiter line names the file and the values");
    ask(RTSS_OUT_LOWLAT);
    poll();
    CHECK(logged("scanline sync 0/0) limits it beside the driver's low latency"), "logged again for the other limiter");
    CHECK(rtss_limit_state() == 4, "kept out: the menu's two-limiter note");
    ask(60000);
    poll();
    global.limit = 0, touch(&global);
    poll();
    CHECK(logged("RivaTuner Statistics Server no longer limits the game beside the game's limiter"),
          "logged once it ended");
    CHECK(rtss_limit_state() == 2, "the administrator note again");
}

// RTSS's hooks library does not load, with a limit set in RTSS's Global profile: the log tells of the two limiters.
static void library_two(void) {
    no_library = 1;
    global.limit = 50;
    app_set(1, 0xA, 1000, 2000, 50);
    ask(60000);
    poll();
    show("no hooks library, Global limit 50");
    CHECK(!rtss_limit_active() && !saves, "the game's limiter");
    CHECK(logged("two limiters: RivaTuner Statistics Server's own setting for the game (Profiles\\Global: limit 50/1, "
                 "frame time limit 0/0, scanline sync 0/0) limits it beside the game's limiter"),
          "the two-limiter line");
}

// RTSS refuses a limit out of its range: the game keeps that one, the earlier limit of this run is cleared to none,
// and a limit in range goes to RTSS again at once.
static void refused_run(void) {
    refuse_over = 200;
    app_set(1, 0xA, 1000, 2000, 60);
    ask(60000);
    poll();
    poll();
    CHECK(own.limit == 60 && rtss_limit_active(), "60 taken");
    settle(240000);
    show("240 refused");
    CHECK(own.exists && own.limit == 0, "the earlier limit of this run is cleared to none");
    CHECK(!rtss_limit_active() && rtss_limit_state() == 0, "the game's limiter keeps 240");
    long s0 = saves;
    poll();
    poll();
    CHECK(saves == s0, "the refused limit is not written again");
    settle(144000);
    show("144 after the refusal");
    CHECK(own.limit == 144, "144 written once it stayed");
    poll();
    CHECK(rtss_limit_active(), "and held from the next poll");
    ask(240000);
    CHECK(!rtss_limit_active(), "240 with the game at once");
    poll();
    CHECK(!rtss_limit_active() && own.limit == 144, "240 stays with the game");
}

// RTSS lists the game but lets it run well over the limit: three fresh periods in a row over 1.05 times the limit, each
// after a poll that stood aside, put the profile back and the game limits for the run.
static void over_run(void) {
    app_set(1, 0xA, 1000, 2000, 144);
    ask(60000);
    for (int i = 0; i < 4; i++) poll();
    CHECK(rtss_limit_active() && own.exists, "two periods over: RTSS still limits");
    poll();
    show("three periods at 144 fps with the limit at 60");
    CHECK(rtss_limit_state() == 3 && !rtss_limit_active(), "the menu says RTSS does not limit the game");
    CHECK(!own.exists, "the profile is put back");
    CHECK(logged("RivaTuner Statistics Server measures 144.0 fps with its limit at 60.000 fps"), "logged");
    settle(30000);
    CHECK(saves == 1 && rtss_limit_state() == 3, "out for the run");
}
// 1.05 times the limit is not caught; a period RTSS no longer moves (the limit lowered, then the window minimised)
// counts neither way, and the game's limiter holds meanwhile; 1.15 times the limit is caught.
static void over_slack_run(void) {
    app_set(1, 0xA, 1000, 2000, 63);
    ask(60000);
    for (int i = 0; i < 20; i++) poll();
    show("20 polls at 63 fps with the limit at 60");
    CHECK(rtss_limit_active() && rtss_limit_state() == 1, "1.05 times the limit is not caught");
    counting = 0;
    app_frames(144);
    for (int i = 0; i < 20; i++) poll();
    show("a period frozen at 144 fps for 20 polls");
    CHECK(!rtss_limit_active() && rtss_limit_state() != 3 && own.exists && own.limit == 60, "never counted");
    CHECK(logged("the game's own limiter (RivaTuner Statistics Server has measured no frames of the game lately)"),
          "logged");
    counting = 1;
    app_frames(69);
    for (int i = 0; i < 6; i++) poll();
    CHECK(rtss_limit_state() == 3, "1.15 times the limit is caught");
}

// RTSS lists the game before it has seen a Vulkan present, and asks the game to read its profile again after each
// write: the game's limiter holds until both are done.
static void api_run(void) {
    app_set(1, 0, 1000, 2000, 60);
    ask(60000);
    for (int i = 0; i < 3; i++) poll();
    show("listed, no API yet");
    CHECK(!rtss_limit_active() && own.limit == 60, "the game's limiter");
    CHECK(logged("the game's own limiter (RivaTuner Statistics Server lists the game but has not seen its Vulkan "
                 "presents yet)"),
          "logged");
    app_flags(0xA | 0x10000000u);
    poll();
    CHECK(!rtss_limit_active() && logged("has not read the game's limit from its profile yet"), "the update asked");
    app_flags(0xA);
    poll();
    show("Vulkan, profile read");
    CHECK(rtss_limit_active() && saves == 1, "RTSS limits, no second write");
}

// RTSS lists the game with the limit and pictures go out, but it never reads the profile again (its update flag stays
// set): after 30 s the profile goes back, the menu says RTSS does not limit the game, and the game's limiter holds for
// the run.
static void unproven_run(void) {
    app_set(1, 0xA | 0x10000000u, 1000, 2000, 60);
    ask(60000);
    poll();
    CHECK(own.exists && own.limit == 60, "written");
    for (int i = 0; i < RTSS_UNPROVEN_POLLS - 1; i++) poll();
    show("29.5 s with the update flag set");
    CHECK(own.exists && rtss_limit_state() == 0 && !rtss_limit_active(), "not yet");
    poll();
    show("30 s");
    CHECK(!own.exists && rtss_limit_state() == 3 && !rtss_limit_active(), "put back; the menu's note");
    CHECK(logged("RivaTuner Statistics Server listed the game with its limit at 60.000 fps for 30 s and never showed "
                 "it holds it; the game's own limiter for the rest of the run"),
          "logged");
    CHECK(logged("the game's own limiter (RivaTuner Statistics Server never showed it holds the game)"), "the reason");
    settle(144000);
    CHECK(saves == 1 && !own.exists && rtss_limit_state() == 3, "out for the run");
}

// No picture goes out (a minimised window): RTSS has nothing to hold, and the game's limiter holds until pictures go
// out again; the profile stays as it is.
static void idle_run(void) {
    app_set(1, 0xA, 1000, 2000, 60);
    ask(60000);
    poll();
    poll();
    CHECK(rtss_limit_active(), "RTSS limits");
    presenting = 0;
    poll();
    show("minimised");
    CHECK(!rtss_limit_active() && logged("the game's own limiter (no picture went out lately: RivaTuner has none to hold)"),
          "the game's limiter");
    presenting = 1;
    poll();
    show("back");
    CHECK(rtss_limit_active() && saves == 1, "RTSS again, no write");
}
static void over_unlimited_run(void) {
    app_set(1, 0xA, 1000, 2000, 700);
    ask(0);
    for (int i = 0; i < 10; i++) poll();
    show("unlimited at 700 fps");
    CHECK(rtss_limit_active() && rtss_limit_state() == 0, "no limit, nothing to check");
}

// RTSS quits while it limits and comes back: the profile stays for its return, no write either way.
static void quit_run(void) {
    app_set(1, 0xA, 1000, 2000, 60);
    ask(60000);
    poll();
    running = 0;
    poll();
    show("RTSS quit");
    CHECK(!rtss_limit_active() && own.exists && own.limit == 60, "the game limits; the profile stays");
    running = 1;
    poll();
    show("RTSS back");
    CHECK(rtss_limit_active() && saves == 1, "RTSS limits again with no new write");
}

// RTSS's hooks library does not load: the game limits for the run.
static void library_run(void) {
    no_library = 1;
    app_set(1, 0xA, 1000, 2000, 60);
    ask(60000);
    poll();
    ask(144000);
    poll();
    show("no hooks library");
    CHECK(loads == 1 && !rtss_limit_active() && !saves, "one try, then the game's limiter for the run");
    CHECK(logged("RivaTuner Statistics Server's hooks library did not load"), "logged");
}

// Low latency switching on and off at every poll (the want flips between a limit and kept out, as at each swapchain
// rebuild): a want acts on the profile only once it stayed two polls, so the flips write and remove nothing, and the
// game's limiter holds each kept-out want at once. A want that stays two polls each time does act, and the log numbers
// each creation and removal so a play-test can count them.
static void churn_run(void) {
    app_set(1, 0xA, 1000, 2000, 60);
    ask(60000);
    poll();
    poll();
    for (int i = 0; i < 10; i++) {
        ask(i & 1 ? 60000 : RTSS_OUT_LOWLAT);
        if (!(i & 1)) CHECK(!rtss_limit_active(), "kept out at flip %d: the game's limiter at once", i);
        poll();
    }
    show("ten polls with the want flipping");
    CHECK(created == 1 && removed == 0, "%ld creations, %ld removals", created, removed);
    poll();
    CHECK(rtss_limit_active(), "RTSS limits once the limit stays");
    for (int i = 0; i < 4; i++) {
        settle(RTSS_OUT_LOWLAT);
        settle(60000);
    }
    show("four flips of two polls each");
    CHECK(created == 5 && removed == 4, "%ld creations, %ld removals", created, removed);
    CHECK(logged("created; it goes away when the game closes (creation 1 this run)\n") &&
              logged("(creation 5 this run)\n") && !logged("(creation 6"),
          "the created lines are numbered");
    CHECK(logged("removed, as before the game started (removal 1 this run)\n") && logged("(removal 4 this run)\n") &&
              !logged("(removal 5"),
          "the removed lines are numbered");
}

// The state file: the run's first write records the profile as it was, each later one the value written, and the exit's
// restore removes the line and the file with it.
static char line_at_save[4096];   // the state file as SaveProfile found it (keep_line)
static void keep_line(void) { snprintf(line_at_save, sizeof line_at_save, "%s", state()); }
static void state_record(void) {
    app_set(1, 0xA, 1000, 2000, 60);
    ask(60000);
    poll();
    show("playing at 60");
    CHECK(own.exists && own.limit == 60 && state_has("\nrtss1 0 0 1 60 1 60 1 ocarina.exe\n"), "recorded:\n%s",
          state());
    CHECK(!strncmp(state(), "# RivaTuner Statistics Server's profiles", 40), "the header first");
    during_save = keep_line;
    settle(144000);
    CHECK(strstr(line_at_save, "\nrtss1 0 0 1 144 1 60 1 ocarina.exe\n"),
          "before the profile changes, the line holds 144 and the 60 before it:\n%s", line_at_save);
    CHECK(own.limit == 144 && state_has("\nrtss1 0 0 1 144 1 144 1 ocarina.exe\n"), "then 144 alone:\n%s", state());
    rtss_limit_restore();
    show("after the restore");
    CHECK(!own.exists && !*state(), "the profile and the state file gone:\n%s", state());
}
static void state_player(void) {
    own = (Prof){1, 30, 1, 0, 0, 0, 0, 0, 0, 0, 1};
    app_set(1, 0xA, 1000, 2000, 60);
    ask(60000);
    poll();
    CHECK(own.limit == 60 && state_has("\nrtss1 1 30 1 60 1 60 1 ocarina.exe\n"), "recorded:\n%s", state());
    settle(RTSS_OUT_LOWLAT);   // kept out: put back during the run, and the line goes with it
    CHECK(own.limit == 30 && !*state(), "put back, no line:\n%s", state());
    settle(60000);
    CHECK(own.limit == 60 && state_has("\nrtss1 1 30 1 60 1 60 1 ocarina.exe\n"), "recorded again:\n%s", state());
    rtss_limit_restore();
    CHECK(own.exists && own.limit == 30 && !*state(), "the player's 30 back, no line");
}

// The state file cannot be written: the run goes on as before it, with a line in the log, and a crash puts the profile
// back as an orderly exit does.
static void state_unwritable(void) {
    state_readonly = 1;
    app_set(1, 0xA, 1000, 2000, 60);
    ask(60000);
    poll();
    poll();
    show("no state file");
    CHECK(own.exists && own.limit == 60 && rtss_limit_active(), "RTSS limits as before");
    CHECK(logged("rtss-profile.txt could not be written; a crash would leave the game's limit in RivaTuner Statistics "
                 "Server's profile ocarina.exe"),
          "logged");
    rtss_limit_crash();
    show("a crash");
    CHECK(!own.exists && !rtss_limit_active(), "the crash put the profile back");
}

// A put back during the run that does not land (the profile went read-only): the line and the values found at the
// run's first write stay. A later limit is the run's first write again and keeps the player's 30 to put back, not the
// game's 60 the profile still holds, and the exit puts 30 back.
static void put_back_owed(void) {
    own = (Prof){1, 30, 1, 0, 0, 0, 0, 0, 0, 0, 1};
    app_set(1, 0xA, 1000, 2000, 60);
    ask(60000);
    poll();
    readonly = 1;
    settle(RTSS_OUT_LOWLAT);
    show("kept out, the put back did not land");
    CHECK(own.limit == 60 && logged("did not take its old limit back") &&
              state_has("\nrtss1 1 30 1 60 1 60 1 ocarina.exe\n"),
          "the line stays:\n%s", state());
    readonly = 0;
    settle(144000);
    show("a limit again");
    CHECK(own.limit == 144 && state_has("\nrtss1 1 30 1 144 1 144 1 ocarina.exe\n"), "the player's 30 still owed:\n%s",
          state());
    rtss_limit_restore();
    show("closed");
    CHECK(own.exists && own.limit == 30 && !*state(), "the exit puts the player's 30 back:\n%s", state());
}

// The same where the run made the profile: a removal that does not land is no removal; the line stays, and the exit
// removes the profile.
static void put_back_undeleted(void) {
    app_set(1, 0xA, 1000, 2000, 60);
    ask(60000);
    poll();
    readonly = 1;
    settle(RTSS_OUT_LOWLAT);
    show("kept out, the removal did not land");
    CHECK(own.exists && logged("profile ocarina.exe did not go away") && !removed &&
              state_has("\nrtss1 0 0 1 60 1 60 1 ocarina.exe\n"),
          "the line stays:\n%s", state());
    readonly = 0;
    rtss_limit_restore();
    show("closed");
    CHECK(!own.exists && removed == 1 && !*state(), "the exit removes it:\n%s", state());
}

// The state file is there with another executable's line but does not read: it is left as it is, with that line, and
// the run goes on as with a file that cannot be written.
static void state_locked(void) {
    char path[MAX_PATH + 32];
    snprintf(path, sizeof path, "%s\\rtss-profile.txt", exe_dir);
    FILE *f = fopen(path, "wb");
    if (f) fputs("rtss1 0 0 1 60 1 60 1 ocarina_dev.exe\n", f), fclose(f);
    state_unreadable = 1;
    app_set(1, 0xA, 1000, 2000, 60);
    ask(60000);
    poll();
    poll();
    show("a state file that does not read");
    CHECK(own.exists && own.limit == 60 && rtss_limit_active(), "RTSS limits as before");
    CHECK(logged("rtss-profile.txt could not be written"), "logged");
    CHECK(!strcmp(state(), "rtss1 0 0 1 60 1 60 1 ocarina_dev.exe\n"), "the file as it was:\n%s", state());
    state_unreadable = 0;
    rtss_limit_restore();
    CHECK(!own.exists && !strcmp(state(), "rtss1 0 0 1 60 1 60 1 ocarina_dev.exe\n"), "still as it was:\n%s", state());
}

// A crash on RTSS's own thread, inside SaveProfile with the lock held, in a run whose state file could not be written:
// rtss_limit_crash closes the run and returns at once, with no call into RTSS's library and no wait for the lock that
// thread holds (rtss_limit_restore would try it for a second); nothing after it reaches the profile.
static ULONGLONG crash_ms;
static LONG crash_calls = -1;
static void crash_on_worker(void) {
    LONG c0 = calls;
    ULONGLONG t0 = GetTickCount64();
    rtss_limit_crash();
    crash_ms = GetTickCount64() - t0;
    crash_calls = calls - c0;
}
static void crash_worker_thread(void) {
    state_readonly = 1;
    app_set(1, 0xA, 1000, 2000, 60);
    during_save = crash_on_worker;
    ask(60000);
    poll();
    show("a crash on RTSS's thread inside SaveProfile");
    CHECK(crash_calls == 0, "%ld calls into RTSS's library from the crash", crash_calls);
    CHECK(crash_ms < 200, "the crash took %llu ms", crash_ms);
    CHECK(atomic_load(&closed) && !rtss_limit_active() && !rtss_limit_state(), "the run closed");
    LONG s0 = saves;
    ask(144000);
    poll();
    poll();
    CHECK(saves == s0, "nothing after the crash reaches the profile");
}

// A crash with the line in the state file: no call into RTSS's library, the profile and the line stay for the next
// start, and nothing after it reaches the profile.
static void crash_recorded(void) {
    app_set(1, 0xA, 1000, 2000, 60);
    ask(60000);
    poll();
    long s0 = saves, d0 = deletes, u0 = updates;
    rtss_limit_crash();
    show("a crash");
    CHECK(saves == s0 && deletes == d0 && updates == u0, "no profile call");
    CHECK(own.exists && own.limit == 60 && state_has("\nrtss1 0 0 1 60 1 60 1 ocarina.exe\n"),
          "left for the next start");
    CHECK(!rtss_limit_active() && !rtss_limit_state(), "the game's limiter");
    ask(144000);
    poll();
    CHECK(saves == s0 && own.limit == 60, "a want after it changes nothing");
}

// A run killed while RTSS held its limit (kill_first), and the next start (kill_next): the profile as before the killed
// run, then the next run's own limit, and back again at its close. variant 1: the player had a profile with 30/1;
// variant 2: the player set 75 in RTSS between the two runs; variant 3: the next start has frame generation on, which
// keeps RTSS out, and still puts the profile back (the hooks library loaded for that alone) with no write of its own;
// variant 4: the next start has NX2NAVI_RTSS=0, which keeps the thread read-only: the log says what the file holds,
// and the profile and the line stay for a start with RTSS let in; variant 5: the run is killed inside the save of its
// second limit, after the line took 144 and before the profile did: the next start finds the 60 before it and removes
// the profile.
static int variant;
static void kill_in_save(void) {
    CHECK(state_has("\nrtss1 0 0 1 144 1 60 1 ocarina.exe\n") && own.limit == 60, "the line before the profile:\n%s",
          state());
    prof_save();
    ExitProcess(fails != 0);
}
static void kill_first(void) {
    prof_load();
    app_set(1, 0xA, 1000, 2000, 60);
    ask(60000);
    poll();
    show("the run before the kill");
    CHECK(own.exists && own.limit == 60 && state_has(variant == 1 ? "\nrtss1 1 30 1 60 1 60 1 ocarina.exe\n"
                                                                  : "\nrtss1 0 0 1 60 1 60 1 ocarina.exe\n"),
          "recorded:\n%s", state());
    if (variant == 5) {   // killed inside the save of 144: the line has 144, the profile still 60
        during_save = kill_in_save;
        settle(144000);
        CHECK(0, "the run went on past the kill");
    }
    prof_save();   // killed: no restore
}
static void kill_next(void) {
    prof_load();
    app_set(1, 0xA, 1000, 2000, 144);
    if (variant == 3 || variant == 4) {
        ask(variant == 3 ? RTSS_OUT_FRAMEGEN : RTSS_OUT_SETTING);
        poll();
        poll();
        show("the next start, kept out");
        CHECK(!saves, "no limit written by this run (%ld saves)", saves);
        if (variant == 3) {
            CHECK(loads == 1 && deletes == 1 && updates == 1 &&
                      logged("profile ocarina.exe had the game's limit of 60/1 from a run that did not close") &&
                      logged("; removed, as before that run"),
                  "the killed run's profile removed, with the hooks library loaded once");
            CHECK(!own.exists && !*state(), "the profile and the line gone:\n%s", state());
        } else {
            CHECK(!loads && logged("rtss-profile.txt holds the game's limit of 60/1 for RivaTuner Statistics Server's "
                                   "profile ocarina.exe from a run that did not close; NX2NAVI_RTSS=0 leaves the "
                                   "profile as it is"),
                  "read only: logged");
            CHECK(own.exists && own.limit == 60 && state_has("\nrtss1 0 0 1 60 1 60 1 ocarina.exe\n"), "left:\n%s",
                  state());
        }
        rtss_limit_restore();
        show("closed");
        prof_save();
        return;
    }
    ask(144000);
    poll();
    show("the next start");
    if (variant == 0 || variant == 5) {
        CHECK(logged("profile ocarina.exe had the game's limit of 60/1 from a run that did not close") &&
                  logged("; removed, as before that run"),
              "the killed run's profile removed");
        CHECK(logged("created; it goes away when the game closes (creation 1 this run)"), "made again by this run");
        CHECK(state_has("\nrtss1 0 0 1 144 1 144 1 ocarina.exe\n"), "this run's line:\n%s", state());
    } else if (variant == 1) {
        CHECK(logged("profile ocarina.exe had the game's limit of 60/1 from a run that did not close") &&
                  logged("; its limit of 30/1 is back"),
              "the player's 30 back");
        CHECK(logged("held a limit of 30/1; it gets that back"), "this run takes 30 for the player's");
        CHECK(state_has("\nrtss1 1 30 1 144 1 144 1 ocarina.exe\n"), "this run's line:\n%s", state());
    } else {
        CHECK(logged("profile ocarina.exe changed since a run that did not close") && logged("; left as it is"),
              "the player's 75 left");
        CHECK(state_has("\nrtss1 1 75 1 144 1 144 1 ocarina.exe\n"), "this run's line:\n%s", state());
    }
    CHECK(own.exists && own.limit == 144, "this run's limit");
    rtss_limit_restore();
    show("closed");
    CHECK(!*state(), "no line left:\n%s", state());
    prof_save();
}
static int run_phase(const char *phase) {
    char exe[MAX_PATH], cmd[3 * MAX_PATH];
    GetModuleFileNameA(NULL, exe, sizeof exe);
    snprintf(cmd, sizeof cmd, "\"%s\" --phase %s %d \"%s\"", exe, phase, variant, exe_dir);
    STARTUPINFOA si = {sizeof si};
    PROCESS_INFORMATION pi;
    if (!CreateProcessA(NULL, cmd, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) return -1;
    WaitForSingleObject(pi.hProcess, 60000);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    return (int)code;
}
static void kill_restart_as(int v) {
    variant = v;
    if (v == 1) own = (Prof){1, 30, 1, 0, 0, 0, 0, 0, 0, 0, 1};
    prof_save();
    CHECK(run_phase("kill_first") == 0, "the killed run");
    prof_load();
    if (v == 2) own.limit = 75, touch(&own), prof_save();
    CHECK(run_phase("kill_next") == 0, "the next start");
    prof_load();
    show("after the next start closed");
    if (v == 0 || v == 5) CHECK(!own.exists, "the profile is gone, as before the killed run");
    if (v == 1) CHECK(own.exists && own.limit == 30, "the player's 30");
    if (v == 2) CHECK(own.exists && own.limit == 75, "the player's 75");
    if (v == 3) CHECK(!own.exists, "the profile is gone, as before the killed run");
    if (v == 4) CHECK(own.exists && own.limit == 60, "the killed run's 60 stays for a start with RTSS let in");
    CHECK(v == 4 ? state_has("\nrtss1 0 0 1 60 1 60 1 ocarina.exe\n") : !*state(), "the state file:\n%s", state());
}
static void kill_restart(void) { kill_restart_as(0); }
static void kill_restart_player(void) { kill_restart_as(1); }
static void kill_restart_edited(void) { kill_restart_as(2); }
static void kill_restart_framegen(void) { kill_restart_as(3); }
static void kill_restart_setting(void) { kill_restart_as(4); }
static void kill_restart_mid_write(void) { kill_restart_as(5); }

// A movie at 60 under a limit of 144: the want stays 144 (frame_rate.c frame_step_cap_ns) and RTSS's profile keeps it
// with no write, while the game's pacer holds the movie at its rate; RTSS holds the game again at its end.
static void movie_run(void) {
    app_set(1, 0xA, 1000, 2000, 144);
    ask(144000);
    poll();
    long s0 = saves;
    rtss_limit_movie(60);
    ask(144000);
    poll();
    show("a movie at 60");
    CHECK(own.limit == 144 && !rtss_limit_active() && saves == s0, "the profile keeps 144; the game's pacer holds it");
    CHECK(logged("the game's own limiter (a movie plays: the game keeps its pictures on time)"), "logged");
    rtss_limit_movie(0);
    poll();
    show("after the movie");
    CHECK(own.limit == 144 && rtss_limit_active() && saves == s0, "RTSS holds again, no write for the movie");
}

// The same under a limit of 40: the profile has no limit while the movie plays, so RTSS cannot space out the frames the
// pacer sends at once after a late one, and gets 40 back at its end.
static void movie_low_run(void) {
    app_set(1, 0xA, 1000, 2000, 40);
    ask(40000);
    poll();
    long s0 = saves;
    rtss_limit_movie(60);
    poll();
    show("a movie at 60 with the limit at 40");
    CHECK(own.exists && own.limit == 0 && !rtss_limit_active(), "no limit in the profile during the movie");
    rtss_limit_movie(0);
    poll();
    show("after the movie");
    CHECK(own.limit == 40 && saves - s0 == 2, "40 back: two writes for the movie");
    poll();
    CHECK(rtss_limit_active(), "RTSS holds 40 again");
}

static const struct { const char *name; void (*run)(void); } scenarios[] = {
    {"restore_race", restore_race},
    {"restore_player", restore_player},
    {"restore_inflight", restore_inflight},
    {"restore_first", restore_first},
    {"restore_kept_out", restore_kept_out},
    {"kept_two", kept_two},
    {"kept_lowlat_then_limit", kept_lowlat_then_limit},
    {"kept_framegen", kept_framegen},
    {"active_race", active_race},
    {"active_logged", active_logged},
    {"round_logged", round_logged},
    {"round_logged_whole", round_logged_whole},
    {"start_off_present", start_off_present},
    {"not_installed", not_installed},
    {"settings_logged", settings_logged},
    {"readonly_run", readonly_run},
    {"readonly_two", readonly_two},
    {"library_two", library_two},
    {"refused_run", refused_run},
    {"over_run", over_run},
    {"over_slack_run", over_slack_run},
    {"over_unlimited_run", over_unlimited_run},
    {"api_run", api_run},
    {"unproven_run", unproven_run},
    {"idle_run", idle_run},
    {"quit_run", quit_run},
    {"library_run", library_run},
    {"churn_run", churn_run},
    {"movie_run", movie_run},
    {"movie_low_run", movie_low_run},
    {"state_record", state_record},
    {"state_player", state_player},
    {"state_unwritable", state_unwritable},
    {"state_locked", state_locked},
    {"crash_recorded", crash_recorded},
    {"crash_worker_thread", crash_worker_thread},
    {"kill_restart", kill_restart},
    {"kill_restart_player", kill_restart_player},
    {"kill_restart_edited", kill_restart_edited},
    {"kill_restart_framegen", kill_restart_framegen},
    {"kill_restart_setting", kill_restart_setting},
    {"kill_restart_mid_write", kill_restart_mid_write},
    {"put_back_owed", put_back_owed},
    {"put_back_undeleted", put_back_undeleted},
};

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    tick = CreateEventA(NULL, FALSE, FALSE, NULL), done = CreateEventA(NULL, FALSE, FALSE, NULL);
    at_lock = CreateEventA(NULL, FALSE, FALSE, NULL), go = CreateEventA(NULL, FALSE, FALSE, NULL);
    if (argc == 2 && !strcmp(argv[1], "--list")) {
        for (size_t i = 0; i < sizeof scenarios / sizeof *scenarios; i++) printf("%s\n", scenarios[i].name);
        return 0;
    }
    if (argc == 5 && !strcmp(argv[1], "--phase")) {   // a process of a kill scenario, in that scenario's folder
        variant = atoi(argv[3]);
        snprintf(exe_dir, sizeof exe_dir, "%s", argv[4]);
        printf(" phase %s\n", argv[2]);
        if (!strcmp(argv[2], "kill_first")) kill_first();
        else if (!strcmp(argv[2], "kill_next")) kill_next();
        else return 2;
        return fails != 0;
    }
    for (size_t i = 0; argc == 2 && i < sizeof scenarios / sizeof *scenarios; i++) {
        if (strcmp(argv[1], scenarios[i].name)) continue;
        printf("scenario %s\n", argv[1]);
        char exe[MAX_PATH], *cut;   // the scenario's folder, emptied: rtss_state\<scenario> beside this program
        GetModuleFileNameA(NULL, exe, sizeof exe);
        if ((cut = strrchr(exe, '\\'))) *cut = 0;
        snprintf(exe_dir, sizeof exe_dir, "%s\\rtss_state", exe);
        CreateDirectoryA(exe_dir, NULL);
        snprintf(exe_dir, sizeof exe_dir, "%s\\rtss_state\\%s", exe, argv[1]);
        CreateDirectoryA(exe_dir, NULL);
        char path[MAX_PATH + 32];
        snprintf(path, sizeof path, "%s\\rtss-profile.txt", exe_dir);
        remove(path);
        snprintf(path, sizeof path, "%s\\own.bin", exe_dir);
        remove(path);
        scenarios[i].run();
        return fails != 0;
    }
    fprintf(stderr, "usage: rtss_worker_check --list | <scenario>\n");
    return 2;
}
