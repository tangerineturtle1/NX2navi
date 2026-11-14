// Exercise the real built-in policies against synthetic guest memory. No game files.
#include <assert.h>
#include "../runtime/cheats.c"

static uint8_t *arena;
static size_t cursor;
static const size_t arena_size = 0x5000000;
static uint64_t manager;
volatile int g_fnprof;

static uint64_t alloc_bytes(size_t n) {
    uint64_t a = (uint64_t)(uintptr_t)(arena + cursor);
    cursor += (n + 15) & ~(size_t)15;
    assert(cursor < 0x4000000);
    return a;
}
static void u32(uint64_t a, uint32_t v) { memcpy((void *)(uintptr_t)a, &v, 4); }
static void u64(uint64_t a, uint64_t v) { memcpy((void *)(uintptr_t)a, &v, 8); }
static uint32_t get32(uint64_t a) { uint32_t v; memcpy(&v, (void *)(uintptr_t)a, 4); return v; }
static void f32(uint64_t a, float v) { memcpy((void *)(uintptr_t)a, &v, 4); }
static float getfloat(uint64_t a) { float v; memcpy(&v, (void *)(uintptr_t)a, 4); return v; }

extern const char *test_spin_range;
static void spin_checks(void) {
    uint64_t mgr=alloc_bytes(0x500), actor=alloc_bytes(0x500), components=alloc_bytes(0x500);
    uint64_t equipment=alloc_bytes(0x100), ref=alloc_bytes(0x80), sword=alloc_bytes(0x300);
    uint64_t sword_components=alloc_bytes(0x500), weapon=alloc_bytes(0x200), name=alloc_bytes(32);
    u64(g_main+0x4e371d0,mgr); u64(mgr+0x480,actor); u64(actor,g_main+0x4b25e18);
    u64(actor+0x270,components);u64(components+0x200,equipment);u64(equipment+0x28,ref);
    u32(equipment+0x30,123);u32(ref+0x48,123);u64(ref+0x40,sword);
    u64(sword+0x270,sword_components);u64(sword_components+0x458,weapon);u64(weapon+0x18,sword);
    u64(sword+0x20,name);memcpy((void *)(uintptr_t)name,"Weapon_Sword_Kokiri",20);
    const char *modes[]={"normal","1.5","2","2.5"};float factors[]={1,1.5f,2,2.5f};
    for(int i=0;i<4;i++) {
        Cpu cpu={0};cpu.x[20]=weapon;cpu.nzcv=0xa0000000;cpu.x[19]=0x12345;
        float length=.53f,radius=10;
        memcpy(&cpu.v[0],&length,4);memcpy(&cpu.v[1],&radius,4);
        Cpu before=cpu;test_spin_range=modes[i];cheats_spin_range_host(&cpu);
        memcpy(&length,&cpu.v[0],4);memcpy(&radius,&cpu.v[1],4);
        assert(fabsf((length+1)-1.53f*factors[i])<.00001f && radius==10*factors[i]);
        cpu.v[0]=before.v[0];cpu.v[1]=before.v[1];assert(!memcmp(&cpu,&before,sizeof cpu));
    }
    Cpu cpu={0};cpu.x[20]=weapon;Cpu before=cpu;
    u32(ref+0x48,124);cheats_spin_range_host(&cpu);assert(!memcmp(&cpu,&before,sizeof cpu));
    u32(ref+0x48,123);memcpy((void *)(uintptr_t)name,"Weapon_Hammer",14);
    cheats_spin_range_host(&cpu);assert(!memcmp(&cpu,&before,sizeof cpu));
    u64(weapon+0x18,0);cheats_spin_range_host(&cpu);assert(!memcmp(&cpu,&before,sizeof cpu));
    g_movement_build=0;cheats_spin_range_host(&cpu);assert(!memcmp(&cpu,&before,sizeof cpu));
    g_movement_build=1;test_spin_range="normal";
}

uint32_t mem_query(uint64_t a, RtMemoryInfo *out) {
    uint64_t base = (uint64_t)(uintptr_t)arena;
    if (a < base || a - base >= arena_size) return 1;
    *out = (RtMemoryInfo){.base = base, .size = arena_size, .state = 1, .perm = 3};
    return 0;
}
int plat_mem_state(uint64_t a, uint64_t b, uint64_t *end) { (void)a; *end = b; return PLAT_PROT_RW; }
int plat_mem_protect(uint64_t a, uint64_t n, int prot) { (void)a; (void)n; (void)prot; return 1; }
void rt_log(const char *fmt, ...) { (void)fmt; }
void rt_fatal(const char *fmt, ...) { (void)fmt; abort(); }

