// RivaTuner Statistics Server as the frame limiter (frame_rate.c frame_step_cap_ns): where RTSS is installed and running,
// the frame limit goes into RTSS's profile for this executable and, while RTSS is seen holding the game to it, the
// game's own pacer keeps only the 240-frame ceiling (rtss_decide.h rtss_pace_period), so there is one limiter (at a
// limit of 240 the ceiling is RTSS's own period). Anywhere else, or when any step fails, the game's own limiter works
// as it always did.
//
// RTSS's hooks library (RTSSHooks64.dll in the folder its installer records under
// HKLM\SOFTWARE\WOW6432Node\Unwinder\RTSS, InstallDir) exports the profile calls, loaded here at run time:
// LoadProfile(name) reads a profile (the global one for a name without its own), Get/SetProfileProperty read and change
// the loaded copy, SaveProfile(name) writes it to Profiles\<name>.cfg, UpdateProfiles makes every hooked process
// read its profile again, EnumProfiles lists the profiles that exist (profile_exists says why it is not called),
// DeleteProfile removes one. "FramerateLimit" is the limit in frames per second (0: none); "FramerateLimitDenominator"
// divides it, so 59940 over 1000 limits to 59.94 (a monitor's measured rate). SaveProfile reports nothing: RTSS's
// Profiles folder sits under Program Files, which a process without administrator rights cannot write, and the save
// then fails silently. So every change is read back
// from the saved profile, and a limit that did not stick leaves the game's own limiter in charge for the session.
//
// RTSS runs when its shared memory "RTSSSharedMemoryV2" carries the signature 'RTSS'. The game's own limiter stands
// aside only once RTSS is seen to hold the game (rtss_decide.h rtss_decide): the memory's application list holds this
// process id with its Vulkan presents (RTSS hooks a process some seconds after it starts, its injection delay), RTSS
// has read the profile the game wrote, it measured a period of the game's frames lately, no movie plays and pictures
// go out. Until then, and again whenever one of these ends (RTSS quits, the window is minimised), the game's own limiter
// paces the frames. While RTSS limits, each new period RTSS measures for the game is checked against the limit: above
// 1.05 times it for three periods in a row, and the game takes the limiting back for the rest of the run. So it does
// when RTSS lists the game with the limit for 30 s while pictures go out and never shows its hold: RTSS may then hold
// the frames beside the game's limiter.
//
// The calls run on a thread of their own, every half second, which also looks RTSS up (find_rtss), so a present never
// waits on the registry or profile files. On every orderly exit (rtss_limit_restore) the profile gets back what it held
// before the game changed it: the old limit, or no profile of its own when there was none; from then on no want reaches
// it and the game's own limiter paces the frames left. What it held is also kept in rtss-profile.txt next to the
// executable (rtss_decide.h RtssSaved), written before the run's first change of the profile and removed once the
// profile is back: an exit that skips the restore (a kill, a hang, the device-lost watchdog) leaves the line, and the
// next start puts the profile back from it before it changes anything (left_start, restore_left), unless the player
// changed the profile since; a start with NX2NAVI_RTSS=0 only logs it. A crash and a fatal error leave it to that start
// too (rtss_limit_crash), with no call into RTSS's library from the failing process, unless the file could not be
// written: then they put the profile back as an orderly exit does, but not from the worker's own thread.
//
// NX2NAVI_RTSS=0 keeps RTSS out of it. Frame generation keeps it out too (RTSS counts every presented frame, the
// generated ones included), and so does a driver's low latency (vk_lowlat.c), which holds the frames where they start.
// While RTSS is kept out the thread does not load the hooks library and only reads RTSS's own settings for the game, to
// tell when they limit it a second time (watch), except once at the start, to put back a profile that a run which did
// not close left with its limit (left_start); a later limit (frame generation or low latency switched off) loads it
// and hands RTSS the limit as usual, and NX2NAVI_RTSS=0 keeps the thread read-only for the run.
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gfx.h"
#include "host.h"
#include "platform/platform.h"
#include "rtss_decide.h"

#ifdef _WIN32
#include <windows.h>

typedef DWORD (*EnumProfilesFn)(LPSTR list, DWORD size);
typedef void (*ProfileFn)(LPCSTR name);
typedef BOOL (*PropertyFn)(LPCSTR property, LPBYTE data, DWORD size);
typedef void (*UpdateFn)(void);

#define POLL_MS 500

