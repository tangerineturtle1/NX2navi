// Built-in resource policies and the mods' Atmosphere cheat files (cheats.h).
// Essentials uses a build-specific, validated GameData lookup rather than emulator heap offsets.
// Each enabled mod the port can use (mods.h) may hold
// <build id>.txt in its cheats folder, the build id the first 8 bytes of main's NSO build id, as Atmosphere and the
// emulators name it; the file name's case does not matter. Each cheat's switch is the setting NX2NAVI_CHEAT_<hash>
// (cheat_<hash> under [cheats] in nx2navi.ini), the hash FNV-1a 32 of "<mod folder name>/<cheat name>", so two mods'
// cheats of one name keep their own switches.
//
// Code cheats (opcode 0 writes into main's code) cannot be applied here: the game's code is lifted C. tools/cheats.py
// turns the code cheats of every mod into patch sets cheat_<hash> when the game is built and writes
// build-cache\cheats.status next to nx2navi.exe, one line per cheat: name hash, content hash, kind, buttons, note
// (tools/cheats.py has the format). A cheat whose status line is missing or has another content hash was added or
// edited after the build: a data cheat runs anyway, a code cheat waits for the setup to run again.
//
// Data cheats run in a VM after Atmosphere's dmnt cheat VM (opcodes 0-A; the 0xC.. and 0xF.. extensions are not
// supported), every presented frame while their switch is on, with registers cleared before each cheat. Memory is the
// guest's, mapped at its own addresses: main's image is written with its pages made writable for the write (read-only
// data included, as dmnt writes through the debugger), everything else after mem_query says the guest may read or write
// it. Buttons are input_poll's HidNpadButton bits, the same as the cheat format's key masks; they read 0 while the menu
// is open.
#include <stdint.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "cfg.h"
#include "cheats.h"
#include "host.h"
#include "input.h"
#include "mods.h"
#include "ui_text.h"
#include "platform/platform.h"

#ifdef _WIN32
#define SEP "\\"
#else
#define SEP "/"
#endif

enum { STATUS_MAX = 160, OPS_PER_RUN = 1 << 20 };

typedef struct {
    CheatInfo info;
    uint32_t *words;
    int n;
    int dup;              // 1 for the first cheat of its name in its file, 2 for the next ...
    uint32_t content;     // FNV-1a 32 of its words as tools/cheats.py writes them
    int set;              // patch set, -1 none
    int basic;            // built-in player resource policy, 0 for an imported cheat
    volatile int32_t on;
    char note[STATUS_MAX];
} Cheat;

static Cheat *g_cheats;
static int g_n, g_cap, g_vm;   // g_vm: some cheat needs cheats_frame
static char g_file[64];        // "<build id>.txt"
static char g_dir[PLAT_PATH_MAX];   // the folder of nx2navi.exe, with a trailing separator
static int g_files;            // mods whose file was read
static uint64_t g_main, g_main_size, g_code_end;   // main's image; its code with the padding up to .rodata
static int g_movement_build;
static int g_sprint_set = -2;
static int g_jump_held;

static uint32_t fnv(const char *s, size_t n) {
    uint32_t h = 0x811C9DC5u;
    for (size_t i = 0; i < n; i++) h = (h ^ (uint8_t)s[i]) * 0x01000193u;
    return h;
}

// The folder of the executable, with a trailing separator.
static int exe_dir(char *out, size_t n) {
    if (!plat_exe_path(out, n)) return 0;
    char *cut = plat_last_sep(out);
    if (!cut) return 0;
    cut[1] = 0;
    return 1;
}

static int build_id(char out[17]) {
    char path[PLAT_PATH_MAX];
    snprintf(path, sizeof path, "%s/main", rt_config()->exefs);
    PlatFile *f = plat_file_open(path);
    if (!f) return 0;
    uint8_t head[0x48];
    int ok = plat_file_pread(f, head, sizeof head, 0) == sizeof head && !memcmp(head, "NSO0", 4);
    plat_file_close(f);
    for (int i = 0; ok && i < 8; i++) snprintf(out + 2 * i, 3, "%02X", head[0x40 + i]);
    return ok;
}

// ---- opcodes

// Words of the opcode at w (left words remain), 0 when it is cut short.
static int op_len(const uint32_t *w, int left) {
    uint32_t f = w[0];
    int t = f >> 28, width = (f >> 24) & 0xF, n;
    switch (t) {
    case 0: case 1: n = width == 8 ? 4 : 3; break;
    case 2: case 8: n = 1; break;
    case 3: n = width == 0 ? 2 : 1; break;
    case 4: case 6: n = 3; break;
    case 5: case 7: n = 2; break;
    case 9: n = 1 + (((f >> 8) & 0xF) ? (width == 8 ? 2 : 1) : 0); break;
    case 0xA: n = ((f >> 8) & 0xF) == 2 || ((f >> 8) & 0xF) == 4 || ((f >> 8) & 0xF) == 5 ? 2 : 1; break;
    default: n = 1; break;
    }
    return n <= left ? n : 0;
}

// NULL when the VM runs the opcode, else why not (tools/cheats.py vm_ok).
static const char *vm_unsupported(const uint32_t *w, char *buf, int n) {
    uint32_t f = w[0];
    int t = f >> 28, width = (f >> 24) & 0xF;
    if (t > 0xA || (t == 5 && ((f >> 12) & 0xF) > 1) || (t == 0xA && ((f >> 8) & 0xF) > 5)) {
        snprintf(buf, n, "opcode %08X is not supported", f);
        return buf;
    }
    if ((t <= 1 || t == 5 || t == 6 || t == 7 || t == 9 || t == 0xA) && width != 1 && width != 2 && width != 4 &&
        width != 8) {
        snprintf(buf, n, "opcode %08X has a bad width", f);
        return buf;
    }
    return NULL;
}

// The kind of a cheat the build did not see: heading, data, rebuild (it writes main's code) or unsupported.
static CheatKind classify(Cheat *c) {
    int ops = 0, ends = 0;
    for (int i = 0; i < c->n;) {
        int len = op_len(c->words + i, c->n - i);
        if (!len) {
            snprintf(c->note, sizeof c->note, "opcode %08X is cut short", c->words[i]);
            return CHEAT_UNSUPPORTED;
        }
        uint32_t f = c->words[i];
        ops++;
        ends += f >> 28 == 2;
        if (f >> 28 == 0 && ((f >> 20) & 0xF) == 0) {
            uint64_t off = ((uint64_t)(f & 0xFF) << 32) | c->words[i + 1];
            if (off < g_code_end - g_main) return CHEAT_REBUILD;
        }
        if (vm_unsupported(c->words + i, c->note, sizeof c->note)) return CHEAT_UNSUPPORTED;
        i += len;
    }
    return ops == ends && ops <= 1 ? CHEAT_HEADING : CHEAT_DATA;
}

// ---- guest memory

static uint64_t region(int m) {
    const RtGuestLayout *l = mem_layout();
    switch (m) {
    case 0: return g_main;
    case 1: return l->heap.base;
    case 2: return l->alias.base;
    case 3: return l->aslr.base;
    default: return 0;
    }
}

// Without a + n: an address near 2^64 (a pointer chain that read -1) would wrap the sum and pass.
static int in_main(uint64_t a, unsigned n) { return a >= g_main && n <= g_main_size && a - g_main <= g_main_size - n; }

