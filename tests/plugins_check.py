"""The plugin interface from a mod author's side (runtime/plugin_api.h, docs/plugins.md). No game files.

  - plugin_api.h compiles on its own as strict C99, C11 and C++17 with every warning on: it is the one file a mod
    copies, so it may lean on nothing else of the runtime and on no compiler extension;
  - the layout a plugin sees is the recorded one (sizes and the offset of every member a plugin of each version may
    read): a change here breaks plugins built before it, and belongs at the end of a structure with this record
    extended. What each version recorded is kept as it was, apart from the sizes of the structures a later one added to;
  - a plugin built against the version 1 header (kept below, cut to what it needs) still gets the table from a
    later game and passes it a register block of the old size, and one with the version 5 table finds its 26
    functions in a version 6 game;
  - version 6 (pictures and fonts for the overlay) is wired: the table's functions, the overlay's drawing calls,
    the freeing of pictures before the callbacks, the atlas made again for a font loaded later (the PNG reader, the
    handles and the limits are checked by the game's own `--probe-plugins`);
  - tests/plugins/hello_plugin.c builds into a library that exports nx2navi_plugin_init;
  - with a game executable given: it exports nx2navi_plugin_api (Windows), `--probe-plugins` passes, and the same
    probe started with NX2NAVI_MOD_DLLS naming that library and a missing one loads the first, calls its
    nx2navi_plugin_init and its frame callback, and names the second as not loaded.

Usage: plugins_check.py [PATH_TO_GAME_EXE]
"""
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MSYS_BIN = Path(r'C:\msys64\ucrt64\bin')

