// --probe-plugins: the plugin interface (plugins.c) as a plugin uses it, through the table the executable exports: the
// version answer, the game's description, the memory calls on mapped, unmapped and read-only memory, each kind of
// callback with its removal and the table's limit, the input capture's count, and the guest call's refusal outside a
// frame callback, and the loading of the libraries NX2NAVI_MOD_DLLS names. Function hooks are registered on real
// function starts of the linked modules and run through plugins_hook_enter with a stand-in for the lifted function:
// the argument changes, call_original, the skip, the order of several hooks and what the caller's registers hold
// afterwards. Needs no game files and starts no guest thread, so no guest call and no lifted code runs here.
#include <stdio.h>
#include <string.h>

#include "dispatch.h"
#include "host.h"
#include "platform/platform.h"
#include "plugin_game_ui.h"
#include "plugins.h"
#include "steam.h"
#include "save_zip.h"

static int fails;
#define CHECK(c, what) do { if (!(c)) { fails++; fprintf(stderr, "FAIL plugins: %s\n", what); } } while (0)

static const Nx2PluginApi *api;
static int frame_hits, overlay_hits, key_hits, text_hits;
static uint64_t frame_index, call_result = 1;
static unsigned last_key, last_text;

static void on_frame(const Nx2Frame *f, void *user) {
    frame_hits += user == &frame_hits && f->size == sizeof *f;
    frame_index = f->index;
    call_result = api->guest_call(api->game_info()->main_base, NULL, 0);   // not a guest thread: refused, 0
}
static void on_frame_removes(const Nx2Frame *f, void *user) { (void)f, api->remove(*(int *)user); }
static void on_overlay(const Nx2Draw *d, void *user) { overlay_hits += user == &overlay_hits && d->width == 640; }
static int on_key(const Nx2KeyEvent *e, void *user) {
    key_hits++, last_key = e->key | (unsigned)e->down << 16 | (unsigned)e->repeat << 17;
    return user != NULL;
}
static void on_text(uint32_t c, void *user) { (void)user, text_hits++, last_text = c; }

// Function hooks. body stands for the lifted function: x0 = 2 * x0 + x1, and it leaves marks in registers a callee may
// change. The hooks note their order in `order`, one digit each.
static int body_hits, exit_hits, order;
static uint64_t seen_sp, seen_lr, seen_fp, body_x19;
static void body(Cpu *c) {
    body_hits++, body_x19 = c->x[19];
    c->x[0] = 2 * c->x[0] + c->x[1];
    c->x[9] = c->x[17] = 0xdead, c->v[0] = 7, c->v[20] = 9;
}
static void hook_args(Nx2HookCall *k, void *user) {   // changes an argument and lets the function run
    order = order * 10 + 1;
    seen_sp = k->sp, seen_lr = k->lr, seen_fp = NX2_HAS(k, Nx2HookCall, fp) ? k->fp : 0;
    k->x[1] += (uint64_t)(uintptr_t)user;
}
static void hook_orig(Nx2HookCall *k, void *user) {   // runs the function itself and changes the result
    (void)user;
    order = order * 10 + 2;
    uint64_t r = api->call_original(k);
    k->x[0] = r + 100;
}
static void hook_skip(Nx2HookCall *k, void *user) {   // leaves the function out
    order = order * 10 + 3;
    k->skip = 1, k->x[0] = (uint64_t)(uintptr_t)user, k->v[0][0] = 42;
}
static void hook_twice(Nx2HookCall *k, void *user) {  // runs the function twice, the second time on the first result
    (void)user;
    api->call_original(k);
    api->call_original(k);
}
static void on_exit_cb(void *user) { exit_hits += user == &exit_hits; }

// Title menu buttons and pad events. The buttons are checked the drawn way (plugins.c "title menu buttons"): the title
// menu is stood in for by an object in the guest heap with the fields the runtime reads, handed to the menu's update
// hook with a stand-in for the function. Of the game's own entry, which needs the game's layout, the parts that do not
// are checked: its text, and the hook of the sequence's node that takes its choice.
static int title_hits, pad_hits;
static unsigned pad_last;
static uint64_t title_call;
static void on_title(void *user) {
    title_hits += (int)(uintptr_t)user;
    title_call = api->guest_call(api->game_info()->main_base, NULL, 0);   // allowed as in a frame callback; no guest thread here
}
static void on_pad(const Nx2PadEvent *e, void *user) { (void)user, pad_hits++, pad_last = e->button | (unsigned)e->down << 8; }
static void no_body(Cpu *c) { (void)c; }

static void probe_title(void) {
    uint64_t heap = 0, menu_update = api->game_info()->main_base + 0x377af70;
    if (rt_game_version != 111 || !rt_dispatch_lookup(menu_update)) {
        Nx2TitleButton b = {.size = sizeof b, .label = "x", .on_select = on_title};
        CHECK(!api->add_title_button(&b), "a title menu button in a game whose title menu is not known");
        printf("probe-plugins: not game 1.1.1: title menu buttons only refused\n");
        return;
    }
    plugin_game_ui_for_probe();
    CHECK(mem_set_heap_size(0x200000, &heap) == 0 && heap, "SetHeapSize for the title menu");
    if (!heap) return;
    uint64_t menu = heap + 0x1000, entries = heap + 0x4000;
    uint32_t shown = 0x20000, fading = 0x10000, count = 3;
#define MENU(state, cursor) do { uint64_t e_ = entries, c_ = entries + 0xe8 + 0x228 * (uint64_t)(cursor);                      \
        api->write(menu + 0x380, &(state), 4), api->write(menu + 0x340, &count, 4), api->write(menu + 0x310, &e_, 8);       \
        api->write(menu + 0x188, &c_, 8), c->x[0] = menu, plugins_hook_enter(c, menu_update, no_body); } while (0)