static int guest_ok(uint64_t a, unsigned n, uint32_t perm) {
    if (in_main(a, n)) return 1;
    RtMemoryInfo mi;
    if (mem_query(a, &mi) || !mi.state || (mi.perm & perm) != perm) return 0;
    return a + n <= mi.base + mi.size;
}

static int rd(uint64_t a, int width, uint64_t *v) {
    if (!a || !guest_ok(a, (unsigned)width, 1)) return 0;
    uint64_t x = 0;
    memcpy(&x, (const void *)(uintptr_t)a, (size_t)width);
    *v = x;
    return 1;
}

// Writes n bytes into main's image a page at a time: a read-only page is writable for its part of the copy only.
static void wr_main(uint64_t a, const uint8_t *from, int n) {
    while (n > 0) {
        uint64_t page = a & ~0xFFFull, end;
        int k = (int)(page + 0x1000 - a);
        if (k > n) k = n;
        int prot = plat_mem_state(page, page + 0x1000, &end);
        if (prot != PLAT_PROT_READ && prot != PLAT_PROT_RW) return;
        if (prot == PLAT_PROT_READ && !plat_mem_protect(page, 0x1000, PLAT_PROT_RW)) return;
        memcpy((void *)(uintptr_t)a, from, (size_t)k);
        if (prot == PLAT_PROT_READ) plat_mem_protect(page, 0x1000, PLAT_PROT_READ);
        a += (uint64_t)k;
        from += k;
        n -= k;
    }
}

static void wr(uint64_t a, int width, uint64_t v) {
    uint64_t now;
    if (!rd(a, width, &now) || !memcmp(&now, &v, (size_t)width)) return;   // unchanged: no write, no protection change
    void *p = (void *)(uintptr_t)a;
    if (in_main(a, (unsigned)width)) {
        wr_main(a, (const uint8_t *)&v, width);
    } else if (guest_ok(a, (unsigned)width, 2)) {
        memcpy(p, &v, (size_t)width);   // a frozen region thaws on the write (memory.c)
    }
}

// Ocarina 1.1.1's gmd integer table. The manager is reached through the same
// singleton chain as the game's getters; heap allocation addresses are never kept.
// Both the name hash and its expected index must agree before a resource is used.
static uint64_t basic_entry(uint32_t hash, uint32_t expected, unsigned offset, unsigned expected_count,
                            unsigned stride, unsigned value_offset, unsigned width) {
    uint64_t root, manager, count, entries, table, slots;
    if (!rd(g_main + 0x4c7e180, 8, &root) || !rd(root, 8, &manager) ||
        !rd(manager + offset, 4, &count) || count != expected_count ||
        !rd(manager + offset + 8, 8, &entries) || !rd(manager + offset + 16, 8, &table) ||
        !rd(manager + offset + 28, 4, &slots) || slots != 2 * count) return 0;
    uint32_t slot = hash % (uint32_t)slots;
    for (uint32_t tried = 0; tried < slots; tried++, slot = (slot + 1) % (uint32_t)slots) {
        uint64_t key, index;
        if (!rd(table + 8 * slot, 4, &key) || !key) return 0;
        if (key != hash) continue;
        if (!rd(table + 8 * slot + 4, 4, &index) || index != expected || index >= count) return 0;
        uint64_t entry = entries + index * stride, vtable;
        if (!rd(entry, 8, &vtable) || !in_main(vtable, 8) || !guest_ok(entry + value_offset, width, 1)) return 0;
        return entry + value_offset;
    }
    return 0;
}

static uint64_t basic_int(uint32_t hash, uint32_t expected) {
    uint64_t a = basic_entry(hash, expected, 0xd0, 1259, 40, 28, 8);
    return a && guest_ok(a, 8, 2) ? a : 0;
}

static int basic_owned(uint32_t hash, uint32_t expected) {
    uint64_t a = basic_entry(hash, expected, 0x10, 11292, 24, 22, 1), value;
    return a && rd(a, 1, &value) && value == 1;
}

int cheats_bomb_arrow_ammo(int consume) {
    if (!g_movement_build) return 0;
    uint64_t bombs=basic_int(0xdb1bcd4f,1007),bag=basic_int(0xe753508b,972),quiver=basic_int(0x418db791,971);
    uint64_t stock,cap,bow;
    if (!bombs || !bag || !quiver || !rd(bombs,4,&stock) || !rd(bag,4,&cap) || !rd(quiver,4,&bow) ||
        !cap || cap>9999 || !bow || bow>9999 || !stock || stock>cap) return 0;
    if (consume) { wr(bombs,4,stock-1); wr(bombs+4,4,stock-1); }
    return 1;
}

static const uint32_t bottle_valid_hash[CHEAT_BOTTLE_SLOTS] = {0xf39ce410, 0x000ff6ea, 0xf63b1194, 0xb7f052ce, 0xe27b136d};
static const uint32_t bottle_item_hash[CHEAT_BOTTLE_SLOTS] = {0x7d48df76, 0x1edd164a, 0xb80c70a8, 0xe4c1b1fc, 0x365e1953};
static const uint32_t bottle_values[] = {0xbe69de49, 0xeaba6067, 0x049d37c1, 0xf383d6e5, 0x1c293517,
    0x1e834ef8, 0x0f24e808, 0xcdc21471, 0x02084f9e, 0x74d62ecb, 0x6f49e357, 0x73356827, 0xeef6e4b0};

static uint64_t bottle_entry(int slot) {
    if (!g_movement_build || slot < 0 || slot >= CHEAT_BOTTLE_SLOTS || !basic_owned(bottle_valid_hash[slot], 10756 + slot)) return 0;
    return basic_entry(bottle_item_hash[slot], 60 + slot, 0x250, 130, 56, 28, 8);
}

int cheats_bottle_content(int slot) {
    uint64_t a = bottle_entry(slot), value;
    if (!a || !rd(a, 4, &value)) return -1;
    for (int i = 0; i < (int)(sizeof bottle_values / sizeof *bottle_values); i++)
        if (value == bottle_values[i]) return i;
    return -1;
}

int cheats_bottle_set(int slot, int content) {
    if (content < 0 || content >= (int)(sizeof bottle_values / sizeof *bottle_values)) return 0;
    uint64_t a = bottle_entry(slot), count, values;
    // The enum's native list must also contain the requested value. Unlock flags and bottle stock are read-only.
    if (!a || !guest_ok(a, 8, 2) || !rd(a + 12, 4, &count) || count != 13 || !rd(a + 20, 8, &values)) return 0;
    for (uint32_t i = 0; i < count; i++) {
        uint64_t value;
        if (!rd(values + i * 4, 4, &value)) return 0;
        if (value == bottle_values[content]) {
            wr(a, 4, value);
            wr(a + 4, 4, value);
            return 1;
        }
    }
    return 0;
}

static uint64_t g_health_binding;
static uint64_t g_magic_binding;

static int magic_binding(uint64_t a, uint64_t *value) {
    uint64_t key, max_key, extra_key;
    // Native resource controller passed to main+0x1639b0. These three GameData keys identify the magic pool.
    return rd(a + 0x38, 4, &key) && key == 0x76b18200 &&
           rd(a + 0x3c, 4, &max_key) && max_key == 0x12d63a97 &&
           rd(a + 0x40, 4, &extra_key) && extra_key == 0x3afa3349 &&
           rd(a + 0x44, 4, value) && *value <= 9999 && guest_ok(a + 0x44, 4, 2);
}