# sizeof and offsetof on x86-64, as each version fixed them (docs/plugins.md, "Versions": members are only added at the
# end). The first block is version 1's record, unchanged but for the three sizes that grew; the second is version 2's, and after
# it what versions 3 (the menu page, the picture's rectangle), 4 (the camera, the menu page's additions), 5 (the hook's
# frame pointer, the game's language and account, camera_reset, set_save_dir), 6 (pictures and fonts for the
# overlay) and 7 (Steam for a mod's own multiplayer) added.
LAYOUT = '''
#include <stdio.h>
#include "plugin_api.h"
#define S(t) printf(#t " %u\\n", (unsigned)sizeof(t))
#define O(t, m) printf(#t "." #m " %u\\n", (unsigned)offsetof(t, m))
int main(void) {
    S(Nx2GameInfo); O(Nx2GameInfo, runtime_version); O(Nx2GameInfo, build_id); O(Nx2GameInfo, main_size);
    S(Nx2Frame); O(Nx2Frame, index);
    S(Nx2GuestRegs); O(Nx2GuestRegs, x); O(Nx2GuestRegs, v);
    S(Nx2Draw); O(Nx2Draw, width); O(Nx2Draw, frame); O(Nx2Draw, text); O(Nx2Draw, line);
    S(Nx2KeyEvent); O(Nx2KeyEvent, key); O(Nx2KeyEvent, down); O(Nx2KeyEvent, repeat);
    S(Nx2PluginApi); O(Nx2PluginApi, game_info); O(Nx2PluginApi, read); O(Nx2PluginApi, on_frame);
    O(Nx2PluginApi, remove); O(Nx2PluginApi, guest_call); O(Nx2PluginApi, capture_input);
    printf("NX2_RGBA %08x\\n", NX2_RGBA(1, 2, 3, 4));
    O(Nx2GameInfo, profile_dir); O(Nx2GameInfo, save_dir);
    O(Nx2GuestRegs, stack); O(Nx2GuestRegs, nstack);
    S(Nx2HookCall); O(Nx2HookCall, skip); O(Nx2HookCall, addr); O(Nx2HookCall, x); O(Nx2HookCall, v);
    O(Nx2HookCall, sp); O(Nx2HookCall, lr);
    O(Nx2PluginApi, hook_function); O(Nx2PluginApi, call_original); O(Nx2PluginApi, map_range); O(Nx2PluginApi, on_exit);
    O(Nx2PluginApi, add_title_button); O(Nx2PluginApi, on_pad);
    S(Nx2TitleButton); O(Nx2TitleButton, order); O(Nx2TitleButton, label); O(Nx2TitleButton, description);
    O(Nx2TitleButton, on_select); O(Nx2TitleButton, user);
    S(Nx2PadEvent); O(Nx2PadEvent, button); O(Nx2PadEvent, down);
    printf("pad %d %d %d %d, title buttons %d\\n", NX2_PAD_A, NX2_PAD_PLUS, NX2_PAD_LEFT, NX2_PAD_DOWN, NX2_TITLE_BUTTONS_MAX);
    O(Nx2Draw, picture_x); O(Nx2Draw, picture_height);
    S(Nx2MenuRow); O(Nx2MenuRow, kind); O(Nx2MenuRow, label); O(Nx2MenuRow, description); O(Nx2MenuRow, value);
    O(Nx2MenuRow, choices); O(Nx2MenuRow, choice_count); O(Nx2MenuRow, current); O(Nx2MenuRow, disabled); O(Nx2MenuRow, text_max);
    S(Nx2MenuEvent); O(Nx2MenuEvent, kind); O(Nx2MenuEvent, row); O(Nx2MenuEvent, value); O(Nx2MenuEvent, text);
    S(Nx2Menu); O(Nx2Menu, row_count); O(Nx2Menu, title); O(Nx2Menu, rows); O(Nx2Menu, on_event); O(Nx2Menu, user);
    O(Nx2Menu, focus_row);
    O(Nx2PluginApi, open_menu); O(Nx2PluginApi, update_menu); O(Nx2PluginApi, close_menu);
    printf("rows %d %d %d %d %d, events %d %d %d %d %d\\n", NX2_ROW_ACTION, NX2_ROW_TOGGLE, NX2_ROW_SELECT, NX2_ROW_TEXT,
           NX2_ROW_SEPARATOR, NX2_MENU_ACTIVATE, NX2_MENU_CHANGE, NX2_MENU_FOCUS, NX2_MENU_CLOSED, NX2_MENU_TEXT);
    O(Nx2PadEvent, keyboard);
    S(Nx2Camera); O(Nx2Camera, position); O(Nx2Camera, target); O(Nx2Camera, up); O(Nx2Camera, near_plane);
    O(Nx2Camera, far_plane); O(Nx2Camera, fov_y); O(Nx2Camera, aspect); O(Nx2Camera, view); O(Nx2Camera, projection);
    O(Nx2PluginApi, camera); O(Nx2PluginApi, world_to_screen); O(Nx2PluginApi, restart);
    O(Nx2HookCall, fp); O(Nx2GameInfo, language); O(Nx2GameInfo, account);
    O(Nx2PluginApi, camera_reset); O(Nx2PluginApi, set_save_dir);
    O(Nx2Draw, image); O(Nx2Draw, text_font); O(Nx2Draw, text_font_size);
    O(Nx2PluginApi, texture_create); O(Nx2PluginApi, texture_load); O(Nx2PluginApi, texture_free);
    O(Nx2PluginApi, font_load); O(Nx2PluginApi, font_free);
    O(Nx2PluginApi, steam); S(Nx2Steam); O(Nx2Steam, start); O(Nx2Steam, send);
    S(Nx2SteamEvent); O(Nx2SteamEvent, data); S(Nx2SteamStatus); O(Nx2SteamStatus, says); O(Nx2SteamStatus, self);
    O(Nx2SteamStatus, room_code); O(Nx2SteamStatus, members); S(Nx2SteamMember); O(Nx2SteamMember, self);
    S(Nx2SteamFriend); O(Nx2SteamFriend, online);
    printf("steam %d %d %d %d, events %d %d, fails %d %d, caps %d %d %d %d\\n", NX2_STEAM_OFF, NX2_STEAM_STARTING, NX2_STEAM_READY,
           NX2_STEAM_UNAVAILABLE, NX2_STEAM_EV_STATE, NX2_STEAM_EV_LOST, NX2_STEAM_FAIL_CREATE, NX2_STEAM_FAIL_NOT_OURS,
           NX2_STEAM_NAME_CAP, NX2_STEAM_CODE_CAP, NX2_STEAM_MESSAGE_MAX, NX2_STEAM_ROOM_MAX);
    printf("flags %d %d, pictures %d of %d pixels a side, fonts %d\\n", NX2_TEXTURE_PIXELS, NX2_FONT_EXACT, NX2_TEXTURES_MAX,
           NX2_TEXTURE_SIDE_MAX, NX2_FONTS_MAX);
    printf("version %u, hooks %d, stack arguments %d\\n", NX2_PLUGIN_API_VERSION, NX2_HOOKS_MAX, NX2_STACK_ARGS_MAX);
    return 0;
}
'''
EXPECTED = '''Nx2GameInfo 88
Nx2GameInfo.runtime_version 8
Nx2GameInfo.build_id 32
Nx2GameInfo.main_size 48
Nx2Frame 16
Nx2Frame.index 8
Nx2GuestRegs 224
Nx2GuestRegs.x 8
Nx2GuestRegs.v 80
Nx2Draw 112
Nx2Draw.width 8
Nx2Draw.frame 24
Nx2Draw.text 32
Nx2Draw.line 64
Nx2KeyEvent 16
Nx2KeyEvent.key 4
Nx2KeyEvent.down 8
Nx2KeyEvent.repeat 9
Nx2PluginApi 264
Nx2PluginApi.game_info 8
Nx2PluginApi.read 24
Nx2PluginApi.on_frame 40
Nx2PluginApi.remove 72
Nx2PluginApi.guest_call 80
Nx2PluginApi.capture_input 96
NX2_RGBA 04030201
Nx2GameInfo.profile_dir 56
Nx2GameInfo.save_dir 64
Nx2GuestRegs.stack 208
Nx2GuestRegs.nstack 216
Nx2HookCall 240
Nx2HookCall.skip 4
Nx2HookCall.addr 8
Nx2HookCall.x 16
Nx2HookCall.v 88
Nx2HookCall.sp 216
Nx2HookCall.lr 224
Nx2PluginApi.hook_function 104
Nx2PluginApi.call_original 112
Nx2PluginApi.map_range 120
Nx2PluginApi.on_exit 128
Nx2PluginApi.add_title_button 136
Nx2PluginApi.on_pad 144
Nx2TitleButton 40
Nx2TitleButton.order 4
Nx2TitleButton.label 8
Nx2TitleButton.description 16
Nx2TitleButton.on_select 24
Nx2TitleButton.user 32
Nx2PadEvent 16
Nx2PadEvent.button 4
Nx2PadEvent.down 8
pad 0 10 12 15, title buttons 4
Nx2Draw.picture_x 72
Nx2Draw.picture_height 84
Nx2MenuRow 56
Nx2MenuRow.kind 4
Nx2MenuRow.label 8
Nx2MenuRow.description 16
Nx2MenuRow.value 24
Nx2MenuRow.choices 32
Nx2MenuRow.choice_count 40
Nx2MenuRow.current 44
Nx2MenuRow.disabled 48
Nx2MenuRow.text_max 52
Nx2MenuEvent 24
Nx2MenuEvent.kind 4
Nx2MenuEvent.row 8
Nx2MenuEvent.value 12
Nx2MenuEvent.text 16
Nx2Menu 48
Nx2Menu.row_count 4
Nx2Menu.title 8
Nx2Menu.rows 16
Nx2Menu.on_event 24
Nx2Menu.user 32
Nx2Menu.focus_row 40
Nx2PluginApi.open_menu 152
Nx2PluginApi.update_menu 160
Nx2PluginApi.close_menu 168
rows 0 1 2 3 4, events 0 1 2 3 4
Nx2PadEvent.keyboard 9
Nx2Camera 188
Nx2Camera.position 8
Nx2Camera.target 20
Nx2Camera.up 32
Nx2Camera.near_plane 44
Nx2Camera.far_plane 48
Nx2Camera.fov_y 52
Nx2Camera.aspect 56
Nx2Camera.view 60
Nx2Camera.projection 124
Nx2PluginApi.camera 176
Nx2PluginApi.world_to_screen 184
Nx2PluginApi.restart 192
Nx2HookCall.fp 232
Nx2GameInfo.language 72
Nx2GameInfo.account 80
Nx2PluginApi.camera_reset 200
Nx2PluginApi.set_save_dir 208
Nx2Draw.image 88
Nx2Draw.text_font 96
Nx2Draw.text_font_size 104
Nx2PluginApi.texture_create 216
Nx2PluginApi.texture_load 224
Nx2PluginApi.texture_free 232
Nx2PluginApi.font_load 240
Nx2PluginApi.font_free 248
Nx2PluginApi.steam 256
Nx2Steam 96
Nx2Steam.start 8
Nx2Steam.send 88
Nx2SteamEvent 40
Nx2SteamEvent.data 32
Nx2SteamStatus 328
Nx2SteamStatus.says 16
Nx2SteamStatus.self 216
Nx2SteamStatus.room_code 304
Nx2SteamStatus.members 324
Nx2SteamMember 80
Nx2SteamMember.self 72
Nx2SteamFriend 80
Nx2SteamFriend.online 72
steam 0 1 2 3, events 1 10, fails 1 6, caps 64 16 1200 8
flags 1 1, pictures 256 of 4096 pixels a side, fonts 16
version 7, hooks 64, stack arguments 64
'''