#define POLL(b_, ly_) (buttons = (b_), lx = 0, ly = (ly_), rx = ry = 0, plugins_title_poll(&buttons, &lx, &ly, &rx, &ry))
    Cpu *c = plat_aligned_alloc(sizeof(Cpu), 64);
    memset(c, 0, sizeof *c);
    const char *label[NX2_TITLE_BUTTONS_MAX], *desc[NX2_TITLE_BUTTONS_MAX];
    float cy[NX2_TITLE_BUTTONS_MAX], half[NX2_TITLE_BUTTONS_MAX], text[NX2_TITLE_BUTTONS_MAX];
    int sel = 7, lx, ly, rx, ry;
    uint64_t buttons, down = 1ull << 15, up = 1ull << 13, a = 1ull << 0;

    Nx2TitleButton b = {.size = sizeof b, .order = 5, .label = "Second", .description = "(later)", .on_select = on_title,
                        .user = (void *)(uintptr_t)10};
    Nx2TitleButton bad = b;
    bad.label = NULL;
    CHECK(!api->add_title_button(NULL) && !api->add_title_button(&bad), "a title menu button without a label");
    bad = b, bad.on_select = NULL;
    CHECK(!api->add_title_button(&bad), "a title menu button without a callback");
    bad = b, bad.size = 8;
    CHECK(!api->add_title_button(&bad), "a title menu button from a structure that is too small");
    int h2 = api->add_title_button(&b);
    b.order = 1, b.label = "First", b.description = NULL, b.user = (void *)(uintptr_t)1;
    int h1 = api->add_title_button(&b);
    CHECK(h1 > 0 && h2 > 0 && h1 != h2 && (g_fnprof & RT_FN_HOOKS), "title menu button handles, and the menu's hook");
    // the game's own entry: its text in the game's message format
    char etext[128];
    static const char small_tag[] = {0x0e, 0, 0, 2, 0, 2, 0, 80, 0};
    int tn = plugins_title_text(etext, sizeof etext, "Co-op", NULL, 0);
    CHECK(tn == 5 && !strcmp(etext, "Co-op"), "the entry's text for one button without a description");
    tn = plugins_title_text(etext, sizeof etext, "Param\xc3\xa8tres", "(mods)", 0);
    CHECK(tn == 11 + 9 + 7 && !memcmp(etext, "Param\xc3\xa8tres", 11) && !memcmp(etext + 11, small_tag, 9) &&
          !strcmp(etext + 20, " (mods)"), "the entry's text with a description: the size tag, UTF-8 kept");
    tn = plugins_title_text(etext, sizeof etext, "A\nB", NULL, 1);
    CHECK(tn == 9 && !strcmp(etext, "<  A B  >"), "the entry's text for one of several buttons, a control byte made a space");
    CHECK(plugins_title_text(etext, 8, "A long label", "and more", 1) < 8, "the entry's text overran a small buffer");
    // ... and its choice: the node's update with our control chosen queues the callback and empties the part's name
    uint64_t node = heap + 0x8000, name = node + 0x58, control = heap + 0xa000, zero = 0;
    api->write(node + 0x48, &name, 8), api->write(name, "Pa_Button_03", 13), api->write(node + 0xd8, &control, 8);
    api->write(node + 0x30, &control, 8);   // still queued at the screen: not answered yet
    plugins_title_probe_decide(c, node, control);
    plugins_frame(c);
    CHECK(!title_hits && *(const char *)(uintptr_t)name == 'P', "the entry was chosen while the node still waited");
    api->write(node + 0x30, &zero, 8);
    plugins_title_probe_decide(c, node, control + 0x228);
    plugins_frame(c);
    CHECK(!title_hits && *(const char *)(uintptr_t)name == 'P', "another control's choice was taken for the entry's");
    plugins_title_probe_decide(c, node, control);
    CHECK(title_hits == 1 && title_call == 0 && !*(const char *)(uintptr_t)name,
          "the entry's choice calls the first button's callback and hands the sequence an empty name");
    Nx2Menu no_menu = {.size = sizeof no_menu};
    CHECK(!api->open_menu(&no_menu) && !api->update_menu(&no_menu), "a menu page outside a title button's callback");
    api->close_menu();
    title_hits = 0;
    plugins_title_force_drawn();   // from here on the drawn buttons
    CHECK(!plugins_title_buttons(NX2_TITLE_BUTTONS_MAX, label, desc, cy, half, text, &sel) && !plugins_overlay_wanted(),
          "title menu buttons show before the title menu does");
    MENU(fading, 0);
    CHECK(!plugins_title_buttons(NX2_TITLE_BUTTONS_MAX, label, desc, cy, half, text, &sel), "buttons while the title fades in");
    MENU(shown, 0);
    CHECK(plugins_title_buttons(NX2_TITLE_BUTTONS_MAX, label, desc, cy, half, text, &sel) == 2 && sel == -1 &&
          !strcmp(label[0], "First") && !strcmp(label[1], "Second") && !strcmp(desc[1], "(later)") && !desc[0][0] &&
          cy[0] > 0.869f && cy[1] > cy[0] && cy[1] + half[1] <= 1.0f && plugins_overlay_wanted(),
          "the title menu's buttons, by order, under the game's entries and on the picture");
    // the game's cursor on its first entry: down is the game's
    POLL(down, 0);
    CHECK(buttons == down && plugins_title_buttons(4, label, desc, cy, half, text, &sel) && sel == -1, "down on the game's first entry");
    POLL(0, 0);
    // on its last entry: down moves into our buttons and the game gets nothing
    MENU(shown, 2);
    POLL(down, 0);
    CHECK(!buttons && plugins_title_buttons(4, label, desc, cy, half, text, &sel) && sel == 0, "down past the game's last entry");
    POLL(0, 0), POLL(0, -32000);
    CHECK(!ly && plugins_title_buttons(4, label, desc, cy, half, text, &sel) && sel == 1, "the stick moves to the second button");
    POLL(0, 0), POLL(down, 0);
    CHECK(plugins_title_buttons(4, label, desc, cy, half, text, &sel) && sel == 1, "down on the last button moved on");
    POLL(0, 0), POLL(a, 0);
    CHECK(!buttons && !title_hits, "the game got the confirm press, or the callback ran at once");
    plugins_frame(c);
    CHECK(title_hits == 10 && title_call == 0, "the chosen button's callback at the next frame");
    MENU(shown, 2);
    POLL(0, 0), POLL(up, 0), POLL(0, 0), POLL(up, 0);
    CHECK(!buttons && plugins_title_buttons(4, label, desc, cy, half, text, &sel) && sel == -1, "up from the first button");
    POLL(up, 0);
    CHECK(!buttons, "the game got the press that left the buttons while it was still held");
    POLL(0, 0), POLL(up, 0);
    CHECK(buttons == up, "the game's input did not come back after the release");
    // gone when the menu's update stops (a submenu, a loaded save): a few frames later nothing shows
    POLL(0, 0), POLL(down, 0);
    for (int i = 0; i < 6; i++) plugins_frame(c);
    CHECK(!plugins_title_buttons(4, label, desc, cy, half, text, &sel) && !plugins_overlay_wanted(), "buttons after the title menu went");
    POLL(0, 0), POLL(down, 0);
    CHECK(buttons == down, "input was kept from the game after the title menu went");
    api->remove(h1);
    MENU(shown, 2);
    CHECK(plugins_title_buttons(4, label, desc, cy, half, text, &sel) == 1 && !strcmp(label[0], "Second"), "a removed title menu button");
    int hs[NX2_TITLE_BUTTONS_MAX + 2], taken = 0;
    for (int i = 0; i < NX2_TITLE_BUTTONS_MAX + 2; i++) taken += (hs[i] = api->add_title_button(&b)) > 0;
    CHECK(taken == NX2_TITLE_BUTTONS_MAX - 1, "the title menu takes NX2_TITLE_BUTTONS_MAX buttons");
    for (int i = 0; i < NX2_TITLE_BUTTONS_MAX + 2; i++) api->remove(hs[i]);
    api->remove(h2);
    for (int i = 0; i < 6; i++) plugins_frame(c);