static PlatMutex lock = PLAT_MUTEX_INIT;   // the hooks calls and the state below
static _Atomic int64_t want = RTSS_UNSET;  // millihertz from the pacer: >0 a limit, 0 none; RTSS_OUT_* kept out
static _Atomic int64_t active_for = RTSS_UNSET;   // the want RTSS holds the game to now; RTSS_UNSET none
static _Atomic int64_t movie_mhz;          // a movie's rate while one plays (rtss_limit_movie), 0 none
static _Atomic int failed;                 // RTSS_FAIL_*: RTSS is out of it for the rest of the run (S.fail)
static _Atomic int closed;                 // rtss_limit_restore ran: nothing reaches the profile any more
static _Atomic int two;                    // kept out, and RTSS limits the game by a setting of its own (watch)
static _Atomic int started;                // rtss_limit_want spawned the worker
static _Atomic int ready;                  // the worker found RTSS: dll_path, rtss_dir and profile hold their values
static char dll_path[MAX_PATH], profile[MAX_PATH], rtss_dir[MAX_PATH];
static HMODULE dll;
static EnumProfilesFn enum_profiles;
static ProfileFn load_profile, save_profile, delete_profile;
static PropertyFn get_property, set_property;
static UpdateFn update_profiles;
static RtssState S = RTSS_STATE_INIT;   // the worker's decisions (rtss_decide.h), under lock
static int has_denominator;   // this RTSS knows FramerateLimitDenominator
static int had_profile;       // the profile existed before this run changed it
static DWORD old_limit, old_denominator = 1;
static int owed;              // this run changed the profile and has not put it back: the three above stay as found
static int creations, removals;   // the profile's "created" and "removed" lines this run, numbered (under lock)
static char state_path[MAX_PATH + 32];   // rtss-profile.txt next to the executable; "" when its path is unknown
static RtssSaved rec;                    // this run's line in it, while the profile holds a value of this run
static _Atomic int recorded;             // the file holds rec: a crash leaves the profile to the next start
static int state_off;                    // a line this run could not act on stays as it is: no writes of the file
static _Atomic uint32_t rtss_tid;        // the worker's thread, which may hold the lock when a crash comes

// RTSS's shared memory as rtss_parse reads it: whether RTSS runs, whether it lists this process, and its frame count.
static void rtss_map(RtssApp *a) {
    memset(a, 0, sizeof *a);
    HANDLE m = OpenFileMappingA(FILE_MAP_READ, FALSE, "RTSSSharedMemoryV2");
    if (!m) return;
    const uint8_t *p = MapViewOfFile(m, FILE_MAP_READ, 0, 0, 0);
    MEMORY_BASIC_INFORMATION mi;
    if (p && VirtualQuery(p, &mi, sizeof mi)) rtss_parse(p, mi.RegionSize, GetCurrentProcessId(), a);
    if (p) UnmapViewOfFile(p);
    CloseHandle(m);
}

static int load_hooks(void) {
    if (dll) return 1;
    dll = LoadLibraryA(dll_path);
    if (!dll) {
        rt_log("frame rate: RivaTuner Statistics Server's %s did not load (%lu)", dll_path, GetLastError());
        return 0;
    }
    enum_profiles = (EnumProfilesFn)(void *)GetProcAddress(dll, "EnumProfiles");
    load_profile = (ProfileFn)(void *)GetProcAddress(dll, "LoadProfile");
    save_profile = (ProfileFn)(void *)GetProcAddress(dll, "SaveProfile");
    delete_profile = (ProfileFn)(void *)GetProcAddress(dll, "DeleteProfile");
    get_property = (PropertyFn)(void *)GetProcAddress(dll, "GetProfileProperty");
    set_property = (PropertyFn)(void *)GetProcAddress(dll, "SetProfileProperty");
    update_profiles = (UpdateFn)(void *)GetProcAddress(dll, "UpdateProfiles");
    if (enum_profiles && load_profile && save_profile && delete_profile && get_property && set_property && update_profiles)
        return 1;
    rt_log("frame rate: RivaTuner Statistics Server's hooks library lacks the profile calls");
    FreeLibrary(dll);
    dll = NULL;
    return 0;
}

// The profile exists on its own: RTSS keeps it as Profiles\<name>.cfg in its folder. EnumProfiles would say the same
// (a comma-separated list of names) but crashes this process: it walks Profiles\*.cfg with _findfirst and _findnext and
// keeps the find handle in 32 bits. The handle points into the process heap, which here lies above 4 GB (the guest
// address space takes the low part, memory.c mem_init), so _findnext gets a cut pointer and faults in ntdll writing to
// it. NX2NAVI_RTSS_ENUM=1 calls EnumProfiles anyway, to test an RTSS that keeps the whole handle.
static int profile_exists(void) {
    if (!rt_knob("RTSS_ENUM", "0")) {
        char path[MAX_PATH * 2];
        snprintf(path, sizeof path, "%s\\Profiles\\%s.cfg", rtss_dir, profile);
        return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
    }
    char list[8192];
    DWORD need = enum_profiles(list, sizeof list);
    list[sizeof list - 1] = 0;
    if (need > sizeof list)
        rt_log("frame rate: RivaTuner Statistics Server lists %lu bytes of profiles; reading the first %zu", need, sizeof list);
    size_t n = strlen(profile);
    for (char *s = list; *s;) {
        char *end = strchr(s, ',');
        size_t len = end ? (size_t)(end - s) : strlen(s);
        if (len == n && !_strnicmp(s, profile, n)) return 1;
        if (!end) break;
        s = end + 1;
    }
    return 0;
}