# A plugin as one built against the version 1 header: the structures as that header had them (56, 208 and 104 bytes),
# under names of their own. It asks for version 1, reads only what version 1 had, and hands guest_call_regs a register
# block of the old size, which a version 2 game must take as one without stack arguments (the call is still refused
# here, for the reason every guest call is outside a frame callback: not for the block's size).
V1_PLUGIN = '''
#include <stddef.h>
#include <stdint.h>
typedef struct { uint32_t size, game_version; const char *runtime_version, *game_dir, *mods_dir, *build_id;
                 uint64_t main_base, main_size; } Info1;
typedef struct { uint32_t size, reserved; uint64_t x[9]; uint64_t v[8][2]; } Regs1;
typedef struct { uint32_t size, reserved; uint64_t index; } Frame1;
typedef struct {
    uint32_t size, version;
    const Info1 *(*game_info)(void);
    void (*log)(const char *fmt, ...);
    int (*read)(uint64_t, void *, size_t);
    int (*write)(uint64_t, const void *, size_t);
    int (*on_frame)(void (*)(const Frame1 *, void *), void *);
    int (*on_overlay)(void *, void *);
    int (*on_key)(void *, void *);
    int (*on_text)(void *, void *);
    void (*remove)(int);
    uint64_t (*guest_call)(uint64_t, const uint64_t *, int);
    int (*guest_call_regs)(uint64_t, Regs1 *);
    void (*capture_input)(int);
} Api1;
_Static_assert(sizeof(Info1) == 56 && sizeof(Regs1) == 208 && sizeof(Api1) == 104, "the version 1 layout");
static const Api1 *api;
static void frame(const Frame1 *f, void *user) {
    (void)user;
    Regs1 r = {.size = sizeof r};
    const Info1 *g = api->game_info();
    api->log("old: frame %llu, game %u, main %#llx, regs call %d", (unsigned long long)f->index, g->game_version,
             (unsigned long long)g->main_base, api->guest_call_regs(g->main_base, &r));
}
__declspec(dllexport) void nx2navi_plugin_init(const void *a) {
    api = a;
    if (api->version < 1 || api->size < sizeof *api) return;
    api->log("old: a version 1 plugin got a table of version %u, %u bytes", api->version, api->size);
    api->on_frame(frame, 0);
}
'''