#undef MENU
#undef POLL
    // pad events: every change of a button, the left stick as directions
    plugins_pad(1, 0, 0);
    CHECK(!pad_hits, "a pad event without a callback");
    int hp = api->on_pad(on_pad, NULL);
    plugins_pad(0, 0, 0), plugins_pad(1ull << NX2_PAD_B, 0, 0);
    CHECK(hp > 0 && pad_hits == 1 && pad_last == (NX2_PAD_B | 1u << 8), "a pad button going down");
    plugins_pad(1ull << NX2_PAD_B, 0, 30000);
    CHECK(pad_hits == 2 && pad_last == (NX2_PAD_UP | 1u << 8), "the left stick as a direction");
    plugins_pad(0, 0, 0);
    CHECK(pad_hits == 4, "the releases");
    api->remove(hp);
    plat_aligned_free(c);
    mem_set_heap_size(0, &heap);
}

static void probe_hooks(void) {
    static RtModule mods[16];
    hostfn_seal();   // the dispatch table, from the function tables linked into this executable
    int n = rt_linked_modules(mods, 16);
    uint64_t fa = 0, fb = 0;
    for (int i = 0; i < n; i++)
        if (!strcmp(mods[i].name, "main") && mods[i].count > 1) fa = mods[i].addrs[0], fb = mods[i].addrs[1];
    if (!fa) {
        printf("probe-plugins: no lifted main in this executable: function hooks not checked\n");
        return;
    }
    Cpu *c = plat_aligned_alloc(sizeof(Cpu), 64);
    memset(c, 0, sizeof *c);
    CHECK(NX2_HAS(api, Nx2PluginApi, on_exit) && api->hook_function && api->call_original, "the version 2 members");
    CHECK(!api->hook_function(fa, NULL, NULL) && !api->hook_function(fa + 1, hook_args, NULL) &&
          !api->hook_function(0x10, hook_args, NULL) && !api->hook_function(RT_HOSTFN_BASE, hook_args, NULL),
          "a hook without a callback or on an address that starts no function was taken");
    CHECK(!(g_fnprof & RT_FN_HOOKS) && !plugins_fn_enter(c, fa), "hooks are on before any was registered");

    int h1 = api->hook_function(fa, hook_args, (void *)1);
    CHECK(h1 > 0 && (g_fnprof & RT_FN_HOOKS) && rt_fn_filter[RT_FN_SLOT(fa)] == 1, "a hook's handle, the switch and the filter");
    CHECK(RT_FN_SLOT(fa) == RT_FN_SLOT(fb) || !rt_fn_filter[RT_FN_SLOT(fb)], "the filter marks a function nobody hooked");
    CHECK(!plugins_fn_enter(c, fb), "a function without a hook was taken for a hooked one");
#define CALL(x0, x1) (c->x[0] = (x0), c->x[1] = (x1), c->x[19] = 19, c->x[29] = 0x4ff0, c->x[30] = 0x7000001234ull, c->sp = 0x5000, \
                      c->v[8] = 8, c->v[0] = 1, order = 0, plugins_hook_enter(c, fa, body))
    CHECK(CALL(5, 1) == 1 && c->x[0] == 12 && body_hits == 1 && order == 1, "a hook that changes an argument: 2*5 + (1+1)");
    CHECK(seen_sp == 0x5000 && seen_lr == 0x7000001234ull && seen_fp == 0x4ff0 && body_x19 == 19,
          "the call's stack pointer, return address and frame pointer");
    CHECK(c->x[19] == 19 && c->x[30] == 0x7000001234ull && c->sp == 0x5000 && c->v[8] == 8 && !c->fn_pass && !c->fn_hooks,
          "the registers a callee keeps after a hooked call");
    CHECK((uint64_t)c->v[0] == 7, "the function's vector result after a hooked call");

    int h2 = api->hook_function(fa, hook_orig, NULL);
    CHECK(h2 > h1 && rt_fn_filter[RT_FN_SLOT(fa)] == 2, "a second hook on the function");
    CHECK(CALL(5, 1) == 1 && c->x[0] == 112 && body_hits == 2 && order == 12, "two hooks in order, the second through call_original");
    int h3 = api->hook_function(fa, hook_skip, (void *)555);
    CHECK(CALL(5, 1) == 1 && c->x[0] == 655 && body_hits == 2 && order == 123 && (uint64_t)c->v[0] == 42,
          "a third hook that leaves the function out: the second gets its result");
    api->remove(h1), api->remove(h2);
    CHECK(CALL(5, 1) == 1 && c->x[0] == 555 && body_hits == 2 && order == 3 && c->x[19] == 19, "a skipped call");
    // the way the lifted code comes in (cpu.h FN_ENTER): the hook, or once past it for a hook's own call
    c->x[0] = 1, order = 0;
    CHECK(fnprof_hit(fa, c) == 1 && c->x[0] == 555 && order == 3, "the function-entry hook did not reach the hook");
    c->fn_pass = fa;
    CHECK(fnprof_hit(fa, c) == 0 && !c->fn_pass, "a hook's own call of its function was hooked again");
    api->remove(h3);
    CHECK(!(g_fnprof & RT_FN_HOOKS) && !rt_fn_filter[RT_FN_SLOT(fa)] && !plugins_fn_enter(c, fa), "hooks stay on after the last removal");
    int h4 = api->hook_function(fa, hook_twice, NULL);
    CHECK(CALL(5, 1) == 1 && c->x[0] == 23 && body_hits == 4, "call_original twice: 2*(2*5+1) + 1");
    api->remove(h4);
    struct { Nx2HookCall k; uint64_t after[8]; } stale = {.k = {.size = sizeof stale.k}};
    CHECK(api->call_original(&stale.k) == 0 && api->call_original(NULL) == 0 && body_hits == 4, "call_original outside a hook callback");
    int hs[NX2_HOOKS_MAX + 6], taken = 0;
    for (int i = 0; i < NX2_HOOKS_MAX + 6; i++) taken += (hs[i] = api->hook_function(fb, hook_args, NULL)) > 0;
    CHECK(taken == NX2_HOOKS_MAX, "the table takes NX2_HOOKS_MAX hooks");
    for (int i = 0; i < NX2_HOOKS_MAX + 6; i++) api->remove(hs[i]);
    CHECK(!(g_fnprof & RT_FN_HOOKS) && !rt_fn_filter[RT_FN_SLOT(fb)], "the filter after every hook is gone");