// The player's live linked integers feed GameData every tick. Writing the GameData
// copy alone changes the display briefly, then loses to the next linked-value sync.
static int health_binding(uint64_t a, uint64_t *value, uint64_t *capacity) {
    uint64_t vt, next_vt, life_key, max_key;
    return rd(a, 8, &vt) && vt == g_main + 0x4b63438 &&
           rd(a + 12, 4, &life_key) && life_key == 0x31ab5580 &&
           rd(a + 16, 8, &next_vt) && next_vt == vt &&
           rd(a + 28, 4, &max_key) && max_key == 0xfbe01da1 &&
           rd(a + 8, 4, value) && rd(a + 24, 4, capacity) &&
           *capacity >= 4 && *capacity <= 160 && !(*capacity % 4) && *value <= *capacity &&
           guest_ok(a + 8, 4, 2);
}

// The existing entry observer is enabled only until the game synchronizes this
// binding. No heap scan or persistent function tracing runs while playing.
void cheats_fn_hit(uint64_t addr, uint64_t x0) {
    uint64_t value, capacity;
    if (addr == g_main + 0x1639b0 && (__atomic_load_n(&g_fnprof, __ATOMIC_RELAXED) & FNPROF_MAGIC) && magic_binding(x0, &value)) {
        __atomic_store_n(&g_magic_binding, x0, __ATOMIC_RELEASE);
        __atomic_fetch_and(&g_fnprof, ~FNPROF_MAGIC, __ATOMIC_RELAXED);
        rt_log("cheats: active magic binding resolved");
    }
    if (addr != g_main + 0x163b20) return;
    if (!(__atomic_load_n(&g_fnprof, __ATOMIC_RELAXED) & FNPROF_CHEATS) || !health_binding(x0, &value, &capacity)) return;
    __atomic_store_n(&g_health_binding, x0, __ATOMIC_RELEASE);
    __atomic_fetch_and(&g_fnprof, ~FNPROF_CHEATS, __ATOMIC_RELAXED);
    rt_log("cheats: active health binding resolved");
}

static void basic_health(void) {
    uint64_t a = __atomic_load_n(&g_health_binding, __ATOMIC_ACQUIRE), value, capacity, saved_cap;
    uint64_t max = basic_int(0xfbe01da1, 964);
    if (!a || !health_binding(a, &value, &capacity) || !max || !rd(max, 4, &saved_cap) || saved_cap != capacity) {
        __atomic_store_n(&g_health_binding, 0, __ATOMIC_RELEASE);
        __atomic_fetch_or(&g_fnprof, FNPROF_CHEATS, __ATOMIC_RELAXED);
        return;
    }
    if (value && value < capacity) wr(a + 8, 4, capacity);   // never revive after death or add containers
}

static void basic_magic(void) {
    uint64_t cap_target = basic_int(0x12d63a97, 967), capacity, value;
    if (!cap_target || !rd(cap_target, 4, &capacity) || !capacity || capacity > 9999) return;
    uint64_t a = __atomic_load_n(&g_magic_binding, __ATOMIC_ACQUIRE);
    if (!a || !magic_binding(a, &value) || value > capacity) {
        __atomic_store_n(&g_magic_binding, 0, __ATOMIC_RELEASE);
        __atomic_fetch_or(&g_fnprof, FNPROF_MAGIC, __ATOMIC_RELAXED);
        return;
    }
    if (value < capacity) wr(a + 0x44, 4, capacity);
}

static void basic_refill(uint32_t hash, uint32_t index, uint32_t cap_hash, uint32_t cap_index, int hearts) {
    uint64_t target = basic_int(hash, index), cap_target = basic_int(cap_hash, cap_index), value, capacity;
    if (!target || !cap_target || !rd(target, 4, &value) || !rd(cap_target, 4, &capacity)) return;
    // Zero capacity means the item/bag has not been obtained. Health never revives
    // a dead player or changes the maximum number of hearts.
    if (!capacity || capacity > (hearts ? 160u : 9999u) || value > 9999 ||
        (hearts && (!value || capacity % 4 || value >= capacity))) return;
    wr(target, 4, capacity);
    wr(target + 4, 4, capacity);   // keep the buffered value used by saved game state in agreement
}

static uint64_t g_shared_timer_entry;
static float g_shared_timer_held;

static void basic_shared_countdown(void) {
    // The shared minigame/event timer uses 60-Hz ticks. Keep its running flag intact so objectives still work.
    uint64_t active = basic_entry(0xf0312188, 10101, 0x10, 11292, 24, 22, 1);
    uint64_t down = basic_entry(0xdd49184a, 10100, 0x10, 11292, 24, 22, 1), running, countdown;
    uint64_t a = basic_entry(0x9d350590, 67, 0x190, 89, 40, 28, 8), raw;
    if (!active || !down || !a || !rd(active, 1, &running) || running != 1 ||
        !rd(down, 1, &countdown) || countdown != 1 || !rd(a, 4, &raw) || !guest_ok(a, 8, 2)) {
        g_shared_timer_entry = 0; return;
    }
    uint32_t bits = (uint32_t)raw;
    float value;
    memcpy(&value, &bits, 4);
    if (!isfinite(value) || value <= 0 || value > 216000) { g_shared_timer_entry = 0; return; }
    // Rebase when a timer starts, changes duration, or receives an explicit large time adjustment.
    if (a != g_shared_timer_entry || value > g_shared_timer_held + .1f || g_shared_timer_held - value > 120.0f) {
        g_shared_timer_entry = a;
        g_shared_timer_held = value;
    }
    memcpy(&bits, &g_shared_timer_held, 4);
    wr(a, 4, bits);
    wr(a + 4, 4, bits);
}

static void basic_fixed_item(uint32_t hash, uint32_t index, uint32_t owned_hash, uint32_t owned_index, uint32_t max) {
    uint64_t a, value;
    if (!basic_owned(owned_hash, owned_index) || !(a = basic_int(hash, index)) ||
        !rd(a, 4, &value) || value > max) return;
    wr(a, 4, max);
    wr(a + 4, 4, max);
}

static void basic_run(int kind) {
    switch (kind) {
    case 1: basic_health(); break;
    case 2: basic_refill(0xa77921d7, 962, 0xe0010a50, 976, 0); break;
    case 3: basic_refill(0xdb1bcd4f, 1007, 0xe753508b, 972, 0); break;
    case 4: break;   // legacy combined switch: migrated to the independent ammunition policies at startup
    case 6: basic_magic(); break;
    case 7: break;   // legacy combined items switch; independent policies below own all replenishment
    case 8: basic_refill(0x518ec74b, 1006, 0x418db791, 971, 0); break;
    case 9:
        if (basic_owned(0x5ab49c8e, 8852)) basic_refill(0xdf7c517a, 1011, 0xe753508b, 972, 0);
        break;
    case 10: basic_refill(0xf30d9f54, 1019, 0x735b00f7, 973, 0); break;
    case 11: basic_refill(0x5fadf7fc, 1020, 0xae8cc742, 974, 0); break;
    case 12: basic_refill(0xb9494411, 1021, 0x457616b0, 975, 0); break;
    case 13: basic_fixed_item(0xb003124d, 1032, 0x6d39c600, 8896, 10); break;
    case 14: basic_fixed_item(0x600e29eb, 1057, 0xcfbb16f6, 8934, 30); break;
    case 15: basic_shared_countdown(); break;
    }
}

