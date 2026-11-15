"""Static checks of the upscalers' waits for the device (runtime/vk_dlaa.c "what the upscalers let go of while the GPU
may still use it", runtime/vk_ffx.cpp, runtime/vk_ffx4.cpp).

It fails when:
- vkDeviceWaitIdle is called in vk_dlaa.c outside vk_upscaler_device_idle, in vk_ffx.cpp outside vk_ffx_probe_cache
  (the --probe-ffx-cache path), or anywhere in vk_ffx4.cpp: each such wait stalls the replaying thread with the queue
  lock held;
- vk_upscaler_device_idle is called outside vk_upscaler_settle and vk_upscaler_gpu_wait (the rollback switch
  NX2NAVI_UPSCALER_IDLE_WAIT=1, a full list, a refused creation's failed wait);
- one of the places that let something go (vk_dlaa.c free_size, free_snap, stale_sizes, release_kept, hudless_from,
  pb_images, retire_idle and evaluate, vk_ffx.cpp and vk_ffx4.cpp context_for) does not call vk_upscaler_let_go, which
  frees what the GPU is past and which the rollback switch turns into a wait for the device;
- one of the places that create a DLSS feature or an FSR context (vk_dlaa.c evaluate, vk_ffx.cpp and vk_ffx4.cpp
  context_for) does not call vk_upscaler_gpu_wait: a creation refused while what was let go of is still in flight
  then fails for want of its video memory, where the wait for the device of 0.10.1 had freed it first;
- one of the places that let go of what the GPU may still use (those above, the helpers that hand it to the lists,
  vk_dlaa.c retire_size, retire_feat, retire_snap, retire_hl and retire_pb, vk_ffx.cpp and vk_ffx4.cpp retire_up, and
  the FSR files' trims and release_idle calls) frees something at once: a destroy that takes an idle device or a
  half-made object (vk_dlaa.c destroy_size, destroy_snap, hl_destroy, pb_destroy, feat_release, N.release,
  vkDestroyImageView, vmaDestroyImage; the FSR files' A.destroy), which on a GPU still running a frame is a use after
  free. vk_dlaa.c hudless_from, pb_images and snap_for may destroy only after their first image2d call, what of the new
  images was made and never used;
- a whole list is taken whatever its gates (upd_take_all) outside teardown (vk_dlaa.c vk_dlaa_release, vk_ffx.cpp
  vk_ffx_release, vk_ffx4.cpp vk_ffx4_release) and a full list's fallback (vk_dlaa.c dead_put, the FSR files'
  retire_up), or there before vk_upscaler_settle's wait for the device;
- NX2NAVI_UPSCALER_IDLE_WAIT has no sections[] entry in cfg.c or no line in nx2navi.ini.example (its known[] name and
  its metas[] row, and a // that would hide them, are tests/cfg_rows_check.py's);
- perf.c's mark_name has another count of names than host.h has PERF_M_* kinds: a kind without a name prints a null
  string on a slow frame's line;
- vk_dlaa.c pb_images does not note PB.used after the last call that clears PB: idle_release then took the images it
  had just made for 600 presents unused and freed them while the frame that used them could be in flight.

No GPU, no game files, any Python. Usage: upscaler_idle_check.py [repository root]
"""
import re
import sys
from pathlib import Path