#undef CALL
    plat_aligned_free(c);
}

// ---- version 6: pictures and fonts. No window here, so nothing reaches the GPU: the handles, the limits, and the PNG
// reader on files made in memory.

// A PNG file of w x h pixels from px (ch bytes a pixel at 8 bits, or one index a pixel packed at `depth` bits when
// depth is under 8), row y stored with filter type filters[y % 5]; plte and trns are its palette and transparency
// chunks or NULL. The checksums are left 0: the reader does not look at them. malloc'd, *n bytes.
static void png_chunk(uint8_t *out, size_t *at, const char *type, const uint8_t *body, uint32_t len) {
    uint8_t *q = out + *at;
    q[0] = (uint8_t)(len >> 24), q[1] = (uint8_t)(len >> 16), q[2] = (uint8_t)(len >> 8), q[3] = (uint8_t)len;
    memcpy(q + 4, type, 4);
    if (len) memcpy(q + 8, body, len);
    memset(q + 8 + len, 0, 4);
    *at += 12 + len;
}

static uint8_t *png_make(uint32_t w, uint32_t h, int ctype, int depth, const uint8_t *px, const uint8_t *filters,
                         const uint8_t *plte, uint32_t plte_n, const uint8_t *trns, uint32_t trns_n, size_t *n) {
    uint32_t ch = ctype == 6 ? 4 : ctype == 2 ? 3 : ctype == 4 ? 2 : 1, bpp = depth < 8 ? 1 : ch;
    size_t rb = ((size_t)w * ch * (size_t)depth + 7) / 8;
    uint8_t *rows = calloc(h, rb), *raw = malloc(h * (rb + 1));
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++) {
            if (depth == 8) memcpy(rows + y * rb + x * ch, px + ((size_t)y * w + x) * ch, ch);
            else rows[y * rb + x * (uint32_t)depth / 8] |= (uint8_t)(px[(size_t)y * w + x] << (8 - depth - (int)(x * (uint32_t)depth % 8)));
        }
    for (uint32_t y = 0; y < h; y++) {
        const uint8_t *cur = rows + y * rb, *prev = y ? cur - rb : NULL;
        uint8_t ft = filters[y % 5], *o = raw + y * (rb + 1);
        *o++ = ft;
        for (size_t i = 0; i < rb; i++) {
            int a = i >= bpp ? cur[i - bpp] : 0, b = prev ? prev[i] : 0, c = prev && i >= bpp ? prev[i - bpp] : 0;
            int pa = abs(b - c), pb = abs(a - c), pc = abs(a + b - 2 * c);
            int pred = ft == 0 ? 0 : ft == 1 ? a : ft == 2 ? b : ft == 3 ? (a + b) / 2 : pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
            o[i] = (uint8_t)(cur[i] - pred);
        }
    }
    size_t zn = 0, at = 8;
    uint8_t *z = sz_deflate(raw, h * (rb + 1), &zn), *idat = malloc(zn + 6), *out = malloc(zn + 1200);
    idat[0] = 0x78, idat[1] = 0x01;
    memcpy(idat + 2, z, zn);
    memset(idat + 2 + zn, 0, 4);
    memcpy(out, "\x89PNG\r\n\x1a\n", 8);
    uint8_t ihdr[13] = {(uint8_t)(w >> 24), (uint8_t)(w >> 16), (uint8_t)(w >> 8), (uint8_t)w, (uint8_t)(h >> 24), (uint8_t)(h >> 16),
                        (uint8_t)(h >> 8), (uint8_t)h, (uint8_t)depth, (uint8_t)ctype, 0, 0, 0};
    png_chunk(out, &at, "IHDR", ihdr, 13);
    if (plte) png_chunk(out, &at, "PLTE", plte, plte_n);
    if (trns) png_chunk(out, &at, "tRNS", trns, trns_n);
    png_chunk(out, &at, "IDAT", idat, (uint32_t)(zn / 2 + 2));   // the data in two chunks, as a file may have it
    png_chunk(out, &at, "IDAT", idat + zn / 2 + 2, (uint32_t)(zn + 6 - (zn / 2 + 2)));
    png_chunk(out, &at, "IEND", NULL, 0);
    free(rows), free(raw), free(z), free(idat);
    *n = at;
    return out;
}