static void basic_add(const char *bid) {
    if (strcmp(bid, "E0F3CCFAF50065AE")) return;
    g_movement_build = 1;
    static const char *const names[] = {"Infinite health", "Infinite rupees", "Infinite bombs", "Infinite ammo",
                                         "Infinite challenge timers", "Infinite magic", "Infinite items",
                                         "Infinite arrows", "Infinite bombchus", "Infinite Deku nuts", "Infinite Deku seeds", "Infinite Deku sticks",
                                         "Infinite magic beans", "Infinite Scrub seeds", "Freeze shared countdown"};
    static const char *const notes[] = {
        "Refills your existing hearts while alive. Does not add heart containers or revive after death.",
        "Keeps rupees at your current wallet capacity. Does not upgrade the wallet.",
        "Refills bombs to your current bomb-bag capacity. Obtain a bomb bag first.",
        "Refills arrows, Deku seeds, nuts and sticks to their current capacities. Does not unlock items or upgrades.",
        "Not available yet for this build. Quest and minigame timers still run; bomb fuses are unchanged.",
        "Refills magic to your existing magic-meter capacity. Obtain a magic meter first. Does not unlock or upgrade it.",
        "Refills owned magic beans and Scrub seeds. Set ammunition separately. Does not unlock items.",
        "Refills arrows to the current quiver capacity.", "Refills owned bombchus to the current bomb-bag capacity.",
        "Refills Deku nuts to their current capacity.", "Refills Deku seeds to their current capacity.",
        "Refills Deku sticks to their current capacity.",
        "Keeps already obtained magic beans at the native limit of 10.",
        "Keeps already obtained Scrub business seeds at the native limit of 30.",
        "Freezes the shared minigame/event countdown. Count-up timers and unrelated timer systems are unchanged."};
    g_cap = 32;
    g_cheats = (Cheat *)calloc((size_t)g_cap, sizeof *g_cheats);
    if (!g_cheats) rt_fatal("cheats: out of memory");
    for (int i = 0; i < 15; i++) {
        Cheat *c = &g_cheats[g_n++];
        c->info.name = strdup(names[i]);
        c->info.mod = "Essentials";
        c->set = -1;
        c->dup = 1;
        c->basic = i + 1;
        snprintf(c->note, sizeof c->note, "%s", notes[i]);
    }
}

// Player-specific parameters and the character controller; no position writes or global time scaling.
static uint64_t move_ptr(uint64_t a) {
    uint64_t p = 0;
    return rd(a, 8, &p) ? p : 0;
}

static int move_float(uint64_t a, float *out) {
    uint64_t bits;
    if (!rd(a, 4, &bits)) return 0;
    uint32_t b = (uint32_t)bits;
    memcpy(out, &b, 4);
    return isfinite(*out);
}

static void move_write_float(uint64_t a, float value) {
    uint32_t bits;
    memcpy(&bits, &value, 4);
    wr(a, 4, bits);
}

static uint64_t move_param(uint64_t components) {
    uint64_t bb = move_ptr(components + 0xa8), count, entries;
    if (!bb || !rd(bb + 0x30, 4, &count) || count < 1 || count > 4096 ||
        !(entries = move_ptr(bb + 0x38))) return 0;
    // The same sorted blackboard lookup the native jump action uses.
    uint32_t lo = 0, hi = (uint32_t)count;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        uint64_t entry = move_ptr(entries + 8ull * mid), hash;
        if (!entry || !rd(entry, 4, &hash)) return 0;
        if (hash < 0x6b7e094a) lo = mid + 1;
        else if (hash > 0x6b7e094a) hi = mid;
        else {
            uint64_t p = move_ptr(move_ptr(entry + 8) + 0x150);
            return p && move_ptr(p) == g_main + 0x4b6d1a8 ? p : 0;
        }
    }
    return 0;
}

// Called only on the native charged-spin branches of Weapon::getAttackShapeScale.
// The game still owns sword-specific geometry, attack windows, magic checks and shape restoration.
// s0 is (length scale - 1), s1 the thickness scale; the caller adds 1 to s0 afterwards.
void cheats_spin_range_host(Cpu *c) {
    if (!g_movement_build) return;
    const char *mode = cfg_value("NX2NAVI_SPIN_RANGE");
    float factor = !strcmp(mode, "1.5") ? 1.5f : !strcmp(mode, "2") ? 2.0f : !strcmp(mode, "2.5") ? 2.5f : 1.0f;
    if (factor == 1) return;
    uint64_t weapon = c->x[20], sword = move_ptr(weapon + 0x18);
    uint64_t actor = move_ptr(move_ptr(g_main + 0x4e371d0) + 0x480);
    uint64_t components = move_ptr(actor + 0x270), equipment = move_ptr(components + 0x200);
    uint64_t entry = move_ptr(equipment + 0x28), generation, actual;
    if (!actor || move_ptr(actor) != g_main + 0x4b25e18 || !equipment || !entry ||
        !rd(equipment + 0x30, 4, &generation) || generation == 0xffffffffu ||
        !rd(entry + 0x48, 4, &actual) || generation != actual || move_ptr(entry + 0x40) != sword ||
        move_ptr(move_ptr(sword + 0x270) + 0x458) != weapon) return;
    uint64_t name = move_ptr(sword + 0x20);
    if (!name || !guest_ok(name, 13, 1) || memcmp((void *)(uintptr_t)name, "Weapon_Sword_", 13)) return;
    float length, radius;
    memcpy(&length, &c->v[0], 4);
    memcpy(&radius, &c->v[1], 4);
    if (!isfinite(length) || !isfinite(radius) || length <= -1 || length > 20 || radius < 1 || radius > 20) return;
    length = (length + 1) * factor - 1;
    radius *= factor;
    memcpy(&c->v[0], &length, 4);
    memcpy(&c->v[1], &radius, 4);
}

