#!/usr/bin/env python3
"""Exercise the built-in cheat against the runtime's actual C functions."""
import os
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
CC = os.environ.get("CC", "clang")


def function(source, name):
    match = re.search(r"\b(?:int|void)\s+" + name + r"\s*\([^)]*\)\s*\{", source)
    if not match:
        return None
    depth = 1
    i = match.end()
    while depth:
        if source[i] == "{":
            depth += 1
        elif source[i] == "}":
            depth -= 1
        i += 1
    return source[match.start():i]


def run_api_test(tmp):
    source = (ROOT / "runtime/cheats.c").read_text(encoding="utf-8")
    names = ("cheats_rupees_supported", "cheats_rupees_on", "cheats_rupees_set")
    funcs = [function(source, n) for n in names]
    # Missing functions are represented by failing sentinels: the RED signal is
    # a runtime behavior assertion rather than a compiler/linker failure.
    fallbacks = ("int cheats_rupees_supported(void) { return 0; }",
                 "int cheats_rupees_on(void) { return 0; }",
                 "void cheats_rupees_set(int on) { (void)on; }")
    body = "\n".join(f or fallback for f, fallback in zip(funcs, fallbacks))
    harness = r'''
#include <assert.h>
#include <string.h>
#include "cheats.h"
#include "cfg.h"
#include "host.h"
static int available = 1, state = 0, other = 1, switches, writes;
int rt_patch_set(const char *name) { assert(!strcmp(name, "rupees_refill")); return available ? 3 : -1; }
int rt_patch_enabled(int set) { assert(set == 3); return state; }
void rt_patch_switch(int set, int on) { assert(set == 3); assert(on == 0 || on == 1); switches++; state = on; }
void cfg_set(const char *key, const char *value) {
    assert(!strcmp(key, "NX2NAVI_CHEAT_RUPEES_REFILL"));
    assert(!strcmp(value, state ? "1" : "0")); writes++;
}
''' + body + r'''
int main(void) {
    assert(cheats_rupees_supported() == 1);
    assert(cheats_rupees_on() == 0);
    state = 1; assert(cheats_rupees_on() == 1);
    cheats_rupees_set(0); assert(!state && switches == 1 && writes == 1);
    cheats_rupees_set(-23); assert(state && switches == 2 && writes == 2);
    cheats_rupees_set(42); assert(state && switches == 3 && writes == 3);
    available = 0;
    assert(!cheats_rupees_supported() && !cheats_rupees_on());
    cheats_rupees_set(0); cheats_rupees_set(1);
    assert(state && other == 1 && switches == 3 && writes == 3);
    return 0;
}
'''
    c = tmp / "api.c"
    c.write_text(harness, encoding="utf-8")
    exe = tmp / "api.exe"
    subprocess.run([CC, "-std=gnu11", "-I", str(ROOT / "runtime"), str(c), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
    print("API behavior: PASS")


def run_ui_test(tmp):
    harness = r'''
#include <cassert>
#include <cstring>
#include "ui/option_rows.h"
extern "C" {
#include "cheats.h"
#include "ui_text.h"
bool tab_cheats(float);
}
static int support, state, count, clicked, disabled, builtin_rows, imported_rows, saved, sets, groups, rebuild_click;
static int external_click, external_sets, notices;
static CheatInfo external = {"External", "Test mod", "", CHEAT_CODE, 0, "NX2NAVI_CHEAT_12345678"};
extern "C" int cheats_rupees_supported(void) { return support; }
extern "C" int cheats_rupees_on(void) { return support && state; }
extern "C" void cheats_rupees_set(int on) { assert(support); state = on; sets++; }
extern "C" int cheats_count(void) { return count; }
extern "C" const CheatInfo *cheats_info(int i) { assert(i == 0); return &external; }
extern "C" int cheats_on(int) { return 0; }
extern "C" void cheats_set(int i, int on) { assert(i == 0 && on == 1); external_sets++; }
extern "C" const char *cheats_file(void) { return "id.txt"; }
extern "C" int cheats_rebuild_pending(void) { return 1; }
extern "C" const char *cheats_buttons(uint32_t, char *, int) { return "A"; }
extern "C" const char *menu_text(UiTextId id) {
    if (id == UI_MENU_CHEATS_NONE) return "%s";
    if (id == UI_MENU_CHEATS_MOD) return "%s %d";
    if (id == UI_MENU_CHEAT_RUPEES) return "Refill rupees";
    if (id == UI_MENU_CHEAT_RUPEES_DESC) return "rupee description";
    return "text";
}
void opt_begin(const char *, float) {}
void opt_end(void) {}
void opt_disable_next(UiTextId note) { assert(note == UI_MENU_CHEAT_RUPEES_UNAVAILABLE); disabled = 1; }
bool opt_button(const char *, UiTextId, UiTextId, const char *) { return rebuild_click; }
void opt_group_text(const char *) { groups++; }
void opt_text_str(const char *, bool) { notices++; }
void opt_save_soon(void) { saved++; }
bool opt_toggle_text(const char *id, const char *label, const char *desc, bool on, const char *) {
    if (!strcmp(id, "NX2NAVI_CHEAT_RUPEES_REFILL")) {
        assert(!strcmp(label, "Refill rupees") && !strcmp(desc, "rupee description"));
        builtin_rows++; assert(on == (support && state));
        assert(disabled == !support);
        bool action = clicked && !disabled;
        disabled = 0; return action;
    }
    assert(!strcmp(id, external.key)); imported_rows++; disabled = 0; return external_click;
}
int main() {
    support = 1; clicked = 1; count = 0;
    assert(!tab_cheats(1));
    assert(builtin_rows == 1 && sets == 1 && state == 1 && saved == 1 && imported_rows == 0);
    assert(notices == 0);
    clicked = 0; count = 1; rebuild_click = 1; external_click = 1;
    assert(tab_cheats(1));
    assert(builtin_rows == 2 && imported_rows == 1 && groups == 1 && external_sets == 1 && saved == 2);
    support = 0; clicked = 1; count = 0;
    assert(!tab_cheats(1));
    assert(builtin_rows == 3 && sets == 1 && state == 1 && saved == 2);
    assert(notices == 1);
}
'''
    c = tmp / "ui.cpp"
    c.write_text(harness, encoding="utf-8")
    exe = tmp / "ui.exe"
    cxx = os.environ.get("CXX", "clang++")
    subprocess.run([cxx, "-std=gnu++17", "-I", str(ROOT / "runtime"),
                    "-I", str(ROOT / "runtime/third_party/imgui"), str(c),
                    str(ROOT / "runtime/ui/cheats_tab.cpp"), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
    print("Cheats tab behavior: PASS")


def run_cfg_test(tmp):
    harness = r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cfg.h"
#include "host.h"
#include "platform/platform.h"
#include "dispatch.h"
#include "debug_tools.h"
static int registrations;
void dbg_freecam_host(Cpu *c) { (void)c; assert(0); }
void rt_register_host(uint64_t addr, GuestFn fn) {
    assert(addr == 0x10000 + 0xf70 && fn == dbg_freecam_host); registrations++;
}
void perf_size(PerfSizes *s, const char *name, uint64_t count, uint64_t limit, uint64_t bytes) {
    (void)s; (void)name; (void)count; (void)limit; (void)bytes;
}
void rt_log(const char *fmt, ...) { (void)fmt; }
void rt_fatal(const char *fmt, ...) { (void)fmt; abort(); }
void plat_mutex_lock(PlatMutex *m) { (void)m; }
void plat_mutex_unlock(PlatMutex *m) { (void)m; }
int plat_setenv(const char *name, const char *value) { (void)name; (void)value; return 0; }
int plat_exe_path(char *out, size_t cap) { (void)out; (void)cap; return 0; }
int plat_mem_commit(uint64_t addr, size_t n) { (void)addr; (void)n; return 1; }
uint8_t *plat_file_read(const char *path, uint64_t *size) {
    FILE *f = fopen(path, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); rewind(f);
    uint8_t *p = malloc((size_t)n + 1); assert(p);
    assert(fread(p, 1, (size_t)n, f) == (size_t)n); fclose(f);
    p[n] = 0; *size = (uint64_t)n; return p;
}
int plat_file_exists(const char *path) { FILE *f = fopen(path, "rb"); if (!f) return 0; fclose(f); return 1; }
int64_t plat_file_size(const char *path) { FILE *f = fopen(path, "rb"); if (!f) return -1; fseek(f, 0, SEEK_END); long n = ftell(f); fclose(f); return n; }
int plat_file_write_atomic(const char *path, const void *data, size_t n) {
    FILE *f = fopen(path, "wb"); if (!f) return 0;
    int ok = fwrite(data, 1, n, f) == n; return fclose(f) == 0 && ok;
}
static int found_env;
static void active(const char *key, const char *value, int from_env, void *ctx) {
    (void)ctx;
    if (!strcmp(key, "CHEAT_RUPEES_REFILL") && from_env && !strcmp(value, "1")) found_env++;
}
int main(int argc, char **argv) {
    assert(argc == 3); const char *key = "NX2NAVI_CHEAT_RUPEES_REFILL";
    assert(registrations == 1);
    int stage = atoi(argv[2]);
    const CfgMeta *m = cfg_meta(key);
    assert(m && !strcmp(m->def, "0") && !strcmp(m->values, "0|1") && m->live);
    if (stage) assert(cfg_load(argv[1]));
    assert(cfg_bool(key) == (stage == 1 || stage == 3));
    if (stage == 3) { cfg_each_active(active, NULL); assert(found_env == 1); }
    rt_patches_init();
    int set = rt_patch_set("rupees_refill"), other = rt_patch_set("other");
    assert(set >= 0 && other >= 0 && rt_patch_enabled(set) == (stage == 1 || stage == 3));
    assert(rt_patch_enabled(other));
    if (stage < 2) {
        cfg_set(key, stage == 0 ? "1" : "0");
        assert(cfg_save_to(argv[1]));
    }
}
'''
    c, exe, ini = tmp / "cfg_test.c", tmp / "cfg_test.exe", tmp / "settings.ini"
    c.write_text(harness, encoding="utf-8")
    (tmp / "patches.h").write_text(
        '#define RT_PATCH_COUNT 2\n#define RT_PATCH_PAGE 0x10000ULL\n'
        '#define RT_PATCH_SETS(X) X(0, rupees_refill, "NX2NAVI_CHEAT_RUPEES_REFILL", 0) '
        'X(1, other, "NX2NAVI_TEST_OTHER", 1)\n', encoding="utf-8")
    subprocess.run([CC, "-std=gnu11", "-ffunction-sections", "-fdata-sections",
                    "-I", str(tmp), "-I", str(ROOT / "runtime"), str(c), str(ROOT / "runtime/cfg.c"),
                    str(ROOT / "runtime/patches.c"), "-Wl,--gc-sections", "-o", str(exe)], check=True)
    env = os.environ.copy()
    env.pop("NX2NAVI_CHEAT_RUPEES_REFILL", None)
    env.pop("NX2NAVI_TEST_OTHER", None)
    for stage in range(4):
        if stage == 3:
            env["NX2NAVI_CHEAT_RUPEES_REFILL"] = "1"
        subprocess.run([str(exe), str(ini), str(stage)], check=True, env=env)
    assert "[cheats]" in ini.read_text() and "cheat_rupees_refill = 0" in ini.read_text()
    print("Configuration and real patch startup across fresh processes: PASS")


def run_text_test(tmp):
    harness = r'''
#include <assert.h>
#include <string.h>
#include "ui_text.h"
const char *ov_language(void) { return "en"; }
const char *plat_ui_language(void) { return "en"; }
const char *cfg_value(const char *key) { (void)key; return ""; }
unsigned cfg_version(void) { return 0; }
int main(void) {
    const char *langs[] = {"en", "ru", "pt-BR", "zh-Hans"};
    UiTextId ids[] = {UI_MENU_CHEAT_RUPEES, UI_MENU_CHEAT_RUPEES_DESC, UI_MENU_CHEAT_RUPEES_UNAVAILABLE};
    for (unsigned l = 0; l < 4; l++)
        for (unsigned i = 0; i < 3; i++) {
            assert(ui_text_defined(langs[l], ids[i]));
            assert(*ui_text_in(langs[l], ids[i]));
            if (l) assert(strcmp(ui_text_in(langs[l], ids[i]), ui_text_in("en", ids[i])));
        }
    assert(!strcmp(ui_text_in("en", UI_MENU_CHEAT_RUPEES), "Refill rupees"));
}
'''
    c, exe = tmp / "text.c", tmp / "text.exe"
    c.write_text(harness, encoding="utf-8")
    subprocess.run([CC, "-std=gnu11", "-I", str(ROOT / "runtime"), str(c),
                    str(ROOT / "runtime/ui_text.c"), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
    print("Four localized catalogs: PASS")


def main():
    with tempfile.TemporaryDirectory(prefix="builtin-cheats-") as d:
        tmp = Path(d)
        run_api_test(tmp)
        run_ui_test(tmp)
        run_cfg_test(tmp)
        run_text_test(tmp)


if __name__ == "__main__":
    main()