// Writes limit/denominator into the profile and reads them back from the saved file: 1 when they stuck, 0 when RTSS
// refused a value, -1 when the save did not reach the file. RTSS checks each value against the other one already in
// the profile (the limit over the denominator must be a rate it accepts: 165080 is refused over 1 and taken over 1000),
// so the limit goes to 0 (none) before the denominator changes.
static int write_limit(DWORD limit, DWORD denominator) {
    load_profile(profile);
    DWORD none = 0;
    if (!set_property("FramerateLimit", (LPBYTE)&none, sizeof none)) return 0;
    if (has_denominator && !set_property("FramerateLimitDenominator", (LPBYTE)&denominator, sizeof denominator)) return 0;
    if (limit && !set_property("FramerateLimit", (LPBYTE)&limit, sizeof limit)) return 0;
    save_profile(profile);
    load_profile(profile);
    DWORD l = ~0u, d = 1;
    if (!get_property("FramerateLimit", (LPBYTE)&l, sizeof l)) return -1;
    if (has_denominator && !get_property("FramerateLimitDenominator", (LPBYTE)&d, sizeof d)) return -1;
    return l == limit && (!has_denominator || d == denominator) ? 1 : -1;
}

// The state file with this profile's line set to r, or removed for r NULL; the file goes once it holds no line. Under
// lock: the worker, or rtss_limit_restore's thread at the exit. 1 when the file holds what was asked.
static int state_put(const RtssSaved *r) {
    if (state_off || !*state_path) return 0;
    uint64_t n = 0;
    char *text = (char *)plat_file_read(state_path, &n);
    // a file that is there but did not read (another program holds it, a scan) keeps the lines of the other executables
    // in the folder: rewritten from nothing, they would go
    if (!text && plat_file_size(state_path) > 0) return 0;
    size_t cap = (size_t)(text ? n : 0) + sizeof RTSS_SAVED_HEADER + 2 * (RTSS_NAME_MAX + 96);
    char *out = malloc(cap);
    int records = 0, ok = 0;
    size_t len = out ? rtss_saved_update(text ? text : "", profile, r, out, cap, &records) : 0;
    if (len && !records) {
        plat_file_delete(state_path);
        ok = !plat_file_exists(state_path);
    } else if (len) {
        ok = plat_file_write_atomic(state_path, out, len);
    }
    free(out);
    free(text);
    return ok;
}

// The state file did not take this run's line: a crash then puts the profile back as an orderly exit does
// (rtss_limit_crash). Logged once a run.
static void unrecorded(void) {
    static int said;
    atomic_store(&recorded, 0);
    if (!state_off && !said++)
        rt_log("frame rate: %s could not be written; a crash would leave the game's limit in RivaTuner Statistics "
               "Server's profile %s", *state_path ? state_path : "rtss-profile.txt", profile);
}

// Before a write of limit/denominator into the profile: rec's written value is that one, and the one written before it
// stays as the previous one (rtss_decide.h RtssSaved), so that a run that ends before the profile changed, or after,
// leaves a profile the next start tells from a value the player set since.
static void record_before(DWORD limit, DWORD denominator) {
    if (!atomic_load(&recorded) || (rec.limit == limit && rec.den == denominator)) return;
    rec.prev_limit = rec.limit, rec.prev_den = rec.den;
    rec.limit = limit, rec.den = denominator;
    if (!state_put(&rec)) unrecorded();
}

// After that write: the profile holds the written value alone (landed), or kept the previous one.
static void record_after(int landed) {
    if (!atomic_load(&recorded) || (rec.limit == rec.prev_limit && rec.den == rec.prev_den)) return;
    if (landed) rec.prev_limit = rec.limit, rec.prev_den = rec.den;
    else rec.limit = rec.prev_limit, rec.den = rec.prev_den;
    if (!state_put(&rec)) unrecorded();
}