static void movement_frame(uint64_t keys, int32_t lx, int32_t ly) {
    static int charge_set = -2;
    if (charge_set == -2) charge_set = rt_patch_set("spin_instant");
    int instant = g_movement_build && !strcmp(cfg_value("NX2NAVI_SPIN_CHARGE"), "instant");
    if (charge_set >= 0 && rt_patch_enabled(charge_set) != instant) rt_patch_switch(charge_set, instant);
    int jump_down = (keys & (1ull << 2)) != 0;   // native jump action after input mapping
    int jump_press = jump_down && !g_jump_held;
    g_jump_held = jump_down;
    const char *mode = cfg_value("NX2NAVI_SPRINT_SPEED");
    float multiplier = !strcmp(mode, "2") ? 2.0f : !strcmp(mode, "3.5") ? 3.5f : !strcmp(mode, "5") ? 5.0f : 1.0f;
    int sprint = multiplier > 1 && (keys & 1) && (lx > 4096 || lx < -4096 || ly > 4096 || ly < -4096);
    int jump = cfg_bool("NX2NAVI_AIR_JUMPS") && jump_press;
    // Only the native player movement speed is scaled. Native input, animation, gravity and collisions stay in charge.
    if (g_sprint_set == -2) g_sprint_set = rt_patch_set("sprint_boost");
    void *page = rt_patch_page();
    if (g_movement_build && page && g_sprint_set >= 0) {
        float factor = sprint ? multiplier : 1.0f;
        uint32_t bits;
        memcpy(&bits, &factor, 4);
        __atomic_store_n((uint32_t *)((uint8_t *)page + 384), bits, __ATOMIC_RELAXED);
        if (rt_patch_enabled(g_sprint_set) != sprint) rt_patch_switch(g_sprint_set, sprint);
    }
    if (!g_movement_build || !jump) return;
    uint64_t actor = move_ptr(move_ptr(g_main + 0x4e371d0) + 0x480);
    char name[7];
    uint64_t name_ptr = move_ptr(actor + 0x20);
    if (!actor || move_ptr(actor) != g_main + 0x4b25e18 || !name_ptr ||
        !guest_ok(name_ptr, sizeof name, 1)) return;
    memcpy(name, (const void *)(uintptr_t)name_ptr, sizeof name);
    if (memcmp(name, "Player", sizeof name)) return;
    uint64_t components = move_ptr(actor + 0x270), player = move_ptr(components + 0x358);
    if (!components || !player || move_ptr(player) != g_main + 0x4b4dfc0) return;
    uint64_t param = move_param(components);
    if (!param) return;
    if (jump) {
        uint64_t physics = move_ptr(components + 0x50), cct = move_ptr(physics + 0x20);
        uint64_t character = move_ptr(cct + 0x10), motion = move_ptr(character + 8), state = move_ptr(motion + 0x30);
        uint64_t parent = move_ptr(state + 0x30), flags;
        float height;
        if (!physics || !cct || !character || !motion || !state || !parent ||
            move_ptr(state) != g_main + 0x4b9c298 || move_ptr(state + 0x10) != character ||
            !move_float(param + 0x334, &height) || height <= 0 || height > 5 ||
            !rd(parent + 0x20, 4, &flags) || !guest_ok(state + 0x3c, 0x11, 2) ||
            !guest_ok(parent + 0x20, 4, 2)) return;
        // The native setter requests one impulse and marks the controller dirty. The physics update consumes it.
        move_write_float(state + 0x3c, height);
        wr(state + 0x4c, 1, 1);
        wr(parent + 0x20, 4, flags | 2);
    }
}

static uint64_t masked(int width, uint64_t v) { return width >= 8 ? v : v & ((1ull << (8 * width)) - 1); }

static uint64_t arith(int op, uint64_t a, uint64_t b) {
    switch (op) {
    case 0: return a + b;
    case 1: return a - b;
    case 2: return a * b;
    case 3: return b < 64 ? a << b : 0;
    case 4: return b < 64 ? a >> b : 0;
    case 5: return a & b;
    case 6: return a | b;
    case 7: return ~a;
    case 8: return a ^ b;
    default: return a;   // 9: move
    }
}

static int compare(int cond, uint64_t a, uint64_t b) {
    switch (cond) {
    case 1: return a > b;
    case 2: return a >= b;
    case 3: return a < b;
    case 4: return a <= b;
    case 5: return a == b;
    case 6: return a != b;
    default: return 0;
    }
}

// From i (after a failed condition, or an else reached while running), the opcode to continue at: after the
// matching else (else_ok) or end.
static int skip(const uint32_t *w, int n, int i, int else_ok) {
    int depth = 0;
    while (i < n) {
        int len = op_len(w + i, n - i);
        if (!len) return n;
        uint32_t f = w[i];
        int t = f >> 28;
        i += len;
        if (t == 1 || t == 8) depth++;
        else if (t == 2 && ((f >> 24) & 0xF) == 0 && depth-- == 0) return i;
        else if (t == 2 && depth == 0 && else_ok) return i;
    }
    return n;
}

static uint64_t imm64(const uint32_t *w) { return (uint64_t)w[0] << 32 | w[1]; }

static void run(const Cheat *c, uint64_t keys) {
    uint64_t r[16] = {0};
    int loop[16] = {0};
    const uint32_t *w = c->words;
    int n = c->n, budget = OPS_PER_RUN;
    for (int i = 0; i < n && budget-- > 0;) {
        int len = op_len(w + i, n - i);
        if (!len) return;
        uint32_t f = w[i];
        int t = f >> 28, width = (f >> 24) & 0xF, x = (f >> 20) & 0xF, y = (f >> 16) & 0xF, z = (f >> 12) & 0xF;
        uint64_t rel = ((uint64_t)(f & 0xFF) << 32) | (len > 1 ? w[i + 1] : 0), v;
        switch (t) {
        case 0:   // 0TMR00AA AAAAAAAA V..: store a value at region M + A + register R
            wr(region(x) + rel + r[y], width, width == 8 ? imm64(w + i + 2) : w[i + 2]);
            break;
        case 1:   // 1TMC00AA AAAAAAAA V..: if (region M + A) <cond C> V
            if (!rd(region(x) + rel, width, &v) || !compare(y, v, width == 8 ? imm64(w + i + 2) : w[i + 2])) {
                i = skip(w, n, i + len, 1);
                continue;
            }
            break;
        case 2:   // 20000000 end, 21000000 else: an else reached while running skips to the end
            if (width == 1) {
                i = skip(w, n, i + len, 0);
                continue;
            }
            break;
        case 3:   // 300R0000 VVVVVVVV loop start, 310R0000 loop end
            if (width == 0) {
                r[y] = w[i + 1];
                loop[y] = i + len;
            } else if (r[y] && --r[y]) {
                i = loop[y];
                continue;
            }
            break;
        case 4:   // 400R0000 V V: register R = V
            r[y] = imm64(w + i + 1);
            break;
        case 5:   // 5TMRI0AA AAAAAAAA: R = (I ? R : region M) + A, read
            if (!rd(z ? r[y] + rel : region(x) + rel, width, &v)) return;   // a broken pointer chain: nothing more
            r[y] = v;
            break;
        case 6: {   // 6T0RIor0 V V: store V at R (+ register r); I: R += width
            uint64_t a = r[y] + (((f >> 8) & 0xF) ? r[(f >> 4) & 0xF] : 0);
            wr(a, width, imm64(w + i + 1));
            if (z) r[y] += (uint64_t)width;
            break;
        }
        case 7:   // 7T0RC000 VVVVVVVV: R = R <op C> V
            r[y] = masked(width, arith(z, r[y], w[i + 1]));
            break;
        case 8:   // 8kkkkkkk: if the buttons are held
            if ((keys & (f & 0x0FFFFFFF)) != (f & 0x0FFFFFFF)) {
                i = skip(w, n, i + len, 1);
                continue;
            }
            break;
        case 9: {   // 9TCRS0s0 / 9TCRS100 V..: R = S <op C> (s or V)
            uint64_t b = (f >> 8) & 0xF ? (width == 8 ? imm64(w + i + 1) : w[i + 1]) : r[(f >> 4) & 0xF];
            r[y] = masked(width, arith(x, r[z], b));
            break;
        }
        case 0xA: {   // ATSRIOxa (aaaaaaaa): store register S at an address of type O
            int o = (f >> 8) & 0xF, m = (f >> 4) & 0xF;
            uint64_t imm = ((uint64_t)(f & 0xF) << 32) | (len > 1 ? w[i + 1] : 0), a;
            switch (o) {
            case 0: a = r[y]; break;
            case 1: a = r[y] + r[m]; break;
            case 2: a = r[y] + imm; break;
            case 3: a = region(m) + r[y]; break;
            case 4: a = region(m) + imm; break;
            default: a = region(m) + r[y] + imm; break;
            }
            wr(a, width, r[x]);
            if (z) r[y] += (uint64_t)width;
            break;
        }
        default:
            return;
        }
        i += len;
    }
}