static void probe_pictures(const Nx2PluginApi *api) {
    CHECK(NX2_HAS(api, Nx2PluginApi, font_free) && api->texture_create && api->texture_load && api->texture_free &&
          api->font_load && api->font_free, "the table's version 6 members");
    // pictures: handles above 0, a limit, slots given back, a handle that went is nobody's
    uint8_t px[5 * 7 * 4];
    for (size_t i = 0; i < sizeof px; i++) px[i] = (uint8_t)(i * 37 + (i >> 3) * 11);
    CHECK(!api->texture_create(0, 4, px, 0) && !api->texture_create(4, 0, px, 0) && !api->texture_create(4, 4, NULL, 0) &&
          !api->texture_create(NX2_TEXTURE_SIDE_MAX + 1, 1, px, 0), "a picture without pixels or with a side out of range");
    static int hs[NX2_TEXTURES_MAX + 1];
    int made = 0, distinct = 1;
    for (int i = 0; i <= NX2_TEXTURES_MAX; i++) made += (hs[i] = api->texture_create(5, 7, px, i & 1 ? NX2_TEXTURE_PIXELS : 0)) > 0;
    for (int i = 1; i < NX2_TEXTURES_MAX; i++) distinct &= hs[i] != hs[i - 1];
    CHECK(made == NX2_TEXTURES_MAX && !hs[NX2_TEXTURES_MAX] && distinct, "the pictures' handles and their limit");
    api->texture_free(hs[3]), api->texture_free(hs[3]), api->texture_free(0), api->texture_free(-7);
    int again = api->texture_create(1, 1, px, 0);
    CHECK(again > 0 && again != hs[3], "a freed picture's slot is used again under a new handle");
    api->texture_free(again);
    for (int i = 0; i < NX2_TEXTURES_MAX; i++) api->texture_free(hs[i]);
    uint32_t wh[2] = {9, 9};
    CHECK(!api->texture_load("no-such-folder/no-such-picture.png", 0, wh) && !wh[0] && !wh[1] && !api->texture_load(NULL, 0, NULL) &&
          !api->texture_load("romfs:no-such-archive.bntx", 0, NULL), "a picture from a file that is not there");
    // the PNG reader: colour with alpha through each of the five row filters, then colour, grey with alpha, a
    // palette of four colours at two bits with one of them transparent, and files that are not pictures
    static const uint8_t filters[5] = {1, 2, 4, 3, 0}, plain[5] = {0, 0, 0, 0, 0};
    const char *why = "";
    uint32_t w = 0, h = 0;
    size_t n = 0;
    uint8_t *file = png_make(5, 7, 6, 8, px, filters, NULL, 0, NULL, 0, &n);
    uint8_t *got = plugin_png_decode(file, n, &w, &h, &why);
    CHECK(got && w == 5 && h == 7 && !memcmp(got, px, sizeof px), "a PNG with alpha, every row filter");
    free(got);
    CHECK(!plugin_png_decode(file, n - 20, &w, &h, &why) && !plugin_png_decode(file + 1, n - 1, &w, &h, &why) &&
          !plugin_png_decode(file, 20, &w, &h, &why), "a cut-off PNG and a file that is none");
    file[8 + 8 + 12] = 1;   // IHDR's interlace byte
    CHECK(!plugin_png_decode(file, n, &w, &h, &why) && strstr(why, "interlaced"), "an interlaced PNG is refused");
    free(file);
    file = png_make(5, 7, 2, 8, px, filters, NULL, 0, NULL, 0, &n);
    got = plugin_png_decode(file, n, &w, &h, &why);
    int ok = got && w == 5 && h == 7;
    for (int i = 0; ok && i < 35; i++) ok = !memcmp(got + i * 4, px + i * 3, 3) && got[i * 4 + 3] == 255;
    CHECK(ok, "a PNG without alpha");
    free(got), free(file);
    file = png_make(5, 7, 4, 8, px, filters, NULL, 0, NULL, 0, &n);
    got = plugin_png_decode(file, n, &w, &h, &why);
    ok = got != NULL;
    for (int i = 0; ok && i < 35; i++) ok = got[i * 4] == px[i * 2] && got[i * 4 + 2] == px[i * 2] && got[i * 4 + 3] == px[i * 2 + 1];
    CHECK(ok, "a grey PNG with alpha");
    free(got), free(file);
    static const uint8_t plte[12] = {255, 0, 0, 0, 255, 0, 0, 0, 255, 10, 20, 30}, trns[2] = {255, 0};
    uint8_t idx[35];
    for (int i = 0; i < 35; i++) idx[i] = (uint8_t)((i * 7 + i / 5) & 3);
    file = png_make(5, 7, 3, 2, idx, plain, plte, 12, trns, 2, &n);
    got = plugin_png_decode(file, n, &w, &h, &why);
    ok = got != NULL;
    for (int i = 0; ok && i < 35; i++) ok = !memcmp(got + i * 4, plte + idx[i] * 3, 3) && got[i * 4 + 3] == (idx[i] == 1 ? 0 : 255);
    CHECK(ok, "a PNG with a palette of two bits and a transparent colour");
    free(got), free(file);
    // fonts: a size out of range, a file that is not there, a file that is not a font
    char self[1024] = "";
    plat_exe_path(self, sizeof self);
    CHECK(!api->font_load("no-such-folder/no-such-font.ttf", 24, 0) && !api->font_load(NULL, 24, 0) &&
          !api->font_load("romfs:no-such-font.bfotf", 24, NX2_FONT_EXACT) && !api->font_load(self, 2, 0) &&
          !api->font_load(self, 500, NX2_FONT_EXACT),
          "a font from a file that is not there, or at a size out of range");
    api->font_free(0), api->font_free(12345);
}

// ---- Steam for plugins (plugin_steam.c). The checks that need no Steam run always. NX2NAVI_PLUGIN_STEAM_PROBE runs a
// live one beside them, for two of these probes on one PC with the stand-in (NX2NAVI_PLUGIN_STEAM_LOCAL=<port>) or
// one with Steam itself:
//   make:<seconds>          makes a room, prints its id, invites its first friend, answers every datagram
//   join:<room id>:<seconds>   joins it, sends datagrams, counts the answers
//   invited:<seconds>       waits for an invite and joins its room, then as join
//   look:<seconds>          signs in, makes a room and lists the friends: prints counts only, no id and no name
static struct { volatile int state, entered, failed, joined, left, got, invited, lost; uint64_t room, invite_room; int echo; } sp;