// Writes mhz into the profile (rtss_poll's RtssOps.apply): RTSS_WROTE_*. first: the run's first write, which records
// what to put back on exit, in the state file too before anything changes; clear: on a refusal, clear the limit of this
// run the profile holds.
static int apply(void *ctx, int64_t mhz, int first, int clear) {
    (void)ctx;
    if (first && !owed) {   // what to put back on exit; after a put back that did not land, the profile still owes it
        had_profile = profile_exists();
        load_profile(profile);
        old_limit = 0, old_denominator = 1;
        get_property("FramerateLimit", (LPBYTE)&old_limit, sizeof old_limit);
        has_denominator = get_property("FramerateLimitDenominator", (LPBYTE)&old_denominator, sizeof old_denominator);
        if (!old_denominator) old_denominator = 1;
    }
    uint32_t l, d;
    rtss_limit_value(mhz, has_denominator, &l, &d);
    DWORD limit = l, denominator = d;
    if (first) {   // the line goes in before the profile changes: a kill between the two finds it as it was
        RtssSaved was = rec;   // still in the file after a put back that did not land: the profile holds its value
        int kept = atomic_load(&recorded);
        rec = (RtssSaved){had_profile, old_limit, old_denominator, limit, denominator, kept ? was.limit : limit,
                          kept ? was.den : denominator, ""};
        snprintf(rec.name, sizeof rec.name, "%s", profile);
        if (state_put(&rec)) atomic_store(&recorded, 1);
        else unrecorded();
    } else {
        record_before(limit, denominator);
    }
    int r = write_limit(limit, denominator);
    if (r < 0) {   // the line keeps both values: the profile may hold either
        rt_log("frame rate: RivaTuner Statistics Server's profile %s did not keep the limit: its Profiles folder needs the "
               "game run as administrator", profile);
        if (first && !owed && atomic_exchange(&recorded, 0)) state_put(NULL);   // the profile is as it was
        return RTSS_WROTE_READONLY;
    }
    owed = 1;
    if (first) {
        if (!had_profile)   // numbered: a want that flips between a limit and kept out creates it again at each flip
            rt_log("frame rate: RivaTuner Statistics Server profile %s created; it goes away when the game closes "
                   "(creation %d this run)", profile, ++creations);
        else
            rt_log("frame rate: RivaTuner Statistics Server profile %s held a limit of %lu/%lu; it gets that back when the "
                   "game closes", profile, old_limit, old_denominator);
    }
    if (r == 0) {   // RTSS must not keep an earlier limit of this run under the game's own one
        rt_log("frame rate: RivaTuner Statistics Server refused a limit of %.3f fps", mhz / 1000.0);
        record_after(0);
        if (!clear) return RTSS_WROTE_REFUSED;
        record_before(0, 1);
        int cleared = write_limit(0, 1) > 0;
        record_after(cleared);
        if (!cleared) return RTSS_WROTE_REFUSED;
        update_profiles();
        return RTSS_WROTE_REFUSED_CLEARED;
    }
    record_after(1);
    update_profiles();
    if (!mhz) rt_log("frame rate: RivaTuner Statistics Server's profile %s has no limit", profile);
    else if ((int64_t)limit * 1000 == mhz * denominator)
        rt_log("frame rate: RivaTuner Statistics Server's profile %s limits to %.3f fps", profile, mhz / 1000.0);
    else   // rounded to whole frames by an RTSS without FramerateLimitDenominator: the log says what RTSS holds
        rt_log("frame rate: RivaTuner Statistics Server's profile %s limits to %lu/%lu (%.3f fps; the game asked %.3f)",
               profile, limit, denominator, (double)limit / denominator, mhz / 1000.0);
    return RTSS_WROTE_HELD;
}

// The profile as it was before this run changed it (the caller checked S.changed or owed, and marks S done). Its line
// in the state file goes once it is back; one that did not go back keeps the line, and owed, for the exit and the next
// start.
static void put_back(void) {
    int back = 1;
    if (!had_profile) {
        delete_profile(profile);
        back = !profile_exists();
        if (back)
            rt_log("frame rate: RivaTuner Statistics Server profile %s removed, as before the game started "
                   "(removal %d this run)", profile, ++removals);
        else
            rt_log("frame rate: RivaTuner Statistics Server profile %s did not go away", profile);
    } else if (write_limit(old_limit, old_denominator) > 0) {
        rt_log("frame rate: RivaTuner Statistics Server profile %s has its limit of %lu/%lu back", profile, old_limit,
               old_denominator);
    } else {
        rt_log("frame rate: RivaTuner Statistics Server profile %s did not take its old limit back", profile);
        back = 0;
    }
    update_profiles();
    if (back) owed = 0;
    if (back && atomic_exchange(&recorded, 0)) state_put(NULL);
}

// This profile's line in the state file into r: 1 when there is one.
static int left_line(RtssSaved *r) {
    uint64_t n = 0;
    char *text = *state_path ? (char *)plat_file_read(state_path, &n) : NULL;
    int found = text && rtss_saved_find(text, profile, r);
    free(text);
    return found;
}