// ---- the file

static char *read_text(const char *path) {
    uint64_t n = 0;
    char *s = (char *)plat_file_read(path, &n);
    if (s && n >= 3 && !memcmp(s, "\xEF\xBB\xBF", 3)) memmove(s, s + 3, (size_t)n - 2);   // a UTF-8 BOM
    return s;
}

static void add_word(Cheat *c, uint32_t v, int *cap) {
    if (c->n == *cap) {
        *cap = *cap ? *cap * 2 : 16;
        c->words = (uint32_t *)realloc(c->words, sizeof *c->words * (size_t)*cap);
        if (!c->words) rt_fatal("cheats: out of memory");
    }
    c->words[c->n++] = v;
}

// Appends the cheats of one mod's file; mod: the mod's folder name (kept).
static void parse(char *s, const char *mod) {
    int first = g_n, wcap = 0;
    for (char *line = s; line && *line;) {
        char *end = strchr(line, '\n'), *next = end ? end + 1 : NULL;
        if (end) *end = 0;
        while (*line == ' ' || *line == '\t') line++;
        size_t len = strlen(line);
        while (len && (line[len - 1] == '\r' || line[len - 1] == ' ' || line[len - 1] == '\t')) line[--len] = 0;
        if (len >= 2 && (line[0] == '[' || line[0] == '{') && (line[len - 1] == ']' || line[len - 1] == '}')) {
            if (g_n == g_cap) {
                g_cap = g_cap ? g_cap * 2 : 32;
                g_cheats = (Cheat *)realloc(g_cheats, sizeof *g_cheats * (size_t)g_cap);
                if (!g_cheats) rt_fatal("cheats: out of memory");
            }
            Cheat *c = &g_cheats[g_n++];
            memset(c, 0, sizeof *c);
            line[len - 1] = 0;
            char *name = line + 1;
            while (*name == ' ' || *name == '\t') name++;
            size_t k = strlen(name);
            while (k && (name[k - 1] == ' ' || name[k - 1] == '\t')) name[--k] = 0;
            c->info.name = strdup(name);
            c->info.mod = mod;
            c->info.kind = line[0] == '{' ? CHEAT_MASTER : CHEAT_HEADING;
            c->set = -1;
            c->dup = 1;   // a repeated name gets "#2", "#3" ... (tools/cheats.py parse)
            for (int j = first; j < g_n - 1; j++) c->dup += !strcmp(g_cheats[j].info.name, c->info.name);
            wcap = 0;
        } else if (g_n > first) {
            for (char *tok = strtok(line, " \t"); tok; tok = strtok(NULL, " \t")) {
                char *e;
                unsigned long v = strtoul(tok, &e, 16);
                if (e - tok == 8 && !*e) add_word(&g_cheats[g_n - 1], (uint32_t)v, &wcap);
            }
        }
        line = next;
    }
}

// The file g_file in folder, its name in any case, into out; 0 when there is none.
typedef struct {
    const char *dir;
    char *out;
    size_t cap;
    int found;
} FindFile;
static int find_cb(const char *name, int kind, uint64_t size, void *ctx) {
    (void)size;
    FindFile *f = (FindFile *)ctx;
    if (kind == PLAT_FS_FILE && !strcasecmp(name, g_file) &&
        snprintf(f->out, f->cap, "%s/%s", f->dir, name) < (int)f->cap)
        f->found = 1;
    return !f->found;
}
static int find_file(const char *folder, char *out, size_t cap) {
    FindFile f = {folder, out, cap, 0};
    plat_fs_list(folder, find_cb, &f);
    return f.found;
}

// The status line of key, its fields split in place; 0 when none.
static int status_of(char *status, const char *key, char **fields) {
    for (char *line = status; line && *line;) {
        char *end = strchr(line, '\n'), *next = end ? end + 1 : NULL;
        if (!strncmp(line, key, 8) && line[8] == '\t') {
            static char copy[512];
            size_t n = end ? (size_t)(end - line) : strlen(line);
            if (n >= sizeof copy) n = sizeof copy - 1;
            memcpy(copy, line, n);
            copy[n] = 0;
            if (n && copy[n - 1] == '\r') copy[n - 1] = 0;
            int k = 0;
            fields[k++] = copy;
            for (char *p = copy; *p && k < 5; p++)
                if (*p == '\t') *p = 0, fields[k++] = p + 1;
            while (k < 5) fields[k++] = (char *)"";
            return 1;
        }
        line = next;
    }
    return 0;
}

// The menu's font has no numero sign (U+2116), which cheat names often start with: they show "No" instead.
static void plain_name(char *s) {
    for (char *p; (p = strstr(s, "\xE2\x84\x96")) != NULL;) {
        p[0] = 'N', p[1] = 'o';
        memmove(p + 2, p + 3, strlen(p + 3) + 1);
    }
}

// ---- rebuild: code cheats and ExeFS patches the build lacks
// The setup writes build-cache\rebuild.txt (installer/steps.py write_rebuild_command), the command that starts its
// window in rebuild mode on this install folder, one argument per line. When an enabled mod has code cheats this build
// lacks (CHEAT_REBUILD) or ExeFS patch files it lacks (mods_exefs_pending), the game runs it and exits; the setup lifts
// and compiles them in and starts the game again.
// build-cache\rebuild.skip holds the key of the cheats a rebuild was started for: a rebuild that failed or was
// cancelled leaves them unbuilt, and the same set does not start it again. NX2NAVI_MODS_REBUILD=off never starts it.
// Scripted runs (--seconds, --boot) never start it.
static void rebuild_if_needed(const char *dir) {
    uint32_t key = 0x811C9DC5u;
    int need = 0;
    for (int i = 0; i < g_n; i++) {
        const Cheat *c = &g_cheats[i];
        if (c->info.kind != CHEAT_REBUILD) continue;
        need++;
        key = (key ^ fnv(c->info.key, strlen(c->info.key))) * 0x01000193u;
        key = (key ^ c->content) * 0x01000193u;
    }
    int patches = mods_exefs_pending(&key);
    if (!need && !patches) return;
    char cmd_path[PLAT_PATH_MAX + 64], skip_path[PLAT_PATH_MAX + 64], skip[16] = "", want[16];
    snprintf(cmd_path, sizeof cmd_path, "%sbuild-cache" SEP "rebuild.txt", dir);
    snprintf(skip_path, sizeof skip_path, "%sbuild-cache" SEP "rebuild.skip", dir);
    snprintf(want, sizeof want, "%08X", key);
    char *old = read_text(skip_path);
    if (old) snprintf(skip, sizeof skip, "%.8s", old);
    free(old);
    const char *why = !strcmp(cfg_value("NX2NAVI_MODS_REBUILD"), "off") ? "mods_rebuild = off"
                    : rt_config()->seconds || rt_config()->boot   ? "a scripted run"
                    : !strcmp(skip, want)                           ? "the last rebuild for them failed or was cancelled"
                                                                    : NULL;
    char *cmd = why ? NULL : read_text(cmd_path);
    if (!why && !cmd) why = "no build-cache/rebuild.txt (a build without the setup)";
    if (why) {
        rt_log("cheats: %d code cheats and %d ExeFS patch files are not built into the game; no rebuild: %s", need,
               patches, why);
        free(cmd);
        return;
    }
    const char *argv[32];
    int argc = 0;
    for (char *line = cmd; line && *line && argc < 31;) {
        char *end = strchr(line, '\n'), *next = end ? end + 1 : NULL;
        if (end) *end = 0;
        size_t n = strlen(line);
        if (n && line[n - 1] == '\r') line[n - 1] = 0;
        if (*line) argv[argc++] = line;
        line = next;
    }
    argv[argc] = NULL;
    if (argc < 2 || !plat_file_write(skip_path, want, strlen(want))) {
        rt_log("cheats: %d code cheats and %d ExeFS patch files are not built into the game; cannot start the "
               "rebuild (%s)", need, patches, cmd_path);
        free(cmd);
        return;
    }
    rt_log("cheats: %d code cheats and %d ExeFS patch files are not built into the game; starting the rebuild: %s",
           need, patches, argv[0]);
    int ok = plat_spawn(argv, dir);
    free(cmd);
    if (!ok) {
        rt_log("cheats: the rebuild did not start; the game starts with the code it has");
        return;
    }
    prof_write();   // plat_exit runs no atexit handlers
    rtss_limit_restore();
    plat_exit(0);
}