static void steam_probe_event(const Nx2SteamEvent *e, void *user) {
    const Nx2Steam *s = user;
    switch (e->kind) {
    case NX2_STEAM_EV_STATE: sp.state++; break;
    case NX2_STEAM_EV_ROOM_ENTERED: sp.room = e->a, sp.entered++; break;
    case NX2_STEAM_EV_ROOM_FAILED: sp.failed = e->code; break;
    case NX2_STEAM_EV_MEMBER_JOINED: sp.joined++; break;
    case NX2_STEAM_EV_MEMBER_LEFT: sp.left++; break;
    case NX2_STEAM_EV_INVITED: sp.invite_room = e->a, sp.invited++; break;
    case NX2_STEAM_EV_LOST: sp.lost++; break;
    case NX2_STEAM_EV_MESSAGE:
        sp.got++;
        if (sp.echo && e->n >= 1 && ((const uint8_t *)e->data)[0] == 'q') {
            uint8_t back[NX2_STEAM_MESSAGE_MAX];
            memcpy(back, e->data, e->n);
            back[0] = 'a';
            s->send(e->a, back, e->n, e->n & 1);
        }
        break;
    }
}

static int steam_probe_wait(volatile int *what, int ms) {
    for (int t = 0; t < ms && !*what; t += 10) plat_sleep_ms(10);
    return *what != 0;
}

static void probe_steam(const Nx2PluginApi *api) {
    CHECK(NX2_HAS(api, Nx2PluginApi, steam) && api->steam, "the table's version 7 member");
    const Nx2Steam *s = api->steam();
    CHECK(s && s == api->steam() && s->size == sizeof *s && s->start && s->stop && s->status && s->room_create && s->room_join &&
          s->room_leave && s->room_code && s->members && s->friends && s->invite && s->send, "the Steam table");
    Nx2SteamStatus st = {.size = sizeof st};
    CHECK(s->status(&st) && st.state == NX2_STEAM_OFF && !st.room && !st.self, "Steam's state before any start");
    Nx2SteamStatus small = {.size = 16, .local = 77};
    CHECK(s->status(&small) && small.size == 16 && small.local == 0 && !s->status(NULL), "a status block of an older size");
    CHECK(!s->start(NULL, steam_probe_event, NULL) && !s->start("", steam_probe_event, NULL) && !s->start("Upper", steam_probe_event, NULL) &&
          !s->start("a tag that is far too long for a room's note", steam_probe_event, NULL) && !s->start("probe", NULL, NULL),
          "a start with a tag out of the rule, or without a callback");
    CHECK(s->status(&st) && st.state == NX2_STEAM_OFF && st.why == NX2_STEAM_WHY_OTHER && st.says[0], "a refused start says why");
    // nothing works before start, and nothing faults
    Nx2SteamMember members[NX2_STEAM_ROOM_MAX];
    Nx2SteamFriend friends[4];
    CHECK(!s->room_create(4) && !s->room_join("2222-2222") && !s->invite(1) && !s->send(0, "x", 1, 1) && !s->members(members, 8) &&
          !s->friends(friends, 4), "rooms, invites and datagrams before start");
    s->room_leave(), s->stop(), s->stop();
    // room ids: the game's own co-op's format; a lobby's number has one, another number none
    char code[NX2_STEAM_CODE_CAP] = "x";
    uint64_t lobby = 0x01860000ull << 32 | 0x12345678u;
    CHECK(s->room_code(lobby, code) && strlen(code) == 9 && code[4] == '-' && steam_code_parse(code) == lobby, "a room's id");
    CHECK(!s->room_code(0x0110000100000001ull, code) && !code[0] && !s->room_code(lobby, NULL), "a number that is no room's has no id");

    char mode[96];
    if (!plat_getenv("NX2NAVI_PLUGIN_STEAM_PROBE", mode, sizeof mode) || !mode[0]) return;
    char *arg = strchr(mode, ':'), *arg2 = NULL;
    if (arg) *arg++ = 0, arg2 = strchr(arg, ':');
    if (arg2) *arg2++ = 0;
    int make = !strcmp(mode, "make"), look = !strcmp(mode, "look"), invited = !strcmp(mode, "invited"), join = !strcmp(mode, "join");
    int seconds = atoi(join ? (arg2 ? arg2 : "10") : (arg ? arg : "10"));
    memset(&sp, 0, sizeof sp);
    sp.echo = make;
    if (!s->start("nx2navi.probe", steam_probe_event, (void *)s)) {
        // refused at once (the game's own co-op is on): the status says why, nothing was started
        s->status(&st);
        printf("steam probe: the start was refused, why %u: %s\n", st.why, st.says);
        CHECK(st.state == NX2_STEAM_OFF && st.why && st.says[0] && !s->room_create(4), "a refused start says why");
        return;
    }
    CHECK(!s->start("nx2navi.other", steam_probe_event, NULL), "a second start while the first has Steam");
    steam_probe_wait(&sp.state, 15000);
    s->status(&st);
    printf("steam probe: state %u, why %u, stand-in %u%s%s\n", st.state, st.why, st.local, st.says[0] ? ": " : "", st.says);
    if (st.state != NX2_STEAM_READY) {
        CHECK(st.state == NX2_STEAM_UNAVAILABLE && st.why && st.says[0], "an unavailable Steam says why");
        CHECK(!s->room_create(4), "a room without Steam");
        s->stop();
        return;
    }
    CHECK(st.self && st.name[0] && !st.room && !st.entering, "signed in: an id and a name, no room");
    if (make || look) {
        CHECK(s->room_create(4) && !s->room_create(4), "a room is asked for once");
        CHECK(steam_probe_wait(&sp.entered, 15000), "the room was made");
        s->status(&st);
        CHECK(st.room == sp.room && st.owner == st.self && strlen(st.room_code) == 9 && st.members == 1, "the room's status");
        CHECK(s->members(members, 8) == 1 && members[0].self && members[0].owner && members[0].name[0], "the room's one member");
        if (st.local) printf("steam probe: room id %s\n", st.room_code);   // a stand-in's id is a port, nobody's secret
        else printf("steam probe: room made, its id has %u characters\n", (unsigned)strlen(st.room_code));
        fflush(stdout);
    } else {
        if (invited) {
            CHECK(steam_probe_wait(&sp.invited, seconds * 1000), "an invite came");
            CHECK(s->room_code(sp.invite_room, code), "the invite's room has an id");
        } else {
            snprintf(code, sizeof code, "%s", arg ? arg : "");
        }
        CHECK(!s->room_join("not a room id") && s->room_join(code), "a room joined by its id");
        CHECK(steam_probe_wait(&sp.entered, 15000) || sp.failed, "the join answered");
        if (!sp.entered) {
            printf("steam probe: not entered, code %d\n", sp.failed);
            s->stop();
            return;
        }
        s->status(&st);
        CHECK(st.room && st.owner && st.owner != st.self && st.members == 2, "the joined room's status");
        CHECK(s->members(members, 8) == 2 && members[0].owner && !members[0].self && members[1].self && members[0].name[0],
              "the joined room's members, the owner first");
    }
    static Nx2SteamFriend all[512];
    int nf = 0, sent = 0, invited_one = 0;
    for (int t = 0; t < seconds * 10; t++) {
        plat_sleep_ms(100);
        nf = s->friends(all, 512);
        if (make && nf && !invited_one && sp.joined == 0 && t == 20) invited_one = s->invite(all[0].id);
        if ((join || invited) && t < 50) {
            uint8_t q[NX2_STEAM_MESSAGE_MAX];
            memset(q, t, sizeof q);
            q[0] = 'q';
            // every size class, reliable and not; to the owner by id, and to "everyone else"
            size_t n = t % 5 == 0 ? NX2_STEAM_MESSAGE_MAX : (size_t)(1 + t * 7);
            sent += s->send(t & 2 ? 0 : st.owner, q, n, t & 1);
        }
        if (sp.lost) break;
    }
    s->status(&st);
    printf("steam probe: %d friends, %u in the room, joined %d, left %d, datagrams sent %d, got %d, invites got %d, lost %d\n", nf,
           st.members, sp.joined, sp.left, sent, sp.got, sp.invited, sp.lost);
    if (join || invited) CHECK(sent == 50 && sp.got == 50, "every datagram was answered");
    CHECK(!s->send(0, "x", 0, 1) && !s->send(0, NULL, 4, 1) && !s->send(0, all, NX2_STEAM_MESSAGE_MAX + 1, 1), "a datagram out of range");
    s->room_leave();
    for (int t = 0; t < 100; t++) {
        plat_sleep_ms(10);
        s->status(&st);
        if (!st.room) break;
    }
    CHECK(!st.room && !st.room_code[0] && !s->members(members, 8) && !s->send(0, "x", 1, 1), "out of the room after room_leave");
    s->stop();
    s->status(&st);
    CHECK(st.state == NX2_STEAM_OFF && !st.self, "off after stop");
}