static uint64_t table(unsigned offset, uint32_t count, unsigned stride) {
    uint64_t entries = alloc_bytes((size_t)count * stride);
    uint64_t hashes = alloc_bytes((size_t)count * 2 * 8);
    u32(manager + offset, count);
    u64(manager + offset + 8, entries);
    u64(manager + offset + 16, hashes);
    u32(manager + offset + 28, 2 * count);
    return entries;
}
static uint64_t entry(unsigned offset, uint32_t hash, uint32_t index, unsigned stride, unsigned value) {
    uint64_t hashes, entries;
    memcpy(&hashes, (void *)(uintptr_t)(manager + offset + 16), 8);
    memcpy(&entries, (void *)(uintptr_t)(manager + offset + 8), 8);
    uint32_t slots = get32(manager + offset + 28), slot = hash % slots;
    while (get32(hashes + 8ull * slot)) slot = (slot + 1) % slots;
    u32(hashes + 8ull * slot, hash);
    u32(hashes + 8ull * slot + 4, index);
    u64(entries + (uint64_t)index * stride, g_main + 0x100);
    return entries + (uint64_t)index * stride + value;
}

int main(void) {
    arena = calloc(1, arena_size);
    assert(arena);
    g_main = (uint64_t)(uintptr_t)arena;
    g_main_size = 0x1000;
    cursor = 0x2000;
    uint64_t root = alloc_bytes(8);
    manager = alloc_bytes(0x300);
    u64(g_main + 0x4c7e180, root); u64(root, manager);
    table(0xd0, 1259, 40);
    table(0x10, 11292, 24);
    table(0x250, 130, 56);
    table(0x190, 89, 40);
    basic_add("0000000000000000"); assert(g_n == 0);
    basic_add("E0F3CCFAF50065AE"); assert(g_n == 15);

    uint64_t hearts = entry(0xd0, 0xfbe01da1, 964, 40, 28);
    uint64_t health = alloc_bytes(32);
    u32(hearts, 12);
    u64(health, g_main + 0x4b63438); u32(health + 8, 11); u32(health + 12, 0x31ab5580);
    u64(health + 16, g_main + 0x4b63438); u32(health + 24, 12); u32(health + 28, 0xfbe01da1);
    g_health_binding = health;
    basic_run(1); assert(get32(health + 8) == 12 && get32(hearts) == 12);
    u32(health + 8, 0); basic_run(1); assert(get32(health + 8) == 0);
    u32(health + 8, 11); u32(hearts, 16); basic_run(1); assert(get32(health + 8) == 11);
    assert(!g_health_binding && (g_fnprof & FNPROF_CHEATS));

    uint64_t max_magic = entry(0xd0, 0x12d63a97, 967, 40, 28);
    uint64_t magic = alloc_bytes(0x48);
    u32(max_magic, 240); u32(magic + 0x38, 0x76b18200);
    u32(magic + 0x3c, 0x12d63a97); u32(magic + 0x40, 0x3afa3349); u32(magic + 0x44, 180);
    g_magic_binding = magic;
    basic_run(6); assert(get32(magic + 0x44) == 240 && get32(max_magic) == 240);
    u32(max_magic, 0); u32(magic + 0x44, 180); basic_run(6); assert(get32(magic + 0x44) == 180);

    uint64_t rupees = entry(0xd0, 0xa77921d7, 962, 40, 28);
    uint64_t wallet = entry(0xd0, 0xe0010a50, 976, 40, 28);
    u32(rupees, 3); u32(rupees + 4, 3); u32(wallet, 200);
    basic_run(2); assert(get32(rupees) == 200 && get32(rupees + 4) == 200 && get32(wallet) == 200);
    u32(rupees, 3); u32(wallet, 0); basic_run(2); assert(get32(rupees) == 3);
    u32(wallet, 200); u32(manager + 0xd0, 1258); basic_run(2); assert(get32(rupees) == 3);
    u32(manager + 0xd0, 1259);

    static const uint32_t stock_hash[] = {0xdb1bcd4f, 0x518ec74b, 0xdf7c517a, 0xf30d9f54, 0x5fadf7fc, 0xb9494411};
    static const uint32_t stock_index[] = {1007, 1006, 1011, 1019, 1020, 1021};
    static const uint32_t cap_hash[] = {0xe753508b, 0x418db791, 0xe753508b, 0x735b00f7, 0xae8cc742, 0x457616b0};
    static const uint32_t cap_index[] = {972, 971, 972, 973, 974, 975};
    static const int policies[] = {3, 8, 9, 10, 11, 12};
    uint64_t stocks[6];
    uint64_t chu_owned = entry(0x10, 0x5ab49c8e, 8852, 24, 22);
    for (int i = 0; i < 6; i++) {
        stocks[i] = entry(0xd0, stock_hash[i], stock_index[i], 40, 28);
        u32(entry(0xd0, cap_hash[i], cap_index[i], 40, 28), 30);
    }
    u32(stocks[2], 1); basic_run(9); assert(get32(stocks[2]) == 1);
    *(uint8_t *)(uintptr_t)chu_owned = 1;
    for (int i = 0; i < 6; i++) {
        for (int j = 0; j < 6; j++) { u32(stocks[j], 1); u32(stocks[j] + 4, 1); }
        basic_run(policies[i]);
        for (int j = 0; j < 6; j++) {
            assert(get32(stocks[j]) == (i == j ? 30u : 1u));
            assert(get32(stocks[j] + 4) == (i == j ? 30u : 1u));
        }
    }

    // Reserving a bomb must neither spend arrows twice nor grant missing equipment.
    uint64_t bag=basic_int(0xe753508b,972),quiver=basic_int(0x418db791,971);
    u32(stocks[0],2);u32(stocks[0]+4,2);u32(stocks[1],7);u32(stocks[1]+4,7);
    assert(cheats_bomb_arrow_ammo(0) && get32(stocks[0])==2);
    assert(cheats_bomb_arrow_ammo(1) && get32(stocks[0])==1 && get32(stocks[0]+4)==1);
    assert(get32(stocks[1])==7 && get32(stocks[1]+4)==7);
    assert(cheats_bomb_arrow_ammo(1) && get32(stocks[0])==0);
    assert(!cheats_bomb_arrow_ammo(1) && get32(stocks[0])==0);
    u32(stocks[0],2);u32(bag,0);assert(!cheats_bomb_arrow_ammo(1) && get32(stocks[0])==2);
    u32(bag,30);u32(quiver,0);assert(!cheats_bomb_arrow_ammo(1) && get32(stocks[0])==2);
    u32(quiver,30);u32(stocks[0],31);assert(!cheats_bomb_arrow_ammo(1));u32(stocks[0],2);

    uint64_t beans = entry(0xd0, 0xb003124d, 1032, 40, 28);
    uint64_t owned = entry(0x10, 0x6d39c600, 8896, 24, 22);
    basic_run(13); assert(get32(beans) == 0);
    *(uint8_t *)(uintptr_t)owned = 1; basic_run(13);
    assert(get32(beans) == 10 && get32(beans + 4) == 10);
    u32(beans, 2); basic_run(4); basic_run(7); assert(get32(beans) == 2);

    for (int slot = 0; slot < CHEAT_BOTTLE_SLOTS; slot++) {
        uint64_t valid = entry(0x10, bottle_valid_hash[slot], 10756 + slot, 24, 22);
        uint64_t bottle = entry(0x250, bottle_item_hash[slot], 60 + slot, 56, 28);
        uint64_t values = alloc_bytes(sizeof bottle_values);
        memcpy((void *)(uintptr_t)values, bottle_values, sizeof bottle_values);
        u64(bottle + 20, values); u32(bottle + 12, 13);
        assert(cheats_bottle_content(slot) == -1 && !cheats_bottle_set(slot, 0));
        *(uint8_t *)(uintptr_t)valid = 1;
        for (int content = 0; content < 13; content++) {
            assert(cheats_bottle_set(slot, content));
            assert(cheats_bottle_content(slot) == content && get32(bottle + 4) == bottle_values[content]);
        }
        assert(!cheats_bottle_set(slot, -1) && !cheats_bottle_set(slot, 13));
        u32(bottle + 12, 12); assert(!cheats_bottle_set(slot, 0));
    }
    assert(cheats_bottle_content(-1) == -1 && !cheats_bottle_set(5, 0));

    uint64_t active = entry(0x10, 0xf0312188, 10101, 24, 22);
    uint64_t down = entry(0x10, 0xdd49184a, 10100, 24, 22);
    uint64_t timer = entry(0x190, 0x9d350590, 67, 40, 28);
    *(uint8_t *)(uintptr_t)active = *(uint8_t *)(uintptr_t)down = 1;
    f32(timer, 1800); basic_run(15); f32(timer, 1799); basic_run(15);
    assert(getfloat(timer) == 1800 && getfloat(timer + 4) == 1800);
    *(uint8_t *)(uintptr_t)down = 0; f32(timer, 1801); basic_run(15); assert(getfloat(timer) == 1801);
    *(uint8_t *)(uintptr_t)down = 1; f32(timer, 0); basic_run(15); assert(getfloat(timer) == 0);
    f32(timer, NAN); basic_run(15); assert(isnan(getfloat(timer)));
    spin_checks();
    puts("cheats_check: passed (resources, bottles, countdown, spin multipliers and stale/non-sword guards)");
    return 0;
}