// The line r of this profile, left in the state file by a run that did not close (left_start): before this run changes
// anything, the profile gets back what it held before that run (rtss_restore_plan), unless the player changed it since,
// and the line goes. A profile that does not go back keeps the line, and this run leaves the file alone.
static void restore_left(const RtssSaved *r) {
    int exists = profile_exists();
    load_profile(profile);
    DWORD limit = 0, den = 1;
    get_property("FramerateLimit", (LPBYTE)&limit, sizeof limit);
    has_denominator = get_property("FramerateLimitDenominator", (LPBYTE)&den, sizeof den);
    if (!den) den = 1;
    int plan = rtss_restore_plan(r, exists, limit, den);
    if (plan == RTSS_PLAN_DELETE) delete_profile(profile);
    if ((plan == RTSS_PLAN_WRITE && write_limit(r->old_limit, r->old_den) <= 0) ||
        (plan == RTSS_PLAN_DELETE && profile_exists())) {
        rt_log("frame rate: RivaTuner Statistics Server profile %s still has the game's limit of %lu/%lu from a run "
               "that did not close, and did not go back as it was; %s keeps its line", profile, limit, den, state_path);
        state_off = 1;
        return;
    }
    if (plan == RTSS_PLAN_WRITE || plan == RTSS_PLAN_DELETE) update_profiles();
    if (plan == RTSS_PLAN_WRITE)
        rt_log("frame rate: RivaTuner Statistics Server profile %s had the game's limit of %lu/%lu from a run that did "
               "not close (%s); its limit of %u/%u is back", profile, limit, den, state_path, r->old_limit, r->old_den);
    else if (plan == RTSS_PLAN_DELETE)
        rt_log("frame rate: RivaTuner Statistics Server profile %s had the game's limit of %lu/%lu from a run that did "
               "not close (%s); removed, as before that run", profile, limit, den, state_path);
    else if (plan == RTSS_PLAN_LEAVE)
        rt_log("frame rate: RivaTuner Statistics Server profile %s changed since a run that did not close (%s); left as "
               "it is", profile, state_path);
    else
        rt_log("frame rate: RivaTuner Statistics Server profile %s is as it was before a run that did not close (%s)",
               profile, state_path);
    if (!state_put(NULL)) {
        rt_log("frame rate: %s could not be written; its line for %s stays", state_path, profile);
        state_off = 1;
    }
}

// The state file's line for this profile, on the worker's first poll that finds RTSS running (w the want it read,
// hooks as RtssIn.hooks): 1 once the line is dealt with or there is none, 0 when a restore is still owed, for the first
// poll with the hooks library. A limit puts the profile back with the library it loads for its own writes. Frame
// generation and low latency keep RTSS out of the run's limit, but a profile left with an old limit would hold the
// game beside them (two limiters, and under frame generation RTSS would count the generated frames too): the library is
// loaded for that one restore. NX2NAVI_RTSS=0 keeps the thread read-only, so the log says what the file holds.
static int left_start(int64_t w, int hooks) {
    RtssSaved r;
    if (!left_line(&r)) return 1;
    if (hooks != 1 && w == RTSS_OUT_SETTING) {
        rt_log("frame rate: %s holds the game's limit of %u/%u for RivaTuner Statistics Server's profile %s from a run "
               "that did not close; NX2NAVI_RTSS=0 leaves the profile as it is, and a start with RTSS let in puts it "
               "back", state_path, r.limit, r.den, profile);
        return 1;
    }
    if (hooks != 1 && (w >= 0 || !load_hooks())) return 0;
    restore_left(&r);
    return 1;
}

static void put_back_op(void *ctx, const RtssOut *o) {   // rtss_poll's RtssOps.put_back
    (void)ctx;
    if (o->why == RTSS_WHY_FAILED && S.fail == RTSS_FAIL_NOT_LIMITING)   // the frame count check gave up on RTSS
        rt_log("frame rate: RivaTuner Statistics Server measures %.1f fps with its limit at %.3f fps", o->fps,
               o->mhz / 1000.0);
    else if (o->why == RTSS_WHY_FAILED)   // the lines before say which sign of its hold never came
        rt_log("frame rate: RivaTuner Statistics Server listed the game with its limit at %.3f fps for %d s and never "
               "showed it holds it; the game's own limiter for the rest of the run", o->mhz / 1000.0,
               RTSS_UNPROVEN_POLLS * POLL_MS / 1000);
    put_back();
}