WAIT_ALLOWED = {   # where vkDeviceWaitIdle may be called
    'vk_dlaa.c': {'vk_upscaler_device_idle'},
    'vk_ffx.cpp': {'vk_ffx_probe_cache'},
    'vk_ffx4.cpp': set(),
}
IDLE_ALLOWED = {'vk_upscaler_settle', 'vk_upscaler_gpu_wait'}   # vk_dlaa.c: who may call vk_upscaler_device_idle
LET_GO = {   # functions that let something go and must call vk_upscaler_let_go
    'vk_dlaa.c': ['free_size', 'free_snap', 'stale_sizes', 'release_kept', 'hudless_from', 'pb_images', 'retire_idle',
                  'evaluate'],
    'vk_ffx.cpp': ['context_for'],
    'vk_ffx4.cpp': ['context_for'],
}
REFUSED = {   # functions that create a feature or context and must retry a refusal after vk_upscaler_gpu_wait
    'vk_dlaa.c': ['evaluate'],
    'vk_ffx.cpp': ['context_for'],
    'vk_ffx4.cpp': ['context_for'],
}
# Callees that free at once, without a wait: each is for an idle device (teardown) or for what no submission named yet
# (a half-made object).
FREE_NOW = {
    'vk_dlaa.c': ['destroy_size', 'destroy_snap', 'hl_destroy', 'pb_destroy', 'feat_release', 'N.release',
                  'vkDestroyImageView', 'vmaDestroyImage'],
    'vk_ffx.cpp': ['A.destroy'],
    'vk_ffx4.cpp': ['A.destroy'],
}
NO_FREE_NOW = {   # functions that let go of what the GPU may still use: no FREE_NOW callee anywhere in them
    'vk_dlaa.c': ['free_size', 'free_snap', 'stale_sizes', 'release_kept', 'retire_idle', 'evaluate', 'retire_size',
                  'retire_feat', 'retire_snap', 'retire_hl', 'retire_pb'],
    'vk_ffx.cpp': ['context_for', 'trim', 'retire_up', 'vk_ffx_trim_idle', 'vk_ffx_release_idle'],
    'vk_ffx4.cpp': ['context_for', 'vk_ffx4_trim', 'retire_up', 'vk_ffx4_release_idle'],
}
# Functions that let go of the old images and then make new ones: no FREE_NOW callee before their first image2d call;
# after it, the failure path destroys what of the new images was made, which no submission named.
NO_FREE_BEFORE_MAKE = {'vk_dlaa.c': ['hudless_from', 'pb_images', 'snap_for']}
# Where upd_take_all may be called, and whether vk_upscaler_settle's wait for the device must come before it there.
TAKE_ALL = {
    'vk_dlaa.c': {'vk_dlaa_release': False, 'dead_put': True},
    'vk_ffx.cpp': {'vk_ffx_release': False, 'retire_up': True},
    'vk_ffx4.cpp': {'vk_ffx4_release': False, 'retire_up': True},
}
# A function definition: at column 0, a return type, the name, the parameters and the body's brace (the runtime's
# style); declarations end with ';' before any '{'. Read in code whose string literals are blanked: extern "C" is
# extern " " there.
FUNC = re.compile(r'^(?:extern "[^"\n]*" )?(?:static )?(?:inline )?[A-Za-z_][\w \*]*?\b([A-Za-z_]\w*)\s*\([^;{}]*\)\s*\{',
                  re.M)


def blank_comments(src):
    """src with comments and string literals replaced by spaces (newlines kept), so offsets stay and only code is read."""
    out, i, n = list(src), 0, len(src)
    while i < n:
        if src.startswith('//', i):
            j = src.find('\n', i)
            j = n if j < 0 else j
            for k in range(i, j):
                out[k] = ' '
            i = j
        elif src.startswith('/*', i):
            j = src.find('*/', i + 2)
            j = n if j < 0 else j + 2
            for k in range(i, j):
                if out[k] != '\n':
                    out[k] = ' '
            i = j
        elif src[i] in '"\'':
            q, j = src[i], i + 1
            while j < n and src[j] != q and src[j] != '\n':
                j += 2 if src[j] == '\\' else 1
            for k in range(i + 1, min(j, n)):
                out[k] = ' '
            i = j + 1
        else:
            i += 1
    return ''.join(out)


def functions(code):
    """[(name, body start, body end)] of each function defined in code (comments blanked)."""
    found = []
    for m in FUNC.finditer(code):
        depth, k = 0, m.end() - 1
        while k < len(code):
            if code[k] == '{':
                depth += 1
            elif code[k] == '}':
                depth -= 1
                if depth == 0:
                    break
            k += 1
        found.append((m.group(1), m.end() - 1, k))
    return found


def enclosing(funcs, pos):
    for name, a, b in funcs:
        if a <= pos <= b:
            return name
    return None


def calls(code, funcs, callee):
    """[(line, enclosing function)] of each call of callee (its definition left out)."""
    out = []
    for m in re.finditer(r'\b%s\s*\(' % re.escape(callee), code):
        fn = enclosing(funcs, m.start())
        if fn is None:   # the definition's own head, or a declaration
            continue
        out.append((code.count('\n', 0, m.start()) + 1, fn))
    return out


def call_offsets(code, callee, a, b):
    """The offsets of each call of callee between a and b."""
    return [m.start() for m in re.compile(r'\b%s\s*\(' % re.escape(callee)).finditer(code, a, b)]


def body(code, funcs, name):
    for n, a, b in funcs:
        if n == name:
            return code[a:b + 1]
    return None