void cheats_init(void) {
    const RtLoaded *m = ldr_count() ? ldr_module(0) : NULL;
    char dir[PLAT_PATH_MAX], bid[17], path[2400];   // path: a mod's cheats folder (ModInfo.cheats) + the file
    if (!m || !exe_dir(dir, sizeof dir) || !build_id(bid)) return;
    snprintf(g_dir, sizeof g_dir, "%s", dir);
    g_main = m->base;
    g_main_size = m->size;
    g_code_end = m->base + ((m->text_end + 0xFFF) & ~0xFFFull);
    snprintf(g_file, sizeof g_file, "%s.txt", bid);
    basic_add(bid);
    mods_scan();
    for (int k = 0; k < mods_count(); k++) {   // the Mods tab's order, top first
        const ModInfo *mod = mods_at(k);
        if (!mod->enabled || mod->problem != MOD_OK || !mod->cheats[0] || !find_file(mod->cheats, path, sizeof path))
            continue;
        char *text = read_text(path);
        if (!text) {
            rt_log("cheats: %s: cannot read %s", mod->name, path);
            continue;
        }
        parse(text, strdup(mod->name));
        free(text);
        g_files++;
    }
    if (!g_files && !g_n) {
        rt_log("cheats: no enabled mod holds cheats/%s", g_file);
        rebuild_if_needed(dir);   // an enabled mod's ExeFS patches may still wait for it
        return;
    }
    snprintf(path, sizeof path, "%sbuild-cache" SEP "cheats.status", dir);
    char *status = read_text(path);
    int counts[CHEAT_UNSUPPORTED + 1] = {0}, seen_dups = 0;
    for (int i = 0; i < g_n; i++) {
        Cheat *c = &g_cheats[i];
        char named[900];
        snprintf(named, sizeof named, c->dup > 1 ? "%s/%s#%d" : "%s/%s", c->info.mod, c->info.name, c->dup);
        seen_dups += c->dup > 1;
        char key[9], content[9];
        snprintf(key, sizeof key, "%08X", fnv(named, strlen(named)));
        size_t tn = (size_t)c->n * 9 + 1, tl = 0;
        char *t = (char *)malloc(tn);
        if (!t) rt_fatal("cheats: out of memory");
        t[0] = 0;
        for (int k = 0; k < c->n; k++) tl += (size_t)snprintf(t + tl, tn - tl, k ? " %08X" : "%08X", c->words[k]);
        c->content = fnv(t, tl);
        snprintf(content, sizeof content, "%08X", c->content);
        free(t);
        snprintf(c->info.key, sizeof c->info.key, "NX2NAVI_CHEAT_%s", key);
        int master = c->info.kind == CHEAT_MASTER;
        char *f[5];
        CheatKind kind;
        if (c->basic) {
            kind = c->basic == 5 ? CHEAT_UNSUPPORTED : CHEAT_DATA;
        } else if (status && status_of(status, key, f) && !strcmp(f[1], content)) {
            const char *k = f[2];
            kind = !strcmp(k, "code") ? CHEAT_CODE : !strcmp(k, "key") ? CHEAT_KEY : !strcmp(k, "master") ? CHEAT_MASTER
                 : !strcmp(k, "restore") ? CHEAT_RESTORE : !strcmp(k, "heading") ? CHEAT_HEADING
                 : !strcmp(k, "data") ? CHEAT_DATA : CHEAT_UNSUPPORTED;
            c->info.buttons = (uint32_t)strtoul(f[3], NULL, 16);
            snprintf(c->note, sizeof c->note, "%s", f[4]);
            if (kind == CHEAT_CODE || kind == CHEAT_KEY || kind == CHEAT_MASTER) {
                char set[32];
                snprintf(set, sizeof set, "cheat_%08x", fnv(named, strlen(named)));
                c->set = rt_patch_set(set);
                if (c->set < 0 && kind != CHEAT_MASTER) kind = CHEAT_REBUILD;
            }
        } else {
            kind = classify(c);
        }
        if (master && kind == CHEAT_DATA) kind = CHEAT_MASTER;   // a data master code: the VM, always on
        c->info.kind = kind;
        c->info.note = c->note;
        const char *v = cfg_get(c->info.key);
        c->on = kind == CHEAT_MASTER || (v && *v && strcmp(v, "0") != 0);
        if (kind == CHEAT_KEY && c->set >= 0) rt_patch_switch(c->set, 0);   // on while the buttons are held
        if (kind == CHEAT_KEY || kind == CHEAT_DATA || (kind == CHEAT_MASTER && c->set < 0)) g_vm = 1;
        counts[kind]++;
    }
    free(status);
    int legacy_ammo = 0, legacy_items = 0, migrated = 0;
    for (int i = 0; i < g_n; i++) {
        if (g_cheats[i].basic == 4) legacy_ammo = g_cheats[i].on;
        if (g_cheats[i].basic == 7) legacy_items = g_cheats[i].on;
    }
    for (int i = 0; i < g_n; i++) {
        Cheat *c = &g_cheats[i];
        if (c->basic >= 8 && c->basic <= 12 && (c->basic == 9 ? legacy_items : legacy_ammo) && !cfg_get(c->info.key)) {
            cfg_set(c->info.key, "1");
            c->on = 1;
            migrated = 1;
        }
        if (c->basic >= 13 && c->basic <= 14 && legacy_items && !cfg_get(c->info.key)) {
            cfg_set(c->info.key, "1"); c->on = 1; migrated = 1;
        }
    }
    if (migrated && !cfg_save()) rt_log("cheats: could not save the independent ammunition settings");
    rebuild_if_needed(dir);
    for (int i = 0; i < g_n; i++) plain_name((char *)g_cheats[i].info.name);   // the hashes above read the file's bytes
    int on = 0;
    for (int i = 0; i < g_n; i++) on += g_cheats[i].on && g_cheats[i].info.kind != CHEAT_HEADING;
    rt_log("cheats: %s in %d mods: %d cheats, %d on; %d code, %d while buttons are held, %d data, %d master, "
           "%d restore, %d headings, %d wait for a rebuild, %d not supported%s",
           g_file, g_files, g_n, on, counts[CHEAT_CODE], counts[CHEAT_KEY], counts[CHEAT_DATA], counts[CHEAT_MASTER],
           counts[CHEAT_RESTORE], counts[CHEAT_HEADING], counts[CHEAT_REBUILD], counts[CHEAT_UNSUPPORTED],
           seen_dups ? " (repeated names numbered)" : "");
}