# A plugin as one built against the version 5 header: a table of 216 bytes (26 functions). It must get the same table
# from a version 6 game, with everything version 5 had still in its place.
V5_PLUGIN = '''
#include <stddef.h>
#include <stdint.h>
typedef struct { uint32_t size, version; void *fn[26]; } Api5;
_Static_assert(sizeof(Api5) == 216, "the version 5 layout");
__declspec(dllexport) void nx2navi_plugin_init(const Api5 *a) {
    void (*log)(const char *, ...) = (void (*)(const char *, ...))a->fn[1];
    int all = 1;
    for (int i = 0; i < 26; i++) all &= a->fn[i] != 0;
    if (a->version < 5 || a->size < sizeof *a || !all) return;
    log("five: a version 5 plugin got a table of version %u, %u bytes, its 26 functions there", a->version, a->size);
}
'''


def tool(name):
    env = os.environ.get('CC' if name == 'clang' else 'CXX')
    if env:
        return env
    exe = MSYS_BIN / (name + '.exe')
    return str(exe) if os.name == 'nt' and exe.exists() else shutil.which(name) or name


def exports(path):
    """The names a Windows executable or library exports, read from its export table."""
    import struct
    data = Path(path).read_bytes()
    pe = struct.unpack_from('<I', data, 0x3c)[0]
    nsec, opt_size = struct.unpack_from('<H', data, pe + 6)[0], struct.unpack_from('<H', data, pe + 20)[0]
    opt = pe + 24
    rva, _size = struct.unpack_from('<II', data, opt + 112)   # PE32+: the data directories start at 112
    secs = [struct.unpack_from('<8sIIII', data, opt + opt_size + 40 * i) for i in range(nsec)]

    def off(r):
        for _name, vsize, vaddr, rsize, raw in secs:
            if vaddr <= r < vaddr + max(vsize, rsize):
                return raw + r - vaddr
        raise ValueError('rva %#x is in no section' % r)

    if not rva:
        return []
    count, names = struct.unpack_from('<I', data, off(rva) + 24)[0], struct.unpack_from('<I', data, off(rva) + 32)[0]
    out = []
    for i in range(count):
        p = off(struct.unpack_from('<I', data, off(names) + 4 * i)[0])
        out.append(data[p:data.index(b'\0', p)].decode('ascii', 'replace'))
    return out