// Whether RTSS holds the game to the want w this poll read, and the log of which limiter is in use: the first decision,
// then each change of the limiter or of why the game's own one paces (RTSS not hooked yet, not read the profile, a
// minimised window), up to LIMITER_LINES lines. The want may have changed since the poll read it: rtss_limit_active
// compares the two, so a stale store never lets the game's limiter stand aside for a newer want. The store is made
// either way (a want that went w, w2, w again must not find an older hold), but a hold is logged only while its want
// is still the current one: the game's limiter paces a newer want, and the next poll logs what holds it.
#define LIMITER_LINES 100
static void set_active(int64_t w, int on, const char *why) {
    static int logged = -1, lines;
    static char said[128];
    atomic_store(&active_for, on ? w : RTSS_UNSET);
    if ((on && atomic_load(&want) != w) || (logged == on && (on || !strcmp(why, said)))) return;
    logged = on;
    snprintf(said, sizeof said, "%s", on ? "" : why);
    if (++lines > LIMITER_LINES) return;
    if (on) rt_log("frame rate: RivaTuner Statistics Server does the limiting; the game's own limiter stands aside");
    else rt_log("frame rate: the game's own limiter (%s)", why);
    if (lines == LIMITER_LINES) rt_log("frame rate: later changes of the limiter in charge are not logged");
}

// RTSS's own settings for the game, read from its profile file as RTSS keeps it: Profiles\<exe>.cfg, else
// Profiles\Global, and a key the game's profile lacks from Global. The system's ini reader does it, so no call goes into
// RTSS's hooks library, which a poll of a kept-out want never loads. The files are read again only when the time
// either was written changes. Worker thread only, while RTSS lists the game (watch).
static RtssSettings settings;
static char settings_file[MAX_PATH + 16];   // which file they came from, as Profiles\<name>

static uint64_t file_stamp(const char *path) {   // 0: no such file
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &a)) return 0;
    return ((uint64_t)a.ftLastWriteTime.dwHighDateTime << 32 | a.ftLastWriteTime.dwLowDateTime) | 1;
}

static int cfg_int(const char *own, const char *global, const char *key) {
    char v[32] = "";
    if (own) GetPrivateProfileStringA("Framerate", key, "", v, sizeof v, own);
    if (!*v) GetPrivateProfileStringA("Framerate", key, "", v, sizeof v, global);
    return (int)strtol(v, NULL, 10);   // GetPrivateProfileIntA would read a negative scanline as 0
}

static void read_settings(void) {
    static uint64_t stamps[2] = {~0ull, ~0ull};   // never read
    char own[MAX_PATH * 2], global[MAX_PATH * 2];
    snprintf(own, sizeof own, "%s\\Profiles\\%s.cfg", rtss_dir, profile);
    snprintf(global, sizeof global, "%s\\Profiles\\Global", rtss_dir);
    uint64_t a = file_stamp(own), b = file_stamp(global);
    if (a == stamps[0] && b == stamps[1]) return;
    stamps[0] = a, stamps[1] = b;
    const char *p = a ? own : NULL;
    settings = (RtssSettings){cfg_int(p, global, "Limit"), cfg_int(p, global, "LimitDenominator"),
                              cfg_int(p, global, "LimitTime"), cfg_int(p, global, "LimitTimeDenominator"),
                              cfg_int(p, global, "SyncScanline0"), cfg_int(p, global, "SyncScanline1"),
                              cfg_int(p, global, "SyncDisplay"), cfg_int(p, global, "ReflexSleep"),
                              cfg_int(p, global, "PassiveWait")};
    if (a) snprintf(settings_file, sizeof settings_file, "Profiles\\%s.cfg", profile);
    else snprintf(settings_file, sizeof settings_file, "Profiles\\Global");
}

// While RTSS lists the game, the settings it holds for it are logged once per change: what RTSS applies may be more than
// the FramerateLimit the game writes (a frame time limit, scanline sync, its own Reflex). While RTSS has no part in the
// game's limit (rtss_out_of_limit: kept out, w < 0, or out for the run because its profile cannot be written or its
// hooks library did not load) the game's own limiter, the driver's low latency or frame generation holds the frames,
// and a limit the player set in RTSS itself for the game (its global profile or the game's own, a frame time limit or
// scanline sync) still holds them a second time. Logged once per change, and again when the other limiter changes;
// the menu shows it for a kept-out want (rtss_limit_state 4), while a profile that cannot be written keeps its own note
// there (state 2).
static void watch(int64_t w, int fail, int listed) {
    static int said, lines, settings_lines;
    static const char *said_by;
    static char said_text[128], said_settings[MAX_PATH + 256];
    int out = rtss_out_of_limit(w, fail), on = 0;
    const char *by = rtss_out_limiter(w);   // the limiter beside RTSS's own setting
    char text[128] = "";
    if (listed) {
        read_settings();
        char all[MAX_PATH + 256], modes[192];
        rtss_settings_text(modes, sizeof modes, &settings, 1);
        snprintf(all, sizeof all, "(%s): %s", settings_file, modes);
        if (strcmp(all, said_settings) && settings_lines++ < 40)
            rt_log("frame rate: RivaTuner Statistics Server's settings for the game %s", all);
        snprintf(said_settings, sizeof said_settings, "%s", all);
    }
    if (out && listed) {
        on = rtss_two(w, fail, listed, rtss_settings_limit(&settings));
        rtss_settings_text(text, sizeof text, &settings, 0);
    }
    if (!out) said = 0;   // RTSS takes the game's limit: a later want out of it logs afresh
    else if (on != said || (on && (strcmp(text, said_text) || by != said_by))) {
        if (lines++ < 40) {
            if (on)
                rt_log("frame rate: two limiters: RivaTuner Statistics Server's own setting for the game (%s: %s) limits "
                       "it beside %s", settings_file, text, by);
            else
                rt_log("frame rate: RivaTuner Statistics Server no longer limits the game beside %s", by);
        }
        said = on, said_by = by;
        snprintf(said_text, sizeof said_text, "%s", text);
    }
    atomic_store(&two, on && w < 0);   // the menu's note (rtss_limit_state 4) is for a kept-out want
}