int cheats_rebuild_pending(void) {
    char path[PLAT_PATH_MAX + 64];
    snprintf(path, sizeof path, "%sbuild-cache" SEP "rebuild.txt", g_dir);
    uint32_t key = 0;
    int need = mods_exefs_pending(&key);
    for (int i = 0; i < g_n && !need; i++) need = g_cheats[i].info.kind == CHEAT_REBUILD;
    return need && g_dir[0] && plat_file_exists(path);
}

void cheats_rebuild_allow(void) {
    char path[PLAT_PATH_MAX + 64];
    snprintf(path, sizeof path, "%sbuild-cache" SEP "rebuild.skip", g_dir);
    plat_fs_unlink(path);
}

int cheats_count(void) { return g_n; }

const CheatInfo *cheats_info(int i) { return i >= 0 && i < g_n ? &g_cheats[i].info : NULL; }

int cheats_on(int i) { return i >= 0 && i < g_n && g_cheats[i].on; }

const char *cheats_file(void) { return g_file; }

int cheats_builtin_kind(int i) { return i >= 0 && i < g_n ? g_cheats[i].basic : 0; }

const char *cheats_display_name(int i) {
    static const UiTextId ids[] = {UI_MENU_CHEAT_HEALTH, UI_MENU_CHEAT_RUPEES, UI_MENU_CHEAT_BOMBS,
        UI_MENU_CHEAT_AMMO, UI_MENU_CHEAT_TIMERS, UI_MENU_CHEAT_MAGIC, UI_MENU_CHEAT_ITEMS,
        UI_MENU_CHEAT_ARROWS, UI_MENU_CHEAT_BOMBCHUS, UI_MENU_CHEAT_NUTS, UI_MENU_CHEAT_SEEDS, UI_MENU_CHEAT_STICKS,
        UI_MENU_CHEAT_BEANS, UI_MENU_CHEAT_SCRUB_SEEDS, UI_MENU_TIMER_MINIGAME};
    int kind = cheats_builtin_kind(i);
    if (kind >= 1 && kind <= (int)(sizeof ids / sizeof *ids)) return menu_text(ids[kind - 1]);
    return i >= 0 && i < g_n ? g_cheats[i].info.name : "";
}

const char *cheats_display_note(int i) {
    static const UiTextId ids[] = {UI_MENU_CHEAT_HEALTH_DESC, UI_MENU_CHEAT_RUPEES_DESC, UI_MENU_CHEAT_BOMBS_DESC,
        UI_MENU_CHEAT_AMMO_DESC, UI_MENU_CHEAT_TIMERS_DESC, UI_MENU_CHEAT_MAGIC_DESC, UI_MENU_CHEAT_ITEMS_DESC,
        UI_MENU_CHEAT_ARROWS_DESC, UI_MENU_CHEAT_BOMBCHUS_DESC, UI_MENU_CHEAT_NUTS_DESC, UI_MENU_CHEAT_SEEDS_DESC, UI_MENU_CHEAT_STICKS_DESC,
        UI_MENU_CHEAT_BEANS_DESC, UI_MENU_CHEAT_SCRUB_SEEDS_DESC, UI_MENU_TIMER_MINIGAME_DESC};
    int kind = cheats_builtin_kind(i);
    if (kind >= 1 && kind <= (int)(sizeof ids / sizeof *ids)) return menu_text(ids[kind - 1]);
    return i >= 0 && i < g_n ? g_cheats[i].note : "";
}

void cheats_set(int i, int on) {
    if (i < 0 || i >= g_n) return;
    Cheat *c = &g_cheats[i];
    if (c->info.kind != CHEAT_CODE && c->info.kind != CHEAT_KEY && c->info.kind != CHEAT_DATA) return;
    plat_xchg32(&c->on, on != 0);
    if (c->basic == 1 && !on) {
        __atomic_store_n(&g_health_binding, 0, __ATOMIC_RELEASE);
        __atomic_fetch_and(&g_fnprof, ~FNPROF_CHEATS, __ATOMIC_RELAXED);
    }
    if (c->basic == 6 && !on) {
        __atomic_store_n(&g_magic_binding, 0, __ATOMIC_RELEASE);
        __atomic_fetch_and(&g_fnprof, ~FNPROF_MAGIC, __ATOMIC_RELAXED);
    }
    if (c->basic == 15 && !on) g_shared_timer_entry = 0;
    if (c->info.kind == CHEAT_CODE) rt_patch_switch(c->set, on);   // CHEAT_KEY: cheats_frame follows the buttons
    cfg_set(c->info.key, on ? "1" : "0");
    rt_log("cheats: %s/%s %s", c->info.mod, c->info.name, on ? "on" : "off");
}

void cheats_frame(uint64_t frame) {
    if (!g_vm || frame < 60) return;   // the game polls its input (and starts SDL) before this
    uint64_t keys = 0;
    int32_t lx, ly, rx, ry;
    input_poll(&keys, &lx, &ly, &rx, &ry);   // 0 while the menu is open
    movement_frame(keys, lx, ly);
    for (int i = 0; i < g_n; i++) {
        Cheat *c = &g_cheats[i];
        switch (c->info.kind) {
        case CHEAT_KEY:
            if (c->set >= 0) {
                int want = c->on && (keys & c->info.buttons) == c->info.buttons;
                if (rt_patch_enabled(c->set) != want) rt_patch_switch(c->set, want);
            }
            break;
        case CHEAT_DATA:
            if (c->on) {
                if (c->basic) basic_run(c->basic);
                else run(c, keys);
            }
            break;
        case CHEAT_MASTER:
            if (c->set < 0) run(c, keys);
            break;
        default:
            break;
        }
    }
}

const char *cheats_buttons(uint32_t mask, char *buf, int n) {
    static const char *const names[] = {"A", "B", "X", "Y", "Left stick press", "Right stick press", "L", "R", "ZL",
                                        "ZR", "+", "-", "D-pad left", "D-pad up", "D-pad right", "D-pad down",
                                        "Left stick left", "Left stick up", "Left stick right", "Left stick down",
                                        "Right stick left", "Right stick up", "Right stick right", "Right stick down"};
    int k = 0;
    buf[0] = 0;
    for (int b = 0; b < 24; b++)
        if (mask & (1u << b)) k += snprintf(buf + k, (size_t)(n > k ? n - k : 0), "%s%s", k ? " + " : "", names[b]);
    return buf;
}