def main():
    if len(sys.argv) > 2:
        print(__doc__)
        return 2
    win = os.name == 'nt'
    work = ROOT / 'build' / 'test' / 'plugins'
    work.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ)
    if win and MSYS_BIN.exists():
        env['PATH'] = str(MSYS_BIN) + os.pathsep + env.get('PATH', '')
    rt = ROOT / 'runtime'
    fails = []
    warn = ['-Wall', '-Wextra', '-Wpedantic', '-Werror', '-fsyntax-only', '-I', str(rt)]
    for cc, std, lang in ((tool('clang'), '-std=c99', 'c'), (tool('clang'), '-std=c11', 'c'),
                          (tool('clang++'), '-std=c++17', 'c++')):
        p = subprocess.run([cc, std, '-x', lang, *warn, str(rt / 'plugin_api.h')], capture_output=True, text=True, env=env)
        if p.returncode:
            fails.append(f'plugin_api.h does not compile alone as {std}:\n{p.stderr}')

    src, exe = work / 'layout.c', work / ('layout.exe' if win else 'layout')
    src.write_text(LAYOUT)
    subprocess.check_call([tool('clang'), '-O1', '-I', str(rt), '-o', str(exe), str(src)], env=env)
    got = subprocess.run([str(exe)], capture_output=True, text=True, env=env).stdout.replace('\r\n', '\n')
    if got != EXPECTED:
        want = EXPECTED.splitlines()
        diff = [f'{g} (recorded: {w})' for g, w in zip(got.splitlines(), want) if g != w]
        fails.append('the layout a plugin sees changed: ' + '; '.join(diff or [got]))

    lib = work / ('hello_plugin.dll' if win else 'hello_plugin.so')
    subprocess.check_call([tool('clang'), '-O2', '-Wall', '-Wextra', '-Werror', '-shared', *([] if win else ['-fPIC']),
                           '-I', str(rt), '-o', str(lib), str(ROOT / 'tests' / 'plugins' / 'hello_plugin.c')], env=env)
    if win and 'nx2navi_plugin_init' not in exports(lib):
        fails.append('hello_plugin.dll does not export nx2navi_plugin_init')
    old_src, old = work / 'old_plugin.c', work / ('old_plugin.dll' if win else 'old_plugin.so')
    old_src.write_text(V1_PLUGIN if win else V1_PLUGIN.replace('__declspec(dllexport)', '__attribute__((visibility("default")))'))
    subprocess.check_call([tool('clang'), '-O2', '-Wall', '-Werror', '-shared', *([] if win else ['-fPIC']),
                           '-o', str(old), str(old_src)], env=env)

    five_src, five = work / 'five_plugin.c', work / ('five_plugin.dll' if win else 'five_plugin.so')
    five_src.write_text(V5_PLUGIN if win else V5_PLUGIN.replace('__declspec(dllexport)', '__attribute__((visibility("default")))'))
    subprocess.check_call([tool('clang'), '-O2', '-Wall', '-Werror', '-shared', *([] if win else ['-fPIC']),
                           '-o', str(five), str(five_src)], env=env)

    # Version 6 by the source: the table holds its five functions, every overlay gets the three drawing calls, the
    # pictures given up are freed before the callbacks, and a font loaded later makes the atlas again before a frame.
    plugins_c = (rt / 'plugins.c').read_text(encoding='utf-8')
    draw_cpp = (rt / 'ui' / 'plugin_draw.cpp').read_text(encoding='utf-8')
    menu_cpp = (rt / 'ui' / 'menu.cpp').read_text(encoding='utf-8')
    # Version 7 by the source: the table has steam, and the game's end leaves a plugin's room.
    if '.steam = plugin_steam,' not in plugins_c or 'plugin_steam_shutdown();' not in plugins_c:
        fails.append('plugins.c: the table has no steam, or the exit does not shut it down')
    for name in ('texture_create', 'texture_load', 'texture_free', 'font_load', 'font_free'):
        if f'.{name} = plugin_{name},' not in plugins_c:
            fails.append(f'plugins.c: the table has no {name}')
    if 'd.image = draw_image, d.text_font = draw_text_font, d.text_font_size = draw_text_font_size;' not in draw_cpp:
        fails.append("ui/plugin_draw.cpp: the overlay's structure lacks the version 6 drawing calls")
    overlay = draw_cpp.split('extern "C" void plugins_overlay_imgui(', 1)[-1]
    if not 0 < overlay.find('plugin_tex_sweep();') < overlay.find('plugins_overlay(&d);'):
        fails.append('ui/plugin_draw.cpp: pictures given up are not freed before the overlay callbacks')
    if 'g_pfont_dirty.load(std::memory_order_acquire) ? fonts_up(scale) : 1' not in menu_cpp:
        fails.append('ui/menu.cpp: a font a plugin loaded does not make the atlas again')

    # A menu page's game calls are made at the frame step, never where a plugin calls update_menu (a frame callback
    # runs while the game's layout animator may: docs/plugins.md "Threads"). Held here by the source: update_menu's
    # body calls no game function, and the frame step's hook does the work.
    menu_c = (rt / 'plugin_menu.c').read_text(encoding='utf-8')
    body = menu_c.split('int plugin_menu_update(const Nx2Menu *menu) {', 1)[-1].split(chr(10) + '}' + chr(10), 1)[0]
    if any(word in body for word in ('call(', 'apply_row(', 'apply_set(', 'edit_end(', 'focus_row(', 'guest_call')):
        fails.append('plugin_menu.c: update_menu makes a game call where the plugin called it')
    if 'static void h_step(' not in menu_c or 'G->frame_step, h_step' not in menu_c or 'pending_set = s;' not in body:
        fails.append("plugin_menu.c: the page's rows are not put on the screen by a hook of the frame step")

    if len(sys.argv) == 2:
        game = sys.argv[1]
        if win and 'nx2navi_plugin_api' not in exports(game):
            fails.append(f'{game} does not export nx2navi_plugin_api: plugins cannot find the interface')
        p = subprocess.run([game, '--probe-plugins'], capture_output=True, text=True, errors='replace')
        if p.returncode or 'probe-plugins: ok' not in p.stdout:
            fails.append(f'--probe-plugins: exit {p.returncode}\n{p.stdout[-2000:]}{p.stderr[-2000:]}')
        elif '[plugin ' not in p.stderr:
            fails.append("--probe-plugins: the plugin's log line is missing")
        missing = work / 'not_there.dll'
        p = subprocess.run([game, '--probe-plugins'], capture_output=True, text=True, errors='replace',
                           env=dict(os.environ, NX2NAVI_MOD_DLLS=f'{lib}|{missing}|{old}|{five}'))
        for want in ('probe-plugins: ok', 'plugins: loaded ', '[plugin hello_plugin] hello: interface version 7',
                     '[plugin hello_plugin] hello: a picture of 2x2 pixels, handle ',
                     '[plugin hello_plugin] hello: first frame callback', 'not_there.dll did not load',
                     '[plugin hello_plugin] hello: the game ends after 1 frames',
                     'hello_plugin added the title menu button "Hello Plugin"',
                     '[plugin old_plugin] old: a version 1 plugin got a table of version 7',
                     '[plugin five_plugin] five: a version 5 plugin got a table of version 7, 264 bytes, its 26 functions there',
                     '[plugin old_plugin] old: frame ', 'regs call 0'):
            if want not in p.stdout + p.stderr:
                fails.append(f'--probe-plugins with NX2NAVI_MOD_DLLS: no line "{want}"\n{p.stderr[-2000:]}')
    else:
        print('plugins_check: no game executable given: the header, its layout and the example plugin only')

    for f in fails:
        print('plugins_check: FAIL:', f)
    print('plugins_check:', 'FAILED' if fails else 'ok')
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