// RTSS's folder (InstallDir under its registry key), its hooks library and this executable's profile name, on the
// worker before its first poll. 0 when RTSS is not installed: the game's own limiter for the run.
static int find_rtss(void) {
    char dir[MAX_PATH];
    DWORD size = sizeof dir;
    if (RegGetValueA(HKEY_LOCAL_MACHINE, "SOFTWARE\\WOW6432Node\\Unwinder\\RTSS", "InstallDir", RRF_RT_REG_SZ, NULL, dir,
                     &size) != ERROR_SUCCESS) {
        rt_log("frame rate: RivaTuner Statistics Server is not installed; the game's own limiter");
        return 0;
    }
    snprintf(dll_path, sizeof dll_path, "%s\\RTSSHooks64.dll", dir);
    snprintf(rtss_dir, sizeof rtss_dir, "%s", dir);
    if (GetFileAttributesA(dll_path) == INVALID_FILE_ATTRIBUTES) {
        rt_log("frame rate: RivaTuner Statistics Server's folder %s has no RTSSHooks64.dll; the game's own limiter", dir);
        return 0;
    }
    char exe[MAX_PATH];
    DWORD n = GetModuleFileNameA(NULL, exe, sizeof exe);
    if (!n || n >= sizeof exe) return 0;
    const char *base = exe;
    for (const char *s = exe; *s; s++)
        if (*s == '\\' || *s == '/') base = s + 1;
    snprintf(profile, sizeof profile, "%s", base);   // RTSS names a profile after the executable, as ocarina.exe
    char path[MAX_PATH], *cut;   // the state file next to the executable, where nx2navi.ini is (cfg.c find_ini)
    if (plat_exe_path(path, sizeof path) && (cut = plat_last_sep(path)))
        snprintf(state_path, sizeof state_path, "%.*s%s", (int)(cut + 1 - path), path, "rtss-profile.txt");
    int64_t first = atomic_load(&want);
    if (first >= 0)
        rt_log("frame rate: RivaTuner Statistics Server found in %s; it takes the frame limit while it runs", dir);
    else
        rt_log("frame rate: RivaTuner Statistics Server found in %s; kept out (%s), and while it is kept out its "
               "thread only reads RTSS's own settings for the game, to tell when RTSS limits it a second time", dir,
               rtss_out_text(first));
    return 1;
}

static void worker(void *arg) {
    (void)arg;
    atomic_store(&rtss_tid, plat_thread_id());
    if (!find_rtss()) return;
    atomic_store_explicit(&ready, 1, memory_order_release);   // rtss_limit_restore may read the names now
    int was_running = -1, was_listed = 0, left_done = 0, left_tried = 0;
    const RtssOps ops = {NULL, apply, put_back_op};
    for (;; plat_sleep_ms(POLL_MS)) {
        int64_t w = atomic_load(&want);
        if (w == RTSS_UNSET) continue;
        plat_mutex_lock(&lock);
        if (atomic_load(&closed)) {   // a want read before rtss_limit_restore must not reach the profile after it
            plat_mutex_unlock(&lock);
            continue;
        }
        RtssIn in = {.want = w, .movie_mhz = atomic_load(&movie_mhz), .now_ns = kern_now_ns(),
                     .hosted_ns = gfx_window_hosted_ns()};
        rtss_map(&in.app);
        if (in.app.running != was_running || in.app.listed != was_listed) {
            if (!in.app.running) rt_log("frame rate: RivaTuner Statistics Server is installed but not running");
            else if (!in.app.listed) rt_log("frame rate: RivaTuner Statistics Server runs; it has not hooked the game yet");
            else rt_log("frame rate: RivaTuner Statistics Server has hooked the game");
            was_running = in.app.running, was_listed = in.app.listed;
        }
        in.hooks = dll ? 1 : rtss_wants_hooks(&S, &in) ? load_hooks() : -1;
        if (!left_done && in.app.running && (in.hooks == 1 || !left_tried)) {   // before this run writes anything
            left_tried = 1;
            left_done = left_start(w, in.hooks);
        }
        in.current = atomic_load(&want) == w;
        RtssOut o = rtss_poll(&S, &in, &ops);
        atomic_store(&failed, S.fail);
        if (o.why != RTSS_WHY_NEWER) {   // a newer want waits for the next poll
            char why[128];
            rtss_why_text(why, sizeof why, &o, S.fail);
            set_active(w, o.holds, why);
        }
        watch(w, S.fail, in.app.listed);
        plat_mutex_unlock(&lock);
    }
}