def main():
    root = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).resolve().parents[1]
    rt = root / 'runtime'
    fails = []
    sources = {}
    for f in WAIT_ALLOWED:
        code = blank_comments((rt / f).read_text(encoding='utf-8'))
        sources[f] = (code, functions(code))
    for f, allowed in WAIT_ALLOWED.items():
        code, funcs = sources[f]
        for line, fn in calls(code, funcs, 'vkDeviceWaitIdle'):
            if fn not in allowed:
                fails.append(f'{f}:{line}: vkDeviceWaitIdle in {fn}, allowed only in {sorted(allowed) or "none"}')
        for line, fn in calls(code, funcs, 'vk_upscaler_device_idle'):
            if f != 'vk_dlaa.c' or fn not in IDLE_ALLOWED:
                fails.append(f'{f}:{line}: vk_upscaler_device_idle in {fn}, allowed only in vk_dlaa.c '
                             f'{sorted(IDLE_ALLOWED)}')
    nwaits = len(calls(*sources['vk_dlaa.c'], 'vkDeviceWaitIdle'))
    if nwaits != 1:
        fails.append(f'vk_dlaa.c: {nwaits} calls of vkDeviceWaitIdle, expected the one in vk_upscaler_device_idle')
    for f, names in LET_GO.items():
        code, funcs = sources[f]
        for name in names:
            b = body(code, funcs, name)
            if b is None:
                fails.append(f'{f}: no function {name}')
            elif not re.search(r'\bvk_upscaler_let_go\s*\(', b):
                fails.append(f'{f} {name}: lets something go without vk_upscaler_let_go')
    for f, names in REFUSED.items():
        code, funcs = sources[f]
        for name in names:
            b = body(code, funcs, name)
            if b is None:
                fails.append(f'{f}: no function {name}')
            elif not re.search(r'\bvk_upscaler_gpu_wait\s*\(', b):
                fails.append(f'{f} {name}: no retry of a refused creation after vk_upscaler_gpu_wait')
    for f, names in NO_FREE_NOW.items():
        code, funcs = sources[f]
        defined = {n for n, _, _ in funcs}
        fails += [f'{f}: no function {name}' for name in names if name not in defined]
        for callee in FREE_NOW[f]:
            for line, fn in calls(code, funcs, callee):
                if fn in names:
                    fails.append(f'{f}:{line}: {fn} calls {callee}, which frees at once what the GPU may still use')
    for f, names in NO_FREE_BEFORE_MAKE.items():
        code, funcs = sources[f]
        for name in names:
            spans = [(a, b) for n, a, b in funcs if n == name]
            if not spans:
                fails.append(f'{f}: no function {name}')
            for a, b in spans:
                made = call_offsets(code, 'image2d', a, b)
                if not made:
                    fails.append(f'{f} {name}: no image2d call')
                    continue
                for callee in FREE_NOW[f]:
                    for at in call_offsets(code, callee, a, made[0]):
                        fails.append(f'{f}:{code.count(chr(10), 0, at) + 1}: {name} calls {callee} before it makes '
                                     f'its images, on what the GPU may still use')
    for f, allowed in TAKE_ALL.items():
        code, funcs = sources[f]
        for name, a, b in funcs:
            for at in call_offsets(code, 'upd_take_all', a, b):
                line = code.count('\n', 0, at) + 1
                if name not in allowed:
                    fails.append(f'{f}:{line}: upd_take_all in {name}, allowed only in {sorted(allowed)}')
                elif allowed[name] and not call_offsets(code, 'vk_upscaler_settle', a, at):
                    fails.append(f'{f}:{line}: upd_take_all in {name} before vk_upscaler_settle')

    key = 'UPSCALER_IDLE_WAIT'
    cfg = (rt / 'cfg.c').read_text(encoding='utf-8-sig')
    if not re.search(r'\{\s*"%s"\s*,\s*"display"\s*\}' % key, cfg):
        fails.append(f'cfg.c: {key} has no sections[] entry in "display"')
    ini = (root / 'nx2navi.ini.example').read_text(encoding='utf-8-sig')
    if not re.search(r'^#\s*%s\s*=' % key.lower(), ini, re.M):
        fails.append(f'nx2navi.ini.example: no {key.lower()} line')

    host = (rt / 'host.h').read_text(encoding='utf-8')
    m = re.search(r'enum\s*\{\s*(PERF_M_FILE[^}]*)\}', host)
    kinds = [k.strip() for k in m.group(1).split(',') if k.strip() and k.strip() != 'PERF_M_N'] if m else []
    perf = (rt / 'perf.c').read_text(encoding='utf-8')
    m = re.search(r'mark_name\[PERF_M_N\]\s*=\s*\{(.*?)\};', perf, re.S)
    names = re.findall(r'"[^"]*"', m.group(1)) if m else []
    if not kinds or len(names) != len(kinds):
        fails.append(f'perf.c: mark_name has {len(names)} names for {len(kinds)} PERF_M kinds in host.h')

    code, funcs = sources['vk_dlaa.c']
    b = body(code, funcs, 'pb_images')
    if b is None:
        fails.append('vk_dlaa.c: no function pb_images')
    else:
        clears = [mm.end() for mm in re.finditer(r'\b(?:pb_destroy|retire_pb)\s*\(\s*\)|memset\s*\(\s*&PB\b', b)]
        sets = [mm.start() for mm in re.finditer(r'\bPB\.used\s*=', b)]
        last_clear = max(clears) if clears else -1
        if not sets or max(sets) < last_clear:
            fails.append('vk_dlaa.c pb_images: PB.used is not noted after the last call that clears PB')

    for f in fails:
        print('FAIL', f)
    print('upscaler_idle_check:', 'FAILED' if fails else
          f'passed (vkDeviceWaitIdle once in vk_dlaa.c, {sum(len(v) for v in LET_GO.values())} places let go, '
          f'{len(kinds)} perf mark kinds named)')
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
