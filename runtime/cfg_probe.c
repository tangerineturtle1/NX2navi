// --probe-cfg: nx2navi.ini parsing, the environment override, a missing file and the settings waiting for a restart
// (cfg.c). Needs no game files.
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "host.h"
#include "input.h"
#include "input_map.h"
#include "platform/platform.h"
#include "vk_dlssg.h"

static int fails;
#define CHECK(c, what) do { if (!(c)) { fails++; fprintf(stderr, "FAIL cfg: %s\n", what); } } while (0)
#define EQ(a, b) ((a) && !strcmp((a), (b)))

int probe_cfg(void) {
    // cfg_pending, before this probe's files replace the settings cfg_init read: a restart-only setting counts while its
    // value differs from the one at start, a live one never
    if (!cfg_from_env("NX2NAVI_MICROPHONE") && !cfg_from_env("NX2NAVI_VOLUME")) {
        const char *mic = cfg_value("NX2NAVI_MICROPHONE");
        int base = cfg_pending_count();
        CHECK(!cfg_pending("NX2NAVI_MICROPHONE"), "pending: unchanged since the start");
        cfg_set("NX2NAVI_MICROPHONE", strcmp(mic, "0") ? "0" : "1");
        CHECK(cfg_pending("NX2NAVI_MICROPHONE") && cfg_pending_count() == base + 1, "pending: a restart-only setting changed");
        cfg_set("NX2NAVI_MICROPHONE", mic);
        CHECK(!cfg_pending("NX2NAVI_MICROPHONE") && cfg_pending_count() == base, "pending: set back to its start value");
        cfg_set("NX2NAVI_VOLUME", "7");
        CHECK(!cfg_pending("NX2NAVI_VOLUME") && cfg_pending_count() == base, "pending: a live setting never counts");
        // a value the environment sets after the start (the command line's --monitor arrives that way) never waits:
        // no row can change it and a restart keeps it
        if (!cfg_from_env("NX2NAVI_MONITOR")) {
            cfg_setenv("NX2NAVI_MONITOR", "2");
            CHECK(!cfg_pending("NX2NAVI_MONITOR") && cfg_pending_count() == base, "pending: set by the environment after the start");
            cfg_setenv("NX2NAVI_MONITOR", NULL);
        }
    }
    char path[1024];
    if (!plat_exe_path(path, sizeof path)) return 1;
    char *cut = plat_last_sep(path);
    if (!cut) return 1;
    strcpy(cut + 1, "cfg-probe.ini");
    static const char ini[] =
        "\xEF\xBB\xBF# comment\r\n"
        "; another comment\r\n"
        "[frame rate]\r\n"
        "  fps60 = 0  \r\n"
        "Realtime=0\r\n"
        "[Display]\r\n"
        "GPU_name = \"My GPU 1\"\r\n"
        "NX2NAVI_Locale = de-DE\r\n"
        "lang =\r\n"
        "trace = a\r\n"
        "trace = b\r\n"
        "no_such_setting = 1\r\n"
        "no equals sign\r\n"
        "last = no newline at the end";
    cfg_setenv("NX2NAVI_LOCALE", NULL);
    cfg_setenv("NX2NAVI_FPS60", NULL);
    CHECK(plat_file_write(path, ini, sizeof ini - 1), "write the probe ini");
    CHECK(cfg_load(path) == 1, "load the probe ini");
    CHECK(EQ(cfg_get("NX2NAVI_FPS60"), "0"), "value read, whitespace trimmed, lower-case key");
    CHECK(EQ(cfg_get("NX2NAVI_REALTIME"), "0"), "key without spaces, mixed case");
    CHECK(EQ(cfg_get("NX2NAVI_GPU_NAME"), "My GPU 1"), "quoted value, section ignored");
    CHECK(EQ(cfg_get("NX2NAVI_LOCALE"), "de-DE"), "NX2NAVI_ prefix accepted in the file");
    CHECK(cfg_get("NX2NAVI_LANG") == NULL, "empty value is unset");
    CHECK(EQ(cfg_get("NX2NAVI_TRACE"), "b"), "later line wins");
    CHECK(cfg_get("NX2NAVI_CLOTH_REALTIME") == NULL, "absent key is NULL");
    CHECK(cfg_get("NX2NAVI_comment") == NULL, "comment is not a key");
    CHECK(EQ(cfg_get("NX2NAVI_NO_SUCH_SETTING"), "1"), "unknown key is still readable");
    {
        int n = -1;
        const char *u = cfg_unknown_keys(&n);
        CHECK(n == 2 && EQ(u, "no_such_setting, last"), "the keys that are no settings: listed lower-case, in the file's order");
    }
    cfg_setenv("NX2NAVI_FPS60", "1");
    CHECK(EQ(cfg_get("NX2NAVI_FPS60"), "1"), "environment overrides the file");
    cfg_setenv("NX2NAVI_FPS60", NULL);
    CHECK(EQ(cfg_get("NX2NAVI_FPS60"), "0"), "file value returns when the variable is removed");
    plat_file_delete(path);
    CHECK(cfg_load(path) == 0, "missing file is not an error");
    CHECK(cfg_get("NX2NAVI_FPS60") == NULL, "missing file leaves no keys");
    {
        int n = -1;
        CHECK(EQ(cfg_unknown_keys(&n), "") && n == 0, "a missing file leaves no unknown keys either");
    }

    // A file from the co-op of versions up to 0.9.2: its lines are retired settings, not unknown keys, and its switch,
    // coop = 1, does not turn the new co-op on, whose switch has a name of its own (coop_steam).
    {
        static const char old_coop[] = "[coop]\ncoop = 1\ncoop_beta = 1\ncoop_name = someone\ncoop_progress = shared\n";
        cfg_setenv("NX2NAVI_COOP_STEAM", NULL);
        CHECK(plat_file_write(path, old_coop, sizeof old_coop - 1) && cfg_load(path) == 1, "load a file of the old co-op");
        int n = -1;
        CHECK(EQ(cfg_unknown_keys(&n), "") && n == 0, "the old co-op's settings are no unknown keys");
        CHECK(!cfg_bool("NX2NAVI_COOP_STEAM"), "the old co-op's coop = 1 does not turn the new co-op on");
        static const char new_coop[] = "[coop]\ncoop = 0\ncoop_steam = 1\n";
        CHECK(plat_file_write(path, new_coop, sizeof new_coop - 1) && cfg_load(path) == 1 && cfg_bool("NX2NAVI_COOP_STEAM"),
              "coop_steam = 1 turns the new co-op on");
        plat_file_delete(path);
        CHECK(cfg_load(path) == 0, "the co-op file is gone again");
    }

    // A player's file as a report showed it: settings the menu wrote, and three lines added by hand that are no
    // settings of this version (the player took the first for the fix of a crash). The three are named as such, a
    // repeated one once, a key of other characters is counted without being listed, and the menu's save keeps every
    // line it does not change byte for byte, the three included: only the changed, the reset and the new key move.
#define PLAYER_HEAD \
        "[display]\n" \
        "async_compute = 1\n" \
        "fullscreen = 1\n" \
        "window_size = 1920x1080\n" \
        "gpu_index = 0\n" \
        "perf_overlay = fps\n" \
        "vk_present = mailbox\n" \
        "upscaler = bilinear\n" \
        "dlaa = 1\n" \
        "framegen = off\n" \
        "taa = fsr3\n" \
        "hdr = 0\n" \
        "aspect = fit\n" \
        "game_res = 1800\n"
#define PLAYER_MID \
        "aniso = 2\n" \
        "assume_focus = 1\n" \
        "texture_detail = auto\n" \
        "\n" \
        "[input]\n" \
        "bind_a = key:default, pad:south\n" \
        "bind_b = key:default, pad:east\n" \
        "\n" \
        "[language]\n" \
        "operation_mode = docked\n" \
        "\n" \
        "[performance]\n" \
        "wb_lazy = 0\n" \
        "vram_safe_mode = 1\n" \
        "exception_handler = 1\n" \
        "ASYNC_COMPUTE = 1\n" \
        "f\xC3\xBCr sp\xC3\xA4ter = 1\n" \
        "\n" \
        "[frame rate]\n"
    static const char player[] = PLAYER_HEAD "sharpness = 80\n" PLAYER_MID "fps_limit = 120\n";
    static const char player_saved[] = PLAYER_HEAD PLAYER_MID "fps_limit = 60\n\n[audio]\nvolume = 55\n";
#undef PLAYER_HEAD
#undef PLAYER_MID
    cfg_setenv("NX2NAVI_FPS_LIMIT", NULL);
    cfg_setenv("NX2NAVI_VOLUME", NULL);
    cfg_setenv("NX2NAVI_SHARPNESS", NULL);
    cfg_setenv("NX2NAVI_WB_LAZY", NULL);
    CHECK(plat_file_write(path, player, sizeof player - 1), "write the player's ini");
    CHECK(cfg_load(path) == 1, "load the player's ini");
    {
        int n = -1;
        const char *u = cfg_unknown_keys(&n);
        CHECK(n == 4 && EQ(u, "async_compute, vram_safe_mode, exception_handler"),
              "the player's three lines are named as no settings; a repeat counts once, a key of other characters is only counted");
        CHECK(EQ(cfg_get("NX2NAVI_WB_LAZY"), "0") && cfg_is_set("NX2NAVI_WB_LAZY"), "wb_lazy = 0 is a setting, and set");
        CHECK(!cfg_is_set("NX2NAVI_VK_NAN_PRESERVE"), "a key the file lacks is not set");
        static const char *const reset[] = {"SHARPNESS"};
        cfg_set("NX2NAVI_FPS_LIMIT", "60");
        cfg_set("NX2NAVI_VOLUME", "55");
        CHECK(cfg_reset(reset, 1) == 1 && cfg_save_to(path) == 1, "the menu changes, adds and resets a key and saves");
        uint64_t pn = 0;
        uint8_t *p = plat_file_read(path, &pn);
        CHECK(p && pn == sizeof player_saved - 1 && !memcmp(p, player_saved, pn),
              "the save keeps every other line byte for byte, the lines that are no settings included");
        if (p && (pn != sizeof player_saved - 1 || memcmp(p, player_saved, pn))) fprintf(stderr, "---- saved:\n%s\n----\n", p);
        free(p);
    }
    plat_file_delete(path);
    CHECK(cfg_load(path) == 0, "the player's ini is gone again");

    // cfg_set / cfg_save: only changed lines move, comments stay, a missing key goes under its section
    static const char before[] =
        "\xEF\xBB\xBF# my settings\r\n"
        "[frame rate]\r\n"
        "  FPS60 = 1\r\n"
        "; keep this\r\n"
        "\r\n"
        "[input]\r\n"
        "# Gamepad motion controls (0 = off) and vibration (0 = off).\r\n"
        "# motion = 1\r\n"
        "# rumble = 1\r\n"
        "rumble_curve = 0.6\r\n"
        "\r\n"
        "[files]\r\n"
        "romfs_mods = C:\\mods";
    static const char after[] =
        "\xEF\xBB\xBF# my settings\r\n"
        "[frame rate]\r\n"
        "  FPS60 = 0\r\n"
        "; keep this\r\n"
        "\r\n"
        "[input]\r\n"
        "# Gamepad motion controls (0 = off) and vibration (0 = off).\r\n"
        "# motion = 1\r\n"
        "motion = 0\r\n"
        "# rumble = 1\r\n"
        "rumble_curve = 0.6\r\n"
        "rumble_strength = 80\r\n"
        "\r\n"
        "[files]\r\n"
        "romfs_mods = C:\\mods\r\n"
        "\r\n"
        "[audio]\r\n"
        "volume = 40\r\n";
    cfg_setenv("NX2NAVI_MOTION", NULL);
    cfg_setenv("NX2NAVI_VOLUME", NULL);
    cfg_setenv("NX2NAVI_RUMBLE_STRENGTH", NULL);
    CHECK(plat_file_write(path, before, sizeof before - 1), "write the save ini");
    CHECK(cfg_load(path) == 1, "load the save ini");
    unsigned v0 = cfg_version();
    cfg_set("NX2NAVI_FPS60", "0");
    cfg_set("NX2NAVI_MOTION", "0");
    cfg_set("NX2NAVI_RUMBLE_STRENGTH", "80");
    cfg_set("NX2NAVI_VOLUME", "40");
    CHECK(cfg_version() == v0 + 4, "cfg_set bumps the version");
    CHECK(EQ(cfg_get("NX2NAVI_FPS60"), "0") && EQ(cfg_get("NX2NAVI_VOLUME"), "40"), "cfg_set values are read back at once");
    CHECK(cfg_save_to(path) == 1, "cfg_save_to writes");
    uint64_t got_n = 0;
    uint8_t *got = plat_file_read(path, &got_n);
    CHECK(got && got_n == sizeof after - 1 && !memcmp(got, after, got_n),
          "saved file: changed line replaced, comments kept, missing keys under their sections");
    if (got && (got_n != sizeof after - 1 || memcmp(got, after, got_n))) fprintf(stderr, "---- saved:\n%s\n----\n", got);
    free(got);
    CHECK(cfg_save_to(path) == 1, "a save with nothing changed");
    got = plat_file_read(path, &got_n);
    CHECK(got && got_n == sizeof after - 1 && !memcmp(got, after, got_n), "a save with nothing changed keeps the file");
    free(got);
    CHECK(cfg_load(path) == 1 && EQ(cfg_get("NX2NAVI_FPS60"), "0") && EQ(cfg_get("NX2NAVI_MOTION"), "0") &&
          EQ(cfg_get("NX2NAVI_RUMBLE_STRENGTH"), "80") && EQ(cfg_get("NX2NAVI_VOLUME"), "40") &&
          EQ(cfg_get("NX2NAVI_ROMFS_MODS"), "C:\\mods"), "the saved file reads back");
    const char *held = cfg_get("NX2NAVI_VOLUME");
    cfg_setenv("NX2NAVI_VOLUME", "70");
    cfg_set("NX2NAVI_VOLUME", "90");
    CHECK(cfg_from_env("NX2NAVI_VOLUME") && EQ(cfg_get("NX2NAVI_VOLUME"), "70"), "environment wins over cfg_set");
    CHECK(EQ(held, "40"), "a pointer cfg_get handed out stays valid after cfg_set");
    cfg_setenv("NX2NAVI_VOLUME", NULL);
    CHECK(!cfg_from_env("NX2NAVI_VOLUME") && EQ(cfg_get("NX2NAVI_VOLUME"), "90"), "cfg_set value returns without the variable");
    plat_file_delete(path);
    CHECK(cfg_save_to(path) == 1, "cfg_save_to creates a missing file");
    got = plat_file_read(path, &got_n);
    CHECK(got && !strcmp((const char *)got, "[audio]\nvolume = 90\n"), "new file: the section and the key");
    free(got);
    // values cfg_load would trim or unquote: mod folder names in the order list start with a space or are quoted
    cfg_set("NX2NAVI_MODS_ORDER", " Lead|Tail ");
    cfg_set("NX2NAVI_MODS_DISABLED", "\"Q\"");
    CHECK(cfg_save_to(path) == 1 && cfg_load(path) == 1 && EQ(cfg_get("NX2NAVI_MODS_ORDER"), " Lead|Tail ") &&
          EQ(cfg_get("NX2NAVI_MODS_DISABLED"), "\"Q\""), "blanks at the ends and quotes survive a save");
    cfg_set("NX2NAVI_MODS_ORDER", "A|B");
    CHECK(cfg_save_to(path) == 1 && cfg_load(path) == 1 && EQ(cfg_get("NX2NAVI_MODS_ORDER"), "A|B"),
          "a plain value replacing a quoted one");
    got = plat_file_read(path, &got_n);
    CHECK(got && strstr((const char *)got, "mods_order = A|B\n") &&
              strstr((const char *)got, "mods_disabled = \"\"Q\"\"\n"),
          "only the values that need it are quoted");
    free(got);

    // Defaults table: the menu's "default" marks and Reset
    const CfgMeta *m = cfg_meta("FPS60");
    CHECK(m && EQ(m->def, "1") && m->live, "meta: fps60 defaults to 1 and is live");
    m = cfg_meta("ASYNC_PREEMPT");
    CHECK(m && EQ(m->values, "0|1|2") && !m->live, "meta: async_preempt takes 0|1|2 and needs a restart");
    m = cfg_meta("nx2navi_vk_present");
    CHECK(m && EQ(m->key, "VK_PRESENT") && EQ(m->values, "mailbox|fifo|immediate") && m->live,
          "meta: prefix and case are accepted");
    m = cfg_meta("RUMBLE_STRENGTH");
    CHECK(m && EQ(m->def, "130") && EQ(m->values, "0..300"), "meta: rumble strength 130 in 0..300");
    m = cfg_meta("MONITOR");
    CHECK(m && EQ(m->def, "auto") && !m->values && !m->live, "meta: monitor is auto by default, free text, read at start");
    m = cfg_meta("PERF_OVERLAY");
    CHECK(m && EQ(m->def, "off") && EQ(m->values, "off|fps|full") && m->live, "meta: perf_overlay is live, off by default");
    m = cfg_meta("AUDIO_DEVICE");
    CHECK(m && EQ(m->def, "default") && m->values == NULL && !m->live, "meta: audio_device is free text, default, needs a restart");
    CHECK(cfg_meta("BIND_A") == NULL && cfg_meta("FPS_COUNTER") == NULL && cfg_meta("NO_SUCH") == NULL,
          "meta: keys the menu does not show are NULL");
    m = cfg_meta("FRAMEGEN_MULTIPLIER");
    CHECK(m && EQ(m->def, "2") && EQ(m->values, "2|3|4") && m->live, "meta: framegen_multiplier is 2|3|4, 2 by default, live");

    // DLSS frame generation's multiplier (vk_dlssg.c): the setting clamped to what the GPU and driver allow, with the
    // reason vk_dlssg_frame logs when it is not taken as it is
    const char *why = "";
    CHECK(vk_dlssg_multiplier_of("3", 4, &why) == 3 && !why, "multiplier: 3 where 4x runs");
    CHECK(vk_dlssg_multiplier_of("4x", 4, &why) == 4 && !why, "multiplier: 4x reads as 4");
    CHECK(vk_dlssg_multiplier_of("4", 2, &why) == 2 && why, "multiplier: 4 where 2x is the most is 2, with a reason");
    CHECK(vk_dlssg_multiplier_of("4", 3, &why) == 3 && why, "multiplier: 4 where 3x is the most is 3, with a reason");
    CHECK(vk_dlssg_multiplier_of("9", 0, &why) == 4 && why, "multiplier: above 4 is 4, with a reason");
    CHECK(vk_dlssg_multiplier_of("1", 4, &why) == 2 && why, "multiplier: below 2 is 2, with a reason");
    CHECK(vk_dlssg_multiplier_of("fast", 4, &why) == 2 && why, "multiplier: not a number is 2, with a reason");
    CHECK(vk_dlssg_multiplier_of("3y", 4, &why) == 2 && why, "multiplier: a number with other text after it is 2");
    CHECK(vk_dlssg_multiplier_of("", 3, &why) == 2 && !why && vk_dlssg_multiplier_of(NULL, 3, NULL) == 2,
          "multiplier: unset is 2");
    CHECK(vk_dlssg_multiplier_of("4", 0, &why) == 4 && !why, "multiplier: kept as it is while the GPU's limit is not known");
    CHECK(vk_dlssg_multiplier_max(NULL) == 0 && vk_dlssg_multiplier() == 1, "multiplier: 1, no limit, while DLSS-G does not run");

    // cfg_reset: the key's active lines leave nx2navi.ini, the commented example stays
    static const char rbase[] = "[audio]\n# volume = 100\n[input]\nmotion = 0\n";
    static const char *const vol[] = {"VOLUME"};
    cfg_setenv("NX2NAVI_VOLUME", NULL);
    CHECK(plat_file_write(path, rbase, sizeof rbase - 1), "write the reset ini");
    CHECK(cfg_load(path) == 1, "load the reset ini");
    cfg_set("NX2NAVI_VOLUME", "40");
    CHECK(cfg_save_to(path) == 1 && cfg_last_save_ok(), "reset: volume saved");
    got = plat_file_read(path, &got_n);
    CHECK(got && strstr((const char *)got, "\nvolume = 40\n"), "reset: the active line is in the file");
    free(got);
    unsigned v1 = cfg_version();
    CHECK(cfg_reset(vol, 1) == 1, "reset: one key reset");
    CHECK(cfg_version() > v1 && cfg_get("NX2NAVI_VOLUME") == NULL, "reset: the value is gone in memory, the version moved");
    CHECK(cfg_reset(vol, 1) == 0, "reset: a key already reset counts 0");
    CHECK(cfg_save_to(path) == 1, "reset: save");
    got = plat_file_read(path, &got_n);
    CHECK(got && got_n == sizeof rbase - 1 && !memcmp(got, rbase, got_n) && strstr((const char *)got, "# volume = 100"),
          "reset: no active volume line, the commented example and the other keys stay");
    if (got && (got_n != sizeof rbase - 1 || memcmp(got, rbase, got_n))) fprintf(stderr, "---- saved:\n%s\n----\n", got);
    free(got);
    cfg_set("NX2NAVI_VOLUME", "55");
    CHECK(cfg_reset(vol, 1) == 1 && cfg_get("NX2NAVI_VOLUME") == NULL, "reset: a value set after a save resets too");
    cfg_set("NX2NAVI_VOLUME", "60");
    CHECK(cfg_save_to(path) == 1 && EQ(cfg_get("NX2NAVI_VOLUME"), "60"), "reset then set before the save: the set wins");
    got = plat_file_read(path, &got_n);
    CHECK(got && strstr((const char *)got, "\nvolume = 60\n"), "reset then set: the new line is written");
    free(got);

    // an active line with an empty value has no entry in memory; Reset still removes it from the file
    static const char rempty[] = "[audio]\n# volume = 100\nvolume =\n[input]\nmotion = 0\n";
    CHECK(plat_file_write(path, rempty, sizeof rempty - 1) && cfg_load(path) == 1 && cfg_get("NX2NAVI_VOLUME") == NULL,
          "reset: load an ini with an empty value");
    CHECK(cfg_reset(vol, 1) == 0 && cfg_save_to(path) == 1, "reset: an empty-valued key saves");
    got = plat_file_read(path, &got_n);
    CHECK(got && got_n == sizeof rbase - 1 && !memcmp(got, rbase, got_n), "reset: the empty-valued line is gone");
    free(got);
    cfg_set("NX2NAVI_VOLUME", "60");
    CHECK(cfg_save_to(path) == 1, "reset: volume 60 again for the environment case");

    // a file saved as UTF-16 (little and big endian, with a line ending of two characters) reads as the same lines
    static const char wtext[] = "[audio]\r\nvolume = 35\r\n[input]\r\nmotion = 0\r\n";
    for (int be = 0; be < 2; be++) {
        uint8_t wbuf[2 + 2 * sizeof wtext];
        size_t wn = 0;
        wbuf[wn++] = be ? 0xFE : 0xFF, wbuf[wn++] = be ? 0xFF : 0xFE;
        for (size_t i = 0; i + 1 < sizeof wtext; i++)
            wbuf[wn++] = be ? 0 : (uint8_t)wtext[i], wbuf[wn++] = be ? (uint8_t)wtext[i] : 0;
        CHECK(plat_file_write("cfg-probe-utf16.ini", wbuf, wn) && cfg_load("cfg-probe-utf16.ini") == 1 &&
              EQ(cfg_get("NX2NAVI_VOLUME"), "35") && EQ(cfg_get("NX2NAVI_MOTION"), "0"),
              be ? "an ini saved as UTF-16, big endian, is not read" : "an ini saved as UTF-16, little endian, is not read");
    }
    plat_file_delete("cfg-probe-utf16.ini");

    // a key the environment sets is skipped and keeps its file line
    CHECK(cfg_load(path) == 1 && EQ(cfg_get("NX2NAVI_VOLUME"), "60"), "reset: reload");
    cfg_setenv("NX2NAVI_VOLUME", "70");
    CHECK(cfg_reset(vol, 1) == 0, "reset: an environment-set key is skipped");
    cfg_setenv("NX2NAVI_VOLUME", NULL);
    CHECK(EQ(cfg_get("NX2NAVI_VOLUME"), "60"), "reset: the skipped key keeps its value");
    CHECK(cfg_save_to(path) == 1, "reset: save after a skip");
    got = plat_file_read(path, &got_n);
    CHECK(got && strstr((const char *)got, "\nvolume = 60\n"), "reset: the skipped key keeps its file line");
    free(got);

    // a failed save: the call fails, cfg_last_save_ok says so, the value stays set and a later save works
    CHECK(cfg_last_save_ok() == 1, "save status: ok after a good save");
    cfg_set("NX2NAVI_VOLUME", "20");
    char bad[1100];
    snprintf(bad, sizeof bad, "%s", path);
    strcpy(plat_last_sep(bad) + 1, "no-such-dir/cfg-probe.ini");
    CHECK(cfg_save_to(bad) == 0 && cfg_last_save_ok() == 0 && EQ(cfg_get("NX2NAVI_VOLUME"), "20"),
          "save status: an unwritable path fails, the value stays");
    cfg_set("NX2NAVI_VOLUME", "21");
    CHECK(plat_file_write(path, rbase, sizeof rbase - 1), "write the read-only ini");
    CHECK(plat_file_set_readonly(path, 1), "set the file read-only");
    CHECK(cfg_save_to(path) == 0 && cfg_last_save_ok() == 0 && EQ(cfg_get("NX2NAVI_VOLUME"), "21"),
          "save status: a read-only file fails, cfg_get returns the new value");
    plat_file_set_readonly(path, 0);
    CHECK(cfg_save_to(path) == 1 && cfg_last_save_ok() == 1, "save status: the retry after the failure works");
    got = plat_file_read(path, &got_n);
    CHECK(got && strstr((const char *)got, "\nvolume = 21\n"), "save status: the retried save wrote the value");
    free(got);
    plat_file_delete(path);
    cfg_load(path);
    CHECK(rumble_motor(0, 0.6f, 1.3f) == 0 && rumble_motor(0.5f, 0.6f, 0) == 0, "rumble: zero amplitude or strength is silent");
    CHECK(rumble_motor(1, 0.6f, 3) == 65535 && rumble_motor(5, 1, 1) == 65535, "rumble: saturates at 65535");
    CHECK(rumble_motor(0.25f, 1, 1) > 16000 && rumble_motor(0.25f, 1, 1) < 16800, "rumble: curve 1, strength 100% is linear");
    CHECK(rumble_motor(0.1f, 0.6f, 1.3f) > rumble_motor(0.1f, 1, 1.3f), "rumble: curve lifts weak values");
    CHECK(rumble_motor(0.3f, 0.6f, 1.3f) > rumble_motor(0.3f, 0.6f, 1), "rumble: strength scales");
    // Keyboard and mouse bindings (input_map.c)
    uint8_t dn[KEY_COUNT] = {0};
    int32_t st[2];
    CHECK(input_map_buttons(dn) == 0, "keys: nothing held, no buttons");
    dn[KEY_MOUSE_L] = 1;
    CHECK(input_map_buttons(dn) == 1ull << B_Y, "keys: left mouse button is Y");
    dn[KEY_MOUSE_R] = dn[KEY_Z] = 1;
    CHECK(input_map_buttons(dn) == (1ull << B_Y | 1ull << B_ZL), "keys: right mouse button and Z both press ZL");
    memset(dn, 0, sizeof dn);
    dn[KEY_X] = dn[KEY_TAB] = dn[KEY_M] = dn[KEY_V] = dn[KEY_3] = 1;
    CHECK(input_map_buttons(dn) == (1ull << B_ZR | 1ull << B_PLUS | 1ull << B_MINUS | 1ull << B_STICK_L | 1ull << B_RIGHT),
          "keys: X ZR, Tab Plus, M Minus, V stick click, 3 D-pad right");
    memset(dn, 0, sizeof dn);
    dn[KEY_LSHIFT] = 1;
    CHECK(input_map_buttons(dn) == 1ull << B_A, "keys: Left Shift is A (Ocarina dashes on A held)");
    dn[KEY_LSHIFT] = 0, dn[KEY_LCTRL] = dn[KEY_O] = dn[KEY_N] = 1;
    CHECK(input_map_buttons(dn) == (1ull << B_B | 1ull << B_LEFT | 1ull << B_RIGHT),
          "keys: Left Ctrl puts away (B), O the ocarina (D-pad left), N Navi (D-pad right)");
    memset(dn, 0, sizeof dn);
    dn[KEY_G] = 1;
    CHECK(input_map_buttons(dn) == 0 && !input_map_held(dn, B_SHAKE), "keys: G is free (no shake key by default)");
    memset(dn, 0, sizeof dn);
    dn[KEY_W] = 1;
    input_map_stick(dn, 0, st);
    CHECK(st[0] == 0 && st[1] == 32767, "keys: W is left stick up");
    dn[KEY_A] = 1;
    input_map_stick(dn, 0, st);
    CHECK(st[0] == -23170 && st[1] == 23170, "keys: W A is a diagonal on the unit circle");
    dn[KEY_W] = dn[KEY_A] = dn[KEY_S] = 1;
    dn[KEY_D] = 1;
    input_map_stick(dn, 0, st);
    CHECK(st[0] == 0 && st[1] == 0, "keys: opposite keys cancel");
    memset(dn, 0, sizeof dn);
    dn[KEY_LEFT] = 1;
    input_map_stick(dn, 1, st);
    CHECK(st[0] == -32767 && st[1] == 0, "keys: left arrow is right stick left");
    input_map_stick(dn, 0, st);
    CHECK(st[0] == 0 && st[1] == 0, "keys: arrows leave the left stick alone");
    input_mouse_stick(30, 0, 1.0 / 60, 1, 1, st);
    CHECK(st[0] == 32767 * 1800 / 3000 && st[1] == 0, "mouse: 30 counts in 1/60 s is 60% right");
    input_mouse_stick(0, -10, 1.0 / 60, 1, 1, st);
    CHECK(st[0] == 0 && st[1] > 0, "mouse: moving up is stick up");
    input_mouse_stick(30, 0, 1.0 / 30, 1, 1, st);
    CHECK(st[0] == 32767 * 900 / 3000, "mouse: the same counts over twice the time are half the deflection");
    input_mouse_stick(15, 0, 1.0 / 60, 2, 1, st);
    CHECK(st[0] == 32767 * 1800 / 3000, "mouse: doubling sensitivity halves the counts needed (15 counts at 2 = 30 at 1)");
    input_mouse_stick(1000, 1000, 1.0 / 60, 1, 1, st);
    CHECK((int64_t)st[0] * st[0] + (int64_t)st[1] * st[1] <= 32768ll * 32768, "mouse: clamped to the unit circle");
    input_mouse_stick(30, 0, 1.0 / 60, 0, 1, st);
    CHECK(st[0] == 0 && st[1] == 0, "mouse: sensitivity 0 is off");
    input_mouse_stick(30, 0, 1.0 / 60, 1, 1.5f, st);
    CHECK(st[0] == 13107 && st[1] == 0, "mouse: a camera speed of 1.5 divides the deflection, so the turn per count stays");
    input_mouse_stick(1000, 0, 1.0 / 60, 1, 1.5f, st);
    CHECK(st[0] == 32767 && st[1] == 0, "mouse: a fast flick reaches full tilt at a camera speed of 1.5 too");
    input_pad_stick(3000, -2000, 0.15f, st);
    CHECK(st[0] == 0 && st[1] == 0, "pad stick: an 11% drift inside a 15% dead zone reads centred");
    input_pad_stick(4000, -3000, 0, st);
    CHECK(st[0] == 4000 && st[1] == -3000, "pad stick: dead zone 0 passes the stick unchanged");
    input_pad_stick(32767, 0, 0.15f, st);
    CHECK(st[0] == 32767 && st[1] == 0, "pad stick: full deflection still reaches full");
    input_pad_stick(-32768, -32768, 0.15f, st);
    CHECK(st[0] < 0 && st[1] < 0 && st[0] >= -32767 && (int64_t)st[0] * st[0] + (int64_t)st[1] * st[1] <= 32768ll * 32768,
          "pad stick: a square gate's corner stays on the unit circle");
    input_pad_stick(0, 32767 / 2 + 1, 0.2f, st);
    CHECK(abs(st[1] - 32767 * 3 / 8) <= 3, "pad stick: half deflection past a 20% zone is (0.5 - 0.2) / 0.8 of full");
    cfg_setenv("NX2NAVI_STICK_DEADZONE", "12");
    cfg_setenv("NX2NAVI_LSTICK_DEADZONE", NULL);
    cfg_setenv("NX2NAVI_RSTICK_DEADZONE", NULL);
    CHECK(fabsf(input_pad_deadzone(0) - 0.12f) < 1e-6f && fabsf(input_pad_deadzone(1) - 0.12f) < 1e-6f,
          "pad dead zone: both sticks follow stick_deadzone while lstick/rstick are unset");
    cfg_setenv("NX2NAVI_RSTICK_DEADZONE", "20");
    CHECK(fabsf(input_pad_deadzone(0) - 0.12f) < 1e-6f && fabsf(input_pad_deadzone(1) - 0.20f) < 1e-6f,
          "pad dead zone: rstick_deadzone sets the right stick alone");
    cfg_setenv("NX2NAVI_LSTICK_DEADZONE", "80");
    CHECK(fabsf(input_pad_deadzone(0) - 0.5f) < 1e-6f, "pad dead zone: clamped to 50%");
    cfg_setenv("NX2NAVI_STICK_DEADZONE", NULL);
    cfg_setenv("NX2NAVI_LSTICK_DEADZONE", NULL);
    cfg_setenv("NX2NAVI_RSTICK_DEADZONE", NULL);
    CHECK(input_stick_percent(32767, 0) > 99.99 && input_stick_percent(32767, 0) < 100.01 &&
              input_stick_percent(0, 0) == 0 && fabs(input_stick_percent(3000, -4000) - 5000 * 100.0 / 32767) < 1e-9,
          "stick percent: an axis's end is 100%, the centre 0%, round in between");
    // Gamepad layouts (input_map.c)
    CHECK(input_pad_layout_pick("auto", 1) == PAD_LAYOUT_NINTENDO && input_pad_layout_pick(NULL, 0) == PAD_LAYOUT_XBOX,
          "pad layout: automatic is Nintendo for an east button labelled A, Xbox otherwise");
    CHECK(input_pad_layout_pick("Xbox", 1) == PAD_LAYOUT_XBOX && input_pad_layout_pick("nintendo", 0) == PAD_LAYOUT_NINTENDO,
          "pad layout: a set layout wins over the labels");
    CHECK(input_pad_layout_pick("xbox_prompts", 0) == PAD_LAYOUT_NINTENDO &&
              input_pad_layout_pick("Xbox_Prompts", 1) == PAD_LAYOUT_NINTENDO,
          "pad layout: 0.1.9's xbox_prompts keeps the Nintendo positions (only the prompts change, romfs.c overlay_init)");
    CHECK(input_pad_layout_pick("PlayStation", 1) == PAD_LAYOUT_XBOX && input_pad_layout_pick("playstation", 0) == PAD_LAYOUT_XBOX,
          "pad layout: playstation plays by label as xbox does (cross A), whatever the pad's labels");
    // Button prompts (input_map.c)
    CHECK(input_prompts_pick(NULL, "auto") == PROMPTS_NINTENDO && input_prompts_pick("", "xbox") == PROMPTS_NINTENDO &&
              input_prompts_pick("Xbox", "nintendo") == PROMPTS_XBOX && input_prompts_pick("nintendo", "xbox") == PROMPTS_NINTENDO,
          "prompts: the game's own unless set to Xbox");
    CHECK(input_prompts_pick("PC", "nintendo") == PROMPTS_PC && input_prompts_pick("pc", "xbox_prompts") == PROMPTS_PC &&
              EQ(input_prompts_folder(PROMPTS_PC, "nintendo"), "romfs-patches-pc") &&
              EQ(input_prompts_folder(PROMPTS_PC, "xbox"), "romfs-patches-pc") &&
              EQ(input_prompts_folder(PROMPTS_PC, NULL), "romfs-patches-pc"),
          "prompts: pc shows the keyboard and mouse whatever the pad layout");
    CHECK(input_prompts_pick(NULL, "xbox_prompts") == PROMPTS_XBOX && input_prompts_pick("nintendo", "xbox_prompts") == PROMPTS_NINTENDO,
          "prompts: 0.1.9's pad_layout = xbox_prompts means Xbox while prompts is unset");
    CHECK(!input_prompts_folder(PROMPTS_NINTENDO, "nintendo") && !input_prompts_folder(PROMPTS_NINTENDO, "xbox"),
          "prompts: the game's own need no folder");
    CHECK(EQ(input_prompts_folder(PROMPTS_XBOX, "nintendo"), "romfs-patches-xbox") &&
              EQ(input_prompts_folder(PROMPTS_XBOX, "xbox_prompts"), "romfs-patches-xbox") &&
              EQ(input_prompts_folder(PROMPTS_XBOX, "xbox"), "romfs-patches-xbox-label") &&
              EQ(input_prompts_folder(PROMPTS_XBOX, "auto"), "romfs-patches-xbox-label") &&
              EQ(input_prompts_folder(PROMPTS_XBOX, NULL), "romfs-patches-xbox-label"),
          "prompts: the face buttons by position for the Nintendo layout, by label for xbox and auto");
    CHECK(input_prompts_pick("PlayStation", "nintendo") == PROMPTS_PLAYSTATION &&
              input_prompts_pick("playstation", "xbox_prompts") == PROMPTS_PLAYSTATION &&
              input_prompts_pick("ps", "nintendo") == PROMPTS_NINTENDO,
          "prompts: playstation in any case; an unknown value is the game's own");
    CHECK(input_prompts_pick("SteamDeck", "nintendo") == PROMPTS_STEAMDECK &&
              input_prompts_pick("steamdeck", "xbox_prompts") == PROMPTS_STEAMDECK &&
              EQ(input_prompts_folder(PROMPTS_STEAMDECK, "nintendo"), "romfs-patches-steamdeck") &&
              EQ(input_prompts_folder(PROMPTS_STEAMDECK, "xbox"), "romfs-patches-steamdeck-label") &&
              EQ(input_prompts_folder(PROMPTS_STEAMDECK, "auto"), "romfs-patches-steamdeck-label") &&
              EQ(input_prompts_folder(PROMPTS_STEAMDECK, NULL), "romfs-patches-steamdeck-label"),
          "prompts: steamdeck in any case, its face buttons by position for the Nintendo layout, by label otherwise");
    CHECK(EQ(input_prompts_folder(PROMPTS_PLAYSTATION, "nintendo"), "romfs-patches-playstation") &&
              EQ(input_prompts_folder(PROMPTS_PLAYSTATION, "xbox_prompts"), "romfs-patches-playstation") &&
              EQ(input_prompts_folder(PROMPTS_PLAYSTATION, "xbox"), "romfs-patches-playstation-label") &&
              EQ(input_prompts_folder(PROMPTS_PLAYSTATION, "auto"), "romfs-patches-playstation-label") &&
              EQ(input_prompts_folder(PROMPTS_PLAYSTATION, NULL), "romfs-patches-playstation-label"),
          "prompts: the PlayStation face buttons by position for the Nintendo layout, by label for xbox and auto");
    // romfs-patches-xbox shows A as B and romfs-patches-playstation A as circle (tools/pad_prompts.py FACE). That is
    // right exactly where the layout puts A on the east button, which an Xbox pad labels B and a PlayStation pad circle.
    input_map_reset();
    static const char *const layouts[] = {"nintendo", "xbox", "playstation", "auto", "xbox_prompts"};
    for (int i = 0; i < 5; i++) {
        int a_east = input_map_pad_buttons((uint8_t[PAD_COUNT]){[PAD_EAST] = 1}, input_pad_layout_pick(layouts[i], 0)) == 1ull << B_A;
        CHECK(a_east == EQ(input_prompts_folder(PROMPTS_XBOX, layouts[i]), "romfs-patches-xbox"),
              "prompts: the folder names A as B exactly when the layout puts A on the button labelled B");
        CHECK(a_east == EQ(input_prompts_folder(PROMPTS_PLAYSTATION, layouts[i]), "romfs-patches-playstation"),
              "prompts: the folder names A as circle exactly when the layout puts A on the circle button");
    }
    // The default layout is the Switch's own buttons by position, whatever the pad's labels (an Xbox pad's east button
    // is labelled B and does Switch A): A right, B bottom, X top, Y left, L/R the bumpers, ZL/ZR the triggers.
    const CfgMeta *pl = cfg_meta("PAD_LAYOUT");
    CHECK(pl && input_pad_layout_pick(pl->def, 0) == PAD_LAYOUT_NINTENDO, "pad layout: the default is Nintendo, also for an Xbox pad");
    static const struct { int pad, button; } sw[] = {{PAD_EAST, B_A}, {PAD_SOUTH, B_B}, {PAD_NORTH, B_X}, {PAD_WEST, B_Y},
                                                      {PAD_L, B_L}, {PAD_R, B_R}, {PAD_ZL, B_ZL}, {PAD_ZR, B_ZR},
                                                      {PAD_START, B_PLUS}, {PAD_BACK, B_MINUS}, {PAD_LSTICK, B_STICK_L},
                                                      {PAD_RSTICK, B_STICK_R}, {PAD_DPAD_UP, B_UP}, {PAD_DPAD_DOWN, B_DOWN},
                                                      {PAD_DPAD_LEFT, B_LEFT}, {PAD_DPAD_RIGHT, B_RIGHT}};
    int sw_ok = 1;
    for (size_t i = 0; i < sizeof sw / sizeof *sw; i++) {
        uint8_t one[PAD_COUNT] = {0};
        one[sw[i].pad] = 1;
        sw_ok &= input_map_pad_buttons(one, PAD_LAYOUT_NINTENDO) == 1ull << sw[i].button;
    }
    CHECK(sw_ok, "pad layout: Nintendo puts every Switch button at its position on the Switch");
    uint8_t pd[PAD_COUNT] = {0};
    pd[PAD_SOUTH] = pd[PAD_NORTH] = 1;
    CHECK(input_map_pad_buttons(pd, PAD_LAYOUT_NINTENDO) == (1ull << B_B | 1ull << B_X) &&
          input_map_pad_buttons(pd, PAD_LAYOUT_XBOX) == (1ull << B_A | 1ull << B_Y),
          "pad layout: south and north are B and X by position, A and Y on Xbox");
    pd[PAD_SOUTH] = pd[PAD_NORTH] = 0;
    pd[PAD_START] = pd[PAD_ZR] = 1;
    CHECK(input_map_pad_buttons(pd, PAD_LAYOUT_XBOX) == (1ull << B_PLUS | 1ull << B_ZR), "pad layout: Xbox keeps the other buttons");
    CHECK(input_map_bind("A", "pad:west") && input_map_pad_buttons((uint8_t[PAD_COUNT]){[PAD_WEST] = 1}, PAD_LAYOUT_XBOX) ==
                                                 (1ull << B_A | 1ull << B_X),
          "pad layout: a bind line adds its physical button in every layout");
    CHECK(input_map_pad_buttons((uint8_t[PAD_COUNT]){[PAD_SOUTH] = 1}, PAD_LAYOUT_XBOX) == 0,
          "pad layout: a bind line replaces the layout's own button");
    input_map_reset();
    CHECK(input_map_pad_buttons((uint8_t[PAD_COUNT]){[PAD_RPADDLE1] = 1}, PAD_LAYOUT_XBOX) == 0,
          "pad layout: a back paddle is unbound by default");
    CHECK(input_map_bind("ZL", "pad:default, pad:rpaddle1") &&
              input_map_pad_buttons((uint8_t[PAD_COUNT]){[PAD_RPADDLE1] = 1}, PAD_LAYOUT_NINTENDO) == 1ull << B_ZL &&
              input_map_pad_buttons((uint8_t[PAD_COUNT]){[PAD_ZL] = 1}, PAD_LAYOUT_NINTENDO) == 1ull << B_ZL,
          "bind: pad:rpaddle1 adds a back paddle, pad:default keeps the trigger");
    input_map_reset();
    // Bind lines for stick directions and default parts; the remapping rows' edits (input_map.c)
    memset(dn, 0, sizeof dn);
    dn[KEY_I] = 1;
    CHECK(input_map_bind("LSTICK_UP", "key:i, pad:south") == 1, "bind: a stick direction takes keys, skips pad entries");
    input_map_stick(dn, 0, st);
    CHECK(st[1] == 32767, "bind: I moves forward after bind_LSTICK_UP = key:i");
    dn[KEY_I] = 0, dn[KEY_W] = 1;
    input_map_stick(dn, 0, st);
    CHECK(st[1] == 0, "bind: W no longer moves forward");
    CHECK(input_map_bind("A", "key:default, pad:north") == 1, "bind: key:default with a pad entry");
    memset(dn, 0, sizeof dn);
    dn[KEY_E] = 1;
    CHECK(input_map_buttons(dn) == 1ull << B_A &&
              input_map_pad_buttons((uint8_t[PAD_COUNT]){[PAD_NORTH] = 1}, PAD_LAYOUT_XBOX) == (1ull << B_A | 1ull << B_Y),
          "bind: key:default keeps E, pad:north adds the top button in every layout");
    CHECK(input_map_bind("B", "key:j, pad:default") == 1 &&
              input_map_pad_buttons((uint8_t[PAD_COUNT]){[PAD_EAST] = 1}, PAD_LAYOUT_XBOX) == 1ull << B_B &&
              input_map_pad_buttons((uint8_t[PAD_COUNT]){[PAD_SOUTH] = 1}, PAD_LAYOUT_NINTENDO) == 1ull << B_B,
          "bind: pad:default keeps each layout's own button");
    input_map_reset();
    InputMap mp = input_map;
    char line[256];
    input_map_line(&mp, B_ZR, line, sizeof line);
    CHECK(!line[0], "edit: an action at its defaults needs no line");
    CHECK(input_map_set_key(&mp, B_A, 0, KEY_F) == B_ZR, "edit: F taken from ZR");
    input_map_line(&mp, B_A, line, sizeof line);
    CHECK(!strcmp(line, "key:f, key:lshift, pad:default"), "edit: A's line is key:f, key:lshift (its second key), pad:default");
    input_map_line(&mp, B_ZR, line, sizeof line);
    CHECK(!strcmp(line, "key:x, pad:default"), "edit: ZR keeps X");
    CHECK(input_map_set_key(&mp, B_ZL, 1, KEY_NONE) == -1, "edit: clearing a key takes from no one");
    uint8_t ks[8];
    CHECK(input_map_keys_of(&mp, B_ZL, ks, 8) == 1 && ks[0] == KEY_MOUSE_R, "edit: ZL keeps its first key only");
    mp = input_map;
    CHECK(input_map_set_pad(&mp, B_X, PAD_LAYOUT_XBOX, PAD_SOUTH) == B_A, "edit: south swaps X with A (Xbox layout)");
    uint8_t ps[8];
    CHECK(input_map_pads_of(&mp, B_A, PAD_LAYOUT_XBOX, ps, 8) == 1 && ps[0] == PAD_WEST, "edit: A takes X's west button");
    CHECK(input_map_pads_of(&mp, B_A, PAD_LAYOUT_NINTENDO, ps, 8) == 1 && ps[0] == PAD_WEST &&
              input_map_pads_of(&mp, B_B, PAD_LAYOUT_NINTENDO, ps, 8) == 1 && ps[0] == PAD_EAST,
          "edit: the face buttons are pinned in every layout after a gamepad edit");
    input_map_line(&mp, B_X, line, sizeof line);
    CHECK(!strcmp(line, "key:default, pad:south"), "edit: X's line is key:default, pad:south");
    input_map_set_pad(&mp, B_X, PAD_LAYOUT_XBOX, PAD_WEST);   // and back
    input_map_line(&mp, B_A, line, sizeof line);
    CHECK(!line[0] && input_map_pads_of(&mp, B_A, PAD_LAYOUT_NINTENDO, ps, 8) == 1 && ps[0] == PAD_EAST,
          "edit: swapping back restores every layout's defaults");
    mp = input_map;
    input_map_set_key(&mp, B_SL_UP, 0, KEY_NONE);
    input_map_line(&mp, B_SL_UP, line, sizeof line);
    CHECK(!strcmp(line, "none"), "edit: a stick direction without keys is none");
    // defaults live in the CfgMeta table: cfg_value and the typed accessors fall back to it
    cfg_load("cfg-probe-missing.ini");
    cfg_setenv("NX2NAVI_FPS60", NULL);
    cfg_setenv("NX2NAVI_MOUSE_SENS", NULL);
    cfg_setenv("NX2NAVI_FREE_CORES", NULL);
    CHECK(cfg_bool("NX2NAVI_FPS60") == 1, "default: unset FPS60 is on");
    CHECK(EQ(cfg_value("NX2NAVI_VK_PRESENT"), "mailbox") && cfg_int("NX2NAVI_SPLIT_DRAWS") == 64, "default: text and integer");
    cfg_setenv("NX2NAVI_MOUSE_SENS", "2.5");
    CHECK(cfg_double("NX2NAVI_MOUSE_SENS") == 2.5, "double: the environment value");
    cfg_setenv("NX2NAVI_MOUSE_SENS", "fast");
    CHECK(cfg_double("NX2NAVI_MOUSE_SENS") == 1.0, "double: a value that does not parse gives the default");
    cfg_setenv("NX2NAVI_MOUSE_SENS", NULL);
    cfg_set("NX2NAVI_MOUSE_SENS", "3");
    CHECK(cfg_double("NX2NAVI_MOUSE_SENS") == 3.0 && cfg_int("NX2NAVI_MOUSE_SENS") == 3, "a cfg_set value beats the default");
    CHECK(cfg_bool("NX2NAVI_FREE_CORES") == 0, "free_cores unset: off");
    cfg_setenv("NX2NAVI_FREE_CORES", "0");
    CHECK(cfg_bool("NX2NAVI_FREE_CORES") == 0, "free_cores 0 gives the same as unset");
    cfg_setenv("NX2NAVI_FREE_CORES", "1");
    CHECK(cfg_bool("NX2NAVI_FREE_CORES") == 1, "free_cores 1: on");
    cfg_setenv("NX2NAVI_FREE_CORES", NULL);
    // 0.1.9's pad_layout = xbox_prompts (cfg.c migrate): read as the Nintendo layout with the Xbox prompts, saved so
    cfg_setenv("NX2NAVI_PAD_LAYOUT", NULL);
    cfg_setenv("NX2NAVI_PROMPTS", NULL);
    static const char old_layout[] = "[input]\r\npad_layout = xbox_prompts\r\n";
    CHECK(plat_file_write(path, old_layout, sizeof old_layout - 1) && cfg_load(path) == 1 &&
              EQ(cfg_get("NX2NAVI_PAD_LAYOUT"), "nintendo") && EQ(cfg_get("NX2NAVI_PROMPTS"), "xbox"),
          "0.1.9's pad_layout = xbox_prompts reads as nintendo with prompts = xbox");
    CHECK(cfg_save_to(path) == 1, "the read pad layout and prompts save");
    uint64_t saved_n = 0;
    char *saved = (char *)plat_file_read(path, &saved_n);
    CHECK(saved && strstr(saved, "pad_layout = nintendo") && strstr(saved, "prompts = xbox") && !strstr(saved, "xbox_prompts"),
          "the next save writes the pad layout and the prompts");
    free(saved);
    static const char old_layout_own[] = "[input]\r\npad_layout = xbox_prompts\r\nprompts = nintendo\r\n";
    CHECK(plat_file_write(path, old_layout_own, sizeof old_layout_own - 1) && cfg_load(path) == 1 &&
              EQ(cfg_get("NX2NAVI_PAD_LAYOUT"), "nintendo") && EQ(cfg_get("NX2NAVI_PROMPTS"), "nintendo"),
          "a prompts line of the file's own wins over 0.1.9's pad_layout = xbox_prompts");
    plat_file_delete(path);
    cfg_load("cfg-probe-missing.ini");
    int listed = 0;
    for (int i = 0; cfg_meta_at(i); i++) {
        const CfgMeta *m = cfg_meta_at(i);
        if (!*m->def) continue;
        listed++;
        char name[96];
        snprintf(name, sizeof name, "NX2NAVI_%s", m->key);
        cfg_setenv(name, "");
        CHECK(EQ(cfg_value(name), m->def), "cfg_value returns the table default");
        int ok = 0;
        if (!m->values) ok = 1;
        else if (strstr(m->values, "..")) {
            // strtod: ranges such as 0.2..0.9 have fractional ends
            double lo = strtod(m->values, NULL), hi = strtod(strstr(m->values, "..") + 2, NULL);
            double d = strtod(m->def, NULL);
            ok = d >= lo && d <= hi;
        } else {
            size_t dl = strlen(m->def);
            for (const char *v = m->values; *v;) {
                size_t n = strcspn(v, "|");
                ok |= n == dl && !strncmp(v, m->def, n);
                v += n + (v[n] == '|');
            }
        }
        if (!ok) fprintf(stderr, "FAIL cfg: default '%s' of %s is not one of its values '%s'\n", m->def, m->key, m->values), fails++;
    }
    CHECK(listed >= 30, "the defaults table was walked");
    // cfg_meta answers with the first row of a key: a second row would be dead, and Reset would visit the key twice
    for (int i = 0; cfg_meta_at(i); i++)
        for (int j = i + 1; cfg_meta_at(j); j++)
            if (!strcmp(cfg_meta_at(i)->key, cfg_meta_at(j)->key))
                fprintf(stderr, "FAIL cfg: %s has two rows in the defaults table\n", cfg_meta_at(i)->key), fails++;
    printf("probe-cfg: %s\n", fails ? "FAIL" : "ok");
    return fails ? 1 : 0;
}