int probe_plugins(void) {
    CHECK(!nx2navi_plugin_api(0) && !nx2navi_plugin_api(NX2_PLUGIN_API_VERSION + 1), "a version the table does not serve got one");
    api = nx2navi_plugin_api(NX2_PLUGIN_API_VERSION);
    CHECK(api && api->size == sizeof *api && api->version == NX2_PLUGIN_API_VERSION, "the table's size and version");
    if (!api) return 1;
    CHECK(NX2_HAS(api, Nx2PluginApi, capture_input) && !NX2_HAS(&(Nx2Frame){.size = 8}, Nx2Frame, index), "NX2_HAS");

    const Nx2GameInfo *g = api->game_info();
    CHECK(g && g == api->game_info() && g->size == sizeof *g, "the game's description is one structure");
    CHECK(g->game_dir && *g->game_dir && g->mods_dir && g->build_id && g->runtime_version && *g->runtime_version,
          "the game's description lacks a text");
    CHECK(g->main_base == 0x7000000000ull && g->main_size, "main's place");
    CHECK(g->game_version == (uint32_t)rt_game_version, "the game's version");
    CHECK(NX2_HAS(g, Nx2GameInfo, save_dir) && g->profile_dir && g->save_dir && !*g->save_dir,
          "the profile folder, and no save folder before the game's storage is set up");
    CHECK(NX2_HAS(g, Nx2GameInfo, account) && g->language && *g->language && g->account, "the game's language and the account's name");
    CHECK(NX2_HAS(api, Nx2PluginApi, set_save_dir) && api->camera_reset && api->set_save_dir, "the version 5 members");
    CHECK(nx2navi_plugin_api(1) == api, "a version 1 plugin gets another table");
    api->log("probe: %s in %s, main at %#llx", g->runtime_version, g->game_dir, (unsigned long long)g->main_base);

    // memory: a host buffer reads and is no place to write; the guest heap reads and writes; unmapped memory does neither
    char src[16] = "plugin probe", dst[16] = "";
    CHECK(api->read((uint64_t)(uintptr_t)src, dst, sizeof src) && !strcmp(dst, src), "read of host memory");
    CHECK(!api->write((uint64_t)(uintptr_t)dst, "x", 1) && !strcmp(dst, src), "write outside guest memory was taken");
    CHECK(!api->read(0x10, dst, 4) && !api->write(0x10, src, 4), "the null page");
    CHECK(!api->read(RT_HOSTFN_BASE, dst, 4) && !api->write(RT_HOSTFN_BASE, src, 4), "the host-function window");
    CHECK(!api->read(~0ull - 2, dst, 8) && !api->write(~0ull - 2, src, 8), "a range that wraps");
    CHECK(api->read(0x10, dst, 0) && api->write(0x10, src, 0), "an empty range");
    uint64_t heap = 0;
    CHECK(mem_set_heap_size(0x200000, &heap) == 0 && heap, "SetHeapSize");
    if (heap) {
        uint64_t v = 0x1122334455667788ull, got = 0;
        CHECK(api->write(heap + 0x1000, &v, sizeof v) && api->read(heap + 0x1000, &got, sizeof got) && got == v,
              "write and read of the guest heap");
        got = 0;
        CHECK(!api->write(heap + 0x200000 - 4, &v, sizeof v), "a write across the heap's end was taken");
        CHECK(api->read(heap + 0x200000 - 4, &got, 4) && got == 0, "the heap's last bytes changed");
        CHECK(!api->read(heap + 0x200000 - 4, &got, sizeof got), "a read across the heap's end");
        // map_range: the heap is the game's to read and write directly; nothing else here is
        uint64_t *direct = api->map_range(heap + 0x1000, sizeof v, 1);
        CHECK(direct == (uint64_t *)(uintptr_t)(heap + 0x1000) && *direct == v && api->map_range(heap, 0x200000, 0),
              "map_range of the guest heap");
        if (direct) *direct = ~v;
        CHECK(api->read(heap + 0x1000, &got, sizeof got) && got == ~v, "a direct write is not what read gives");
        CHECK(!api->map_range(heap + 0x200000 - 4, 8, 0) && !api->map_range(heap, 0, 0), "map_range across the heap's end");
        CHECK(!api->map_range(0x10, 4, 0) && !api->map_range(RT_HOSTFN_BASE, 4, 0) &&
              !api->map_range((uint64_t)(uintptr_t)src, 4, 0) && !api->map_range(~0ull - 2, 8, 0),
              "map_range outside the guest's memory");
        mem_set_heap_size(0, &heap);
        CHECK(!api->map_range(heap + 0x1000, 8, 0) && !api->read(heap + 0x1000, &got, 8), "the heap after it was given back");
    }

    // frame callbacks: called in order with their user pointer; one may remove another; the registers stay
    Cpu *c = plat_aligned_alloc(sizeof(Cpu), 64);
    memset(c, 0, sizeof *c);
    c->x[3] = 77;
    plugins_frame(c);
    CHECK(!frame_hits, "a frame callback ran before any was registered");
    int h1 = api->on_frame(on_frame, &frame_hits), h2 = api->on_frame(on_frame, &frame_hits);
    CHECK(h1 > 0 && h2 > 0 && h1 != h2 && !api->on_frame(NULL, NULL), "frame callback handles");
    plugins_frame(c);
    CHECK(frame_hits == 2 && frame_index == 1 && c->x[3] == 77, "two frame callbacks at the second frame");
    CHECK(call_result == 0, "a guest call outside a guest thread was not refused");
    Nx2GuestRegs regs = {.size = sizeof regs};
    CHECK(!api->guest_call_regs(g->main_base, &regs) && !api->guest_call(g->main_base, NULL, 9),
          "guest calls outside a frame callback");
    int h3 = api->on_frame(on_frame_removes, &h2);
    plugins_frame(c);   // this pass took its copy first: both ran, then h2 went
    plugins_frame(c);
    CHECK(frame_hits == 5, "a callback removed by another one kept running");
    api->remove(h1), api->remove(h3), api->remove(h3), api->remove(0), api->remove(-5);
    plugins_frame(c);
    CHECK(frame_hits == 5, "removed frame callbacks ran");
    int hs[40], taken = 0;
    for (int i = 0; i < 40; i++) taken += (hs[i] = api->on_frame(on_frame, NULL)) > 0;
    CHECK(taken == 32, "the table takes 32 callbacks of a kind");
    for (int i = 0; i < 40; i++) api->remove(hs[i]);

    // overlay
    Nx2Draw d = {.size = sizeof d, .width = 640, .height = 360};
    CHECK(!plugins_overlay_wanted(), "an overlay is wanted with no callback");
    int ho = api->on_overlay(on_overlay, &overlay_hits);
    CHECK(ho > 0 && plugins_overlay_wanted(), "the overlay callback's handle");
    plugins_overlay(&d);
    CHECK(overlay_hits == 1 && d.frame == 4, "the overlay callback and its frame");
    api->remove(ho);
    CHECK(!plugins_overlay_wanted(), "an overlay is wanted after the removal");

    // keys reach every callback and are kept when one says so; typed characters arrive only while the input is taken
    int hk = api->on_key(on_key, NULL);
    CHECK(!plugins_key(NX2_KEY_ENTER, 1, 0) && key_hits == 1 && last_key == (NX2_KEY_ENTER | 1u << 16), "a key event");
    int hk2 = api->on_key(on_key, &key_hits);
    CHECK(plugins_key('T', 0, 0) && key_hits == 3 && last_key == 'T', "a kept key");
    api->remove(hk), api->remove(hk2);
    int ht = api->on_text(on_text, NULL);
    plugins_text('a');
    CHECK(!text_hits && !plugins_capturing(), "a character arrived without the input taken");
    api->capture_input(1), api->capture_input(1), api->capture_input(0);
    CHECK(plugins_capturing(), "the input capture is counted");
    plugins_text(0x1F600);
    CHECK(text_hits == 1 && last_text == 0x1F600, "a typed character");
    api->capture_input(0), api->capture_input(0);
    CHECK(!plugins_capturing(), "the input was not given back");
    api->capture_input(1);
    CHECK(plugins_capturing(), "a capture after one give-back too many");
    api->capture_input(0);
    api->remove(ht);

    probe_pictures(api);
    probe_steam(api);
    probe_hooks();   // before a plugin of NX2NAVI_MOD_DLLS registers hooks of its own
    probe_title();

    // The libraries NX2NAVI_MOD_DLLS names, as at the game's start: none in a plain run; tests/plugins_check.py names the
    // example plugin and a missing file, and reads the lines of both. A loaded plugin's frame callback runs once.
    plugins_start();
    plugins_frame(c);
    plat_aligned_free(c);

    // the exit callbacks run once, whoever ends the process
    int he = api->on_exit(on_exit_cb, &exit_hits);
    CHECK(he > 0 && !exit_hits, "the exit callback's handle");
    plugins_exit(), plugins_exit();
    CHECK(exit_hits == 1, "the exit callbacks run once");
    rt_log_sync();
    printf("probe-plugins: %s\n", fails ? "FAILED" : "ok");
    return fails != 0;
}