void rtss_limit_want(int64_t mhz) {
    if (atomic_load_explicit(&closed, memory_order_relaxed)) return;   // the exit put the profile back
    // a new limit: the game's own limiter holds it until the worker had RTSS apply it, up to POLL_MS later (a movie's
    // cap that waited for that ran the movie's 60 Hz count fast for half a second): active_for no longer matches
    atomic_store(&want, mhz);
    // The first want starts the worker, a kept-out one too (the worker then only reads, watch). The worker looks for
    // RTSS itself (find_rtss), so the present that gets here waits on no registry key or file.
    if (!atomic_load_explicit(&started, memory_order_relaxed) && !atomic_exchange(&started, 1) &&
        !plat_thread_spawn(worker, NULL, "rtss", 0))
        rt_log("frame rate: the RivaTuner Statistics Server thread did not start");
}

void rtss_limit_movie(int hz) { atomic_store_explicit(&movie_mhz, (int64_t)hz * 1000, memory_order_relaxed); }

int rtss_limit_active(void) {
    if (atomic_load_explicit(&closed, memory_order_relaxed)) return 0;
    int64_t a = atomic_load_explicit(&active_for, memory_order_relaxed);
    return a >= 0 && a == atomic_load_explicit(&want, memory_order_relaxed);
}

int rtss_limit_state(void) {
    if (atomic_load_explicit(&closed, memory_order_relaxed)) return 0;
    int f = atomic_load_explicit(&failed, memory_order_relaxed);
    int64_t w = atomic_load(&want);
    if (w >= 0 && (f == RTSS_FAIL_READONLY || f == RTSS_FAIL_NOT_LIMITING || f == RTSS_FAIL_UNPROVEN))
        return f == RTSS_FAIL_READONLY ? 2 : 3;
    if (w < 0 && w != RTSS_UNSET && atomic_load_explicit(&two, memory_order_relaxed)) return 4;
    return rtss_limit_active() && w > 0;
}

void rtss_limit_restore(void) {
    // The game presents on after this call (the rest of the exit runs first), so from here the game's own limiter holds
    // the frames left and no want reaches the profile again: the worker would otherwise write the game's limit back into
    // the profile just put back, and the next run would take it for the player's own. Set before the check below, so
    // that an exit before the first limit, or while RTSS was kept out, also stops a later want from starting the worker.
    atomic_store(&closed, 1);
    if (!atomic_load_explicit(&ready, memory_order_acquire)) return;
    // the exit may come from a crash on any thread: give up after a second rather than wait on a lock held there
    for (int i = 0; i < 100; i++) {
        if (plat_mutex_trylock(&lock)) {
            if (dll && (S.changed || owed)) put_back(), rtss_put_back_done(&S);
            plat_mutex_unlock(&lock);
            return;
        }
        plat_sleep_ms(10);
    }
}

// A crash's and a fatal error's end (main.c crash_handler, dispatch.c fatal_end), from any thread: no want reaches the
// profile any more, and no call goes into RTSS's library, which may be what failed and is no safe call on a damaged
// heap. The state file keeps this run's line, and the next start puts the profile back from it (restore_left). Without
// that line (the file could not be written) the profile goes back as at an orderly exit, but not from the worker's own
// thread, which may hold the lock that takes.
void rtss_limit_crash(void) {
    atomic_store(&closed, 1);
    atomic_store(&active_for, RTSS_UNSET);
    if (atomic_load(&recorded) || plat_thread_id() == atomic_load(&rtss_tid)) return;
    rtss_limit_restore();
}

#else
void rtss_limit_want(int64_t mhz) { (void)mhz; }
void rtss_limit_movie(int hz) { (void)hz; }
int rtss_limit_active(void) { return 0; }
int rtss_limit_state(void) { return 0; }
void rtss_limit_restore(void) {}
void rtss_limit_crash(void) {}
#endif
